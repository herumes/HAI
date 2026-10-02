#include "pch.h"
#include "buildsession.hpp"
#include "prefab.hpp"
#include "common.hpp"
#include "gamesdk.hpp"     // where the player is, for "nearby"
#include "position.hpp"
#include "placewatch.hpp"  // arming follow-along
#include "packets.hpp"     // whether the outbound hook is in

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>

std::atomic<bool> g_sessionBusy{ false };
std::atomic<bool> g_sessionAutoResume{ true };

namespace {

// =============================================================================
// State
// =============================================================================
struct SessionMeta {
    std::string id, name, note, prefabName, prefabSource;
    int64_t createdUnix = 0;
    uint64_t fingerprint = 0;
    size_t blocks = 0;
};

std::mutex g_sessMtx;
SessionMeta g_active;                      // guarded by g_sessMtx
bool g_haveActive = false;                 // guarded by g_sessMtx
std::string g_statusText;                  // guarded by g_sessMtx
std::string g_saveError;                   // guarded by g_sessMtx
std::vector<BuildSessionInfo> g_list;      // guarded by g_sessMtx

std::atomic<uint64_t> g_metaRev{ 1 };      // name and note changes
std::atomic<uint64_t> g_savedKey{ 0 };     // CurrentKey() as of the last good save
std::atomic<long long> g_lastSavedMs{ 0 };
std::atomic<long long> g_saveRetryAtMs{ 0 };
std::atomic<bool> g_saveInFlight{ false };
std::atomic<bool> g_listStale{ true };

// One writer at a time. The autosave writes on a worker, and opening or
// closing a build writes from another; two of them racing on one .tmp file
// would be exactly the half-written save this is all meant to prevent.
std::mutex g_writeMtx;

// How much history goes to disk. The rest stays in memory for this session.
// Enough to rewind a rollback or undo an evening's mistakes after a restart,
// without rewriting megabytes every time a block goes down.
constexpr size_t kPersistHistory = 5000;

void SetStatus(const std::string& s) {
    std::lock_guard lock(g_sessMtx);
    g_statusText = s;
}

// =============================================================================
// Where things live
// =============================================================================
std::wstring RootDir()                          { return GetThisModuleDirectory() + L"build_sessions"; }
std::wstring DirOf(const std::string& id)       { return RootDir() + L"\\" + Utf8ToWide(id); }
std::wstring StateFileOf(const std::string& id) { return DirOf(id) + L"\\session.txt"; }
std::wstring CopyFileOf(const std::string& id)  { return DirOf(id) + L"\\prefab.json"; }
std::wstring ActiveFile()                       { return RootDir() + L"\\active.txt"; }

// An id is a folder name we made. Anything else - a path separator, a dot -
// is refused outright, because ids are what Delete removes.
bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 96) return false;
    for (char ch : id)
        if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '_')) return false;
    return true;
}

std::string MakeId(const std::string& stem) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[40];
    snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d-",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::string safe;
    for (char ch : stem) {
        if (safe.size() >= 40) break;
        if (isalnum((unsigned char)ch) || ch == '-' || ch == '_') safe += ch;
        else if (ch == ' ') safe += '_';
    }
    if (safe.empty()) safe = "build";

    const std::string base = stamp + safe;
    std::string id = base;
    for (int n = 2; GetFileAttributesW(DirOf(id).c_str()) != INVALID_FILE_ATTRIBUTES && n < 1000; ++n)
        id = base + "-" + std::to_string(n);
    return id;
}

bool ReadText(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
        size.QuadPart > 256LL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)size.QuadPart);
    size_t got = 0;
    while (got < out.size()) {
        DWORD n = 0;
        const DWORD want = (DWORD)(std::min)(out.size() - got, (size_t)(1u << 24));
        if (!::ReadFile(h, &out[got], want, &n, nullptr) || n == 0) break;
        got += n;
    }
    CloseHandle(h);
    out.resize(got);
    return true;
}

// Names and notes go on one line of a text file, so they are made to fit one.
std::string OneLine(const std::string& s, size_t maxLen = 120) {
    std::string out;
    for (char ch : s) {
        if (out.size() >= maxLen) break;
        out += (ch == '\r' || ch == '\n' || ch == '\t') ? ' ' : ch;
    }
    return TrimCopy(out);
}

void WriteActivePointer(const std::string& id) {
    std::string err;
    WriteFileAtomic(ActiveFile(), "# the build that was open last - reopened on the next launch\nid=" +
        id + "\n", err, false);
}

void ClearActivePointer() { DeleteFileW(ActiveFile().c_str()); }

std::string ReadActivePointer() {
    std::string text;
    if (!ReadText(ActiveFile(), text)) return std::string();
    const size_t at = text.find("id=");
    if (at == std::string::npos) return std::string();
    size_t end = text.find_first_of("\r\n", at);
    if (end == std::string::npos) end = text.size();
    return TrimCopy(text.substr(at + 3, end - at - 3));
}

bool PlayerPosition(double& x, double& y, double& z) {
    const GsdkPlayerInfo me = GsdkGetPlayerInfo();
    if (me.valid) { x = me.x; y = me.y; z = me.z; return true; }
    if (NowSteadyMs() - g_posLastUpdateMs.load() < 3000) {
        x = g_posX.load(); y = g_posY.load(); z = g_posZ.load();
        return true;
    }
    return false;
}

// =============================================================================
// The file
// =============================================================================
// Plain key=value lines, so it can be read and fixed by hand. The block states
// are run-length encoded - "120D4T1S..." - because a build is long runs of the
// same thing, and a 50,000-block build then saves as a few hundred bytes of
// progress rather than 50,000 characters of it.
//
// The last line is always "end=ok". A file without it was cut short, and the
// loader falls back to the backup rather than trusting half a save.
std::string EncodeStates(const std::vector<uint8_t>& st) {
    static const char kCh[3] = { 'T', 'D', 'S' };
    std::string out;
    size_t i = 0;
    while (i < st.size()) {
        const uint8_t v = st[i] > 2 ? 0 : st[i];
        size_t j = i + 1;
        while (j < st.size() && (st[j] > 2 ? 0 : st[j]) == v) ++j;
        out += std::to_string(j - i);
        out += kCh[v];
        i = j;
    }
    return out;
}

bool DecodeStates(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    uint64_t n = 0;
    bool haveDigit = false;
    for (char ch : s) {
        if (ch >= '0' && ch <= '9') {
            n = n * 10 + (uint64_t)(ch - '0');
            if (n > 50000000ull) return false;
            haveDigit = true;
            continue;
        }
        uint8_t v = 0;
        if (ch == 'T') v = 0;
        else if (ch == 'D') v = 1;
        else if (ch == 'S') v = 2;
        else if (ch == ' ' || ch == '\r') continue;
        else return false;
        if (!haveDigit || out.size() + n > 50000000ull) return false;
        out.insert(out.end(), (size_t)n, v);
        n = 0;
        haveDigit = false;
    }
    return !haveDigit;
}

// What went into each block, as runs: "120:-;3:0;7:2;" - count, then an index
// into the placed-name table or '-' for unknown. Most of a build is one or two
// materials laid in long runs, so this stays small.
std::string EncodePlacedAs(const std::vector<int32_t>& v) {
    std::string out;
    size_t i = 0;
    while (i < v.size()) {
        const int32_t x = v[i] < 0 ? -1 : v[i];
        size_t j = i + 1;
        while (j < v.size() && (v[j] < 0 ? -1 : v[j]) == x) ++j;
        out += std::to_string(j - i);
        out += ':';
        out += x < 0 ? std::string("-") : std::to_string(x);
        out += ';';
        i = j;
    }
    return out;
}

bool DecodePlacedAs(const std::string& s, std::vector<int32_t>& out) {
    out.clear();
    size_t i = 0;
    while (i < s.size()) {
        const size_t colon = s.find(':', i);
        const size_t semi = s.find(';', i);
        if (colon == std::string::npos || semi == std::string::npos || colon > semi) return false;
        const unsigned long long n = strtoull(s.substr(i, colon - i).c_str(), nullptr, 10);
        const std::string v = s.substr(colon + 1, semi - colon - 1);
        if (n == 0 || out.size() + n > 50000000ull) return false;
        const int32_t x = (v == "-") ? -1 : (int32_t)strtol(v.c_str(), nullptr, 10);
        out.insert(out.end(), (size_t)n, x);
        i = semi + 1;
    }
    return true;
}

struct SessionFile {
    SessionMeta meta;
    BuildSnapshot snap;
    size_t done = 0, skipped = 0;
    int64_t updatedUnix = 0;
    bool complete = false;
};

// The tail of a history, without cutting a group in half: a bulk mark that is
// only half undoable is worse than one that is not undoable at all.
size_t PersistFrom(const std::vector<BuildHistoryEntry>& h) {
    if (h.size() <= kPersistHistory) return 0;
    size_t from = h.size() - kPersistHistory;
    while (from < h.size() && h[from].group != 0 && h[from].group == h[from - 1].group) ++from;
    return from;
}

std::string Serialize(const SessionMeta& m, const BuildSnapshot& s) {
    size_t done = 0, skipped = 0;
    for (uint8_t v : s.state) {
        if (v == (uint8_t)BuildBlockState::Done) ++done;
        else if (v == (uint8_t)BuildBlockState::Skipped) ++skipped;
    }

    std::string o;
    o.reserve(2048 + s.history.size() * 40);
    o += "# HyAI build session - saved automatically while you build.\n"
         "# Delete this folder to forget the build. Hand edits are fine as long\n"
         "# as the last line stays \"end=ok\".\n";
    o += "version=1\n";
    o += "name=" + OneLine(m.name) + "\n";
    o += "note=" + OneLine(m.note) + "\n";
    o += "prefabName=" + OneLine(m.prefabName) + "\n";
    o += "prefabSource=" + OneLine(m.prefabSource, 1024) + "\n";

    char buf[512];
    snprintf(buf, sizeof(buf),
        "blocks=%zu\ndone=%zu\nskipped=%zu\nfingerprint=%016llx\ncreated=%lld\nupdated=%lld\n",
        m.blocks, done, skipped, (unsigned long long)m.fingerprint,
        (long long)m.createdUnix, (long long)(BuildUnixMs() / 1000));
    o += buf;
    snprintf(buf, sizeof(buf),
        "originX=%d\noriginY=%d\noriginZ=%d\nrotation=%d\nlocked=%d\nshown=%d\n",
        s.originX, s.originY, s.originZ, s.rotation, s.locked ? 1 : 0, s.shown ? 1 : 0);
    o += buf;
    snprintf(buf, sizeof(buf),
        "orderMode=%d\nlayer=%d\nlayerValid=%d\nlayerOnly=%d\nshowMode=%d\nghostCount=%d\n",
        s.orderMode, s.layer, s.layerValid ? 1 : 0, s.layerOnly ? 1 : 0, s.showMode,
        s.ghostCount);
    o += buf;
    snprintf(buf, sizeof(buf),
        "scale=%.4f\nnudgeX=%.4f\nnudgeY=%.4f\nnudgeZ=%.4f\nopacityNear=%.4f\nopacityFar=%.4f\n",
        s.scale, s.nudgeX, s.nudgeY, s.nudgeZ, s.opNear, s.opFar);
    o += buf;
    snprintf(buf, sizeof(buf),
        "fade=%d\ncolourByType=%d\nshowDone=%d\ncheckMaterial=%d\nactiveMs=%lld\nplaced=%llu\n",
        s.fade ? 1 : 0, s.byType ? 1 : 0, s.showDone ? 1 : 0, s.checkMaterial ? 1 : 0,
        (long long)s.activeMs, (unsigned long long)s.placed);
    o += buf;

    o += "state=" + EncodeStates(s.state) + "\n";

    auto writeHist = [&](const char* key, const std::vector<BuildHistoryEntry>& h, size_t from) {
        for (size_t i = from; i < h.size(); ++i) {
            const BuildHistoryEntry& e = h[i];
            snprintf(buf, sizeof(buf), "%s=%u,%u,%u,%u,%u,%lld\n", key, e.index,
                (unsigned)e.from, (unsigned)e.to, (unsigned)e.source, e.group,
                (long long)e.unixMs);
            o += buf;
        }
    };
    writeHist("h", s.history, PersistFrom(s.history));
    // The redo list's END is the next redo, so it is the end that is kept.
    writeHist("r", s.redo, PersistFrom(s.redo));

    for (const BuildMistake& mk : s.mistakes) {
        snprintf(buf, sizeof(buf), "m=%u\t%lld\t", mk.index, (long long)mk.unixMs);
        o += buf;
        o += OneLine(mk.placedName, 200) + "\n";
    }
    for (const auto& sw : s.swaps)
        o += "x=" + OneLine(sw.first, 200) + "\t" + OneLine(sw.second, 200) + "\n";
    for (const std::string& n : s.placedNames) o += "pn=" + OneLine(n, 200) + "\n";
    if (!s.placedNames.empty()) o += "pa=" + EncodePlacedAs(s.placedAs) + "\n";

    o += "end=ok\n";
    return o;
}

bool ParseHist(const std::string& v, BuildHistoryEntry& e) {
    unsigned idx = 0, from = 0, to = 0, src = 0, group = 0;
    long long ms = 0;
    if (sscanf_s(v.c_str(), "%u,%u,%u,%u,%u,%lld", &idx, &from, &to, &src, &group, &ms) != 6)
        return false;
    e.index = idx;
    e.from = (uint8_t)from;
    e.to = (uint8_t)to;
    e.source = (uint8_t)src;
    e.group = group;
    e.unixMs = ms;
    return from <= 2 && to <= 2;
}

// `full` false skips the heavy parts - states, history - for listing.
bool ParseSession(const std::string& text, SessionFile& f, bool full) {
    f = SessionFile{};
    BuildSnapshot& s = f.snap;
    size_t i = 0;
    while (i < text.size()) {
        size_t eol = text.find('\n', i);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(i, eol - i);
        i = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        const long long n = atoll(v.c_str());
        const bool b = v == "1";

        if (k == "end")               f.complete = (v == "ok");
        else if (k == "name")         f.meta.name = v;
        else if (k == "note")         f.meta.note = v;
        else if (k == "prefabName")   f.meta.prefabName = v;
        else if (k == "prefabSource") f.meta.prefabSource = v;
        else if (k == "blocks")       f.meta.blocks = (size_t)(n < 0 ? 0 : n);
        else if (k == "done")         f.done = (size_t)(n < 0 ? 0 : n);
        else if (k == "skipped")      f.skipped = (size_t)(n < 0 ? 0 : n);
        else if (k == "fingerprint")  f.meta.fingerprint = strtoull(v.c_str(), nullptr, 16);
        else if (k == "created")      f.meta.createdUnix = n;
        else if (k == "updated")      f.updatedUnix = n;
        else if (k == "originX")      s.originX = (int)n;
        else if (k == "originY")      s.originY = (int)n;
        else if (k == "originZ")      s.originZ = (int)n;
        else if (k == "rotation")     s.rotation = (int)n & 3;
        else if (k == "locked")       s.locked = b;
        else if (k == "shown")        s.shown = b;
        else if (k == "orderMode")    s.orderMode = (int)n;
        else if (k == "layer")        s.layer = (int)n;
        else if (k == "layerValid")   s.layerValid = b;
        else if (k == "layerOnly")    s.layerOnly = b;
        else if (k == "showMode")     s.showMode = (int)n;
        else if (k == "ghostCount")   s.ghostCount = (int)n;
        else if (k == "scale")        s.scale = (float)atof(v.c_str());
        else if (k == "nudgeX")       s.nudgeX = (float)atof(v.c_str());
        else if (k == "nudgeY")       s.nudgeY = (float)atof(v.c_str());
        else if (k == "nudgeZ")       s.nudgeZ = (float)atof(v.c_str());
        else if (k == "opacityNear")  s.opNear = (float)atof(v.c_str());
        else if (k == "opacityFar")   s.opFar = (float)atof(v.c_str());
        else if (k == "fade")         s.fade = b;
        else if (k == "colourByType") s.byType = b;
        else if (k == "showDone")     s.showDone = b;
        else if (k == "checkMaterial") s.checkMaterial = b;
        else if (k == "activeMs")     s.activeMs = n;
        else if (k == "placed")       s.placed = (uint64_t)(n < 0 ? 0 : n);
        else if (!full) continue;
        else if (k == "state") {
            if (!DecodeStates(v, s.state)) return false;
        }
        else if (k == "h" || k == "r") {
            BuildHistoryEntry e;
            if (ParseHist(v, e)) (k == "h" ? s.history : s.redo).push_back(e);
        }
        else if (k == "m") {
            const size_t t1 = v.find('\t');
            const size_t t2 = (t1 == std::string::npos) ? t1 : v.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            BuildMistake mk;
            mk.index = (uint32_t)strtoul(v.substr(0, t1).c_str(), nullptr, 10);
            mk.unixMs = atoll(v.substr(t1 + 1, t2 - t1 - 1).c_str());
            mk.placedName = v.substr(t2 + 1);
            s.mistakes.push_back(mk);
        }
        else if (k == "x" || k == "s") {        // "s=" is the older name for the same thing
            const size_t t = v.find('\t');
            if (t != std::string::npos) s.swaps.emplace_back(v.substr(0, t), v.substr(t + 1));
        }
        else if (k == "pn") s.placedNames.push_back(v);
        else if (k == "pa") {
            // Only a nicety - a bad line loses "what did I really place", never
            // the build itself.
            if (!DecodePlacedAs(v, s.placedAs)) s.placedAs.clear();
        }
    }
    // A state that does not cover the build it claims is not a state.
    if (full && !s.state.empty() && s.state.size() != f.meta.blocks) return false;
    return true;
}

// The newest save, or the one before it if the newest is damaged.
bool LoadSessionFile(const std::string& id, SessionFile& f, bool full, bool& fromBackup,
    std::string& err) {
    fromBackup = false;
    std::string text;
    if (ReadText(StateFileOf(id), text) && ParseSession(text, f, full) && f.complete) {
        f.meta.id = id;
        return true;
    }
    if (ReadText(StateFileOf(id) + L".bak", text) && ParseSession(text, f, full) && f.complete) {
        f.meta.id = id;
        fromBackup = true;
        return true;
    }
    f = SessionFile{};
    err = "its save file is missing or damaged, and so is the backup";
    return false;
}

// Everything that would make a save different. Compared against the key of the
// last save, so a session that is only being looked at is never rewritten.
uint64_t CurrentKey() {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    auto mixf = [&mix](float f) { mix((uint64_t)(int64_t)(f * 10000.0f)); };
    mix(BuildRevision());
    mix(g_metaRev.load());
    mix((uint32_t)g_buildOriginX.load());
    mix((uint32_t)g_buildOriginY.load());
    mix((uint32_t)g_buildOriginZ.load());
    mix((uint32_t)g_buildRotation.load());
    mix(g_buildAnchorLocked.load() ? 3 : 5);
    mix(g_buildActive.load() ? 7 : 11);
    mix((uint32_t)g_buildOrderMode.load());
    mix(g_buildLayerOnly.load() ? 13 : 17);
    mix((uint32_t)g_buildShowMode.load());
    mix((uint32_t)g_buildGhostCount.load());
    mixf(g_buildGhostScale.load());
    mixf(g_buildNudgeX.load());
    mixf(g_buildNudgeY.load());
    mixf(g_buildNudgeZ.load());
    mixf(g_buildOpacityNear.load());
    mixf(g_buildOpacityFar.load());
    mix(g_buildFadeOtherLayers.load() ? 19 : 23);
    mix(g_buildColourByType.load() ? 29 : 31);
    mix(g_buildShowDone.load() ? 37 : 41);
    mix(g_buildCheckMaterial.load() ? 43 : 47);
    return h;
}

void RecordSave(const std::string& id, bool ok, uint64_t key, const std::string& err) {
    if (ok) {
        g_savedKey.store(key);
        g_lastSavedMs.store(NowSteadyMs());
        g_saveRetryAtMs.store(0);
        g_listStale.store(true);
        std::lock_guard lock(g_sessMtx);
        g_saveError.clear();
        return;
    }
    // Retry in a while rather than every frame: a full disk does not empty
    // itself because it was asked twice a second.
    g_saveRetryAtMs.store(NowSteadyMs() + 5000);
    LogToFile("session: save of " + id + " failed: " + err);
    std::lock_guard lock(g_sessMtx);
    if (g_haveActive && g_active.id == id) g_saveError = err;
}

// Waits for an autosave that is already writing, so a later save can never be
// overwritten by an earlier one finishing after it.
void WaitForInFlightSave() {
    for (int i = 0; i < 300 && g_saveInFlight.load(); ++i) Sleep(10);
}

bool SaveActiveSync() {
    SessionMeta m;
    {
        std::lock_guard lock(g_sessMtx);
        if (!g_haveActive) return false;
        m = g_active;
    }
    WaitForInFlightSave();
    const uint64_t key = CurrentKey();
    BuildSnapshot s;
    BuildExportState(s);
    const std::string text = Serialize(m, s);

    std::string err;
    bool ok = false;
    {
        std::lock_guard w(g_writeMtx);
        ok = WriteFileAtomic(StateFileOf(m.id), text, err, true);
    }
    RecordSave(m.id, ok, key, err);
    return ok;
}

// The autosave. Serialised here, written on a worker - the flush to disk is
// the slow part, and the overlay should not hitch for it.
void SaveActiveAsync() {
    if (g_saveInFlight.exchange(true)) return;
    SessionMeta m;
    {
        std::lock_guard lock(g_sessMtx);
        if (!g_haveActive) { g_saveInFlight.store(false); return; }
        m = g_active;
    }
    const uint64_t key = CurrentKey();
    BuildSnapshot s;
    BuildExportState(s);
    std::string text = Serialize(m, s);

    StartWorker([id = m.id, key, text = std::move(text)]() {
        WorkerGuard guard;
        std::string err;
        bool ok = false;
        {
            std::lock_guard w(g_writeMtx);
            ok = WriteFileAtomic(StateFileOf(id), text, err, true);
        }
        RecordSave(id, ok, key, err);
        g_saveInFlight.store(false);
    });
}

// =============================================================================
// Opening, starting, closing
// =============================================================================
bool OpenSync(const std::string& id, std::string& err, std::string& note) {
    SessionFile f;
    bool fromBackup = false;
    if (!LoadSessionFile(id, f, true, fromBackup, err)) return false;

    // The session's own copy first; the original file only if the copy is gone.
    Prefab p;
    std::string perr;
    bool fromSource = false;
    if (!PrefabParseFile(WideToUtf8Path(CopyFileOf(id)), p, perr)) {
        std::string serr;
        if (f.meta.prefabSource.empty() || !PrefabParseFile(f.meta.prefabSource, p, serr)) {
            err = "its copy of the prefab could not be read (" + perr + ")";
            return false;
        }
        fromSource = true;
    }
    if (!f.meta.prefabName.empty()) p.name = f.meta.prefabName;
    p.path = f.meta.prefabSource;

    BuildSnapshot snap = f.snap;
    if (f.meta.fingerprint && p.fingerprint != f.meta.fingerprint) {
        // Different blocks at the same numbers. Applying the progress would
        // mark the wrong blocks done, which is worse than marking none.
        snap.state.clear();
        snap.history.clear();
        snap.redo.clear();
        snap.mistakes.clear();
        snap.locked = false;
        note = "the prefab is not the one this build was started from, so progress "
               "could not be matched - its position and settings were kept";
    }
    f.meta.fingerprint = p.fingerprint;
    f.meta.blocks = p.blocks.size();

    if (fromSource) {
        std::string e2;
        PrefabWriteJson(p, CopyFileOf(id), e2);   // put the missing copy back
    }
    if (fromBackup) {
        // Keep the damaged one for the curious, out of the way of the next save
        // - which would otherwise rotate it into the backup slot over the good
        // copy we just loaded.
        MoveFileExW(StateFileOf(id).c_str(), (StateFileOf(id) + L".damaged").c_str(),
            MOVEFILE_REPLACE_EXISTING);
        note += (note.empty() ? "" : "; ");
        note += "the newest save was damaged, so the one before it was used";
    }

    auto shared = std::make_shared<const Prefab>(std::move(p));
    BuildStop();
    BuildInstallPrefab(shared);
    std::string importNote;
    BuildImportState(snap, importNote);
    if (!importNote.empty()) note += (note.empty() ? "" : "; ") + importNote;

    {
        std::lock_guard lock(g_sessMtx);
        g_active = f.meta;
        g_haveActive = true;
        g_saveError.clear();
    }
    g_metaRev.fetch_add(1);
    WriteActivePointer(id);
    if (snap.shown) BuildStart();
    return true;
}

bool StartNewSync(const std::string& path, std::string& err, std::string& msg) {
    Prefab p;
    if (!PrefabParseFile(path, p, err)) return false;

    CreateDirectoryW(RootDir().c_str(), nullptr);
    const std::string id = MakeId(p.name);
    if (!CreateDirectoryW(DirOf(id).c_str(), nullptr)) {
        err = "could not create a folder for it under build_sessions";
        return false;
    }

    std::string copyErr;
    const bool copied = PrefabWriteJson(p, CopyFileOf(id), copyErr);

    SessionMeta m;
    m.id = id;
    m.name = p.name;
    m.prefabName = p.name;
    m.prefabSource = path;
    m.createdUnix = BuildUnixMs() / 1000;
    m.fingerprint = p.fingerprint;
    m.blocks = p.blocks.size();

    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %zu blocks, %zu materials, %d x %d x %d",
        p.name.c_str(), p.blocks.size(), p.names.size(), (int)p.SizeX(), (int)p.SizeY(),
        (int)p.SizeZ());
    msg = buf;
    if (p.airSkipped) msg += ", " + std::to_string(p.airSkipped) + " air cells ignored";

    auto shared = std::make_shared<const Prefab>(std::move(p));
    BuildStop();
    BuildInstallPrefab(shared);

    // A new build starts at your feet, turned the way it was drawn, free to
    // move. View settings carry over from whatever you had last.
    g_buildAnchorLocked.store(false);
    g_buildRotation.store(0);
    BuildSetOriginToPlayer();

    {
        std::lock_guard lock(g_sessMtx);
        g_active = m;
        g_haveActive = true;
        g_saveError.clear();
    }
    g_metaRev.fetch_add(1);
    WriteActivePointer(id);
    BuildStart();
    SaveActiveSync();

    if (!copied)
        msg += " - but its own copy of the prefab could not be written (" + copyErr +
               "), so keep the original file where it is";
    return true;
}

void CloseSync() {
    SaveActiveSync();
    BuildStop();
    BuildInstallPrefab(nullptr);
    {
        std::lock_guard lock(g_sessMtx);
        g_active = SessionMeta{};
        g_haveActive = false;
        g_saveError.clear();
    }
    ClearActivePointer();
}

bool DuplicateSync(const std::string& id, bool keepProgress, std::string& err,
    std::string& newName) {
    if (SessionActiveId() == id) SaveActiveSync();

    SessionFile f;
    bool fromBackup = false;
    if (!LoadSessionFile(id, f, true, fromBackup, err)) return false;

    const std::string newId = MakeId(f.meta.prefabName.empty() ? f.meta.name
                                                               : f.meta.prefabName);
    if (!CreateDirectoryW(DirOf(newId).c_str(), nullptr)) {
        err = "could not create a folder for the copy";
        return false;
    }
    if (!CopyFileW(CopyFileOf(id).c_str(), CopyFileOf(newId).c_str(), FALSE)) {
        RemoveDirectoryW(DirOf(newId).c_str());
        err = "could not copy its prefab";
        return false;
    }

    f.meta.id = newId;
    f.meta.name = OneLine(f.meta.name + " (copy)");
    f.meta.createdUnix = BuildUnixMs() / 1000;
    // A copy is for putting the same thing up somewhere else, so it starts
    // unlocked and hidden, wherever the original was.
    f.snap.locked = false;
    f.snap.shown = false;
    if (!keepProgress) {
        f.snap.state.clear();
        f.snap.history.clear();
        f.snap.redo.clear();
        f.snap.mistakes.clear();
        f.snap.activeMs = 0;
        f.snap.placed = 0;
        f.snap.layerValid = false;
    }

    const std::string text = Serialize(f.meta, f.snap);
    std::lock_guard w(g_writeMtx);
    if (!WriteFileAtomic(StateFileOf(newId), text, err, false)) return false;
    newName = f.meta.name;
    return true;
}

void DeleteFolderContents(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;   // we never make any
        DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void OpenAsync(const std::string& id, bool resuming) {
    if (!ValidId(id)) return;
    if (SessionActiveId() == id) return;
    if (g_sessionBusy.exchange(true)) return;
    SetStatus(resuming ? "picking up where you left off..." : "opening...");

    StartWorker([id, resuming]() {
        WorkerGuard guard;
        // The one being left is saved first - switching must never lose work.
        if (SessionHasActive()) SaveActiveSync();

        std::string err, note;
        if (OpenSync(id, err, note)) {
            std::string msg = (resuming ? "picked up " : "opened ") + SessionActiveName() +
                              " where you left off";
            if (!note.empty()) msg += " - " + note;
            SetStatus(msg);
        }
        else {
            SetStatus("could not open that build: " + err);
            LogToFile("session: open of " + id + " failed: " + err);
        }
        g_listStale.store(true);
        g_sessionBusy.store(false);
    });
}

// Follow-along learns the placement packet once per launch - its type moves
// every time the client starts. This used to be armed by drawing the Build
// tab, so it never started while the overlay was hidden, which is exactly
// when you are building. Now it arms itself whenever a ghost is up.
void ArmFollowAlong(long long now) {
    static long long lastArm = 0;
    if (!g_buildActive.load() || !g_pktOutHooked.load()) return;
    if (g_placeLearning.load()) return;
    {
        std::lock_guard lock(g_placeMtx);
        if (g_placeLayout.learned) return;
    }
    // Throttled: a round that times out without settling would otherwise
    // re-arm every frame forever.
    if (now - lastArm < 4000) return;
    lastArm = now;
    PlaceLearnArm();
}

} // namespace

// =============================================================================
// Public
// =============================================================================
std::string SessionFolder() { return WideToUtf8Path(RootDir()); }

std::string SessionStatus() {
    std::lock_guard lock(g_sessMtx);
    return g_statusText;
}

std::string SessionSaveError() {
    std::lock_guard lock(g_sessMtx);
    return g_saveError;
}

long long SessionLastSavedMs() { return g_lastSavedMs.load(); }

bool SessionHasActive() {
    std::lock_guard lock(g_sessMtx);
    return g_haveActive;
}

std::string SessionActiveId() {
    std::lock_guard lock(g_sessMtx);
    return g_haveActive ? g_active.id : std::string();
}

std::string SessionActiveName() {
    std::lock_guard lock(g_sessMtx);
    return g_haveActive ? g_active.name : std::string();
}

std::string SessionActiveNote() {
    std::lock_guard lock(g_sessMtx);
    return g_haveActive ? g_active.note : std::string();
}

void SessionRename(const std::string& name) {
    const std::string clean = OneLine(name);
    if (clean.empty()) return;
    {
        std::lock_guard lock(g_sessMtx);
        if (!g_haveActive || g_active.name == clean) return;
        g_active.name = clean;
    }
    g_metaRev.fetch_add(1);
}

void SessionSetNote(const std::string& note) {
    const std::string clean = OneLine(note);
    {
        std::lock_guard lock(g_sessMtx);
        if (!g_haveActive || g_active.note == clean) return;
        g_active.note = clean;
    }
    g_metaRev.fetch_add(1);
}

void SessionStartNew(const std::string& prefabPath) {
    if (g_sessionBusy.exchange(true)) return;
    SetStatus("loading...");
    StartWorker([prefabPath]() {
        WorkerGuard guard;
        if (SessionHasActive()) SaveActiveSync();
        std::string err, msg;
        if (StartNewSync(prefabPath, err, msg)) SetStatus("started " + msg);
        else SetStatus("could not start it: " + err);
        g_listStale.store(true);
        g_sessionBusy.store(false);
    });
}

void SessionOpen(const std::string& id) { OpenAsync(id, false); }

void SessionClose() {
    if (!SessionHasActive()) return;
    if (g_sessionBusy.exchange(true)) return;
    StartWorker([]() {
        WorkerGuard guard;
        const std::string name = SessionActiveName();
        CloseSync();
        SetStatus("put " + name + " away - it is saved, and in the list below");
        g_listStale.store(true);
        g_sessionBusy.store(false);
    });
}

bool SessionDelete(const std::string& id) {
    if (!ValidId(id)) return false;
    if (g_sessionBusy.exchange(true)) return false;

    if (SessionActiveId() == id) {
        // Nothing to save - it is about to be deleted.
        BuildStop();
        BuildInstallPrefab(nullptr);
        {
            std::lock_guard lock(g_sessMtx);
            g_active = SessionMeta{};
            g_haveActive = false;
        }
        ClearActivePointer();
    }
    WaitForInFlightSave();
    {
        std::lock_guard w(g_writeMtx);
        DeleteFolderContents(DirOf(id));
    }
    const bool ok = RemoveDirectoryW(DirOf(id).c_str()) != 0;
    SetStatus(ok ? "deleted" : "deleted what it could - something else is in that folder");
    g_sessionBusy.store(false);
    SessionRefreshList();
    return ok;
}

void SessionDuplicate(const std::string& id, bool keepProgress) {
    if (!ValidId(id)) return;
    if (g_sessionBusy.exchange(true)) return;
    StartWorker([id, keepProgress]() {
        WorkerGuard guard;
        std::string err, newName;
        if (DuplicateSync(id, keepProgress, err, newName))
            SetStatus("made " + newName + " - open it and put it where you want it");
        else
            SetStatus("could not copy it: " + err);
        g_listStale.store(true);
        g_sessionBusy.store(false);
    });
}

bool SessionSaveNow() {
    if (g_sessionBusy.load()) return false;
    return SaveActiveSync();
}

void SessionRefreshList() {
    const std::string activeId = SessionActiveId();
    std::vector<BuildSessionInfo> out;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((RootDir() + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::string id = WideToUtf8Path(fd.cFileName);
            if (!ValidId(id)) continue;          // "." and ".." included

            SessionFile f;
            bool fromBackup = false;
            std::string err;
            if (!LoadSessionFile(id, f, false, fromBackup, err)) continue;

            BuildSessionInfo info;
            info.id = id;
            info.name = f.meta.name.empty() ? id : f.meta.name;
            info.note = f.meta.note;
            info.prefabName = f.meta.prefabName;
            info.blocks = f.meta.blocks;
            info.done = f.done;
            info.skipped = f.skipped;
            info.createdUnix = f.meta.createdUnix;
            info.updatedUnix = f.updatedUnix;
            info.originX = f.snap.originX;
            info.originY = f.snap.originY;
            info.originZ = f.snap.originZ;
            info.activeMs = f.snap.activeMs;
            info.active = (id == activeId);
            info.fromBackup = fromBackup;
            out.push_back(std::move(info));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::sort(out.begin(), out.end(), [](const BuildSessionInfo& a, const BuildSessionInfo& b) {
        if (a.updatedUnix != b.updatedUnix) return a.updatedUnix > b.updatedUnix;
        return a.id > b.id;
    });

    std::lock_guard lock(g_sessMtx);
    g_list.swap(out);
    g_listStale.store(false);
}

std::vector<BuildSessionInfo> SessionList() {
    std::lock_guard lock(g_sessMtx);
    return g_list;
}

bool SessionNearby(BuildSessionInfo& out, int radius) {
    double x = 0, y = 0, z = 0;
    if (!PlayerPosition(x, y, z)) return false;

    bool found = false;
    double best = (double)radius * (double)radius;
    for (const BuildSessionInfo& s : SessionList()) {
        if (s.active || !s.blocks || s.done + s.skipped >= s.blocks) continue;
        const double dx = s.originX - x, dy = s.originY - y, dz = s.originZ - z;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= best) { best = d2; out = s; found = true; }
    }
    return found;
}

void SessionTick() {
    const long long now = NowSteadyMs();

    // ---- once: pick up where the last launch left off ---------------------
    static bool started = false;
    if (!started) {
        started = true;
        CreateDirectoryW(RootDir().c_str(), nullptr);
        PrefabScanFolder();
        SessionRefreshList();
        if (g_sessionAutoResume.load()) {
            const std::string id = ReadActivePointer();
            if (ValidId(id)) OpenAsync(id, true);
        }
    }

    {
        static long long lastList = 0;
        if (g_listStale.load() || now - lastList > 15000) {
            lastList = now;
            if (!g_sessionBusy.load()) SessionRefreshList();
        }
    }

    ArmFollowAlong(now);

    // ---- autosave ----------------------------------------------------------
    // Within a second of anything changing, and never when nothing has. A
    // steady stream of placements is saved at least every three seconds
    // rather than waiting for a pause that never comes.
    if (g_sessionBusy.load() || !SessionHasActive()) return;

    static uint64_t lastKey = 0;
    static long long changedAt = 0, dirtySince = 0, nextTryAt = 0;
    const uint64_t key = CurrentKey();
    if (key == g_savedKey.load()) { dirtySince = 0; return; }
    if (key != lastKey) {
        lastKey = key;
        changedAt = now;
        if (!dirtySince) dirtySince = now;
    }
    if (g_saveInFlight.load() || now < nextTryAt || now < g_saveRetryAtMs.load()) return;
    if (now - changedAt >= 600 || now - dirtySince >= 3000) {
        SaveActiveAsync();
        nextTryAt = now + 400;
    }
}

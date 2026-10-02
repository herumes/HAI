#include "pch.h"
#include "farmroute.hpp"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {

// A segment shorter than this is "not moving".
constexpr float kStill = 0.03f;
// A run of still segments at least this long shows as a stop on the map.
constexpr int kStopMs = 200;
// Speed is measured over this much time, the same way on both sides.
constexpr double kVelWindowMs = 50.0;
// Stood still further than this short of where the recording stood still,
// measured along the way you came in, hasn't reached that stop yet. Off to
// one side doesn't count - walls put you there.
constexpr float kStopReach = 0.35f;
// The first this-many ms of a stop are its way in.
constexpr float kGateMs = 150.0f;

float Dist(float ax, float ay, float az, float bx, float by, float bz) {
    const float dx = ax - bx, dy = ay - by, dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
float Dist(const FarmSample& a, const FarmSample& b) { return Dist(a.x, a.y, a.z, b.x, b.y, b.z); }

// Wrap into (-180, 180].
float AngDiff(float a, float b) {
    float d = std::fmod(a - b + 180.0f, 360.0f);
    if (d < 0) d += 360.0f;
    return d - 180.0f;
}

float AngLerp(float a, float b, float f) { return a + AngDiff(b, a) * f; }

} // namespace

// =============================================================================
// Route
// =============================================================================
double FarmRoute::Wrap(double t) const {
    const double p = Period();
    if (p <= 0) return 0;
    if (loop) {
        t = std::fmod(t, p);
        if (t < 0) t += p;
        return t;
    }
    return (std::max)(0.0, (std::min)(p, t));
}

int FarmRoute::SampleAt(double t) const {
    if (s.empty()) return 0;
    t = Wrap(t);
    int lo = 0, hi = (int)s.size() - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (s[mid].t <= t) lo = mid; else hi = mid - 1;
    }
    return lo;
}

void FarmRoute::PosAt(double t, float& x, float& y, float& z) const {
    t = Wrap(t);
    const int j = SampleAt(t);
    if (j + 1 >= (int)s.size()) { x = s[j].x; y = s[j].y; z = s[j].z; return; }
    const FarmSample& a = s[j];
    const FarmSample& b = s[j + 1];
    const double span = (double)b.t - a.t;
    const float f = span > 0 ? (float)((t - a.t) / span) : 0.0f;
    x = a.x + (b.x - a.x) * f;
    y = a.y + (b.y - a.y) * f;
    z = a.z + (b.z - a.z) * f;
}

float FarmRoute::YawAt(double t) const {
    t = Wrap(t);
    const int j = SampleAt(t);
    if (j + 1 >= (int)s.size()) return s[j].yaw;
    const double span = (double)s[j + 1].t - s[j].t;
    return AngLerp(s[j].yaw, s[j + 1].yaw, span > 0 ? (float)((t - s[j].t) / span) : 0.0f);
}

float FarmRoute::DistAt(double t) const {
    t = Wrap(t);
    const int j = SampleAt(t);
    if (j + 1 >= (int)s.size()) return cum[j];
    const double span = (double)s[j + 1].t - s[j].t;
    return cum[j] + (cum[j + 1] - cum[j]) * (span > 0 ? (float)((t - s[j].t) / span) : 0.0f);
}

uint32_t FarmRoute::KeysAt(double t) const {
    if (changes.empty()) return 0;
    t = Wrap(t);
    if (changes[0].first > t) return changes.back().second;
    int lo = 0, hi = (int)changes.size() - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (changes[mid].first <= t) lo = mid; else hi = mid - 1;
    }
    return changes[lo].second;
}

void FarmRoute::Finish() {
    const int n = (int)s.size();
    cum.assign(n, 0.0f);
    for (int i = 1; i < n; ++i) cum[i] = cum[i - 1] + Dist(s[i - 1], s[i]);
    length = n ? cum[n - 1] : 0.0f;
    durationMs = n ? s[n - 1].t - s[0].t : 0;

    stopEnd.assign(n, -1);
    for (int i = 0; i + 1 < n;) {
        if (SegLen(i) >= kStill) { ++i; continue; }
        const int a = i;
        while (i + 1 < n && SegLen(i) < kStill) ++i;
        if (s[i].t - s[a].t >= kStopMs) stopEnd[a] = i;
    }

    // Key changes, each moved by its nudge but never past its neighbours, so
    // nudges cannot reorder the steps.
    changes.clear();
    if (n) changes.emplace_back((double)s[0].t, s[0].keys);
    std::vector<int> bounds;
    for (int i = 1; i < n; ++i) if (s[i].keys != s[i - 1].keys) bounds.push_back(i);
    for (size_t k = 0; k < bounds.size(); ++k) {
        const int b = bounds[k];
        double t = s[b].t;
        auto it = nudges.find(b);
        if (it != nudges.end() && it->second != 0.0f) {
            const double lo = (k ? s[bounds[k - 1]].t : s[0].t) + 1.0;
            const double hi = (k + 1 < bounds.size() ? s[bounds[k + 1]].t : Period()) - 1.0;
            t = (std::max)(lo, (std::min)(hi, t + it->second));
        }
        changes.emplace_back(t, s[b].keys);
    }
    eff.assign(n, 0);
    for (int i = 0; i < n; ++i) eff[i] = KeysAt(s[i].t);

    // The grid playback lines up against. Speed comes from the position 50 ms
    // earlier, exactly as it is measured live.
    grid.clear();
    const double p = Period();
    if (n < 2 || p <= 0) return;
    const int cells = loop ? (std::max)(1, (int)(p / kGridMs)) : (int)(p / kGridMs) + 1;
    grid.resize(cells);
    for (int c = 0; c < cells; ++c) {
        const double t = c * (double)kGridMs;
        Cell& g = grid[c];
        PosAt(t, g.x, g.y, g.z);
        double t0 = t - kVelWindowMs;
        if (!loop && t0 < 0) t0 = 0;
        const double span = loop ? kVelWindowMs : t - t0;
        float ax, ay, az;
        PosAt(t0, ax, ay, az);
        if (span >= 10.0) {
            const float k = (float)(1000.0 / span);
            g.vx = (g.x - ax) * k; g.vy = (g.y - ay) * k; g.vz = (g.z - az) * k;
        }
        else g.vx = g.vy = g.vz = 0;
        g.speed = std::sqrt(g.vx * g.vx + g.vy * g.vy + g.vz * g.vz);
        const int j = SampleAt(t);
        if (j + 1 < n) {
            const double sp = (double)s[j + 1].t - s[j].t;
            g.ride = AngLerp(s[j].ride, s[j + 1].ride, sp > 0 ? (float)((t - s[j].t) / sp) : 0.0f);
        }
        else g.ride = s[j].ride;
    }
    // Direction of travel, carried through stops from the way you came in.
    // On a loop the first stops come in from the end of the lap.
    float ux = 0, uz = 0, age = 0;
    for (int pass = 0; pass < (loop ? 2 : 1); ++pass) {
        for (int c = 0; c < cells; ++c) {
            Cell& g = grid[c];
            const float h = std::sqrt(g.vx * g.vx + g.vz * g.vz);
            if (g.speed > 0.5f && h > 0.3f) { ux = g.vx / h; uz = g.vz / h; }
            g.ux = ux;
            g.uz = uz;
            age = g.speed < 0.5f ? age + kGridMs : 0.0f;
            g.stopAge = age;
        }
    }
    FindTurnKeys();
}

// Which keys turn the tractor, and how fast: the turning rate you actually
// got, fitted against which keys were down at the time. Least squares, so
// keys held together (W+D) don't blur each other.
void FarmRoute::FindTurnKeys() {
    turnPlus = turnMinus = -1;
    turnRate = 0;
    if (!hasRide || grid.size() < 20) return;
    uint32_t used = 0;
    for (const auto& c : changes) used |= c.second;
    std::vector<int> bits;
    for (int b = 0; b < 32; ++b) if (used & (1u << b)) bits.push_back(b);
    const int m = (int)bits.size();
    if (!m) return;
    std::vector<double> ata(m * m, 0.0), aty(m, 0.0);
    const int K = (int)grid.size();
    for (int c = 1; c < K; ++c) {
        const double rate = AngDiff(grid[c].ride, grid[c - 1].ride) * 1000.0 / kGridMs;
        if (std::fabs(rate) > 2000.0) continue;
        const uint32_t k = KeysAt(c * (double)kGridMs);
        for (int i = 0; i < m; ++i) {
            if (!(k & (1u << bits[i]))) continue;
            aty[i] += rate;
            for (int j = 0; j < m; ++j) if (k & (1u << bits[j])) ata[i * m + j] += 1.0;
        }
    }
    // Solve by Gaussian elimination, with a little damping for keys that
    // were barely used.
    for (int i = 0; i < m; ++i) ata[i * m + i] += 1.0;
    std::vector<double> beta = aty;
    for (int col = 0; col < m; ++col) {
        int piv = col;
        for (int r = col + 1; r < m; ++r) if (std::fabs(ata[r * m + col]) > std::fabs(ata[piv * m + col])) piv = r;
        if (std::fabs(ata[piv * m + col]) < 1e-9) continue;
        if (piv != col) {
            for (int j = 0; j < m; ++j) std::swap(ata[col * m + j], ata[piv * m + j]);
            std::swap(beta[col], beta[piv]);
        }
        for (int r = 0; r < m; ++r) {
            if (r == col) continue;
            const double f = ata[r * m + col] / ata[col * m + col];
            if (f == 0) continue;
            for (int j = col; j < m; ++j) ata[r * m + j] -= f * ata[col * m + j];
            beta[r] -= f * beta[col];
        }
    }
    float bestPlus = 20.0f, bestMinus = 20.0f;
    for (int i = 0; i < m; ++i) {
        if (std::fabs(ata[i * m + i]) < 1e-9) continue;
        const float rate = (float)(beta[i] / ata[i * m + i]);
        if (rate > bestPlus) { bestPlus = rate; turnPlus = bits[i]; }
        if (-rate > bestMinus) { bestMinus = -rate; turnMinus = bits[i]; }
    }
    if (turnPlus >= 0 && turnMinus >= 0) turnRate = (std::min)(bestPlus, bestMinus);
    else turnPlus = turnMinus = -1;
}

std::vector<FarmStep> FarmSteps(const FarmRoute& r) {
    std::vector<FarmStep> out;
    const int n = (int)r.s.size();
    if (n < 2 || (int)r.cum.size() != n) return out;
    int first = 0;
    for (int i = 1; i <= n; ++i) {
        if (i < n && r.s[i].keys == r.s[first].keys && i != n - 1) continue;
        const int last = (i < n) ? i : n - 1;
        if (last <= first) continue;
        FarmStep st;
        st.first = first;
        st.last = last;
        st.keys = r.s[first].keys;
        st.length = r.cum[last] - r.cum[first];
        st.ms = r.s[last].t - r.s[first].t;
        st.stopped = st.length < 0.3f && st.ms >= kStopMs;
        st.dx = r.s[last].x - r.s[first].x;
        st.dz = r.s[last].z - r.s[first].z;
        out.push_back(st);
        first = last;
    }
    return out;
}

// =============================================================================
// Keys
// =============================================================================
namespace {
struct KeyWord { const char* name; int vk; };
const KeyWord kKeyWords[] = {
    { "Space", VK_SPACE }, { "Shift", VK_SHIFT }, { "Ctrl", VK_CONTROL }, { "Alt", VK_MENU },
    { "LShift", VK_LSHIFT }, { "RShift", VK_RSHIFT }, { "LCtrl", VK_LCONTROL }, { "RCtrl", VK_RCONTROL },
    { "LAlt", VK_LMENU }, { "RAlt", VK_RMENU }, { "Tab", VK_TAB }, { "Enter", VK_RETURN },
    { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT },
    { "LMB", VK_LBUTTON }, { "RMB", VK_RBUTTON }, { "MMB", VK_MBUTTON },
    { "Mouse4", VK_XBUTTON1 }, { "Mouse5", VK_XBUTTON2 }, { "CapsLock", VK_CAPITAL },
    { "Control", VK_CONTROL }, { "Spacebar", VK_SPACE },
};
}

std::string FarmKeyName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    for (const KeyWord& k : kKeyWords) if (k.vk == vk) return k.name;
    const UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    wchar_t name[48] = L"";
    if (sc && GetKeyNameTextW((LONG)(sc << 16), name, 48) > 0) {
        std::string o;
        for (const wchar_t* p = name; *p; ++p) if (*p > 32 && *p < 127) o += (char)*p;
        if (!o.empty()) return o;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "vk%d", vk);
    return buf;
}

std::string FarmKeysText(const std::vector<int>& vks, uint32_t keys) {
    std::string o;
    for (size_t i = 0; i < vks.size() && i < 32; ++i) {
        if (!(keys & (1u << i))) continue;
        if (!o.empty()) o += "+";
        o += FarmKeyName(vks[i]);
    }
    return o.empty() ? "(nothing)" : o;
}
std::string FarmKeysText(const FarmRoute& r, uint32_t keys) { return FarmKeysText(r.vks, keys); }

std::vector<int> FarmParseKeyList(const std::string& text) {
    std::vector<int> out;
    std::string w;
    auto flush = [&]() {
        if (w.empty()) return;
        int vk = 0;
        std::string up;
        for (char c : w) up += (char)toupper((unsigned char)c);
        if (up.size() == 1 && ((up[0] >= 'A' && up[0] <= 'Z') || (up[0] >= '0' && up[0] <= '9'))) vk = up[0];
        else if (up.size() >= 2 && up[0] == 'F' && isdigit((unsigned char)up[1])) {
            const int f = atoi(up.c_str() + 1);
            if (f >= 1 && f <= 24) vk = VK_F1 + f - 1;
        }
        else if (up.rfind("VK", 0) == 0 && up.size() > 2) vk = atoi(up.c_str() + 2);
        else {
            for (const KeyWord& k : kKeyWords) {
                std::string kn;
                for (const char* p = k.name; *p; ++p) kn += (char)toupper((unsigned char)*p);
                if (kn == up) { vk = k.vk; break; }
            }
        }
        if (vk > 0 && vk < 256 && std::find(out.begin(), out.end(), vk) == out.end() && out.size() < 32)
            out.push_back(vk);
        w.clear();
    };
    for (char c : text) {
        if (isspace((unsigned char)c) || c == ',' || c == '+' || c == ';') flush();
        else w += c;
    }
    flush();
    return out;
}

std::string FarmFormatKeyList(const std::vector<int>& vks) {
    std::string o;
    for (int vk : vks) {
        if (!o.empty()) o += ' ';
        o += FarmKeyName(vk);
    }
    return o;
}

// =============================================================================
// Files
// =============================================================================
// Version 2 adds the tractor's facing to every sample, and nudges in
// milliseconds. Version 1 files (nudges in blocks) still load.
bool FarmSaveRoute(const FarmRoute& r, const std::wstring& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    std::string name = r.name;
    for (char& c : name) if (c == '\r' || c == '\n') c = ' ';
    f << "hyai-farm 2\n";
    f << "name=" << name << "\n";
    f << "loop=" << (r.loop ? 1 : 0) << "\n";
    f << "yaw=" << (r.hasYaw ? 1 : 0) << "\n";
    f << "ride=" << (r.hasRide ? 1 : 0) << "\n";
    f << "frames=" << (r.perFrame ? 1 : 0) << "\n";
    f << "created=" << r.createdUnix << "\n";
    f << "keys=";
    for (size_t i = 0; i < r.vks.size(); ++i) f << (i ? " " : "") << r.vks[i];
    f << "\n";
    for (const auto& [idx, ms] : r.nudges)
        if (ms != 0.0f) f << "nudgems=" << idx << " " << ms << "\n";
    f << "samples=" << r.s.size() << "\n";
    char line[192];
    for (const FarmSample& p : r.s) {
        snprintf(line, sizeof(line), "%d %.3f %.3f %.3f %.2f %.2f %u\n", p.t, p.x, p.y, p.z, p.yaw, p.ride, p.keys);
        f << line;
    }
    return (bool)f;
}

bool FarmLoadRoute(const std::wstring& path, FarmRoute& out, std::string& why) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { why = "can't open it"; return false; }
    std::string line;
    if (!std::getline(f, line) || line.rfind("hyai-farm", 0) != 0) { why = "not a farm route file"; return false; }
    const int version = atoi(line.c_str() + 9);
    FarmRoute r;
    std::map<int, float> blockNudges;
    bool inSamples = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (inSamples) {
            FarmSample p;
            unsigned keys = 0;
            if (version >= 2) {
                if (sscanf_s(line.c_str(), "%d %f %f %f %f %f %u", &p.t, &p.x, &p.y, &p.z, &p.yaw, &p.ride, &keys) != 7)
                    continue;
            }
            else if (sscanf_s(line.c_str(), "%d %f %f %f %f %u", &p.t, &p.x, &p.y, &p.z, &p.yaw, &keys) != 6)
                continue;
            p.keys = keys;
            r.s.push_back(p);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "name") r.name = v;
        else if (k == "loop") r.loop = v == "1";
        else if (k == "yaw") r.hasYaw = v == "1";
        else if (k == "ride") r.hasRide = v == "1";
        else if (k == "frames") r.perFrame = v == "1";
        else if (k == "created") r.createdUnix = atoll(v.c_str());
        else if (k == "keys") {
            std::istringstream ss(v);
            int vk;
            while (ss >> vk) if (vk > 0 && vk < 256 && r.vks.size() < 32) r.vks.push_back(vk);
        }
        else if (k == "nudgems" || k == "nudge") {
            int idx = 0;
            float val = 0;
            if (sscanf_s(v.c_str(), "%d %f", &idx, &val) == 2) (k == "nudge" ? blockNudges : r.nudges)[idx] = val;
        }
        else if (k == "samples") inSamples = true;
    }
    if (r.s.size() < 2) { why = "it has no lap in it"; return false; }
    if (r.vks.empty()) { why = "it has no keys in it"; return false; }
    r.file = path;
    r.Finish();
    // Old nudges were in blocks; the same shift in time is the blocks over the
    // speed you were doing there.
    if (!blockNudges.empty()) {
        const int n = (int)r.s.size();
        for (const auto& [b, blocks] : blockNudges) {
            if (b <= 0 || b >= n) continue;
            const int a = (std::max)(0, b - 5), c = (std::min)(n - 1, b + 5);
            const double dt = (double)r.s[c].t - r.s[a].t;
            const double speed = dt > 0 ? (r.cum[c] - r.cum[a]) / (dt / 1000.0) : 0.0;
            if (speed >= 1.0) r.nudges[b] = (float)(blocks / speed * 1000.0);
        }
        r.Finish();
    }
    out = std::move(r);
    return true;
}

// =============================================================================
// Recorder
// =============================================================================
void FarmRecorder::Begin(const std::vector<int>& vks, bool autoClose, bool perFrame, long long nowMs) {
    *this = FarmRecorder();
    vks_ = vks;
    autoClose_ = autoClose;
    perFrame_ = perFrame;
    active_ = true;
    t0_ = nowMs;
}

void FarmRecorder::Push(long long nowMs, float x, float y, float z, float yaw, float ride, uint32_t keys) {
    FarmSample p;
    p.t = (int32_t)(nowMs - t0_);
    p.x = x; p.y = y; p.z = z;
    p.yaw = yaw;
    p.ride = ride;
    p.keys = keys;
    s_.push_back(p);
    lastPushMs_ = nowMs;
}

bool FarmRecorder::Feed(long long nowMs, float x, float y, float z, bool haveYaw, float yaw,
    bool haveRide, float ride, uint32_t keys) {
    if (!active_ || closed_) return false;
    if (!haveYaw) yaw = s_.empty() ? 0.0f : s_.back().yaw;
    if (!haveRide) ride = s_.empty() ? 0.0f : s_.back().ride;
    if (startIdx_ >= 0) {
        ++(haveYaw ? yawSeen_ : yawMissed_);
        ++(haveRide ? rideSeen_ : rideMissed_);
    }

    if (s_.empty()) {
        Push(nowMs, x, y, z, yaw, ride, keys);
        if (keys) {   // already driving when recording began
            startIdx_ = 0;
            startMs_ = nowMs;
            sx_ = x; sy_ = y; sz_ = z;
        }
        return false;
    }

    const FarmSample last = s_.back();
    const float d = Dist(last.x, last.y, last.z, x, y, z);
    const bool keysChanged = keys != last.keys;
    const bool moved = d > 0.0005f;
    const bool turned = (haveYaw && std::fabs(AngDiff(yaw, last.yaw)) > 0.25f) ||
                        (haveRide && std::fabs(AngDiff(ride, last.ride)) > 0.25f);
    if (!perFrame_ && !keysChanged && !moved && !turned && nowMs - lastPushMs_ < 50) return false;

    if (startIdx_ < 0) {
        if (!keys && d <= 0.01f) {
            // Still idle before the lap: keep only the latest reading.
            s_.back() = FarmSample{ (int32_t)(nowMs - t0_), x, y, z, yaw, ride, keys };
            lastPushMs_ = nowMs;
            return false;
        }
        if (keys && !moved) {
            startIdx_ = (int)s_.size();
            sx_ = x; sy_ = y; sz_ = z;
        }
        else {
            startIdx_ = (int)s_.size() - 1;
            sx_ = last.x; sy_ = last.y; sz_ = last.z;
            // Pressed and moved within one reading: the keys belong to the
            // step that leaves the standing spot.
            s_.back().keys = keys;
        }
        startMs_ = nowMs;
    }

    if (keysChanged) ++keyChanges_;
    travelled_ += d;
    fromStart_ = Dist(x, y, z, sx_, sy_, sz_);
    maxFromStart_ = (std::max)(maxFromStart_, fromStart_);
    if (!haveDir_ && fromStart_ > 2.0f) {
        const float dx = x - sx_, dz = z - sz_, l = std::sqrt(dx * dx + dz * dz);
        if (l > 0.5f) { dirX_ = dx / l; dirZ_ = dz / l; haveDir_ = true; }
    }

    // Back where the lap began, going the same way: that is one lap. The frame
    // step at tractor speed is well over a block, so test the whole step.
    if (autoClose_ && travelled_ > 40.0f && maxFromStart_ > 16.0f && moved) {
        const float ex = x - last.x, ey = y - last.y, ez = z - last.z;
        const float len2 = ex * ex + ey * ey + ez * ez;
        float t = len2 > 1e-8f ? ((sx_ - last.x) * ex + (sy_ - last.y) * ey + (sz_ - last.z) * ez) / len2 : 0.0f;
        t = (std::max)(0.0f, (std::min)(1.0f, t));
        const float gap = Dist(last.x + ex * t, last.y + ey * t, last.z + ez * t, sx_, sy_, sz_);
        bool sameWay = true;
        if (haveDir_) {
            const float ml = std::sqrt(ex * ex + ez * ez);
            sameWay = ml < 1e-4f || (ex * dirX_ + ez * dirZ_) / ml > 0.5f;
        }
        if (gap < 2.5f && sameWay) {
            // End on the start, at the moment you crossed it, so the lap joins
            // up without a jog in place or in time.
            const long long lastMs = (long long)last.t + t0_;
            const long long crossMs = lastMs + (long long)std::lround((double)(nowMs - lastMs) * t);
            const FarmSample& first = s_[startIdx_];
            Push((std::max)(crossMs, lastMs + 1), sx_, sy_, sz_, first.yaw, first.ride, last.keys);
            closed_ = true;
            return true;
        }
    }

    Push(nowMs, x, y, z, yaw, ride, keys);
    return false;
}

bool FarmRecorder::Finish(FarmRoute& out, std::string& why) {
    active_ = false;
    if (startIdx_ < 0 || startIdx_ >= (int)s_.size()) {
        why = "You didn't move - nothing was recorded.";
        return false;
    }
    std::vector<FarmSample> v(s_.begin() + startIdx_, s_.end());

    uint32_t anyKeys = 0;
    for (const FarmSample& p : v) anyKeys |= p.keys;
    if (!anyKeys) {
        why = "None of the keys you drove with are on the record list. Add them on Settings > Auto farm.";
        return false;
    }
    if (travelled_ < 5.0f || v.size() < 4) {
        why = "That was too short to be a lap.";
        return false;
    }

    bool loop = closed_;
    if (!closed_) {
        // Drop the standing about after the last key came up, keeping the
        // release itself.
        while (v.size() > 2) {
            const FarmSample& a = v[v.size() - 2];
            const FarmSample& b = v.back();
            if (b.keys || a.keys || Dist(a, b) > 0.01f) break;
            v.pop_back();
        }
        // Stopped by hand right where it started: still a lap.
        if (travelled_ > 40.0f && maxFromStart_ > 16.0f &&
            Dist(v.back().x, v.back().y, v.back().z, sx_, sy_, sz_) < 3.0f) {
            FarmSample end = v.back();
            end.x = sx_; end.y = sy_; end.z = sz_;
            end.t += 1;
            v.push_back(end);
            loop = true;
        }
    }

    const int32_t t0 = v.front().t;
    for (FarmSample& p : v) p.t -= t0;

    FarmRoute r;
    r.vks = vks_;
    r.s = std::move(v);
    r.loop = loop;
    // A reading missed here and there is fine; mostly missing means it isn't
    // set up, and lining up by stale numbers would be worse than not at all.
    r.hasYaw = yawSeen_ > 0 && yawMissed_ * 20 <= yawSeen_;
    r.hasRide = rideSeen_ > 0 && rideMissed_ * 20 <= rideSeen_;
    r.perFrame = perFrame_;
    r.createdUnix = (long long)time(nullptr);
    r.Finish();
    out = std::move(r);
    return true;
}

// =============================================================================
// Follower
// =============================================================================
float FarmFollower::Progress() const {
    if (!r_ || r_->Period() <= 0) return 0;
    return (float)(std::max)(0.0, (std::min)(1.0, tau_ / r_->Period()));
}

int FarmFollower::Segment() const { return r_ ? r_->SampleAt(tau_) : 0; }

bool FarmFollower::InStop() const {
    if (!r_ || r_->grid.empty() || w_.empty()) return false;
    return r_->grid[Map()].speed < 0.5f;
}

float FarmFollower::ToNextChangeMs() const {
    if (!r_ || !running_ || r_->changes.empty()) return -1;
    const double lead = (cfg_.frameLocked ? dtEst_ : 8.0) + cfg_.extraLeadMs;
    const double from = r_->Wrap(tau_ + lead);
    const uint32_t now = r_->KeysAt(from);
    for (const auto& c : r_->changes)
        if (c.first > from && c.second != now) return (float)(c.first - from);
    if (r_->loop)
        for (const auto& c : r_->changes)
            if (c.second != now) return (float)(c.first + r_->Period() - from);
    return -1;
}

void FarmFollower::Fail(FarmEnd e, const std::string& why) {
    running_ = false;
    end_ = e;
    endText_ = why;
    last_ = FarmFollowOut{};
}

int FarmFollower::Map() const {
    int best = 0;
    float bw = -1;
    for (int i = 0; i < (int)w_.size(); ++i) if (w_[i] > bw) { bw = w_[i]; best = i; }
    return best;
}

bool FarmFollower::Velocity(double nowMs, float& vx, float& vy, float& vz) const {
    if (hist_.size() < 2) return false;
    const Obs& cur = hist_.back();
    const Obs* from = nullptr;
    for (size_t i = hist_.size() - 1; i-- > 0;)
        if (nowMs - hist_[i].t >= kVelWindowMs) { from = &hist_[i]; break; }
    if (!from) {
        if (nowMs - hist_.front().t < 10.0) return false;
        from = &hist_.front();
    }
    const float k = (float)(1000.0 / (cur.t - from->t));
    vx = (cur.x - from->x) * k; vy = (cur.y - from->y) * k; vz = (cur.z - from->z) * k;
    return true;
}

// How badly cell `c` of the lap matches what is seen right now. Each term is
// capped, so one wildly wrong reading makes a guess unlikely rather than
// impossible - the next frames get to argue.
float FarmFollower::Cost(int c, const FarmObs& o, bool haveVel, float vx, float vy, float vz, bool posOnly) const {
    const FarmRoute::Cell& g = r_->grid[c];
    const float dx = o.x - g.x, dy = o.y - g.y, dz = o.z - g.z;
    const float sp = cfg_.posSigma;
    float cost = (std::min)(dx * dx + dy * dy + dz * dz, 36.0f) / (2.0f * sp * sp);
    if (!posOnly && haveVel) {
        const float ex = vx - g.vx, ey = vy - g.vy, ez = vz - g.vz;
        const float sv = 3.0f + 0.08f * g.speed;
        cost += (std::min)(ex * ex + ey * ey + ez * ez, 16.0f * sv * sv) / (2.0f * sv * sv);
    }
    if (usingRide_ && o.haveRide) {
        const float da = AngDiff(o.ride, g.ride);
        cost += (std::min)(da * da, 45.0f * 45.0f) / (2.0f * 5.0f * 5.0f);
    }
    // What the recording did while stood still - a spin, a key change against
    // a wall - it did at that spot. Stood still short of it, you haven't got
    // there yet.
    // Only the way in is barred: once a stop has begun, it plays out wherever
    // you are - creeping in the middle of a spin would drive you off sideways.
    // The bar is deeper than one frame can jump, so it can't be stepped over.
    if (!posOnly && haveVel && g.speed < 0.5f && g.stopAge <= kGateMs &&
        vx * vx + vy * vy + vz * vz < 0.25f && dx * -g.ux + dz * -g.uz > gateReach_)
        cost += 12.0f;
    return cost;
}

// How far short of the next stop you are, along the way into it, if the lap
// stops within the next second or so. Negative or -1: not short of one.
float FarmFollower::ShortOfStop(int c, const FarmObs& o) const {
    const int K = (int)r_->grid.size();
    for (int d = 0; d < 250; ++d) {
        int i = c + d;
        if (i >= K) { if (!r_->loop) return -1; i -= K; }
        const FarmRoute::Cell& g = r_->grid[i];
        if (g.speed < 0.5f) return (g.x - o.x) * g.ux + (g.z - o.z) * g.uz;
    }
    return -1;
}

// Moves every guess on by the time that passed. Mostly by about that much
// (the tractor going as fast as it did), with some room for slower, faster,
// or not at all - which the observations then settle.
//
// On average it must move on by exactly the time that passed. While nothing
// can be seen changing - stood still, spinning - this is all there is, and a
// clock running even 5% slow holds every spin a few frames too long.
void FarmFollower::Predict(double dtMs) {
    const int K = (int)w_.size();
    const double g = FarmRoute::kGridMs;
    const double m = dtMs / g;
    const double sc = (std::max)(4.0, 0.35 * dtMs) / g;
    const int maxO = (std::min)(400, (int)std::ceil(2.0 * m + 4.0 * sc));
    kernel_.assign(maxO + 1, 0.0f);
    std::vector<double> gauss(maxO + 1), slow(maxO + 1), fast(maxO + 1);
    double zg = 0, zs = 0, zf = 0;
    for (int o = 0; o <= maxO; ++o) {
        gauss[o] = std::exp(-(o - m) * (o - m) / (2.0 * sc * sc));
        slow[o] = o <= m ? 1.0 : 0.0;                   // anywhere from stopped to on time
        fast[o] = o >= m && o <= 2.0 * m ? 1.0 : 0.0;   // on time to twice as fast
        zg += gauss[o]; zs += slow[o]; zf += fast[o];
    }
    // The spread either side is the same size, so it doesn't shift the average.
    double mean = 0;
    for (int o = 0; o <= maxO; ++o) {
        const double k = 0.9 * gauss[o] / zg + (zs > 0 ? 0.05 * slow[o] / zs : 0.0) + (zf > 0 ? 0.05 * fast[o] / zf : 0.0);
        kernel_[o] = (float)k;
        mean += o * k;
    }
    // Whole cells can't express m exactly; tilt towards the side it lost.
    const double miss = m - mean;
    if (std::fabs(miss) > 1e-3) {
        const int lo = (int)std::floor(m), hi = lo + 1;
        if (hi <= maxO && lo >= 0) {
            const float move = (float)std::fabs(miss);
            if (miss > 0 && kernel_[lo] > move) { kernel_[lo] -= move; kernel_[hi] += move; }
            else if (miss < 0 && kernel_[hi] > move) { kernel_[hi] -= move; kernel_[lo] += move; }
        }
    }
    double z = 0;
    for (float k : kernel_) z += k;
    for (float& k : kernel_) k = (float)(k / z);

    std::fill(tmp_.begin(), tmp_.end(), 0.0f);
    const bool loop = r_->loop;
    for (int i = 0; i < K; ++i) {
        const float wi = w_[i];
        if (wi <= 0) continue;
        for (int o = 0; o <= maxO; ++o) {
            int j = i + o;
            if (j >= K) j = loop ? j % K : K - 1;
            tmp_[j] += wi * kernel_[o];
        }
    }
    w_.swap(tmp_);
}

bool FarmFollower::Update(const FarmObs& o, bool haveVel, float vx, float vy, float vz) {
    const int K = (int)w_.size();
    float minCost = 1e30f;
    for (int i = 0; i < K; ++i) {
        if (w_[i] <= 0) continue;
        tmp_[i] = Cost(i, o, haveVel, vx, vy, vz, false);
        minCost = (std::min)(minCost, tmp_[i]);
    }
    double sum = 0;
    float mx = 0;
    for (int i = 0; i < K; ++i) {
        if (w_[i] <= 0) continue;
        w_[i] *= std::exp(-(tmp_[i] - minCost));
        sum += w_[i];
        mx = (std::max)(mx, w_[i]);
    }
    if (!(sum > 0)) return false;
    const float floorW = mx * 1e-6f;
    for (int i = 0; i < K; ++i) {
        if (w_[i] < floorW) w_[i] = 0;
        else w_[i] = (float)(w_[i] / sum);
    }
    return true;
}

float FarmFollower::Reacquire(const FarmObs& o, bool haveVel, float vx, float vy, float vz, double aroundMs, double spanMs) {
    const int K = (int)w_.size();
    const double g = FarmRoute::kGridMs;
    const double p = r_->Period();
    auto inSpan = [&](int i) {
        if (spanMs <= 0) return true;
        double d = i * g - aroundMs;
        if (r_->loop) {
            d = std::fmod(d, p);
            if (d > p / 2) d -= p;
            if (d < -p / 2) d += p;
        }
        return std::fabs(d) <= spanMs;
    };
    float minCost = 1e30f;
    int best = -1;
    for (int i = 0; i < K; ++i) {
        if (!inSpan(i)) { tmp_[i] = 1e30f; continue; }
        tmp_[i] = Cost(i, o, haveVel, vx, vy, vz, false);
        if (tmp_[i] < minCost) { minCost = tmp_[i]; best = i; }
    }
    if (best < 0) return 1e30f;
    double sum = 0;
    for (int i = 0; i < K; ++i) {
        w_[i] = tmp_[i] - minCost < 20.0f ? std::exp(-(tmp_[i] - minCost)) : 0.0f;
        sum += w_[i];
    }
    for (float& w : w_) w = (float)(w / sum);
    const FarmRoute::Cell& c = r_->grid[best];
    return Dist(o.x, o.y, o.z, c.x, c.y, c.z);
}

bool FarmFollower::Start(std::shared_ptr<const FarmRoute> r, const FarmFollowCfg& cfg, double nowMs,
    const FarmObs& o, std::string& why) {
    *this = FarmFollower();
    r_ = std::move(r);
    cfg_ = cfg;
    if (!r_ || r_->grid.size() < 2 || r_->length < 1.0f || r_->changes.empty()) {
        end_ = FarmEnd::NoRoute;
        why = endText_ = "That route is empty.";
        return false;
    }
    usingRide_ = cfg_.useRide && r_->hasRide;
    const int K = (int)r_->grid.size();
    w_.assign(K, 0.0f);
    tmp_.assign(K, 0.0f);

    // Nearest the route gets to you, for the "get on it first" message.
    float nearest = 1e30f;
    for (const FarmRoute::Cell& c : r_->grid) nearest = (std::min)(nearest, Dist(o.x, o.y, o.z, c.x, c.y, c.z));
    if (!o.havePos || nearest > cfg_.startRadius) {
        char buf[200];
        snprintf(buf, sizeof(buf),
            "You're %.0f blocks from the route. Get on it (the map shows where) and start again.", nearest);
        end_ = FarmEnd::OffRoute;
        why = endText_ = buf;
        return false;
    }
    Reacquire(o, false, 0, 0, 0, 0, 0);
    const int c = Map();
    tau_ = c * (double)FarmRoute::kGridMs;
    off_ = Dist(o.x, o.y, o.z, r_->grid[c].x, r_->grid[c].y, r_->grid[c].z);
    cx_ = r_->grid[c].x;
    cz_ = r_->grid[c].z;
    hist_.push_back(Obs{ nowMs, o.x, o.y, o.z });
    lastObsMs_ = lastPosMs_ = progressMs_ = lapStartMs_ = nowMs;
    obsCount_ = 1;
    running_ = true;
    return true;
}

void FarmFollower::Resume(double nowMs) {
    lastPosMs_ = progressMs_ = nowMs;
    offSinceMs_ = 0;
    hist_.clear();
    obsCount_ = 0;
    speed_ = 0;
    unstickUntilMs_ = unstickNextMs_ = 0;
    gateReach_ = kStopReach;
    creepShort_ = 0;
    creepFutile_ = 0;
}

FarmFollowOut FarmFollower::Tick(double nowMs, const FarmObs& o) {
    if (!running_) return FarmFollowOut{};
    const FarmRoute& r = *r_;
    const double P = r.Period();

    if (!o.havePos) {
        if (nowMs - lastPosMs_ > cfg_.lostMs)
            Fail(FarmEnd::Lost, "Lost track of where you are (left the world, or the position stopped reading).");
        return last_;
    }
    lastPosMs_ = nowMs;

    // Time since the last reading. After a pause the first reading has no
    // "last", so it moves on by an ordinary frame rather than by the pause.
    double dt = obsCount_ ? nowMs - lastObsMs_ : dtEst_;
    dt = (std::max)(0.5, (std::min)(300.0, dt));
    if (obsCount_) dtEst_ = dtEst_ * 0.9 + (std::min)(dt, 50.0) * 0.1;
    lastObsMs_ = nowMs;
    ++obsCount_;

    hist_.push_back(Obs{ nowMs, o.x, o.y, o.z });
    while (hist_.size() > 2 && nowMs - hist_[1].t > 250.0) hist_.erase(hist_.begin());
    float vx = 0, vy = 0, vz = 0;
    const bool haveVel = Velocity(nowMs, vx, vy, vz);
    speed_ = haveVel ? std::sqrt(vx * vx + vy * vy + vz * vz) : 0.0f;

    // ---- which moment of the lap is this? ------------------------------------
    const double oldTau = tau_;
    Predict(dt);
    if (!Update(o, haveVel, vx, vy, vz)) Reacquire(o, haveVel, vx, vy, vz, tau_, 3000);
    int c = Map();
    off_ = Dist(o.x, o.y, o.z, r.grid[c].x, r.grid[c].y, r.grid[c].z);

    if (off_ > cfg_.maxOff) {
        // A lag spike or the server putting you back can move you further than
        // the guesses stretch. Look again nearby, then round the whole lap, and
        // accept it if you are squarely on the route a little way on or back.
        std::vector<float> keep = w_;
        float d = Reacquire(o, haveVel, vx, vy, vz, tau_, 4000);
        if (d > cfg_.maxOff * 0.6f) d = Reacquire(o, haveVel, vx, vy, vz, 0, 0);
        bool took = false;
        if (d <= cfg_.maxOff * 0.6f) {
            const int nc = Map();
            float fwd = r.DistAt(nc * (double)FarmRoute::kGridMs) - r.DistAt(oldTau);
            if (r.loop) {
                if (fwd < 0) fwd += r.length;
                if (fwd > r.length - 20.0f) fwd -= r.length;
            }
            took = fwd < 80.0f && fwd > -20.0f;
        }
        if (took) {
            c = Map();
            off_ = d;
        }
        else {
            w_ = std::move(keep);
            if (!offSinceMs_) offSinceMs_ = nowMs;
            if (nowMs - offSinceMs_ > 250 || off_ > cfg_.maxOff * 3.0f) {
                char buf[160];
                snprintf(buf, sizeof(buf), "You left the route (%.1f blocks off it).", off_);
                Fail(FarmEnd::OffRoute, buf);
                return FarmFollowOut{};
            }
            return last_;
        }
    }
    offSinceMs_ = 0;

    // The moment is the average of the guesses around the best one - not the
    // best one alone, which steps in whole cells and, while nothing changes,
    // runs slow. Moving fast, it is then split finer along the direction of
    // travel.
    {
        const FarmRoute::Cell& g = r.grid[c];
        const int K = (int)w_.size();
        double sw = 0, st = 0;
        for (int d = -40; d <= 40; ++d) {
            int i = c + d;
            if (i < 0 || i >= K) { if (!r.loop) continue; i = (i + K) % K; }
            sw += w_[i];
            st += w_[i] * d;
        }
        if (g.speed > 2.0f) {
            const float along = ((o.x - g.x) * g.vx + (o.y - g.y) * g.vy + (o.z - g.z) * g.vz) / g.speed;
            const double shift = along / g.speed * 1000.0;
            tau_ = c * (double)FarmRoute::kGridMs +
                (std::max)(-(double)FarmRoute::kGridMs, (std::min)((double)FarmRoute::kGridMs, shift));
        }
        else tau_ = (c + (sw > 0 ? st / sw : 0.0)) * (double)FarmRoute::kGridMs;
        tau_ = r.Wrap(tau_);
        cx_ = g.x;
        cz_ = g.z;
    }

    // ---- progress, laps, stuck --------------------------------------------------
    double delta = tau_ - oldTau;
    if (r.loop) {
        if (delta > P * 0.5) delta -= P;
        else if (delta < -P * 0.5) delta += P;
    }
    unwrapped_ += delta;
    if (unwrapped_ - progressMark_ >= 100.0) {
        progressMark_ = unwrapped_;
        progressMs_ = nowMs;
    }
    else if (unwrapped_ < progressMark_ - 2000.0) progressMark_ = unwrapped_;
    if (r.loop) {
        while (unwrapped_ >= (laps_ + 1) * P) {
            ++laps_;
            lastLapMs_ = (int)(nowMs - lapStartMs_);
            if (!bestLapMs_ || lastLapMs_ < bestLapMs_) bestLapMs_ = lastLapMs_;
            lapStartMs_ = nowMs;
        }
    }
    else if (tau_ >= P - 30.0) {
        Fail(FarmEnd::Finished, "Route finished.");
        return FarmFollowOut{};
    }
    if (nowMs - progressMs_ > cfg_.stuckMs) {
        char buf[160];
        snprintf(buf, sizeof(buf), "Stuck - no progress along the route for %.0f s.", cfg_.stuckMs / 1000.0f);
        Fail(FarmEnd::Stuck, buf);
        return FarmFollowOut{};
    }

    // ---- keys, a frame ahead ------------------------------------------------------------
    // What is sent now reaches the game on its next frame, whose tick covers
    // the frame after that. The recording's keys at the middle of that span
    // are the right ones: it rounds a key change to the nearest frame instead
    // of always the later one.
    FarmFollowOut out;
    const double lead = (cfg_.frameLocked ? dtEst_ * 1.5 : 8.0) + cfg_.extraLeadMs;
    const double tk = r.Wrap(tau_ + lead);
    out.keys = r.KeysAt(tk);
    if (r.hasYaw) {
        out.steer = true;
        out.yaw = r.YawAt(tk);
    }

    // ---- stalled -------------------------------------------------------------------------
    // Standing still where the recording was still moving - or short of where
    // it stopped - means the corner or the stop hasn't been reached. Nudge on
    // with the keys that were driving you there, briefly, then look again:
    // what a person does creeping up to a mark.
    const FarmRoute::Cell& here = r.grid[c];
    if (here.speed > 2.0f) {
        // Under way again: the next stop gets its own creeping.
        gateReach_ = kStopReach;
        creepShort_ = 0;
        creepFutile_ = 0;
    }
    const float shortBy = speed_ < 0.5f && here.stopAge <= kGateMs ? ShortOfStop(c, o) : -1.0f;
    const bool shortOfStop = shortBy > gateReach_;
    const bool stalled = here.speed > 2.0f && nowMs - progressMs_ > 400;
    // Short of a stop, the keys a frame ahead already belong to the stop (the
    // spin). Hold nothing between nudges rather than start it here.
    if (shortOfStop) out.keys = 0;
    if (nowMs < unstickUntilMs_) out.keys = unstickKeys_;
    else if (speed_ < 0.5f && nowMs >= unstickNextMs_ &&
             (stalled || (shortOfStop && nowMs - progressMs_ > 250))) {
        if (shortOfStop) {
            // Nudges that don't bring you closer mean something is in the way
            // (a wall a little short of where the recording stopped): take the
            // stop as reached here.
            if (creepShort_ > 0 && shortBy > creepShort_ - 0.05f) {
                if (++creepFutile_ >= 3) gateReach_ = 1e9f;
            }
            else creepFutile_ = 0;
            creepShort_ = shortBy;
        }
        // The keys held when the recording was going fastest in the last few
        // seconds are the ones that drive you along.
        uint32_t drive = 0;
        float best = 2.0f;
        const int K = (int)r.grid.size();
        for (int back = 0; back < 600; back += 4) {
            int i = c - back;
            if (i < 0) { if (!r.loop) break; i += K; }
            if (r.grid[i].speed > best) {
                const uint32_t k = r.KeysAt(i * (double)FarmRoute::kGridMs);
                if (k) { best = r.grid[i].speed; drive = k; }
            }
        }
        if (drive && (stalled || gateReach_ < 1e8f)) {
            unstickKeys_ = drive;
            // Creeping: a tap sized to the gap, so a small gap isn't overshot.
            const double tap = stalled ? 250.0 : (std::max)(30.0, (std::min)(150.0, shortBy * 40.0));
            unstickUntilMs_ = nowMs + tap;
            unstickNextMs_ = nowMs + tap + (stalled ? 650.0 : 300.0);
            ++unsticks_;
            out.keys = drive;
        }
    }

    // ---- keep the tractor pointing the way it did -------------------------------------
    // A spin a frame long or short leaves the tractor a couple of degrees off,
    // and on the next straight that becomes a block of drift. Where the
    // recording drove straight on, tap the turn key that brings the facing
    // back to what it was, the way you would. Every other frame, so each tap
    // is seen before the next.
    headingTapped_ = false;
    if (usingRide_ && o.haveRide && r.turnPlus >= 0 && here.speed > 2.0f && nowMs >= unstickUntilMs_) {
        const uint32_t turnKeys = (1u << r.turnPlus) | (1u << r.turnMinus);
        if (!(out.keys & turnKeys) && !lastHeadingTap_) {
            const float err = AngDiff(here.ride, o.ride);
            const float perFrame = r.turnRate * (float)dtEst_ / 1000.0f;
            if (std::fabs(err) > (std::max)(1.0f, perFrame * 0.6f)) {
                out.keys |= 1u << (err > 0 ? r.turnPlus : r.turnMinus);
                headingTapped_ = true;
                ++headingTaps_;
            }
        }
    }
    lastHeadingTap_ = headingTapped_;
    last_ = out;
    return out;
}

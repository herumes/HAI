#include "pch.h"
#include "common.hpp"

size_t HYTALE_CHAT_MAX = 250;
size_t MAX_CHAT_CHUNK = 150;
int    SEND_GAP_MS = 120;

std::atomic<bool> g_shuttingDown{ false };
std::atomic<int>  g_hooksInFlight{ 0 };
std::atomic<int>  g_workersAlive{ 0 };

std::atomic<HINTERNET> g_aiSession{ nullptr };
std::atomic<HINTERNET> g_aiConnect{ nullptr };
std::atomic<HINTERNET> g_aiRequest{ nullptr };
std::atomic<HINTERNET> g_txSession{ nullptr };
std::atomic<HINTERNET> g_txConnect{ nullptr };
std::atomic<HINTERNET> g_txRequest{ nullptr };

static std::mutex g_logFileMtx;
static std::mutex g_workerMtx;

// A worker plus a flag it sets on its way out. std::thread objects used to pile
// up in a vector for the life of the process - one per AI request, translation
// burst and scan - each holding an OS thread handle long after the thread ended.
struct WorkerSlot {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
};
// Deliberately never destroyed. When the game exits, the CRT runs this DLL's
// static destructors after every other thread is already gone, and a
// std::thread that was never joined or detached calls std::terminate() from its
// destructor - abort(), 0xC0000409 in this DLL, every time the game closed with
// a worker still registered. DllMain cannot detach them first either: on
// process exit the thread that held g_workerMtx may have been killed holding it.
static std::vector<WorkerSlot>& g_workers = *new std::vector<WorkerSlot>();
static std::chrono::steady_clock::time_point g_startTime = std::chrono::steady_clock::now();

static std::atomic<int> g_tzWanted{ 0 };
static int g_tzApplied = -1;
static std::mutex g_tzMtx;

void SetTimeZoneMode(int mode) {
    g_tzWanted.store((std::max)(0, (std::min)(2, mode)));
}

void EnsureTimeZone() {
    const int want = g_tzWanted.load();
    std::lock_guard lock(g_tzMtx);
    if (want == g_tzApplied) return;
    // "TZ=" with nothing after it removes the variable, which makes the CRT
    // fall back to the zone Windows is set to.
    if (want == 1) _putenv("TZ=");
    else if (want == 2) _putenv("TZ=UTC0");
    else _putenv("TZ=PST8PDT");
    _tzset();
    g_tzApplied = want;
}

void LogToFile(const std::string& msg) {
    std::lock_guard fileLock(g_logFileMtx);
    // Rotate at 8 MB so a long session can't grow the log without bound.
    {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(L"C:\\temp\\hyai_log.txt", GetFileExInfoStandard, &fa) &&
            (((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) > 8ull * 1024 * 1024)
            MoveFileExW(L"C:\\temp\\hyai_log.txt", L"C:\\temp\\hyai_log.old.txt", MOVEFILE_REPLACE_EXISTING);
    }
    std::ofstream log("C:\\temp\\hyai_log.txt", std::ios::app);
    if (log.is_open()) {
        EnsureTimeZone();
        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        struct tm timeinfo;
        localtime_s(&timeinfo, &now_c);
        char buf[80];
        strftime(buf, sizeof(buf), "%Y-%m-%d %I:%M:%S %p", &timeinfo);
        log << buf << " " << msg << std::endl;
    }
}

std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string ToUpper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::string TrimCopy(std::string s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

long long NowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string NowHHMM() {
    EnsureTimeZone();
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &now_c);
    char buf[16];
    strftime(buf, sizeof(buf), "%I:%M %p", &timeinfo);
    return buf;
}

std::string NowHHMMSS() {
    EnsureTimeZone();
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &now_c);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    return buf;
}

std::string FormatUptime() {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - g_startTime).count();
    if (secs < 0) secs = 0;
    long long h = secs / 3600;
    long long m = (secs % 3600) / 60;
    long long s = secs % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
    return buf;
}

std::wstring GetThisModuleDirectory() {
    HMODULE hm = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LogToFile),
        &hm);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(hm, path, MAX_PATH);
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        dir.resize(slash + 1);
    return dir;
}

std::string WideToUtf8Path(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string WideToUtf8(const wchar_t* w, int len) {
    if (!w || len <= 0) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string s(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], bytes, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, &s[0], (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, &s[0], (int)s.size(), &w[0], n);
    return w;
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    const char* hex = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\b': o += "\\b";  break;
        case '\f': o += "\\f";  break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) {
                o += "\\u00";
                o += hex[(c >> 4) & 0xF];
                o += hex[c & 0xF];
            }
            else {
                o += static_cast<char>(c);
            }
        }
    }
    return o;
}

static void AppendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out += (char)code;
    }
    else if (code < 0x800) {
        out += (char)(0xC0 | (code >> 6));
        out += (char)(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000) {
        out += (char)(0xE0 | (code >> 12));
        out += (char)(0x80 | ((code >> 6) & 0x3F));
        out += (char)(0x80 | (code & 0x3F));
    }
    else {
        out += (char)(0xF0 | (code >> 18));
        out += (char)(0x80 | ((code >> 12) & 0x3F));
        out += (char)(0x80 | ((code >> 6) & 0x3F));
        out += (char)(0x80 | (code & 0x3F));
    }
}

static bool ParseHex4(const std::string& in, size_t start, unsigned& code) {
    if (start + 4 > in.size()) return false;
    code = 0;
    for (int k = 0; k < 4; ++k) {
        char h = in[start + k];
        code <<= 4;
        if (h >= '0' && h <= '9')      code |= (unsigned)(h - '0');
        else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
        else return false;
    }
    return true;
}

bool JsonUnescape(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\' || i + 1 >= in.size()) {
            out += in[i];
            continue;
        }
        ++i;
        switch (in[i]) {
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        case 'u': {
            unsigned code = 0;
            if (!ParseHex4(in, i + 1, code)) {
                out += 'u';
                break;
            }
            i += 4;
            if (code >= 0xD800 && code <= 0xDBFF &&
                i + 6 < in.size() && in[i + 1] == '\\' && in[i + 2] == 'u') {
                unsigned low = 0;
                if (ParseHex4(in, i + 3, low) && low >= 0xDC00 && low <= 0xDFFF) {
                    i += 6;
                    code = 0x10000 + (((code - 0xD800) << 10) | (low - 0xDC00));
                }
            }
            AppendUtf8(out, code);
            break;
        }
        default:
            out += in[i];
            break;
        }
    }
    return true;
}

bool ExtractJsonStringField(const std::string& json, const char* key,
    std::string& value, size_t startPos) {
    value.clear();
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = startPos;
    while ((pos = json.find(pat, pos)) != std::string::npos) {
        if (pos > 0) {
            unsigned char prev = (unsigned char)json[pos - 1];
            if (prev == '_' || isalnum(prev)) {
                pos += pat.size();
                continue;
            }
        }

        size_t i = pos + pat.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n'))
            ++i;
        if (i >= json.size() || json[i] != ':') {
            pos += pat.size();
            continue;
        }
        ++i;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n'))
            ++i;
        if (i >= json.size())
            return false;

        if (json[i] != '"')
            return false;

        ++i;
        const size_t start = i;
        bool escaped = false;
        for (; i < json.size(); ++i) {
            if (escaped) { escaped = false; continue; }
            if (json[i] == '\\') { escaped = true; continue; }
            if (json[i] == '"') break;
        }
        if (i >= json.size())
            return false;

        std::string raw = json.substr(start, i - start);
        JsonUnescape(raw, value);
        return true;
    }
    return false;
}

bool ContainsWord(const std::string& text, const std::string& word) {
    if (text.empty() || word.empty()) return false;
    std::string lowerText = ToLower(text);
    std::string lowerWord = ToLower(word);
    return lowerText.find(lowerWord) != std::string::npos;
}

bool StartsWithCI(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) return false;
    return ToLower(text.substr(0, prefix.size())) == ToLower(prefix);
}

bool HasWholeWord(const std::string& lowerHay, const std::string& word) {
    if (word.empty() || lowerHay.empty()) return false;
    size_t pos = 0;
    const size_t n = word.size();
    while ((pos = lowerHay.find(word, pos)) != std::string::npos) {
        unsigned char before = (pos == 0) ? ' ' : (unsigned char)lowerHay[pos - 1];
        unsigned char after = (pos + n >= lowerHay.size()) ? ' ' : (unsigned char)lowerHay[pos + n];
        if (!isalnum(before) && !isalnum(after))
            return true;
        ++pos;
    }
    return false;
}

void CloseHttp(std::atomic<HINTERNET>& h) {
    HINTERNET p = h.exchange(nullptr);
    if (p) WinHttpCloseHandle(p);
}

void AbortHttp() {
    CloseHttp(g_aiRequest);
    CloseHttp(g_aiConnect);
    CloseHttp(g_aiSession);
    CloseHttp(g_txRequest);
    CloseHttp(g_txConnect);
    CloseHttp(g_txSession);
}

void StartWorker(std::function<void()> fn) {
    if (g_shuttingDown.load() || !fn) return;
    try {
        std::lock_guard lock(g_workerMtx);
        if (g_shuttingDown.load()) return;

        // Let go of workers that have finished. A finished thread is only
        // returning from its lambda, so detaching it is safe and frees the slot.
        for (auto it = g_workers.begin(); it != g_workers.end();) {
            if (it->done->load()) {
                if (it->thread.joinable()) it->thread.detach();
                it = g_workers.erase(it);
            }
            else ++it;
        }

        auto done = std::make_shared<std::atomic<bool>>(false);
        WorkerSlot slot;
        slot.done = done;
        slot.thread = std::thread([fn = std::move(fn), done]() {
            {
                WorkerGuard alive;
                try { fn(); }
                catch (...) {}
            }
            done->store(true);
            });
        g_workers.push_back(std::move(slot));
    }
    catch (...) {
        LogToFile("StartWorker failed");
    }
}

void DetachAllWorkers() {
    std::lock_guard lock(g_workerMtx);
    for (auto& w : g_workers) {
        if (w.thread.joinable())
            w.thread.detach();
    }
    g_workers.clear();
}


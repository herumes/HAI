#include "pch.h"
#include "extras.hpp"
#include "common.hpp"
#include "chat.hpp"
#include "settings.hpp"

#include <mutex>
#include <vector>
#include <sstream>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <random>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

// From legacy_core / settings
extern std::mutex g_mtx;
extern std::deque<ChatMessage> g_memory;
extern std::deque<GlobalChatEntry> g_globalChat;
extern int g_maxMemory;
extern int g_maxChatContext;
extern bool g_includeGlobalChat;

namespace Extras {

    // =============================================================================
    // Time zones (Windows Time Zone IDs + friendly aliases)
    // =============================================================================
    struct TzAlias {
        const char* key;   // lower
        const char* winId; // Windows registry TZ name, or "UTC"
    };

    static const TzAlias kTzTable[] = {
        { "utc", "UTC" }, { "gmt", "UTC" }, { "z", "UTC" },
        { "pst", "Pacific Standard Time" }, { "pdt", "Pacific Standard Time" },
        { "pacific", "Pacific Standard Time" }, { "la", "Pacific Standard Time" },
        { "los angeles", "Pacific Standard Time" }, { "seattle", "Pacific Standard Time" },
        { "mst", "Mountain Standard Time" }, { "mdt", "Mountain Standard Time" },
        { "mountain", "Mountain Standard Time" }, { "denver", "Mountain Standard Time" },
        { "cst", "Central Standard Time" }, { "cdt", "Central Standard Time" },
        { "central", "Central Standard Time" }, { "chicago", "Central Standard Time" },
        { "est", "Eastern Standard Time" }, { "edt", "Eastern Standard Time" },
        { "eastern", "Eastern Standard Time" }, { "ny", "Eastern Standard Time" },
        { "new york", "Eastern Standard Time" }, { "nyc", "Eastern Standard Time" },
        { "akst", "Alaskan Standard Time" }, { "alaska", "Alaskan Standard Time" },
        { "hst", "Hawaiian Standard Time" }, { "hawaii", "Hawaiian Standard Time" },
        { "cet", "W. Europe Standard Time" }, { "cest", "W. Europe Standard Time" },
        { "germany", "W. Europe Standard Time" }, { "berlin", "W. Europe Standard Time" },
        { "france", "Romance Standard Time" }, { "paris", "Romance Standard Time" },
        { "spain", "Romance Standard Time" }, { "madrid", "Romance Standard Time" },
        { "italy", "W. Europe Standard Time" }, { "rome", "W. Europe Standard Time" },
        { "uk", "GMT Standard Time" }, { "london", "GMT Standard Time" },
        { "bst", "GMT Standard Time" }, { "britain", "GMT Standard Time" },
        { "england", "GMT Standard Time" },
        { "netherlands", "W. Europe Standard Time" }, { "amsterdam", "W. Europe Standard Time" },
        { "poland", "Central European Standard Time" }, { "warsaw", "Central European Standard Time" },
        { "russia", "Russian Standard Time" }, { "moscow", "Russian Standard Time" },
        { "msk", "Russian Standard Time" },
        { "turkey", "Turkey Standard Time" }, { "istanbul", "Turkey Standard Time" },
        { "japan", "Tokyo Standard Time" }, { "tokyo", "Tokyo Standard Time" }, { "jst", "Tokyo Standard Time" },
        { "korea", "Korea Standard Time" }, { "seoul", "Korea Standard Time" }, { "kst", "Korea Standard Time" },
        { "china", "China Standard Time" }, { "beijing", "China Standard Time" }, { "cst china", "China Standard Time" },
        { "hong kong", "China Standard Time" },
        { "india", "India Standard Time" }, { "ist", "India Standard Time" }, { "delhi", "India Standard Time" },
        { "mumbai", "India Standard Time" },
        { "australia", "AUS Eastern Standard Time" }, { "sydney", "AUS Eastern Standard Time" },
        { "aest", "AUS Eastern Standard Time" }, { "melbourne", "AUS Eastern Standard Time" },
        { "brazil", "E. South America Standard Time" }, { "sao paulo", "E. South America Standard Time" },
        { "mexico", "Central Standard Time (Mexico)" }, { "canada", "Eastern Standard Time" },
        { "toronto", "Eastern Standard Time" }, { "vancouver", "Pacific Standard Time" },
    };

    static std::string ResolveWindowsTz(const std::string& query) {
        std::string k = ToLower(TrimCopy(query));
        if (k.empty() || k == "local" || k == "here" || k == "me" || k == "now")
            return {}; // local
        // strip common words
        if (k.rfind("time in ", 0) == 0) k = TrimCopy(k.substr(8));
        if (k.rfind("timezone ", 0) == 0) k = TrimCopy(k.substr(9));
        if (k.rfind("tz ", 0) == 0) k = TrimCopy(k.substr(3));

        for (const auto& a : kTzTable) {
            if (k == a.key) return a.winId;
        }
        // substring match for countries/cities
        for (const auto& a : kTzTable) {
            if (k.find(a.key) != std::string::npos || std::string(a.key).find(k) != std::string::npos)
                return a.winId;
        }
        // pass through as Windows TZ name if it looks like one
        return query;
    }

    static bool GetLocalSystemTimeAsUtc(SYSTEMTIME& utcOut) {
        GetSystemTime(&utcOut);
        return true;
    }

    static std::string FormatSystemTime(const SYSTEMTIME& st, const char* label) {
        char buf[128];
        // 12-hour with AM/PM + 24h
        const char* ampm = (st.wHour >= 12) ? "PM" : "AM";
        int h12 = st.wHour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u (%d:%02u %s) [%s]",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond,
            h12, st.wMinute, ampm,
            label ? label : "local");
        return buf;
    }

    std::string ToolGetTime(const std::string& query) {
        SYSTEMTIME utc{};
        GetSystemTime(&utc);

        std::string q = TrimCopy(query);
        std::string winTz = ResolveWindowsTz(q);

        if (winTz.empty()) {
            SYSTEMTIME local{};
            GetLocalTime(&local);
            TIME_ZONE_INFORMATION tzi{};
            GetTimeZoneInformation(&tzi);
            char nameA[128] = "local";
            WideCharToMultiByte(CP_UTF8, 0, tzi.StandardName, -1, nameA, 127, nullptr, nullptr);
            return std::string("Current local time: ") + FormatSystemTime(local, nameA);
        }

        if (ToUpper(winTz) == "UTC" || ToUpper(winTz) == "GMT") {
            return std::string("Current UTC/GMT: ") + FormatSystemTime(utc, "UTC");
        }

        // Convert UTC -> target zone via Windows TZ API
        DYNAMIC_TIME_ZONE_INFORMATION dtzi{};
        bool found = false;
        // Enumerate dynamic time zones
        DWORD i = 0;
        DYNAMIC_TIME_ZONE_INFORMATION enumTz{};
        while (EnumDynamicTimeZoneInformation(i, &enumTz) == ERROR_SUCCESS) {
            char stdName[128]{};
            char keyName[128]{};
            WideCharToMultiByte(CP_UTF8, 0, enumTz.StandardName, -1, stdName, 127, nullptr, nullptr);
            WideCharToMultiByte(CP_UTF8, 0, enumTz.TimeZoneKeyName, -1, keyName, 127, nullptr, nullptr);
            if (_stricmp(keyName, winTz.c_str()) == 0 || _stricmp(stdName, winTz.c_str()) == 0) {
                dtzi = enumTz;
                found = true;
                break;
            }
            // also match partial
            if (!found && (ToLower(keyName).find(ToLower(winTz)) != std::string::npos
                || ToLower(winTz).find(ToLower(keyName)) != std::string::npos)) {
                dtzi = enumTz;
                found = true;
                // keep searching for exact
            }
            ++i;
            if (i > 400) break;
        }

        if (!found) {
            // Fallback: try TimeZoneKeyName as-is with TzSpecificLocalTimeToSystemTime path inverted
            // Use local + note
            SYSTEMTIME local{};
            GetLocalTime(&local);
            return "Could not resolve timezone \"" + q + "\". Local time: "
                + FormatSystemTime(local, "local")
                + ". Try UTC, PST, CST, EST, CET, Germany, London, Tokyo, etc.";
        }

        TIME_ZONE_INFORMATION tzi{};
        if (!GetTimeZoneInformationForYear(utc.wYear, &dtzi, &tzi)) {
            // copy bias from dynamic
            tzi.Bias = dtzi.Bias;
            tzi.StandardBias = dtzi.StandardBias;
            tzi.DaylightBias = dtzi.DaylightBias;
            memcpy(tzi.StandardName, dtzi.StandardName, sizeof(tzi.StandardName));
            memcpy(tzi.DaylightName, dtzi.DaylightName, sizeof(tzi.DaylightName));
            tzi.StandardDate = dtzi.StandardDate;
            tzi.DaylightDate = dtzi.DaylightDate;
        }

        SYSTEMTIME localInTz{};
        if (!SystemTimeToTzSpecificLocalTime(&tzi, &utc, &localInTz)) {
            return "Timezone conversion failed for \"" + q + "\".";
        }

        char label[160]{};
        WideCharToMultiByte(CP_UTF8, 0, dtzi.TimeZoneKeyName, -1, label, 159, nullptr, nullptr);
        std::string out = "Current time in ";
        if (!q.empty()) out += q + " (";
        out += label;
        if (!q.empty()) out += ")";
        out += ": " + FormatSystemTime(localInTz, label);
        return out;
    }

    // =============================================================================
    // Reminders
    // =============================================================================
    struct Reminder {
        uint64_t id = 0;
        std::string userKey;
        std::string userDisplay;
        std::string message;
        long long fireAtMs = 0;
        bool delivered = false;
    };

    static std::mutex g_remMtx;
    static std::vector<Reminder> g_reminders;
    static std::atomic<uint64_t> g_nextRemId{ 1 };
    static constexpr size_t kMaxReminders = 24;
    static constexpr int kMaxSeconds = 7 * 24 * 3600; // 7 days

    std::string ToolSetReminder(const std::string& user, int inSeconds, const std::string& message) {
        if (user.empty()) return "Error: user name required.";
        if (message.empty()) return "Error: message required.";
        if (inSeconds < 5) return "Error: minimum delay is 5 seconds.";
        if (inSeconds > kMaxSeconds) return "Error: maximum delay is 7 days.";

        std::string display = TrimCopy(user);
        std::string key = ToLower(display);

        Reminder r;
        r.id = g_nextRemId.fetch_add(1);
        r.userKey = key;
        r.userDisplay = display;
        r.message = message;
        r.fireAtMs = NowSteadyMs() + (long long)inSeconds * 1000LL;

        {
            std::lock_guard lock(g_remMtx);
            // prune delivered
            g_reminders.erase(
                std::remove_if(g_reminders.begin(), g_reminders.end(),
                    [](const Reminder& x) { return x.delivered; }),
                g_reminders.end());
            if (g_reminders.size() >= kMaxReminders)
                return "Error: too many active reminders (max " + std::to_string(kMaxReminders) + "). Cancel some first.";
            g_reminders.push_back(r);
        }

        int mins = inSeconds / 60;
        int secs = inSeconds % 60;
        std::ostringstream ss;
        ss << "OK. Reminder #" << r.id << " for " << display << " in ";
        if (mins > 0) ss << mins << "m ";
        ss << secs << "s: \"" << message << "\"";
        return ss.str();
    }

    std::string FormatReminderList(const std::string& userFilter) {
        std::string filt = ToLower(TrimCopy(userFilter));
        long long now = NowSteadyMs();
        std::ostringstream ss;
        int n = 0;
        std::lock_guard lock(g_remMtx);
        for (const auto& r : g_reminders) {
            if (r.delivered) continue;
            if (!filt.empty() && r.userKey.find(filt) == std::string::npos) continue;
            long long left = (r.fireAtMs - now) / 1000;
            if (left < 0) left = 0;
            ss << "#" << r.id << " @" << r.userDisplay << " in " << left << "s: " << r.message << "\n";
            ++n;
        }
        if (n == 0) return "No active reminders.";
        return "Active reminders (" + std::to_string(n) + "):\n" + ss.str();
    }

    std::string ToolListReminders(const std::string& userFilter) {
        return FormatReminderList(userFilter);
    }

    bool CancelReminderById(uint64_t id) {
        std::lock_guard lock(g_remMtx);
        for (auto it = g_reminders.begin(); it != g_reminders.end(); ++it) {
            if (it->id == id && !it->delivered) {
                g_reminders.erase(it);
                return true;
            }
        }
        return false;
    }

    std::string ToolCancelReminder(uint64_t id) {
        if (CancelReminderById(id))
            return "Cancelled reminder #" + std::to_string(id) + ".";
        return "No active reminder with id " + std::to_string(id) + ".";
    }

    void TickReminders(void (*deliverCb)(const std::string& text)) {
        if (!deliverCb) return;
        long long now = NowSteadyMs();
        std::vector<std::string> due;
        {
            std::lock_guard lock(g_remMtx);
            for (auto& r : g_reminders) {
                if (r.delivered) continue;
                if (r.fireAtMs <= now) {
                    r.delivered = true;
                    std::string line = r.userDisplay + ": reminder — " + r.message;
                    due.push_back(std::move(line));
                }
            }
            // light prune
            if (g_reminders.size() > 8) {
                g_reminders.erase(
                    std::remove_if(g_reminders.begin(), g_reminders.end(),
                        [](const Reminder& x) { return x.delivered; }),
                    g_reminders.end());
            }
        }
        for (auto& t : due)
            deliverCb(t);
    }

    // =============================================================================
    // Math eval (safe: + - * / and parentheses only)
    // =============================================================================
    struct MathParser {
        const char* p;
        const char* end;
        std::string err;

        void skip() {
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
        }
        bool match(char c) {
            skip();
            if (p < end && *p == c) { ++p; return true; }
            return false;
        }

        double parseNumber() {
            skip();
            const char* start = p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            bool any = false;
            while (p < end && (isdigit((unsigned char)*p) || *p == '.')) { any = true; ++p; }
            if (!any) { err = "expected number"; return 0; }
            try { return std::stod(std::string(start, p)); }
            catch (...) { err = "bad number"; return 0; }
        }

        double parseFactor() {
            skip();
            if (match('(')) {
                double v = parseExpr();
                if (!match(')')) err = "missing )";
                return v;
            }
            if (match('+')) return parseFactor();
            if (match('-')) return -parseFactor();
            return parseNumber();
        }

        double parseTerm() {
            double v = parseFactor();
            for (;;) {
                if (match('*')) v *= parseFactor();
                else if (match('/')) {
                    double d = parseFactor();
                    if (d == 0.0) { err = "division by zero"; return 0; }
                    v /= d;
                }
                else break;
            }
            return v;
        }

        double parseExpr() {
            double v = parseTerm();
            for (;;) {
                if (match('+')) v += parseTerm();
                else if (match('-')) v -= parseTerm();
                else break;
            }
            return v;
        }
    };

    static bool EvalMath(const std::string& expr, double& out, std::string& errOut) {
        MathParser mp;
        mp.p = expr.c_str();
        mp.end = mp.p + expr.size();
        mp.err.clear();
        out = mp.parseExpr();
        mp.skip();
        if (mp.p != mp.end && mp.err.empty()) mp.err = "trailing junk";
        if (!mp.err.empty()) { errOut = mp.err; return false; }
        errOut.clear();
        return true;
    }

    static std::string FormatNumber(double v) {
        if (!std::isfinite(v)) return "NaN";
        double r = std::round(v);
        if (std::fabs(v - r) < 1e-9 && std::fabs(v) < 1e15) {
            long long iv = (long long)r;
            return std::to_string(iv);
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%.10g", v);
        return buf;
    }

    std::string ExpandEchoPlaceholders(const std::string& input) {
        std::string out;
        out.reserve(input.size() + 16);
        size_t i = 0;
        while (i < input.size()) {
            if (input[i] == '$' && i + 1 < input.size() && input[i + 1] == '{') {
                size_t close = input.find('}', i + 2);
                if (close == std::string::npos) {
                    out += input[i++];
                    continue;
                }
                std::string inner = input.substr(i + 2, close - (i + 2));
                std::string lower = ToLower(TrimCopy(inner));
                std::string repl;

                if (lower.rfind("math(", 0) == 0 && !inner.empty() && inner.back() == ')') {
                    // preserve original expr between math( and )
                    size_t lp = inner.find('(');
                    std::string expr = inner.substr(lp + 1);
                    if (!expr.empty() && expr.back() == ')') expr.pop_back();
                    double val = 0;
                    std::string err;
                    if (EvalMath(expr, val, err))
                        repl = FormatNumber(val);
                    else
                        repl = "${math error: " + err + "}";
                }
                else if (lower == "time" || lower == "time:local") {
                    repl = ToolGetTime("");
                    // shorten: pull just the clock part if long
                }
                else if (lower.rfind("time:", 0) == 0) {
                    repl = ToolGetTime(inner.substr(5));
                }
                else if (lower == "uptime") {
                    repl = FormatUptime();
                }
                else if (lower.rfind("random:", 0) == 0) {
                    std::string range = inner.substr(7);
                    int a = 0, b = 100;
                    size_t dash = range.find('-');
                    try {
                        if (dash == std::string::npos) {
                            b = std::stoi(range);
                            if (b < 1) b = 1;
                            a = 1;
                        }
                        else {
                            a = std::stoi(range.substr(0, dash));
                            b = std::stoi(range.substr(dash + 1));
                        }
                    }
                    catch (...) { a = 0; b = 100; }
                    if (a > b) std::swap(a, b);
                    static thread_local std::mt19937 rng{ std::random_device{}() };
                    std::uniform_int_distribution<int> dist(a, b);
                    repl = std::to_string(dist(rng));
                }
                else {
                    repl = "${" + inner + "}"; // unknown — leave-ish
                }

                out += repl;
                i = close + 1;
            }
            else {
                out += input[i++];
            }
        }
        return out;
    }

    // =============================================================================
// Private chat (AES-256-GCM + PBKDF2 via BCrypt)
// =============================================================================
    static std::mutex g_privMtx;
    static std::vector<uint8_t> g_privKey; // 32 bytes
    static constexpr char kPrivPrefix[] = "~jb1.";

    static std::string Base64UrlEncode(const uint8_t* data, size_t len) {
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((len + 2) / 3 * 4);
        for (size_t i = 0; i < len; i += 3) {
            unsigned n = data[i] << 16;
            if (i + 1 < len) n |= data[i + 1] << 8;
            if (i + 2 < len) n |= data[i + 2];
            out.push_back(tbl[(n >> 18) & 63]);
            out.push_back(tbl[(n >> 12) & 63]);
            out.push_back((i + 1 < len) ? tbl[(n >> 6) & 63] : '=');
            out.push_back((i + 2 < len) ? tbl[n & 63] : '=');
        }
        // url-safe
        for (char& c : out) {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }
        while (!out.empty() && out.back() == '=') out.pop_back();
        return out;
    }

    static bool Base64UrlDecode(const std::string& in, std::vector<uint8_t>& out) {
        std::string s = in;
        for (char& c : s) {
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }
        while (s.size() % 4) s.push_back('=');
        auto val = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
            };
        out.clear();
        for (size_t i = 0; i + 3 < s.size(); i += 4) {
            int a = val(s[i]), b = val(s[i + 1]), c = val(s[i + 2]), d = val(s[i + 3]);
            if (a < 0 || b < 0) return false;
            out.push_back((uint8_t)((a << 2) | (b >> 4)));
            if (s[i + 2] != '=') {
                if (c < 0) return false;
                out.push_back((uint8_t)(((b & 15) << 4) | (c >> 2)));
            }
            if (s[i + 3] != '=') {
                if (d < 0) return false;
                out.push_back((uint8_t)(((c & 3) << 6) | d));
            }
        }
        return true;
    }

    void SetPrivatePassphrase(const std::string& passphrase) {
        std::lock_guard lock(g_privMtx);
        g_privKey.clear();
        // Always trim so "password" and "password " produce the same key
        std::string pass = TrimCopy(passphrase);
        if (pass.empty()) return;

        BCRYPT_ALG_HANDLE hAlg = nullptr;
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
            return;

        // PBKDF2-HMAC-SHA256, 100k iterations, fixed salt "josebot-priv-v1"
        const uint8_t salt[] = "josebot-priv-v1";
        g_privKey.resize(32);
        NTSTATUS st = BCryptDeriveKeyPBKDF2(hAlg,
            (PUCHAR)pass.data(), (ULONG)pass.size(),
            (PUCHAR)salt, sizeof(salt) - 1,
            100000,
            g_privKey.data(), (ULONG)g_privKey.size(),
            0);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        if (!NT_SUCCESS(st))
            g_privKey.clear();
    }

    bool HasPrivateKey() {
        std::lock_guard lock(g_privMtx);
        return g_privKey.size() == 32;
    }

    void ClearPrivateKey() {
        std::lock_guard lock(g_privMtx);
        if (!g_privKey.empty()) {
            SecureZeroMemory(g_privKey.data(), g_privKey.size());
            g_privKey.clear();
        }
    }

    // Short non-reversible fingerprint so two clients can compare keys without
    // revealing the passphrase or the raw key bytes.
    std::string PrivateKeyFingerprint() {
        std::lock_guard lock(g_privMtx);
        if (g_privKey.size() != 32) return "none";
        uint32_t h = 0x811c9dc5u;
        for (uint8_t b : g_privKey) {
            h ^= b;
            h *= 0x01000193u;
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%08x", h);
        return buf;
    }

    bool IsPrivateCiphertext(const std::string& body) {
        std::string t = TrimCopy(body);
        return t.size() > 6 && t.compare(0, 5, kPrivPrefix) == 0;
    }

    std::string EncryptPrivateMessage(const std::string& plaintext) {
        std::vector<uint8_t> key;
        {
            std::lock_guard lock(g_privMtx);
            if (g_privKey.size() != 32) return {};
            key = g_privKey;
        }
        if (plaintext.empty()) return {};

        BCRYPT_ALG_HANDLE hAlg = nullptr;
        BCRYPT_KEY_HANDLE hKey = nullptr;

        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
            return {};
        if (!NT_SUCCESS(BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
            (PUCHAR)BCRYPT_CHAIN_MODE_GCM, (ULONG)sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return {};
        }
        if (!NT_SUCCESS(BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, key.data(), 32, 0))) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return {};
        }

        uint8_t nonce[12];
        if (!NT_SUCCESS(BCryptGenRandom(nullptr, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            BCryptDestroyKey(hKey);
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return {};
        }

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = nonce;
        authInfo.cbNonce = sizeof(nonce);
        uint8_t tag[16]{};
        authInfo.pbTag = tag;
        authInfo.cbTag = sizeof(tag);

        ULONG cbResult = 0;
        std::vector<uint8_t> cipher(plaintext.size());
        NTSTATUS st = BCryptEncrypt(hKey,
            (PUCHAR)plaintext.data(), (ULONG)plaintext.size(),
            &authInfo,
            nullptr, 0,
            cipher.data(), (ULONG)cipher.size(),
            &cbResult, 0);

        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        if (!NT_SUCCESS(st)) return {};

        cipher.resize(cbResult);
        // pack: nonce(12) || ciphertext || tag(16)
        std::vector<uint8_t> packed;
        packed.reserve(12 + cipher.size() + 16);
        packed.insert(packed.end(), nonce, nonce + 12);
        packed.insert(packed.end(), cipher.begin(), cipher.end());
        packed.insert(packed.end(), tag, tag + 16);

        return std::string(kPrivPrefix) + Base64UrlEncode(packed.data(), packed.size());
    }

    std::string DecryptPrivateMessage(const std::string& wire) {
        std::string t = TrimCopy(wire);
        if (!IsPrivateCiphertext(t)) return {};
        std::string b64 = t.substr(5);
        std::vector<uint8_t> packed;
        if (!Base64UrlDecode(b64, packed) || packed.size() < 12 + 16) return {};

        std::vector<uint8_t> key;
        {
            std::lock_guard lock(g_privMtx);
            if (g_privKey.size() != 32) return {};
            key = g_privKey;
        }

        uint8_t nonce[12];
        memcpy(nonce, packed.data(), 12);
        uint8_t tag[16];
        memcpy(tag, packed.data() + packed.size() - 16, 16);
        size_t ctLen = packed.size() - 12 - 16;
        std::vector<uint8_t> cipher(packed.begin() + 12, packed.begin() + 12 + ctLen);

        BCRYPT_ALG_HANDLE hAlg = nullptr;
        BCRYPT_KEY_HANDLE hKey = nullptr;
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
            return {};
        if (!NT_SUCCESS(BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
            (PUCHAR)BCRYPT_CHAIN_MODE_GCM, (ULONG)sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return {};
        }
        if (!NT_SUCCESS(BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, key.data(), 32, 0))) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return {};
        }

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = nonce;
        authInfo.cbNonce = 12;
        authInfo.pbTag = tag;
        authInfo.cbTag = 16;

        std::vector<uint8_t> plain(ctLen);
        ULONG cbResult = 0;
        NTSTATUS st = BCryptDecrypt(hKey,
            cipher.data(), (ULONG)cipher.size(),
            &authInfo,
            nullptr, 0,
            plain.data(), (ULONG)plain.size(),
            &cbResult, 0);

        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        if (!NT_SUCCESS(st)) return {};
        return std::string((char*)plain.data(), cbResult);
    }

    // =============================================================================
    // AI context dump
    // =============================================================================
    static std::mutex g_lastPromptMtx;
    static std::string g_lastComposed;

    void SetLastComposedPrompt(const std::string& composed) {
        std::lock_guard lock(g_lastPromptMtx);
        g_lastComposed = composed;
    }

    std::string GetLastComposedPrompt() {
        std::lock_guard lock(g_lastPromptMtx);
        return g_lastComposed;
    }

    std::string BuildAiContextDump() {
        std::ostringstream ss;
        ss << "=== AI CONTEXT DUMP ===\n\n";

        // System prompt (global free function from settings.hpp / legacy_core.cpp)
        std::string sys = ::GetCachedSystemPrompt();
        ss << "--- system prompt (" << sys.size() << " chars) ---\n";
        if (sys.size() > 3000)
            ss << sys.substr(0, 3000) << "\n... [truncated]\n";
        else
            ss << sys << "\n";

        int maxMem = 4, maxChat = 20;
        bool includeGlobal = true;
        {
            std::lock_guard lock(g_settingsMtx);
            maxMem = g_maxMemory;
            maxChat = g_maxChatContext;
            includeGlobal = g_includeGlobalChat;
        }

        ss << "\n--- memory turns (max " << maxMem << " pairs) ---\n";
        {
            std::lock_guard lock(g_mtx);
            for (const auto& m : g_memory)
                ss << "[" << m.role << "] " << m.content << "\n";
        }

        ss << "\n--- global chat context (include=" << (includeGlobal ? "yes" : "no")
            << ", max lines=" << maxChat << ") ---\n";
        if (includeGlobal) {
            std::lock_guard lock(g_mtx);
            const int n = (int)g_globalChat.size();
            const int startIdx = (n > maxChat) ? (n - maxChat) : 0;
            for (int i = startIdx; i < n; ++i) {
                const auto& e = g_globalChat[(size_t)i];
                if (!e.speaker.empty()) ss << e.speaker << ": " << e.body << "\n";
                else ss << e.plain << "\n";
            }
        }

        std::string last = GetLastComposedPrompt();
        ss << "\n--- last composed user prompt (" << last.size() << " chars) ---\n";
        if (!last.empty())
            ss << last << "\n";
        else
            ss << "(none yet — trigger the bot once first)\n";

        ss << "\n=== END DUMP ===\n";
        return ss.str();
    }


    // =============================================================================
    // Web search — LangSearch (https://api.langsearch.com/v1/web-search)
    // Token-efficient: few results, snippets by default, hard char budget.
    // =============================================================================
    static std::string TruncateAtWord(const std::string& s, size_t maxLen) {
        if (s.size() <= maxLen) return s;
        size_t cut = s.rfind(' ', maxLen);
        if (cut == std::string::npos || cut < maxLen / 2) cut = maxLen;
        return s.substr(0, cut) + "...";
    }

    static bool HttpPostJsonSimple(const std::wstring& host, INTERNET_PORT port, bool https,
        const std::wstring& path, const std::wstring& extraHeaders,
        const std::string& payload, std::string& bodyOut, DWORD& statusCode)
    {
        bodyOut.clear();
        statusCode = 0;
        HINTERNET hSession = WinHttpOpen(L"JosebotWebSearch/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
        if (!hSession) return false;
        WinHttpSetTimeouts(hSession, 15000, 15000, 20000, 45000);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

        DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hReq) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        std::wstring headers = L"Content-Type: application/json\r\n";
        headers += extraHeaders;
        bool ok = false;
        if (WinHttpSendRequest(hReq, headers.c_str(), (DWORD)-1,
            (LPVOID)payload.data(), (DWORD)payload.size(), (DWORD)payload.size(), 0)
            && WinHttpReceiveResponse(hReq, nullptr)) {
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
            char buffer[4096];
            DWORD bytesRead = 0;
            while (WinHttpReadData(hReq, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
                bodyOut.append(buffer, bytesRead);
                if (bodyOut.size() > 512 * 1024) break;
            }
            ok = (statusCode == 200);
        }
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return ok;
    }

    static bool ExtractJsonArrayAfterKey(const std::string& json, const char* key, std::string& arrOut) {
        arrOut.clear();
        std::string pat = std::string("\"") + key + "\"";
        size_t pos = json.find(pat);
        if (pos == std::string::npos) return false;
        pos = json.find('[', pos + pat.size());
        if (pos == std::string::npos) return false;
        int depth = 0;
        bool inStr = false, esc = false;
        for (size_t i = pos; i < json.size(); ++i) {
            char c = json[i];
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == '[') ++depth;
            else if (c == ']') {
                --depth;
                if (depth == 0) {
                    arrOut = json.substr(pos, i - pos + 1);
                    return true;
                }
            }
        }
        return false;
    }

    static bool ExtractBalancedObj(const std::string& s, size_t openIdx, std::string& out) {
        out.clear();
        if (openIdx >= s.size() || s[openIdx] != '{') return false;
        int depth = 0;
        bool inStr = false, esc = false;
        for (size_t i = openIdx; i < s.size(); ++i) {
            char c = s[i];
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) {
                    out = s.substr(openIdx, i - openIdx + 1);
                    return true;
                }
            }
        }
        return false;
    }

    std::string ToolWebSearch(const std::string& query, const std::string& freshnessOverride) {
        std::string q = TrimCopy(query);
        if (q.empty()) return "Error: empty search query.";

        bool enabled = false;
        std::string apiKey;
        int count = 4;
        bool wantSummary = false;
        std::string freshness = "noLimit";
        int maxChars = 1600;
        {
            std::lock_guard lock(g_settingsMtx);
            enabled = g_webSearchEnabled;
            apiKey = g_webSearchApiKey;
            count = g_webSearchCount;
            wantSummary = g_webSearchSummary;
            freshness = g_webSearchFreshness.empty() ? "noLimit" : g_webSearchFreshness;
            maxChars = g_webSearchMaxChars;
        }
        if (!enabled)
            return "Error: web search is disabled in settings.";
        if (apiKey.empty())
            return "Error: LangSearch API key not set.";

        if (!freshnessOverride.empty())
            freshness = TrimCopy(freshnessOverride);

        if (count < 1) count = 1;
        if (count > 10) count = 10;
        if (maxChars < 400) maxChars = 400;
        if (maxChars > 8000) maxChars = 8000;

        {
            std::string fl = ToLower(freshness);
            if (fl == "oneday") freshness = "oneDay";
            else if (fl == "oneweek") freshness = "oneWeek";
            else if (fl == "onemonth") freshness = "oneMonth";
            else if (fl == "oneyear") freshness = "oneYear";
            else freshness = "noLimit";
        }

        std::ostringstream body;
        body << "{\"query\":\"" << JsonEscape(q) << "\""
            << ",\"freshness\":\"" << JsonEscape(freshness) << "\""
            << ",\"summary\":" << (wantSummary ? "true" : "false")
            << ",\"count\":" << count
            << "}";

        std::string response;
        DWORD status = 0;
        std::wstring auth = L"Authorization: Bearer " + Utf8ToWide(apiKey) + L"\r\n";
        bool httpOk = HttpPostJsonSimple(
            L"api.langsearch.com", 443, true,
            L"/v1/web-search", auth,
            body.str(), response, status);

        if (!httpOk) {
            LogToFile("web_search HTTP failed status=" + std::to_string(status)
                + " body=" + response.substr(0, 200));
            if (status == 401 || status == 403)
                return "Error: LangSearch auth failed (check API key).";
            if (status == 429)
                return "Error: LangSearch rate limited; try again later.";
            return "Error: web search request failed (HTTP " + std::to_string(status) + ").";
        }

        std::string valueArr;
        size_t wp = response.find("\"webPages\"");
        if (wp != std::string::npos) {
            std::string fromWp = response.substr(wp);
            if (!ExtractJsonArrayAfterKey(fromWp, "value", valueArr))
                ExtractJsonArrayAfterKey(response, "value", valueArr);
        }
        else {
            ExtractJsonArrayAfterKey(response, "value", valueArr);
        }

        if (valueArr.size() < 3) {
            std::string msg;
            ExtractJsonStringField(response, "msg", msg);
            if (!msg.empty()) return "Error: " + msg;
            return "No web results for: " + q;
        }

        std::ostringstream out;
        out << "Web results for \"" << q << "\":\n";
        int n = 0;
        size_t i = 1;
        const size_t perSnippet = wantSummary ? 420u : 220u;

        while (i < valueArr.size() && n < count) {
            while (i < valueArr.size() && (valueArr[i] == ' ' || valueArr[i] == '\n'
                || valueArr[i] == '\r' || valueArr[i] == '\t' || valueArr[i] == ','))
                ++i;
            if (i >= valueArr.size() || valueArr[i] == ']') break;
            if (valueArr[i] != '{') { ++i; continue; }

            std::string obj;
            if (!ExtractBalancedObj(valueArr, i, obj)) break;
            i += obj.size();

            std::string name, url, snippet, summary;
            ExtractJsonStringField(obj, "name", name);
            ExtractJsonStringField(obj, "url", url);
            ExtractJsonStringField(obj, "snippet", snippet);
            if (wantSummary)
                ExtractJsonStringField(obj, "summary", summary);

            std::string text = wantSummary && !summary.empty() ? summary : snippet;
            {
                std::string clean;
                clean.reserve(text.size());
                bool sp = false;
                for (unsigned char c : text) {
                    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                    if (c == ' ') {
                        if (!sp) clean += ' ';
                        sp = true;
                    }
                    else {
                        clean += (char)c;
                        sp = false;
                    }
                }
                text = TrimCopy(clean);
            }
            text = TruncateAtWord(text, perSnippet);

            if (name.empty() && text.empty()) continue;
            ++n;
            out << n << ". " << (name.empty() ? "(untitled)" : TruncateAtWord(name, 100));
            if (!url.empty())
                out << " | " << TruncateAtWord(url, 120);
            out << "\n";
            if (!text.empty())
                out << "   " << text << "\n";
        }

        if (n == 0)
            return "No web results for: " + q;

        std::string result = out.str();
        if ((int)result.size() > maxChars)
            result = TruncateAtWord(result, (size_t)maxChars) + "\n[truncated]";
        LogToFile("web_search ok q=\"" + q + "\" results=" + std::to_string(n)
            + " chars=" + std::to_string(result.size()));
        return result;
    }


} // namespace Extras


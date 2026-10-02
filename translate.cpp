#include "pch.h"
#include "translate.hpp"
#include "common.hpp"
#include "chat.hpp"
#include "settings.hpp"
#include "shared.hpp"

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif

// TX state
std::mutex g_txMtx;
static std::deque<TranslateJob> g_txQueue;
static std::map<std::string, TranslateResult> g_txCache;
static std::deque<std::string> g_txCacheOrder;
static std::atomic<bool> g_txWorkerRunning{ false };
std::atomic<bool> g_txInProgress{ false };
std::atomic<uint32_t> g_txCalls{ 0 };
std::atomic<uint32_t> g_txCacheHits{ 0 };
std::atomic<uint32_t> g_txSkipped{ 0 };
static const size_t TX_CACHE_MAX = 220;
static const size_t TX_QUEUE_MAX = 16;

// Forward from chat_core — ApplyIncomingResult needs g_log
void ApplyIncomingResult(uint64_t lineId, const TranslateResult& r, const std::string& tl);

// =============================================================================
static bool HasLatinAccent(const std::string& s) {
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        unsigned char d = (unsigned char)s[i + 1];
        if (c == 0xC3 && d >= 0x80) return true;
        if (c == 0xC4 || c == 0xC5) return true;
        if (c == 0xD0 || c == 0xD1) return true;
    }
    return false;
}

static int CountAsciiLetters(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if (isalpha(c)) ++n;
    return n;
}

static std::string StripOuterPunct(std::string s) {
    s = TrimCopy(s);
    while (!s.empty() && ispunct((unsigned char)s.front()) && (unsigned char)s.front() < 0x80)
        s.erase(0, 1);
    while (!s.empty() && ispunct((unsigned char)s.back()) && (unsigned char)s.back() < 0x80)
        s.pop_back();
    return TrimCopy(s);
}

static bool IsCommonChatSlang(const std::string& text) {
    std::string t = ToLower(StripOuterPunct(text));
    if (t.empty()) return true;
    static const char* kExact[] = {
        "gg","wp","gl","hf","glhf","ggwp","lol","lmao","rofl","omg","brb","afk",
        "nvm","ty","thx","np","ok","k","kk","yes","no","hi","hey","yo","yep","yeah",
        "nah","rn","tbh","idk","imo","imho","nty","wtf","ez","nt","ns","gn","gm",
        "gtg","ofc","pvp","pve","xp","hp","okey","okay","cool","nice","true","fake",
        "wait","what","why","how","who","wow","good","bad","sure","stop","go","run",
        "help","sell","buy","trade","party","raid","boss","loot","mine","craft",
        "farm","skyblock","histatu","hyai","hello","thanks","sorry","please",
        "wsp","wyd","hbu","wbu","omw","gnight","wb","welcome","tyty",
        "xd","xdd","haha","hahaha","bro","dude","man","sir","fam","any1","u","ur",
        "r","n","y","m","cya","bye","bb","tyvm","tysm","npnp","ggs","g",
        nullptr
    };
    for (int i = 0; kExact[i]; ++i)
        if (t == kExact[i]) return true;

    static const char* kWord[] = {
        "gg","wp","gl","hf","lol","ok","yes","no","hi","hey","yo","u","ur","the","a",
        "i","you","me","my","we","it","is","to","and","for","in","on","at","of",
        "just","like","get","got","go","can","will","be","do","not","but","so",
        "this","that","what","how","why","who","yes","no","ok","okay","ty","thx",
        "np","bro","man","pls","plz","please","help","need","want","have","has",
        "here","there","now","then","too","also","one","two","up","down","out",
        "off","back","come","coming","going","gonna","wanna","im","ive","dont",
        "cant","wont","thats","its","ill","lets","let","us","them","they","he",
        "she","his","her","our","your","im","rn","tbh","idk","lol","lmao","ez",
        "nice","good","bad","cool","true","wait","sorry","thanks","thank","you",
        nullptr
    };
    std::vector<std::string> words;
    std::string cur;
    for (unsigned char c : t) {
        if (isalnum(c)) cur += (char)c;
        else if (!cur.empty()) { words.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) words.push_back(cur);
    if (words.empty()) return true;
    if (words.size() > 8) return false;
    for (const auto& w : words) {
        bool found = false;
        for (int i = 0; kWord[i]; ++i) if (w == kWord[i]) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

static bool LooksLikeEnglish(const std::string& text) {
    std::string l = ToLower(text);
    static const char* kEn[] = {
        "the","you","your","that","this","have","with","they","what","how",
        "will","just","like","not","but","please","thanks","would","could",
        "should","about","from","there","here","want","going","know","think",
        "because","really","something","anyone","someone","where","when",
        "which","been","were","we're","you're","don't","can't","won't",
        nullptr
    };
    int hits = 0;
    for (int i = 0; kEn[i]; ++i)
        if (HasWholeWord(l, kEn[i])) ++hits;
    return hits >= 2;
}

static int CountLangHits(const std::string& l, const char* const* words) {
    int hits = 0;
    int longHits = 0;
    for (int i = 0; words[i]; ++i) {
        if (!HasWholeWord(l, words[i])) continue;
        ++hits;
        if (strlen(words[i]) > 3) ++longHits;
    }
    if (longHits > 0) return hits + longHits;
    return hits;
}

static bool LooksLikeForeign(const std::string& text) {
    if (HasLatinAccent(text)) return true;
    std::string l = ToLower(text);
    static const char* kFr[] = {
        "bonjour","salut","merci","comment","c'est","j'ai","oui","non",
        "suis","sont","avons","fait","faire","mais","bien","très",
        "aussi","alors","parce","cette","tout","tous","rien","jamais",
        "peut","veux","voir","quand","quoi","avec","dans","pour",
        nullptr
    };
    static const char* kDe[] = {
        "nicht","und","eine","danke","bitte","dass","haben","sind",
        "auch","oder","mein","mich","dich","sich","kann","will",
        "schon","noch","aber","wenn","hier","warum","heute","morgen",
        nullptr
    };
    static const char* kEs[] = {
        "hola","gracias","porque","esto","esta","está","están",
        "también","pero","ahora","donde","quién","bueno","nada",
        "vamos","quiero","tiene","hacer","para",
        nullptr
    };
    const int hits = CountLangHits(l, kFr) + CountLangHits(l, kDe) + CountLangHits(l, kEs);
    return hits >= 2;
}

bool ShouldAutoTranslate(const std::string& text) {
    std::string t = TrimCopy(text);
    if (t.size() < 4) return false;
    if (CountAsciiLetters(t) < 3 && !HasLatinAccent(t)) return false;
    if (IsCommonChatSlang(t)) return false;
    // The heuristics below only know how to spot "this is already English". For
    // any other target language, let the translator decide; a line that is
    // already in the target comes back marked Skipped and shows nothing.
    {
        std::string target;
        {
            std::lock_guard slock(g_settingsMtx);
            target = ToLower(TrimCopy(g_prefs.txTarget));
        }
        if (!target.empty() && target != "en") return true;
    }
    if (LooksLikeForeign(t)) return true;
    if (LooksLikeEnglish(t)) return false;
    if (t.size() >= 48 && CountAsciiLetters(t) >= 24) return true;
    return false;
}

// =============================================================================
// Google Translate
// =============================================================================
static std::string UrlEncode(const std::string& s) {
    std::string o;
    o.reserve(s.size() * 3);
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            o += (char)c;
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 0xF];
        }
    }
    return o;
}

static bool ExtractJsonStringAt(const std::string& json, size_t& i, std::string& out) {
    out.clear();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t' || json[i] == ','))
        ++i;
    if (i >= json.size() || json[i] != '"') return false;
    ++i;
    std::string raw;
    bool esc = false;
    for (; i < json.size(); ++i) {
        char c = json[i];
        if (esc) { raw += c; esc = false; continue; }
        if (c == '\\') { raw += c; esc = true; continue; }
        if (c == '"') { ++i; break; }
        raw += c;
    }
    JsonUnescape(raw, out);
    return true;
}

static bool ParseGoogleTranslateResponse(const std::string& json, TranslateResult& out) {
    out = {};
    if (json.size() < 8 || json[0] != '[') return false;

    size_t i = 0;
    while (i + 1 < json.size() && !(json[i] == '[' && json[i + 1] == '[')) ++i;
    if (i + 1 >= json.size()) return false;
    i += 2;

    std::string translated;
    while (i < json.size()) {
        while (i < json.size() && json[i] != '[' && json[i] != ']') ++i;
        if (i >= json.size()) break;
        if (json[i] == ']') { ++i; break; }
        ++i;
        std::string piece;
        if (!ExtractJsonStringAt(json, i, piece)) break;
        translated += piece;
        int depth = 1;
        bool inStr = false, esc = false;
        for (; i < json.size() && depth > 0; ++i) {
            char c = json[i];
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') inStr = true;
            else if (c == '[') ++depth;
            else if (c == ']') --depth;
        }
    }

    while (i < json.size() && (json[i] == ' ' || json[i] == ',' || json[i] == 'n' || json[i] == 'u' || json[i] == 'l')) {
        if (json[i] == '"') break;
        ++i;
    }
    std::string detected;
    ExtractJsonStringAt(json, i, detected);
    if (detected.empty()) {
        size_t p = json.find("],null,\"");
        if (p != std::string::npos) {
            p += 8;
            size_t e = p;
            while (e < json.size() && json[e] != '"') ++e;
            detected = json.substr(p, e - p);
        }
    }

    if (translated.empty()) return false;
    out.ok = true;
    out.translated = translated;
    out.detected = ToLower(detected);
    return true;
}

static std::string TxCacheKey(const std::string& sl, const std::string& tl, const std::string& text) {
    return sl + "|" + tl + "|" + text;
}

static bool TxCacheGet(const std::string& key, TranslateResult& out) {
    std::lock_guard lock(g_txMtx);
    auto it = g_txCache.find(key);
    if (it == g_txCache.end()) return false;
    out = it->second;
    return true;
}

static void TxCachePut(const std::string& key, const TranslateResult& val) {
    std::lock_guard lock(g_txMtx);
    if (g_txCache.find(key) == g_txCache.end()) {
        g_txCacheOrder.push_back(key);
        while (g_txCacheOrder.size() > TX_CACHE_MAX) {
            g_txCache.erase(g_txCacheOrder.front());
            g_txCacheOrder.pop_front();
        }
    }
    g_txCache[key] = val;
}

static TranslateResult TranslateHttp(const std::string& sl, const std::string& tl, const std::string& text) {
    TranslateResult result;
    if (text.empty() || tl.empty() || g_shuttingDown.load()) return result;

    std::string q = text;
    if (q.size() > 400) q.resize(400);

    const std::string key = TxCacheKey(sl, tl, q);
    if (TxCacheGet(key, result)) {
        g_txCacheHits.fetch_add(1);
        return result;
    }

    std::string pathA = "/translate_a/single?client=gtx&sl=" + sl + "&tl=" + tl +
        "&hl=en&dt=t&ie=UTF-8&oe=UTF-8&q=" + UrlEncode(q);
    if (pathA.size() > 1800) {
        q.resize(q.size() / 2);
        pathA = "/translate_a/single?client=gtx&sl=" + sl + "&tl=" + tl +
            "&hl=en&dt=t&ie=UTF-8&oe=UTF-8&q=" + UrlEncode(q);
    }

    int n = MultiByteToWideChar(CP_UTF8, 0, pathA.c_str(), (int)pathA.size(), nullptr, 0);
    std::wstring pathW;
    if (n > 0) {
        pathW.assign(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, pathA.c_str(), (int)pathA.size(), &pathW[0], n);
    }
    else {
        pathW.assign(pathA.begin(), pathA.end());
    }

    static const wchar_t* kChromeUA =
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        L"(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";

    HINTERNET hSession = WinHttpOpen(
        kChromeUA,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) return result;
    g_txSession.store(hSession);

    DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
    WinHttpSetTimeouts(hSession, 5000, 5000, 8000, 12000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"translate.googleapis.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        CloseHttp(g_txSession);
        return result;
    }
    g_txConnect.store(hConnect);

    HINTERNET hReq = WinHttpOpenRequest(
        hConnect, L"GET", pathW.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (hReq) {
        g_txRequest.store(hReq);

        DWORD redir = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(hReq, WINHTTP_OPTION_DISABLE_FEATURE, &redir, sizeof(redir));

        const wchar_t* extraHeaders =
            L"Accept: application/json, text/plain, */*\r\n"
            L"Accept-Language: en-US,en;q=0.9\r\n"
            L"Referer: https://translate.google.com/\r\n"
            L"Origin: https://translate.google.com\r\n"
            L"Sec-Fetch-Dest: empty\r\n"
            L"Sec-Fetch-Mode: cors\r\n"
            L"Sec-Fetch-Site: cross-site\r\n"
            L"Cache-Control: no-cache\r\n";

        g_txCalls.fetch_add(1);
        if (!g_shuttingDown.load() &&
            WinHttpSendRequest(hReq, extraHeaders, (DWORD)-1,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hReq, nullptr)) {

            DWORD statusCode = 0, statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hReq,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                WINHTTP_NO_HEADER_INDEX);

            std::string body;
            char buffer[4096];
            DWORD bytesRead = 0;
            while (!g_shuttingDown.load() &&
                WinHttpReadData(hReq, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0)
                body.append(buffer, bytesRead);

            if (statusCode == 200) {
                ParseGoogleTranslateResponse(body, result);
                if (!result.ok) {
                    std::string trans, src;
                    if (ExtractJsonStringField(body, "trans", trans) && !trans.empty()) {
                        result.ok = true;
                        result.translated = trans;
                        if (ExtractJsonStringField(body, "src", src))
                            result.detected = ToLower(src);
                    }
                }
            }
            else if (statusCode == 429) {
                LogToFile("Translate HTTP 429 (rate limited). Body length=" +
                    std::to_string(body.size()) + " — back off and retry later.");
                Sleep(2500);
            }
            else {
                std::string snippet = body.size() > 300 ? body.substr(0, 300) + "..." : body;
                LogToFile("Translate HTTP " + std::to_string(statusCode) + " body: " + snippet);
            }
        }
        CloseHttp(g_txRequest);
    }
    CloseHttp(g_txConnect);
    CloseHttp(g_txSession);

    if (result.ok)
        TxCachePut(key, result);
    return result;
}

static bool SameText(const std::string& a, const std::string& b) {
    return ToLower(TrimCopy(a)) == ToLower(TrimCopy(b));
}

void ApplyIncomingResult(uint64_t lineId, const TranslateResult& r, const std::string& tl) {
    std::lock_guard lock(g_mtx);
    for (auto& line : g_log) {
        if (line.id != lineId) continue;
        TouchChatLine(line);
        if (!r.ok) {
            line.txStatus = TxStatus::Failed;
            return;
        }
        const std::string det = r.detected.empty() ? "auto" : r.detected;
        if (det == ToLower(tl) || SameText(r.translated, line.body.empty() ? line.plain : line.body)) {
            line.txStatus = TxStatus::Skipped;
            line.txFrom = det;
            line.txTo = tl;
            g_txSkipped.fetch_add(1);
            return;
        }
        line.txText = r.translated;
        line.txFrom = det;
        line.txTo = tl;
        line.txStatus = TxStatus::Done;
        return;
    }
}

// A job dropped from a full queue would otherwise leave its line showing
// "translating..." forever. Put those lines back to untranslated instead, which
// brings their "tr" button back.
static void ResetDroppedLines(const std::vector<uint64_t>& ids) {
    if (ids.empty()) return;
    std::lock_guard lock(g_mtx);
    for (auto& line : g_log) {
        if (line.txStatus != TxStatus::Pending) continue;
        if (std::find(ids.begin(), ids.end(), line.id) == ids.end()) continue;
        line.txStatus = TxStatus::None;
        TouchChatLine(line);
    }
}

static void TxWorkerLoop();

void EnsureTxWorker() {
    if (g_shuttingDown.load()) return;
    bool expected = false;
    if (!g_txWorkerRunning.compare_exchange_strong(expected, true))
        return;
    StartWorker(&TxWorkerLoop);
}

static void EnqueueTranslate(TranslateJob job) {
    if (g_shuttingDown.load()) return;
    if (job.text.empty()) return;
    job.text = TrimCopy(job.text);
    if (job.text.empty()) return;

    std::vector<uint64_t> dropped;
    {
        std::lock_guard lock(g_txMtx);
        if (job.lineId) {
            for (auto& j : g_txQueue)
                if (j.lineId == job.lineId) return;
        }
        while (g_txQueue.size() >= TX_QUEUE_MAX) {
            if (g_txQueue.back().lineId) dropped.push_back(g_txQueue.back().lineId);
            g_txQueue.pop_back();
        }
        if (job.priority)
            g_txQueue.push_front(std::move(job));
        else
            g_txQueue.push_back(std::move(job));
    }
    ResetDroppedLines(dropped);
    EnsureTxWorker();
}

static void TxWorkerLoop() {
    for (;;) {
        if (g_shuttingDown.load()) {
            g_txWorkerRunning = false;
            g_txInProgress = false;
            return;
        }
        TranslateJob job;
        {
            std::lock_guard lock(g_txMtx);
            if (g_txQueue.empty()) {
                g_txWorkerRunning = false;
                g_txInProgress = false;
                return;
            }
            job = std::move(g_txQueue.front());
            g_txQueue.pop_front();
        }
        g_txInProgress = true;

        TranslateResult r = TranslateHttp(job.sl, job.tl, job.text);

        if (g_shuttingDown.load()) {
            g_txWorkerRunning = false;
            g_txInProgress = false;
            return;
        }

        if (job.outgoing) {
            PendingSend ps;
            ps.typedText = job.text;
            if (r.ok && !r.translated.empty() && ToLower(r.detected) != ToLower(job.tl) && !SameText(r.translated, job.text)) {
                ps.gameText = r.translated;
                ps.txFrom = r.detected.empty() ? "en" : r.detected;
                ps.txTo = job.tl;
                ps.txText = r.translated;
            }
            else {
                ps.gameText = job.text;
            }
            QueueSend(std::move(ps));
        }
        else {
            ApplyIncomingResult(job.lineId, r, job.tl);
        }
        g_txInProgress = false;
    }
}

void RequestIncomingTranslate(uint64_t lineId, const std::string& body, bool priority) {
    if (!lineId || body.empty()) return;
    bool found = false;
    {
        std::lock_guard lock(g_mtx);
        for (auto& line : g_log) {
            if (line.id != lineId) continue;
            found = true;
            if (line.txStatus == TxStatus::Pending) return;
            if (line.txStatus == TxStatus::Done && !priority) return;
            line.txStatus = TxStatus::Pending;
            TouchChatLine(line);
            break;
        }
    }
    if (!found) return;

    std::string target;
    {
        std::lock_guard slock(g_settingsMtx);
        target = ToLower(TrimCopy(g_prefs.txTarget));
    }
    if (target.empty()) target = "en";

    TranslateJob job;
    job.lineId = lineId;
    job.text = body;
    job.sl = "auto";
    job.tl = target;
    job.outgoing = false;
    job.priority = priority;
    EnqueueTranslate(std::move(job));
}

void RequestOutgoingTranslate(const std::string& text, const std::string& tl) {
    TranslateJob job;
    job.text = text;
    job.sl = "auto";
    job.tl = tl;
    job.outgoing = true;
    job.priority = true;
    EnqueueTranslate(std::move(job));
}

bool StripLangPrefix(std::string& text, std::string& langOut) {
    langOut.clear();
    if (StartsWithCI(text, "/en ") || StartsWithCI(text, "/as ")) {
        text = TrimCopy(text.substr(4));
        langOut.clear();
        return true;
    }
    for (const auto& l : OUT_LANGS) {
        if (!l.prefix) continue;
        if (StartsWithCI(text, l.prefix)) {
            text = TrimCopy(text.substr(strlen(l.prefix)));
            langOut = l.code;
            return true;
        }
    }
    return false;
}

// =============================================================================

size_t TxCacheSize() {
    std::lock_guard lock(g_txMtx);
    return g_txCache.size();
}
size_t TxQueueSize() {
    std::lock_guard lock(g_txMtx);
    return g_txQueue.size();
}

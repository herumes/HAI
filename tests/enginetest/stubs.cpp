// Stand-ins for everything prefab.cpp and buildsession.cpp reach into the game
// for. The shape list is simulated closely enough to test the draw diffing and
// the relog recovery: tags live in one set, and "what is really in the client's
// list" in another, so a relog can be faked by emptying only the second.
#include "pch.h"
#include "common.hpp"
#include "hooks.hpp"
#include "debugshape.hpp"
#include "gamesdk.hpp"
#include "position.hpp"
#include "placewatch.hpp"
#include "packets.hpp"
#include <unordered_set>

// ---- test controls ----------------------------------------------------------
long long g_testNowMs = 1000000;
std::wstring g_testDir;
bool g_testPlayerValid = true;
double g_testPX = 0, g_testPY = 0, g_testPZ = 0;
std::unordered_set<uint64_t> g_testTags;       // what the tag table believes
std::unordered_set<uint64_t> g_testInList;     // what the client really has
int g_testSpawns = 0, g_testUpdates = 0, g_testRemoves = 0, g_testClears = 0;

// ---- common ------------------------------------------------------------------
std::atomic<bool> g_shuttingDown{ false };
std::atomic<int>  g_hooksInFlight{ 0 };
std::atomic<int>  g_workersAlive{ 0 };
void LogToFile(const std::string&) {}
std::string ToLower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string TrimCopy(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}
long long NowSteadyMs() { return g_testNowMs; }
std::wstring GetThisModuleDirectory() { return g_testDir; }
std::string WideToUtf8(const wchar_t* w, int len) {
    if (len <= 0) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), n, nullptr, nullptr);
    return out;
}
std::string WideToUtf8Path(const std::wstring& w) { return WideToUtf8(w.c_str(), (int)w.size()); }
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}
std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}
// Synchronous, so the async session paths can be tested step by step.
void StartWorker(std::function<void()> fn) { fn(); }

// ---- hooks -------------------------------------------------------------------
bool IsPlausiblePtr(const void*) { return false; }
bool SafeRead(const void*, void*, size_t) { return false; }

// ---- shapes ------------------------------------------------------------------
std::atomic<bool> g_dbgKeepAlive{ false };
std::atomic<int>  g_dbgWantedCapacity{ 64 };
const int kDbgMaxCapacity = 8192;
void DbgClearSpawnQueue() {}
void DbgRequestClearAll() { g_testTags.clear(); g_testInList.clear(); ++g_testClears; }
void DbgForgetTags() { g_testTags.clear(); }
bool DbgSpawnReady() { return true; }
std::string DbgAutoStatus() { return "ready"; }
int DbgListCapacity() { return 8192; }
size_t DbgSpawnQueueDepth() { return 0; }
bool DbgHasTag(uint64_t t) { return g_testTags.count(t) != 0; }
bool DbgUpdateTagged(uint64_t t, const DbgRequest&) {
    if (!g_testTags.count(t) || !g_testInList.count(t)) return false;
    ++g_testUpdates; return true;
}
bool DbgRemoveTagged(uint64_t t) { ++g_testRemoves; g_testTags.erase(t); g_testInList.erase(t); return true; }
bool DbgReuseSlot(uint64_t, const DbgRequest&) { return false; }
bool DbgQueueSpawn(const DbgRequest& r) {
    ++g_testSpawns; g_testTags.insert(r.tag); g_testInList.insert(r.tag); return true;
}
int DbgCountTagsAlive(const std::vector<uint64_t>& tags, int& missing) {
    missing = 0; int alive = 0;
    for (uint64_t t : tags) {
        if (!g_testTags.count(t)) { ++missing; continue; }
        if (g_testInList.count(t)) ++alive;
    }
    return alive;
}

// ---- game ------------------------------------------------------------------------
std::mutex g_gsdkFacingMtx;
GsdkFacingPath g_gsdkFacing;
uintptr_t GsdkCurrentRoot() { return 0; }
uintptr_t GsdkFollow(uintptr_t, const std::vector<uint32_t>&) { return 0; }
GsdkPlayerInfo GsdkGetPlayerInfo() {
    GsdkPlayerInfo me;
    me.valid = g_testPlayerValid;
    me.x = g_testPX; me.y = g_testPY; me.z = g_testPZ;
    return me;
}
std::atomic<double> g_posX{ 0 }, g_posY{ 0 }, g_posZ{ 0 };
std::atomic<long long> g_posLastUpdateMs{ 0 };

// ---- placement watcher -------------------------------------------------------
std::mutex g_placeMtx;
PlaceLayout g_placeLayout;
std::atomic<bool> g_placeLearning{ false };
std::atomic<bool> g_pktOutHooked{ false };
bool PlaceIsWatching() { return false; }
void PlaceLearnArm() {}

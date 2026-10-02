#include "pch.h"
#include "autofarm.hpp"
#include "common.hpp"
#include "settings.hpp"
#include "gamesdk.hpp"
#include "position.hpp"
#include "chat.hpp"      // g_hWnd - Hytale's window
#include <mmsystem.h>
#include <cmath>
#include <deque>

#pragma comment(lib, "winmm.lib")

namespace {

float AngDiff(float a, float b) {
    float d = std::fmod(a - b + 180.0f, 360.0f);
    if (d < 0) d += 360.0f;
    return d - 180.0f;
}

bool Down(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool HytaleInFront() {
    const HWND h = g_hWnd;
    return h && GetForegroundWindow() == h;
}

// Sub-millisecond clock for frame times; the rest of the program's ms clock
// is too coarse to tell 144 fps frames apart properly.
double PreciseMs() {
    static const double perMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / perMs;
}

// =============================================================================
// Settings, as the worker sees them
// =============================================================================
// Only the UI thread writes g_prefs, and everyone else locks to read, so the
// worker takes a copy a few times a second instead of locking every poll.
struct Cfg {
    std::vector<int> keys;
    int inputMode = 0, inputLearned = 0, steer = 0, timing = 50;
    float degPerCount = 0, extraLeadMs = 0, maxOff = 5;
    int stuckSec = 4, focusLoss = 0, laps = 0, minutes = 0, countdown = 3;
    bool stopOnKeys = true, autoFinish = true, sounds = true, flash = true, useRide = true;
};

Cfg ReadCfg() {
    Cfg c;
    std::string keys;
    {
        std::lock_guard lock(g_settingsMtx);
        const ChatPrefs& p = g_prefs;
        keys = p.farmKeys;
        c.inputMode = p.farmInputMode;
        c.inputLearned = p.farmInputLearned;
        c.steer = p.farmSteer;
        c.degPerCount = p.farmDegPerCount;
        c.extraLeadMs = p.farmExtraLeadMs;
        c.timing = p.farmTiming;
        c.useRide = p.farmUseRide;
        c.maxOff = p.farmMaxOff;
        c.stuckSec = p.farmStuckSec;
        c.focusLoss = p.farmFocusLoss;
        c.laps = p.farmLaps;
        c.minutes = p.farmMinutes;
        c.countdown = p.farmCountdown;
        c.stopOnKeys = p.farmStopOnKeys;
        c.autoFinish = p.farmAutoFinish;
        c.sounds = p.farmSounds;
        c.flash = p.farmFlash;
    }
    c.keys = FarmParseKeyList(keys);
    return c;
}

// What the worker found out, waiting for the UI thread to store it.
std::atomic<int>   g_learnedInput{ -1 };
std::atomic<float> g_learnedRate{ 0.0f };
std::atomic<bool>  g_learnedRatePending{ false };

// =============================================================================
// Library
// =============================================================================
std::mutex g_libMtx;
std::vector<std::shared_ptr<const FarmRoute>> g_lib;   // newest first
std::wstring g_selected;                                  // file path

std::wstring SelectedFile() { return FarmFolder() + L"selected.txt"; }

void SaveSelected() {
    std::wstring sel;
    { std::lock_guard lock(g_libMtx); sel = g_selected; }
    std::ofstream f(SelectedFile(), std::ios::binary | std::ios::trunc);
    if (f) f << WideToUtf8(sel.c_str(), (int)sel.size());
}

// Swap in an edited copy of one route.
void Replace(const std::wstring& file, std::shared_ptr<const FarmRoute> r) {
    std::lock_guard lock(g_libMtx);
    for (auto& p : g_lib) if (p->file == file) p = std::move(r);
}

std::shared_ptr<FarmRoute> CopyOf(const std::wstring& file) {
    std::lock_guard lock(g_libMtx);
    for (auto& p : g_lib) if (p->file == file) return std::make_shared<FarmRoute>(*p);
    return nullptr;
}

std::string NextName() {
    std::lock_guard lock(g_libMtx);
    for (int n = 1;; ++n) {
        const std::string name = "Farm " + std::to_string(n);
        bool used = false;
        for (auto& p : g_lib) if (p->name == name) { used = true; break; }
        if (!used) return name;
    }
}

// =============================================================================
// Sounds and the taskbar
// =============================================================================
enum class Snd { Rec, Saved, Start, Stop, Done, Error, Tick };

void Sound(const Cfg& c, Snd s) {
    if (!c.sounds) return;
    // Beep blocks for its duration, and the worker must not.
    StartWorker([s]() {
        switch (s) {
        case Snd::Rec:   Beep(1175, 140); break;
        case Snd::Saved: Beep(660, 70); Beep(880, 70); Beep(1320, 110); break;
        case Snd::Start: Beep(660, 70); Beep(990, 110); break;
        case Snd::Stop:  Beep(990, 70); Beep(660, 110); break;
        case Snd::Done:  Beep(880, 90); Beep(880, 90); Beep(1320, 160); break;
        case Snd::Error: Beep(420, 180); Beep(320, 180); Beep(420, 180); break;
        case Snd::Tick:  Beep(880, 60); break;
        }
    });
}

void Flash() {
    const HWND h = g_hWnd;
    if (!h) return;
    FLASHWINFO fi{ sizeof(fi) };
    fi.hwnd = h;
    fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    FlashWindowEx(&fi);
}

// =============================================================================
// Keys into Hytale
// =============================================================================
bool IsMouseVk(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

// Background: the same messages a real key press puts in the window's queue,
// scan code and all. Games that read keys through SDL work from the scan code
// in lParam, not the virtual key.
void PostKey(int vk, bool down, bool repeat) {
    const HWND h = g_hWnd;
    if (!h || !IsWindow(h)) return;
    if (IsMouseVk(vk)) {
        if (repeat) return;
        RECT rc{};
        GetClientRect(h, &rc);
        const LPARAM pt = MAKELPARAM((rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2);
        UINT msg = 0;
        WPARAM w = 0;
        switch (vk) {
        case VK_LBUTTON: msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; w = down ? MK_LBUTTON : 0; break;
        case VK_RBUTTON: msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; w = down ? MK_RBUTTON : 0; break;
        case VK_MBUTTON: msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; w = down ? MK_MBUTTON : 0; break;
        default:
            msg = down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
            w = MAKEWPARAM(0, vk == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2);
            break;
        }
        PostMessageW(h, msg, w, pt);
        return;
    }
    const UINT scx = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC_EX);
    const UINT sc = scx & 0xFF;
    const bool ext = (scx & 0xFF00) != 0;
    WPARAM w = (WPARAM)vk;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) w = VK_SHIFT;
    else if (vk == VK_LCONTROL || vk == VK_RCONTROL) w = VK_CONTROL;
    else if (vk == VK_LMENU || vk == VK_RMENU) w = VK_MENU;
    const bool alt = w == VK_MENU;
    LPARAM l = 1 | ((LPARAM)sc << 16) | (ext ? (LPARAM)1 << 24 : 0) | (alt ? (LPARAM)1 << 29 : 0);
    if (!down) l |= ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
    else if (repeat) l |= (LPARAM)1 << 30;
    const UINT msg = down ? (alt ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt ? WM_SYSKEYUP : WM_KEYUP);
    PostMessageW(h, msg, w, l);
}

// Direct: SendInput with scan codes, which is what games that read raw input
// see. Goes to whichever window is in front.
void SendKey(int vk, bool down) {
    INPUT in{};
    if (IsMouseVk(vk)) {
        in.type = INPUT_MOUSE;
        switch (vk) {
        case VK_LBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case VK_RBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case VK_MBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        default:
            in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            in.mi.mouseData = vk == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2;
            break;
        }
    }
    else {
        in.type = INPUT_KEYBOARD;
        const UINT scx = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC_EX);
        in.ki.wScan = (WORD)(scx & 0xFF);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | ((scx & 0xFF00) ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    }
    SendInput(1, &in, sizeof(in));
}

void MouseMove(int dx) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

void KeyOut(int mode, int vk, bool down, bool repeat) {
    if (mode == 2) {
        if (repeat && IsMouseVk(vk)) return;
        SendKey(vk, down);
    }
    else PostKey(vk, down, repeat);
}

// Mirror of what is held, readable without locks from DllMain.
std::atomic<uint32_t> g_heldMask{ 0 };
std::atomic<int> g_heldMode{ 1 };
int g_heldVks[32]{};

struct Injector {
    int mode = 1;                   // 1 background, 2 direct
    std::vector<int> vks;
    uint32_t held = 0;
    long long lastAssertMs = 0;
    long long releasedMs[32]{};     // when each key was last let go of

    void Begin(const std::vector<int>& v, int m) {
        ReleaseAll();
        vks = v;
        mode = m;
        for (size_t i = 0; i < 32; ++i) g_heldVks[i] = i < vks.size() ? vks[i] : 0;
        g_heldMode.store(mode);
    }
    void Set(uint32_t want, long long now) {
        const uint32_t change = want ^ held;
        for (size_t i = 0; i < vks.size() && i < 32; ++i) {
            if (!(change & (1u << i))) continue;
            const bool down = (want >> i) & 1;
            KeyOut(mode, vks[i], down, false);
            if (!down) releasedMs[i] = now;
        }
        held = want;
        g_heldMask.store(held);
        // A real keyboard repeats a held key, and games that drop their key
        // state (focus changes, a menu opening) pick it up again from that.
        if (held && now - lastAssertMs >= 250) {
            for (size_t i = 0; i < vks.size() && i < 32; ++i)
                if (held & (1u << i)) KeyOut(mode, vks[i], true, true);
            lastAssertMs = now;
        }
    }
    void ReleaseAll() {
        const long long now = NowSteadyMs();
        for (size_t i = 0; i < vks.size() && i < 32; ++i) {
            if (!(held & (1u << i))) continue;
            KeyOut(mode, vks[i], false, false);
            releasedMs[i] = now;
        }
        held = 0;
        g_heldMask.store(0);
    }
    void SwitchMode(int m) {
        ReleaseAll();
        mode = m;
        g_heldMode.store(m);
    }
};

// =============================================================================
// Reading the player, outside the frame hook
// =============================================================================
bool ReadPos(float& x, float& y, float& z) {
    if (GsdkReadPositionFast(x, y, z)) return true;
    // Whatever the overlay found another way (the F7 line, a manual lock),
    // as long as it is fresh.
    if (g_posLocked.load() && NowSteadyMs() - g_posLastUpdateMs.load() < 300) {
        x = (float)g_posX.load(); y = (float)g_posY.load(); z = (float)g_posZ.load();
        return true;
    }
    return false;
}

// =============================================================================
// Frame snapshots
// =============================================================================
// Taken on the game's thread at the start of every frame, so a snapshot's
// position is exactly the state the game is about to step from and its keys
// exactly what that step will use. Keys sent after it land on the next frame.
struct Snap {
    double t = 0;
    bool havePos = false, haveYaw = false, haveRide = false, front = false;
    float x = 0, y = 0, z = 0, yaw = 0, ride = 0;
    uint32_t keys = 0;              // the record list, as held (recording only)
};

std::atomic<bool> g_wantFrames{ false };
std::atomic<bool> g_wantKeys{ false };
std::mutex g_snapMtx;
std::deque<Snap> g_snaps;
HANDLE g_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
std::atomic<long long> g_lastFrameMs{ 0 };
std::mutex g_hookKeysMtx;
std::vector<int> g_hookKeys;        // the record list, for the hook to read

// =============================================================================
// Where you went, for the map
// =============================================================================
std::mutex g_trailMtx;
FarmTrail g_trail;

// =============================================================================
// The worker
// =============================================================================
enum class Cmd { Record, CancelRecord, Play, PlayCountdown, Stop };
std::mutex g_cmdMtx;
std::deque<Cmd> g_cmds;
void Push(Cmd c) { std::lock_guard lock(g_cmdMtx); g_cmds.push_back(c); }

std::mutex g_statusMtx;
FarmStatus g_status;
std::atomic<int> g_mode{ (int)FarmMode::Idle };
std::atomic<bool> g_started{ false };

std::string Clock(long long ms) {
    char b[32];
    const long long s = ms / 1000;
    if (s >= 3600) snprintf(b, sizeof(b), "%lldh %02lldm", s / 3600, (s / 60) % 60);
    else snprintf(b, sizeof(b), "%lld:%02lld", s / 60, s % 60);
    return b;
}

class Worker {
public:
    void Run();

private:
    void Message(const std::string& m, bool error) {
        std::lock_guard lock(g_statusMtx);
        g_status.message = m;
        g_status.messageIsError = error;
        g_status.messageMs = NowSteadyMs();
    }
    void SetMode(FarmMode m) {
        mode_ = m;
        g_mode.store((int)m);
        const bool frames = m == FarmMode::Recording || m == FarmMode::Playing ||
                            m == FarmMode::Paused || m == FarmMode::Countdown;
        g_wantFrames.store(frames);
        g_wantKeys.store(m == FarmMode::Recording);
    }

    void StartRecording();
    void FinishRecording(bool closedByItself);
    void BeginPlay();
    void StopPlay(const std::string& why, Snd snd, bool error);
    void EndFromFollower();
    void Calibrate();
    void OnSnap(const Snap& s);
    void TickPlaying(const Snap& s);
    void Publish();
    Snap Synthesize();
    FarmObs Obs(const Snap& s) const {
        FarmObs o;
        o.havePos = s.havePos;
        o.x = s.x; o.y = s.y; o.z = s.z;
        o.haveRide = s.haveRide;
        o.ride = s.ride;
        return o;
    }

    Cfg cfg_;
    FarmMode mode_ = FarmMode::Idle;
    long long now_ = 0;
    Snap cur_;                      // the latest reading, however it was taken
    long long lastPosOkMs_ = 0;
    bool framesLive_ = false;
    double lastSynthT_ = 0;

    FarmRecorder rec_;
    FarmFollower fol_;
    std::shared_ptr<const FarmRoute> route_;
    Injector inj_;

    long long countdownEnd_ = 0;
    int lastCountdown_ = -1;
    long long playStart_ = 0;
    bool verifying_ = false;
    double verifyHeldMs_ = 0, lastPlayT_ = 0;
    float vx0_ = 0, vy0_ = 0, vz0_ = 0;
    bool steering_ = false;
    float rate_ = 0;
    long long lastSteerMs_ = 0;
    float lastYawSeen_ = 1e9f;
    bool armed_[32]{};
    bool handBackArmed_[4]{};
    std::string pauseReason_;
    double trailAt_ = 0;
};

// Without the frame hook: a reading made here, only when something changed
// (or now and then), so still frames don't pile up.
Snap Worker::Synthesize() {
    Snap s;
    s.t = PreciseMs();
    s.havePos = ReadPos(s.x, s.y, s.z);
    s.haveYaw = GsdkReadCompassFast(s.yaw);
    s.haveRide = GsdkReadRideYaw(s.ride);
    s.front = HytaleInFront();
    if (mode_ == FarmMode::Recording && s.front)
        for (size_t i = 0; i < cfg_.keys.size() && i < 32; ++i)
            if (Down(cfg_.keys[i])) s.keys |= 1u << i;
    return s;
}

void Worker::StartRecording() {
    if (!cur_.havePos) {
        Message("Can't see where you are yet. Check the Player tab shows your position, then try again.", true);
        Sound(cfg_, Snd::Error);
        return;
    }
    if (cfg_.keys.empty()) {
        Message("There are no keys to record. Add the keys you drive with on Settings > Auto farm.", true);
        Sound(cfg_, Snd::Error);
        return;
    }
    { std::lock_guard lock(g_hookKeysMtx); g_hookKeys = cfg_.keys; }
    rec_.Begin(cfg_.keys, cfg_.autoFinish, framesLive_, (long long)PreciseMs());
    SetMode(FarmMode::Recording);
    std::string m = cfg_.autoFinish
        ? "Recording. Drive one full lap - it finishes by itself when you're back where you started."
        : "Recording. Drive the route, then press the record key again to finish.";
    if (!cur_.haveRide) {
        GsdkRidePath rp;
        { std::lock_guard lock(g_gsdkRideMtx); rp = g_gsdkRide; }
        if (rp.found) m += " (The tractor's direction isn't reading - get on the tractor so it's recorded too.)";
    }
    Message(m, false);
    Sound(cfg_, Snd::Rec);
}

void Worker::FinishRecording(bool closedByItself) {
    FarmRoute r;
    std::string why;
    SetMode(FarmMode::Idle);
    if (!rec_.Finish(r, why)) {
        Message("Nothing saved: " + why, true);
        Sound(cfg_, Snd::Error);
        return;
    }
    r.name = NextName();
    CreateDirectoryW(FarmFolder().c_str(), nullptr);
    std::wstring path;
    for (int i = 0;; ++i) {
        path = FarmFolder() + L"farm_" + std::to_wstring(r.createdUnix) + (i ? L"_" + std::to_wstring(i) : L"") + L".farm";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    r.file = path;
    if (!FarmSaveRoute(r, path)) {
        Message("Recorded, but couldn't write " + WideToUtf8Path(path) + ".", true);
        Sound(cfg_, Snd::Error);
        return;
    }
    {
        std::lock_guard lock(g_libMtx);
        g_lib.insert(g_lib.begin(), std::make_shared<FarmRoute>(r));
        g_selected = path;
    }
    SaveSelected();

    char buf[480];
    const float swing = FarmYawSwing(r);
    snprintf(buf, sizeof(buf), "Saved \"%s\": %s, %.0f blocks, %.1f s%s%s. %s",
        r.name.c_str(), r.loop ? "a full lap" : "one way (it didn't end where it started)",
        r.length, r.durationMs / 1000.0,
        r.hasRide ? (r.turnPlus >= 0 ? ", tractor direction and turn keys recorded" : ", tractor direction recorded")
                  : ", no tractor direction",
        r.hasYaw && swing > 20.0f ? ", turns the camera" : "",
        r.loop ? "Press the farm key to drive it." : "It will drive once and stop.");
    Message(buf, false);
    Sound(cfg_, Snd::Saved);
    LogToFile(std::string("autofarm: ") + buf + (closedByItself ? " (closed by itself)" : "") +
        (r.perFrame ? " [per frame]" : " [polled]"));
}

void Worker::Calibrate() {
    // Nudge the camera a known amount and see how far it turned. Needed once:
    // it depends on your mouse sensitivity, which nothing else tells us.
    float y0 = 0, y1 = 0;
    bool ok = GsdkReadCompassFast(y0);
    if (ok) {
        for (int i = 0; i < 4; ++i) { MouseMove(30); Sleep(12); }
        Sleep(160);
        ok = GsdkReadCompassFast(y1);
        for (int i = 0; i < 4; ++i) { MouseMove(-30); Sleep(12); }
        Sleep(80);
    }
    const float r = ok ? AngDiff(y1, y0) / 120.0f : 0.0f;
    if (!ok || std::fabs(r) < 0.002f) {
        steering_ = false;
        Message("Moving the mouse didn't turn the camera, so it's driving without turning it.", true);
        return;
    }
    rate_ = r;
    g_learnedRate.store(r);
    g_learnedRatePending.store(true);
}

void Worker::BeginPlay() {
    route_ = FarmSelected();
    if (!route_) {
        Message("Record a lap first.", true);
        Sound(cfg_, Snd::Error);
        SetMode(FarmMode::Idle);
        return;
    }
    if (!cur_.havePos) {
        Message("Can't see where you are. Check the Player tab shows your position.", true);
        Sound(cfg_, Snd::Error);
        SetMode(FarmMode::Idle);
        return;
    }
    FarmFollowCfg fc;
    fc.frameLocked = framesLive_;
    fc.extraLeadMs = cfg_.extraLeadMs;
    // 0 = trust where you are (0.1 block), 50 = balanced (0.4), 100 = trust the timing (1.6).
    fc.posSigma = 0.4f * std::pow(2.0f, (cfg_.timing - 50) / 25.0f);
    fc.useRide = cfg_.useRide;
    fc.maxOff = cfg_.maxOff;
    fc.startRadius = (std::max)(cfg_.maxOff, 5.0f);
    fc.stuckMs = cfg_.stuckSec * 1000;
    std::string why;
    if (!fol_.Start(route_, fc, cur_.t, Obs(cur_), why)) {
        Message(why, true);
        Sound(cfg_, Snd::Error);
        SetMode(FarmMode::Idle);
        return;
    }

    const int im = cfg_.inputMode == 0 ? (cfg_.inputLearned == 2 ? 2 : 1) : cfg_.inputMode;
    inj_.Begin(route_->vks, im);
    verifying_ = true;
    verifyHeldMs_ = 0;
    lastPlayT_ = cur_.t;
    vx0_ = cur_.x; vy0_ = cur_.y; vz0_ = cur_.z;

    steering_ = route_->hasYaw && cfg_.steer != 2 && (cfg_.steer == 1 || FarmYawSwing(*route_) > 20.0f);
    if (steering_ && !cur_.haveYaw) steering_ = false;
    rate_ = cfg_.degPerCount;
    lastYawSeen_ = 1e9f;

    // A key you are already holding when it starts isn't you taking over.
    for (size_t i = 0; i < 32; ++i) armed_[i] = i < route_->vks.size() && !Down(route_->vks[i]);
    for (bool& a : handBackArmed_) a = false;

    {
        std::lock_guard lock(g_trailMtx);
        g_trail = FarmTrail{};
        g_trail.routeFile = route_->file;
    }
    trailAt_ = 0;

    playStart_ = now_;
    SetMode(FarmMode::Playing);
    std::string note;
    if (route_->hasRide && !fol_.UsingRide())
        note = cfg_.useRide ? "The tractor's direction isn't reading - playing without it." : "";
    else if (!route_->hasRide) {
        GsdkRidePath rp;
        { std::lock_guard lock(g_gsdkRideMtx); rp = g_gsdkRide; }
        if (rp.found) note = "This lap was recorded before the tractor's direction was found - re-record it to use it.";
    }
    Message(note, false);
    Sound(cfg_, Snd::Start);
    LogToFile("autofarm: playing \"" + route_->name + "\" with " + (im == 2 ? "direct" : "background") +
        " keys" + (steering_ ? ", turning the camera" : "") + (fol_.UsingRide() ? ", by tractor direction" : "") +
        (framesLive_ ? ", frame-locked" : ", polled"));

    if ((inj_.mode == 2 || steering_) && !HytaleInFront()) {
        pauseReason_ = "Click into Hytale - it starts as soon as the game is in front.";
        SetMode(FarmMode::Paused);
    }
}

void Worker::StopPlay(const std::string& why, Snd snd, bool error) {
    inj_.ReleaseAll();
    std::string m = why;
    if (route_ && fol_.Laps() > 0) {
        char b[96];
        snprintf(b, sizeof(b), "  (%d lap%s in %s)", fol_.Laps(), fol_.Laps() == 1 ? "" : "s",
            Clock(now_ - playStart_).c_str());
        m += b;
    }
    Message(m, error);
    Sound(cfg_, snd);
    if (error && cfg_.flash) Flash();
    LogToFile("autofarm: " + m);
    SetMode(FarmMode::Idle);
}

void Worker::EndFromFollower() {
    if (fol_.End() == FarmEnd::Finished) { StopPlay("Route finished.", Snd::Done, false); return; }
    // Mark it on the map: where you were, and where the lap had you.
    {
        std::lock_guard lock(g_trailMtx);
        g_trail.haveFail = true;
        g_trail.failX = cur_.x;
        g_trail.failZ = cur_.z;
        g_trail.wantX = fol_.CursorX();
        g_trail.wantZ = fol_.CursorZ();
        g_trail.failText = fol_.EndText();
    }
    StopPlay("Stopped: " + fol_.EndText() + "  The map shows where.", Snd::Error, true);
}

void Worker::OnSnap(const Snap& s) {
    cur_ = s;
    if (s.havePos) lastPosOkMs_ = now_;
    switch (mode_) {
    case FarmMode::Recording:
        if (!s.havePos) {
            if (now_ - lastPosOkMs_ > 2000) {
                SetMode(FarmMode::Idle);
                Message("Recording stopped: lost track of where you are.", true);
                Sound(cfg_, Snd::Error);
            }
            return;
        }
        // Only what reaches the game counts: keys typed into another window
        // are not driving the tractor.
        if (rec_.Feed((long long)std::llround(s.t), s.x, s.y, s.z, s.haveYaw, s.yaw, s.haveRide, s.ride,
                s.front ? s.keys : 0))
            FinishRecording(true);
        break;
    case FarmMode::Playing:
        TickPlaying(s);
        break;
    default: break;
    }
}

void Worker::TickPlaying(const Snap& s) {
    const bool needsFront = inj_.mode == 2 || steering_;
    if (needsFront && !HytaleInFront()) {
        if (cfg_.focusLoss == 1) {
            StopPlay("Stopped: Hytale stopped being the active window.", Snd::Stop, false);
            return;
        }
        inj_.ReleaseAll();
        pauseReason_ = "Paused - click back into Hytale to carry on.";
        SetMode(FarmMode::Paused);
        Sound(cfg_, Snd::Tick);
        return;
    }
    if (steering_ && rate_ == 0.0f) {
        Calibrate();
        fol_.Resume(PreciseMs());
        return;
    }

    const FarmFollowOut o = fol_.Tick(s.t, Obs(s));
    if (!fol_.Running()) { EndFromFollower(); return; }

    if (s.havePos && s.t - trailAt_ >= 100.0) {
        trailAt_ = s.t;
        std::lock_guard lock(g_trailMtx);
        if (g_trail.points.size() < 20000) g_trail.points.emplace_back(s.x, s.z);
    }

    // You taking over: a route key held down that isn't one we are holding.
    // Only while Hytale is in front - typing in another window isn't driving.
    if (cfg_.stopOnKeys && HytaleInFront()) {
        for (size_t i = 0; i < route_->vks.size() && i < 32; ++i) {
            const bool phys = Down(route_->vks[i]);
            if (!phys) { armed_[i] = true; continue; }
            // Direct keys show up as held; ours are ours, not yours. One we
            // have just let go of can still read as held for a moment.
            const bool ours = inj_.mode == 2 && ((inj_.held & (1u << i)) || now_ - inj_.releasedMs[i] < 150);
            if (armed_[i] && !ours) {
                StopPlay("Stopped: you pressed " + FarmKeyName(route_->vks[i]) + " - it's all yours.", Snd::Stop, false);
                return;
            }
        }
        // Opening chat or the menu: the held keys would type into it.
        static const int kHandBack[] = { VK_ESCAPE, 'T', VK_RETURN, VK_OEM_2 };
        for (int i = 0; i < 4; ++i) {
            const int vk = kHandBack[i];
            if (std::find(route_->vks.begin(), route_->vks.end(), vk) != route_->vks.end()) continue;
            const bool phys = Down(vk);
            if (!phys) { handBackArmed_[i] = true; continue; }
            if (handBackArmed_[i]) {
                StopPlay("Stopped: you opened chat or the menu - it's all yours.", Snd::Stop, false);
                return;
            }
        }
    }

    if (cfg_.laps > 0 && fol_.Laps() >= cfg_.laps) {
        StopPlay("Done - " + std::to_string(cfg_.laps) + " lap" + (cfg_.laps == 1 ? "" : "s") + ".", Snd::Done, false);
        return;
    }
    if (cfg_.minutes > 0 && now_ - playStart_ >= (long long)cfg_.minutes * 60000) {
        StopPlay("Done - " + std::to_string(cfg_.minutes) + " minutes are up.", Snd::Done, false);
        return;
    }

    // Do the keys reach the game? Holding them for a second and a half without
    // moving an inch says no.
    const double dt = (std::max)(0.0, s.t - lastPlayT_);
    lastPlayT_ = s.t;
    if (verifying_ && s.havePos) {
        const float dx = s.x - vx0_, dy = s.y - vy0_, dz = s.z - vz0_;
        if (dx * dx + dy * dy + dz * dz > 0.75f * 0.75f) {
            verifying_ = false;
            if (cfg_.inputMode == 0 && cfg_.inputLearned != inj_.mode) g_learnedInput.store(inj_.mode);
        }
        else if (o.keys && !fol_.InStop()) {
            verifyHeldMs_ += dt;
            if (verifyHeldMs_ > 1500) {
                if (inj_.mode == 1 && cfg_.inputMode == 0) {
                    inj_.SwitchMode(2);
                    verifyHeldMs_ = 0;
                    vx0_ = s.x; vy0_ = s.y; vz0_ = s.z;
                    fol_.Resume(s.t);
                    LogToFile("autofarm: background keys didn't move the player - switching to direct");
                    if (!HytaleInFront()) {
                        pauseReason_ = "Hytale doesn't take background keys on this PC. Click into Hytale and it carries on.";
                        SetMode(FarmMode::Paused);
                        return;
                    }
                    Message("Hytale doesn't take background keys here, so it's using direct keys - keep Hytale in front.", false);
                }
                else {
                    StopPlay("Stopped: holding the route's keys didn't move you. Are you on the tractor, and are "
                        "these the keys you drive with?", Snd::Error, true);
                    return;
                }
            }
        }
    }

    inj_.Set(o.keys, now_);

    // Camera: once per new reading, a proportional step towards the recorded
    // direction. Readings only change once a frame, so steering faster than
    // that just overshoots.
    if (steering_ && o.steer && s.haveYaw && rate_ != 0.0f && HytaleInFront()) {
        if (s.yaw != lastYawSeen_ || now_ - lastSteerMs_ > 40) {
            const float err = AngDiff(o.yaw, s.yaw);
            if (std::fabs(err) > 1.5f) {
                const float counts = (std::max)(-800.0f, (std::min)(800.0f, err / rate_ * 0.6f));
                if (std::fabs(counts) >= 1.0f) MouseMove((int)std::lround(counts));
            }
            lastSteerMs_ = now_;
            lastYawSeen_ = s.yaw;
        }
    }
}

void Worker::Publish() {
    FarmStatus s;
    {
        std::lock_guard lock(g_statusMtx);
        s.message = g_status.message;
        s.messageIsError = g_status.messageIsError;
        s.messageMs = g_status.messageMs;
    }
    s.mode = mode_;
    s.nowMs = now_;
    s.havePos = cur_.havePos;
    s.x = cur_.x; s.y = cur_.y; s.z = cur_.z;
    s.haveYaw = cur_.haveYaw;
    s.yaw = cur_.yaw;
    s.haveRide = cur_.haveRide;
    s.ride = cur_.ride;
    s.framesLive = framesLive_;
    if (mode_ == FarmMode::Recording) {
        s.recStarted = rec_.Started();
        s.recMs = rec_.ElapsedMs((long long)std::llround(cur_.t));
        s.recTravelled = rec_.Travelled();
        s.recFromStart = rec_.FromStart();
        s.recKeyChanges = rec_.KeyChanges();
        s.recVks = cfg_.keys;
        uint32_t mask = 0;
        for (size_t i = 0; i < cfg_.keys.size() && i < 32; ++i) if (Down(cfg_.keys[i])) mask |= 1u << i;
        s.recKeys = FarmKeysText(cfg_.keys, mask);
    }
    if (mode_ == FarmMode::Countdown)
        s.countdownLeft = (int)((countdownEnd_ - now_ + 999) / 1000);
    if ((mode_ == FarmMode::Playing || mode_ == FarmMode::Paused) && route_) {
        s.routeName = route_->name;
        s.routeFile = route_->file;
        s.segment = fol_.Segment();
        s.playMs = now_ - playStart_;
        s.laps = fol_.Laps();
        s.lastLapMs = fol_.LastLapMs();
        s.bestLapMs = fol_.BestLapMs();
        s.progress = fol_.Progress();
        s.speed = fol_.Speed();
        s.off = fol_.OffBy();
        s.toNextMs = fol_.ToNextChangeMs();
        s.inStop = fol_.InStop();
        s.unsticking = fol_.Unsticking(cur_.t);
        s.unsticks = fol_.Unsticks();
        s.usingRide = fol_.UsingRide();
        s.headingTaps = fol_.HeadingTaps();
        s.holding = FarmKeysText(*route_, inj_.held);
        s.cursorX = fol_.CursorX();
        s.cursorZ = fol_.CursorZ();
        s.inputMode = inj_.mode;
        s.verifying = verifying_;
        s.steering = steering_;
        s.pauseReason = mode_ == FarmMode::Paused ? pauseReason_ : "";
    }
    std::lock_guard lock(g_statusMtx);
    g_status = std::move(s);
}

void Worker::Run() {
    bool fineTimer = false;
    long long cfgAt = 0, publishAt = 0, rideAt = 0;
    while (!g_shuttingDown.load()) {
        now_ = NowSteadyMs();
        if (now_ - cfgAt > 250) { cfg_ = ReadCfg(); cfgAt = now_; }
        if (now_ - rideAt > 250) { GsdkRideTick(); rideAt = now_; }

        std::deque<Cmd> cmds;
        { std::lock_guard lock(g_cmdMtx); cmds.swap(g_cmds); }
        for (Cmd c : cmds) {
            const bool driving = mode_ == FarmMode::Playing || mode_ == FarmMode::Paused || mode_ == FarmMode::Countdown;
            switch (c) {
            case Cmd::Record:
                if (mode_ == FarmMode::Recording) FinishRecording(false);
                else if (g_gsdkRideFinding.load()) Message("Finish finding the tractor's direction first.", true);
                else {
                    if (driving) StopPlay("Stopped.", Snd::Stop, false);
                    StartRecording();
                }
                break;
            case Cmd::CancelRecord:
                if (mode_ == FarmMode::Recording) {
                    SetMode(FarmMode::Idle);
                    Message("Recording thrown away.", false);
                    Sound(cfg_, Snd::Stop);
                }
                break;
            case Cmd::Play:
            case Cmd::PlayCountdown:
                if (driving) StopPlay("Stopped.", Snd::Stop, false);
                else if (mode_ == FarmMode::Recording) {
                    Message("Finish the recording first (the record key, or Stop and save).", true);
                    Sound(cfg_, Snd::Error);
                }
                else if (g_gsdkRideFinding.load()) Message("Finish finding the tractor's direction first.", true);
                else if (c == Cmd::PlayCountdown && cfg_.countdown > 0) {
                    countdownEnd_ = now_ + cfg_.countdown * 1000LL;
                    lastCountdown_ = -1;
                    fol_ = FarmFollower();
                    route_ = FarmSelected();
                    SetMode(FarmMode::Countdown);
                }
                else BeginPlay();
                break;
            case Cmd::Stop:
                if (mode_ == FarmMode::Recording) FinishRecording(false);
                else if (driving) StopPlay("Stopped.", Snd::Stop, false);
                break;
            }
        }

        // ---- readings ------------------------------------------------------------
        // From the game's frames when they're coming in; otherwise read here,
        // keeping only readings where something changed.
        std::deque<Snap> snaps;
        { std::lock_guard lock(g_snapMtx); snaps.swap(g_snaps); }
        const long long lastFrame = g_lastFrameMs.load();
        framesLive_ = g_wantFrames.load() && lastFrame && now_ - lastFrame < 300;
        if (snaps.empty() && !framesLive_) {
            Snap s = Synthesize();
            const bool changed = s.havePos != cur_.havePos || s.x != cur_.x || s.y != cur_.y || s.z != cur_.z ||
                                 s.ride != cur_.ride || s.keys != cur_.keys || s.yaw != cur_.yaw;
            if (mode_ == FarmMode::Idle || changed || s.t - lastSynthT_ >= 50.0) {
                lastSynthT_ = s.t;
                snaps.push_back(s);
            }
        }
        for (const Snap& s : snaps) {
            OnSnap(s);
            if (mode_ == FarmMode::Idle && !snaps.empty()) { cur_ = snaps.back(); break; }
        }

        switch (mode_) {
        case FarmMode::Countdown: {
            const int left = (int)((countdownEnd_ - now_ + 999) / 1000);
            if (left != lastCountdown_ && left > 0) { lastCountdown_ = left; Sound(cfg_, Snd::Tick); }
            if (now_ >= countdownEnd_) BeginPlay();
            break;
        }
        case FarmMode::Paused:
            if (HytaleInFront()) {
                fol_.Resume(PreciseMs());
                verifyHeldMs_ = 0;
                vx0_ = cur_.x; vy0_ = cur_.y; vz0_ = cur_.z;
                pauseReason_.clear();
                SetMode(FarmMode::Playing);
            }
            break;
        default: break;
        }

        if (now_ - publishAt >= 30) { Publish(); publishAt = now_; }

        // Fine timing only while it matters. With frames coming in, the frame
        // hook wakes this the moment a snapshot is ready.
        const bool active = mode_ != FarmMode::Idle;
        if (active != fineTimer) {
            if (active) timeBeginPeriod(1); else timeEndPeriod(1);
            fineTimer = active;
        }
        if (!active) Sleep(15);
        else if (framesLive_) WaitForSingleObject(g_frameEvent, 5);
        else Sleep(1);
    }
    inj_.ReleaseAll();
    if (fineTimer) timeEndPeriod(1);
}

bool KeyEdge(int vk, bool& was) {
    if (vk <= 0 || vk > 255) { was = false; return false; }
    const bool now = Down(vk);
    const bool fired = now && !was;
    was = now;
    return fired;
}

} // namespace

// =============================================================================
// Public
// =============================================================================
void FarmOnGameFrame() {
    if (!g_wantFrames.load(std::memory_order_relaxed)) return;
    try {
        Snap s;
        s.t = PreciseMs();
        s.havePos = GsdkReadPositionFast(s.x, s.y, s.z);
        s.haveYaw = GsdkReadCompassFast(s.yaw);
        s.haveRide = GsdkReadRideYaw(s.ride);
        s.front = HytaleInFront();
        if (s.front && g_wantKeys.load(std::memory_order_relaxed)) {
            std::lock_guard lock(g_hookKeysMtx);
            for (size_t i = 0; i < g_hookKeys.size() && i < 32; ++i)
                if (Down(g_hookKeys[i])) s.keys |= 1u << i;
        }
        {
            std::lock_guard lock(g_snapMtx);
            if (g_snaps.size() >= 256) g_snaps.pop_front();
            g_snaps.push_back(s);
        }
        g_lastFrameMs.store(NowSteadyMs(), std::memory_order_relaxed);
        if (g_frameEvent) SetEvent(g_frameEvent);
    }
    catch (...) {}
}

FarmTrail FarmGetTrail() {
    std::lock_guard lock(g_trailMtx);
    return g_trail;
}

std::wstring FarmFolder() { return GetThisModuleDirectory() + L"farms\\"; }

void FarmReload() {
    std::vector<std::shared_ptr<const FarmRoute>> lib;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(FarmFolder(), ec)) {
        if (!e.is_regular_file() || e.path().extension() != L".farm") continue;
        FarmRoute r;
        std::string why;
        if (FarmLoadRoute(e.path().wstring(), r, why)) lib.push_back(std::make_shared<FarmRoute>(std::move(r)));
        else LogToFile("autofarm: skipped " + WideToUtf8Path(e.path().wstring()) + ": " + why);
    }
    std::sort(lib.begin(), lib.end(), [](const auto& a, const auto& b) { return a->createdUnix > b->createdUnix; });

    std::wstring sel;
    {
        std::ifstream f(FarmFolder() + L"selected.txt", std::ios::binary);
        std::string s;
        if (f) std::getline(f, s);
        sel = Utf8ToWide(TrimCopy(s));
    }
    std::lock_guard lock(g_libMtx);
    g_lib = std::move(lib);
    g_selected = sel;
}

void FarmInit() {
    if (g_started.exchange(true)) return;
    FarmReload();
    StartWorker([]() { Worker w; w.Run(); });
}

void FarmUiTick(bool hotkeysAllowed) {
    static bool wasPlay = false, wasRec = false;
    const bool play = KeyEdge(g_prefs.keyFarmPlay, wasPlay);
    const bool rec = KeyEdge(g_prefs.keyFarmRecord, wasRec);
    if (hotkeysAllowed && play) FarmTogglePlay(false);
    if (hotkeysAllowed && rec) FarmToggleRecord();

    const int li = g_learnedInput.exchange(-1);
    if (li >= 0) {
        std::lock_guard lock(g_settingsMtx);
        g_prefs.farmInputLearned = li;
        MarkPrefsChanged();
    }
    if (g_learnedRatePending.exchange(false)) {
        std::lock_guard lock(g_settingsMtx);
        g_prefs.farmDegPerCount = g_learnedRate.load();
        MarkPrefsChanged();
    }
}

void FarmToggleRecord() { Push(Cmd::Record); }
void FarmCancelRecord() { Push(Cmd::CancelRecord); }
void FarmStop() { Push(Cmd::Stop); }

void FarmTogglePlay(bool fromOverlay) {
    if (fromOverlay && !FarmActive()) {
        // The overlay has focus right now, so it may hand it to the game.
        const HWND h = g_hWnd;
        if (h && IsWindow(h)) {
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
        }
    }
    Push(fromOverlay ? Cmd::PlayCountdown : Cmd::Play);
}

bool FarmActive() { return g_mode.load() != (int)FarmMode::Idle; }

void FarmReleaseAllNow() {
    const uint32_t held = g_heldMask.exchange(0);
    const int mode = g_heldMode.load();
    for (int i = 0; i < 32; ++i)
        if ((held & (1u << i)) && g_heldVks[i]) KeyOut(mode, g_heldVks[i], false, false);
}

FarmStatus FarmGetStatus() {
    std::lock_guard lock(g_statusMtx);
    return g_status;
}

std::wstring FarmCompactText() {
    const FarmStatus s = FarmGetStatus();
    wchar_t b[128] = L"";
    switch (s.mode) {
    case FarmMode::Recording:
        swprintf_s(b, L"REC %s  %.0f blocks", Utf8ToWide(Clock(s.recMs)).c_str(), s.recTravelled);
        break;
    case FarmMode::Countdown: swprintf_s(b, L"Farm starts in %d", s.countdownLeft); break;
    case FarmMode::Playing:
        swprintf_s(b, L"FARM lap %d  %.0f%%", s.laps + 1, s.progress * 100.0f);
        break;
    case FarmMode::Paused: swprintf_s(b, L"FARM paused - click into Hytale"); break;
    default: break;
    }
    return b;
}

std::vector<std::shared_ptr<const FarmRoute>> FarmList() {
    std::lock_guard lock(g_libMtx);
    return g_lib;
}

std::shared_ptr<const FarmRoute> FarmSelected() {
    std::lock_guard lock(g_libMtx);
    for (auto& p : g_lib) if (p->file == g_selected) return p;
    return g_lib.empty() ? nullptr : g_lib.front();
}

void FarmSelect(const std::wstring& file) {
    { std::lock_guard lock(g_libMtx); g_selected = file; }
    SaveSelected();
}

bool FarmRename(const std::wstring& file, const std::string& name) {
    auto r = CopyOf(file);
    const std::string n = TrimCopy(name);
    if (!r || n.empty()) return false;
    r->name = n;
    if (!FarmSaveRoute(*r, file)) return false;
    Replace(file, r);
    return true;
}

bool FarmSetNudge(const std::wstring& file, int boundary, float ms) {
    auto r = CopyOf(file);
    if (!r) return false;
    if (ms == 0.0f) r->nudges.erase(boundary);
    else r->nudges[boundary] = ms;
    r->Finish();
    if (!FarmSaveRoute(*r, file)) return false;
    Replace(file, r);
    return true;
}

bool FarmDelete(const std::wstring& file) {
    const std::wstring bin = FarmFolder() + L"deleted\\";
    CreateDirectoryW(bin.c_str(), nullptr);
    const std::wstring to = bin + std::filesystem::path(file).filename().wstring();
    if (!MoveFileExW(file.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return false;
    std::lock_guard lock(g_libMtx);
    g_lib.erase(std::remove_if(g_lib.begin(), g_lib.end(), [&](const auto& p) { return p->file == file; }), g_lib.end());
    return true;
}

float FarmYawSwing(const FarmRoute& r) {
    if (!r.hasYaw || r.s.empty()) return 0;
    float cur = 0, lo = 0, hi = 0;
    for (size_t i = 1; i < r.s.size(); ++i) {
        cur += AngDiff(r.s[i].yaw, r.s[i - 1].yaw);
        lo = (std::min)(lo, cur);
        hi = (std::max)(hi, cur);
    }
    return hi - lo;
}

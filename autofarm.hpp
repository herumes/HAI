#pragma once
// =============================================================================
// Auto farm: record a lap of your farm once, then have it driven for you
// =============================================================================
// farmroute.hpp is the brain (where am I on the lap, which keys go with that).
// This file is the hands: a worker thread that reads your position from the
// client every millisecond, feeds it to the recorder or the follower, and
// presses or lets go of keys in Hytale to match.
//
// Keys reach the game one of two ways, and which one works depends on how the
// client reads its keyboard, which can only be found out by trying:
//   * background - key messages posted straight to Hytale's window. Hytale
//     doesn't need to be in front, and your real keyboard is left alone, so you
//     can use other windows while it farms.
//   * direct - SendInput, exactly as if typed. Always understood, but only by
//     whichever window is in front, so it pauses when Hytale isn't.
// "Automatic" starts with background and watches: if holding the keys doesn't
// move you within a second and a half, it switches to direct and remembers.
//
// Turning the camera is always direct (the client reads the mouse as raw
// input, which has no message to post), and only happens when the recorded lap
// turned it.
//
// Everything that can go wrong stops it and lets go of every key: leaving the
// route, getting stuck, the position going away, you pressing a route key
// yourself, the hotkey.
#include "pch.h"
#include "farmroute.hpp"
#include <memory>
#include <string>
#include <vector>

enum class FarmMode : int { Idle, Recording, Countdown, Playing, Paused };

struct FarmStatus {
    FarmMode mode = FarmMode::Idle;
    long long nowMs = 0;

    bool havePos = false;
    float x = 0, y = 0, z = 0;
    bool haveYaw = false;
    float yaw = 0;
    bool haveRide = false;          // the tractor's own facing, if found and readable
    float ride = 0;
    bool framesLive = false;        // snapshots come from the game's own frames

    // recording
    bool recStarted = false;        // the lap proper has begun (you moved or pressed a key)
    long long recMs = 0;
    float recTravelled = 0, recFromStart = 0;
    int recKeyChanges = 0;
    std::string recKeys;            // held right now, from the record list
    std::vector<int> recVks;

    // countdown / playing
    int countdownLeft = 0;
    std::string routeName;
    std::wstring routeFile;
    int segment = 0;                // where on the route the follower is
    long long playMs = 0;
    int laps = 0, lastLapMs = 0, bestLapMs = 0;
    float progress = 0, speed = 0, off = 0, toNextMs = -1;
    bool inStop = false, unsticking = false;
    int unsticks = 0;
    bool usingRide = false;         // lining up by the tractor's direction
    int headingTaps = 0;            // turn-key taps that kept the facing as recorded
    std::string holding;
    float cursorX = 0, cursorZ = 0;
    int inputMode = 0;              // 1 background, 2 direct
    bool verifying = false;         // automatic mode still checking background keys work
    bool steering = false;
    std::string pauseReason;

    // the last thing that happened, for the UI's message line
    std::string message;
    bool messageIsError = false;
    long long messageMs = 0;
};

// Once, from the overlay thread: loads the saved routes and starts the worker.
void FarmInit();

// Called by the game's own frame hook (gamesdk.cpp), on the game thread, every
// frame. While recording or playing it takes a snapshot - position, facings,
// the keys that frame used - and wakes the worker, so everything lines up with
// the game's frames instead of a clock that runs beside them. Nothing when idle.
void FarmOnGameFrame();

// Where you actually went this run (and the last), for the map: a white line
// next to the recorded one shows exactly where a lap wandered off.
struct FarmTrail {
    std::wstring routeFile;
    std::vector<std::pair<float, float>> points;   // x, z
    bool haveFail = false;
    float failX = 0, failZ = 0;                    // where it gave up
    float wantX = 0, wantZ = 0;                    // where the recording was at that moment
    std::string failText;
};
FarmTrail FarmGetTrail();
// Every overlay loop, whether or not the overlay is showing. `allowed` is the
// overlay's own "hotkeys may fire now" answer. Also where settings the worker
// has learned get written back, since only the UI thread writes settings.
void FarmUiTick(bool hotkeysAllowed);

void FarmToggleRecord();
void FarmCancelRecord();
// From the hotkey: starts at once. From the overlay: brings Hytale to the
// front and counts down first.
void FarmTogglePlay(bool fromOverlay);
void FarmStop();
bool FarmActive();

// DLL unload: let go of every key this holds, right now, without locks.
void FarmReleaseAllNow();

FarmStatus FarmGetStatus();
// One short line for the compact chat window; empty when idle.
std::wstring FarmCompactText();

// ---- the saved routes -------------------------------------------------------
std::vector<std::shared_ptr<const FarmRoute>> FarmList();
std::shared_ptr<const FarmRoute> FarmSelected();
void FarmSelect(const std::wstring& file);
bool FarmRename(const std::wstring& file, const std::string& name);
// Moves the file into farms\deleted rather than destroying it.
bool FarmDelete(const std::wstring& file);
bool FarmSetNudge(const std::wstring& file, int boundary, float ms);
void FarmReload();
std::wstring FarmFolder();

// How far the camera swings over a lap, in degrees. Small = the lap doesn't
// need the camera turned, so playback leaves your mouse alone.
float FarmYawSwing(const FarmRoute& r);

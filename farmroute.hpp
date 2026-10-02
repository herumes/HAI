#pragma once
// =============================================================================
// Farm routes: one recorded lap, and driving it again by lining up with it
// =============================================================================
// A farm lap is a handful of keys held in the right places: glitch down a lane,
// swing round at the end, glitch back up the next one. Replaying that on a
// timer does not work - a millisecond of drift per second is a block of error
// per lap at tractor speed, and the U-turns are two blocks wide.
//
// The first version keyed everything to position: wherever you are on the
// lap, hold what was held there. That is exact at speed and useless when the
// tractor is slow, stopped against a wall or spinning in place - exactly what
// the glitchy corners are made of - because there position barely changes
// while the keys change a lot.
//
// So playback lines itself up with the recording the way a person following
// a video would, using everything it can see at once:
//   * where you are          - decisive at speed,
//   * how fast you're going  - tells "just hit the wall" from "still coming in",
//   * which way the tractor faces, once that has been found - decisive while
//     turning in place,
//   * and the recording's own clock - what's left when nothing else changes,
//     so a stop or a spin is held for exactly as long as you held it.
// Internally it keeps a spread of guesses about "which moment of the lap is
// this?", moves them on every frame by the time that passed, and weighs them
// by how well each matches what it sees. Whichever evidence is sharp at that
// moment wins, with no switching between modes.
//
// Keys are pressed one game frame ahead of the matched moment, because what is
// sent now is what the game uses on its next frame.
//
// This file has no game in it at all (observations in, keys out), which is what
// lets tests/farmtest drive it with a simulated tractor. autofarm.cpp connects
// it to the client.
#include "pch.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct FarmSample {
    int32_t t = 0;              // ms since the lap started
    float x = 0, y = 0, z = 0;
    float yaw = 0;              // camera, compass degrees; meaningful only if route.hasYaw
    float ride = 0;             // the tractor's own facing, degrees; only if route.hasRide
    uint32_t keys = 0;          // bit i set = route.vks[i] held
};

struct FarmRoute {
    std::string name;
    std::wstring file;              // where it lives on disk, empty until saved
    std::vector<int> vks;           // the key behind each bit of FarmSample::keys
    std::vector<FarmSample> s;
    bool loop = false;              // the last sample is the first one again
    bool hasYaw = false;            // camera direction recorded
    bool hasRide = false;           // tractor direction recorded
    bool perFrame = false;          // one sample per game frame (the current recorder)
    long long createdUnix = 0;
    // Sample index where the keys change -> milliseconds to move that change
    // by. Negative = sooner. Lets one bad corner be fixed without re-recording.
    std::map<int, float> nudges;

    // ---- derived by Finish() ----
    std::vector<float> cum;         // path distance at each sample
    std::vector<uint32_t> eff;      // keys at each sample once nudges are applied
    std::vector<int> stopEnd;       // at the first sample of a stop: the sample it ends at; else -1
    float length = 0;
    int durationMs = 0;

    // Key changes as (time, keys from then on), nudges applied, in time order.
    std::vector<std::pair<double, uint32_t>> changes;

    // With the tractor's direction recorded: which keys turn it, worked out
    // from how it turned while you held them. -1 if none found.
    int turnPlus = -1, turnMinus = -1;      // key bits that turn it up / down in degrees
    float turnRate = 0;                     // degrees per second, the slower of the two

    // The lap resampled every kGridMs: what playback lines itself up against.
    static constexpr float kGridMs = 5.0f;
    // ux/uz: which way you were travelling - for a cell stood still, the way
    // you came in, so "short of the stop" can be measured along it.
    // stopAge: ms since the recording came to a standstill here (0 while moving).
    struct Cell { float x, y, z, vx, vy, vz, speed, ride, ux, uz, stopAge; };
    std::vector<Cell> grid;

    void Finish();
    void FindTurnKeys();
    int SegCount() const { return s.size() < 2 ? 0 : (int)s.size() - 1; }
    float SegLen(int j) const { return cum[j + 1] - cum[j]; }
    double Period() const { return s.empty() ? 0.0 : (double)s.back().t; }
    double Wrap(double t) const;            // into [0, Period) on a loop, clamped otherwise
    int SampleAt(double t) const;           // last sample at or before t
    uint32_t KeysAt(double t) const;        // nudges applied
    void PosAt(double t, float& x, float& y, float& z) const;
    float YawAt(double t) const;
    float DistAt(double t) const;           // path distance at t
};

// One maximal run of the same keys, for the steps list in the UI.
struct FarmStep {
    int first = 0, last = 0;        // sample range [first, last)
    uint32_t keys = 0;
    float length = 0;               // blocks
    int ms = 0;
    bool stopped = false;           // happened while standing still
    float dx = 0, dz = 0;           // overall direction of travel
};
std::vector<FarmStep> FarmSteps(const FarmRoute& r);

std::string FarmKeyName(int vk);
// "W+A", "(nothing)"
std::string FarmKeysText(const FarmRoute& r, uint32_t keys);
std::string FarmKeysText(const std::vector<int>& vks, uint32_t keys);
// "W A S D Space Shift Ctrl" -> VK codes; unknown words are skipped.
std::vector<int> FarmParseKeyList(const std::string& text);
std::string FarmFormatKeyList(const std::vector<int>& vks);

bool FarmSaveRoute(const FarmRoute& r, const std::wstring& path);
bool FarmLoadRoute(const std::wstring& path, FarmRoute& out, std::string& why);

// =============================================================================
// Recording
// =============================================================================
class FarmRecorder {
public:
    // perFrame: one call per game frame, every one kept. Otherwise calls come
    // from a fast poll and only changes (plus a heartbeat) are kept.
    void Begin(const std::vector<int>& vks, bool autoClose, bool perFrame, long long nowMs);
    // Returns true the moment the lap closes by itself (you drove back to
    // where it started, going the same way).
    bool Feed(long long nowMs, float x, float y, float z, bool haveYaw, float yaw,
        bool haveRide, float ride, uint32_t keys);
    // Manual stop, or collecting a lap that closed. False + why if unusable.
    bool Finish(FarmRoute& out, std::string& why);

    bool Active() const { return active_; }
    bool Closed() const { return closed_; }
    bool Started() const { return startIdx_ >= 0; }
    float Travelled() const { return travelled_; }
    float FromStart() const { return fromStart_; }
    int KeyChanges() const { return keyChanges_; }
    long long ElapsedMs(long long nowMs) const { return startIdx_ >= 0 ? nowMs - startMs_ : 0; }
    size_t Samples() const { return s_.size(); }

private:
    void Push(long long nowMs, float x, float y, float z, float yaw, float ride, uint32_t keys);

    std::vector<int> vks_;
    std::vector<FarmSample> s_;
    long long t0_ = 0, startMs_ = 0, lastPushMs_ = 0;
    bool active_ = false, autoClose_ = true, closed_ = false, perFrame_ = false;
    int yawSeen_ = 0, yawMissed_ = 0, rideSeen_ = 0, rideMissed_ = 0;
    int startIdx_ = -1;             // first sample of the lap proper (idle before it is dropped)
    float sx_ = 0, sy_ = 0, sz_ = 0; // where the lap starts
    bool haveDir_ = false;
    float dirX_ = 0, dirZ_ = 0;     // which way you set off
    float travelled_ = 0, fromStart_ = 0, maxFromStart_ = 0;
    int keyChanges_ = 0;
};

// =============================================================================
// Playback
// =============================================================================
struct FarmFollowCfg {
    bool  frameLocked = true;   // observations arrive once per game frame (press keys a frame ahead)
    float extraLeadMs = 0.0f;   // on top of that: + if every turn is late, - if early
    float posSigma = 0.4f;      // blocks; bigger trusts the recorded timing more than position
    bool  useRide = true;       // line up by the tractor's facing too, when both sides have it
    float maxOff = 5.0f;        // blocks from the route before giving up
    float startRadius = 5.0f;   // how close you must be to start
    int   stuckMs = 4000;       // no progress for this long = stuck
    int   lostMs = 700;         // position unreadable for this long = stop
};

enum class FarmEnd { None, Finished, OffRoute, Stuck, Lost, NoRoute };

struct FarmObs {
    bool havePos = false;
    float x = 0, y = 0, z = 0;
    bool haveRide = false;
    float ride = 0;             // tractor facing, degrees
};

struct FarmFollowOut {
    uint32_t keys = 0;
    bool steer = false;         // route has a camera direction here
    float yaw = 0;              // compass degrees to face
};

class FarmFollower {
public:
    // False + why if you are not near enough the route to start.
    bool Start(std::shared_ptr<const FarmRoute> r, const FarmFollowCfg& cfg, double nowMs,
        const FarmObs& o, std::string& why);
    FarmFollowOut Tick(double nowMs, const FarmObs& o);
    // After a pause: the clocks that decide "stuck" and "lost" start again.
    void Resume(double nowMs);

    bool Running() const { return running_; }
    FarmEnd End() const { return end_; }
    const std::string& EndText() const { return endText_; }

    int Laps() const { return laps_; }
    int LastLapMs() const { return lastLapMs_; }
    int BestLapMs() const { return bestLapMs_; }
    float Progress() const;             // 0..1 around the lap
    float Speed() const { return speed_; }
    float OffBy() const { return off_; }
    int Segment() const;                // sample being played, for the steps list
    bool InStop() const;                // the recording was standing still here
    float ToNextChangeMs() const;       // until the keys change; <0 if none ahead
    float CursorX() const { return cx_; }
    float CursorZ() const { return cz_; }
    double Tau() const { return tau_; } // matched moment of the lap, ms
    bool UsingRide() const { return usingRide_; }
    int Unsticks() const { return unsticks_; }      // times a stall was pushed through
    int HeadingTaps() const { return headingTaps_; } // turn-key taps that kept the facing right
    bool Unsticking(double nowMs) const { return nowMs < unstickUntilMs_; }

private:
    void Fail(FarmEnd e, const std::string& why);
    float Cost(int cell, const FarmObs& o, bool haveVel, float vx, float vy, float vz, bool posOnly) const;
    void Predict(double dtMs);
    bool Update(const FarmObs& o, bool haveVel, float vx, float vy, float vz);
    // Throws the guesses away and lines up afresh from what is seen now, over
    // the whole lap (spanMs <= 0) or near `aroundMs`. Returns the best distance.
    float Reacquire(const FarmObs& o, bool haveVel, float vx, float vy, float vz, double aroundMs, double spanMs);
    int Map() const;
    bool Velocity(double nowMs, float& vx, float& vy, float& vz) const;
    float ShortOfStop(int cell, const FarmObs& o) const;

    std::shared_ptr<const FarmRoute> r_;
    FarmFollowCfg cfg_;
    bool running_ = false;
    FarmEnd end_ = FarmEnd::None;
    std::string endText_;

    std::vector<float> w_, tmp_;        // belief over grid cells
    std::vector<float> kernel_;
    double tau_ = 0;
    double lastObsMs_ = 0, dtEst_ = 16.7;
    int obsCount_ = 0;
    bool usingRide_ = false;

    struct Obs { double t; float x, y, z; };
    std::vector<Obs> hist_;             // recent positions, for speed

    double lastPosMs_ = 0, progressMs_ = 0, offSinceMs_ = 0;
    double unwrapped_ = 0, progressMark_ = 0, lapStartMs_ = 0;
    float speed_ = 0, off_ = 0, cx_ = 0, cz_ = 0;
    int laps_ = 0, lastLapMs_ = 0, bestLapMs_ = 0;
    double unstickUntilMs_ = 0, unstickNextMs_ = 0;
    uint32_t unstickKeys_ = 0;
    int unsticks_ = 0;
    // Creeping up on a stop: how short it was at the last nudge, how many
    // nudges haven't helped. Against a wall they never will, and then the
    // stop counts as reached where you are.
    float gateReach_ = 0.35f;
    float creepShort_ = 0;
    int creepFutile_ = 0;
    bool headingTapped_ = false, lastHeadingTap_ = false;
    int headingTaps_ = 0;
    FarmFollowOut last_;
};

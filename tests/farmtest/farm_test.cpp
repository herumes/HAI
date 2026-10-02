// Auto-farm engine tests: the real farmroute.cpp driving a simulated tractor
// round a simulated farm. No game needed.
//
// The tractor here has its own facing, separate from anything else: A and D
// turn it (in place, too), W drives it forward, S brakes. The farm is four
// 2-wide lanes joined by open plazas, so every corner is the fiddly kind -
// drive in, stop, spin to face the next lane, cross, stop, spin, and line up
// with a lane entrance only two blocks wide. Get the spin a few degrees wrong
// and the tractor misses the entrance and jams.
//
// Like the game, keys decided at a frame are used by the next frame's tick,
// and the recorder sees the keys a frame's tick actually used.
#include "pch.h"
#include "farmroute.hpp"
#include <cmath>
#include <cstdio>
#include <functional>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; \
    printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

enum : uint32_t { KW = 1, KS = 2, KD = 4, KA = 8 };   // bits for vks {W, S, D, A}
static const std::vector<int> kVks = { 'W', 'S', 'D', 'A' };
static const float kPi = 3.14159265f;

static float AngDiff(float a, float b) {
    float d = std::fmod(a - b + 180.0f, 360.0f);
    if (d < 0) d += 360.0f;
    return d - 180.0f;
}

// ---- the farm ---------------------------------------------------------------
struct Rect { float x0, x1, z0, z1; };
struct Farm {
    float L;
    bool open;
    std::vector<Rect> r;
    std::vector<Rect> blocks;       // obstacles added by tests
    // Walled: 2-wide channels join the lanes, like a real tractor farm - the
    // walls keep you straight. Open: wide plazas where nothing does, so the
    // tractor has to be pointing exactly right on its own.
    Farm(float len, bool openPlazas) : L(len), open(openPlazas) {
        if (open) r = {
            { -1, 1, -L - 2, 12 }, { 11, 13, -L - 2, 2 }, { 23, 25, -L - 2, 2 }, { 35, 37, -L - 2, 12 },
            { -1, 13, -L - 14, -L }, { 23, 37, -L - 14, -L },                               // north plazas
            { 11, 25, 0, 8 },                                                               // south plaza
            { -1, 37, 10, 18 },                                                             // way back
        };
        else r = {
            { -1, 1, -L - 2, 12 }, { 11, 13, -L - 2, 2 }, { 23, 25, -L - 2, 2 }, { 35, 37, -L - 2, 12 },
            { -1, 13, -L - 2, -L }, { 23, 37, -L - 2, -L },                                 // north channels
            { 11, 25, 0, 2 },                                                               // south channel
            { -1, 37, 10, 12 },                                                             // way back
        };
    }
    bool Inside(float x, float z) const {
        for (const Rect& b : blocks) if (x > b.x0 && x < b.x1 && z > b.z0 && z < b.z1) return false;
        for (const Rect& q : r)
            if (x >= q.x0 + 0.45f && x <= q.x1 - 0.45f && z >= q.z0 + 0.45f && z <= q.z1 - 0.45f) return true;
        return false;
    }
};

// ---- the tractor --------------------------------------------------------------
struct Tractor {
    float x = 0, z = 0, yaw = -90;  // 0 = +x, 90 = +z; -90 faces "north" up the lanes
    float v = 0;                    // along the facing
    float vmax = 100, accel = 300, brake = 400, turn = 120;
    void Frame(const Farm& f, uint32_t k, float dt) {
        if (k & KA) yaw -= turn * dt;
        if (k & KD) yaw += turn * dt;
        if (k & KW) v = (std::min)(vmax, v + accel * dt);
        else if (k & KS) v = (std::max)(0.0f, v - brake * dt);
        else v *= std::pow(0.9f, dt * 60.0f);
        const float hx = std::cos(yaw * kPi / 180.0f), hz = std::sin(yaw * kPi / 180.0f);
        bool bx = false, bz = false;
        const float nx = x + hx * v * dt;
        if (f.Inside(nx, z)) x = nx; else bx = std::fabs(hx) > 0.02f;
        const float nz = z + hz * v * dt;
        if (f.Inside(x, nz)) z = nz; else bz = std::fabs(hz) > 0.02f;
        // Against a wall you slide along it at the part of your speed that
        // points along it; square into it you stop.
        if (bx && bz) v = 0;
        else if (bx) v *= std::fabs(hz);
        else if (bz) v *= std::fabs(hx);
    }
};

// ---- the driver who records the lap ---------------------------------------------
// Drives by looking, like a person: spin until facing the right way, drive,
// brake in time to stop on the mark, spin again.
struct Driver {
    struct Leg { float heading; int axis; float target; };
    std::vector<Leg> legs;
    int leg = 0, phase = 1;         // 0 turn, 1 drive, 2 brake
    Driver(float L, bool open) {
        const float n = open ? -L - 7 : -L - 1, s = open ? 4.0f : 1.0f, back = open ? 14.0f : 11.0f;
        legs = {
            { -90, 1, n }, { 0, 0, 12 }, { 90, 1, s }, { 0, 0, 24 },
            { -90, 1, n }, { 0, 0, 36 }, { 90, 1, back }, { 180, 0, 0 },
        };
    }
    uint32_t Keys(const Tractor& t) {
        for (int guard = 0; guard < 4; ++guard) {
            const Leg& g = legs[leg];
            const float err = AngDiff(g.heading, t.yaw);
            if (phase == 0) {
                if (std::fabs(err) > 1.0f) return err > 0 ? KD : KA;
                phase = 1;
                continue;
            }
            const float pos = g.axis == 0 ? t.x : t.z;
            const float dir = g.axis == 0 ? std::cos(g.heading * kPi / 180.0f) : std::sin(g.heading * kPi / 180.0f);
            const float rem = (g.target - pos) * (dir > 0 ? 1.0f : -1.0f);
            if (phase == 1) {
                if (rem <= t.v * t.v / (2.0f * t.brake)) { phase = 2; continue; }
                uint32_t k = KW;
                if (err > 1.5f) k |= KD;
                else if (err < -1.5f) k |= KA;
                return k;
            }
            if (t.v > 0.5f) return KS;
            // Stopped short of the mark: creep up on it, a tap at a time.
            if (rem > 0.15f && phase == 2) { phase = 3; return KW; }
            if (phase == 3) { phase = 2; return KS; }
            leg = (leg + 1) % (int)legs.size();
            phase = 0;
        }
        return 0;
    }
};

// ---- a game with frames -------------------------------------------------------------
struct Sim {
    Farm farm;
    Tractor tr;
    double now = 1000000;
    double frameMs = 1000.0 / 60.0;
    uint32_t pending = 0;           // keys sent this frame, used by the next tick
    Sim(float len, bool open) : farm(len, open) {}
    FarmObs Obs(bool ride) const {
        FarmObs o;
        o.havePos = true;
        o.x = tr.x; o.y = 64; o.z = tr.z;
        o.haveRide = ride;
        o.ride = tr.yaw;
        return o;
    }
    void Tick(uint32_t keys) {
        tr.Frame(farm, keys, (float)(frameMs / 1000.0));
        now += frameMs;
    }
    void Place(float x, float z, float yaw) { tr.x = x; tr.z = z; tr.yaw = yaw; tr.v = 0; pending = 0; }
};

// Records one lap per frame (or, for the old recorder, per millisecond).
static bool RecordLap(Sim& sim, FarmRoute& route, bool withRide, bool perFrame = true, int stopAtLeg = -1) {
    Driver d(sim.farm.L, sim.farm.open);
    sim.Place(0, 0, -90);
    FarmRecorder rec;
    rec.Begin(kVks, true, perFrame, (long long)sim.now);
    // Stand still a moment first, like pressing the hotkey and then driving.
    for (int i = 0; i < 20; ++i) {
        rec.Feed((long long)sim.now, sim.tr.x, 64, sim.tr.z, false, 0, withRide, sim.tr.yaw, 0);
        sim.Tick(0);
    }
    for (int i = 0; i < 20000; ++i) {
        const uint32_t k = d.Keys(sim.tr);
        if (stopAtLeg >= 0 && d.leg == stopAtLeg) break;
        bool closed = false;
        if (perFrame) closed = rec.Feed((long long)sim.now, sim.tr.x, 64, sim.tr.z, false, 0, withRide, sim.tr.yaw, k);
        else {
            // The old recorder polled every millisecond between frames.
            for (int ms = 0; ms < (int)sim.frameMs && !closed; ++ms)
                closed = rec.Feed((long long)sim.now + ms, sim.tr.x, 64, sim.tr.z, false, 0, withRide, sim.tr.yaw, k);
        }
        if (closed) break;
        if (GetEnvironmentVariableA("FARMDRV", nullptr, 0) && i % 30 == 0)
            printf("  f%d leg %d ph %d  (%.2f, %.2f) yaw %.1f v %.1f keys %s\n", i, d.leg, d.phase,
                sim.tr.x, sim.tr.z, sim.tr.yaw, sim.tr.v, FarmKeysText(kVks, k).c_str());
        sim.Tick(k);
    }
    std::string why;
    const bool ok = rec.Finish(route, why);
    if (!ok) printf("  record failed: %s\n", why.c_str());
    return ok;
}

static std::string Lbl(bool ride, const char* what) {
    return std::string(what) + (ride ? ", with direction:" : ":");
}

// FARMTRACE=from,to (ms into a playback) prints every frame in that window.
static bool TraceOn(double t) {
    char buf[64] = "";
    if (!GetEnvironmentVariableA("FARMTRACE", buf, sizeof(buf))) return false;
    double a = 0, b = 0;
    if (sscanf_s(buf, "%lf,%lf", &a, &b) != 2) return false;
    return t >= a && t <= b;
}

struct PlayResult { int laps = 0; FarmEnd end = FarmEnd::None; std::string why; int lastLap = 0; int unsticks = 0; int taps = 0; };

static PlayResult Play(Sim& sim, std::shared_ptr<const FarmRoute> r, int wantLaps, double maxMs,
    bool ride = false, std::function<void(Sim&, double)> tamper = nullptr, FarmFollowCfg cfg = FarmFollowCfg{}) {
    PlayResult res;
    FarmFollower f;
    cfg.useRide = ride;
    std::string why;
    if (!f.Start(r, cfg, sim.now, sim.Obs(ride), why)) { res.end = f.End(); res.why = why; return res; }
    const double start = sim.now;
    while (sim.now - start < maxMs) {
        const FarmFollowOut o = f.Tick(sim.now, sim.Obs(ride));
        if (!f.Running()) {
            if (GetEnvironmentVariableA("FARMDBG", nullptr, 0))
                printf("  stopped at (%.2f, %.2f) yaw %.1f v %.1f  tau %.0f  keys %s\n", sim.tr.x, sim.tr.z,
                    sim.tr.yaw, sim.tr.v, f.Tau(), FarmKeysText(kVks, sim.pending).c_str());
            break;
        }
        if (tamper) tamper(sim, sim.now - start);
        if (TraceOn(sim.now - start))
            printf("   t %6.0f  (%6.2f, %7.2f) yaw %6.1f v %5.1f | tau %6.0f off %.2f pushes %d keys %s\n", sim.now - start,
                sim.tr.x, sim.tr.z, sim.tr.yaw, sim.tr.v, f.Tau(), f.OffBy(), f.Unsticks(), FarmKeysText(kVks, o.keys).c_str());
        sim.Tick(sim.pending);
        sim.pending = o.keys;
        if (f.Laps() >= wantLaps) break;
    }
    res.laps = f.Laps();
    res.end = f.End();
    res.why = f.EndText();
    res.lastLap = f.LastLapMs();
    res.unsticks = f.Unsticks();
    res.taps = f.HeadingTaps();
    return res;
}

static void Report(const char* what, const PlayResult& r, int recMs = 0) {
    printf("%-34s laps %d", what, r.laps);
    if (r.lastLap) printf(", last lap %.2f s", r.lastLap / 1000.0);
    if (recMs) printf(" (recorded %.2f)", recMs / 1000.0);
    if (r.unsticks) printf(", %d stall pushes", r.unsticks);
    if (r.taps) printf(", %d heading taps", r.taps);
    if (r.end != FarmEnd::None) printf(", ended: %s", r.why.c_str());
    printf("\n");
}

int main() {
    // ---- keys ----
    {
        const std::vector<int> k = FarmParseKeyList("w a, S+d Space shift ctrl LMB f5 bogus W");
        CHECK(k.size() == 9);
        CHECK(k[0] == 'W' && k[3] == 'D' && k[4] == VK_SPACE && k[5] == VK_SHIFT && k[6] == VK_CONTROL);
        CHECK(k[7] == VK_LBUTTON && k[8] == VK_F5);
        CHECK(FarmFormatKeyList({ 'W', VK_SPACE, VK_SHIFT }) == "W Space Shift");
        CHECK(FarmKeysText(kVks, KW | KD) == "W+D");
        CHECK(FarmKeysText(kVks, 0) == "(nothing)");
    }

    const float L = 300;

    // =========================================================================
    // A walled farm, like the real one: 2-wide lanes and channels, so the
    // walls keep the tractor straight. This has to work without the tractor's
    // direction, since that's how it runs until "Find tractor direction" has
    // been done.
    // =========================================================================
    Sim sim(L, false);
    FarmRoute route;
    CHECK(RecordLap(sim, route, true));
    printf("walled farm recorded: %zu samples, %.0f blocks, %.2f s, loop=%d, turn keys %s/%s at %.0f deg/s\n",
        route.s.size(), route.length, route.durationMs / 1000.0, (int)route.loop,
        route.turnPlus >= 0 ? FarmKeyName(kVks[route.turnPlus]).c_str() : "-",
        route.turnMinus >= 0 ? FarmKeyName(kVks[route.turnMinus]).c_str() : "-", route.turnRate);
    CHECK(route.loop);
    CHECK(route.hasRide);
    CHECK(route.perFrame);
    CHECK(route.length > 1250 && route.length < 1450);
    CHECK(route.s.front().keys == KW);       // idle before the lap was dropped
    CHECK(!route.grid.empty());
    // The turn keys and rate, worked out from the lap: D turns it up, A down, 120 deg/s.
    CHECK(route.turnPlus == 2 && route.turnMinus == 3);
    CHECK(route.turnRate > 100 && route.turnRate < 140);
    FarmRoute plain = route;
    plain.hasRide = false;
    plain.Finish();
    // Twice: as it runs before "Find tractor direction" has been done, and after.
    for (bool ride : { false, true }) {
        auto walled = std::make_shared<FarmRoute>(ride ? route : plain);

        {
            Sim p(L, false);
            p.Place(0, 0, -90);
            const PlayResult r = Play(p, walled, 4, 200000, ride);
            Report(Lbl(ride, "walled, 60 fps").c_str(), r, route.durationMs);
            CHECK(r.laps == 4);
            CHECK(std::abs(r.lastLap - route.durationMs) < route.durationMs * (ride ? 0.04 : 0.20));
        }
        for (double fps : { 30.0, 144.0, 240.0 }) {
            Sim p(L, false);
            p.frameMs = 1000.0 / fps;
            p.Place(0, 0, -90);
            const PlayResult r = Play(p, walled, 3, 200000, ride);
            char label[40];
            snprintf(label, sizeof(label), "walled, %.0f fps", fps);
            Report(Lbl(ride, label).c_str(), r);
            CHECK(r.laps == 3);
            CHECK(std::abs(r.lastLap - route.durationMs) < route.durationMs * (ride ? 0.08 : 0.20));
        }
        // Every frame a different length.
        {
            Sim p(L, false);
            p.Place(0, 0, -90);
            unsigned seed = 12345;
            const PlayResult r = Play(p, walled, 3, 200000, ride, [&seed](Sim& s, double) {
                seed = seed * 1103515245u + 12345u;
                s.frameMs = 10.0 + (seed >> 16) % 1400 / 100.0;   // 10..24 ms
            });
            Report(Lbl(ride, "walled, jittery frames").c_str(), r);
            CHECK(r.laps == 3);
        }
        {
            Sim p(L, false);
            p.tr.vmax = 70;
            p.Place(0, 0, -90);
            const PlayResult r = Play(p, walled, 2, 200000, ride);
            Report(Lbl(ride, "walled, slower tractor").c_str(), r);
            CHECK(r.laps == 2);
        }
        {
            Sim p(L, false);
            p.Place(12, -150, 90);
            const PlayResult r = Play(p, walled, 2, 200000, ride);
            Report(Lbl(ride, "walled, start mid-lane").c_str(), r);
            CHECK(r.laps == 2);
        }
        // No frames for 250 ms, then the catch-up jump.
        {
            Sim p(L, false);
            p.Place(0, 0, -90);
            bool done = false;
            const PlayResult r = Play(p, walled, 2, 200000, ride, [&done](Sim& s, double t) {
                if (!done && t >= 1500) {
                    done = true;
                    for (int i = 0; i < 15; ++i) s.tr.Frame(s.farm, s.pending, 1 / 60.0f);
                    s.now += 250;
                }
            });
            Report(Lbl(ride, "walled, lag spike").c_str(), r);
            CHECK(r.laps == 2);
        }
        // The server puts you 8 blocks back.
        {
            Sim p(L, false);
            p.Place(0, 0, -90);
            bool done = false;
            const PlayResult r = Play(p, walled, 2, 200000, ride, [&done](Sim& s, double t) {
                if (!done && t >= 2000) { done = true; s.tr.z += 8; }
            });
            Report(Lbl(ride, "walled, rubber band").c_str(), r);
            CHECK(r.laps == 2);
        }
        // Teleported away: must stop, not drive blind.
        {
            Sim p(L, false);
            p.Place(0, 0, -90);
            const PlayResult r = Play(p, walled, 2, 60000, ride, [](Sim& s, double t) {
                if (t >= 2000 && t < 2020) { s.tr.x = 200; s.tr.z = 200; }
            });
            Report("teleported:", r);
            CHECK(r.end == FarmEnd::OffRoute);
            CHECK(r.laps == 0);
        }
        // Something in the lane: must stop as stuck.
        {
            Sim p(L, false);
            p.farm.blocks.push_back({ 10, 14, -101, -99 });
            p.Place(0, 0, -90);
            const PlayResult r = Play(p, walled, 2, 60000, ride);
            Report("blocked:", r);
            CHECK(r.end == FarmEnd::Stuck);
        }
    }

    // Too far away to start.
    {
        Sim p(L, false);
        p.Place(60, 60, 0);
        FarmFollower f;
        std::string why;
        CHECK(!f.Start(std::make_shared<FarmRoute>(plain), FarmFollowCfg{}, p.now, p.Obs(false), why));
        CHECK(!why.empty());
    }
    // The old recorder (polled every ms, v1 files) still plays.
    {
        Sim s2(L, false);
        FarmRoute old;
        CHECK(RecordLap(s2, old, false, false));
        CHECK(old.loop && !old.perFrame);
        Sim p(L, false);
        p.Place(0, 0, -90);
        const PlayResult r = Play(p, std::make_shared<FarmRoute>(old), 2, 200000);
        Report("walled, old-style recording:", r);
        CHECK(r.laps == 2);
    }
    // A route that is not a loop plays once and finishes.
    {
        Sim s2(L, false);
        FarmRoute oneWay;
        CHECK(RecordLap(s2, oneWay, false, true, 3));
        CHECK(!oneWay.loop);
        Sim p(L, false);
        p.Place(0, 0, -90);
        const PlayResult r = Play(p, std::make_shared<FarmRoute>(oneWay), 1, 60000);
        Report("one way:", r);
        CHECK(r.end == FarmEnd::Finished);
    }

    // =========================================================================
    // An open farm: every corner is a spin in a wide plaza, then a lane two
    // blocks wide a dozen blocks away. Only the tractor's own direction can
    // tell whether a spin ended right, so this is what that setup is for.
    // =========================================================================
    Sim simO(L, true);
    FarmRoute openRoute;
    CHECK(RecordLap(simO, openRoute, true));
    CHECK(openRoute.loop && openRoute.hasRide && openRoute.turnPlus == 2 && openRoute.turnMinus == 3);
    printf("open farm recorded: %.0f blocks, %.2f s\n", openRoute.length, openRoute.durationMs / 1000.0);
    auto open = std::make_shared<FarmRoute>(openRoute);
    FarmRoute openPlainR = openRoute;
    openPlainR.hasRide = false;
    openPlainR.Finish();
    auto openPlain = std::make_shared<FarmRoute>(openPlainR);
    {
        Sim p(L, true);
        p.Place(0, 0, -90);
        const PlayResult r = Play(p, openPlain, 3, 200000);
        Report("open, 60 fps, no direction:", r);   // not required: nothing to steer by
    }
    for (double fps : { 60.0, 144.0, 30.0 }) {
        Sim p(L, true);
        p.frameMs = 1000.0 / fps;
        p.Place(0, 0, -90);
        const PlayResult r = Play(p, open, 3, 200000, true);
        char label[48];
        snprintf(label, sizeof(label), "open, %.0f fps, with direction:", fps);
        Report(label, r);
        CHECK(r.laps == 3);
    }
    {
        Sim p(L, true);
        p.Place(0, 0, -90);
        unsigned seed = 777;
        const PlayResult r = Play(p, open, 3, 200000, true, [&seed](Sim& s, double) {
            seed = seed * 1103515245u + 12345u;
            s.frameMs = 10.0 + (seed >> 16) % 1400 / 100.0;
        });
        Report("open, jittery, with direction:", r);
        CHECK(r.laps == 3);
    }
    // The tractor turns 15% faster or slower than when recorded: held for the
    // recorded time every spin would end 13 degrees out.
    for (float k : { 1.15f, 0.85f }) {
        Sim p(L, true);
        p.tr.turn *= k;
        p.Place(0, 0, -90);
        const PlayResult r = Play(p, open, 2, 200000, true);
        Report(k > 1 ? "open, turns 15% faster:" : "open, turns 15% slower:", r);
        CHECK(r.laps == 2);
    }
    {
        Sim p(L, true);
        p.Place(0, -L - 7, -45);
        const PlayResult r = Play(p, open, 1, 200000, true);
        Report("open, start mid-spin:", r);
        CHECK(r.laps == 1);
    }

    // ---- nudges move a key change in time ----
    {
        FarmRoute n = plain;
        int b = -1;
        for (int i = 1; i < (int)n.s.size(); ++i)
            if (n.s[i].keys != n.s[i - 1].keys && n.s[i].t > 2000) { b = i; break; }
        CHECK(b > 0);
        const double raw = n.s[b].t;
        n.nudges[b] = -40.0f;
        n.Finish();
        CHECK(n.KeysAt(raw - 30) == n.s[b].keys);
        CHECK(n.KeysAt(raw - 50) == n.s[b - 1].keys);
        n.nudges[b] = 25.0f;
        n.Finish();
        CHECK(n.KeysAt(raw + 20) == n.s[b - 1].keys);
        CHECK(n.KeysAt(raw + 30) == n.s[b].keys);
    }

    // ---- save and load, and old files ----
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring path = std::wstring(tmp) + L"hyai_farmtest.farm";
        FarmRoute a = route;
        a.name = "Sam's farm";
        a.nudges[5] = -12.5f;
        CHECK(FarmSaveRoute(a, path));
        FarmRoute b;
        std::string why;
        CHECK(FarmLoadRoute(path, b, why));
        CHECK(b.name == "Sam's farm");
        CHECK(b.s.size() == a.s.size());
        CHECK(b.vks == a.vks);
        CHECK(b.loop == a.loop && b.hasRide == a.hasRide && b.perFrame == a.perFrame);
        CHECK(b.nudges.size() == 1 && b.nudges[5] == -12.5f);
        CHECK(std::fabs(b.length - a.length) < 0.5f);
        CHECK(std::fabs(b.s[100].ride - a.s[100].ride) < 0.01f);

        // Version 1: no tractor direction, nudges in blocks.
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << "hyai-farm 1\nname=Old\nloop=0\nyaw=0\ncreated=1\nkeys=87 83\nnudge=2 -1.5\nsamples=4\n";
            f << "0 0 64 0 0 1\n100 0 64 -10 0 1\n200 0 64 -20 0 2\n300 0 64 -30 0 2\n";
        }
        FarmRoute old;
        CHECK(FarmLoadRoute(path, old, why));
        CHECK(old.s.size() == 4 && !old.hasRide && !old.perFrame);
        // 1.5 blocks at 100 blocks/s is 15 ms.
        CHECK(old.nudges.count(2) && std::fabs(old.nudges[2] + 15.0f) < 0.5f);
        DeleteFileW(path.c_str());
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

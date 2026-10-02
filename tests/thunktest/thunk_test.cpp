// Exercises hookthunk.cpp without the game: calls through a thunk exactly the
// way a MinHook-detoured call arrives, and checks that every argument register
// survives, that a pre-function can change them, swallow the call or send it
// somewhere else, that an exception can unwind through the thunk, and that the
// stack is aligned inside the pre-function.
#include "../../hookthunk.hpp"
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <intrin.h>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// Six arguments: four in registers (rcx, xmm1, r8, xmm3) and two on the stack,
// so a thunk that disturbed rsp would hand the target the wrong stack args.
__declspec(noinline) double Target(long long a, double b, long long c, float d, long long e, double f) {
    return (double)a * 1.0 + b * 10.0 + (double)c * 100.0 + (double)d * 1000.0 + (double)e * 10000.0 + f * 100000.0;
}
__declspec(noinline) double Other(long long a, double b, long long, float, long long, double) {
    return -((double)a + b);
}
using Fn = double(*)(long long, double, long long, float, long long, double);

enum class Mode { Pass, Modify, Swallow, Substitute, Throw };
static Mode g_mode = Mode::Pass;
static bool g_aligned = true;
static uint64_t g_seenRet = 0;

static uintptr_t Pre(HookRegs* r) {
    alignas(16) volatile __m128 probe = _mm_set1_ps(1.0f);
    if (((uintptr_t)&probe & 15) != 0) g_aligned = false;
    (void)probe;
    g_seenRet = r->ret;
    switch (g_mode) {
    case Mode::Pass:
        return (uintptr_t)&Target;
    case Mode::Modify: {
        r->rcx = 7;
        double b; memcpy(&b, r->xmm1, 8); b += 0.5; memcpy(r->xmm1, &b, 8);
        return (uintptr_t)&Target;
    }
    case Mode::Swallow:
        return 0;
    case Mode::Substitute:
        return (uintptr_t)&Other;
    case Mode::Throw:
        throw std::runtime_error("from pre");
    }
    return 0;
}

// Integer-returning target, to check rax on a swallowed call.
__declspec(noinline) long long IntTarget(long long a, long long b) { return a + b; }
static uintptr_t IntPre(HookRegs* r) { return r->rcx == 0 ? 0 : (uintptr_t)&IntTarget; }

// Float-in-xmm1 target, the shape of the game's frame hook, with the dt
// carried across a skipped call the way gamesdk.cpp does it.
static float g_accum = 0;
__declspec(noinline) void Tick(void*, float dt) { g_accum += dt; }
static float g_carry = 0;
static int g_tickCall = 0;
static uintptr_t TickPre(HookRegs* r) {
    float dt = r->Xmm1Float() + g_carry;
    g_carry = 0;
    if (++g_tickCall % 3 == 0) { g_carry = dt; return (uintptr_t)&Other; }   // stand in for this call
    r->SetXmm1Float(dt);
    return (uintptr_t)&Tick;
}

int main() {
    void* t = MakePreHookThunk(&Pre);
    CHECK(t != nullptr);
    Fn f = (Fn)t;

    const double want = Target(1, 2.0, 3, 4.0f, 5, 6.0);

    g_mode = Mode::Pass;
    CHECK(f(1, 2.0, 3, 4.0f, 5, 6.0) == want);
    CHECK(g_aligned);
    CHECK(g_seenRet != 0);

    g_mode = Mode::Modify;
    CHECK(f(1, 2.0, 3, 4.0f, 5, 6.0) == Target(7, 2.5, 3, 4.0f, 5, 6.0));

    g_mode = Mode::Substitute;
    CHECK(f(1, 2.0, 3, 4.0f, 5, 6.0) == Other(1, 2.0, 3, 4.0f, 5, 6.0));

    g_mode = Mode::Swallow;
    f(1, 2.0, 3, 4.0f, 5, 6.0);   // must simply come back

    g_mode = Mode::Throw;
    bool caught = false;
    try { f(1, 2.0, 3, 4.0f, 5, 6.0); }
    catch (const std::runtime_error&) { caught = true; }
    CHECK(caught);

    // Still healthy after an exception went through it.
    g_mode = Mode::Pass;
    CHECK(f(1, 2.0, 3, 4.0f, 5, 6.0) == want);

    using IntFn = long long(*)(long long, long long);
    IntFn g = (IntFn)MakePreHookThunk(&IntPre);
    CHECK(g(40, 2) == 42);
    CHECK(g(0, 99) == 0);

    using TickFn = void(*)(void*, float);
    TickFn tick = (TickFn)MakePreHookThunk(&TickPre);
    for (int i = 0; i < 30; ++i) tick(nullptr, 0.25f);
    CHECK(std::fabs(g_accum + g_carry - 7.5f) < 1e-4f);   // nothing lost, only delayed

    // Many calls in a row, in case anything leaks stack.
    for (int i = 0; i < 100000; ++i) {
        if (f(1, 2.0, 3, 4.0f, 5, 6.0) != want) { CHECK(false); break; }
    }

    if (g_failures == 0) std::printf("thunktest: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}

#pragma once
// =============================================================================
// Hooks that never leave a native frame under the game's code
// =============================================================================
// The client is NativeAOT. Its garbage collector has to walk every thread's
// stack, and it can only unwind frames it compiled itself. A detour written the
// obvious way -
//
//     void hkFoo(void* self, void* arg) { ...; oFoo(self, arg); ...; }
//
// - CALLS the original, so for as long as the game's function runs, one of our
// C++ frames sits underneath it. If a collection happens in that window (any
// allocation on this thread, or any other thread asking for one), the walker
// meets our return address, recognises nothing, and the runtime fail-fasts the
// whole process: exception 0xC0000602 in HytaleClient.exe, no error message.
// Rare per call, certain over enough calls - which is what "works great for a
// while, then the game just closes" was. Every crash dump had hkOnReceive's
// return address as the frame the walker choked on.
//
// So hooks here run all of their own work FIRST, in a pre-function, and then
// JUMP to wherever the pre-function says. By the time the game's code runs, the
// thunk and the pre-function are gone from the stack and the game's function
// returns straight to the game's caller.
//
// The thunk saves the four argument registers and xmm0-3, calls the
// pre-function with a pointer to them, restores them (including any changes the
// pre-function made) and then:
//   - jumps to the returned address, or
//   - returns straight to the caller with rax = 0 if it returned 0 (a hook that
//     swallows the call - only valid for functions that return void or a value
//     where 0 is fine).
//
// Returning a DIFFERENT function with different arguments is how we call into
// the game safely: the game's caller "calls" that one instead, and our frame is
// not underneath it. Only do that in place of a call that is safe to skip.
//
// Rules for a pre-function:
//   - never let an exception escape (wrap the body in try/catch);
//   - never call managed code or a runtime allocator slow path from it unless
//     you accept the risk above - hand the call back to the thunk instead.
#include "pch.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

struct HookRegs {
    // Saved by the thunk, in its own frame. Changes are written back into the
    // registers before the jump.
    uint8_t  xmm0[16];
    uint8_t  xmm1[16];
    uint8_t  xmm2[16];
    uint8_t  xmm3[16];
    uint64_t pad;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t ret;   // the game's return address - read only

    float Xmm1Float() const { float f; memcpy(&f, xmm1, 4); return f; }
    void  SetXmm1Float(float f) { memcpy(xmm1, &f, 4); }
};
static_assert(offsetof(HookRegs, xmm1) == 0x10, "thunk layout");
static_assert(offsetof(HookRegs, r9) == 0x48, "thunk layout");
static_assert(offsetof(HookRegs, rcx) == 0x60, "thunk layout");
static_assert(offsetof(HookRegs, ret) == 0x68, "thunk layout");

// Returns the address to jump to with the (possibly changed) registers, or 0 to
// return to the caller without calling anything.
using HookPreFn = uintptr_t(*)(HookRegs* regs);

// Builds an executable thunk around `pre`, suitable as a MinHook detour.
// Returns nullptr if memory could not be allocated. Thunks live for the life
// of the process.
void* MakePreHookThunk(HookPreFn pre);

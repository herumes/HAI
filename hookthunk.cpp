#include "pch.h"
#include "hookthunk.hpp"

// Layout of one thunk page: code, then its unwind info, then the function table
// entry that points at both. One page per thunk - there are only a handful.
namespace {

constexpr size_t kCodeOff = 0x00;
constexpr size_t kUnwindOff = 0x80;
constexpr size_t kFuncOff = 0xA0;

// See hookthunk.hpp. Offsets in the comments are what the unwind info below
// refers to, so they must stay in step with it.
const uint8_t kThunk[] = {
    0x51,                               // 00 push rcx
    0x52,                               // 01 push rdx
    0x41, 0x50,                         // 02 push r8
    0x41, 0x51,                         // 04 push r9
    0x48, 0x83, 0xEC, 0x68,             // 06 sub  rsp, 68h          (end of prolog: 0Ah)
    0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, // 0A movdqu [rsp+20h], xmm0
    0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30, // 10 movdqu [rsp+30h], xmm1
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, // 16 movdqu [rsp+40h], xmm2
    0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50, // 1C movdqu [rsp+50h], xmm3
    0x48, 0x8D, 0x4C, 0x24, 0x20,       // 22 lea  rcx, [rsp+20h]     HookRegs*
    0x48, 0xB8,                         // 27 mov  rax, imm64         pre-function
    0, 0, 0, 0, 0, 0, 0, 0,             // 29   (patched)
    0xFF, 0xD0,                         // 31 call rax
    0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, // 33 movdqu xmm0, [rsp+20h]
    0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30, // 39 movdqu xmm1, [rsp+30h]
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, // 3F movdqu xmm2, [rsp+40h]
    0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50, // 45 movdqu xmm3, [rsp+50h]
    0x48, 0x83, 0xC4, 0x68,             // 4B add  rsp, 68h
    0x41, 0x59,                         // 4F pop  r9
    0x41, 0x58,                         // 51 pop  r8
    0x5A,                               // 53 pop  rdx
    0x59,                               // 54 pop  rcx
    0x48, 0x85, 0xC0,                   // 55 test rax, rax
    0x74, 0x02,                         // 58 jz   5C                 swallow: rax is already 0
    0xFF, 0xE0,                         // 5A jmp  rax                tail jump, our frame is gone
    0xC3,                               // 5C ret
};
constexpr size_t kImmOff = 0x29;
static_assert(sizeof(kThunk) == 0x5D, "thunk size");
static_assert(sizeof(kThunk) <= kUnwindOff, "thunk overlaps its unwind info");

// UNWIND_INFO for the prolog above, so a debugger or an exception can walk
// through a thunk while its pre-function is running. Codes are listed from the
// last prolog instruction back to the first. Pushing volatile registers is
// described with PUSH_NONVOL; the unwinder only needs to know the stack moved.
const uint8_t kUnwind[] = {
    0x01,       // version 1, no flags
    0x0A,       // size of prolog
    0x05,       // count of codes
    0x00,       // no frame register
    0x0A, 0xC2, // at 0A: UWOP_ALLOC_SMALL, (0x68 - 8) / 8 = 12
    0x06, 0x90, // at 06: UWOP_PUSH_NONVOL r9
    0x04, 0x80, // at 04: UWOP_PUSH_NONVOL r8
    0x02, 0x20, // at 02: UWOP_PUSH_NONVOL rdx
    0x01, 0x10, // at 01: UWOP_PUSH_NONVOL rcx
    0x00, 0x00, // padding to an even count
};
static_assert(kUnwindOff + sizeof(kUnwind) <= kFuncOff, "unwind info overlaps the table");

} // namespace

void* MakePreHookThunk(HookPreFn pre) {
    if (!pre) return nullptr;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const size_t pageSize = si.dwPageSize ? si.dwPageSize : 4096;

    uint8_t* page = (uint8_t*)VirtualAlloc(nullptr, pageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!page) return nullptr;

    memcpy(page + kCodeOff, kThunk, sizeof(kThunk));
    const uint64_t target = (uint64_t)(uintptr_t)pre;
    memcpy(page + kCodeOff + kImmOff, &target, 8);
    memcpy(page + kUnwindOff, kUnwind, sizeof(kUnwind));

    auto* rf = (RUNTIME_FUNCTION*)(page + kFuncOff);
    rf->BeginAddress = (DWORD)kCodeOff;
    rf->EndAddress = (DWORD)(kCodeOff + sizeof(kThunk));
    rf->UnwindData = (DWORD)kUnwindOff;

    DWORD old = 0;
    if (!VirtualProtect(page, pageSize, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), page, pageSize);

    // Not fatal if it fails: the thunk works the same, only a debugger would
    // lose the ability to walk through it.
    RtlAddFunctionTable(rf, 1, (DWORD64)(uintptr_t)page);
    return page + kCodeOff;
}

#pragma once
// Game hooks (signatures, pattern scan, Send/Add/OnReceive).
#include "pch.h"

using tChatFn = void(__fastcall*)(void* self, void* message);

extern tChatFn oSendMessage;
extern tChatFn oAddMessage;
extern tChatFn oOnReceive;

bool InstallChatHooks();
void UninstallChatHooks();

// Memory helpers used by chat processing as well
bool RegionReadable(const void* addr, size_t n);
bool IsPlausiblePtr(const void* p);
bool SafeRead(const void* src, void* dst, size_t n);

// Base/size of HytaleClient.exe's own module image. Used for chat-function
// pattern scanning and reused by the position pointer-chain finder (a hit
// inside this range is a stable, ASLR-relative chain root).
uintptr_t GetGameModuleBaseSize(size_t& outSize);

// IDA-style ("48 8B ? ? E8") pattern scan over HytaleClient.exe's image.
// Returns an absolute address, or 0. Shared with gamesdk.cpp, which uses it to
// find the per-frame function it detours to get hold of the game objects.
uintptr_t GameModulePatternScan(const char* idaPattern);

// Captured from the chat receive hook the first time it fires: where the client
// called it from, and the start of that calling function. Chat arrives over the
// network, so its caller is on the inbound packet path - which is the only
// handle we have on a build whose inbound signature does not match.
extern std::atomic<uintptr_t> g_recvCallerRva;
extern std::atomic<uintptr_t> g_recvFuncRva;
bool ReadSystemString(void* obj, std::string& out, uint64_t* outVT = nullptr);
void* CreateSystemString(const std::string& utf8);


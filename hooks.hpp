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
bool ReadSystemString(void* obj, std::string& out, uint64_t* outVT = nullptr);
void* CreateSystemString(const std::string& utf8);


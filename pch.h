#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <Windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <MinHook.h>

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <algorithm>
#include <thread>
#include <atomic>
#include <map>
#include <unordered_map>
#include <cctype>
#include <winhttp.h>
#include <fstream>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <set>
#include <deque>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winhttp.lib")

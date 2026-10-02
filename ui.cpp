#include "pch.h"
#include "ui.hpp"
#include "common.hpp"
#include "chat.hpp"
#include "settings.hpp"
#include "ai.hpp"
#include "extras.hpp"
#include "knowledge.hpp"
#include "translate.hpp"
#include "hooks.hpp"
#include "shared.hpp"
#include "position.hpp"
#include "gamesdk.hpp"
#include "packets.hpp"
#include "debugshape.hpp"
#include "prefab.hpp"
#include "buildsession.hpp"
#include "placewatch.hpp"
#include "selection.hpp"
#include "worldread.hpp"
#include "chatview.hpp"
#include "chatprefix.hpp"
#include "aiflow.hpp"
#include "aiprompt.hpp"
#include "autoreply.hpp"
#include "autofarm.hpp"


// Overlay D3D / window state
HWND g_hwndOverlay = nullptr;
ID3D11Device* g_pd3dDeviceOverlay = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContextOverlay = nullptr;
IDXGISwapChain* g_pSwapChainOverlay = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetViewOverlay = nullptr;

static std::atomic<bool> g_compactUi{ false }; // sole authority for compact RENDER + present

void ToggleCompactChatMode(); // defined below
bool IsCompactMode() { return g_compactUi.load(); }

// =============================================================================
// Compact chat — SEPARATE HWND that never has a DXGI swap chain
//
// Full UI keeps g_hwndOverlay + D3D. Compact is pure GDI + UpdateLayeredWindow.
// Features: scroll + auto-stick to latest, hover-only Full/scrollbar, resizable,
// remembers where you put it, right-click menu.
// =============================================================================

static HWND     g_hwndCompact = nullptr;
static HDC      g_cScreenDc = nullptr;
static HDC      g_cMemDc = nullptr;
static HBITMAP  g_cDib = nullptr;
static HGDIOBJ  g_cOrigBmp = nullptr;   // the DC's own bitmap, put back before ours is deleted
static void* g_cBits = nullptr;
static int      g_cDibW = 0, g_cDibH = 0;

// Scroll: 0 = stuck to bottom (latest). Positive = pixels scrolled up from bottom.
static int      g_cScrollFromBottom = 0;
static bool     g_cStickBottom = true;
static bool     g_cHovered = false;
static bool     g_cScrollbarDrag = false;
static int      g_cScrollbarDragOff = 0;

// Content metrics from last paint (for hit-testing / wheel)
static int      g_cContentH = 0;
static int      g_cViewH = 0;
static int      g_cViewW = 0;

// The compact window used to copy the entire chat log and create a GDI font on
// every paint - 60 times a second, whether or not anything had changed. It now
// keeps its own incrementally-updated copy of the log, caches the font, and
// skips painting when nothing it draws has changed.
static std::vector<ChatLine> g_cLines;
static uint64_t g_cLinesSeen = 0;
static HFONT    g_cFont = nullptr;
static std::wstring g_cFontFace;
static int      g_cFontPx = 0;
static std::string g_cLastPaintKey;

static void ReleaseCompactSurface() {
    if (g_cMemDc && g_cOrigBmp) { SelectObject(g_cMemDc, g_cOrigBmp); g_cOrigBmp = nullptr; }
    if (g_cDib) { DeleteObject(g_cDib); g_cDib = nullptr; }
    if (g_cMemDc) { DeleteDC(g_cMemDc); g_cMemDc = nullptr; }
    if (g_cScreenDc) { ReleaseDC(nullptr, g_cScreenDc); g_cScreenDc = nullptr; }
    if (g_cFont) { DeleteObject(g_cFont); g_cFont = nullptr; }
    g_cFontFace.clear();
    g_cFontPx = 0;
    g_cBits = nullptr;
    g_cDibW = g_cDibH = 0;
    g_cLastPaintKey.clear();
}

static bool EnsureCompactSurface(int w, int h) {
    if (w < 1 || h < 1) return false;
    if (g_cDib && g_cDibW == w && g_cDibH == h && g_cBits)
        return true;
    // A bitmap can't be deleted while a DC has it selected - that used to leak
    // one per resize. Swap the DC's original back in first.
    if (g_cMemDc && g_cOrigBmp) { SelectObject(g_cMemDc, g_cOrigBmp); g_cOrigBmp = nullptr; }
    if (g_cDib) { DeleteObject(g_cDib); g_cDib = nullptr; }
    g_cBits = nullptr;
    g_cLastPaintKey.clear();

    if (!g_cScreenDc) g_cScreenDc = GetDC(nullptr);
    if (!g_cScreenDc) return false;
    if (!g_cMemDc) g_cMemDc = CreateCompatibleDC(g_cScreenDc);
    if (!g_cMemDc) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_cDib = CreateDIBSection(g_cMemDc, &bmi, DIB_RGB_COLORS, &g_cBits, nullptr, 0);
    if (!g_cDib || !g_cBits) return false;
    g_cOrigBmp = SelectObject(g_cMemDc, g_cDib);
    g_cDibW = w;
    g_cDibH = h;
    return true;
}

static bool CompactIsHovered() {
    if (!g_hwndCompact || !IsWindow(g_hwndCompact)) return false;
    POINT pt{};
    GetCursorPos(&pt);
    RECT wr{};
    if (!GetWindowRect(g_hwndCompact, &wr)) return false;
    return PtInRect(&wr, pt) != FALSE;
}

// Forward
static void PaintCompactWindow(bool force = false);

static void ApplyCompactRegion(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const int d = (int)(g_prefs.compactRadius * 2.0f + 0.5f);
    if (d <= 1 || w < 8 || h < 8) { SetWindowRgn(hwnd, nullptr, TRUE); return; }
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, (std::min)(d, w), (std::min)(d, h));
    if (rgn) SetWindowRgn(hwnd, rgn, TRUE);
}

static void SaveCompactGeometry() {
    if (!g_hwndCompact || !IsWindowVisible(g_hwndCompact)) return;
    RECT wr{};
    if (!GetWindowRect(g_hwndCompact, &wr)) return;
    std::lock_guard lock(g_settingsMtx);
    g_prefs.compactX = wr.left;
    g_prefs.compactY = wr.top;
    g_prefs.compactW = wr.right - wr.left;
    g_prefs.compactH = wr.bottom - wr.top;
    g_settingsDirty = true;
}

// Where the full window is, so it comes back to the same place next launch.
static void SaveOverlayGeometry() {
    if (!g_hwndOverlay || g_compactUi.load() || !IsWindowVisible(g_hwndOverlay) ||
        IsIconic(g_hwndOverlay) || IsZoomed(g_hwndOverlay))
        return;
    RECT wr{};
    if (!GetWindowRect(g_hwndOverlay, &wr)) return;
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w < 480 || h < 320) return;
    std::lock_guard lock(g_settingsMtx);
    g_prefs.fullX = wr.left;
    g_prefs.fullY = wr.top;
    g_prefs.fullW = w;
    g_prefs.fullH = h;
    g_settingsDirty = true;
}

// A saved position is only used while a monitor still shows most of it - a
// window restored onto an unplugged screen is lost.
static bool RectOnScreen(int x, int y, int w, int h) {
    RECT r{ x, y, x + w, y + h };
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    if (!mon) return false;
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return false;
    RECT inter{};
    if (!IntersectRect(&inter, &r, &mi.rcWork)) return false;
    return (inter.right - inter.left) >= w / 3 && (inter.bottom - inter.top) >= 40;
}

static std::string CompactLineText(const ChatLine& L) {
    std::string s;
    if (g_prefs.copyTimestamps) {
        const std::string ts = FormatChatTime(L.unixMs);
        if (!ts.empty()) s = ts + " ";
    }
    s += L.plain;
    if (g_prefs.copyTranslations && L.txStatus == TxStatus::Done && !L.txText.empty())
        s += "  [" + LangBadge(L.outgoing ? L.txTo : L.txFrom) + "] " + L.txText;
    return s;
}

static void CompactContextMenu(HWND hWnd, POINT screenPt) {
    enum { CopyLast = 1, CopyVisible, OnTop, ShowPos, HideBg, Stamps, Full, Settings, PfxLevel, PfxRank, PfxTitle,
           AiSend, AiDiscard, AiRetry, AiModeAuto, AiModeAsk, AiModeDry, AiStudio, AiPresetBase = 100 };
    bool top, showPos, hideBg, stamps;
    { std::lock_guard lock(g_settingsMtx); top = g_alwaysOnTop; }
    showPos = g_prefs.compactShowPos;
    hideBg = g_prefs.compactHideBgIdle;
    stamps = g_prefs.compactTimestamps;

    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, CopyLast, L"Copy last message");
    AppendMenuW(m, MF_STRING, CopyVisible, L"Copy chat");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (top ? MF_CHECKED : 0), OnTop, L"Always on top");
    AppendMenuW(m, MF_STRING | (stamps ? MF_CHECKED : 0), Stamps, L"Timestamps");
    AppendMenuW(m, MF_STRING | (showPos ? MF_CHECKED : 0), ShowPos, L"Show coordinates");
    AppendMenuW(m, MF_STRING | (hideBg ? MF_CHECKED : 0), HideBg, L"Hide background when idle");
    // Tags in front of names: checked = shown. Finer control is on the settings page.
    const bool showLevel = g_prefs.compactShowPrestige || g_prefs.compactShowLevel;
    const bool showRank = g_prefs.compactRankMode == 0;
    const bool showTitle = g_prefs.compactShowTitle;
    if (HMENU pfx = CreatePopupMenu()) {
        AppendMenuW(pfx, MF_STRING | (showLevel ? MF_CHECKED : 0), PfxLevel, L"Level / prestige tag");
        AppendMenuW(pfx, MF_STRING | (showRank ? MF_CHECKED : 0), PfxRank, L"Rank tag");
        AppendMenuW(pfx, MF_STRING | (showTitle ? MF_CHECKED : 0), PfxTitle, L"Title");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)pfx, L"Show before names");
    }
    // The AI: send or drop what is waiting, the reply mode, and each prompt preset on or off.
    std::vector<AiPrompt::Item> presets;
    if (HMENU ai = CreatePopupMenu()) {
        const int waiting = AiFlow::WaitingCount();
        const int mode = g_prefs.aiReplyMode;
        AppendMenuW(ai, MF_STRING | (waiting ? 0 : MF_GRAYED), AiSend, L"Send / allow what is waiting");
        AppendMenuW(ai, MF_STRING | (waiting ? 0 : MF_GRAYED), AiDiscard, L"Discard / deny what is waiting");
        AppendMenuW(ai, MF_STRING, AiRetry, L"Ask again (newest reply)");
        AppendMenuW(ai, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(ai, MF_STRING | (mode == 0 ? MF_CHECKED : 0), AiModeAuto, L"Reply by itself");
        AppendMenuW(ai, MF_STRING | (mode == 1 ? MF_CHECKED : 0), AiModeAsk, L"Ask me first");
        AppendMenuW(ai, MF_STRING | (mode == 2 ? MF_CHECKED : 0), AiModeDry, L"Dry run (never send)");
        presets = AiPrompt::Items();
        if (!presets.empty()) AppendMenuW(ai, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < presets.size() && i < 40; ++i)
            AppendMenuW(ai, MF_STRING | (presets[i].enabled ? MF_CHECKED : 0), (UINT_PTR)(AiPresetBase + i),
                Utf8ToWide("Prompt: " + presets[i].name).c_str());
        AppendMenuW(ai, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(ai, MF_STRING, AiStudio, L"Open AI Studio...");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)ai, L"AI");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, Full, L"Open full window");
    AppendMenuW(m, MF_STRING, Settings, L"Compact chat settings...");

    // Needed for the menu to close when you click elsewhere.
    SetForegroundWindow(hWnd);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        screenPt.x, screenPt.y, 0, hWnd, nullptr);
    DestroyMenu(m);

    switch (cmd) {
    case CopyLast:
        for (auto it = g_cLines.rbegin(); it != g_cLines.rend(); ++it) {
            if (it->local) continue;
            ChatUi::SetClipboard(CompactLineText(*it));
            break;
        }
        break;
    case CopyVisible: {
        std::string all;
        const size_t start = g_cLines.size() > (size_t)g_compactMaxLines ? g_cLines.size() - (size_t)g_compactMaxLines : 0;
        for (size_t i = start; i < g_cLines.size(); ++i) {
            if (!all.empty()) all += '\n';
            all += CompactLineText(g_cLines[i]);
        }
        ChatUi::SetClipboard(all);
        break;
    }
    case OnTop: {
        std::lock_guard lock(g_settingsMtx);
        g_alwaysOnTop = !g_alwaysOnTop;
        g_settingsDirty = true;
        g_windowStyleDirty = true;
        break;
    }
    case Stamps: { std::lock_guard lock(g_settingsMtx); g_prefs.compactTimestamps = !stamps; MarkPrefsChanged(); break; }
    case ShowPos: { std::lock_guard lock(g_settingsMtx); g_prefs.compactShowPos = !showPos; MarkPrefsChanged(); break; }
    case HideBg: { std::lock_guard lock(g_settingsMtx); g_prefs.compactHideBgIdle = !hideBg; MarkPrefsChanged(); break; }
    case PfxLevel: {
        std::lock_guard lock(g_settingsMtx);
        g_prefs.compactShowPrestige = g_prefs.compactShowLevel = !showLevel;
        MarkPrefsChanged();
        break;
    }
    case PfxRank: { std::lock_guard lock(g_settingsMtx); g_prefs.compactRankMode = showRank ? 1 : 0; MarkPrefsChanged(); break; }
    case PfxTitle: { std::lock_guard lock(g_settingsMtx); g_prefs.compactShowTitle = !showTitle; MarkPrefsChanged(); break; }
    case Full: ToggleCompactChatMode(); break;
    case Settings: ToggleCompactChatMode(); UiOpenSettings("Compact chat"); break;
    case AiSend: AiFlow::ApproveOldest(); break;
    case AiDiscard: AiFlow::DiscardOldest(); break;
    case AiRetry: AiFlow::RetryNewest(); break;
    case AiModeAuto: case AiModeAsk: case AiModeDry: {
        std::lock_guard lock(g_settingsMtx);
        g_prefs.aiReplyMode = cmd == AiModeAuto ? 0 : cmd == AiModeAsk ? 1 : 2;
        MarkPrefsChanged();
        break;
    }
    case AiStudio: ToggleCompactChatMode(); UiOpenStudioPage(0); break;
    default:
        if (cmd >= AiPresetBase && (size_t)(cmd - AiPresetBase) < presets.size())
            AiPrompt::SetEnabled(presets[(size_t)(cmd - AiPresetBase)].name, !presets[(size_t)(cmd - AiPresetBase)].enabled);
        break;
    }
    PaintCompactWindow(true);
}

static LRESULT CALLBACK CompactWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const int kTopBar = 22;
    const int kFullW = 48;

    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            return 0; // Make client rect match window rect to fix hit-testing and clipping
        }
        break;

    case WM_GETMINMAXINFO: {
        // Prevent window from resizing smaller than the bounds handled by UpdateLayeredWindow
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 160;
        mmi->ptMinTrackSize.y = 100;
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt{ (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hWnd, &pt);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;

        const int kEdge = 8;
        const int kCorner = 18; // Extra thick grab area for diagonal corner resizing

        const bool left = pt.x < kEdge;
        const bool right = pt.x >= w - kEdge;
        const bool top = pt.y < kEdge;
        const bool bottom = pt.y >= h - kEdge;

        const bool cornerLeft = pt.x < kCorner;
        const bool cornerRight = pt.x >= w - kCorner;
        const bool cornerTop = pt.y < kCorner;
        const bool cornerBottom = pt.y >= h - kCorner;

        // Prioritize corners first for diagonal resizing
        if (cornerTop && cornerLeft)     return HTTOPLEFT;
        if (cornerTop && cornerRight)    return HTTOPRIGHT;
        if (cornerBottom && cornerLeft)  return HTBOTTOMLEFT;
        if (cornerBottom && cornerRight) return HTBOTTOMRIGHT;

        if (left)            return HTLEFT;
        if (right)           return HTRIGHT;
        if (top)             return HTTOP;
        if (bottom)          return HTBOTTOM;

        // Top bar drag (except Full button zone when hovered)
        if (pt.y >= 0 && pt.y < kTopBar) {
            if (g_cHovered && pt.x > w - kFullW - 8)
                return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int step = (delta > 0) ? 36 : -36;
        // Wheel up -> older messages (increase scroll-from-bottom)
        int maxScroll = (std::max)(0, g_cContentH - g_cViewH);
        g_cScrollFromBottom += step;
        if (g_cScrollFromBottom < 0) g_cScrollFromBottom = 0;
        if (g_cScrollFromBottom > maxScroll) g_cScrollFromBottom = maxScroll;
        g_cStickBottom = (g_cScrollFromBottom <= 0);
        PaintCompactWindow(true);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{ (short)LOWORD(lParam), (short)HIWORD(lParam) };
        RECT rc{};
        GetClientRect(hWnd, &rc);
        const int w = rc.right - rc.left;
        // Full button
        if (g_cHovered && pt.y >= 2 && pt.y < 20 && pt.x > w - kFullW - 8) {
            ToggleCompactChatMode();
            return 0;
        }
        // Scrollbar thumb drag
        if (g_cHovered && g_cContentH > g_cViewH) {
            const int sbW = 6;
            const int sbX = w - sbW - 3;
            const int topBar = kTopBar;
            const int trackH = (std::max)(1, g_cViewH);
            if (pt.x >= sbX && pt.y >= topBar) {
                int maxScroll = g_cContentH - g_cViewH;
                int thumbH = (std::max)(20, trackH * g_cViewH / g_cContentH);
                int travel = (std::max)(1, trackH - thumbH);
                int thumbY = topBar + (maxScroll > 0
                    ? (travel * (maxScroll - g_cScrollFromBottom) / maxScroll)
                    : 0);
                if (pt.y >= thumbY && pt.y < thumbY + thumbH) {
                    g_cScrollbarDrag = true;
                    g_cScrollbarDragOff = pt.y - thumbY;
                    SetCapture(hWnd);
                    return 0;
                }
                // Click on track: jump
                int rel = pt.y - topBar - thumbH / 2;
                if (rel < 0) rel = 0;
                if (rel > travel) rel = travel;
                g_cScrollFromBottom = maxScroll - (maxScroll * rel / travel);
                if (g_cScrollFromBottom < 0) g_cScrollFromBottom = 0;
                g_cStickBottom = (g_cScrollFromBottom <= 0);
                PaintCompactWindow(true);
                return 0;
            }
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (g_cScrollbarDrag && (wParam & MK_LBUTTON)) {
            POINT pt{ (short)LOWORD(lParam), (short)HIWORD(lParam) };
            int maxScroll = (std::max)(0, g_cContentH - g_cViewH);
            int trackH = (std::max)(1, g_cViewH);
            int thumbH = (std::max)(20, g_cContentH > 0 ? trackH * g_cViewH / g_cContentH : 20);
            int travel = (std::max)(1, trackH - thumbH);
            int rel = pt.y - kTopBar - g_cScrollbarDragOff;
            if (rel < 0) rel = 0;
            if (rel > travel) rel = travel;
            g_cScrollFromBottom = maxScroll - (maxScroll * rel / travel);
            if (g_cScrollFromBottom < 0) g_cScrollFromBottom = 0;
            g_cStickBottom = (g_cScrollFromBottom <= 0);
            PaintCompactWindow(true);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        if (g_cScrollbarDrag) {
            g_cScrollbarDrag = false;
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_RBUTTONUP: {
        POINT pt{ (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ClientToScreen(hWnd, &pt);
        CompactContextMenu(hWnd, pt);
        return 0;
    }
    case WM_NCRBUTTONUP: {
        // The top strip is a caption as far as Windows is concerned.
        POINT pt{ (short)LOWORD(lParam), (short)HIWORD(lParam) };
        CompactContextMenu(hWnd, pt);
        return 0;
    }
    case WM_EXITSIZEMOVE:
        SaveCompactGeometry();
        return 0;
    case WM_SIZE:
    {
        ApplyCompactRegion(hWnd);
        int maxScroll = (std::max)(0, g_cContentH - g_cViewH);
        if (g_cStickBottom) g_cScrollFromBottom = 0;
        else if (g_cScrollFromBottom > maxScroll) g_cScrollFromBottom = maxScroll;
        PaintCompactWindow(true);
    }
    return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (g_compactUi.load())
            ToggleCompactChatMode();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool EnsureCompactWindow() {
    if (g_hwndCompact && IsWindow(g_hwndCompact))
        return true;

    static bool s_reg;
    if (!s_reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CompactWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"HyAICompactOnlyClass";
        RegisterClassExW(&wc);
        s_reg = true;
    }

    // THICKFRAME enables system resize; we still map edges in WM_NCHITTEST
    g_hwndCompact = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_APPWINDOW,
        L"HyAICompactOnlyClass",
        L"HyAI Compact",
        WS_POPUP | WS_THICKFRAME,
        16, 100, 440, 210,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hwndCompact) {
        LogToFile("EnsureCompactWindow: CreateWindowEx failed");
        return false;
    }
    ShowWindow(g_hwndCompact, SW_HIDE);
    LogToFile("Compact-only HWND created (no DXGI)");
    return true;
}

static void DestroyCompactWindow() {
    ReleaseCompactSurface();
    if (g_hwndCompact) {
        DestroyWindow(g_hwndCompact);
        g_hwndCompact = nullptr;
    }
}

// :heart: / <3 / :) as emoji, for the compact window only (GDI can draw them).
static std::string CompactEmoji(const std::string& in) {
    struct EmojiSub { const char* key; const char* val; };
    static const EmojiSub universal[] = {
        {":heart:", "❤️"}, {":smile:", "😄"}, {":sob:", "😭"},
        {":joy:", "😂"}, {":thinking:", "🤔"}, {":fire:", "🔥"},
        {":star:", "⭐"}, {":thumbsup:", "👍"}, {":thumbsdown:", "👎"},
        {":skull:", "💀"}, {":poop:", "💩"}, {":clown:", "🤡"},
        {":rofl:", "🤣"}, {":wink:", "😉"}, {":stuck_out_tongue:", "😛"},
        {":sunglasses:", "😎"}, {":sweat_smile:", "😅"}, {":smirk:", "😏"},
        {":eyes:", "👀"}, {":check:", "✅"}, {":cross:", "❌"},
        {":sparkles:", "✨"}, {":100:", "💯"}, {":pray:", "🙏"},
        {":clap:", "👏"}, {":cry:", "😢"}, {":angry:", "😠"},
        {":rage:", "😡"}, {":rolling_eyes:", "🙄"}, {":upside_down:", "🙃"},
        {":zzz:", "💤"}, {":kiss:", "💋"}, {":broken_heart:", "💔"},
        {":sparkling_heart:", "💖"}
    };
    std::string utf8 = in;
    for (const auto& sub : universal) {
        const std::string key = sub.key, val = sub.val;
        size_t p = 0;
        while ((p = utf8.find(key, p)) != std::string::npos) {
            utf8.replace(p, key.length(), val);
            p += val.length();
        }
    }
    // Whole-word emoticons only, so "http://" and "a:b" survive.
    std::string out;
    size_t pos = 0;
    while (pos < utf8.size()) {
        size_t sp = utf8.find(' ', pos);
        const size_t end = sp == std::string::npos ? utf8.size() : sp;
        std::string word = utf8.substr(pos, end - pos);
        if (word == "<3") word = "❤️";
        else if (word == ":)") word = "🙂";
        else if (word == ":(") word = "🙁";
        else if (word == ":D") word = "😃";
        else if (word == ":P" || word == ":p") word = "😛";
        else if (word == ";)") word = "😉";
        else if (word == ">:(") word = "😡";
        else if (word == ":O" || word == ":o") word = "😮";
        out += word;
        if (sp != std::string::npos) out += ' ';
        pos = end + 1;
    }
    return out;
}

static void PaintCompactWindow(bool force) {
    if (!g_hwndCompact || !IsWindow(g_hwndCompact)) return;

    // IMPORTANT: Use the full window rect for ULW size.
    RECT wr{};
    if (!GetWindowRect(g_hwndCompact, &wr)) return;
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;
    if (w < 160) w = 160;
    if (h < 100) h = 100;
    if (!EnsureCompactSurface(w, h) || !g_cBits) return;

    float panelOp = 0.78f;
    float lineGap = 1.0f;
    bool topMost = false;
    float fontSize = 17.0f;
    int maxLines = 80;
    float padXf = 10.0f;
    {
        std::lock_guard lock(g_settingsMtx);
        panelOp = g_compactOpacity;
        lineGap = g_chatLineGap;
        topMost = g_alwaysOnTop;
        fontSize = g_compactFontSize;
        maxLines = g_compactMaxLines;
        padXf = g_compactPadX;
    }
    const ChatPrefs& P = g_prefs; // UI thread: the only writer, so reading unlocked is fine
    if (panelOp < 0.10f) panelOp = 0.10f;
    if (panelOp > 1.0f) panelOp = 1.0f;
    if (fontSize < 10.0f) fontSize = 10.0f;
    if (fontSize > 28.0f) fontSize = 28.0f;
    if (maxLines < 20) maxLines = 20;
    if (maxLines > 300) maxLines = 300;
    if (padXf < 4.0f) padXf = 4.0f;
    if (padXf > 32.0f) padXf = 32.0f;

    // Hover state (Full button + scrollbar only when hovered)
    g_cHovered = CompactIsHovered() || g_cScrollbarDrag;
    const bool idle = !g_cHovered;

    // Snapshot log + auto-stick when new lines arrive
    if (SnapshotChatLog(g_cLines, g_cLinesSeen) && g_cStickBottom)
        g_cScrollFromBottom = 0;

    wchar_t posBuf[192] = L"";
    if (P.compactShowPos && g_posLocked.load())
        swprintf_s(posBuf, L"X %.1f  Y %.1f  Z %.1f", g_posX.load(), g_posY.load(), g_posZ.load());
    // A reply waiting for approval, a tool question, or "thinking": the overlay is usually
    // hidden while playing, so this is where it shows.
    {
        const std::wstring ai = AiFlow::BannerText();
        if (!ai.empty()) {
            if (posBuf[0]) wcsncat_s(posBuf, L"   |   ", _TRUNCATE);
            wcsncat_s(posBuf, ai.c_str(), _TRUNCATE);
        }
    }
    // What the auto farm is up to, since the overlay is usually hidden while it runs.
    if (P.farmCompact) {
        const std::wstring farm = FarmCompactText();
        if (!farm.empty()) {
            if (posBuf[0]) wcsncat_s(posBuf, L"   |   ", _TRUNCATE);
            wcsncat_s(posBuf, farm.c_str(), _TRUNCATE);
        }
    }

    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const bool fading = idle && P.compactFadeSec > 0;

    // Everything the picture depends on. Same key as last time = same picture.
    {
        char key[640];
        snprintf(key, sizeof(key), "%llu|%u|%d|%d|%d|%d|%d|%d|%d|%d|%.2f|%.1f|%.1f|%ls|%lld",
            (unsigned long long)g_cLinesSeen, g_prefsVersion.load(), (int)g_cHovered,
            g_cScrollFromBottom, w, h, (int)topMost, maxLines, (int)g_cStickBottom, (int)g_cScrollbarDrag,
            panelOp, fontSize, padXf, posBuf, fading ? nowMs / 1000 : 0LL);
        if (!force && g_cLastPaintKey == key) return;
        g_cLastPaintKey = key;
    }

    const bool bgHidden = idle && P.compactHideBgIdle;
    const BYTE panelA = bgHidden ? 0 : (BYTE)(panelOp * 255.0f + 0.5f);
    auto to255 = [](float v) { return (BYTE)((std::max)(0.0f, (std::min)(1.0f, v)) * 255.0f + 0.5f); };
    const BYTE pR = to255(P.compactBg.x), pG = to255(P.compactBg.y), pB = to255(P.compactBg.z);
    const BYTE pr = (BYTE)((pR * panelA + 127) / 255);
    const BYTE pg = (BYTE)((pG * panelA + 127) / 255);
    const BYTE pb = (BYTE)((pB * panelA + 127) / 255);

    // Fill panel
    {
        BYTE* d = (BYTE*)g_cBits;
        const size_t n = (size_t)w * (size_t)h;
        for (size_t i = 0; i < n; ++i) {
            d[0] = pb; d[1] = pg; d[2] = pr; d[3] = panelA;
            d += 4;
        }
    }

    const int fontPx = (int)(fontSize + 0.5f);
    HDC dc = g_cMemDc;
    const std::wstring face = P.compactFont.empty() ? L"Segoe UI" : Utf8ToWide(P.compactFont);
    if (!g_cFont || face != g_cFontFace || fontPx != g_cFontPx) {
        if (g_cFont) DeleteObject(g_cFont);
        g_cFont = CreateFontW(
            -fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
        g_cFontFace = face;
        g_cFontPx = fontPx;
    }
    HGDIOBJ oldFont = SelectObject(dc, g_cFont ? (HGDIOBJ)g_cFont : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);

    // Top chrome stays fixed (matches CompactWndProc kTopBar hit-testing).
    // Text metrics scale with the compact font size setting.
    const int topBar = 22;
    const int padX = (int)(padXf + 0.5f);
    const int lineH = fontPx + 2;
    const int gap = (int)lineGap;
    const int sbW = 6;
    const int textRight = g_cHovered ? (w - sbW - 8) : (w - padX);

    auto rgbOf = [&](const ImVec4& c) { return RGB(to255(c.x), to255(c.y), to255(c.z)); };
    // Text outline colour: near-black but never exactly the panel colour, which
    // the alpha fix-up below would treat as background.
    const COLORREF shadowCol = RGB(6, 6, 9);

    std::set<std::string> ignoredLower;
    if (P.ignoredDisplay == 2) {
        std::lock_guard lock(g_mtx);
        ignoredLower = g_ignored;
    }
    const ChatPrefix::Options prefixOpts = ChatPrefix::FromPrefs(P);
    std::vector<ChatSeg> shownSegs; // scratch: a line's segments with the hidden tags cut out

    size_t startLine = 0;
    if ((int)g_cLines.size() > maxLines)
        startLine = g_cLines.size() - (size_t)maxLines;

    // --- Dynamic Layout Engine (Handles Emoji Replacement, Line Wrapping + Mid-Word Breaks) ---
    struct VisualSegment {
        std::wstring text;
        COLORREF color;
        int x;
        int yOffset;
    };
    struct VisualLineLayout {
        int height;
        bool mention;
        std::vector<VisualSegment> segments;
    };

    std::vector<VisualLineLayout> layouts;
    layouts.reserve(g_cLines.size() - startLine);

    int contentH = 0;

    for (size_t li = startLine; li < g_cLines.size(); ++li) {
        const ChatLine& line = g_cLines[li];
        if (ChatUi::LineVisibility(line, ignoredLower) == 2) continue;
        if (fading && line.unixMs > 0 && nowMs - line.unixMs > (long long)P.compactFadeSec * 1000) continue;

        VisualLineLayout layout;
        layout.mention = ChatUi::IsMention(line);
        int currentX = padX;
        int currentYOffset = 0;

        auto addText = [&](const std::string& utf8_in, COLORREF color) {
            const std::string utf8 = P.compactEmoji ? CompactEmoji(utf8_in) : utf8_in;
            if (utf8.empty()) return;
            size_t pos = 0;
            while (pos < utf8.length()) {
                size_t nextSpace = utf8.find(' ', pos);
                size_t end = (nextSpace != std::string::npos) ? nextSpace + 1 : utf8.length();
                const std::string word = utf8.substr(pos, end - pos);

                int wlen = MultiByteToWideChar(CP_UTF8, 0, word.c_str(), (int)word.length(), nullptr, 0);
                if (wlen > 0) {
                    std::wstring ws(wlen, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, word.c_str(), (int)word.length(), &ws[0], wlen);

                    int wsPos = 0;
                    while (wsPos < wlen) {
                        SIZE sz{};
                        GetTextExtentPoint32W(dc, ws.c_str() + wsPos, wlen - wsPos, &sz);

                        if (currentX + sz.cx <= textRight) {
                            // Fits on current line
                            layout.segments.push_back({ ws.substr(wsPos), color, currentX, currentYOffset });
                            currentX += sz.cx;
                            break;
                        }
                        // Does not fit on the current line. Drop to a new line if we aren't already there.
                        if (currentX > padX) {
                            currentX = padX;
                            currentYOffset += lineH;

                            // Check if it fits on this fresh line
                            if (currentX + sz.cx <= textRight) {
                                layout.segments.push_back({ ws.substr(wsPos), color, currentX, currentYOffset });
                                currentX += sz.cx;
                                break;
                            }
                        }

                        // If we are here, the word is massively long and must be split mid-character.
                        int maxExtent = textRight - currentX;
                        if (maxExtent <= 0) maxExtent = 1; // Failsafe

                        int fitCount = 0;
                        GetTextExtentExPointW(dc, ws.c_str() + wsPos, wlen - wsPos, maxExtent, &fitCount, nullptr, &sz);

                        if (fitCount == 0) {
                            fitCount = 1; // Force at least 1 character to avoid infinite loops
                            GetTextExtentPoint32W(dc, ws.c_str() + wsPos, fitCount, &sz);
                        }

                        // Commit the split chunk and jump to the next line
                        layout.segments.push_back({ ws.substr(wsPos, fitCount), color, currentX, currentYOffset });
                        currentX = padX;
                        currentYOffset += lineH;
                        wsPos += fitCount; // Advance the pointer to process the remainder of the word
                    }
                }
                pos = end;
            }
            };

        if (P.compactTimestamps) {
            std::string ts = FormatChatTime(line.unixMs);
            if (ts.empty()) ts = line.timeStr;
            if (!ts.empty()) {
                addText(ts, rgbOf(P.colTimestamp));
                currentX += 6; // Add small spacing after timestamp
            }
        }

        if (line.local) {
            for (const auto& seg : line.segs) if (!seg.text.empty()) addText(seg.text, rgbOf(seg.color));
        }
        else if (line.outgoing) {
            addText("> " + line.plain, rgbOf(P.colOutgoing));
        }
        else if (!line.segs.empty()) {
            const std::vector<ChatSeg>& segs =
                ChatPrefix::Simplify(line, prefixOpts, shownSegs) ? shownSegs : line.segs;
            for (const auto& seg : segs)
                if (!seg.text.empty()) addText(seg.text, rgbOf(ChatUi::DisplayColor(seg.color)));
        }
        else {
            addText(line.plain, rgbOf(P.colText));
        }

        // Translations too - the compact window used to leave them out.
        if (line.txStatus == TxStatus::Done && !line.txText.empty()) {
            currentX = padX + fontPx;
            currentYOffset += lineH;
            addText("[" + LangBadge(line.outgoing ? line.txTo : line.txFrom) + "] ", rgbOf(LangColor(line.outgoing ? line.txTo : line.txFrom)));
            addText(line.txText, rgbOf(P.colTranslation));
        }

        layout.height = currentYOffset + lineH;
        contentH += layout.height + gap;
        layouts.push_back(std::move(layout));
    }

    g_cContentH = contentH;
    g_cViewH = h - topBar - 4;
    g_cViewW = w;
    if (g_cViewH < 1) g_cViewH = 1;

    int maxScroll = (std::max)(0, g_cContentH - g_cViewH);
    if (g_cStickBottom)
        g_cScrollFromBottom = 0;
    if (g_cScrollFromBottom > maxScroll)
        g_cScrollFromBottom = maxScroll;
    if (g_cScrollFromBottom < 0)
        g_cScrollFromBottom = 0;

    // Short content sits at the bottom, like the game's own chat.
    const int contentTop = (g_cContentH < g_cViewH)
        ? (h - 4 - g_cContentH)
        : (topBar - maxScroll + g_cScrollFromBottom);

    // --- Execute layout drawing ---
    std::vector<std::pair<int, int>> mentionBars;
    int y = contentTop;
    for (size_t i = 0; i < layouts.size(); ++i) {
        const auto& layout = layouts[i];
        if (y > h) break;

        // Only draw if the combined block is within the visible window
        if (y + layout.height >= topBar) {
            for (const auto& seg : layout.segments) {
                int drawY = y + seg.yOffset;
                // Clip out visual segments scrolling out of the top/bottom
                if (drawY + lineH >= topBar && drawY <= h) {
                    if (P.compactShadow) {
                        SetTextColor(dc, shadowCol);
                        TextOutW(dc, seg.x + 1, drawY + 1, seg.text.c_str(), (int)seg.text.length());
                        TextOutW(dc, seg.x - 1, drawY, seg.text.c_str(), (int)seg.text.length());
                    }
                    SetTextColor(dc, seg.color);
                    TextOutW(dc, seg.x, drawY, seg.text.c_str(), (int)seg.text.length());
                }
            }
            if (layout.mention) mentionBars.push_back({ y, y + layout.height });
        }
        y += layout.height + gap;
    }

    // Live coordinates, if we've located them (see the Position tab in the
    // full window). Always visible, not just on hover — that's the point.
    if (posBuf[0]) {
        RECT posRc{ padX, 3, (g_cHovered ? w - 56 : w - 8), 20 };
        if (P.compactShadow) {
            SetTextColor(dc, shadowCol);
            RECT s = posRc; OffsetRect(&s, 1, 1);
            DrawTextW(dc, posBuf, -1, &s, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        SetTextColor(dc, RGB(150, 230, 160));
        DrawTextW(dc, posBuf, -1, &posRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // Hover-only: Full button
    if (g_cHovered) {
        SetTextColor(dc, RGB(210, 220, 240));
        RECT fullRc{ w - 52, 3, w - 8, 20 };
        DrawTextW(dc, L"Full", 4, &fullRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    // Mention bars down the left edge.
    {
        const ImVec4& mb = P.colBarMention;
        const BYTE r = to255(mb.x), g = to255(mb.y), b = to255(mb.z);
        for (const auto& bar : mentionBars) {
            for (int yy = (std::max)(topBar, bar.first); yy < (std::min)(h, bar.second); ++yy) {
                for (int xx = 2; xx < 5 && xx < w; ++xx) {
                    BYTE* p = (BYTE*)g_cBits + ((size_t)yy * w + xx) * 4;
                    p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
                }
            }
        }
    }

    // Hover-only: scrollbar
    if (g_cHovered && maxScroll > 0) {
        const int trackTop = topBar;
        const int trackH = g_cViewH;
        const int sbX = w - sbW - 3;
        int thumbH = (std::max)(20, trackH * g_cViewH / g_cContentH);
        int travel = (std::max)(1, trackH - thumbH);
        int thumbY = trackTop + (travel * (maxScroll - g_cScrollFromBottom) / maxScroll);

        // track
        for (int yy = trackTop; yy < trackTop + trackH; ++yy) {
            if (yy < 0 || yy >= h) continue;
            for (int xx = sbX; xx < sbX + sbW; ++xx) {
                if (xx < 0 || xx >= w) continue;
                BYTE* p = (BYTE*)g_cBits + (yy * w + xx) * 4;
                p[0] = (BYTE)((int)p[0] * 2 / 3);
                p[1] = (BYTE)((int)p[1] * 2 / 3);
                p[2] = (BYTE)((int)p[2] * 2 / 3);
            }
        }
        // thumb
        for (int yy = thumbY; yy < thumbY + thumbH && yy < h; ++yy) {
            if (yy < trackTop) continue;
            for (int xx = sbX; xx < sbX + sbW; ++xx) {
                if (xx < 0 || xx >= w) continue;
                BYTE* p = (BYTE*)g_cBits + (yy * w + xx) * 4;
                p[0] = 180; p[1] = 185; p[2] = 200; p[3] = 255;
            }
        }
    }

    SelectObject(dc, oldFont);

    // Alpha fixup: non-panel pixels -> fully opaque
    {
        BYTE* d = (BYTE*)g_cBits;
        const size_t n = (size_t)w * (size_t)h;
        for (size_t i = 0; i < n; ++i) {
            if (d[0] != pb || d[1] != pg || d[2] != pr) {
                d[3] = 255;
            }
            d += 4;
        }
    }

    POINT ptSrc{ 0, 0 };
    SIZE size{ w, h };
    POINT ptDst{ wr.left, wr.top };

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    SetWindowPos(g_hwndCompact, topMost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    if (!UpdateLayeredWindow(g_hwndCompact, g_cScreenDc, &ptDst, &size,
        g_cMemDc, &ptSrc, 0, &blend, ULW_ALPHA)) {
        static DWORD s_last = 0;
        DWORD e = GetLastError();
        if (e != s_last) {
            s_last = e;
            LogToFile(std::string("PaintCompactWindow ULW err=") + std::to_string(e));
        }
    }
}

static void ResizeSwapChainToClient() {
    if (!g_hwndOverlay || !g_pSwapChainOverlay || !g_pd3dDeviceOverlay) return;
    RECT crc{};
    if (!GetClientRect(g_hwndOverlay, &crc)) return;
    UINT w = (UINT)(crc.right - crc.left);
    UINT h = (UINT)(crc.bottom - crc.top);
    if (w < 1 || h < 1) return;

    if (g_mainRenderTargetViewOverlay) {
        g_mainRenderTargetViewOverlay->Release();
        g_mainRenderTargetViewOverlay = nullptr;
    }
    HRESULT hr = g_pSwapChainOverlay->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        LogToFile(std::string("ResizeBuffers failed hr=") + std::to_string((long long)hr));
        return;
    }
    ID3D11Texture2D* pBackBuffer = nullptr;
    hr = g_pSwapChainOverlay->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (SUCCEEDED(hr) && pBackBuffer) {
        g_pd3dDeviceOverlay->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetViewOverlay);
        pBackBuffer->Release();
    }
}

static void ApplyDwmCornerPreference(HWND hwnd, bool rounded) {
    if (!hwnd) return;
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (!hDwm) return;
    typedef HRESULT(WINAPI* FnSet)(HWND, DWORD, LPCVOID, DWORD);
    auto pSet = (FnSet)GetProcAddress(hDwm, "DwmSetWindowAttribute");
    if (pSet) {
        const DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
        int pref = rounded ? 2 : 1; // ROUND : DONOTROUND
        pSet(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    }
    FreeLibrary(hDwm);
}

void ApplyWindowStyle() {
    if (!g_hwndOverlay) return;
    bool top = false;
    float opacity = 1.0f;
    {
        std::lock_guard lock(g_settingsMtx);
        top = g_alwaysOnTop;
        opacity = g_windowOpacity;
    }
    // Main overlay is FULL UI only — always constant LWA_ALPHA (never ULW).
    SetWindowPos(g_hwndOverlay, top ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    LONG_PTR ex = GetWindowLongPtrW(g_hwndOverlay, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED))
        SetWindowLongPtrW(g_hwndOverlay, GWL_EXSTYLE, ex | WS_EX_LAYERED);

    BYTE alpha = (BYTE)(std::max)(30, (std::min)(255, (int)(opacity * 255.0f)));
    SetLayeredWindowAttributes(g_hwndOverlay, 0, alpha, LWA_ALPHA);

    // Keep compact HWND topmost in sync when it exists
    if (g_hwndCompact && IsWindow(g_hwndCompact) && g_compactUi.load()) {
        SetWindowPos(g_hwndCompact, top ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_cLastPaintKey.clear();
    }
}


static void EnterCompactChatMode() {
    if (!EnsureCompactWindow()) {
        LogToFile("EnterCompactChatMode: no compact HWND");
        return;
    }

    // Remember full-UI geometry from the main overlay
    SaveOverlayGeometry();

    int x, y, w, h;
    {
        std::lock_guard lock(g_settingsMtx);
        x = g_prefs.compactX;
        y = g_prefs.compactY;
        w = (std::max)(160, g_prefs.compactW);
        h = (std::max)(100, g_prefs.compactH);
    }
    if (x == -1 || y == -1 || !RectOnScreen(x, y, w, h)) {
        const int margin = 16;
        x = margin;
        y = GetSystemMetrics(SM_CYSCREEN) - h - 120;
        if (y < margin) y = margin;
    }

    g_compactUi.store(true);
    g_show = true;

    // Hide the DXGI overlay entirely — it must not paint while compact is up
    if (g_hwndOverlay)
        ShowWindow(g_hwndOverlay, SW_HIDE);

    bool top = false;
    { std::lock_guard lock(g_settingsMtx); top = g_alwaysOnTop; }

    SetWindowPos(g_hwndCompact, top ? HWND_TOPMOST : HWND_NOTOPMOST,
        x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ApplyCompactRegion(g_hwndCompact);

    g_cScrollFromBottom = 0;
    g_cStickBottom = true;
    PaintCompactWindow(true);
    LogToFile("Compact chat mode ON (separate HWND, no DXGI)");
}

static void LeaveCompactChatMode() {
    SaveCompactGeometry();
    g_compactUi.store(false);
    g_show = true;

    // Hide compact-only HWND
    if (g_hwndCompact && IsWindow(g_hwndCompact))
        ShowWindow(g_hwndCompact, SW_HIDE);

    // Restore main overlay where it was
    if (g_hwndOverlay) {
        int x, y, w, h;
        {
            std::lock_guard lock(g_settingsMtx);
            x = g_prefs.fullX; y = g_prefs.fullY; w = g_prefs.fullW; h = g_prefs.fullH;
        }
        if (!RectOnScreen(x, y, w, h)) { x = 100; y = 100; w = 920; h = 720; }
        SetWindowPos(g_hwndOverlay, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        ResizeSwapChainToClient();
        SetWindowRgn(g_hwndOverlay, nullptr, TRUE);
        ApplyDwmCornerPreference(g_hwndOverlay, false);
        ApplyWindowStyle();
        ShowWindow(g_hwndOverlay, SW_SHOW);
        SetForegroundWindow(g_hwndOverlay);
    }
    LogToFile("Compact chat mode OFF (main overlay restored)");
}

void ToggleCompactChatMode() {
    const bool next = !g_compactUi.load();
    {
        std::lock_guard lock(g_settingsMtx);
        g_compactChatMode = next;
        g_settingsDirty = true;
    }
    LogToFile(next ? "ToggleCompact -> ON" : "ToggleCompact -> OFF");
    if (next) EnterCompactChatMode();
    else LeaveCompactChatMode();
}

void ApplyTheme(int preset) {
    if (preset < 0) preset = 0;
    if (preset > 5) preset = 5;

    ImGuiStyle& style = ImGui::GetStyle();
    float rnd = g_uiRounding;
    style.WindowRounding = 0.0f;
    style.ChildRounding = rnd + 3.0f;
    style.FrameRounding = rnd + 1.0f;
    style.PopupRounding = rnd + 3.0f;
    style.ScrollbarRounding = rnd + 4.0f;
    style.GrabRounding = rnd + 1.0f;
    style.TabRounding = rnd + 2.0f;
    style.WindowPadding = ImVec2(14, 12);
    style.FramePadding = ImVec2(11, 7);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(7, 5);
    style.ScrollbarSize = 11.0f;
    style.GrabMinSize = 11.0f;
    style.IndentSpacing = 18.0f;
    style.SeparatorTextPadding = ImVec2(16, 6);

    ImVec4* c = style.Colors;

    auto setBase = [&](ImVec4 win, ImVec4 child, ImVec4 frame, ImVec4 border,
        ImVec4 btn, ImVec4 btnH, ImVec4 btnA,
        ImVec4 header, ImVec4 headerH, ImVec4 headerA,
        ImVec4 tab, ImVec4 tabH, ImVec4 tabA,
        ImVec4 accent, ImVec4 check, ImVec4 slider) {
            c[ImGuiCol_WindowBg] = win;
            c[ImGuiCol_ChildBg] = child;
            c[ImGuiCol_PopupBg] = child;
            c[ImGuiCol_Border] = border;
            c[ImGuiCol_FrameBg] = frame;
            c[ImGuiCol_FrameBgHovered] = ImVec4(frame.x + 0.05f, frame.y + 0.05f, frame.z + 0.07f, 1.f);
            c[ImGuiCol_FrameBgActive] = ImVec4(frame.x + 0.09f, frame.y + 0.09f, frame.z + 0.12f, 1.f);
            c[ImGuiCol_TitleBg] = win;
            c[ImGuiCol_TitleBgActive] = ImVec4(win.x + 0.03f, win.y + 0.04f, win.z + 0.06f, 1.f);
            c[ImGuiCol_TitleBgCollapsed] = ImVec4(win.x, win.y, win.z, 0.80f);
            c[ImGuiCol_MenuBarBg] = child;
            c[ImGuiCol_ScrollbarBg] = ImVec4(win.x, win.y, win.z, 0.40f);
            c[ImGuiCol_ScrollbarGrab] = ImVec4(accent.x * 0.50f, accent.y * 0.50f, accent.z * 0.50f, 0.70f);
            c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(accent.x * 0.72f, accent.y * 0.72f, accent.z * 0.72f, 0.90f);
            c[ImGuiCol_ScrollbarGrabActive] = accent;
            c[ImGuiCol_CheckMark] = check;
            c[ImGuiCol_SliderGrab] = slider;
            c[ImGuiCol_SliderGrabActive] = ImVec4(slider.x + 0.10f, slider.y + 0.10f, slider.z + 0.10f, 1.f);
            c[ImGuiCol_Button] = btn;
            c[ImGuiCol_ButtonHovered] = btnH;
            c[ImGuiCol_ButtonActive] = btnA;
            c[ImGuiCol_Header] = header;
            c[ImGuiCol_HeaderHovered] = headerH;
            c[ImGuiCol_HeaderActive] = headerA;
            c[ImGuiCol_Separator] = ImVec4(border.x, border.y, border.z, border.w * 0.7f);
            c[ImGuiCol_Tab] = tab;
            c[ImGuiCol_TabHovered] = tabH;
            c[ImGuiCol_TabActive] = tabA;
            c[ImGuiCol_TabUnfocused] = tab;
            c[ImGuiCol_TabUnfocusedActive] = tabA;
            c[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.97f, 1.00f);
            c[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.50f, 0.56f, 1.00f);
            c[ImGuiCol_ResizeGrip] = ImVec4(accent.x, accent.y, accent.z, 0.25f);
            c[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
            c[ImGuiCol_ResizeGripActive] = ImVec4(accent.x, accent.y, accent.z, 0.80f);
        };

    switch (preset) {
    case 1: // Purple
        setBase(
            ImVec4(0.08f, 0.06f, 0.12f, 1.f),
            ImVec4(0.11f, 0.08f, 0.16f, 1.f),
            ImVec4(0.16f, 0.12f, 0.24f, 1.f),
            ImVec4(0.45f, 0.30f, 0.65f, 0.55f),
            ImVec4(0.28f, 0.18f, 0.42f, 1.f),
            ImVec4(0.42f, 0.28f, 0.62f, 1.f),
            ImVec4(0.55f, 0.35f, 0.80f, 1.f),
            ImVec4(0.30f, 0.20f, 0.45f, 0.85f),
            ImVec4(0.45f, 0.30f, 0.65f, 0.95f),
            ImVec4(0.55f, 0.38f, 0.78f, 1.f),
            ImVec4(0.18f, 0.12f, 0.28f, 1.f),
            ImVec4(0.45f, 0.30f, 0.70f, 0.95f),
            ImVec4(0.38f, 0.25f, 0.58f, 1.f),
            ImVec4(0.70f, 0.45f, 1.00f, 1.f),
            ImVec4(0.80f, 0.55f, 1.00f, 1.f),
            ImVec4(0.65f, 0.40f, 0.95f, 0.95f)
        );
        break;
    case 2: // Green
        setBase(
            ImVec4(0.05f, 0.09f, 0.07f, 1.f),
            ImVec4(0.07f, 0.12f, 0.10f, 1.f),
            ImVec4(0.10f, 0.18f, 0.14f, 1.f),
            ImVec4(0.25f, 0.55f, 0.35f, 0.50f),
            ImVec4(0.12f, 0.28f, 0.18f, 1.f),
            ImVec4(0.18f, 0.45f, 0.28f, 1.f),
            ImVec4(0.22f, 0.55f, 0.35f, 1.f),
            ImVec4(0.14f, 0.32f, 0.20f, 0.85f),
            ImVec4(0.20f, 0.48f, 0.30f, 0.95f),
            ImVec4(0.25f, 0.58f, 0.38f, 1.f),
            ImVec4(0.10f, 0.20f, 0.14f, 1.f),
            ImVec4(0.20f, 0.50f, 0.32f, 0.95f),
            ImVec4(0.16f, 0.40f, 0.25f, 1.f),
            ImVec4(0.35f, 0.90f, 0.55f, 1.f),
            ImVec4(0.40f, 0.95f, 0.60f, 1.f),
            ImVec4(0.30f, 0.80f, 0.50f, 0.95f)
        );
        break;
    case 3: // Blue
        setBase(
            ImVec4(0.05f, 0.07f, 0.12f, 1.f),
            ImVec4(0.07f, 0.10f, 0.16f, 1.f),
            ImVec4(0.10f, 0.14f, 0.24f, 1.f),
            ImVec4(0.25f, 0.40f, 0.70f, 0.50f),
            ImVec4(0.12f, 0.20f, 0.38f, 1.f),
            ImVec4(0.20f, 0.35f, 0.65f, 1.f),
            ImVec4(0.28f, 0.48f, 0.85f, 1.f),
            ImVec4(0.14f, 0.24f, 0.42f, 0.85f),
            ImVec4(0.22f, 0.38f, 0.65f, 0.95f),
            ImVec4(0.28f, 0.48f, 0.80f, 1.f),
            ImVec4(0.10f, 0.14f, 0.24f, 1.f),
            ImVec4(0.25f, 0.42f, 0.75f, 0.95f),
            ImVec4(0.18f, 0.32f, 0.58f, 1.f),
            ImVec4(0.40f, 0.70f, 1.00f, 1.f),
            ImVec4(0.45f, 0.85f, 1.00f, 1.f),
            ImVec4(0.40f, 0.65f, 0.95f, 0.95f)
        );
        break;
    case 4: // Crimson
        setBase(
            ImVec4(0.10f, 0.05f, 0.06f, 1.f),
            ImVec4(0.14f, 0.07f, 0.08f, 1.f),
            ImVec4(0.22f, 0.10f, 0.12f, 1.f),
            ImVec4(0.65f, 0.25f, 0.30f, 0.55f),
            ImVec4(0.38f, 0.14f, 0.16f, 1.f),
            ImVec4(0.55f, 0.22f, 0.25f, 1.f),
            ImVec4(0.72f, 0.28f, 0.32f, 1.f),
            ImVec4(0.40f, 0.15f, 0.18f, 0.85f),
            ImVec4(0.58f, 0.22f, 0.26f, 0.95f),
            ImVec4(0.70f, 0.28f, 0.32f, 1.f),
            ImVec4(0.20f, 0.10f, 0.12f, 1.f),
            ImVec4(0.60f, 0.25f, 0.30f, 0.95f),
            ImVec4(0.48f, 0.18f, 0.22f, 1.f),
            ImVec4(0.95f, 0.35f, 0.40f, 1.f),
            ImVec4(1.00f, 0.45f, 0.50f, 1.f),
            ImVec4(0.90f, 0.35f, 0.40f, 0.95f)
        );
        break;
    case 5: { // Custom — full palette derived from a single accent color you pick
        float ar = g_customAccent[0], ag = g_customAccent[1], ab = g_customAccent[2];
        float h = 0.f, s = 0.f, v = 0.f;
        ImGui::ColorConvertRGBtoHSV(ar, ag, ab, h, s, v);
        if (s < 0.15f) s = 0.55f; // keep near-white/near-black picks from washing the theme out
        if (v < 0.35f) v = 0.85f;

        auto hsv = [&](float hh, float ss, float vv, float a) -> ImVec4 {
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(hh, ss, vv, r, g, b);
            return ImVec4(r, g, b, a);
            };

        const float bgV = g_customAccentDark ? 0.05f : 0.12f;
        const ImVec4 accentCol = hsv(h, s, v, 1.0f);

        setBase(
            hsv(h, (std::min)(s, 0.35f), bgV, 1.f),                  // window
            hsv(h, (std::min)(s, 0.35f), bgV + 0.03f, 1.f),          // child/popup
            hsv(h, (std::min)(s, 0.45f), bgV + 0.09f, 1.f),          // frame
            hsv(h, s, v, 0.55f),                                     // border
            hsv(h, s, v * 0.55f, 1.f),                               // button
            hsv(h, s, v * 0.80f, 1.f),                               // button hover
            accentCol,                                               // button active
            hsv(h, s, v * 0.45f, 0.85f),                             // header
            hsv(h, s, v * 0.70f, 0.95f),                             // header hover
            accentCol,                                               // header active
            hsv(h, (std::min)(s, 0.35f), bgV + 0.05f, 1.f),          // tab
            hsv(h, s, v * 0.70f, 0.95f),                             // tab hover
            hsv(h, s, v * 0.60f, 1.f),                               // tab active
            accentCol,                                               // scrollbar/misc accent
            accentCol,                                               // checkmark
            hsv(h, s, v * 0.85f, 0.95f)                              // slider grab
        );
        break;
    }
    default: // 0 — Default (refined slate)
        setBase(
            ImVec4(0.065f, 0.068f, 0.095f, 1.f),
            ImVec4(0.085f, 0.090f, 0.125f, 1.f),
            ImVec4(0.125f, 0.135f, 0.185f, 1.f),
            ImVec4(0.28f, 0.34f, 0.50f, 0.45f),
            ImVec4(0.16f, 0.20f, 0.32f, 1.f),
            ImVec4(0.26f, 0.38f, 0.60f, 1.f),
            ImVec4(0.34f, 0.52f, 0.82f, 1.f),
            ImVec4(0.18f, 0.23f, 0.38f, 0.80f),
            ImVec4(0.28f, 0.38f, 0.60f, 0.90f),
            ImVec4(0.34f, 0.48f, 0.78f, 1.f),
            ImVec4(0.12f, 0.14f, 0.22f, 1.f),
            ImVec4(0.28f, 0.42f, 0.70f, 0.90f),
            ImVec4(0.22f, 0.36f, 0.62f, 1.f),
            ImVec4(0.48f, 0.62f, 0.90f, 1.f),
            ImVec4(0.42f, 0.88f, 0.82f, 1.f),
            ImVec4(0.48f, 0.72f, 1.00f, 0.92f)
        );
        break;
    }

    g_appliedTheme = preset;
    g_themeDirty = false;
}




// =============================================================================
// Object-graph position (gamesdk.cpp) — UI
// =============================================================================
// The permanent way to get coordinates: a per-frame detour hands us the live
// GameInstance, and a short chain of field offsets from it reaches the player's
// position. Nothing here scans the heap; the only scan on offer is the App
// fallback, and that only matters if no frame signature matched the build.
static void DrawObjectGraphSection() {
    const bool hooked = g_gsdkHookInstalled.load();
    const bool rootLive = GsdkRootLive();
    const uintptr_t root = GsdkCurrentRoot();

    GsdkPath path;
    { std::lock_guard lock(g_gsdkPathMtx); path = g_gsdkPath; }

    ImGui::TextColored(ImVec4(0.60f, 0.90f, 0.60f, 1.0f),
        "Automatic: read it out of the game's own objects");
    ImGui::TextDisabled(
        "We detour a method the client runs every frame purely to note the\n"
        "GameInstance it was called on, then walk a fixed chain of field offsets\n"
        "from there to your player entity. Because the chain starts at a live\n"
        "object instead of a raw address, it survives restarts, ASLR and the GC -\n"
        "so once it is set up there is nothing to redo on later launches.");
    ImGui::Spacing();

    // ---- step 1: the root ----
    ImGui::Text("1. Game root");
    ImGui::Indent(12.0f);
    if (hooked) {
        std::string hookName;
        { std::lock_guard lock(g_gsdkHookNameMtx); hookName = g_gsdkHookName; }
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "frame hook active");
        ImGui::SameLine();
        ImGui::TextDisabled("(%s at +0x%llX)", hookName.c_str(),
            (unsigned long long)g_gsdkHookRva.load());
    }
    else {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
            "no frame-hook signature matched this client build");
    }

    if (rootLive) {
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
            "GameInstance live @ 0x%llX", (unsigned long long)root);
    }
    else if (root) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "using App fallback root @ 0x%llX", (unsigned long long)root);
    }
    else {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "no live root - join a world (the hook only fires while rendering)");
    }

    if (!hooked || !rootLive) {
        const bool scanning = g_gsdkAppScanning.load();
        ImGui::BeginDisabled(scanning);
        if (ImGui::Button("Find App object (one background scan)"))
            StartWorker([]() { GsdkFindApp(); });
        ImGui::EndDisabled();
        if (scanning) {
            ImGui::SameLine();
            ImGui::TextDisabled("scanning...");
        }
        else if (g_gsdkApp.load()) {
            ImGui::SameLine();
            ImGui::TextDisabled("App @ 0x%llX (%lld ms)",
                (unsigned long long)g_gsdkApp.load(), (long long)g_gsdkAppScanMs.load());
        }
        ImGui::TextDisabled(
            "Only needed when the frame hook can't bind: finds Application.App by\n"
            "its MethodTable, which gives the same object graph a slower way in.");
    }
    ImGui::Unindent(12.0f);
    ImGui::Spacing();

    // ---- step 2: the chain ----
    ImGui::Text("2. Path to your position");
    ImGui::Indent(12.0f);
    if (path.found) {
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "%s", path.ToString().c_str());
        const uintptr_t addr = GsdkResolvePath(path, root);
        double x, y, z;
        if (addr && GsdkReadTriple(addr, path.type, x, y, z)) {
            ImGui::Text("X: %.2f   Y: %.2f   Z: %.2f", x, y, z);
            ImGui::TextDisabled("resolves to 0x%llX", (unsigned long long)addr);
        }
        else {
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "path known but not resolving right now (not in a world?)");
        }
    }
    else {
        ImGui::TextDisabled("no path yet - use one of the three below");
    }
    ImGui::Unindent(12.0f);
    ImGui::Spacing();

    // ---- step 3: how to get the chain ----
    const bool busy = g_gsdkDiscovering.load();
    ImGui::Text("3. Set it up (once)");
    ImGui::Indent(12.0f);

    // --- fully automatic: we read Hytale's own F7 line ourselves ---
    ImGui::TextDisabled(
        "Hytale's F7 overlay already prints your exact position, and that line is\n"
        "an ordinary string in memory - so there is no need to retype it. Turn F7's\n"
        "overlay on and this sets itself up, on this launch and every one after,\n"
        "including after a client patch moves the fields again.");

    bool autoSetup = g_gsdkAutoSetupEnabled.load();
    if (ImGui::Checkbox("Set up automatically from the F7 overlay", &autoSetup))
        GsdkAutoSetupSetEnabled(autoSetup);

    ImGui::BeginDisabled(busy || !root);
    if (ImGui::Button("Set up now from F7"))
        GsdkAutoSetupFromDebugLine();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("needs F7's overlay visible in-game");

    ImGui::Spacing();

    // --- manual fallback: type the numbers in yourself ---
    if (ImGui::CollapsingHeader("Type the coordinates in by hand instead")) {
    ImGui::TextDisabled(
        "Same search, but you read the \"Feet (x, y, z)\" numbers off the overlay\n"
        "and enter them. Useful if the automatic read can't find the line.");

    static float wantCoords[3] = { 0.0f, 0.0f, 0.0f };
    static float wantTol = 2.0f;
    ImGui::SetNextItemWidth(260);
    ImGui::InputFloat3("Feet (x, y, z) from F7", wantCoords, "%.2f");
    ImGui::SetNextItemWidth(110);
    ImGui::InputFloat("tolerance", &wantTol, 0.5f, 2.0f, "%.1f");
    if (wantTol < 0.05f) wantTol = 0.05f;
    if (wantTol > 50.0f) wantTol = 50.0f;

    const bool coordsSet = !(wantCoords[0] == 0.0f && wantCoords[1] == 0.0f &&
                             wantCoords[2] == 0.0f);
    ImGui::BeginDisabled(busy || !root || !coordsSet);
    if (ImGui::Button("Find from F7 coords"))
        GsdkFindFromCoords(wantCoords[0], wantCoords[1], wantCoords[2], wantTol);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!coordsSet)
        ImGui::TextDisabled("type the three numbers above first");
    else
        ImGui::TextDisabled("stand still while it searches");

    } // Type the coordinates in by hand instead

    ImGui::Spacing();
    ImGui::TextDisabled("Other ways in:");

    ImGui::BeginDisabled(busy || !root);
    if (ImGui::Button("Try published offsets"))
        GsdkTryKnownOffsets();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("instant; works only if your build matches the public dumps");

    const bool haveLocked = g_posLocked.load() && g_posLockedAddr.load() != 0;
    ImGui::BeginDisabled(busy || !root || !haveLocked);
    if (ImGui::Button("Learn from the locked address"))
        GsdkLearnFromAddress(g_posLockedAddr.load());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (haveLocked)
        ImGui::TextDisabled("turns 0x%llX into a permanent path",
            (unsigned long long)g_posLockedAddr.load());
    else
        ImGui::TextDisabled("lock an address with the manual scan below first");

    ImGui::BeginDisabled(busy || !root);
    if (ImGui::Button("Auto-find (then walk around for ~8s)"))
        GsdkAutoFind();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("watches which value in the graph tracks your movement");

    if (path.found) {
        if (ImGui::Button("Forget path"))
            GsdkForget();
    }
    ImGui::Unindent(12.0f);

    // ---- progress / status ----
    if (busy) {
        const int phase = g_gsdkDiscoverPhase.load();
        ImGui::Spacing();
        if (phase == 1)
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "walking the object graph... %llu objects",
                (unsigned long long)g_gsdkDiscoverVisited.load());
        else
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "sampling - keep walking! %llu candidates",
                (unsigned long long)g_gsdkDiscoverCandidates.load());
    }

    const std::string status = GsdkGetStatus();
    if (!status.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", status.c_str());
    }
    if (g_gsdkLastWalkTruncated.load()) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "note: the last walk hit its candidate budget and did not cover everything");
    }

    // ---- auto-find survivors ----
    std::vector<GsdkCandidate> cands;
    { std::lock_guard lock(g_gsdkCandMtx); cands = g_gsdkCandidates; }
    if (cands.size() > 1) {
        ImGui::Spacing();
        ImGui::TextDisabled(
            "More than one value matched. The top one is already in use; if its\n"
            "numbers drift away from F7 as you move, click a different one:");
        ImGui::BeginChild("##gsdkcands", ImVec2(0, 150), true);
        for (size_t i = 0; i < cands.size(); ++i) {
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("Use this"))
                GsdkAdoptCandidate(cands[i].path);
            ImGui::SameLine();
            ImGui::Text("%.2f, %.2f, %.2f", cands[i].x, cands[i].y, cands[i].z);
            ImGui::SameLine();
            ImGui::TextDisabled("  %s  (moved %.0f)",
                cands[i].path.ToString().c_str(), cands[i].movement);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
}

// =============================================================================
// Player tab — everything we can read about the local player entity
// =============================================================================
static void DrawPlayerTab() {
    const GsdkPlayerInfo info = GsdkGetPlayerInfo();

    ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Local player");
    ImGui::TextDisabled(
        "Read live from the player entity the object-graph path lands on (see the\n"
        "Position tab). Coordinates are always right once the path is set up; the\n"
        "other fields use published offsets and are simply hidden when they don't\n"
        "read back as something sensible, so a client patch degrades this to\n"
        "coordinates rather than to nonsense.");
    ImGui::Separator();

    if (!info.valid) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "Nothing resolved yet - set up the path on the Position tab and join a world.");
        return;
    }

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "Position");
    ImGui::Indent(12.0f);
    ImGui::Text("X %.3f    Y %.3f    Z %.3f", info.x, info.y, info.z);
    ImGui::Text("Block  %d, %d, %d",
        (int)std::floor(info.x), (int)std::floor(info.y), (int)std::floor(info.z));
    ImGui::Text("Chunk  %d, %d, %d", info.chunkX, info.chunkY, info.chunkZ);
    if (ImGui::SmallButton("Copy coords")) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", info.x, info.y, info.z);
        ImGui::SetClipboardText(buf);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy as /tp")) {
        char buf[96];
        snprintf(buf, sizeof(buf), "/tp %.2f %.2f %.2f", info.x, info.y, info.z);
        ImGui::SetClipboardText(buf);
    }
    ImGui::Unindent(12.0f);

    // ---- facing ----
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "Facing");
    ImGui::Indent(12.0f);
    {
        GsdkFacingPath fp;
        { std::lock_guard lock(g_gsdkFacingMtx); fp = g_gsdkFacing; }
        const bool busy = g_gsdkDiscovering.load();

        // While the search is running it drives you through its phases; that
        // instruction is the whole reason the search works, so it gets the
        // loudest spot on the tab.
        const std::string prompt = GsdkGetFacingPrompt();
        if (!prompt.empty()) {
            ImGui::TextColored(ImVec4(1.00f, 0.85f, 0.30f, 1.0f), "%s", prompt.c_str());
            ImGui::Spacing();
        }

        if (info.haveRotation) {
            ImGui::Text("bearing %.1f°   pitch %.1f°", info.yaw, info.pitch);
            ImGui::Text("%s", info.facing.c_str());
            ImGui::TextDisabled("raw yaw %.2f   %s", info.rawYaw, fp.ToString().c_str());
            if (!fp.eastCalibrated)
                ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                    "Check east before trusting this. North and south read correctly even\n"
                    "when the compass is still wrong, because getting the direction of\n"
                    "count backwards swaps east and west and leaves the other two alone.");
            else
                ImGui::TextDisabled("yaw counts %s",
                    fp.yawSign > 0 ? "clockwise" : "anti-clockwise");
        }
        else if (fp.found) {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
                "The fields we picked have stopped reading as angles.");
            ImGui::TextDisabled("%s", fp.ToString().c_str());
            ImGui::TextDisabled(
                "Either pick a different candidate below, or run the search again.");
        }
        else {
            ImGui::TextDisabled(
                "Not found yet. The published rotation offset is wrong for this build,\n"
                "same as the position one was, so it gets discovered instead.\n"
                "\n"
                "The search runs in three parts and tells you what to do as it goes:\n"
                "  1. hold still, hands off the mouse (3s)  - a real angle is frozen\n"
                "  2. spin a full circle, look right up and right down (8s)\n"
                "  3. hold still again (2s)\n"
                "Anything that drifts during 1 or 3 is thrown out, which is what stops\n"
                "it latching onto a counter that merely looks busy.");
        }

        ImGui::BeginDisabled(busy);
        if (ImGui::Button(fp.found ? "Find facing again" : "Find facing (~13s, follow the prompts)"))
            GsdkFindFacing();
        ImGui::EndDisabled();

        if (fp.found) {
            ImGui::SameLine();
            if (ImGui::Button("Forget facing"))
                GsdkForgetFacing();

            ImGui::Spacing();
            ImGui::TextDisabled(
                "Compass calibration - two points, because the raw number alone says\n"
                "neither where the client's zero points nor which way it counts up.\n"
                "Face each direction on the in-game compass, then click:");
            ImGui::BeginDisabled(busy || !info.haveRotation);
            if (ImGui::Button("Set north"))
                GsdkCalibrateNorth();
            ImGui::SameLine();
            if (ImGui::Button("Set east"))
                GsdkCalibrateEast();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (fp.eastCalibrated)
                ImGui::TextDisabled("calibrated");
            else
                ImGui::TextDisabled("north first, then east");

            ImGui::BeginDisabled(busy || !info.haveRotation);
            if (ImGui::Button("East/West are swapped - flip them"))
                GsdkFlipYawDirection();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("same job as \"Set east\", without going anywhere");
        }

        // Alternatives, when the top-scoring pair turns out to be the wrong one.
        std::vector<GsdkFacingCandidate> fcands;
        { std::lock_guard lock(g_gsdkFacingCandMtx); fcands = g_gsdkFacingCandidates; }
        if (fcands.size() > 1) {
            ImGui::Spacing();
            ImGui::TextDisabled("Other pairs that followed your view - live values, so "
                "turn on the spot and click whichever one tracks you:");
            ImGui::BeginChild("##facingcands", ImVec2(0, 130), true);
            for (size_t i = 0; i < fcands.size(); ++i) {
                ImGui::PushID((int)i);
                if (ImGui::SmallButton("Use this"))
                    GsdkAdoptFacing(fcands[i].path);
                ImGui::SameLine();

                // Read each candidate live rather than showing what it was
                // during the search - that is what makes picking possible.
                float py = 0, pp = 0;
                const bool ok = GsdkReadFacingPath(fcands[i].path, py, pp);

                if (ok)
                    ImGui::Text("yaw %7.1f  pitch %6.1f", py, pp);
                else
                    ImGui::TextDisabled("unreadable");
                ImGui::SameLine();
                ImGui::TextDisabled("  yaw@+0x%X pitch@+0x%X  gap %u",
                    fcands[i].path.yawOffset, fcands[i].path.pitchOffset, fcands[i].gap);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    }
    ImGui::Unindent(12.0f);

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "Entity");
    ImGui::Indent(12.0f);
    ImGui::Text("object      0x%llX", (unsigned long long)info.entity);
    ImGui::Text("GameInstance 0x%llX", (unsigned long long)info.gameInstance);
    if (!info.playerName.empty())
        ImGui::Text("name        %s", info.playerName.c_str());
    if (info.haveNetworkId)
        ImGui::Text("network id  %d", info.networkId);
    if (info.haveFov)
        ImGui::Text("fov         %.1f", info.fov);
    if (info.haveBox)
        ImGui::Text("hitbox      %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f",
            info.boxMinX, info.boxMinY, info.boxMinZ,
            info.boxMaxX, info.boxMaxY, info.boxMaxZ);
    ImGui::Unindent(12.0f);

    // ---- raw slot dump ----
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Raw object dump")) {
        ImGui::TextDisabled(
            "Every 8-byte slot of the entity, shown as a pointer, two floats, two\n"
            "ints and a double at once. This is how the published offsets were\n"
            "worked out in the first place - if a field moved in a patch, you can\n"
            "spot the new home of a value you recognise here.");

        static int startSlot = 0;
        static int slotCount = 160;
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("start (bytes)", &startSlot, 8, 64);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("slots", &slotCount, 16, 64);
        if (startSlot < 0) startSlot = 0;
        if (slotCount < 8) slotCount = 8;
        if (slotCount > 1024) slotCount = 1024;

        const std::vector<GsdkSlot> slots =
            GsdkDumpObject(info.entity, (uint32_t)(startSlot & ~7), (uint32_t)slotCount);

        ImGui::BeginChild("##slotdump", ImVec2(0, 320), true,
            ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& sl : slots) {
            ImGui::Text("+0x%04X", sl.offset);
            ImGui::SameLine(80);
            ImGui::Text("%016llX", (unsigned long long)sl.raw);
            ImGui::SameLine(230);
            if (sl.isPointer) {
                ImGui::TextColored(ImVec4(0.60f, 0.78f, 1.00f, 1.0f), "ptr");
                ImGui::SameLine(275);
                if (!sl.text.empty())
                    ImGui::TextColored(ImVec4(0.85f, 0.90f, 0.60f, 1.0f), "\"%s\"", sl.text.c_str());
                else
                    ImGui::TextDisabled("MT %llX", (unsigned long long)sl.targetMT);
            }
            else {
                ImGui::TextDisabled("f %.3f, %.3f", sl.f0, sl.f1);
                ImGui::SameLine(430);
                ImGui::TextDisabled("i %d, %d", sl.i0, sl.i1);
            }
        }
        ImGui::EndChild();
    }
}

// =============================================================================
// Packets tab — live view of the client's network traffic
// =============================================================================
// The client is NativeAOT, so packets have no readable type names: a packet's
// identity is the MethodTable pointer in its first eight bytes. We give each
// distinct one a small id and count it, and you name the ones you work out.
// Identification in practice is "do the thing, see which id fires".
static void DrawPacketsTab() {
    const bool anyHook = g_pktInHooked.load() || g_pktOutHooked.load();

    ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Packet logger");
    ImGui::TextDisabled(
        "Inbound is hooked at PacketHandler::ProcessPacket, outbound at the\n"
        "connection's ring-buffer enqueue. Neither detour is installed until you\n"
        "ask for it here: they sit on the network thread, and the outbound\n"
        "signature is short enough that it is worth being deliberate about.\n"
        "Outbound also sees a few non-packet items; they show up as their own ids.");

    if (!anyHook) {
        ImGui::Spacing();
        static bool installFailed = false;
        if (ImGui::Button("Install packet hooks")) {
            installFailed = !InstallPacketHooks();
            if (!installFailed) PacketLabelsLoad();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("stays installed until the game closes");
        if (installFailed) {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
                "Neither packet signature matched this client build.");
        }
        return;
    }

    // Which of the two actually bound. "Installed" on its own was useless: a
    // signature that matches nothing on a newer build looks identical to one
    // that matched, and you only find out when a counter sits at zero.
    ImGui::Spacing();
    if (g_pktInHooked.load())
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
            "inbound  hooked at +0x%llX", (unsigned long long)g_pktInRva.load());
    else if (g_pktInSigFound.load())
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
            "inbound  signature found at +0x%llX but the hook would not install",
            (unsigned long long)g_pktInRva.load());
    else
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
            "inbound  SIGNATURE NOT FOUND in this build");

    if (g_pktOutHooked.load())
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
            "outbound hooked at +0x%llX", (unsigned long long)g_pktOutRva.load());
    else if (g_pktOutSigFound.load())
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
            "outbound signature found at +0x%llX but the hook would not install",
            (unsigned long long)g_pktOutRva.load());
    else
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
            "outbound SIGNATURE NOT FOUND in this build");

    ImGui::Separator();
    ImGui::Spacing();

    bool capIn = g_pktCaptureIn.load();
    bool capOut = g_pktCaptureOut.load();
    bool paused = g_pktPaused.load();
    bool toFile = g_pktLogToFile.load();

    ImGui::BeginDisabled(!g_pktInHooked.load());
    if (ImGui::Checkbox("Capture inbound", &capIn)) g_pktCaptureIn.store(capIn);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_pktOutHooked.load());
    if (ImGui::Checkbox("Capture outbound", &capOut)) g_pktCaptureOut.store(capOut);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox("Pause", &paused)) g_pktPaused.store(paused);
    ImGui::SameLine();
    if (ImGui::Checkbox("Also write packets.log", &toFile)) g_pktLogToFile.store(toFile);

    int capBytes = g_pktCaptureBytes.load();
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("bytes per packet", &capBytes, 16, 256))
        g_pktCaptureBytes.store(capBytes);
    ImGui::SameLine();
    int maxEntries = g_pktMaxEntries.load();
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("history", &maxEntries, 50, 20000))
        g_pktMaxEntries.store(maxEntries);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) PacketsClear();

    ImGui::TextDisabled("in %llu   out %llu   dropped while paused %llu",
        (unsigned long long)g_pktTotalIn.load(),
        (unsigned long long)g_pktTotalOut.load(),
        (unsigned long long)g_pktDropped.load());

    if (g_pktPaused.load())
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "Pause is on - packets are counted but nothing is being kept, which is "
            "why the log below is empty.");
    if (g_pktInHooked.load() && g_pktCaptureIn.load() && g_pktTotalIn.load() == 0 &&
        g_pktTotalOut.load() > 100)
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
            "Inbound is hooked but has never fired while outbound has. The inbound\n"
            "signature has almost certainly matched the wrong function on this build.");

    ImGui::Spacing();

    // ---- type table ----
    static uint64_t filterMt = 0;
    if (ImGui::CollapsingHeader("Packet types seen", ImGuiTreeNodeFlags_DefaultOpen)) {
        const std::vector<PacketTypeInfo> types = PacketTypesSnapshot();
        ImGui::TextDisabled("Click a row to filter the log below; double-click the name to rename.");
        ImGui::BeginChild("##pkttypes", ImVec2(0, 170), true);
        if (filterMt) {
            if (ImGui::SmallButton("Clear filter")) filterMt = 0;
            ImGui::SameLine();
            ImGui::TextDisabled("showing one type only");
        }
        for (const auto& t : types) {
            ImGui::PushID((int)t.id);
            const bool selected = (filterMt == t.mt);
            char label[192];
            snprintf(label, sizeof(label), "#%-3u  in %-6llu out %-6llu  %s",
                t.id, (unsigned long long)t.inCount, (unsigned long long)t.outCount,
                t.label.empty() ? "(unnamed)" : t.label.c_str());
            if (ImGui::Selectable(label, selected))
                filterMt = selected ? 0 : t.mt;

            if (ImGui::BeginPopupContextItem("##rename")) {
                static char nameBuf[64];
                if (ImGui::IsWindowAppearing()) {
                    snprintf(nameBuf, sizeof(nameBuf), "%s", t.label.c_str());
                }
                ImGui::SetNextItemWidth(220);
                if (ImGui::InputText("name", nameBuf, sizeof(nameBuf),
                    ImGuiInputTextFlags_EnterReturnsTrue)) {
                    PacketSetTypeLabel(t.mt, nameBuf);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::TextDisabled("saved to packet_types.txt");
                ImGui::EndPopup();
            }
            else if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("MethodTable %llX\nright-click to name it",
                    (unsigned long long)t.mt);
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    // ---- entry log ----
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.60f, 0.90f, 0.60f, 1.0f), "Log");

    // The log is re-snapshotted every frame and shifts as packets arrive, so
    // the selection is a copy of the entry rather than an index into it.
    static PacketEntry selected;
    static bool haveSelection = false;
    static bool follow = true;
    ImGui::Checkbox("Follow newest", &follow);

    const std::vector<PacketEntry> entries = PacketsSnapshot(2000);
    ImGui::BeginChild("##pktlog", ImVec2(0, 220), true);
    for (int i = (int)entries.size() - 1; i >= 0; --i) {
        const PacketEntry& e = entries[i];
        if (filterMt && e.mt != filterMt) continue;
        ImGui::PushID(i);
        char row[256];
        snprintf(row, sizeof(row), "%s #%-3u %3u bytes  %s",
            e.outbound ? "OUT" : "IN ", e.typeId, (unsigned)e.bytes.size(),
            e.strings.empty() ? "" : e.strings.c_str());
        const bool isSelected = haveSelection &&
            selected.whenMs == e.whenMs && selected.mt == e.mt;
        if (ImGui::Selectable(row, isSelected)) {
            selected = e;
            haveSelection = true;
            follow = false;
        }
        ImGui::PopID();
    }
    if (follow && !entries.empty())
        ImGui::SetScrollY(0.0f);   // newest is drawn first, so "newest" is the top
    ImGui::EndChild();

    // ---- hex dump of the selection ----
    if (haveSelection) {
        const PacketEntry& e = selected;
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.60f, 0.90f, 0.60f, 1.0f),
            "Type #%u  (MethodTable %llX)", e.typeId, (unsigned long long)e.mt);
        if (ImGui::SmallButton("Copy hex")) {
            const std::string dump = PacketHexDump(e);
            ImGui::SetClipboardText(dump.c_str());
        }
        const std::string dump = PacketHexDump(e);
        ImGui::BeginChild("##pkthex", ImVec2(0, 160), true,
            ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(dump.c_str());
        ImGui::EndChild();
    }
}

// =============================================================================
// Shapes tab — draw the client's own debug shapes in the world
// =============================================================================
// DisplayDebug is a server-to-client packet, so there is no client call to
// make: we build the object ourselves and hand it to the client's inbound
// handler. Building it needs the class's MethodTable, and a NativeAOT binary
// has no names to look one up by - so it is learned off a real packet rather
// than guessed. See debugshape.hpp for why that matters on this build.
static void DrawShapesTab() {
    ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "In-world debug shapes");
    ImGui::TextDisabled(
        "The client can already draw spheres, cubes and cylinders in the world -\n"
        "that is what the server's /debug shape commands do. We draw them locally\n"
        "by handing the client the same packet it would have received. Nothing is\n"
        "sent to any server; this only affects your own screen.");
    ImGui::Separator();

    // Everything below this line is now driven for you. It is all still here
    // because each step is worth being able to watch when it goes wrong, but
    // the normal case is that this one line is the only one you read.
    {
        const int stage = g_dbgAutoStage.load();
        ImGui::TextColored(
            stage == (int)DbgAutoStage::Ready ? ImVec4(0.55f, 0.95f, 0.55f, 1.0f)
            : stage == (int)DbgAutoStage::NoSeed ? ImVec4(0.95f, 0.55f, 0.45f, 1.0f)
            : ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "auto-setup: %s", DbgAutoStatus().c_str());

        bool autoOn = g_dbgAutoPrepare.load();
        if (ImGui::Checkbox("Do all of this automatically", &autoOn))
            g_dbgAutoPrepare.store(autoOn);
        ImGui::SameLine();
        ImGui::TextDisabled("finds the list, the allocators and the barrier, tests them, "
            "and makes room - in that order, retrying as needed");
    }

    ImGui::Separator();
    ImGui::Spacing();

    // ---------------------------------------------------------------------
    // Route A: take over a shape the server already drew
    // ---------------------------------------------------------------------
    ImGui::TextColored(ImVec4(0.60f, 0.90f, 0.60f, 1.0f),
        "A. Live shapes");

    const bool knownOk = g_dbgKnownWorks.load();
    if (knownOk) {
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
            "shape list found - %d live", g_dbgKnownCount.load());
        ImGui::TextDisabled(
            "Read straight from the client's own list of debug shapes, which sits at\n"
            "three fixed field offsets from the game root. It refreshes by itself, so\n"
            "a shape appears here the moment anything draws one - no searching, no\n"
            "remembering coordinates, and no race against it expiring.");
    }
    else {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
            "the known shape-list offsets don't resolve right now");
        ImGui::TextDisabled(
            "Either you aren't in a world yet, or a client patch has moved them.\n"
            "The search below is the fallback that found them in the first place.");
    }

    // These apply to every shape, spawned ones included, so they sit above the
    // list rather than behind having selected something.
    {
        DbgContainer pc;
        { std::lock_guard lock(g_dbgContMtx); pc = g_dbgContainer; }

        ImGui::BeginDisabled(g_dbgProfiling.load() || g_dbgKnownCount.load() == 0);
        if (ImGui::Button("Show me the entry"))
            DbgBuildEntryReport();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("draw TWO cubes first - colour is random per command, so "
            "what differs between two of them is the colour");

        if (pc.haveLife || pc.haveColor)
            ImGui::TextDisabled("countdown %s%X, colour %s%X   (keep-alive writes %llu)",
                pc.haveLife ? "+0x" : "none ", pc.haveLife ? pc.lifeOffset : 0,
                pc.haveColor ? "+0x" : "none ", pc.haveColor ? pc.colorOffset : 0,
                (unsigned long long)g_dbgKeepAliveWrites.load());
        else
            ImGui::TextDisabled("neither field located yet - colour and lifetime stay "
                "as whatever the template held");

        // Once the report shows where they are, type them in. Cheaper than
        // another heuristic that guesses wrong a fourth time.
        static int colOff = -1, lifeOff = -1;
        static float lifeMaxUi = 30.0f;
        ImGui::SetNextItemWidth(110);
        ImGui::InputInt("colour @", &colOff, 4, 16);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::InputInt("countdown @", &lifeOff, 4, 16);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputFloat("full life", &lifeMaxUi, 5.0f, 30.0f, "%.0f");
        ImGui::SameLine();
        if (ImGui::Button("Apply offsets"))
            DbgSetFieldOffsets(colOff, lifeOff, lifeMaxUi);

        ImGui::BeginDisabled(!pc.haveLife);
        bool keep = g_dbgKeepAlive.load();
        if (ImGui::Checkbox("Keep shapes alive", &keep))
            g_dbgKeepAlive.store(keep);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(pc.haveLife
            ? "refreshes every shape's countdown each frame"
            : "needs the countdown offset before it can do anything");

        if (g_dbgProfiling.load())
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "reading the entry - leave the game alone for a couple of seconds...");

        const std::string entryRep = DbgGetEntryReport();
        if (!entryRep.empty() && ImGui::CollapsingHeader("Entry report")) {
            ImGui::BeginChild("##entryrep", ImVec2(0, 300), true,
                ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(entryRep.c_str());
            ImGui::EndChild();
            if (ImGui::SmallButton("Copy report"))
                ImGui::SetClipboardText(entryRep.c_str());
        }
    }

    const int attached = g_dbgAttachedIndex.load();
    {
        std::vector<DbgLiveShape> live;
        { std::lock_guard lock(g_dbgLiveMtx); live = g_dbgLiveShapes; }

        if (live.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Nothing in the world right now. Run  /debug shape cube  "
                "and it will show up here on its own.");
        }
        else {
            ImGui::BeginChild("##liveshapes", ImVec2(0, 170), true);
            for (size_t i = 0; i < live.size(); ++i) {
                ImGui::PushID((int)i);
                const bool isAttached = (attached == (int)i);
                if (ImGui::SmallButton(isAttached ? "Following" : "Follow me"))
                    DbgAttachToPlayer(isAttached ? -1 : (int)i);
                ImGui::SameLine();
                ImGui::Text("%8.2f, %7.2f, %8.2f", live[i].x, live[i].y, live[i].z);
                if (!live[i].context.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.85f, 0.90f, 0.60f, 1.0f),
                        "%s", live[i].context.c_str());
                }
                ImGui::SameLine();
                ImGui::TextDisabled("  MT %llX", (unsigned long long)live[i].holderMT);
                if (!knownOk) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Analyse"))
                        DbgAnalyzeMatch((int)i);
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (attached >= 0)
                ImGui::TextDisabled("Following you - %llu writes.",
                    (unsigned long long)g_dbgAttachWrites.load());
        }
    }

    // ---- fallback: the search that located the list originally ----
    if (ImGui::CollapsingHeader("If the list above is empty or wrong: search for it again")) {
        ImGui::TextDisabled(
            "This is how the offsets above were found, and it is worth keeping for the\n"
            "next client patch. It is a blunt instrument though - searching the whole\n"
            "object graph for a position also matches every render and animation\n"
            "buffer holding it, so walking well away from the shape first is what\n"
            "makes the answer readable.\n"
            "\n"
            "  1. Stand still, press \"Remember where I am\".\n"
            "  2. Run  /debug shape cube.\n"
            "  3. Walk 20+ blocks away.\n"
            "  4. Search. Entries tagged as a 4x4 matrix are sorted to the top.");

        static float rememberedPos[3] = { 0, 0, 0 };
        static float shapeTol = 1.0f;

        const GsdkPlayerInfo pinfo = GsdkGetPlayerInfo();
        ImGui::BeginDisabled(!pinfo.valid);
        if (ImGui::Button("Remember where I am")) {
            rememberedPos[0] = (float)pinfo.x;
            rememberedPos[1] = (float)pinfo.y;
            rememberedPos[2] = (float)pinfo.z;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(240);
        ImGui::InputFloat3("##remembered", rememberedPos, "%.2f");
        ImGui::SetNextItemWidth(200);
        ImGui::SliderFloat("tolerance", &shapeTol, 0.05f, 5.0f, "%.2f");

        const bool scanning = g_dbgScanning.load();
        const bool haveRemembered = !(rememberedPos[0] == 0.0f && rememberedPos[1] == 0.0f &&
                                      rememberedPos[2] == 0.0f);
        ImGui::BeginDisabled(scanning || !haveRemembered);
        if (ImGui::Button("Search for that position"))
            DbgFindShapes(true, rememberedPos[0], rememberedPos[1], rememberedPos[2], shapeTol);
        ImGui::EndDisabled();
        if (scanning) {
            ImGui::SameLine();
            ImGui::TextDisabled("scanning... %llu objects",
                (unsigned long long)g_dbgScanVisited.load());
        }
    }

    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.60f, 0.90f, 0.60f, 1.0f),
        "B. Make your own, no command needed");

    DbgContainer cont;
    { std::lock_guard lock(g_dbgContMtx); cont = g_dbgContainer; }

    ImGui::TextDisabled(
        "Allocate an object of the entry type, copy a whole entry the client itself\n"
        "built, patch it, and drop it in a free slot. Cloning is the point - every\n"
        "field we never identified arrives already holding a value the renderer\n"
        "accepted.\n"
        "\n"
        "Colour and lifetime only take effect once \"Study a shape's fields\" above\n"
        "has located them. Without that a clone keeps the template's colour, and\n"
        "the template's remaining countdown - which is nearly none, because it was\n"
        "taken from a shape that had already been ticking down.");

    if (!cont.valid) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "No container analysed yet.");
    }
    else {
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
            "room for %u, %d in use%s", cont.arrayLength, cont.liveSlots,
            cont.haveCount ? ", count field found" : ", NO count field found");

        // Capacity 4 is List<T>'s default, and it doubles when the client adds
        // past the end. Rather than allocating a bigger array ourselves - which
        // would need an array allocator we have not verified, and a reference
        // store the GC is never told about - we get the client to grow its own
        // list by asking it for a few shapes the normal way.
        if (cont.liveSlots >= (int)cont.arrayLength)
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "Full. Grow it below, then spawn again.");

        // ---- working without the command ----
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.60f, 0.90f, 0.60f, 1.0f), "On servers");
        ImGui::TextDisabled(
            "Two things here needed a server-drawn shape: the entry's MethodTable,\n"
            "and the 192 bytes we clone. Both are fixed - the MethodTable sits at a\n"
            "known offset in the image, and the entry turned out to hold nothing but\n"
            "floats and ints, not a single object reference - so both keep perfectly\n"
            "well on disk. Save once from a singleplayer world and spawning works\n"
            "anywhere, with no server cooperation at all.");

        // Six separate links have to hold for a spawn, and a server can differ
        // from a singleplayer world at any of them. Show which one broke rather
        // than leaving "it failed" to be guessed at.
        if (ImGui::CollapsingHeader("Why can't I spawn here?")) {
            const std::string ready = DbgBuildReadinessReport();
            ImGui::BeginChild("##ready", ImVec2(0, 200), true,
                ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(ready.c_str());
            ImGui::EndChild();
            if (ImGui::SmallButton("Copy##ready"))
                ImGui::SetClipboardText(ready.c_str());
        }

        // The remembered path was a shortest-path alias that only held in a
        // singleplayer world; on a server it pointed at a field whose contents
        // changed between reads. Finding the list by its element type instead
        // is identity rather than location, so it holds anywhere.
        ImGui::BeginDisabled(g_dbgLocating.load() || !cont.holderMTRva);
        if (ImGui::Button("Find the shape list"))
            DbgLocateList();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(g_dbgLocating.load()
            ? "searching..."
            : "run this first on a server - locates it by element type");

        const std::string locRep = DbgGetLocateReport();
        if (!locRep.empty()) {
            ImGui::BeginChild("##locrep", ImVec2(0, 130), true,
                ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(locRep.c_str());
            ImGui::EndChild();
            if (ImGui::SmallButton("Copy##loc"))
                ImGui::SetClipboardText(locRep.c_str());
        }

        ImGui::BeginDisabled(!cont.haveTemplate || !cont.holderMTRva);
        if (ImGui::Button("Save this as the seed"))
            DbgSaveSeed();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (cont.seeded)
            ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
                "loaded from disk - no server shape needed");
        else if (cont.haveTemplate)
            ImGui::TextDisabled("taken from a live shape; save it to use on servers");
        else
            ImGui::TextDisabled("nothing to save yet");

        // ---- more than four, without the command ----
        ImGui::Spacing();
        static int growBy = 6;
        ImGui::SetNextItemWidth(110);
        ImGui::InputInt("##growby", &growBy, 1, 4);
        ImGui::SameLine();
        if (growBy < 1) growBy = 1;
        if (growBy > 32) growBy = 32;
        if (ImGui::Button("Grow by command"))
            DbgGrowList(growBy);
        ImGui::SameLine();
        ImGui::TextDisabled("singleplayer only - lets the client grow its own list");

        static int growTo = 32;
        ImGui::SetNextItemWidth(110);
        ImGui::InputInt("##growto", &growTo, 8, 32);
        if (growTo < 8) growTo = 8;
        if (growTo > 1024) growTo = 1024;
        ImGui::SameLine();
        if (!g_dbgArrayAllocRva.load()) {
            if (ImGui::Button("Find the array allocator"))
                StartWorker([]() { DbgResolveArrayAllocator(); });
            ImGui::SameLine();
            ImGui::TextDisabled("needed to make a bigger array ourselves");
        }
        else if (!g_dbgArrayAllocVerified.load() || !g_dbgBarrierVerified.load()) {
            if (ImGui::Button("Test it"))
                DbgRequestAllocatorTests();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "checks the array allocator and the GC write barrier, both on a\n"
                "throwaway array - nothing the game can see is touched");
        }
        else {
            if (ImGui::Button("Grow the array"))
                DbgRequestGrowArray(growTo);
            ImGui::SameLine();
            if (ImGui::Button("Create the list"))
                DbgRequestCreateList(growTo);
            ImGui::SameLine();
            ImGui::TextDisabled("create is for a world where the client never made one");
        }

        const std::string barEv = DbgGetBarrierEvidence();
        if (!barEv.empty()) {
            ImGui::TextDisabled("write barrier:");
            ImGui::BeginChild("##barev", ImVec2(0, 90), true,
                ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(barEv.c_str());
            ImGui::EndChild();
        }

        const std::string arrEv = DbgGetArrayAllocEvidence();
        if (!arrEv.empty()) {
            // Tall enough to read the candidate list, which is the point of it.
            ImGui::BeginChild("##arrev", ImVec2(0, 200), true,
                ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(arrEv.c_str());
            ImGui::EndChild();
            if (ImGui::SmallButton("Copy##arrev"))
                ImGui::SetClipboardText(arrEv.c_str());
        }
        if (cont.countCandidates > 1)
            ImGui::TextDisabled("%d ints matched the count - using the first; if nothing "
                "appears, that guess is the thing to doubt", cont.countCandidates);

        static float spawnColor[3] = { 0.2f, 1.0f, 0.3f };
        static float spawnScale = 2.0f;
        static float spawnAhead = 0.0f;
        static float spawnLife = 30.0f;
        static float spawnOpacity = 0.8f;
        static int spawnShape = 3;               // Cube, as the server's demo uses

        // Only the four that need nothing beyond the transform. Frustum, Sector
        // and Disc carry extra parameters in the packet that we never write, so
        // the renderer would read an array we have not provided.
        const char* shapeNames[] = { "Sphere", "Cylinder", "Cone", "Cube" };
        ImGui::SetNextItemWidth(140);
        ImGui::Combo("shape##spawn", &spawnShape, shapeNames, IM_ARRAYSIZE(shapeNames));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::ColorEdit3("colour##spawn", spawnColor);

        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("scale##spawn", &spawnScale, 0.2f, 10.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("blocks ahead", &spawnAhead, 0.0f, 20.0f);

        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("seconds##spawn", &spawnLife, 1.0f, 600.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("opacity##spawn", &spawnOpacity, 0.05f, 1.0f);

        const GsdkPlayerInfo pi = GsdkGetPlayerInfo();
        ImGui::BeginDisabled(!pi.valid);
        if (ImGui::Button("Spawn one at me")) {
            DbgRequest req;
            const float rad = pi.haveRotation ? pi.yaw * 0.01745329f : 0.0f;
            req.x = (float)pi.x + std::sin(rad) * spawnAhead;
            req.y = (float)pi.y;
            req.z = (float)pi.z - std::cos(rad) * spawnAhead;
            req.scaleX = req.scaleY = req.scaleZ = spawnScale;
            req.r = spawnColor[0]; req.g = spawnColor[1]; req.b = spawnColor[2];
            req.shape = (DbgShape)spawnShape;
            req.time = spawnLife;
            req.opacity = spawnOpacity;
            DbgQueueSpawn(req);   // records why if it refuses
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Duplicate an existing one"))
            DbgRequestDuplicate();
        ImGui::SameLine();
        if (ImGui::Button("Test the allocator"))
            DbgRequestAllocTest();
        ImGui::SameLine();
        if (ImGui::Button("Clear all shapes"))
            DbgRequestClearAll();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Nulls every slot and zeroes the count.\n"
                              "A way back to a clean state without restarting.");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Allocates one object and checks its header, without\n"
                              "touching the shape list. Safe, and it separates\n"
                              "\"is the allocator right\" from \"is the clone right\".");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copies a live shape's reference into a free slot.\n"
                              "Allocates nothing, so it works even without the allocator -\n"
                              "if a second shape appears, the array and count are right and\n"
                              "only building an object is missing.");
        ImGui::SameLine();
        ImGui::TextDisabled("spawned %llu   failed %llu",
            (unsigned long long)g_dbgSentCount.load(),
            (unsigned long long)g_dbgFailCount.load());

        const std::string why = DbgGetSpawnFailure();
        if (!why.empty())
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", why.c_str());

        const std::string allocTest = DbgGetAllocTestResult();
        if (!allocTest.empty())
            ImGui::TextWrapped("allocator test: %s", allocTest.c_str());

        // The checkbox that used to live here is gone. It existed because the
        // count offset was guessed by value and matched 86 different ints, so
        // writing it was a gamble. It is now List<T>'s _size, measured
        // structurally - and the client draws entries 0.._size, so not writing
        // it means nothing is drawn. There was never a case for leaving it off.
        ImGui::Separator();
        ImGui::BeginDisabled(g_dbgCountArmed.load());
        if (ImGui::Button("Find the real count field"))
            DbgFindCountField();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("press, then run /debug shape cube - the int that goes up "
            "by exactly one is the count");

        const std::string countRep = DbgGetCountReport();
        if (!countRep.empty()) {
            ImGui::BeginChild("##countrep", ImVec2(0, 120), true,
                ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(countRep.c_str());
            ImGui::EndChild();
        }

        ImGui::TextDisabled(
            "If a spawned shape shows up and then the game dies a few seconds later,\n"
            "that is a different fault to this one: storing a reference into the array\n"
            "with a plain write skips the GC's write barrier, so a fresh object can be\n"
            "collected while the array still points at it. Say so and I will go and\n"
            "find the barrier helper rather than guess at it.");
    }

    // The report is the honest part: if the heuristics above guessed wrong,
    // this is what shows where.
    const std::string report = DbgGetReport();
    if (!report.empty() && ImGui::CollapsingHeader("Container report")) {
        ImGui::BeginChild("##contreport", ImVec2(0, 260), true,
            ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(report.c_str());
        ImGui::EndChild();
        if (ImGui::SmallButton("Copy report"))
            ImGui::SetClipboardText(report.c_str());
    }

    DbgLayout lay;
    { std::lock_guard lock(g_dbgMtx); lay = g_dbgLayout; }
    const bool runtimeOk = g_dbgRuntimeReady.load();
    const bool pktHooked = g_pktInHooked.load();

    // ---- step 1: the allocators ----
    ImGui::Text("1. Runtime");
    ImGui::Indent(12.0f);
    if (g_dbgAllocVerified.load()) {
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
            "allocator at +0x%llX, %d allocation site(s), code check passed",
            (unsigned long long)g_dbgAllocRva.load(), g_dbgAllocVotes.load());
    }
    else {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "allocator not identified yet");
        ImGui::TextDisabled(
            "The published RhpNewFast signature is a call site inside one particular\n"
            "function and the code around it has changed, so it matches nothing here.\n"
            "Instead we look for the pattern every NativeAOT allocation uses -\n"
            "  lea rcx, [SomeMethodTable] / call RhpNewFast\n"
            "- keep only the sites whose MethodTable we already recognise, and let\n"
            "the call targets vote. Needs a shape drawn first, so there is a\n"
            "MethodTable to recognise.");
        if (ImGui::Button("Find the allocator"))
            StartWorker([]() { DbgResolveAllocator(); });
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "Nothing will be called until a candidate passes a check on its own code:\n"
            "an allocator's first job is to read the object size out of the MethodTable\n"
            "it was handed. Votes alone were not enough - plenty of helpers take a\n"
            "MethodTable in rcx, and calling one of those hangs the game rather than\n"
            "faulting, which no guard can recover from.");
    }

    const std::string evidence = DbgGetAllocEvidence();
    if (!evidence.empty()) {
        ImGui::BeginChild("##allocev", ImVec2(0, 90), true,
            ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(evidence.c_str());
        ImGui::EndChild();
    }
    if (runtimeOk)
        ImGui::TextDisabled("signature-based lookup also succeeded");
    ImGui::Unindent(12.0f);
    ImGui::Spacing();

    // ---- step 2: learn the layout ----
    ImGui::Text("2. Packet layout");
    ImGui::Indent(12.0f);
    if (lay.learned) {
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "learned");
        ImGui::TextDisabled("%s", lay.ToString().c_str());
        if (ImGui::Button("Forget layout"))
            DbgForget();
    }
    else {
        ImGui::TextDisabled(
            "Not learned yet. We need to see one real DisplayDebug packet to pick up\n"
            "the MethodTable and field offsets - guessing them has been wrong for\n"
            "every published offset on this build, so this asks rather than assumes.\n"
            "\n"
            "  1. Join a SINGLEPLAYER world. You need command permission, and there\n"
            "     the client is hosting the server itself.\n"
            "  2. Install the packet hooks on the Packets tab (needed to see it).\n"
            "  3. Run  /debug shape cube  in chat.\n"
            "\n"
            "We recognise it by its 4x4 matrix - a float[16] ending in 1.0 with your\n"
            "position in the translation column - so there is nothing to click.\n"
            "Once learned it is saved, and shapes work on any world after that,\n"
            "multiplayer included, because we build the packet locally.");

        bool armed = g_dbgLearnArmed.load();
        if (ImGui::Checkbox("Watch packets for it", &armed))
            g_dbgLearnArmed.store(armed);
        ImGui::TextDisabled("inspected %llu packets, %llu carried a 16-float matrix",
            (unsigned long long)g_dbgInspected.load(),
            (unsigned long long)g_dbgSawMatrix.load());
        ImGui::TextDisabled(
            "Both directions are watched. In a singleplayer world the client hosts\n"
            "the server in the same process, so a server-to-client packet can quite\n"
            "reasonably show up on the send path - assuming otherwise is what made\n"
            "the first attempt find nothing.");

        if (!g_pktInHooked.load() && !g_pktOutHooked.load()) {
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "Packet hooks aren't installed - nothing can be learned until they are.");
        }
        else if (armed) {
            ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
                "watching... run /debug shape cube now");
        }
    }
    ImGui::Unindent(12.0f);
    ImGui::Spacing();

    const std::string status = DbgGetStatus();
    if (!status.empty()) {
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::Spacing();
    }

    if (!lay.learned || !runtimeOk) return;

    // ---- step 3: draw ----
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.60f, 0.90f, 0.60f, 1.0f), "Draw");
    if (!pktHooked) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
            "Packet hooks aren't installed. Drawing goes through the client's own\n"
            "inbound handler, and we call it with the instance the client hands us -\n"
            "so the hook has to be in place and at least one real packet has to have\n"
            "arrived this session. Install them on the Packets tab.");
        return;
    }

    static int shapeIdx = 3;                       // Cube, same as the server's demo
    static float color[3] = { 1.0f, 0.2f, 0.2f };
    static float scale = 2.0f;
    static float lifetime = 30.0f;
    static float opacity = 0.8f;
    static bool flagFade = true;
    static bool flagNoWireframe = false;
    static bool flagNoSolid = false;
    static float yOffset = 0.0f;

    const char* shapeNames[] = { "Sphere", "Cylinder", "Cone", "Cube",
                                 "Frustum", "Sector", "Disc" };
    ImGui::SetNextItemWidth(160);
    ImGui::Combo("shape", &shapeIdx, shapeNames, IM_ARRAYSIZE(shapeNames));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::ColorEdit3("colour", color);

    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("scale", &scale, 0.1f, 20.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("seconds", &lifetime, 1.0f, 300.0f);

    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("opacity", &opacity, 0.05f, 1.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("height offset", &yOffset, -5.0f, 10.0f);

    ImGui::Checkbox("fade", &flagFade);
    ImGui::SameLine();
    ImGui::Checkbox("no wireframe", &flagNoWireframe);
    ImGui::SameLine();
    ImGui::Checkbox("no solid", &flagNoSolid);

    const GsdkPlayerInfo info = GsdkGetPlayerInfo();
    ImGui::BeginDisabled(!info.valid);
    if (ImGui::Button("Draw at my position")) {
        DbgRequest req;
        req.shape = (DbgShape)shapeIdx;
        req.x = (float)info.x;
        req.y = (float)info.y + yOffset;
        req.z = (float)info.z;
        req.scaleX = req.scaleY = req.scaleZ = scale;
        req.r = color[0]; req.g = color[1]; req.b = color[2];
        req.time = lifetime;
        req.opacity = opacity;
        req.flags = (flagFade ? DBG_FLAG_FADE : 0) |
                    (flagNoWireframe ? DBG_FLAG_NO_WIREFRAME : 0) |
                    (flagNoSolid ? DBG_FLAG_NO_SOLID : 0);
        DbgQueueShape(req);
    }
    ImGui::EndDisabled();
    if (!info.valid) {
        ImGui::SameLine();
        ImGui::TextDisabled("needs the position path (Position tab)");
    }

    // A shape out in front is the easy way to tell whether the thing appeared
    // where you meant it to, rather than inside your own head.
    ImGui::SameLine();
    ImGui::BeginDisabled(!info.valid || !info.haveRotation);
    if (ImGui::Button("Draw 5 blocks ahead")) {
        const float rad = info.yaw * 0.01745329f;
        DbgRequest req;
        req.shape = (DbgShape)shapeIdx;
        req.x = (float)info.x + std::sin(rad) * 5.0f;
        req.y = (float)info.y + yOffset;
        req.z = (float)info.z - std::cos(rad) * 5.0f;
        req.scaleX = req.scaleY = req.scaleZ = scale;
        req.r = color[0]; req.g = color[1]; req.b = color[2];
        req.time = lifetime;
        req.opacity = opacity;
        req.flags = (flagFade ? DBG_FLAG_FADE : 0) |
                    (flagNoWireframe ? DBG_FLAG_NO_WIREFRAME : 0) |
                    (flagNoSolid ? DBG_FLAG_NO_SOLID : 0);
        DbgQueueShape(req);
    }
    ImGui::EndDisabled();
    if (info.valid && !info.haveRotation) {
        ImGui::SameLine();
        ImGui::TextDisabled("needs facing too");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("drawn %llu   failed %llu",
        (unsigned long long)g_dbgSentCount.load(),
        (unsigned long long)g_dbgFailCount.load());
    if (g_dbgFailCount.load() > 0)
        ImGui::TextDisabled(
            "Failures usually mean a field offset is off, or no packet has arrived\n"
            "yet this session - we reuse the handler instance the client passes us,\n"
            "so at least one real packet has to come in first.");
}


// =============================================================================
// TAB: Build  (load a prefab, then be shown where every block goes)
// =============================================================================
// This is the Shapes work put to use. A prefab is a list of block names and
// offsets; the shape list can draw cubes anywhere; so the two together are a
// schematic overlay. Nothing here places a block or talks to a server - it
// outlines where blocks go and keeps track of which ones you have done.

// =============================================================================
// Letting the client tell us
// =============================================================================
// Every other way of knowing a block was placed is a guess. This one is not:
// the client announces each placement to the server, and we are already
// standing on the path it takes. The catch is that we cannot look up the
// packet by name or id, so it has to be recognised - which is what the learner
// does, by watching you place a block whose coordinates it already knows.
// =============================================================================
// Recording what you build
// =============================================================================
// The thing you actually want here is to draw a box round something that
// already exists and copy it. That needs reading blocks out of the world, which
// we cannot do. What we can see is every block YOU place - position and id,
// off the same packet the guide already follows - so a build can be captured
// while you make it, and put up again somewhere else.
static void DrawRecordSection() {
    ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Record a build");

    const bool watching = PlaceIsWatching();
    if (!watching) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "Needs the placement watcher above - that is what sees your blocks.");
        return;
    }

    const bool rec = g_buildRecording.load();
    if (rec) {
        if (ImGui::Button("Stop recording", ImVec2(150, 0))) BuildRecordStop();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "recording");
    }
    else {
        if (ImGui::Button("Start recording", ImVec2(150, 0))) BuildRecordStart();
        ImGui::SameLine();
        ImGui::TextDisabled("then build something - every block you place is kept");
    }

    ImGui::TextDisabled("%s", BuildRecordSummary().c_str());

    static char prefabName[64] = "my_build";
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("name", prefabName, sizeof(prefabName));
    ImGui::SameLine();

    static std::string saveMsg;
    ImGui::BeginDisabled(BuildRecordCount() == 0);
    if (ImGui::Button("Save as prefab")) {
        std::string err;
        const bool ok = BuildRecordSavePrefab(prefabName, err);
        saveMsg = ok ? (err.empty() ? "saved to the prefabs folder" : err)
                     : ("could not save: " + err);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) BuildRecordClear();
    if (!saveMsg.empty()) ImGui::TextDisabled("%s", saveMsg.c_str());

    // The table that makes the saved file readable instead of a list of numbers.
    ImGui::TextDisabled("%zu block name(s) known", BuildKnownBlockIds());
    ImGui::SameLine();
    ImGui::TextDisabled("- learned whenever you place one the prefab asked for");
    const std::string conflict = BuildBlockIdConflict();
    if (!conflict.empty())
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", conflict.c_str());

    ImGui::TextDisabled(
        "Blocks with no name yet are saved as Unknown_<id>. Build one of each\n"
        "from a prefab that names them and they fill in - or edit the file.");
}


// =============================================================================
// TAB: Copy  (mark out a region of the world and make a prefab of it)
// =============================================================================
// Building from a prefab and making one are different jobs with different
// controls, and cramming them together was making the Build tab a wall. This is
// the second job: say which part of the world you mean, then turn it into
// something the Build tab can put up again elsewhere.
static void DrawCopyTab() {
    ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Copy part of the world");
    ImGui::TextDisabled(
        "Mark out a box around something that already exists, and turn it into a\n"
        "prefab you can build again somewhere else.");

    // The honest division, said once at the top. Recording works and makes
    // real prefabs; reading blocks out of a box you did not build does not,
    // and no amount of rearranging the tab changes that.
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
        "Copying a build you did not place is not possible on this client - the\n"
        "blocks cannot be read. Recording one as you build it works, and is the\n"
        "first section below.");
    ImGui::Separator();

    // Put the thing that works first. It was at the bottom, underneath four
    // sections of apparatus for a feature that never arrived.
    DrawRecordSection();

    ImGui::Spacing();

    // ---------------------------------------------------------------------
    // A. Marking it out
    // ---------------------------------------------------------------------
    ImGui::SeparatorText("Marking out a region");

    bool show = g_selShow.load();
    if (ImGui::Checkbox("Show it in the world", &show)) {
        g_selShow.store(show);
        if (!show) DbgRequestClearAll();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("uses the same shapes as the build outline, so one at a time");

    ImGui::Spacing();
    ImGui::TextDisabled("Two corners - stand in one, then the other:");
    if (ImGui::Button("Corner 1 = me", ImVec2(130, 0))) SelSetCorner1();
    ImGui::SameLine();
    if (g_sel1Set.load())
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%d, %d, %d",
            g_selX1.load(), g_selY1.load(), g_selZ1.load());
    else
        ImGui::TextDisabled("not set");

    if (ImGui::Button("Corner 2 = me", ImVec2(130, 0))) SelSetCorner2();
    ImGui::SameLine();
    if (g_sel2Set.load())
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "%d, %d, %d",
            g_selX2.load(), g_selY2.load(), g_selZ2.load());
    else
        ImGui::TextDisabled("not set");

    ImGui::Spacing();
    ImGui::TextDisabled("Or a box around you, then push the faces out:");
    static int radius = 8, below = 1, above = 8;
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("out", &radius, 1, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("down", &below, 1, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("up", &above, 1, 4);
    ImGui::SameLine();
    if (ImGui::Button("Box around me")) {
        SelBoxAtPlayer(radius, below, above);
        g_selShow.store(true);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---------------------------------------------------------------------
    // B. Fitting it
    // ---------------------------------------------------------------------
    ImGui::SeparatorText("Fitting the region");

    const bool haveBox = g_sel1Set.load() && g_sel2Set.load();
    ImGui::BeginDisabled(!haveBox);

    static int step = 1;
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("step", &step, 1, 5);
    if (step < 1) step = 1;
    ImGui::SameLine();
    ImGui::TextDisabled("how many blocks each press moves");

    // One row per axis, each with both faces, so a face and its opposite are
    // never on different sides of the panel.
    struct FaceRow { const char* label; int outFace; int inFace; };
    static const FaceRow rows[3] = {
        { "X (east-west)",   1, 0 },
        { "Y (up-down)",     3, 2 },
        { "Z (north-south)", 5, 4 },
    };
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        ImGui::Text("%-16s", rows[i].label);
        ImGui::SameLine();
        if (ImGui::Button("- low")) SelNudgeFace(rows[i].inFace, -step);
        ImGui::SameLine();
        if (ImGui::Button("+ low")) SelNudgeFace(rows[i].inFace, step);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::Button("- high")) SelNudgeFace(rows[i].outFace, -step);
        ImGui::SameLine();
        if (ImGui::Button("+ high")) SelNudgeFace(rows[i].outFace, step);
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (ImGui::Button("Grow all")) SelExpandAll(step);
    ImGui::SameLine();
    if (ImGui::Button("Shrink all")) SelExpandAll(-step);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) { SelClear(); DbgRequestClearAll(); }

    ImGui::TextDisabled("Move the whole box:");
    if (ImGui::Button("-X")) SelMove(-step, 0, 0);
    ImGui::SameLine();
    if (ImGui::Button("+X")) SelMove(step, 0, 0);
    ImGui::SameLine();
    if (ImGui::Button("-Y")) SelMove(0, -step, 0);
    ImGui::SameLine();
    if (ImGui::Button("+Y")) SelMove(0, step, 0);
    ImGui::SameLine();
    if (ImGui::Button("-Z")) SelMove(0, 0, -step);
    ImGui::SameLine();
    if (ImGui::Button("+Z")) SelMove(0, 0, step);

    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextColored(haveBox ? ImVec4(0.55f, 0.95f, 0.55f, 1.0f)
                               : ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
        "%s", SelSummary().c_str());

    {
        int w, h, d;
        long long vol;
        SelSize(w, h, d, vol);
        if (vol > 200000)
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "That is a very large region - a prefab that size would be "
                "unwieldy to build by hand.");
    }

    // ---------------------------------------------------------------------
    // C. Naming the blocks
    // ---------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::SeparatorText("Block names");
    ImGui::TextDisabled(
        "A copied region comes out as block ids. Without names it is a list of\n"
        "numbers, which is why a recorded build shows Unknown_<id> for anything\n"
        "you have not placed from a prefab.");

    const size_t known = BuildKnownBlockIds();

    if (known > 1000) {
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
            "%zu block names on disk - every id resolves, including blocks you "
            "have never touched", known);
        ImGui::TextDisabled(
            "Taken out of the client's own asset map, which is indexed straight\n"
            "by block id. Saved, so this is done with - ids belong to the game,\n"
            "not to a session.");
    }
    else {
        ImGui::TextDisabled("%s", BlockRegStatus().c_str());
        ImGui::BeginDisabled(g_blockRegScanning.load());
        if (ImGui::Button("Find the block name table")) BlockRegFind();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (g_blockRegScanning.load())
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "searching...");
        else
            ImGui::TextDisabled("reads only, takes a few seconds, then never again");
        ImGui::TextDisabled(
            "Searches using a name it already knows: find that text in memory,\n"
            "find what holds it, and check it sits at the index its id names.\n"
            "Needs one name learned first - place a block a prefab asked for.");
        ImGui::TextDisabled("%zu name(s) so far, learned by watching you build", known);
    }

    // Always available, never tucked inside the "not found yet" branch. It used
    // to disappear the moment the name table succeeded, which is precisely when
    // it started mattering for the other thing it feeds: coordinates.
    if (ImGui::TreeNode("Paste /block get lines")) {
        ImGui::TextDisabled(
            "Run  /block get <x y z>  in chat and paste the lines here. Each one\n"
            "gives a name, an id, AND a coordinate - and the coordinate is what\n"
            "section D needs to find where blocks live in memory.");
        static char paste[4096] = "";
        static std::string pasteMsg;
        ImGui::InputTextMultiline("##blockget", paste, sizeof(paste), ImVec2(-1, 80));
        if (ImGui::Button("Read those lines")) {
            const int n = BuildLearnFromBlockGet(paste);
            pasteMsg = "learned " + std::to_string(n) + " new name(s), " +
                std::to_string(BuildGetProbes().size()) + " block(s) known by position";
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", pasteMsg.c_str());
        ImGui::TreePop();
    }

    // A block id you can check by hand. /block get prints ids in chat, so this
    // is a direct way to confirm the table against the game itself rather than
    // against our own guess about it.
    {
        static int probeId = 1238;
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("check id", &probeId, 1, 50);
        if (probeId < 0) probeId = 0;
        ImGui::SameLine();
        const std::string nm = BuildGetBlockName((uint32_t)probeId);
        if (nm.empty())
            ImGui::TextDisabled("(nothing at that id)");
        else
            ImGui::TextColored(ImVec4(1.00f, 0.85f, 0.40f, 1.0f), "%s", nm.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("- compare with /block get <x y z> in chat");
    }
    const std::string rep = BlockRegReport();
    if (!rep.empty() && ImGui::TreeNode("What the search saw")) {
        ImGui::BeginChild("##regrep", ImVec2(0, 130), true,
            ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(rep.c_str());
        ImGui::EndChild();
        if (ImGui::Button("Copy##regrep")) ImGui::SetClipboardText(rep.c_str());
        ImGui::TreePop();
    }

    // Reading blocks out of the world is not available. Five approaches
    // through memory found only lookup tables, and the network route - QUIC,
    // then zstd, then the game protocol - decoded far enough to read a
    // section's geometry but crashed the game on relog. The hooks it needed
    // live on the client's own network thread, and no feature is worth that.
    //
    // What remains below is everything that does work, which is most of it.

}

// =============================================================================
// Having it place them
// =============================================================================
// This sends. Everything else in the build guide reads the client or draws on
// your own screen; this puts packets on the wire that you did not personally
// click for, and it is worth the UI saying so rather than burying it.
//
// It cannot invent a placement - it re-sends one you made, with the position
// changed. So it can only place the block you last placed by hand, which is
// also the only guarantee available that you actually have any.
static void DrawAutoBuildSection() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.00f, 0.72f, 0.35f, 1.0f), "Have it place them");
    // Tested, and it does not work. The packet goes out and the server treats
    // it as advisory: blocks appear on your own screen, no item is consumed,
    // and nothing is there after a relog. Saying so here is the difference
    // between a known dead end and a trap.
    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
        "This does not really place blocks. The server ignores the request -\n"
        "blocks appear on your screen only, your inventory does not change,\n"
        "and they are gone when you log back in. Left here because it proves\n"
        "the send path works, not because it builds anything.");

    const PlaceTemplate tmpl = PlaceGetTemplate();
    if (!tmpl.material.empty()) {
        ImGui::TextDisabled("copying the placement you made at %d, %d, %d - %s",
            tmpl.wx, tmpl.wy, tmpl.wz, tmpl.material.c_str());
        // Zero here is the whole reason the first working send placed nothing.
        // It is worth being able to see at a glance that it is not.
        if (tmpl.blockId)
            ImGui::TextDisabled("block id %u", tmpl.blockId);
        else
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
                "block id reads 0 - that would place nothing. Place another one.");
    }

    // The packet we found carries the coordinates and nothing else - every byte
    // after the position is zero, including whatever would say which block.
    // Copying it therefore asks the server to place nothing, which is exactly
    // what it did. This records a real placement in full instead of guessing
    // which other packet carries the rest.
    ImGui::BeginDisabled(g_placeSnapshot.load());
    if (ImGui::Button("Record one placement")) PlaceSnapshotArm(4000);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (g_placeSnapshot.load())
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "place ONE block now");
    else
        ImGui::TextDisabled("logs every packet for 4s - reads only, sends nothing");
    if (!PlaceSnapshotStatus().empty())
        ImGui::TextDisabled("%s", PlaceSnapshotStatus().c_str());

    std::string whyNot;
    const bool ready = PlaceCanBuild(whyNot);
    if (!ready) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%s", whyNot.c_str());
        return;
    }

    const BuildTarget t = BuildGetTarget();
    const bool sameMaterial = t.valid && t.name == tmpl.material;
    if (t.valid && !sameMaterial)
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "Next is %s. Hold it and place one by hand, then this can carry on.",
            t.name.c_str());

    const bool proven = g_placeProven.load();
    if (!proven)
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
            "Untested on this build, and the first attempt crashed the client.\n"
            "This sends ONE packet and stops. Every step is written to the log\n"
            "first, so if it goes down again the log says which step did it.");

    static int burst = 16;
    if (proven) {
        ImGui::SetNextItemWidth(200);
        ImGui::SliderInt("how many", &burst, 1, 128);
    }

    const int left = g_placeBurstLeft.load();
    if (left > 0) {
        if (ImGui::Button("Stop", ImVec2(170, 0))) PlaceStopBurst();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "%d to go", left);
    }
    else {
        ImGui::BeginDisabled(!sameMaterial);
        if (ImGui::Button(proven ? "Place them for me" : "Try one block",
                ImVec2(170, 0)))
            PlaceStartBurst(proven ? burst : 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("sent %llu, held back %llu as out of reach",
            (unsigned long long)g_placeSent.load(),
            (unsigned long long)g_placeRefused.load());
    }

    if (!PlaceBuildStatus().empty())
        ImGui::TextDisabled("%s", PlaceBuildStatus().c_str());

    float reach = g_placeMaxReach.load();
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderFloat("reach", &reach, 2.0f, 12.0f, "%.0f blocks"))
        g_placeMaxReach.store(reach);
    ImGui::SameLine();
    int gap = g_placeGapMs.load();
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("gap", &gap, 80, 1000, "%d ms")) g_placeGapMs.store(gap);

    ImGui::TextDisabled(
        "A bounded run, on purpose. Nothing ever comes back to say the server\n"
        "accepted a placement, so if it is refusing them the guide ends up wrong\n"
        "by however many you asked for - and no more. Undo walks it back.\n"
        "Rotation comes from the block you placed, so stairs and beams all land\n"
        "the same way round. Reach and spacing are here because a server that\n"
        "checks either will reject the lot.");
    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
        "On a multiplayer server this is a build bot, and almost certainly\n"
        "against the rules. Your own world or your own island, your call.");

    // ---------------------------------------------------------------------
    // The one question the next approach rests on
    // ---------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.60f, 0.90f, 0.60f, 1.0f), "Can the camera be aimed?");
    ImGui::TextDisabled(
        "The way that cannot be ignored by a server is to make the CLIENT place\n"
        "the block: point it at the target and click, so it builds its own\n"
        "request with its own raycast, inventory check and item. That rests on\n"
        "one thing nobody can answer from here - whether writing the look angles\n"
        "sticks, or the client recomputes them from the mouse every frame.");

    static const char* result = nullptr;
    if (ImGui::Button("Turn my view 45 degrees")) {
        result = BuildNudgeYaw(45.0f)
            ? "written - did the view actually turn?"
            : "could not write: the facing fields are not resolved (use Position)";
    }
    ImGui::SameLine();
    ImGui::TextDisabled("writes nothing else, sends nothing, places nothing");
    if (result) ImGui::TextDisabled("%s", result);
}

static void DrawAutoAdvanceSection() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.60f, 0.90f, 0.60f, 1.0f),
        "Or let the client tell it (recommended)");
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
        "This does not place blocks. You still build it by hand - this only\n"
        "notices when you have, so you never click \"Placed it\" again.");
    ImGui::TextDisabled(
        "The client sends the server a packet every time you place a block. If\n"
        "we can recognise that packet, the guide keeps up on its own - including\n"
        "when you build out of order or go back to fill in a gap.\n"
        "Nothing is sent. This only reads what the client was sending anyway.");

    const bool outHooked = g_pktOutHooked.load();
    if (!outHooked) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "the outbound packet hook is not installed");
        static bool failed = false;
        if (ImGui::Button("Install it")) failed = !InstallPacketHooks();
        ImGui::SameLine();
        ImGui::TextDisabled("same hook the Packets tab uses; stays until the game closes");
        if (failed)
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
                "Neither packet signature matched this client build.");
        return;
    }

    PlaceLayout lay;
    { std::lock_guard lock(g_placeMtx); lay = g_placeLayout; }

    if (lay.learned) {
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "learned");
        ImGui::TextDisabled("%s", lay.ToString().c_str());

        bool on = g_placeAutoAdvance.load();
        if (ImGui::Checkbox("Follow along automatically", &on))
            g_placeAutoAdvance.store(on);

        // Three numbers rather than two, because the useful question when this
        // goes quiet is which of them is zero.
        const unsigned long long seen = (unsigned long long)g_placeSeen.load();
        const unsigned long long matched = (unsigned long long)g_placeMatched.load();
        const unsigned long long ignored = (unsigned long long)g_placeIgnored.load();
        ImGui::Text("placements seen %llu     matched %llu     not in the prefab %llu",
            seen, matched, ignored);

        const std::string lastSeen = PlaceGetLastSeen();
        if (!lastSeen.empty()) ImGui::TextDisabled("%s", lastSeen.c_str());

        if (seen == 0)
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "Nothing seen yet. Go and place a block - this notices, it does\n"
                "not act. If the count stays at zero while you are placing, the\n"
                "wrong packet was learned: Forget it and learn it again.");
        else if (matched == 0 && ignored > 2)
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "Placements are being seen, but none of them are where the prefab\n"
                "wants one. Compare the coordinates above with the block it is\n"
                "asking for: off by a constant means the anchor is in the wrong\n"
                "place, nonsense means it learned the wrong field.");

        if (ImGui::Button("Forget it")) PlaceForget();

        ImGui::Spacing();
        ImGui::TextDisabled("Recording a build and copying a region live in the "
            "Copy tab.");

        // Both of these were tried and neither works on this build: the server
        // ignores a forged placement, and written look angles do not stick.
        // They stay because the evidence is worth keeping, but they are closed
        // by default - an open dead end reads like an unfinished feature.
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Things that did not work"))
            DrawAutoBuildSection();
    }
    else if (g_placeLearning.load()) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
            "%s", PlaceGetStatus().c_str());
        ImGui::TextDisabled("%d placement(s) seen, %d reading(s) still fit",
            g_placeSamples.load(), g_placeCandidates.load());
        if (ImGui::Button("Stop")) PlaceLearnCancel();
    }
    else {
        ImGui::TextDisabled("%s", PlaceGetStatus().c_str());
        ImGui::BeginDisabled(!g_buildActive.load());
        if (ImGui::Button("Learn how placing works")) PlaceLearnArm();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (!g_buildActive.load())
            ImGui::TextDisabled("start the guide first - it needs a block to watch for");
        else
            ImGui::TextDisabled("then place the white block, and one more after it");

        ImGui::TextDisabled(
            "It finds what in the outgoing traffic is carrying the coordinates it\n"
            "just asked you for. Two placements, well apart, because one\n"
            "coordinate can turn up by coincidence and two cannot.");
    }

    const std::string rep = PlaceGetReport();
    if (!rep.empty()) {
        if (ImGui::TreeNode("What it found")) {
            ImGui::BeginChild("##placerep", ImVec2(0, 140), true,
                ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(rep.c_str());
            ImGui::EndChild();
            if (ImGui::Button("Copy##placerep")) ImGui::SetClipboardText(rep.c_str());
            ImGui::TreePop();
        }
    }
}

// =============================================================================
// The build tab
// =============================================================================
// Laid out as the three things a person actually does, in order: pick a build,
// put it where you want it, then build it. Everything else - capacity, opacity,
// nudge offsets, the shape system's own setup - is real but is not what you
// came here for, so it lives behind a disclosure and the defaults are right.
//
// The one thing worth saying about the shape of this: the anchor is adjusted
// AFTER the ghost is visible, not before. Judging where a build should sit from
// a coordinate readout is guesswork; judging it from the ghost standing in
// front of you is not. So step 2 is a live control, not a form you fill in.

namespace {

// A colour swatch, so the legend and the target line can show the same colour
// the block is drawn in. Small enough to sit inline with text.
void BlockSwatch(const std::string& name, float size = 13.0f) {
    float r = 1, g = 1, b = 1;
    BuildColourForName(name, r, g, b);
    ImGui::ColorButton(("##sw" + name).c_str(), ImVec4(r, g, b, 1.0f),
        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop |
        ImGuiColorEditFlags_NoBorder,
        ImVec2(size, size));
}

// A compass pad. Four arrows around a centre, which is what "move it that way"
// looks like - a row of +X / -X buttons makes you do the mapping in your head.
void AnchorPad() {
    const float w = 34.0f, h = 26.0f;
    const int step = (std::max)(1, g_buildNudgeStep.load());
    const float indent = w + 6.0f;

    ImGui::Dummy(ImVec2(indent, 1));
    ImGui::SameLine();
    if (ImGui::Button("forward", ImVec2(w + 26.0f, h))) BuildNudgeRelative(step, 0, 0);

    if (ImGui::Button("left", ImVec2(w + 6.0f, h)))  BuildNudgeRelative(0, -step, 0);
    ImGui::SameLine();
    if (ImGui::Button("turn", ImVec2(w + 6.0f, h)))  BuildRotateAnchor(1);
    ImGui::SameLine();
    if (ImGui::Button("right", ImVec2(w + 6.0f, h))) BuildNudgeRelative(0, step, 0);

    ImGui::Dummy(ImVec2(indent, 1));
    ImGui::SameLine();
    if (ImGui::Button("back", ImVec2(w + 26.0f, h))) BuildNudgeRelative(-step, 0, 0);

    ImGui::SameLine(0.0f, 18.0f);
    ImGui::BeginGroup();
    if (ImGui::Button("up", ImVec2(w + 6.0f, h)))   BuildMoveAnchor(0, step, 0);
    if (ImGui::Button("down", ImVec2(w + 6.0f, h))) BuildMoveAnchor(0, -step, 0);
    ImGui::EndGroup();
}

void DrawSetupStrip() {
    const int stage = g_dbgAutoStage.load();
    const bool ready = stage == (int)DbgAutoStage::Ready;
    const bool stuck = stage == (int)DbgAutoStage::NoSeed;

    if (ready) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "Ready to draw");
        ImGui::SameLine();
        // -1 means the list could not be read this frame, which is a normal
        // thing to happen between worlds. Printing it raw just looked broken.
        const int slots = DbgListCapacity();
        if (slots >= 0) ImGui::TextDisabled("- %d shape slots", slots);
        else            ImGui::TextDisabled("- counting...");
        return;
    }

    ImGui::TextColored(stuck ? ImVec4(0.95f, 0.55f, 0.45f, 1.0f)
                             : ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
        "%s", DbgAutoStatus().c_str());
    if (stuck)
        ImGui::TextDisabled(
            "One-time step: load any singleplayer world and type /debug shape cube.\n"
            "That makes the client build a real shape, which is what gets copied.\n"
            "It saves to disk, so it is never needed again - on any server.");
}

} // namespace

// Following along: the guide notices when you place a block and moves on by
// itself, including when you build out of order or go back and fill a gap.
//
// It needs no decisions: the hook goes in at startup, the learning arms itself
// whenever a ghost is up (SessionTick, so it works with the overlay hidden),
// and what it learns is saved - so after the first time, any block you place
// on the build confirms it for the session.
//
// It never places anything. It reads what the client was already sending.
void DrawFollowAlong() {
    if (!g_pktOutHooked.load()) {
        ImGui::TextDisabled("Follow along: not available - no packet hook on this build");
        return;
    }

    PlaceLayout lay;
    { std::lock_guard lock(g_placeMtx); lay = g_placeLayout; }

    if (lay.learned) {
        bool on = g_placeAutoAdvance.load();
        if (ImGui::Checkbox("Follow along as I place blocks", &on))
            g_placeAutoAdvance.store(on);
        ImGui::SameLine();

        const unsigned long long matched = (unsigned long long)g_placeMatched.load();
        const unsigned long long seen = (unsigned long long)g_placeSeen.load();
        const unsigned long long wrong = (unsigned long long)g_placeWrong.load();
        if (!on)
            ImGui::TextDisabled("- off, use the buttons below");
        else if (matched || wrong) {
            ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "- %llu kept up with", matched);
            if (wrong) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%llu wrong block(s)", wrong);
            }
        }
        else if (seen)
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "- %llu placed, none where the ghost wants one", seen);
        else
            ImGui::TextDisabled("- waiting for you to place one");
        return;
    }

    if (g_placeLearning.load()) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "Follow along: %s",
            PlaceGetStatus().c_str());
        if (g_placeSamples.load() > 0)
            ImGui::TextDisabled("%d placement(s) seen, %d reading(s) still fit",
                g_placeSamples.load(), g_placeCandidates.load());
        return;
    }

    ImGui::TextDisabled(g_buildActive.load() ? "Follow along: starting..."
                                             : "Follow along: starts once the ghost is up");
}

namespace {

std::string AgoText(int64_t unixSec) {
    if (unixSec <= 0) return "never";
    const int64_t s = BuildUnixMs() / 1000 - unixSec;
    char buf[48];
    if (s < 60)          return "just now";
    if (s < 3600)        snprintf(buf, sizeof(buf), "%lld min ago", (long long)(s / 60));
    else if (s < 86400)  snprintf(buf, sizeof(buf), "%lld h ago", (long long)(s / 3600));
    else if (s < 172800) return "yesterday";
    else                 snprintf(buf, sizeof(buf), "%lld days ago", (long long)(s / 86400));
    return buf;
}

std::string DurationText(int64_t ms) {
    const int64_t min = ms / 60000;
    char buf[48];
    if (min < 1) return "under a minute";
    if (min < 60) snprintf(buf, sizeof(buf), "%lld min", (long long)min);
    else snprintf(buf, sizeof(buf), "%lld h %lld min", (long long)(min / 60), (long long)(min % 60));
    return buf;
}

// Where you are, when it is known. For "the castle is 300 blocks south-west".
bool UiPlayerPos(double& x, double& y, double& z) {
    if (NowSteadyMs() - g_posLastUpdateMs.load() > 3000) return false;
    x = g_posX.load(); y = g_posY.load(); z = g_posZ.load();
    return true;
}

// A button that has to be pressed twice, for anything that throws work away.
// The second press has three seconds to happen; after that it is a normal
// button again. Cheaper than a dialog, and harder to hit by accident than one.
bool ConfirmButton(const char* label, const char* confirmLabel, const ImVec2& size = ImVec2(0, 0)) {
    static ImGuiID armed = 0;
    static long long armedAt = 0;
    const ImGuiID id = ImGui::GetID(label);
    const long long now = (long long)GetTickCount64();
    const bool isArmed = armed == id && now - armedAt < 3000;

    const std::string text = std::string(isArmed ? confirmLabel : label) + "###" + label;
    if (isArmed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.20f, 0.18f, 1.0f));
    const bool clicked = ImGui::Button(text.c_str(), size);
    if (isArmed) ImGui::PopStyleColor();
    if (!clicked) return false;
    if (isArmed) { armed = 0; return true; }
    armed = id;
    armedAt = now;
    return false;
}

// -----------------------------------------------------------------------------
// 1. Your builds
// -----------------------------------------------------------------------------
// Every build is a session that saves itself. This is where you see the one
// you are on, pick up another, or start a new one - and it is the first thing
// on the tab because after a crash or a relog it is the first thing you want:
// "is my build still there". It is, and it says so.
void DrawSessionsSection() {
    ImGui::SeparatorText("1. Your builds");

    const bool busy = g_sessionBusy.load();
    const bool haveActive = SessionHasActive();

    if (haveActive) {
        // Name and note, edited in place and saved with everything else.
        static std::string shownFor;
        static char nameBuf[128] = "";
        static char noteBuf[128] = "";
        const std::string id = SessionActiveId();
        if (id != shownFor) {
            shownFor = id;
            strncpy_s(nameBuf, SessionActiveName().c_str(), _TRUNCATE);
            strncpy_s(noteBuf, SessionActiveNote().c_str(), _TRUNCATE);
        }
        ImGui::SetNextItemWidth(230);
        if (ImGui::InputText("##sname", nameBuf, sizeof(nameBuf))) SessionRename(nameBuf);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260);
        if (ImGui::InputTextWithHint("##snote", "where is it? server, world, town...",
                noteBuf, sizeof(noteBuf)))
            SessionSetNote(noteBuf);

        const BuildStats st = BuildGetStats();
        const size_t finished = st.done + st.skipped;
        const float frac = st.total ? (float)finished / (float)st.total : 0.0f;
        char overlay[96];
        snprintf(overlay, sizeof(overlay), "%zu of %zu  (%.0f%%)", finished, st.total,
            frac * 100.0f);
        ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);

        std::string line = DurationText(st.activeMs) + " spent placing";
        if (st.perHour > 0.0) {
            char buf[96];
            snprintf(buf, sizeof(buf), "  -  about %.0f blocks an hour", st.perHour);
            line += buf;
            if (st.etaHours > 0.0) {
                line += "  -  roughly " + DurationText((int64_t)(st.etaHours * 3600000.0)) +
                        " to go";
            }
        }
        if (st.skipped) line += "  -  " + std::to_string(st.skipped) + " skipped";
        ImGui::TextDisabled("%s", line.c_str());

        // Whether it is safe is the one thing worth always being able to see.
        const std::string saveErr = SessionSaveError();
        const long long savedAt = SessionLastSavedMs();
        if (!saveErr.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f),
                "NOT SAVING: %s - retrying", saveErr.c_str());
        else if (savedAt) {
            const long long ago = (NowSteadyMs() - savedAt) / 1000;
            if (ago < 2)
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "saved");
            else
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "saved %llds ago",
                    (long long)ago);
        }
        else
            ImGui::TextDisabled("not saved yet");
        ImGui::SameLine();
        ImGui::TextDisabled("- saves itself as you build; a crash or relog loses nothing");

        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Put it away", ImVec2(120, 0))) SessionClose();
        ImGui::SameLine();
        if (ImGui::Button("Make a copy")) SessionDuplicate(id, false);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A second, fresh copy of this build - to put the same\n"
                              "thing up somewhere else. This one is left as it is.");
        ImGui::EndDisabled();
    }
    else if (!busy) {
        ImGui::TextDisabled("No build open. Start one from a prefab, or pick up one you saved.");
    }

    if (busy) ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "working...");
    const std::string status = SessionStatus();
    if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());

    // Standing next to a build you are not working on is almost always because
    // you came back to work on it.
    BuildSessionInfo nearBy;
    if (!busy && SessionNearby(nearBy, 96)) {
        const int pct = nearBy.blocks ? (int)((nearBy.done + nearBy.skipped) * 100 / nearBy.blocks) : 0;
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "You are right by \"%s\" (%d%% done).",
            nearBy.name.c_str(), pct);
        ImGui::SameLine();
        if (ImGui::SmallButton("Pick it up")) SessionOpen(nearBy.id);
    }

    // ---- saved builds ------------------------------------------------------
    const std::vector<BuildSessionInfo> list = SessionList();
    int others = 0;
    for (const BuildSessionInfo& s : list) if (!s.active) ++others;

    if (others > 0) {
        char head[64];
        snprintf(head, sizeof(head), "Saved builds (%d)###savedbuilds", others);
        if (ImGui::TreeNodeEx(head, haveActive ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
            double px = 0, py = 0, pz = 0;
            const bool havePos = UiPlayerPos(px, py, pz);

            if (ImGui::BeginTable("sessions", 5,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
                    ImGuiTableFlags_ScrollY, ImVec2(0, (std::min)(200.0f, 34.0f + 26.0f * others)))) {
                ImGui::TableSetupColumn("build", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("done");
                ImGui::TableSetupColumn("where");
                ImGui::TableSetupColumn("last");
                ImGui::TableSetupColumn("");
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (const BuildSessionInfo& s : list) {
                    if (s.active) continue;
                    ImGui::PushID(s.id.c_str());
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.name.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s\nfrom %s, %zu blocks\nanchor %d, %d, %d\n%s spent placing%s",
                            s.note.empty() ? "(no note)" : s.note.c_str(),
                            s.prefabName.c_str(), s.blocks, s.originX, s.originY, s.originZ,
                            DurationText(s.activeMs).c_str(),
                            s.fromBackup ? "\nits newest save was damaged - the one before will be used" : "");
                    }
                    if (!s.note.empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("- %s", s.note.c_str());
                    }

                    ImGui::TableNextColumn();
                    const float f = s.blocks ? (float)(s.done + s.skipped) / (float)s.blocks : 0.0f;
                    char pct[16];
                    snprintf(pct, sizeof(pct), "%.0f%%", f * 100.0f);
                    ImGui::ProgressBar(f, ImVec2(70, 0), pct);

                    ImGui::TableNextColumn();
                    if (havePos) {
                        const std::string dir = BuildDirectionText(s.originX + 0.5 - px,
                            s.originY - py, s.originZ + 0.5 - pz);
                        ImGui::TextUnformatted(dir.c_str());
                    }
                    else
                        ImGui::TextDisabled("%d, %d, %d", s.originX, s.originY, s.originZ);

                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", AgoText(s.updatedUnix).c_str());

                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(busy);
                    if (ImGui::SmallButton("Open")) SessionOpen(s.id);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Copy")) SessionDuplicate(s.id, false);
                    ImGui::SameLine();
                    if (ConfirmButton("Delete", "Sure? Delete"))
                        SessionDelete(s.id);
                    ImGui::EndDisabled();

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("kept in %s", SessionFolder().c_str());
            ImGui::TreePop();
        }
    }

    // ---- a new one -----------------------------------------------------------
    if (ImGui::TreeNodeEx("Start a new build from a prefab",
            haveActive ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
        static int picked = -1;
        const std::vector<std::string> files = PrefabGetFiles();
        if (picked >= (int)files.size()) picked = -1;

        auto shortName = [](const std::string& path) {
            const size_t slash = path.find_last_of("\\/");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        };

        ImGui::SetNextItemWidth(300);
        const std::string preview = picked >= 0 ? shortName(files[(size_t)picked])
                                                : std::string("select a prefab...");
        if (ImGui::BeginCombo("##prefab", preview.c_str())) {
            for (int i = 0; i < (int)files.size(); ++i) {
                const bool sel = (i == picked);
                if (ImGui::Selectable(shortName(files[(size_t)i]).c_str(), sel)) picked = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(picked < 0 || busy);
        if (ImGui::Button("Start building it")) SessionStartNew(files[(size_t)picked]);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) PrefabScanFolder();

        if (files.empty())
            ImGui::TextDisabled("No .json files yet - put some in %s", PrefabGetFolder().c_str());
        else
            ImGui::TextDisabled("It appears at your feet. Whatever you had open is saved first.");
        ImGui::TreePop();
    }
}

// -----------------------------------------------------------------------------
// Catching up, and rolling back
// -----------------------------------------------------------------------------
void DrawCatchUpSection() {
    if (!ImGui::CollapsingHeader("Catch up, rewind, start over")) return;

    ImGui::TextDisabled(
        "Built part of it while nothing was watching? Mark it off in bulk. Every\n"
        "one of these is a single step that Undo takes back.");

    static std::string result;
    auto report = [](int n, const char* what) {
        result = std::to_string(n) + " block(s) " + what;
    };

    if (ConfirmButton("Mark this layer done", "Sure? Mark it done"))
        report(BuildBulkSet(BuildBulk::ThisLayer, 0, BuildBlockState::Done), "marked done");
    ImGui::SameLine();
    if (ConfirmButton("Mark every layer below this one done", "Sure? All of them"))
        report(BuildBulkSet(BuildBulk::BelowLayer, 0, BuildBlockState::Done), "marked done");

    static int radius = 6;
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("##radius", &radius, 1, 4);
    radius = (std::max)(1, (std::min)(64, radius));
    ImGui::SameLine();
    char nearLabel[80];
    snprintf(nearLabel, sizeof(nearLabel), "Mark everything within %d blocks of me done", radius);
    if (ConfirmButton(nearLabel, "Sure? Mark them done"))
        report(BuildBulkSet(BuildBulk::NearMe, radius, BuildBlockState::Done), "marked done");

    if (ConfirmButton("Put this layer back to not done", "Sure? Reopen the layer"))
        report(BuildBulkSet(BuildBulk::ThisLayer, 0, BuildBlockState::Todo), "reopened");

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Server restarted and lost the last few minutes? Rewind the guide to match.\n"
        "Or turn on \"show blocks already placed\" below: any green cube you can see\n"
        "is a block the server lost - place it again and nothing else needs doing.");
    static int minutes = 5;
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("minutes", &minutes, 1, 5);
    minutes = (std::max)(1, (std::min)(24 * 60, minutes));
    ImGui::SameLine();
    const int wouldUndo = BuildChangedInLastMinutes(minutes);
    ImGui::BeginDisabled(wouldUndo == 0);
    char rewindLabel[80];
    snprintf(rewindLabel, sizeof(rewindLabel), "Rewind %d change(s)", wouldUndo);
    if (ConfirmButton(rewindLabel, "Sure? Rewind"))
        report(BuildRewindMinutes(minutes), "put back - Redo restores them");
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ConfirmButton("Start over", "Sure? Everything back to not done"))
        report(BuildBulkSet(BuildBulk::Everything, 0, BuildBlockState::Todo),
            "reset - Undo brings them back");

    if (!result.empty()) ImGui::TextDisabled("%s", result.c_str());
}

// -----------------------------------------------------------------------------
// Wrong blocks
// -----------------------------------------------------------------------------
void DrawMistakes() {
    const std::vector<BuildMistake> ms = BuildGetMistakes();
    if (ms.empty()) return;

    if (!ms.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f),
            "Wrong block%s (%zu) - drawn red in the world", ms.size() == 1 ? "" : "s", ms.size());
        ImGui::TextDisabled("Break it and place the right one and this clears by itself.");

        const std::shared_ptr<const Prefab> p = PrefabCurrent();
        if (ImGui::BeginTable("mistakes", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, (std::min)(150.0f, 12.0f + 26.0f * (float)ms.size())))) {
            ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            for (size_t i = 0; i < ms.size(); ++i) {
                const BuildMistake& m = ms[i];
                std::string wanted = "?";
                if (p && m.index < p->blocks.size()) {
                    const int32_t id = p->blocks[m.index].nameId;
                    if (id >= 0 && (size_t)id < p->names.size()) wanted = p->names[(size_t)id];
                }
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d, %d, %d:", m.wx, m.wy, m.wz);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.55f, 1.0f), "%s", m.placedName.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("wants");
                ImGui::SameLine();
                ImGui::TextUnformatted(wanted.c_str());
                ImGui::TableNextColumn();
                bool acted = false;
                if (ImGui::SmallButton("It's fine")) { BuildAcceptMistake(i, false); acted = true; }
                if (!acted && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Count this one as placed.");
                ImGui::SameLine();
                if (!acted && ImGui::SmallButton("Use it for all")) { BuildAcceptMistake(i, true); acted = true; }
                if (!acted && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Use %s everywhere this build wants %s.\n"
                        "That is a swap: the ghost, the lists and the check all follow it.\n"
                        "Undo it in Materials.", m.placedName.c_str(), wanted.c_str());
                ImGui::SameLine();
                if (!acted && ImGui::SmallButton("Dismiss")) { BuildDismissMistake(i); acted = true; }
                ImGui::PopID();
                if (acted) break;      // the list just changed under us
            }
            ImGui::EndTable();
        }
        if (ms.size() > 1 && ImGui::SmallButton("Dismiss all")) BuildClearMistakes();
    }
}

// -----------------------------------------------------------------------------
// Picking your own material for one of the prefab's
// -----------------------------------------------------------------------------
// Two ways in, the quick one first: place the block you want to use anywhere,
// and it is offered here by name. Or search the game's own list of blocks.
void DrawSwapPicker(const std::string& wanted, const std::string& mine) {
    ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Use instead of %s", wanted.c_str());

    const std::string last = BuildLastPlacedName();
    if (!last.empty() && last != mine) {
        if (ImGui::Button(("the block I last placed:  " + last).c_str())) {
            BuildSetSwap(wanted, last);
            ImGui::CloseCurrentPopup();
        }
    }
    else if (last.empty())
        ImGui::TextDisabled("Quickest: place the block you want to use anywhere, then\n"
                            "open this again - it is offered right here.");

    static char filter[64] = "";
    ImGui::SetNextItemWidth(320);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##swapfilter", "or search block names...", filter, sizeof(filter));

    const std::vector<std::string> names = BuildBlockNameList();
    if (names.empty()) {
        ImGui::TextDisabled("No block name table yet - use the last placed block instead.");
    }
    else {
        const std::string f = ToLower(filter);
        ImGui::BeginChild("##swaplist", ImVec2(320, 200), true);
        int shown = 0;
        for (const std::string& n : names) {
            if (!f.empty() && ToLower(n).find(f) == std::string::npos) continue;
            if (++shown > 300) { ImGui::TextDisabled("... keep typing to narrow it"); break; }
            float r = 1, g = 1, b = 1;
            BuildColourForName(n, r, g, b);
            ImGui::ColorButton(("##c" + n).c_str(), ImVec4(r, g, b, 1.0f),
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(12, 12));
            ImGui::SameLine();
            if (ImGui::Selectable(n.c_str(), n == mine)) {
                BuildSetSwap(wanted, n);
                filter[0] = 0;
                ImGui::CloseCurrentPopup();
            }
        }
        if (!shown) ImGui::TextDisabled("nothing matches");
        ImGui::EndChild();
    }

    if (mine != wanted) {
        if (ImGui::Button(("Back to " + wanted).c_str())) {
            BuildSetSwap(wanted, "");
            ImGui::CloseCurrentPopup();
        }
    }
}

} // namespace

static void DrawBuildTab() {
    ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Build guide");
    ImGui::TextDisabled(
        "Every block of a prefab outlined in the world in its own colour, and\n"
        "every build saved as you go. Nothing is ever placed for you.");
    DrawSetupStrip();
    ImGui::Spacing();

    DrawSessionsSection();
    DrawAiPrefabSection();

    const std::shared_ptr<const Prefab> p = PrefabCurrent();
    if (!p) return;
    DrawPreview3dRow(p);

    // -------------------------------------------------------------------
    // 2. Where it goes - adjusted while looking at it, not before
    // -------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::SeparatorText("2. Put it where you want it");

    ImGui::Text("Anchor  %d, %d, %d", g_buildOriginX.load(),
        g_buildOriginY.load(), g_buildOriginZ.load());
    ImGui::SameLine();
    ImGui::TextDisabled("- turned %d%c  -  %s, %d x %d x %d", g_buildRotation.load() * 90, 0xB0,
        p->name.c_str(), p->SizeX(), p->SizeY(), p->SizeZ());

    const bool locked = g_buildAnchorLocked.load();
    {
        bool l = locked;
        if (ImGui::Checkbox("Locked in place", &l)) g_buildAnchorLocked.store(l);
        ImGui::SameLine();
        ImGui::TextDisabled(locked
            ? "- unlock to move it; blocks already placed move with it"
            : "- locks itself once the first block you place is confirmed");
    }

    ImGui::BeginDisabled(locked);
    if (ImGui::Button("Put it where I am standing", ImVec2(230, 0))) BuildSetOriginToPlayer();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!g_buildActive.load()) {
        if (ImGui::Button("Show the ghost", ImVec2(150, 0))) BuildStart();
    }
    else {
        if (ImGui::Button("Hide the ghost", ImVec2(150, 0))) BuildStop();
    }

    if (!locked) {
        ImGui::Spacing();
        AnchorPad();

        ImGui::SetNextItemWidth(120);
        int step = g_buildNudgeStep.load();
        if (ImGui::InputInt("blocks per nudge", &step))
            g_buildNudgeStep.store((std::max)(1, (std::min)(64, step)));

        bool nudgeKeys = g_buildNudgeMode.load();
        if (ImGui::Checkbox("Arrow keys move the build", &nudgeKeys))
            g_buildNudgeMode.store(nudgeKeys);
        ImGui::SameLine();
        ImGui::TextDisabled("arrows, PgUp/PgDn for height, Home to turn");
        if (nudgeKeys)
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "the game will not see the arrow keys while this is on");
    }

    // -------------------------------------------------------------------
    // 3. Building it
    // -------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::SeparatorText("3. Build it");

    const BuildTarget t = BuildGetTarget();
    if (t.valid) {
        BlockSwatch(t.name, 18.0f);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.75f, 1.0f), "%s", t.name.c_str());
        if (t.name != t.wanted) {
            ImGui::SameLine();
            ImGui::TextDisabled("(your swap for %s)", t.wanted.c_str());
        }
        const int slot = BuildGetMaterialSlot(t.name);
        if (slot > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "[slot %d]", slot);
        }
        ImGui::TextDisabled("at %d, %d, %d  -  %s%s", t.wx, t.wy, t.wz,
            BuildDirectionText(t.dx, t.dy, t.dz).c_str(),
            BuildRotationLabel(t.rotation, t.name).c_str());
    }
    else {
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "Nothing left to place - it is finished.");
    }

    ImGui::Spacing();
    DrawFollowAlong();

    // The prefab as a shape rather than a shopping list. Follow along keeps
    // working either way; this only decides whether the material matters.
    {
        bool any = !g_buildCheckMaterial.load();
        if (ImGui::Checkbox("Any block counts", &any)) BuildSetAnyBlock(any);
        ImGui::SameLine();
        if (any)
            ImGui::TextDisabled("- build the shape in whatever you like; nothing turns red");
        else if (PlaceIdDeltaKnown())
            ImGui::TextDisabled("- a block other than the one asked for (or your swap) turns red");
        else
            ImGui::TextDisabled("- wrong blocks turn red once a few right ones have been seen");
    }

    ImGui::Spacing();
    if (ImGui::Button("Placed it", ImVec2(110, 0))) BuildMarkDone(1);
    ImGui::SameLine();
    if (ImGui::Button("Skip", ImVec2(70, 0))) BuildSkip(1);
    ImGui::SameLine();
    ImGui::BeginDisabled(BuildUndoDepth() == 0);
    if (ImGui::Button("Undo", ImVec2(70, 0))) BuildUndo(1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(BuildRedoDepth() == 0);
    if (ImGui::Button("Redo", ImVec2(70, 0))) BuildRedo(1);
    ImGui::EndDisabled();
    const std::string last = BuildLastActionText();
    if (!last.empty()) ImGui::TextDisabled("last: %s", last.c_str());
    ImGui::TextDisabled("in game:  ]  placed it    [  undo    \\  skip   (not needed while follow along works)");

    // ---- layers ---------------------------------------------------------
    {
        const BuildLayerInfo li = BuildGetLayerInfo();
        if (li.valid) {
            ImGui::Spacing();
            if (ImGui::ArrowButton("##ldown", ImGuiDir_Down)) BuildStepLayer(-1);
            ImGui::SameLine();
            ImGui::Text("Layer %d of %d", li.position, li.count);
            ImGui::SameLine();
            if (ImGui::ArrowButton("##lup", ImGuiDir_Up)) BuildStepLayer(1);
            ImGui::SameLine();
            if (li.todoHere)
                ImGui::TextDisabled("%d of %d left on this layer", li.todoHere, li.totalHere);
            else
                ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "this layer is done");
            if (li.anyUnfinished && li.lowestUnfinished != li.current) {
                ImGui::SameLine();
                if (ImGui::SmallButton("lowest unfinished")) BuildJumpToLayer(li.lowestUnfinished);
            }

            bool layerOnly = g_buildLayerOnly.load();
            if (ImGui::Checkbox("Stay on this layer", &layerOnly)) g_buildLayerOnly.store(layerOnly);
            ImGui::SameLine();
            int order = g_buildOrderMode.load();
            ImGui::SetNextItemWidth(250);
            if (ImGui::Combo("order", &order,
                    "row by row\0layer by layer, one material at a time\0one material at a time\0"))
                g_buildOrderMode.store(order);
        }
    }

    DrawMistakes();

    ImGui::Spacing();
    DrawCatchUpSection();

    // -------------------------------------------------------------------
    // Materials: what is left, and what you are using for each
    // -------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::SeparatorText("Materials");
    {
        ImGui::TextDisabled(
            "Swap any of the prefab's blocks for your own - the ghost recolours, and\n"
            "the list, the target and the wrong-block check all follow.");

        static bool thisLayer = false;
        if (ImGui::RadioButton("still needed, whole build", !thisLayer)) thisLayer = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("this layer", thisLayer)) thisLayer = true;

        const std::vector<BuildMaterialRow> rows = BuildMaterialRows(thisLayer);
        ImGui::SameLine();
        ImGui::BeginDisabled(rows.empty());
        if (ImGui::SmallButton("Copy the list")) {
            // A shopping list: by what you are actually using, two prefab
            // materials swapped to the same block counted together.
            std::vector<std::pair<std::string, int>> totals;
            for (const BuildMaterialRow& r : rows) {
                bool found = false;
                for (auto& tt : totals) if (tt.first == r.mine) { tt.second += r.left; found = true; }
                if (!found) totals.emplace_back(r.mine, r.left);
            }
            std::sort(totals.begin(), totals.end(),
                [](const auto& x, const auto& y) { return x.second > y.second; });
            std::string text = p->name + (thisLayer ? " - this layer\n" : " - still needed\n");
            for (const auto& tt : totals) text += std::to_string(tt.second) + "  " + tt.first + "\n";
            ImGui::SetClipboardText(text.c_str());
        }
        ImGui::EndDisabled();

        if (rows.empty())
            ImGui::TextDisabled(thisLayer ? "nothing left on this layer" : "none - it is finished");
        else if (ImGui::BeginTable("mats", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_ScrollY, ImVec2(0, 210))) {

            ImGui::TableSetupColumn("");
            ImGui::TableSetupColumn("block", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("left");
            ImGui::TableSetupColumn("your material");
            ImGui::TableSetupColumn("hotbar");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (const BuildMaterialRow& m : rows) {
                ImGui::PushID(m.wanted.c_str());
                ImGui::TableNextRow();
                const bool swapped = m.mine != m.wanted;

                ImGui::TableNextColumn();
                BlockSwatch(m.mine);

                ImGui::TableNextColumn();
                const bool isNext = t.valid && t.wanted == m.wanted;
                const ImVec4 hi(0.95f, 0.95f, 0.75f, 1.0f);
                if (swapped) {
                    if (isNext) ImGui::TextColored(hi, "%s", m.mine.c_str());
                    else        ImGui::TextUnformatted(m.mine.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("for %s", m.wanted.c_str());
                }
                else if (isNext) ImGui::TextColored(hi, "%s", m.wanted.c_str());
                else             ImGui::TextUnformatted(m.wanted.c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%d", m.left);

                ImGui::TableNextColumn();
                if (ImGui::SmallButton(swapped ? "change" : "swap...")) ImGui::OpenPopup("swap");
                if (swapped) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("undo swap")) BuildSetSwap(m.wanted, "");
                }
                if (ImGui::BeginPopup("swap")) {
                    DrawSwapPicker(m.wanted, m.mine);
                    ImGui::EndPopup();
                }

                ImGui::TableNextColumn();
                int slot = BuildGetMaterialSlot(m.mine);
                ImGui::SetNextItemWidth(60);
                if (ImGui::InputInt("##slot", &slot, 0, 0))
                    BuildSetMaterialSlot(m.mine, (std::max)(0, (std::min)(9, slot)));

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("the colour is how that block is outlined in the world");

        // Built it your own way? Keep that way.
        if (ImGui::TreeNode("Save my version as a prefab")) {
            ImGui::TextDisabled(
                "Writes this build with the materials you used: what the game saw you\n"
                "place where it knows, your swaps everywhere else. It goes in the prefabs\n"
                "folder, next to the original - which is never overwritten.");
            static std::string shownFor;
            static char verName[64] = "";
            if (shownFor != p->name) {
                shownFor = p->name;
                strncpy_s(verName, (p->name + "_mine").c_str(), _TRUNCATE);
            }
            ImGui::SetNextItemWidth(220);
            ImGui::InputText("##vername", verName, sizeof(verName));
            ImGui::SameLine();
            static std::string verMsg;
            if (ImGui::Button("Save it")) {
                std::string err;
                size_t known = 0;
                if (BuildSaveMyVersion(verName, err, known))
                    verMsg = err + " - " + std::to_string(known) + " block(s) as you placed them";
                else
                    verMsg = "could not save: " + err;
            }
            if (!verMsg.empty()) ImGui::TextDisabled("%s", verMsg.c_str());
            ImGui::TreePop();
        }
    }

    // -------------------------------------------------------------------
    // Everything that has a sensible default
    // -------------------------------------------------------------------
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("How it looks")) {
        int mode = g_buildShowMode.load();
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("show", &mode,
            "the whole build\0the layer I am on\0just the next few\0"))
            g_buildShowMode.store(mode);

        if (mode == (int)BuildShow::NextFew) {
            int n = g_buildGhostCount.load();
            ImGui::SetNextItemWidth(220);
            if (ImGui::SliderInt("how many", &n, 1, 64)) g_buildGhostCount.store(n);
        }

        bool showDone = g_buildShowDone.load();
        if (ImGui::Checkbox("show blocks already placed (green)", &showDone))
            g_buildShowDone.store(showDone);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Each placed block gets a small green cube INSIDE it. Where the block\n"
                "is there, you cannot see the cube. Where the server lost it - after a\n"
                "restart or a rollback - you can. Place those again and you are done.");

        bool byType = g_buildColourByType.load();
        if (ImGui::Checkbox("colour each block by its type", &byType))
            g_buildColourByType.store(byType);

        bool fade = g_buildFadeOtherLayers.load();
        if (ImGui::Checkbox("fade the layers you are not on", &fade))
            g_buildFadeOtherLayers.store(fade);

        float scale = g_buildGhostScale.load();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("cube size", &scale, 0.2f, 1.5f, "%.2f"))
            g_buildGhostScale.store(scale);

        float opNear = g_buildOpacityNear.load(), opFar = g_buildOpacityFar.load();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("this layer", &opNear, 0.05f, 1.0f, "%.2f opacity"))
            g_buildOpacityNear.store(opNear);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("other layers", &opFar, 0.02f, 1.0f, "%.2f opacity"))
            g_buildOpacityFar.store(opFar);

        ImGui::TextDisabled("%s", BuildGhostStatus().c_str());
    }

    if (ImGui::CollapsingHeader("If the outline sits half a block out")) {
        ImGui::TextDisabled(
            "Block coordinates usually name a corner, so the cube is drawn half\n"
            "a block along each axis. That is an assumption, and it is obvious on\n"
            "screen when it is wrong - so it is a number rather than a guess.");
        float nx = g_buildNudgeX.load(), ny = g_buildNudgeY.load(),
              nz = g_buildNudgeZ.load();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("x offset", &nx, -1.0f, 1.0f, "%.2f"))
            g_buildNudgeX.store(nx);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("y offset", &ny, -1.0f, 1.0f, "%.2f"))
            g_buildNudgeY.store(ny);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("z offset", &nz, -1.0f, 1.0f, "%.2f"))
            g_buildNudgeZ.store(nz);
    }

    if (ImGui::CollapsingHeader("Drawing capacity")) {
        bool autoCap = g_buildAutoCapacity.load();
        if (ImGui::Checkbox("size the shape list to fit the build", &autoCap))
            g_buildAutoCapacity.store(autoCap);
        ImGui::SameLine();
        ImGui::TextDisabled("%d slots, %d wanted",
            DbgListCapacity(), g_dbgWantedCapacity.load());

        ImGui::BeginDisabled(autoCap);
        int cap = g_dbgWantedCapacity.load();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("shape slots", &cap, 8, kDbgMaxCapacity, "%d",
            ImGuiSliderFlags_Logarithmic))
            g_dbgWantedCapacity.store(cap);
        ImGui::EndDisabled();

        // The dial between "appears at once and the game hitches" and "fills in
        // smoothly". Every shape is an object allocation on the game thread.
        int perFrame = g_dbgSpawnPerFrame.load();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("shapes drawn per frame", &perFrame, 16, 512))
            g_dbgSpawnPerFrame.store(perFrame);
        ImGui::TextDisabled("lower this if a big build makes the game stutter");

        const size_t waiting = DbgSpawnQueueDepth();
        if (waiting)
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                "%zu still to draw", waiting);

        bool autoOn = g_dbgAutoPrepare.load();
        if (ImGui::Checkbox("set the shape system up automatically", &autoOn))
            g_dbgAutoPrepare.store(autoOn);
    }
}

// Set by the "open chat" hotkey; the tab bar selects Chat on the next frame.
static bool g_selectChatTab = false;

void RenderOverlay() {
    // Full Josebot UI only. Compact mode is handled exclusively in OverlayThread
    // (never call this function while g_compactUi is true).

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    // This makes ImGui cleanly cover the entire Native window, disabling ImGui's internal resize/move
    // Flush to edges — no outer padding so the custom bar meets the top of the HWND
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("HyAIMain", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    ImGui::PopStyleVar(); // padding only affects Begin; content below can pad itself

    // ---- Custom title bar — sleek, modern, status-aware ----
    {
        const float barH = 36.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
        ImVec2 barStart = ImGui::GetCursorScreenPos();
        float barW = ImGui::GetContentRegionAvail().x;
        ImDrawList* dlBg = ImGui::GetWindowDrawList();

        // Subtle gradient-ish fill (solid + soft bottom edge)
        dlBg->AddRectFilled(
            barStart,
            ImVec2(barStart.x + barW, barStart.y + barH),
            ImGui::GetColorU32(ImGuiCol_TitleBgActive));
        dlBg->AddLine(
            ImVec2(barStart.x, barStart.y + barH - 1.0f),
            ImVec2(barStart.x + barW, barStart.y + barH - 1.0f),
            ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);

        // Drag zone (leave room for Compact + min + close)
        ImGui::InvisibleButton("##titledrag", ImVec2((std::max)(0.0f, barW - 168.0f), barH));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (g_hwndOverlay) {
                ReleaseCapture();
                SendMessageW(g_hwndOverlay, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && g_hwndOverlay) {
            WINDOWPLACEMENT wp{ sizeof(wp) };
            GetWindowPlacement(g_hwndOverlay, &wp);
            ShowWindow(g_hwndOverlay, (wp.showCmd == SW_MAXIMIZE) ? SW_RESTORE : SW_MAXIMIZE);
        }

        // Brand
        ImGui::SetCursorScreenPos(ImVec2(barStart.x + 14.0f, barStart.y + 8.0f));
        ImGui::TextColored(ImVec4(0.78f, 0.85f, 0.98f, 1.0f), "HyAI");
        ImGui::SameLine(0, 6);
        ImGui::TextColored(ImVec4(0.45f, 0.50f, 0.60f, 1.0f), "v3.2");

        // Status pills (compact, color-coded)
        auto statusPill = [&](const char* label, ImVec4 col) {
            ImGui::SameLine(0, 10);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 ts = ImGui::CalcTextSize(label);
            float padX = 7.0f, padY = 2.0f;
            dlBg->AddRectFilled(
                ImVec2(p.x - padX * 0.3f, p.y - padY),
                ImVec2(p.x + ts.x + padX, p.y + ts.y + padY),
                ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.18f)), 4.0f);
            ImGui::TextColored(col, "%s", label);
            };

        if (g_pChat.load() && g_stringVT.load() && g_gameThread.load())
            statusPill("READY", ImVec4(0.35f, 0.92f, 0.55f, 1.0f));
        else
            statusPill("SETUP", ImVec4(1.0f, 0.72f, 0.30f, 1.0f));

        {
            bool muted = false;
            { std::lock_guard lock(g_settingsMtx); muted = g_botMuted; }
            const int waitingForMe = AiFlow::WaitingCount();
            if (muted)
                statusPill("MUTED", ImVec4(1.0f, 0.50f, 0.35f, 1.0f));
            else if (waitingForMe > 0) {
                char pill[32];
                snprintf(pill, sizeof(pill), "%d TO REVIEW", waitingForMe);
                statusPill(pill, ImVec4(1.0f, 0.80f, 0.30f, 1.0f));
            }
            else if (g_aiInProgress)
                statusPill("THINKING", ImVec4(1.0f, 0.72f, 0.35f, 1.0f));
            else if (g_txInProgress)
                statusPill("TRANSLATING", ImVec4(0.50f, 0.78f, 1.0f, 1.0f));
            else
                statusPill("IDLE", ImVec4(0.52f, 0.62f, 0.78f, 1.0f));
        }
        if (const int unread = ChatUi::UnreadMentions()) {
            char pill[32];
            snprintf(pill, sizeof(pill), unread == 1 ? "1 MENTION" : "%d MENTIONS", unread);
            statusPill(pill, ImVec4(1.0f, 0.80f, 0.30f, 1.0f));
        }

        if (g_aiInProgress) {
            ImGui::SameLine(0, 8);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.20f, 0.20f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.28f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.92f, 0.34f, 0.34f, 1.0f));
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, barStart.y + 6.0f));
            if (ImGui::SmallButton("Cancel"))
                CancelAI();
            ImGui::PopStyleColor(3);
        }

        // Right-side window controls
        ImGui::SetCursorScreenPos(ImVec2(barStart.x + barW - 158.0f, barStart.y + 5.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.20f, 0.30f, 0.75f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.38f, 0.55f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.36f, 0.48f, 0.68f, 1.0f));
        if (ImGui::SmallButton("Compact"))
            ToggleCompactChatMode();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Small see-through chat window (hotkey in Settings > Hotkeys)");
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 6);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.32f, 0.45f, 0.75f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.38f, 0.42f, 0.55f, 1.0f));
        if (ImGui::Button("—", ImVec2(28, 24)) && g_hwndOverlay)
            ShowWindow(g_hwndOverlay, SW_MINIMIZE);
        ImGui::SameLine(0, 2);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.22f, 0.22f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.92f, 0.28f, 0.28f, 1.0f));
        if (ImGui::Button("×", ImVec2(28, 24)) && g_hwndOverlay) {
            g_show = false;
            ShowWindow(g_hwndOverlay, SW_HIDE);
        }
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar();

        ImGui::SetCursorScreenPos(ImVec2(barStart.x, barStart.y + barH));
        ImGui::Dummy(ImVec2(0, 10.0f));
        ImGui::Indent(14.0f);
    }

    if (ImGui::BeginTabBar("##hyaitabs", ImGuiTabBarFlags_FittingPolicyScroll)) {

        // Selected from elsewhere: the "open chat" hotkey, or a settings link.
        const bool selectSettings = UiTakeSettingsTabRequest();
        const bool selectStudio = UiTakeStudioTabRequest();
        const bool selectChat = g_selectChatTab;
        g_selectChatTab = false;

        {
            char chatLabel[48];
            const int unread = ChatUi::UnreadMentions();
            if (unread > 0) snprintf(chatLabel, sizeof(chatLabel), "Chat (%d)###Chat", unread);
            else snprintf(chatLabel, sizeof(chatLabel), "Chat###Chat");
            if (ImGui::BeginTabItem(chatLabel, nullptr, selectChat ? ImGuiTabItemFlags_SetSelected : 0)) {
                ChatUi::DrawChatTab();
                ImGui::EndTabItem();
            }
        }

        if (g_prefs.tabIgnored && ImGui::BeginTabItem("Ignored")) {
            ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Ignore list");
            ImGui::TextDisabled("Players here never reach the AI — saves tokens and noise.");
            ImGui::Spacing();

            ImGui::InputTextWithHint("##ignoreadd", "Player name to ignore…", g_ignoreInput, sizeof(g_ignoreInput));
            ImGui::SameLine();
            if (ImGui::Button("Ignore", ImVec2(80, 0)) && g_ignoreInput[0]) {
                std::string name = NormalizeSpeaker(g_ignoreInput);
                if (!name.empty()) {
                    {
                        std::lock_guard lock(g_mtx);
                        g_ignored.insert(ToLower(name));
                    }
                    SaveIgnored();
                    g_ignoreInput[0] = 0;
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Suggested from chat");
            ImGui::TextDisabled("Real player names from recent lines. No announcements, no duplicates.");
            ImGui::SetNextItemWidth(220);
            ImGui::InputTextWithHint("##suggestfilter", "Filter suggestions...", g_suggestFilter, sizeof(g_suggestFilter));

            std::vector<std::string> suggestions;
            std::set<std::string> ignoredCopy;
            {
                std::lock_guard lock(g_mtx);
                ignoredCopy = g_ignored;
                const std::string filter = ToLower(TrimCopy(g_suggestFilter));
                for (auto it = g_seenPlayers.rbegin(); it != g_seenPlayers.rend(); ++it) {
                    if (ignoredCopy.count(it->key)) continue;
                    if (IsBotIdentity(it->key) || IsReservedSpeakerName(it->key)) continue;
                    if (!filter.empty() && it->key.find(filter) == std::string::npos &&
                        ToLower(it->display).find(filter) == std::string::npos)
                        continue;
                    suggestions.push_back(it->display);
                }
            }

            const float listH = ImGui::GetContentRegionAvail().y * 0.45f;
            ImGui::BeginChild("##suggestions", ImVec2(0, listH), true);
            if (suggestions.empty()) {
                ImGui::TextDisabled("No unused player names yet. They appear here as people talk.");
            }
            else {
                std::string addName;
                for (const auto& n : suggestions) {
                    ImGui::PushID(n.c_str());
                    ImGui::Text("%s", n.c_str());
                    ImGui::SameLine(ImGui::GetWindowWidth() - 90);
                    if (ImGui::SmallButton("Ignore"))
                        addName = n;
                    ImGui::PopID();
                }
                if (!addName.empty()) {
                    {
                        std::lock_guard lock(g_mtx);
                        g_ignored.insert(ToLower(NormalizeSpeaker(addName)));
                    }
                    SaveIgnored();
                }
            }
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Currently ignored");
            ImGui::BeginChild("##ignoredlist", ImVec2(0, 0), true);

            std::vector<std::string> ignoredList;
            {
                std::lock_guard lock(g_mtx);
                ignoredList.assign(g_ignored.begin(), g_ignored.end());
            }
            std::vector<std::string> toRemove;
            if (ignoredList.empty()) {
                ImGui::TextDisabled("No one ignored yet.");
            }
            else {
                for (const auto& n : ignoredList) {
                    ImGui::PushID(n.c_str());
                    ImGui::Text("%s", n.c_str());
                    ImGui::SameLine(ImGui::GetWindowWidth() - 90);
                    if (ImGui::SmallButton("Unignore")) {
                        toRemove.push_back(n);
                    }
                    ImGui::PopID();
                }
            }

            for (const auto& n : toRemove) {
                {
                    std::lock_guard lock(g_mtx);
                    g_ignored.erase(n);
                }
                SaveIgnored();
            }

            ImGui::EndChild();

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Whitelist");
            ImGui::TextWrapped(
                "Ignore = never trigger AI.  Whitelist mode inverts this: ONLY listed players can wake the bot. "
                "Add people first, then enable the mode. Your account and bot lines are unaffected. "
                "Ignore still wins if someone is on both lists.");
            {
                bool wlMode = false;
                { std::lock_guard lock(g_settingsMtx); wlMode = g_whitelistMode; }
                if (ImGui::Checkbox("Whitelist mode (only reply to listed players)", &wlMode)) {
                    std::lock_guard lock(g_settingsMtx);
                    g_whitelistMode = wlMode;
                    g_settingsDirty = true;
                }
                if (wlMode)
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "ACTIVE — only whitelist can wake the bot");
                else
                    ImGui::TextDisabled("Off — normal mode (everyone can wake unless ignored)");
            }
            ImGui::InputTextWithHint("##wladd", "Name to whitelist...", g_whitelistInput, sizeof(g_whitelistInput));
            ImGui::SameLine();
            if (ImGui::Button("Add WL", ImVec2(80, 0)) && g_whitelistInput[0]) {
                std::string name = NormalizeSpeaker(g_whitelistInput);
                if (!name.empty()) {
                    { std::lock_guard lock(g_mtx); g_whitelist.insert(ToLower(name)); }
                    SaveWhitelist();
                    g_whitelistInput[0] = 0;
                }
            }
            ImGui::BeginChild("##wllist", ImVec2(0, 100), true);
            std::vector<std::string> wlList;
            {
                std::lock_guard lock(g_mtx);
                wlList.assign(g_whitelist.begin(), g_whitelist.end());
            }
            std::vector<std::string> toRemoveWl;
            if (wlList.empty()) {
                ImGui::TextDisabled("None");
            }
            else {
                for (const auto& n : wlList) {
                    ImGui::PushID(("wl" + n).c_str());
                    ImGui::Text("%s", n.c_str());
                    ImGui::SameLine(ImGui::GetWindowWidth() - 90);
                    if (ImGui::SmallButton("Remove")) toRemoveWl.push_back(n);
                    ImGui::PopID();
                }
            }
            for (const auto& n : toRemoveWl) {
                { std::lock_guard lock(g_mtx); g_whitelist.erase(n); }
                SaveWhitelist();
            }
            ImGui::EndChild();

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Settings", nullptr, selectSettings ? ImGuiTabItemFlags_SetSelected : 0)) {
            DrawSettingsTab();
            ImGui::EndTabItem();
        }

        if (g_prefs.tabStudio) {
            const int waiting = AiFlow::WaitingCount();
            char studioLabel[48];
            if (waiting > 0) snprintf(studioLabel, sizeof(studioLabel), "AI Studio (%d)###aistudio", waiting);
            else snprintf(studioLabel, sizeof(studioLabel), "AI Studio###aistudio");
            if (ImGui::BeginTabItem(studioLabel, nullptr, selectStudio ? ImGuiTabItemFlags_SetSelected : 0)) {
                DrawAiStudioTab();
                ImGui::EndTabItem();
            }
        }

        if (g_prefs.tabAi && ImGui::BeginTabItem("AI & Translation")) {
            size_t memSize = 0;
            size_t chatCtx = 0;
            size_t ignoredCount = 0;
            size_t seenCount = 0;
            {
                std::lock_guard lock(g_mtx);
                memSize = g_memory.size() / 2;
                chatCtx = g_globalChat.size();
                ignoredCount = g_ignored.size();
                seenCount = g_seenPlayers.size();
            }

            int curMaxMem = 4;
            int curMaxChat = 20;
            bool curColored = false, curNick = false;
            {
                std::lock_guard lock(g_settingsMtx);
                curMaxMem = g_maxMemory;
                curMaxChat = g_maxChatContext;
                curColored = g_coloredChat;
                curNick = g_useNickname;
            }

            ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Session");
            ImGui::Spacing();
            ImGui::Text("Uptime  %s", FormatUptime().c_str());
            ImGui::SameLine(0, 28);
            ImGui::Text("Wakes  %llu", (unsigned long long)g_wakeCount.load());
            ImGui::SameLine(0, 28);
            ImGui::TextColored(ImVec4(0.40f, 0.90f, 0.55f, 1.0f), "OK  %llu", (unsigned long long)g_replySuccess.load());
            ImGui::SameLine(0, 16);
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "Fail  %llu", (unsigned long long)g_replyFail.load());

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Columns(2, "##aimetrics", false);
            ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.48f);

            ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "AI context");
            ImGui::Spacing();
            ImGui::Text("Memory turns     %zu / %d", memSize, curMaxMem);
            ImGui::Text("Chat context     %zu / %d", chatCtx, curMaxChat);
            ImGui::Text("Ignored users    %zu", ignoredCount);
            ImGui::Text("Seen players     %zu", seenCount);
            ImGui::Text("Colored AI       %s", curColored ? "on" : "off");
            ImGui::Text("AI nickname      %s", curNick ? "on" : "off");
            ImGui::Text("Chunk size       %zu  (max %zu)", MAX_CHAT_CHUNK, HYTALE_CHAT_MAX);

            ImGui::NextColumn();

            ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Translation");
            ImGui::Spacing();
            ImGui::Text("API calls        %u", g_txCalls.load());
            ImGui::Text("Cache hits       %u", g_txCacheHits.load());
            ImGui::Text("Heuristic skips  %u", g_txSkipped.load());
            ImGui::Text("Cached strings   %zu", TxCacheSize());
            ImGui::Text("Queue size       %zu", TxQueueSize());

            ImGui::Columns(1);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "System prompt");
            ImGui::Spacing();
            std::string promptStatus = GetPromptStatus();
            std::string promptPath = GetPromptPathUtf8();
            if (IsPromptOk())
                ImGui::TextColored(ImVec4(0.40f, 0.92f, 0.55f, 1.0f), "%s", promptStatus.c_str());
            else
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", promptStatus.c_str());
            if (!promptPath.empty())
                ImGui::TextDisabled("%s", promptPath.c_str());
            ImGui::TextDisabled("Edits to CONTEXT.md are picked up on the next AI call.");

            ImGui::Spacing();
            if (ImGui::Button("Reload System Prompt", ImVec2(180, 0)))
                ReloadSystemPrompt(true);
            ImGui::SameLine();
            if (ImGui::Button("Reload All Settings", ImVec2(180, 0))) {
                LoadSettings();
                LoadIgnored();
                LoadWhitelist();
                AutoReply::Load();
                g_fontDirty = true;
                ReloadSystemPrompt(true);
                g_windowStyleDirty = true;
                g_themeDirty = true;
                std::lock_guard lock(g_settingsMtx);
                strncpy_s(g_localNameBuf, g_localName.c_str(), 63);
            }

            ImGui::EndTabItem();
        }

        // ================================================================
        // TAB: Test Chat  (private, ImGui-only — never sent to server)
        // ================================================================
        if (g_prefs.tabTest && ImGui::BeginTabItem("Test Chat")) {
            ImGui::TextColored(ImVec4(0.55f, 0.90f, 0.72f, 1.0f), "Private AI sandbox");
            ImGui::TextDisabled("Replies stay here only — nothing is sent to Hytale. Same model, tools, and CONTEXT.md.");
            ImGui::Spacing();

            {
                std::string st;
                { std::lock_guard lock(g_mtx); st = g_testStatus; }
                if (g_testAiInProgress.load())
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Status: thinking…");
                else if (!st.empty())
                    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Status: %s", st.c_str());
                else
                    ImGui::TextDisabled("Status: idle");
            }

            ImGui::SameLine(ImGui::GetWindowWidth() - 280);
            if (ImGui::Button("Clear Test Chat", ImVec2(130, 0))) {
                std::lock_guard lock(g_mtx);
                g_testChat.clear();
                g_testStatus.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear AI Memory", ImVec2(130, 0))) {
                std::lock_guard lock(g_mtx);
                g_memory.clear();
            }

            ImGui::Separator();

            float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
            ImGui::BeginChild("##testlog", ImVec2(0, -footer), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);

            std::vector<ChatMessage> copy;
            bool doScroll = false;
            {
                std::lock_guard lock(g_mtx);
                copy = g_testChat;
                doScroll = g_testScroll;
                g_testScroll = false;
            }

            for (size_t mi = 0; mi < copy.size(); ++mi) {
                const auto& m = copy[mi];
                ImGui::PushID((int)mi);
                if (m.role == "user") {
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "You:");
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.90f, 0.95f, 1.0f));
                    ImGui::TextWrapped("%s", m.content.c_str());
                    ImGui::PopStyleColor();
                }
                else {
                    ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f), "HyAI:");
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.95f, 0.80f, 1.0f));
                    ImGui::TextWrapped("%s", m.content.c_str());
                    ImGui::PopStyleColor();
                }
                // Right-click a message to copy it.
                if (ImGui::BeginPopupContextItem("##testmsg")) {
                    if (ImGui::MenuItem("Copy message")) ChatUi::SetClipboard(m.content);
                    if (ImGui::MenuItem("Copy whole conversation")) {
                        std::string all;
                        for (const auto& c : copy)
                            all += (c.role == "user" ? "You: " : "HyAI: ") + c.content + "\n";
                        ChatUi::SetClipboard(all);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::Spacing();
            }
            if (doScroll)
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
            bool enter = ImGui::InputTextWithHint("##testin",
                "Ask anything (tools + CONTEXT.md) — private only…",
                g_testInput, sizeof(g_testInput),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            bool click = ImGui::Button("Ask", ImVec2(80, 0));

            if ((enter || click) && g_testInput[0] && !g_testAiInProgress.load()) {
                std::string prompt = g_testInput;
                g_testInput[0] = 0;
                ImGui::SetKeyboardFocusHere(-1);
                HandleTestAIRequest(prompt);
            }

            ImGui::EndTabItem();
        }

        // ================================================================
        // TAB: Tools  (direct Knowledge tool calls — no AI)
        // ================================================================
        if (g_prefs.tabTools && ImGui::BeginTabItem("Tools")) {
            ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Direct tool calls");
            ImGui::TextDisabled("Same tools the AI uses — no LLM, instant results.  ·  %s", Knowledge::Status().c_str());
            ImGui::Spacing();

            const char* toolNames[] = {
                "lookup_mob", "search_mobs", "lookup_item", "search_items",
                "get_time", "set_reminder", "list_reminders", "cancel_reminder",
                "web_search"
            };
            ImGui::SetNextItemWidth(220);
            ImGui::Combo("Tool", &g_toolKind, toolNames, 9);

            if (g_toolKind <= 3 || g_toolKind == 4 || g_toolKind == 8) {
                ImGui::InputTextWithHint("##tq", "query / free text / timezone…", g_toolQuery, sizeof(g_toolQuery));
            }

            if (g_toolKind == 1) { // search_mobs
                ImGui::InputTextWithHint("##tdrops", "drops_item (optional)", g_toolDrops, sizeof(g_toolDrops));
                ImGui::InputTextWithHint("##ttrait", "trait: timid | mountable | spawns_at_night | attack_when_startled",
                    g_toolTrait, sizeof(g_toolTrait));
                ImGui::InputInt("min_health", &g_toolMinHealth);
                ImGui::InputInt("max_health", &g_toolMaxHealth);
                ImGui::SliderInt("limit", &g_toolLimit, 1, 20);
            }
            else if (g_toolKind == 3) { // search_items
                ImGui::InputTextWithHint("##tqual", "quality (Common, Rare…)", g_toolQuality, sizeof(g_toolQuality));
                ImGui::InputTextWithHint("##tcat", "category (Weapons, Food…)", g_toolCategory, sizeof(g_toolCategory));
                ImGui::SliderInt("limit", &g_toolLimit, 1, 20);
            }
            else if (g_toolKind == 4) { // get_time
                ImGui::TextDisabled("Empty = local. Examples: Germany, UTC, CST, Tokyo, London");
            }
            else if (g_toolKind == 5) { // set_reminder
                ImGui::InputTextWithHint("##tremuser", "player name…", g_toolRemUser, sizeof(g_toolRemUser));
                ImGui::InputInt("in_seconds", &g_toolInSeconds);
                if (g_toolInSeconds < 5) g_toolInSeconds = 5;
                ImGui::InputTextWithHint("##tremmsg", "reminder message…", g_toolRemMsg, sizeof(g_toolRemMsg));
            }
            else if (g_toolKind == 6) { // list_reminders
                ImGui::InputTextWithHint("##tremuser2", "optional player filter…", g_toolRemUser, sizeof(g_toolRemUser));
            }
            else if (g_toolKind == 7) { // cancel_reminder
                ImGui::InputInt("reminder id", &g_toolRemId);
                if (g_toolRemId < 1) g_toolRemId = 1;
            }

            ImGui::Spacing();
            if (ImGui::Button("Run Tool", ImVec2(120, 0))) {
                RunToolTest();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload KB", ImVec2(120, 0))) {
                Knowledge::Load(true);
                g_toolResult = "KB reloaded.\n" + Knowledge::Status();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear Result", ImVec2(120, 0))) {
                g_toolResult.clear();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Examples:");
            if (ImGui::SmallButton("Wild Pig")) {
                g_toolKind = 0;
                strncpy_s(g_toolQuery, "Wild Pig", _TRUNCATE);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("drops Void Essence")) {
                g_toolKind = 1;
                g_toolQuery[0] = 0;
                strncpy_s(g_toolDrops, "Void Essence", _TRUNCATE);
                g_toolTrait[0] = 0;
                g_toolMinHealth = 0;
                g_toolMaxHealth = 0;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("timid mobs")) {
                g_toolKind = 1;
                g_toolQuery[0] = 0;
                g_toolDrops[0] = 0;
                strncpy_s(g_toolTrait, "timid", _TRUNCATE);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Raw Pork")) {
                g_toolKind = 2;
                strncpy_s(g_toolQuery, "Raw Pork", _TRUNCATE);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Rare Weapons")) {
                g_toolKind = 3;
                g_toolQuery[0] = 0;
                strncpy_s(g_toolQuality, "Rare", _TRUNCATE);
                strncpy_s(g_toolCategory, "Weapons", _TRUNCATE);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("time Germany")) {
                g_toolKind = 4;
                strncpy_s(g_toolQuery, "Germany", _TRUNCATE);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("remind 60s")) {
                g_toolKind = 5;
                strncpy_s(g_toolRemUser, "Player", _TRUNCATE);
                g_toolInSeconds = 60;
                strncpy_s(g_toolRemMsg, "kraken starting soon", _TRUNCATE);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("list rems")) {
                g_toolKind = 6;
                g_toolRemUser[0] = 0;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("web Hytale")) {
                g_toolKind = 8;
                strncpy_s(g_toolQuery, "Hytale release date", _TRUNCATE);
                g_toolFresh[0] = 0;
            }

            ImGui::Separator();
            ImGui::BeginChild("##toolresult", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            if (g_toolResult.empty())
                ImGui::TextDisabled("Result appears here after you run a tool.");
            else
            {
                // Read-only text box: selectable, and Ctrl+C works.
                ImGui::InputTextMultiline("##toolresulttext", g_toolResult.data(), g_toolResult.size() + 1,
                    ImVec2(-FLT_MIN, (std::max)(ImGui::GetContentRegionAvail().y, 120.0f)), ImGuiInputTextFlags_ReadOnly);
            }
            if (g_toolScroll) {
                ImGui::SetScrollHereY(0.0f);
                g_toolScroll = false;
            }
            ImGui::EndChild();

            ImGui::EndTabItem();
        }

        // ================================================================
        // TAB: Position  (heuristic in-memory player-position finder)
        // ================================================================
        if (g_prefs.tabPosition && ImGui::BeginTabItem("Position")) {
            ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "Live coordinates");
            ImGui::TextDisabled(
                "There's no network packet hook available, so this reads your position\n"
                "out of memory instead. The object-graph method below is the permanent\n"
                "one - it needs no scanning and resumes by itself on every launch.\n"
                "Everything under it is kept as a fallback.");
            ImGui::Spacing();

            DrawObjectGraphSection();

            ImGui::Separator();
            ImGui::Spacing();

            // ================================================================
            // Recommended: HUD-text tracking
            // ================================================================
            bool hudEnabled = g_posHudTextEnabled.load();
            ImGui::TextColored(ImVec4(0.60f, 0.90f, 0.60f, 1.0f), "Recommended: automatic HUD-text tracking");
            ImGui::TextDisabled(
                "Hytale's own F7 debug overlay writes a real line of text with your exact\n"
                "position every frame (\"Feet (x, y, z)\"). Finding that specific text is\n"
                "almost instant and has no false positives, unlike scanning for numbers\n"
                "that merely look like coordinates - so this needs no scanning workflow,\n"
                "no float/double guessing, and resumes automatically on every launch.\n"
                "Requirements: press F7 once in-game to turn the debug overlay on, and\n"
                "type at least one message in chat once per session (so the DLL knows\n"
                "what a game text object looks like in memory).");
            if (ImGui::Checkbox("Enable automatic HUD-text tracking", &hudEnabled)) {
                PosHudTextSetEnabled(hudEnabled);
            }

            if (hudEnabled) {
                const long long now = NowSteadyMs();
                const long long foundAgoMs = now - g_posHudTextLastFoundMs.load();
                if (g_posHudTextLastFoundMs.load() != 0 && foundAgoMs < 3000) {
                    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "TRACKING");
                    ImGui::Text("X: %.2f   Y: %.2f   Z: %.2f", g_posX.load(), g_posY.load(), g_posZ.load());
                    std::string lastLine;
                    { std::lock_guard lock(g_posHudTextMtx); lastLine = g_posHudTextLast; }
                    ImGui::TextDisabled("last line: %s", lastLine.c_str());
                    ImGui::TextDisabled("%s scan, %lld ms",
                        g_posHudTextUsedFastPath.load() ? "fast (cached region)" : "full (slow)",
                        g_posHudTextLastScanDurMs.load());
                }
                else if (g_stringVT.load() == 0) {
                    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f),
                        "Waiting for the chat text-object vtable - type one message in chat first.");
                }
                else {
                    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f),
                        "No match yet - make sure F7's debug overlay is on-screen (last scan took %lld ms).",
                        g_posHudTextLastScanDurMs.load());
                }
            }

            ImGui::Separator();
            ImGui::Spacing();

            // ================================================================
            // Advanced / fallback: manual memory scan
            // ================================================================
            if (ImGui::CollapsingHeader("Advanced: manual memory scan (fallback)")) {
            ImGui::TextDisabled(
                "Use this only if HUD-text tracking above doesn't work for you:\n"
                "1) Press F7 in-game, read the \"Feet (x, y, z)\" line.\n"
                "2) Type those numbers into \"Known coordinates\" below with a small\n"
                "   tolerance (a few units), then First Scan.\n"
                "3) Move, read F7 again, update the numbers, click \"Filter (matches\n"
                "   known coords)\". Repeat until one candidate is left, then Lock it.\n"
                "If nothing survives, try the other value type (float vs double) -\n"
                "we don't know which one the client uses internally.");
            ImGui::Spacing();

            const bool locked = g_posLocked.load();
            const bool scanning = g_posScanning.load();

            if (locked) {
                ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
                    "LOCKED  @ 0x%llX", (unsigned long long)g_posLockedAddr.load());
                ImGui::Text("X: %.2f   Y: %.2f   Z: %.2f",
                    g_posX.load(), g_posY.load(), g_posZ.load());
                long long ageMs = NowSteadyMs() - g_posLastUpdateMs.load();
                ImGui::TextDisabled("updated %.1fs ago", ageMs / 1000.0);
                if (ImGui::Button("Unlock")) {
                    PosUnlock();
                }
            }
            else {
                ImGui::TextDisabled(scanning ? "Scanning..." : "Not locked.");
            }

            ImGui::Separator();

            if (ImGui::CollapsingHeader("Scan settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                int valType = g_posValueType.load();
                ImGui::TextUnformatted("Value type:");
                ImGui::SameLine();
                if (ImGui::RadioButton("double (8B/axis)", valType == (int)PosValueType::Double)) {
                    g_posValueType.store((int)PosValueType::Double);
                    PosClearCandidates(); // old candidates were read at the wrong stride
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("float (4B/axis)", valType == (int)PosValueType::Float)) {
                    g_posValueType.store((int)PosValueType::Float);
                    PosClearCandidates();
                }

                float boundXZ = g_posBoundXZ.load();
                float yMin = g_posBoundYMin.load();
                float yMax = g_posBoundYMax.load();
                ImGui::SetNextItemWidth(160);
                if (ImGui::InputFloat("world +/- X,Z bound", &boundXZ, 100.0f, 1000.0f, "%.0f"))
                    g_posBoundXZ.store((std::max)(1.0f, boundXZ));
                ImGui::SetNextItemWidth(160);
                if (ImGui::InputFloat("min Y", &yMin, 10.0f, 100.0f, "%.0f"))
                    g_posBoundYMin.store(yMin);
                ImGui::SetNextItemWidth(160);
                if (ImGui::InputFloat("max Y", &yMax, 10.0f, 100.0f, "%.0f"))
                    g_posBoundYMax.store(yMax);

                bool useGuess = g_posUseGuess.load();
                if (ImGui::Checkbox("Known coordinates (from F7's \"Feet (...)\" line)", &useGuess))
                    g_posUseGuess.store(useGuess);
                if (useGuess) {
                    static float gx = 0, gy = 65, gz = 0, gtol = 3;
                    ImGui::SetNextItemWidth(100); ImGui::InputFloat("X##guess", &gx, 0, 0, "%.2f"); ImGui::SameLine();
                    ImGui::SetNextItemWidth(100); ImGui::InputFloat("Y##guess", &gy, 0, 0, "%.2f"); ImGui::SameLine();
                    ImGui::SetNextItemWidth(100); ImGui::InputFloat("Z##guess", &gz, 0, 0, "%.2f");
                    ImGui::SetNextItemWidth(160); ImGui::SliderFloat("tolerance", &gtol, 0.5f, 50.0f, "%.1f");
                    g_posGuessX.store((double)gx);
                    g_posGuessY.store((double)gy);
                    g_posGuessZ.store((double)gz);
                    g_posGuessTolerance.store(gtol);
                    ImGui::TextDisabled("Keep this small (a few units) - it's exact ground truth from F7,\nnot a rough guess. Re-type it after every move, before Filter.");
                }
            }

            ImGui::Spacing();
            ImGui::BeginDisabled(scanning);
            if (ImGui::Button("First Scan", ImVec2(140, 0))) {
                PosUnlock();
                StartWorker([]() { PosFirstScan(); });
            }
            ImGui::SameLine();
            bool haveCandidates;
            {
                std::lock_guard lock(g_posMtx);
                haveCandidates = !g_posCandidates.empty();
            }
            ImGui::BeginDisabled(!haveCandidates);
            if (ImGui::Button("Filter (I moved)", ImVec2(160, 0))) {
                StartWorker([]() { PosFilterScan(); });
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!g_posUseGuess.load());
            if (ImGui::Button("Filter (matches known coords)", ImVec2(220, 0))) {
                StartWorker([]() { PosFilterByGuess(); });
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(90, 0))) {
                PosClearCandidates();
            }
            ImGui::EndDisabled();

            {
                size_t n;
                {
                    std::lock_guard lock(g_posMtx);
                    n = g_posCandidates.size();
                }
                ImGui::TextDisabled("Last first scan: %zu region(s), %.1f MB%s  |  last op: %lld ms  ->  %zu candidate(s)",
                    g_posLastScanRegions.load(),
                    g_posLastScanBytes.load() / (1024.0 * 1024.0),
                    g_posLastScanTruncated.load() ? " [hit budget/cap - tighten bound or guess and rescan]" : "",
                    g_posLastScanMs.load(),
                    n);
            }

            ImGui::Separator();

            std::vector<PosCandidate> candCopy;
            {
                std::lock_guard lock(g_posMtx);
                candCopy = g_posCandidates;
            }

            if (candCopy.empty()) {
                ImGui::TextDisabled("No candidates yet - click First Scan.");
            }
            else if (candCopy.size() > 50) {
                ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f),
                    "%zu candidates - move around in-game and click \"Filter (I moved)\" to narrow this down.",
                    candCopy.size());
            }
            else {
                ImGui::BeginChild("##poscands", ImVec2(0, 220), true);
                for (const auto& c : candCopy) {
                    ImGui::PushID((void*)c.addr);
                    if (ImGui::SmallButton("Lock")) {
                        PosLock(c.addr);
                    }
                    ImGui::SameLine();
                    ImGui::Text("0x%llX   X %.2f   Y %.2f   Z %.2f",
                        (unsigned long long)c.addr, c.x, c.y, c.z);
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }

            ImGui::Separator();
            if (ImGui::CollapsingHeader("Make it automatic (pointer chain)")) {
                ImGui::TextDisabled(
                    "The locked address above only works for this game session - ASLR\n"
                    "moves everything on the next launch. This traces a path from\n"
                    "HytaleClient.exe's own (stable) module base, through a chain of\n"
                    "pointers, to reach the same live position - so it can resolve and\n"
                    "lock automatically on every future launch, no scanning required.\n"
                    "Start from a working Lock above, then repeatedly \"scan for what\n"
                    "points here\" and drill into a hit, until one lands inside the\n"
                    "game's own module (marked STATIC) - that's a stable root.");

                const bool chainDone = g_posChainDone.load();
                if (chainDone) {
                    uintptr_t off; size_t levels;
                    {
                        std::lock_guard lock(g_posChainMtx);
                        off = g_posChainStaticOffset;
                        levels = g_posChainOffsets.size();
                    }
                    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
                        "Chain saved: %zu level(s), HytaleClient.exe+0x%llX",
                        levels, (unsigned long long)off);
                    ImGui::TextDisabled("Saved to position_chain.txt - loaded automatically on the next DLL load.");
                    if (ImGui::Button("Test: resolve & lock now")) {
                        StartWorker([]() { PosChainTryAutoLock(); });
                    }
                }

                ImGui::Spacing();
                ImGui::BeginDisabled(!locked);
                if (ImGui::Button("Start chain from locked address")) {
                    PosChainBegin(g_posLockedAddr.load());
                    StartWorker([]() { PosFindPointersToCurrentTarget(); });
                }
                ImGui::EndDisabled();

                const uintptr_t curTarget = g_posPtrCurrentTarget.load();
                if (curTarget) {
                    size_t level;
                    { std::lock_guard lock(g_posChainMtx); level = g_posChainOffsets.size(); }
                    ImGui::Text("Level %zu - current target: 0x%llX", level, (unsigned long long)curTarget);

                    int window = g_posPtrWindow.load();
                    ImGui::SetNextItemWidth(140);
                    if (ImGui::InputInt("search window (bytes)", &window, 0x100, 0x400))
                        g_posPtrWindow.store((std::max)(8, window));

                    const bool ptrScanning = g_posPtrScanning.load();
                    ImGui::BeginDisabled(ptrScanning);
                    if (ImGui::Button("Scan for pointers to current target")) {
                        StartWorker([]() { PosFindPointersToCurrentTarget(); });
                    }
                    ImGui::EndDisabled();

                    std::vector<PosPtrHit> hitsCopy;
                    { std::lock_guard lock(g_posPtrMtx); hitsCopy = g_posPtrHits; }

                    if (hitsCopy.empty()) {
                        ImGui::TextDisabled(ptrScanning ? "Scanning..." : "No hits yet.");
                    }
                    else if (hitsCopy.size() > 100) {
                        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f),
                            "%zu hits - shrink the search window and rescan.", hitsCopy.size());
                    }
                    else {
                        ImGui::BeginChild("##ptrhits", ImVec2(0, 200), true);
                        for (const auto& h : hitsCopy) {
                            ImGui::PushID((void*)h.slotAddr);
                            if (h.inGameModule) {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.25f, 1.0f));
                                if (ImGui::SmallButton("Use as root (finish)")) {
                                    PosChainFinish(h);
                                }
                                ImGui::PopStyleColor();
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "STATIC");
                            }
                            else {
                                if (ImGui::SmallButton("Drill into")) {
                                    PosChainDrillInto(h);
                                    StartWorker([]() { PosFindPointersToCurrentTarget(); });
                                }
                            }
                            ImGui::SameLine();
                            ImGui::Text("0x%llX  +0x%llX",
                                (unsigned long long)h.slotAddr, (unsigned long long)h.offset);
                            ImGui::PopID();
                        }
                        ImGui::EndChild();
                    }
                }
            }
            } // Advanced: manual memory scan (fallback)

            ImGui::EndTabItem();
        }

        // ================================================================
        // TAB: Player  (everything readable off the local player entity)
        // ================================================================
        if (g_prefs.tabPlayer && ImGui::BeginTabItem("Player")) {
            DrawPlayerTab();
            ImGui::EndTabItem();
        }

        // ================================================================
        // TAB: Packets  (network traffic either side of the client)
        // ================================================================
        if (g_prefs.tabPackets && ImGui::BeginTabItem("Packets")) {
            DrawPacketsTab();
            ImGui::EndTabItem();
        }

        // ================================================================
        // TAB: Shapes  (draw the client's own debug shapes in the world)
        // ================================================================
        if (g_prefs.tabShapes && ImGui::BeginTabItem("Shapes")) {
            DrawShapesTab();
            ImGui::EndTabItem();
        }

        // ================================================================
        // TAB: Build  (a prefab, outlined in the world, block by block)
        // ================================================================
        if (g_prefs.tabBuild && ImGui::BeginTabItem("Build")) {
            DrawBuildTab();
            ImGui::EndTabItem();
        }

        // ================================================================
        // TAB: Copy  (mark out part of the world and make a prefab of it)
        // ================================================================
        if (g_prefs.tabCopy && ImGui::BeginTabItem("Copy")) {
            DrawCopyTab();
            ImGui::EndTabItem();
        }

        // ================================================================
        // TAB: Farm  (record a lap of a farm, then have it driven for you)
        // ================================================================
        if (g_prefs.tabFarm && ImGui::BeginTabItem("Farm")) {
            DrawFarmTab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    SaveSettingsThrottled();

    if (g_windowStyleDirty) {
        ApplyWindowStyle();
        g_windowStyleDirty = false;
    }

    // Apply ImGui color theme when preset changes (or on first frame)
    {
        int want = 0;
        { std::lock_guard lock(g_settingsMtx); want = g_themePreset; }
        if (g_themeDirty || g_appliedTheme != want)
            ApplyTheme(want);
    }

    ImGui::Unindent(14.0f);
    ImGui::End();
}

// =============================================================================
// External Standalone Window System
// =============================================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Handle sizing chrome BEFORE ImGui so it cannot swallow edge hits.
    // True borderless: entire window is client area (no native chrome / white strip).
    if (msg == WM_NCCALCSIZE) {
        if (wParam == TRUE)
            return 0; // client = full window rect, zero non-client
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    // Custom hit-test: edges resize (ImGui title bar still handles drag via InvisibleButton).
    if (msg == WM_NCHITTEST) {
        POINT pt{ (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hWnd, &pt);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        // DPI-aware grip so edges stay easy to grab on high-DPI monitors
        int e = 10;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
            auto pGetDpi = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
            if (pGetDpi) {
                UINT dpi = pGetDpi(hWnd);
                if (dpi > 0)
                    e = (int)((10 * dpi + 95) / 96);
            }
        }
        if (e < 8) e = 8;
        const bool left = pt.x < e;
        const bool right = pt.x >= rc.right - e;
        const bool top = pt.y < e;
        const bool bottom = pt.y >= rc.bottom - e;
        if (top && left)     return HTTOPLEFT;
        if (top && right)    return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left)            return HTLEFT;
        if (right)           return HTRIGHT;
        if (top)             return HTTOP;
        if (bottom)          return HTBOTTOM;
        return HTCLIENT;
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    // Prevent default erase (avoids white flash before first Present)
    if (msg == WM_ERASEBKGND)
        return 1;

    if (msg == WM_SIZE) {
        if (g_pd3dDeviceOverlay != nullptr && g_pSwapChainOverlay != nullptr && wParam != SIZE_MINIMIZED) {
            if (g_mainRenderTargetViewOverlay) {
                g_mainRenderTargetViewOverlay->Release();
                g_mainRenderTargetViewOverlay = nullptr;
            }
            HRESULT hr = g_pSwapChainOverlay->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            if (SUCCEEDED(hr)) {
                ID3D11Texture2D* pBackBuffer = nullptr;
                hr = g_pSwapChainOverlay->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
                if (SUCCEEDED(hr) && pBackBuffer) {
                    g_pd3dDeviceOverlay->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetViewOverlay);
                    pBackBuffer->Release();
                }
            }
        }
        return 0;
    }

    // Remember where the window was left, for the next launch.
    if (msg == WM_EXITSIZEMOVE) {
        SaveOverlayGeometry();
        return 0;
    }

    // Instead of destroying when the user clicks 'X', just hide it
    if (msg == WM_CLOSE) {
        g_show = false;
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }

    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChainOverlay, &g_pd3dDeviceOverlay, &featureLevel, &g_pd3dDeviceContextOverlay);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChainOverlay, &g_pd3dDeviceOverlay, &featureLevel, &g_pd3dDeviceContextOverlay);
    if (res != S_OK) return false;

    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hrBuf = g_pSwapChainOverlay->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (FAILED(hrBuf) || !pBackBuffer)
        return false;
    HRESULT hrRtv = g_pd3dDeviceOverlay->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetViewOverlay);
    pBackBuffer->Release();
    if (FAILED(hrRtv) || !g_mainRenderTargetViewOverlay)
        return false;
    return true;
}

static void CleanupDeviceD3D() {
    if (g_mainRenderTargetViewOverlay) { g_mainRenderTargetViewOverlay->Release(); g_mainRenderTargetViewOverlay = nullptr; }
    if (g_pSwapChainOverlay) { g_pSwapChainOverlay->Release(); g_pSwapChainOverlay = nullptr; }
    if (g_pd3dDeviceContextOverlay) { g_pd3dDeviceContextOverlay->Release(); g_pd3dDeviceContextOverlay = nullptr; }
    if (g_pd3dDeviceOverlay) { g_pd3dDeviceOverlay->Release(); g_pd3dDeviceOverlay = nullptr; }
}

// =============================================================================
// Fonts
// =============================================================================
// ImGui 1.92 bakes glyphs on demand at whatever size is asked for, so a size or
// scale change is just a style value now - no atlas rebuild, and text stays
// sharp at every size. The font files are only reloaded when the chosen font
// or the fallback set changes.
static std::string g_loadedFontKey;

static void ApplyFonts() {
    ImGuiIO& io = ImGui::GetIO();
    std::string path;
    bool extra = true;
    {
        std::lock_guard lock(g_settingsMtx);
        path = g_prefs.uiFontPath;
        extra = g_prefs.extraFonts;
    }
    auto exists = [](const std::string& p) {
        return !p.empty() && GetFileAttributesW(Utf8ToWide(p).c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    const std::string key = path + (extra ? "|extra" : "|plain");
    if (key != g_loadedFontKey) {
        g_loadedFontKey = key;
        io.Fonts->Clear();

        std::string main = exists(path) ? path : std::string();
        if (main.empty()) main = "C:\\Windows\\Fonts\\segoeui.ttf";
        if (!exists(main)) main = "C:\\Windows\\Fonts\\arial.ttf";
        ImFont* font = exists(main) ? io.Fonts->AddFontFromFileTTF(main.c_str(), g_fontSize) : nullptr;
        if (!font) font = io.Fonts->AddFontDefault();

        // Merged fallbacks: without them symbols, emoji-range glyphs and CJK /
        // Korean chat show as boxes. Glyphs are only rasterised when first seen.
        int merged = 0;
        if (extra && font) {
            static const char* kFallbacks[] = {
                "C:\\Windows\\Fonts\\seguisym.ttf",   // symbols, arrows, box drawing
                "C:\\Windows\\Fonts\\seguiemj.ttf",   // emoji (monochrome here)
                "C:\\Windows\\Fonts\\msyh.ttc",       // Chinese
                "C:\\Windows\\Fonts\\YuGothM.ttc",    // Japanese
                "C:\\Windows\\Fonts\\malgun.ttf",     // Korean
                "C:\\Windows\\Fonts\\Nirmala.ttf",    // Indic scripts
            };
            for (const char* f : kFallbacks) {
                if (!exists(f)) continue;
                ImFontConfig cfg;
                cfg.MergeMode = true;
                if (io.Fonts->AddFontFromFileTTF(f, 0.0f, &cfg)) ++merged;
            }
        }
        ImGui_ImplDX11_InvalidateDeviceObjects(); // recreated on the next NewFrame
        LogToFile("Overlay fonts loaded: " + main + " + " + std::to_string(merged) + " fallback(s)");
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = g_fontSize;
    style._NextFrameFontSizeBase = g_fontSize;
    style.FontScaleMain = g_uiScale;
}

// =============================================================================
// Hotkeys
// =============================================================================
// Only while a window of this process (Hytale, the overlay, the compact chat)
// is in front, unless turned off - F8 used to fire while typing in a browser.
static bool HotkeysAllowed() {
    if (UiIsCapturingHotkey()) return false;
    if (!g_prefs.hotkeysNeedFocus) return true;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static bool KeyEdge(int vk, bool& was) {
    if (vk <= 0 || vk > 255) { was = false; return false; }
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fired = now && !was;
    was = now;
    return fired;
}

static void ShowFullOverlay() {
    g_show = true;
    ShowWindow(g_hwndOverlay, SW_SHOW);
    SetForegroundWindow(g_hwndOverlay);
}

static void PollOverlayHotkeys() {
    static bool wasOverlay = false, wasCompact = false, wasChat = false;
    const bool allowed = HotkeysAllowed();
    const bool overlayHit = KeyEdge(g_prefs.keyToggleOverlay, wasOverlay) && allowed;
    const bool compactHit = KeyEdge(g_prefs.keyToggleCompact, wasCompact) && allowed;
    const bool chatHit = KeyEdge(g_prefs.keyFocusChat, wasChat) && allowed;

    if (compactHit)
        ToggleCompactChatMode();

    if (overlayHit) {
        g_show = !g_show;
        if (g_compactUi.load()) {
            // In compact mode the key hides and shows the compact window. It
            // used to bring up the (stale) full window on top of it instead.
            if (g_hwndCompact) ShowWindow(g_hwndCompact, g_show ? SW_SHOWNOACTIVATE : SW_HIDE);
            if (g_show) PaintCompactWindow(true);
        }
        else if (g_show) ShowFullOverlay();
        else ShowWindow(g_hwndOverlay, SW_HIDE);
    }

    if (chatHit) {
        if (g_compactUi.load()) ToggleCompactChatMode();
        ShowFullOverlay();
        g_selectChatTab = true;
        ChatUi::FocusInput();
    }

    // AI review keys. They only do anything while something is waiting; with nothing to
    // approve they are just keys, so binding one to a key the game uses costs nothing.
    static bool wasApprove = false, wasDiscard = false, wasRetry = false;
    const bool apprHit = KeyEdge(g_prefs.keyAiApprove, wasApprove) && allowed;
    const bool discHit = KeyEdge(g_prefs.keyAiDiscard, wasDiscard) && allowed;
    const bool retryHit = KeyEdge(g_prefs.keyAiRetry, wasRetry) && allowed;
    if (apprHit) AiFlow::ApproveOldest();
    if (discHit) AiFlow::DiscardOldest();
    if (retryHit) AiFlow::RetryNewest();
    AiFlow::Tick();

    // The farm keys, plus the settings the farm worker learned while driving.
    FarmUiTick(allowed);
}

void OverlayThread() {
    // Settings first: the window opens where it was last left.
    LoadSettings();
    LoadIgnored();
    LoadWhitelist();
    ReloadSystemPrompt(true);
    AutoReply::Load();
    // Its worker reads the position and drives on its own thread, so it runs
    // whether the overlay is showing or not.
    FarmInit();

    int winX, winY, winW, winH;
    bool startHidden = false;
    {
        std::lock_guard lock(g_settingsMtx);
        winX = g_prefs.fullX; winY = g_prefs.fullY; winW = g_prefs.fullW; winH = g_prefs.fullH;
        startHidden = g_prefs.startHidden;
    }
    if (!RectOnScreen(winX, winY, winW, winH)) { winX = 100; winY = 100; winW = 920; winH = 720; }

    // Normal fully-interactive Windows Class
    // Dark brush so any unpainted strip matches the theme (never system white).
    static HBRUSH s_darkBrush = CreateSolidBrush(RGB(18, 18, 26)); // ~0.07,0.07,0.10
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = s_darkBrush;
    wc.lpszClassName = L"HyAIOverlayClass";
    RegisterClassExW(&wc);

    // Borderless look (NCCALCSIZE clears chrome) but keep THICKFRAME so Windows
    // treats the HWND as resizable; actual grips come from WM_NCHITTEST.
    g_hwndOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_APPWINDOW, wc.lpszClassName, L"HyAI",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
        winX, winY, winW, winH,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!g_hwndOverlay) {
        LogToFile("Overlay: CreateWindowExW failed");
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return;
    }

    // Win10/11: kill residual DWM border / caption tint if the API exists
    {
        HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
        if (hDwm) {
            typedef HRESULT(WINAPI* FnSet)(HWND, DWORD, LPCVOID, DWORD);
            auto pSet = (FnSet)GetProcAddress(hDwm, "DwmSetWindowAttribute");
            if (pSet) {
                // DWMWA_BORDER_COLOR = 34, DWMWA_CAPTION_COLOR = 35, DWMWA_USE_IMMERSIVE_DARK_MODE = 20
                const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
                const DWORD DWMWA_BORDER_COLOR = 34;
                const DWORD DWMWA_CAPTION_COLOR = 35;
                BOOL dark = TRUE;
                pSet(g_hwndOverlay, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
                COLORREF border = RGB(18, 18, 26);
                pSet(g_hwndOverlay, DWMWA_BORDER_COLOR, &border, sizeof(border));
                pSet(g_hwndOverlay, DWMWA_CAPTION_COLOR, &border, sizeof(border));
            }
            FreeLibrary(hDwm);
        }
    }

    if (!CreateDeviceD3D(g_hwndOverlay)) {
        LogToFile("Overlay: CreateDeviceD3D failed");
        DestroyCompactWindow();
        CleanupDeviceD3D();
        DestroyWindow(g_hwndOverlay);
        g_hwndOverlay = nullptr;
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return;
    }

    if (startHidden) g_show = false;
    else {
        ShowWindow(g_hwndOverlay, SW_SHOW);
        UpdateWindow(g_hwndOverlay);
    }

    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(g_hwndOverlay);
    ImGui_ImplDX11_Init(g_pd3dDeviceOverlay, g_pd3dDeviceContextOverlay);

    {
        std::lock_guard lock(g_settingsMtx);
        strncpy_s(g_localNameBuf, g_localName.c_str(), 63);
        strncpy_s(g_aiPrefixBuf, g_aiPrefix.c_str(), 95);
        strncpy_s(g_aiColorCodeBuf, g_aiColorCode.c_str(), 15);
        strncpy_s(g_nicknameOnBuf, g_nicknameOn.c_str(), 95);
        strncpy_s(g_nicknameOffBuf, g_nicknameOff.c_str(), 63);
        strncpy_s(g_wakeWordBuf, g_wakeWord.c_str(), 63);
        strncpy_s(g_wakeWord2Buf, g_wakeWord2.c_str(), 63);
        strncpy_s(g_botNameBuf, g_botName.c_str(), 63);
        strncpy_s(g_ollamaModelBuf, g_ollamaModel.c_str(), 95);
        strncpy_s(g_ollamaHostBuf, g_ollamaHost.c_str(), 63);
        strncpy_s(g_thinkLevelBuf, g_thinkLevel.c_str(), 15);
        strncpy_s(g_failMessageBuf, g_failMessage.c_str(), 255);
        strncpy_s(g_openaiBaseBuf, g_openaiBaseUrl.c_str(), 255);
        strncpy_s(g_openaiKeyBuf, g_openaiApiKey.c_str(), 255);
        strncpy_s(g_openaiModelBuf, g_openaiModel.c_str(), 95);
        strncpy_s(g_webSearchKeyBuf, g_webSearchApiKey.c_str(), 255);
        strncpy_s(g_webSearchFreshBuf, g_webSearchFreshness.c_str(), 31);
    }

    // Apply saved theme after settings load
    {
        int preset = 0;
        { std::lock_guard lock(g_settingsMtx); preset = g_themePreset; }
        ApplyTheme(preset);
    }

    // Fonts BEFORE the render loop (never rebuilt mid-frame).
    ApplyFonts();
    g_fontDirty = false;

    g_startTime = std::chrono::steady_clock::now();
    ApplyWindowStyle();

    {
        bool wantCompact = false;
        { std::lock_guard lock(g_settingsMtx); wantCompact = g_compactChatMode; }
        g_compactUi.store(wantCompact);
        if (wantCompact) {
            EnterCompactChatMode();
            if (startHidden) {
                g_show = false;
                if (g_hwndCompact) ShowWindow(g_hwndCompact, SW_HIDE);
            }
        }
    }
    LogToFile("External standalone ImGui window started.");

    while (!g_shuttingDown.load()) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                g_shuttingDown = true;
        }
        if (g_shuttingDown.load()) break;

        // The build guide's own keys. Polled here because this loop runs
        // whether or not the overlay has focus, which is the whole point: they
        // have to work while you are in the game placing blocks.
        BuildPollHotkeys((void*)g_hwndOverlay);
        // Autosave, reopening last launch's build, and arming follow-along.
        // Here rather than below the visibility check, because the overlay is
        // hidden while you play - which is exactly when a build changes.
        SessionTick();

        // Chat work that must not wait for the overlay to be visible: mention
        // alerts and the saved chat log, auto replies, settings saves.
        ChatUi::Tick();
        AutoReply::Tick();
        SaveSettingsThrottled();

        PollOverlayHotkeys();

        // Save GPU and CPU if window is hidden
        if (!g_show) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }


        // The permanent path: resolve root -> player -> position and publish
        // into g_posX/Y/Z. Costs one short pointer walk per frame.
        GsdkFrameTick();
        // Self-throttled: only does anything while no working path exists.
        GsdkAutoSetupTick();
        // Keep the locked position (if any) fresh — cheap, one SafeRead when locked.
        // Skipped while the object-graph path is publishing, so the two readers
        // never disagree about what is at g_posLockedAddr.
        if (!GsdkOwnsPosition())
            PosFrameTick();
        // Recommended path: keeps re-finding Hytale's own F7 debug line. Self-throttled.
        PosHudTextTick();

        // Once: load the saved HUD-text preference, and try resolving a saved
        // pointer chain (see the Position tab) - either makes tracking resume
        // automatically on this launch with no clicks.
        {
            static bool triedAutoStart = false;
            if (!triedAutoStart) {
                triedAutoStart = true;
                PosHudTextLoadFromDisk();
                StartWorker([]() { PosChainTryAutoLock(); });
                // Saved object-graph path first; if there is none, see whether
                // the published offsets happen to fit this build. Both are
                // instant, so there is nothing to wait for.
                GsdkPathLoadFromDisk();
                DbgLoadFromDisk();
                DbgLoadSeed();
                BuildLoadMaterialSlots();
                BuildLoadBlockIds();
                PlaceLoadFromDisk();

                // Install the outbound hook up front. It is what lets the guide
                // notice you placing blocks, and making that a button meant the
                // follow-along feature looked absent until you went looking for
                // it. Nothing is sent - it only reads what the client was
                // already sending.
                InstallOutboundPacketHook();

                StartWorker([]() { DbgResolveRuntime(); });
            }

            // Until a path is known, retry the published offsets every few
            // seconds - the root only becomes live once you are in a world.
            {
                static long long lastKnownTry = 0;
                static long long lastResolvedMs = 0;
                bool havePath = false;
                { std::lock_guard lock(g_gsdkPathMtx); havePath = g_gsdkPath.found; }
                const long long nowMs = NowSteadyMs();

                // A path that resolves keeps its own clock running.
                if (GsdkGetPlayerInfo().valid) lastResolvedMs = nowMs;
                else if (!lastResolvedMs) lastResolvedMs = nowMs;

                // Retry when there is no path AT ALL, and also when the path we
                // have has stopped resolving while the game root is live.
                //
                // The old condition was "no path" alone, which meant a known
                // but DEAD path blocked the fallback forever - exactly the
                // state the auto-setup can leave behind if it learns a chain
                // that only resolves while the F7 overlay is on screen. Sitting
                // on a broken path is worse than having none, because having
                // none is recoverable.
                const bool rootLive = GsdkCurrentRoot() != 0;
                const bool stale = havePath && rootLive &&
                                   nowMs - lastResolvedMs > 5000;

                if ((!havePath || stale) && nowMs - lastKnownTry > 3000) {
                    lastKnownTry = nowMs;
                    // Only adopts when the surrounding struct corroborates, so
                    // this cannot replace a good path with a guess.
                    GsdkTryKnownOffsets();
                }
            }
        }

        // Font or size change: applied BEFORE NewFrame (never mid-frame).
        if (g_fontDirty) {
            ApplyFonts();
            g_fontDirty = false;
        }

        const bool compactNow = g_compactUi.load();

        if (g_windowStyleDirty) {
            ApplyWindowStyle();
            g_windowStyleDirty = false;
        }
        if (compactNow) {
            // Compact-only HWND path: no ImGui, no DXGI on this surface.
            // Pump messages for the compact window (drag / Full button).
            PaintCompactWindow();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // ---- Full UI on the DXGI overlay HWND ----
        if (!g_mainRenderTargetViewOverlay) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderOverlay();

        ImGui::EndFrame();

        float clearCol[4] = { 0.05f, 0.06f, 0.09f, 1.0f };
        g_pd3dDeviceContextOverlay->OMSetRenderTargets(1, &g_mainRenderTargetViewOverlay, nullptr);
        g_pd3dDeviceContextOverlay->ClearRenderTargetView(g_mainRenderTargetViewOverlay, clearCol);
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChainOverlay->Present(1, 0);


    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyCompactWindow();
    CleanupDeviceD3D();
    DestroyWindow(g_hwndOverlay);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
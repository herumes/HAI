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


// Overlay D3D / window state
HWND g_hwndOverlay = nullptr;
ID3D11Device* g_pd3dDeviceOverlay = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContextOverlay = nullptr;
IDXGISwapChain* g_pSwapChainOverlay = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetViewOverlay = nullptr;

// Full-mode window geometry (restored when leaving compact chat mode)
static bool g_haveFullGeom = false;
static int  g_fullX = 100, g_fullY = 100, g_fullW = 920, g_fullH = 720;
static bool g_compactGeomApplied = false;
static std::atomic<bool> g_compactUi{ false }; // sole authority for compact RENDER + present

void ToggleCompactChatMode(); // defined below

// =============================================================================
// Compact chat — SEPARATE HWND that never has a DXGI swap chain
//
// Full UI keeps g_hwndOverlay + D3D. Compact is pure GDI + UpdateLayeredWindow.
// Features: scroll + auto-stick to latest, hover-only Full/scrollbar, resizable.
// =============================================================================

static HWND     g_hwndCompact = nullptr;
static HDC      g_cScreenDc = nullptr;
static HDC      g_cMemDc = nullptr;
static HBITMAP  g_cDib = nullptr;
static void* g_cBits = nullptr;
static int      g_cDibW = 0, g_cDibH = 0;

// Scroll: 0 = stuck to bottom (latest). Positive = pixels scrolled up from bottom.
static int      g_cScrollFromBottom = 0;
static bool     g_cStickBottom = true;
static size_t   g_cLastLogCount = 0;
static bool     g_cHovered = false;
static bool     g_cScrollbarDrag = false;
static int      g_cScrollbarDragOff = 0;

// Content metrics from last paint (for hit-testing / wheel)
static int      g_cContentH = 0;
static int      g_cViewH = 0;
static int      g_cViewW = 0;

static void ReleaseCompactSurface() {
    if (g_cDib) { DeleteObject(g_cDib); g_cDib = nullptr; }
    if (g_cMemDc) { DeleteDC(g_cMemDc); g_cMemDc = nullptr; }
    if (g_cScreenDc) { ReleaseDC(nullptr, g_cScreenDc); g_cScreenDc = nullptr; }
    g_cBits = nullptr;
    g_cDibW = g_cDibH = 0;
}

static bool EnsureCompactSurface(int w, int h) {
    if (w < 1 || h < 1) return false;
    if (g_cDib && g_cDibW == w && g_cDibH == h && g_cBits)
        return true;
    ReleaseCompactSurface();

    g_cScreenDc = GetDC(nullptr);
    if (!g_cScreenDc) return false;
    g_cMemDc = CreateCompatibleDC(g_cScreenDc);
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
    SelectObject(g_cMemDc, g_cDib);
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
static void PaintCompactWindow();

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
        PaintCompactWindow();
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
                PaintCompactWindow();
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
            PaintCompactWindow();
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
    case WM_SIZE:
    {
        RECT rc{};
        GetClientRect(hWnd, &rc);
        int ww = rc.right - rc.left;
        int hh = rc.bottom - rc.top;
        if (ww >= 8 && hh >= 8) {
            int d = 28;
            if (d > ww) d = ww;
            if (d > hh) d = hh;
            HRGN rgn = CreateRoundRectRgn(0, 0, ww + 1, hh + 1, d, d);
            if (rgn) SetWindowRgn(hWnd, rgn, TRUE);
        }
        int maxScroll = (std::max)(0, g_cContentH - g_cViewH);
        if (g_cStickBottom) g_cScrollFromBottom = 0;
        else if (g_cScrollFromBottom > maxScroll) g_cScrollFromBottom = maxScroll;
        PaintCompactWindow();
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
        wc.lpszClassName = L"JosebotCompactOnlyClass";
        RegisterClassExW(&wc);
        s_reg = true;
    }

    // THICKFRAME enables system resize; we still map edges in WM_NCHITTEST
    g_hwndCompact = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_APPWINDOW,
        L"JosebotCompactOnlyClass",
        L"Josebot Compact",
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

static void PaintCompactWindow() {
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
    bool showTs = true;
    float lineGap = 1.0f;
    bool topMost = false;
    float fontSize = 17.0f;
    int maxLines = 80;
    float padXf = 10.0f;
    {
        std::lock_guard lock(g_settingsMtx);
        panelOp = g_compactOpacity;
        showTs = g_showTimestamps;
        lineGap = g_chatLineGap;
        topMost = g_alwaysOnTop;
        fontSize = g_compactFontSize;
        maxLines = g_compactMaxLines;
        padXf = g_compactPadX;
    }
    if (panelOp < 0.10f) panelOp = 0.10f;
    if (panelOp > 1.0f) panelOp = 1.0f;
    if (fontSize < 10.0f) fontSize = 10.0f;
    if (fontSize > 28.0f) fontSize = 28.0f;
    if (maxLines < 20) maxLines = 20;
    if (maxLines > 300) maxLines = 300;
    if (padXf < 4.0f) padXf = 4.0f;
    if (padXf > 32.0f) padXf = 32.0f;

    const BYTE panelA = (BYTE)(panelOp * 255.0f + 0.5f);
    const BYTE pR = 14, pG = 16, pB = 24;
    const BYTE pr = (BYTE)((pR * panelA + 127) / 255);
    const BYTE pg = (BYTE)((pG * panelA + 127) / 255);
    const BYTE pb = (BYTE)((pB * panelA + 127) / 255);

    // Hover state (Full button + scrollbar only when hovered)
    g_cHovered = CompactIsHovered();

    // Snapshot log + auto-stick when new lines arrive
    std::vector<ChatLine> logCopy;
    {
        std::lock_guard lock(g_mtx);
        logCopy = g_log;
    }
    if (logCopy.size() != g_cLastLogCount) {
        if (g_cStickBottom)
            g_cScrollFromBottom = 0;
        g_cLastLogCount = logCopy.size();
    }

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
    HFONT font = CreateFontW(
        -fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);

    // Top chrome stays fixed (matches CompactWndProc kTopBar hit-testing).
    // Text metrics scale with the compact font size setting.
    const int topBar = 22;
    const int padX = (int)(padXf + 0.5f);
    const int lineH = fontPx + 2;
    const int gap = (int)lineGap;
    const int sbW = 6;
    const int textRight = g_cHovered ? (w - sbW - 8) : (w - padX);

    size_t startLine = 0;
    if ((int)logCopy.size() > maxLines)
        startLine = logCopy.size() - (size_t)maxLines;

    // --- Dynamic Layout Engine (Handles Emoji Replacement, Line Wrapping + Mid-Word Breaks) ---
    struct VisualSegment {
        std::wstring text;
        COLORREF color;
        int x;
        int yOffset;
    };
    struct VisualLineLayout {
        int height;
        std::vector<VisualSegment> segments;
    };

    std::vector<VisualLineLayout> layouts;
    layouts.reserve(logCopy.size() - startLine);

    int contentH = 0;

    for (size_t li = startLine; li < logCopy.size(); ++li) {
        const ChatLine& line = logCopy[li];
        VisualLineLayout layout;
        int currentX = padX;
        int currentYOffset = 0;

        auto addText = [&](const std::string& utf8_in, COLORREF color) {
            std::string utf8 = utf8_in;

            // 1. Universal replacements (can be found anywhere, like :heart:)
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
            for (const auto& sub : universal) {
                std::string key = sub.key;
                std::string val = sub.val;
                size_t p = 0;
                while ((p = utf8.find(key, p)) != std::string::npos) {
                    utf8.replace(p, key.length(), val);
                    p += val.length();
                }
            }

            if (utf8.empty()) return;
            size_t pos = 0;
            while (pos < utf8.length()) {
                size_t nextSpace = utf8.find(' ', pos);
                size_t end = (nextSpace != std::string::npos) ? nextSpace + 1 : utf8.length();
                std::string word = utf8.substr(pos, end - pos);

                // 2. Word-boundary exact replacements (prevents mutating http:// links)
                bool hasSpace = (!word.empty() && word.back() == ' ');
                if (hasSpace) word.pop_back();

                if (word == "<3") word = "❤️";
                else if (word == ":)") word = "🙂";
                else if (word == ":(") word = "🙁";
                else if (word == ":D") word = "😃";
                else if (word == ":P" || word == ":p") word = "😛";
                else if (word == ";)") word = "😉";
                else if (word == ">:(") word = "😡";
                else if (word == ":O" || word == ":o") word = "😮";

                if (hasSpace) word += " ";

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
                        else {
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
                }
                pos = end;
            }
            };

        if (showTs && !line.timeStr.empty()) {
            addText(line.timeStr, RGB(150, 155, 170));
            currentX += 6; // Add small spacing after timestamp
        }

        if (line.outgoing) {
            addText("> " + line.plain, RGB(140, 230, 255));
        }
        else if (!line.segs.empty()) {
            for (const auto& seg : line.segs) {
                if (seg.text.empty()) continue;
                int rr = (int)(seg.color.x * 255.f + 0.5f);
                int gg = (int)(seg.color.y * 255.f + 0.5f);
                int bb = (int)(seg.color.z * 255.f + 0.5f);
                rr = (std::max)(0, (std::min)(255, rr + 16));
                gg = (std::max)(0, (std::min)(255, gg + 16));
                bb = (std::max)(0, (std::min)(255, bb + 16));
                addText(seg.text, RGB(rr, gg, bb));
            }
        }
        else {
            addText(line.plain, RGB(235, 235, 240));
        }

        layout.height = currentYOffset + lineH;
        contentH += layout.height + gap;
        layouts.push_back(layout);
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

    const int contentTop = topBar - maxScroll + g_cScrollFromBottom;

    // --- Execute layout drawing ---
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
                    SetTextColor(dc, seg.color);
                    TextOutW(dc, seg.x, drawY, seg.text.c_str(), (int)seg.text.length());
                }
            }
        }
        y += layout.height + gap;
    }

    // Hover-only: Full button
    if (g_cHovered) {
        SetTextColor(dc, RGB(210, 220, 240));
        RECT fullRc{ w - 52, 3, w - 8, 20 };
        DrawTextW(dc, L"Full", 4, &fullRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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
    if (font) DeleteObject(font);

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

static void ApplyWindowShape(HWND hwnd, int radiusPx) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (radiusPx <= 0) {
        SetWindowRgn(hwnd, nullptr, TRUE);
        return;
    }
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 8 || h < 8) return;
    int d = radiusPx * 2;
    if (d > w) d = w;
    if (d > h) d = h;
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, d, d);
    if (rgn)
        SetWindowRgn(hwnd, rgn, TRUE);
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

static void ApplyWindowStyle() {
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
    }
}


static void EnterCompactChatMode() {
    if (!EnsureCompactWindow()) {
        LogToFile("EnterCompactChatMode: no compact HWND");
        return;
    }

    // Remember full-UI geometry from the main overlay
    if (g_hwndOverlay && !g_compactGeomApplied) {
        RECT rc{};
        if (GetWindowRect(g_hwndOverlay, &rc)) {
            const int ww = rc.right - rc.left;
            const int hh = rc.bottom - rc.top;
            if (ww >= 600 && hh >= 400) {
                g_fullX = rc.left;
                g_fullY = rc.top;
                g_fullW = ww;
                g_fullH = hh;
                g_haveFullGeom = true;
            }
        }
    }

    const int w = 440, h = 210;
    const int margin = 16;
    int x = margin;
    int y = GetSystemMetrics(SM_CYSCREEN) - h - 120;
    if (y < margin) y = margin;

    g_compactUi.store(true);
    g_compactGeomApplied = true;

    // Hide the DXGI overlay entirely — it must not paint while compact is up
    if (g_hwndOverlay)
        ShowWindow(g_hwndOverlay, SW_HIDE);

    bool top = false;
    { std::lock_guard lock(g_settingsMtx); top = g_alwaysOnTop; }

    SetWindowPos(g_hwndCompact, top ? HWND_TOPMOST : HWND_NOTOPMOST,
        x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);

    // Rounded corners on the compact HWND
    {
        int d = 32;
        if (d > w) d = w;
        if (d > h) d = h;
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, d, d);
        if (rgn) SetWindowRgn(g_hwndCompact, rgn, TRUE);
    }

    g_cScrollFromBottom = 0;
    g_cStickBottom = true;
    g_cLastLogCount = 0;
    PaintCompactWindow();
    LogToFile("Compact chat mode ON (separate HWND, no DXGI)");
}

static void LeaveCompactChatMode() {
    g_compactUi.store(false);
    g_compactGeomApplied = false;

    // Hide compact-only HWND
    if (g_hwndCompact && IsWindow(g_hwndCompact))
        ShowWindow(g_hwndCompact, SW_HIDE);

    // Restore main overlay
    if (g_hwndOverlay) {
        if (g_haveFullGeom && g_fullW >= 600 && g_fullH >= 400) {
            SetWindowPos(g_hwndOverlay, nullptr, g_fullX, g_fullY, g_fullW, g_fullH,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else {
            SetWindowPos(g_hwndOverlay, nullptr, 100, 100, 920, 720,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ResizeSwapChainToClient();
        ApplyWindowShape(g_hwndOverlay, 0);
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

static void RenderCompactChat() {
    // Match ImGui size to HWND client — critical after resize
    float cw = 440.f, ch = 210.f;
    if (g_hwndOverlay) {
        RECT crc{};
        if (GetClientRect(g_hwndOverlay, &crc)) {
            cw = (float)(crc.right - crc.left);
            ch = (float)(crc.bottom - crc.top);
        }
    }
    if (cw < 32.f) cw = 32.f;
    if (ch < 32.f) ch = 32.f;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(cw, ch);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(cw, ch));

    float panelA = 0.75f;
    bool showTs = false;
    float lineGap = 1.0f;
    {
        std::lock_guard lock(g_settingsMtx);
        panelA = g_compactOpacity;
        showTs = g_showTimestamps;
        lineGap = g_chatLineGap;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f); // never dim whole UI
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
    // Black window bg so PresentCompactFrame can detect "panel" pixels (near-black)
    // and replace them with a translucent panel. Text stays non-black @ full bright.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.45f, 0.50f, 0.60f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.12f, 0.13f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.60f, 0.65f, 0.75f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    (void)panelA;

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("##JosebotCompactChat", nullptr, flags);

    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

    // Drag strip + Full button (hover only)
    {
        float barW = ImGui::GetContentRegionAvail().x;
        float barH = hovered ? 16.0f : 4.0f;
        float btnReserve = hovered ? 52.0f : 0.0f;
        ImGui::InvisibleButton("##cdrag", ImVec2(barW - btnReserve, barH));
        if (ImGui::IsItemActive() && g_hwndOverlay) {
            ReleaseCapture();
            SendMessageW(g_hwndOverlay, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        if (hovered) {
            ImGui::SameLine(0, 4);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.28f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.48f, 0.70f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            if (ImGui::SmallButton("Full"))
                ToggleCompactChatMode();
            ImGui::PopStyleColor(3);
        }
    }

    ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoBackground;
    if (hovered) childFlags |= ImGuiWindowFlags_AlwaysVerticalScrollbar;
    else childFlags |= ImGuiWindowFlags_NoScrollbar;

    ImGui::BeginChild("##clog", ImVec2(0, 0), false, childFlags);

    std::vector<ChatLine> logCopy;
    bool doScroll = false;
    {
        std::lock_guard lock(g_mtx);
        logCopy = g_log;
        doScroll = g_scroll;
        g_scroll = false;
    }

    size_t start = 0;
    if (logCopy.size() > 50) start = logCopy.size() - 50;

    auto solid = [](ImVec4 c) {
        c.w = 1.0f;
        // Slight lift so colored names stay vivid through any residual blend
        c.x = (std::min)(1.0f, c.x * 1.08f + 0.02f);
        c.y = (std::min)(1.0f, c.y * 1.08f + 0.02f);
        c.z = (std::min)(1.0f, c.z * 1.08f + 0.02f);
        return c;
        };

    for (size_t li = start; li < logCopy.size(); ++li) {
        const auto& line = logCopy[li];

        if (showTs && !line.timeStr.empty()) {
            ImGui::TextColored(ImVec4(0.72f, 0.74f, 0.80f, 1.0f), "%s", line.timeStr.c_str());
            ImGui::SameLine(0, 6);
        }

        if (line.outgoing) {
            ImGui::TextColored(ImVec4(0.55f, 0.90f, 1.0f, 1.0f), "> %s", line.plain.c_str());
        }
        else if (!line.segs.empty()) {
            bool any = false;
            for (size_t i = 0; i < line.segs.size(); ++i) {
                const auto& seg = line.segs[i];
                if (seg.text.empty()) continue;
                if (any) ImGui::SameLine(0, 0);
                ImGui::TextColored(solid(seg.color), "%s", seg.text.c_str());
                any = true;
            }
            if (!any)
                ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s", line.plain.c_str());
        }
        else {
            ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s", line.plain.c_str());
        }

        if (lineGap > 0.0f)
            ImGui::Dummy(ImVec2(0, lineGap));
    }

    if (doScroll)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(7);
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

// Alias so any leftover call still works
static void ApplyBeautifulTheme() { ApplyTheme(0); }



void RenderOverlay() {
    // Full Josebot UI only. Compact mode is handled exclusively in OverlayThread
    // (never call this function while g_compactUi is true).

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    // This makes ImGui cleanly cover the entire Native window, disabling ImGui's internal resize/move
    // Flush to edges — no outer padding so the custom bar meets the top of the HWND
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("JosebotMain", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
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
        ImGui::TextColored(ImVec4(0.78f, 0.85f, 0.98f, 1.0f), "Josebot");
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
            if (muted)
                statusPill("MUTED", ImVec4(1.0f, 0.50f, 0.35f, 1.0f));
            else if (g_aiInProgress)
                statusPill("THINKING", ImVec4(1.0f, 0.72f, 0.35f, 1.0f));
            else if (g_txInProgress)
                statusPill("TRANSLATING", ImVec4(0.50f, 0.78f, 1.0f, 1.0f));
            else
                statusPill("IDLE", ImVec4(0.52f, 0.62f, 0.78f, 1.0f));
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimal chat feed only  ·  F8");
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

    if (ImGui::BeginTabBar("##josebottabs", ImGuiTabBarFlags_FittingPolicyScroll)) {

        if (ImGui::BeginTabItem("Chat")) {

            bool locLiveTranslate, locShowTimestamps, locColoredChat, locUseNickname, locShowOwnFormatted;
            std::string locOutgoingLang;
            {
                std::lock_guard lock(g_settingsMtx);
                locLiveTranslate = g_liveTranslate;
                locShowTimestamps = g_showTimestamps;
                locColoredChat = g_coloredChat;
                locUseNickname = g_useNickname;
                locShowOwnFormatted = g_showOwnFormatted;
                locOutgoingLang = g_outgoingLang;
            }

            // Compact toolbar row
            bool changedUI = false;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 6));
            if (ImGui::Checkbox("Live translate", &locLiveTranslate)) changedUI = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically translate incoming chat.");
            ImGui::SameLine();
            if (ImGui::Checkbox("Timestamps", &locShowTimestamps)) changedUI = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Color AI", &locColoredChat)) changedUI = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Prepend &a so AI replies show green in-game.");
            ImGui::SameLine();
            if (ImGui::Checkbox("AI nick", &locUseNickname)) changedUI = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Sends /nickname &cJOSEBOT before the reply and /nickname off after.\n"
                "Also removes the 'Josebot >' prefix (the red JOSEBOT name is enough).");

            ImGui::SameLine(0, 16);
            ImGui::TextDisabled("Send as");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            const char* comboPreview = locOutgoingLang.empty() ? "As-is" : LangName(locOutgoingLang);
            if (!comboPreview) comboPreview = "As-is";
            if (ImGui::BeginCombo("##outlang", comboPreview)) {
                for (const auto& l : OUT_LANGS) {
                    bool is_selected = (locOutgoingLang == l.code);
                    if (ImGui::Selectable(l.name, is_selected)) {
                        locOutgoingLang = l.code;
                        changedUI = true;
                    }
                    if (is_selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Default language for your outgoing messages.\n"
                "Override per-line with /fr, /de, /en, etc.");

            ImGui::SameLine(0, 16);
            {
                float gap = 1.0f;
                { std::lock_guard lock(g_settingsMtx); gap = g_chatLineGap; }
                ImGui::SetNextItemWidth(100);
                if (ImGui::SliderFloat("##linegap", &gap, 0.0f, 12.0f, "%.0f px")) {
                    std::lock_guard lock(g_settingsMtx);
                    g_chatLineGap = gap;
                    g_settingsDirty = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Line spacing in the chat log.");
                ImGui::SameLine(0, 4);
                ImGui::TextDisabled("gap");
            }
            ImGui::PopStyleVar();

            if (changedUI) {
                std::lock_guard lock(g_settingsMtx);
                g_liveTranslate = locLiveTranslate;
                g_showTimestamps = locShowTimestamps;
                g_coloredChat = locColoredChat;
                g_useNickname = locUseNickname;
                g_outgoingLang = locOutgoingLang;
                g_settingsDirty = true;
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            float footer = ImGui::GetFrameHeightWithSpacing() * 2.6f;
            ImGui::BeginChild("##log", ImVec2(0, -footer), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);

            std::vector<uint64_t> toTranslate;
            std::vector<std::string> toTranslateBody;

            std::vector<ChatLine> logCopy;
            bool doScroll = false;
            {
                std::lock_guard lock(g_mtx);
                logCopy = g_log;
                doScroll = g_scroll;
                g_scroll = false;
            }

            for (auto& line : logCopy) {

                if (g_chatFilter[0]) {
                    const bool matchPlain = ContainsWord(line.plain, g_chatFilter);
                    const bool matchTx = !line.txText.empty() && ContainsWord(line.txText, g_chatFilter);
                    if (!matchPlain && !matchTx)
                        continue;
                }

                // Card-style entry: remember the top so we can draw a thin colored
                // accent bar down the left edge once we know how tall this entry is.
                const float cardTopY = ImGui::GetCursorScreenPos().y;
                ImGui::Indent(8.0f);

                if (locShowTimestamps) {
                    ImGui::TextDisabled("[%s]", line.timeStr.c_str());
                    ImGui::SameLine();
                }

                bool danglingSameLine = false;
                const bool renderSegs = !line.segs.empty() && (!line.outgoing || locShowOwnFormatted);

                if (line.outgoing && !renderSegs) {
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "> %s", line.plain.c_str());
                }
                else if (!renderSegs) {
                    ImGui::TextWrapped("%s", line.plain.c_str());
                }
                else {
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
                    float startX = ImGui::GetCursorPosX();
                    float wrapX = startX + ImGui::GetContentRegionAvail().x;

                    for (size_t i = 0; i < line.segs.size(); ++i) {
                        const std::string& segText = line.segs[i].text;
                        ImVec4 color = line.outgoing ? ImVec4(0.45f, 0.85f, 1.0f, 1.0f) : line.segs[i].color;

                        size_t pos = 0;
                        while (pos < segText.length()) {
                            size_t nextSpace = segText.find(' ', pos);
                            size_t end = (nextSpace != std::string::npos) ? nextSpace + 1 : segText.length();
                            std::string word = segText.substr(pos, end - pos);

                            ImVec2 size = ImGui::CalcTextSize(word.c_str());
                            if (ImGui::GetCursorPosX() + size.x > wrapX && ImGui::GetCursorPosX() > startX + 10.0f) {
                                ImGui::NewLine();
                            }

                            ImGui::TextColored(color, "%s", word.c_str());
                            ImGui::SameLine(0.0f, 0.0f);
                            pos = end;
                        }
                    }
                    ImGui::PopStyleVar();
                    danglingSameLine = true;
                }

                if (line.txStatus == TxStatus::Pending) {
                    if (!danglingSameLine) ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 0.8f), " [translating...]");
                    danglingSameLine = false;
                }
                else if (line.txStatus == TxStatus::Done) {
                    if (!danglingSameLine) ImGui::SameLine();
                    ImGui::TextColored(LangColor(line.txFrom), "[%s]", LangBadge(line.txFrom).c_str());
                    if (ImGui::IsItemHovered()) {
                        const char* detectedName = LangName(line.txFrom);
                        ImGui::SetTooltip("Detected: %s", detectedName ? detectedName : line.txFrom.c_str());
                    }

                    ImGui::NewLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.95f, 0.7f, 1.0f));
                    ImGui::TextWrapped("  L %s", line.txText.c_str());
                    ImGui::PopStyleColor();
                    danglingSameLine = false;
                }
                else if (!line.outgoing && !line.body.empty() && line.txStatus == TxStatus::None) {
                    ImGui::SameLine(0.0f, 4.0f);
                    ImGui::PushID((int)line.id);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.1f, 0.15f, 1.0f));
                    if (ImGui::SmallButton(" tr ")) {
                        toTranslate.push_back(line.id);
                        toTranslateBody.push_back(line.body);
                    }
                    ImGui::PopStyleColor(2);
                    ImGui::PopID();
                    danglingSameLine = false;
                }

                if (danglingSameLine) {
                    ImGui::NewLine();
                }

                ImGui::Unindent(8.0f);

                // Accent bar: blue for our own outgoing lines, green for the bot,
                // a soft neutral tone for everything else. Drawn last so it always
                // matches the final rendered height of this entry.
                {
                    const float cardBottomY = ImGui::GetCursorScreenPos().y;
                    if (cardBottomY > cardTopY) {
                        ImVec4 barCol = line.outgoing
                            ? ImVec4(0.45f, 0.85f, 1.0f, 0.85f)
                            : (LooksLikeBotLine(line.speaker, line.body, line.plain)
                                ? ImVec4(0.45f, 0.95f, 0.55f, 0.85f)
                                : ImVec4(0.45f, 0.50f, 0.62f, 0.55f));
                        ImDrawList* dlCard = ImGui::GetWindowDrawList();
                        const float barX = ImGui::GetWindowPos().x + 4.0f;
                        dlCard->AddLine(
                            ImVec2(barX, cardTopY),
                            ImVec2(barX, cardBottomY - 2.0f),
                            ImGui::GetColorU32(barCol), 3.0f);
                    }
                    float lineGap = 1.0f;
                    { std::lock_guard lock(g_settingsMtx); lineGap = g_chatLineGap; }
                    if (lineGap > 0.0f)
                        ImGui::Dummy(ImVec2(0, lineGap)); // gap before the next message
                }
            }
            if (doScroll) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();

            for (size_t i = 0; i < toTranslate.size(); ++i) {
                RequestIncomingTranslate(toTranslate[i], toTranslateBody[i], true);
            }

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 210);
            bool enter = ImGui::InputTextWithHint("##in", "Message…  (/fr /de /en to override language)", g_input, sizeof(g_input), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            bool click = ImGui::Button("Send", ImVec2(70, 0));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::InputTextWithHint("##filter", "Filter…", g_chatFilter, sizeof(g_chatFilter));

            if ((enter || click) && g_input[0]) {
                std::string text = g_input;
                g_input[0] = 0;
                ImGui::SetKeyboardFocusHere(-1);

                if (!HandleOwnerCommand(text, false)) {
                    if (g_pChat.load() && g_stringVT.load()) {
                        std::string langOverride;
                        bool hadPrefix = StripLangPrefix(text, langOverride);

                        std::string targetLang = hadPrefix ? langOverride : locOutgoingLang;

                        if (text.size() > HYTALE_CHAT_MAX)
                            text.resize(HYTALE_CHAT_MAX);

                        if (!targetLang.empty() && targetLang != "en" && !text.empty()) {
                            RequestOutgoingTranslate(text, targetLang);
                        }
                        else if (!text.empty()) {
                            QueueSend({ text, text, "", "", "" });
                        }
                    }
                }
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Ignored")) {
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

        if (ImGui::BeginTabItem("Settings")) {

            // Helper: section label used throughout Settings
            auto SectionLabel = [](const char* title) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.00f, 1.0f), "%s", title);
                ImGui::Spacing();
                };

            bool locAllowSelfTrigger, locShowOwnFormatted, locIncludeGlobal, locColoredChat, locUseNickname;
            int locMaxMemory, locMaxChat;
            {
                std::lock_guard lock(g_settingsMtx);
                locAllowSelfTrigger = g_allowSelfTrigger;
                locShowOwnFormatted = g_showOwnFormatted;
                locIncludeGlobal = g_includeGlobalChat;
                locColoredChat = g_coloredChat;
                locUseNickname = g_useNickname;
                locMaxMemory = g_maxMemory;
                locMaxChat = g_maxChatContext;
            }

            bool changedUI = false;

            // ── Identity & context ──────────────────────────────────────
            SectionLabel("Identity & context");
            ImGui::InputText("Local player name", g_localNameBuf, sizeof(g_localNameBuf));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::lock_guard lock(g_settingsMtx);
                g_localName = g_localNameBuf;
                g_settingsDirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your in-game name. Used to detect self-messages and ownership.");

            ImGui::SetNextItemWidth(180);
            if (ImGui::SliderInt("Memory turns", &locMaxMemory, 1, 8)) changedUI = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Recent user+assistant pairs kept for conversation memory.");
            ImGui::SameLine(0, 24);
            ImGui::SetNextItemWidth(180);
            if (ImGui::SliderInt("Chat context", &locMaxChat, 4, 60)) changedUI = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Recent server chat lines sent to the AI as context.");

            if (ImGui::Checkbox("Allow self-trigger", &locAllowSelfTrigger)) changedUI = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Respond when your own messages contain the wake word.");
            ImGui::SameLine(0, 20);
            if (ImGui::Checkbox("Show own formatted", &locShowOwnFormatted)) changedUI = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render your own messages with full color formatting in the overlay.");
            ImGui::SameLine(0, 20);
            if (ImGui::Checkbox("Global chat as context", &locIncludeGlobal)) changedUI = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Include recent global chat lines in the AI prompt.");

            // ── Reply appearance ────────────────────────────────────────
            SectionLabel("Reply appearance");
            if (ImGui::Checkbox("Colored AI (&a)", &locColoredChat)) changedUI = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Prepends &a so AI replies appear green in-game.");
            ImGui::SameLine(0, 20);
            if (ImGui::Checkbox("AI nickname", &locUseNickname)) changedUI = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Runs /nickname &cJOSEBOT before the reply, then /nickname off after.\n"
                "When on, the 'Josebot >' prefix is omitted.");

            if (changedUI) {
                std::lock_guard lock(g_settingsMtx);
                g_allowSelfTrigger = locAllowSelfTrigger;
                g_showOwnFormatted = locShowOwnFormatted;
                g_includeGlobalChat = locIncludeGlobal;
                g_coloredChat = locColoredChat;
                g_useNickname = locUseNickname;
                g_maxMemory = locMaxMemory;
                g_maxChatContext = locMaxChat;
                g_settingsDirty = true;
            }

            // ── Strings & wake words ────────────────────────────────────
            SectionLabel("Strings & wake words");
            ImGui::Columns(2, "##strcols", false);
            ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.48f);

            ImGui::InputText("AI prefix", g_aiPrefixBuf, sizeof(g_aiPrefixBuf));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::lock_guard lock(g_settingsMtx);
                g_aiPrefix = g_aiPrefixBuf;
                g_settingsDirty = true;
            }
            ImGui::InputText("Color code", g_aiColorCodeBuf, sizeof(g_aiColorCodeBuf));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::lock_guard lock(g_settingsMtx);
                g_aiColorCode = g_aiColorCodeBuf;
                g_settingsDirty = true;
            }
            ImGui::InputText("Nickname ON", g_nicknameOnBuf, sizeof(g_nicknameOnBuf));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::lock_guard lock(g_settingsMtx);
                g_nicknameOn = g_nicknameOnBuf;
                g_settingsDirty = true;
            }
            ImGui::InputText("Nickname OFF", g_nicknameOffBuf, sizeof(g_nicknameOffBuf));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::lock_guard lock(g_settingsMtx);
                g_nicknameOff = g_nicknameOffBuf;
                g_settingsDirty = true;
            }

            ImGui::NextColumn();

            ImGui::InputText("Wake word", g_wakeWordBuf, sizeof(g_wakeWordBuf));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::lock_guard lock(g_settingsMtx);
                g_wakeWord = g_wakeWordBuf;
                g_settingsDirty = true;
            }
            ImGui::InputText("2nd wake word", g_wakeWord2Buf, sizeof(g_wakeWord2Buf));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::lock_guard lock(g_settingsMtx);
                g_wakeWord2 = g_wakeWord2Buf;
                g_settingsDirty = true;
            }
            ImGui::InputText("Bot name", g_botNameBuf, sizeof(g_botNameBuf));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::lock_guard lock(g_settingsMtx);
                g_botName = g_botNameBuf;
                g_settingsDirty = true;
            }
            ImGui::InputText("Failure message", g_failMessageBuf, sizeof(g_failMessageBuf));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::lock_guard lock(g_settingsMtx);
                g_failMessage = g_failMessageBuf;
                g_settingsDirty = true;
            }

            ImGui::Columns(1);

            {
                int nickDelay = 400;
                { std::lock_guard lock(g_settingsMtx); nickDelay = g_nicknameDelayMs; }
                ImGui::SetNextItemWidth(220);
                if (ImGui::SliderInt("Nickname delay (ms)", &nickDelay, 0, 2000)) {
                    std::lock_guard lock(g_settingsMtx);
                    g_nicknameDelayMs = nickDelay;
                    g_settingsDirty = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Wait after /nickname ON before sending the AI reply.\n"
                    "Increase if the first line still shows your real name (try 600–800).");
            }

            // ── AI Provider ─────────────────────────────────────────────
            SectionLabel("AI provider");
            {
                int provIdx = 0;
                {
                    std::lock_guard lock(g_settingsMtx);
                    if (g_aiProvider == "openai") provIdx = 1;
                }
                const char* providers[] = { "Ollama", "OpenAI-compatible" };
                ImGui::SetNextItemWidth(220);
                if (ImGui::Combo("Provider", &provIdx, providers, 2)) {
                    std::lock_guard lock(g_settingsMtx);
                    g_aiProvider = (provIdx == 1) ? "openai" : "ollama";
                    g_settingsDirty = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Ollama: local host:port /api/chat.\n"
                    "OpenAI-compatible: base URL + API key (OpenAI, LiteLLM, vLLM, LM Studio, etc.).");

                std::string prov;
                { std::lock_guard lock(g_settingsMtx); prov = g_aiProvider; }
                if (prov != "openai") {
                    ImGui::InputText("Ollama model", g_ollamaModelBuf, sizeof(g_ollamaModelBuf));
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        std::lock_guard lock(g_settingsMtx);
                        g_ollamaModel = g_ollamaModelBuf;
                        g_settingsDirty = true;
                    }
                    ImGui::InputText("Ollama host", g_ollamaHostBuf, sizeof(g_ollamaHostBuf));
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        std::lock_guard lock(g_settingsMtx);
                        g_ollamaHost = g_ollamaHostBuf;
                        g_settingsDirty = true;
                    }
                    {
                        int port = 11434;
                        { std::lock_guard lock(g_settingsMtx); port = g_ollamaPort; }
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::InputInt("Ollama port", &port)) {
                            if (port < 1) port = 1;
                            if (port > 65535) port = 65535;
                            std::lock_guard lock(g_settingsMtx);
                            g_ollamaPort = port;
                            g_settingsDirty = true;
                        }
                    }
                }
                else {
                    ImGui::InputText("OpenAI base URL", g_openaiBaseBuf, sizeof(g_openaiBaseBuf));
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        std::lock_guard lock(g_settingsMtx);
                        g_openaiBaseUrl = g_openaiBaseBuf;
                        g_settingsDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "e.g. https://api.openai.com or http://127.0.0.1:1234\n"
                        "Do not include /v1/chat/completions — appended automatically.");
                    ImGui::InputText("API key", g_openaiKeyBuf, sizeof(g_openaiKeyBuf), ImGuiInputTextFlags_Password);
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        std::lock_guard lock(g_settingsMtx);
                        g_openaiApiKey = g_openaiKeyBuf;
                        g_settingsDirty = true;
                    }
                    ImGui::InputText("OpenAI model", g_openaiModelBuf, sizeof(g_openaiModelBuf));
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        std::lock_guard lock(g_settingsMtx);
                        g_openaiModel = g_openaiModelBuf;
                        g_settingsDirty = true;
                    }
                }
            }
            ImGui::Spacing();
            if (ImGui::TreeNodeEx("Web search (LangSearch)", ImGuiTreeNodeFlags_Framed)) {
                bool wsOn = false;
                int wsCount = 4, wsMax = 1600;
                bool wsSum = false;
                {
                    std::lock_guard lock(g_settingsMtx);
                    wsOn = g_webSearchEnabled;
                    wsCount = g_webSearchCount;
                    wsSum = g_webSearchSummary;
                    wsMax = g_webSearchMaxChars;
                }
                bool ch = false;
                if (ImGui::Checkbox("Enable web_search tool", &wsOn)) ch = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "When off, the tool is not sent to the model (saves tokens).\n"
                    "When on, the model can call web_search for live internet facts.");
                ImGui::InputText("LangSearch API key", g_webSearchKeyBuf, sizeof(g_webSearchKeyBuf),
                    ImGuiInputTextFlags_Password);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    std::lock_guard lock(g_settingsMtx);
                    g_webSearchApiKey = g_webSearchKeyBuf;
                    g_settingsDirty = true;
                }
                if (ImGui::SliderInt("Result count", &wsCount, 1, 10)) ch = true;
                if (ImGui::Checkbox("Long summaries", &wsSum)) ch = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Off (default): short snippets only — fewer tokens.\n"
                    "On: full summaries from API (can be large).");
                ImGui::InputText("Freshness", g_webSearchFreshBuf, sizeof(g_webSearchFreshBuf));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "oneDay | oneWeek | oneMonth | oneYear | noLimit");
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    std::lock_guard lock(g_settingsMtx);
                    g_webSearchFreshness = g_webSearchFreshBuf;
                    g_settingsDirty = true;
                }
                if (ImGui::SliderInt("Max result chars", &wsMax, 400, 4000)) ch = true;
                if (ch) {
                    std::lock_guard lock(g_settingsMtx);
                    g_webSearchEnabled = wsOn;
                    g_webSearchCount = wsCount;
                    g_webSearchSummary = wsSum;
                    g_webSearchMaxChars = wsMax;
                    g_settingsDirty = true;
                }
                ImGui::TreePop();
            }

            ImGui::Spacing();
            SectionLabel("Generation");
            {
                float temp = 1.0f;
                int npred = 512;
                int nctx = 32768;
                { std::lock_guard lock(g_settingsMtx); temp = g_ollamaTemperature; npred = g_ollamaNumPredict; nctx = g_ollamaNumCtx; }
                ImGui::SetNextItemWidth(200);
                if (ImGui::SliderFloat("Temperature", &temp, 0.0f, 2.0f, "%.2f")) {
                    std::lock_guard lock(g_settingsMtx);
                    g_ollamaTemperature = temp;
                    g_settingsDirty = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Higher = more random / creative. Lower = more focused.\n"
                    "Typical: 0.3–1.0 steady · 1.0 default · 1.5+ wild.");
                ImGui::SameLine(0, 20);
                ImGui::SetNextItemWidth(200);
                if (ImGui::SliderInt("Max tokens", &npred, 16, 16549)) {
                    std::lock_guard lock(g_settingsMtx);
                    g_ollamaNumPredict = npred;
                    g_settingsDirty = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Hard cap on generated tokens.\n"
                    "For short chat replies 512–768 is usually enough.");
                ImGui::SetNextItemWidth(220);
                if (ImGui::SliderInt("Context window", &nctx, 2048, 131072)) {
                    std::lock_guard lock(g_settingsMtx);
                    g_ollamaNumCtx = nctx;
                    g_settingsDirty = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Ollama num_ctx. Higher uses more VRAM.\n"
                    "Ignored by pure OpenAI cloud models.");
            }
            {
                bool muted = false, top = false, blockSlash = true, think = false, wlMode = false;
                float opacity = 1.0f;
                int theme = 0;
                std::string thinkLvl;
                {
                    std::lock_guard lock(g_settingsMtx);
                    muted = g_botMuted; top = g_alwaysOnTop; opacity = g_windowOpacity;
                    blockSlash = g_blockLeadingSlash; think = g_thinkEnabled;
                    wlMode = g_whitelistMode; theme = g_themePreset;
                    thinkLvl = g_thinkLevel;
                }
                bool ch = false, styleCh = false;
                SectionLabel("Behavior");
                if (ImGui::Checkbox("Bot muted (AFK)", &muted)) ch = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("When muted the bot ignores wake-words and stays silent.");
                ImGui::SameLine(0, 20);
                if (ImGui::Checkbox("Block leading / or !", &blockSlash)) ch = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ignore messages that start with / or ! (commands).");

                ImGui::Spacing();
                if (ImGui::TreeNodeEx("Auto reactions (system chat)", ImGuiTreeNodeFlags_Framed)) {
                    bool aw = false, awb = false, agg = false;
                    bool wvShow = true, wvAuto = false;
                    bool mcShow = true, mcAuto = false;
                    bool ccShow = true, ccAuto = false;
                    bool clShow = true, clAuto = false;
                    bool pnShow = true, pnAuto = false;
                    {
                        std::lock_guard lock(g_settingsMtx);
                        aw = g_autoWelcome;
                        awb = g_autoWelcomeBack;
                        agg = g_autoGg;
                        wvShow = g_wordValueShow;
                        wvAuto = g_wordValueAuto;
                        mcShow = g_morseShow;
                        mcAuto = g_morseAuto;
                        ccShow = g_caesarShow;
                        ccAuto = g_caesarAuto;
                        clShow = g_countLettersShow;
                        clAuto = g_countLettersAuto;
                        pnShow = g_patternShow;
                        pnAuto = g_patternAuto;
                    }
                    bool ar = false;

                    // --- Classic reactions ---
                    if (ImGui::Checkbox("Auto welcome (\"Welcome...\")", &aw)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("When a chat line starts with Welcome, auto-send: welcome");
                    if (ImGui::Checkbox("Auto welcome back (\"[+]...\")", &awb)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("When a chat line starts with [+], auto-send: wb");
                    if (ImGui::Checkbox("Auto GG (\"[!] ... reached Ascension\")", &agg)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("When a line starts with [!] and contains reached Ascension, auto-send: gg");

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.85f, 0.90f, 0.55f, 1.0f), "Chat minigames (local, no API)");

                    // --- Word Value (A=1 … Z=26) ---
                    ImGui::TextDisabled("Word Value");
                    if (ImGui::Checkbox("Show answer under chat##wv", &wvShow)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Detect \"What is WORD worth?\" (with A=1 / WORD VALUE) and show the letter-sum\n"
                        "underneath the line (blue [WV] badge, same style as live translate).");
                    if (ImGui::Checkbox("Auto-reply the number##wv", &wvAuto)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Also queue-send the calculated number.\n"
                        "Respects mute + 2.5 s cooldown. Works even if \"Show\" is off.");

                    ImGui::Spacing();

                    // --- Morse Code ---
                    ImGui::TextDisabled("Morse Code");
                    if (ImGui::Checkbox("Show decoded answer under chat##mc", &mcShow)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Detect \"MORSE CODE\" / \"Decode this Morse:\" lines, decode the dots & dashes\n"
                        "locally, and show the word underneath (blue [MC] badge).");
                    if (ImGui::Checkbox("Auto-reply the decoded word##mc", &mcAuto)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Also queue-send the decoded word.\n"
                        "Respects mute + 2.5 s cooldown. Works even if \"Show\" is off.");

                    ImGui::Spacing();

                    // --- Caesar Cipher ---
                    ImGui::TextDisabled("Caesar Cipher");
                    if (ImGui::Checkbox("Show decoded answer under chat##cc", &ccShow)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Detect \"CAESAR CIPHER\" / \"Caesar cipher, shift N -- decode: TEXT\" lines,\n"
                        "decrypt with the given shift, and show the plaintext underneath (blue [CC] badge).");
                    if (ImGui::Checkbox("Auto-reply the decoded word##cc", &ccAuto)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Also queue-send the decrypted word.\n"
                        "Respects mute + 2.5 s cooldown. Works even if \"Show\" is off.");

                    ImGui::Spacing();

                    // --- Count the Letters ---
                    ImGui::TextDisabled("Count the Letters");
                    if (ImGui::Checkbox("Show answer under chat##cl", &clShow)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Detect \"COUNT THE LETTERS! How many letters are in WORD?\" and show the letter count\n"
                        "underneath the line (green [CL] badge).");
                    if (ImGui::Checkbox("Auto-reply the count##cl", &clAuto)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Also queue-send the letter count.\n"
                        "Respects mute + 2.5 s cooldown. Works even if \"Show\" is off.");

                    ImGui::Spacing();

                    // --- Pattern ---
                    ImGui::TextDisabled("Pattern (what comes next)");
                    if (ImGui::Checkbox("Show answer under chat##pn", &pnShow)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Detect \"PATTERN! What comes next: 81, 27, 9, 3, ?\" style lines and solve locally\n"
                        "(geometric / arithmetic / 2nd-order / Fibonacci). Shows orange [PN] badge.");
                    if (ImGui::Checkbox("Auto-reply the next number##pn", &pnAuto)) ar = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Also queue-send the next number.\n"
                        "Respects mute + 2.5 s cooldown. Works even if \"Show\" is off.");

                    if (ar) {
                        std::lock_guard lock(g_settingsMtx);
                        g_autoWelcome = aw;
                        g_autoWelcomeBack = awb;
                        g_autoGg = agg;
                        g_wordValueShow = wvShow;
                        g_wordValueAuto = wvAuto;
                        g_morseShow = mcShow;
                        g_morseAuto = mcAuto;
                        g_caesarShow = ccShow;
                        g_caesarAuto = ccAuto;
                        g_countLettersShow = clShow;
                        g_countLettersAuto = clAuto;
                        g_patternShow = pnShow;
                        g_patternAuto = pnAuto;
                        g_settingsDirty = true;
                    }
                    ImGui::TreePop();
                }

                ImGui::Spacing();
                bool thinkOpen = ImGui::TreeNodeEx("Think mode (Ollama reasoning)", ImGuiTreeNodeFlags_Framed);
                if (thinkOpen) {
                    if (ImGui::Checkbox("Enable think", &think)) ch = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "When on, the model returns separate thinking output in addition to the reply.\n"
                        "Supported levels: high / medium / low / max (model-dependent).");

                    // Map stored string -> combo index
                    const char* thinkLevels[] = { "On (true)", "Low", "Medium", "High", "Max", "Default - Off (false)" };
                    int thinkIdx = 0;
                    {
                        std::string tl = ToLower(TrimCopy(thinkLvl));
                        if (tl == "low") thinkIdx = 1;
                        else if (tl == "medium" || tl == "med") thinkIdx = 2;
                        else if (tl == "high") thinkIdx = 3;
                        else if (tl == "max") thinkIdx = 4;
                        else if (tl == "false" || tl == "0" || tl == "off") thinkIdx = 5;
                        else thinkIdx = 0;
                    }
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::Combo("Think level", &thinkIdx, thinkLevels, 6)) {
                        ch = true;
                        switch (thinkIdx) {
                        case 1: thinkLvl = "low"; break;
                        case 2: thinkLvl = "medium"; break;
                        case 3: thinkLvl = "high"; break;
                        case 4: thinkLvl = "max"; break;
                        case 5: thinkLvl = "false"; think = false; break;
                        default: thinkLvl = "false"; break;
                        }
                        if (thinkIdx >= 1 && thinkIdx <= 4) think = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "true/false or high/medium/low/max.\n"
                        "\"max\" requests the highest thinking level on supported models.");
                }
                if (thinkOpen) ImGui::TreePop();

                SectionLabel("Window & theme");
                if (ImGui::Checkbox("Whitelist mode", &wlMode)) ch = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "When ON: only players on the Whitelist (Ignored tab) can trigger the AI.\n"
                    "Everyone else is treated as ignored for wake-words.\n"
                    "Manage the list under the Ignored tab.");
                {
                    bool compact = false;
                    { std::lock_guard lock(g_settingsMtx); compact = g_compactChatMode; }
                    if (ImGui::Checkbox("Compact chat mode (chat feed only)", &compact)) {
                        {
                            std::lock_guard lock(g_settingsMtx);
                            g_compactChatMode = compact;
                            g_settingsDirty = true;
                            g_windowStyleDirty = true;
                        }
                        g_compactUi.store(compact);
                        if (compact) EnterCompactChatMode();
                        else LeaveCompactChatMode();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Minimal overlay: only the live chat feed.\n"
                        "Rounded dark panel sized like vanilla chat.\n"
                        "Hotkey: F8 to toggle. Drag the top edge to move.");
                    {
                        float cop = 0.78f;
                        { std::lock_guard lock(g_settingsMtx); cop = g_compactOpacity; }
                        ImGui::SetNextItemWidth(160);
                        if (ImGui::SliderFloat("Compact opacity", &cop, 0.30f, 1.00f, "%.2f")) {
                            std::lock_guard lock(g_settingsMtx);
                            g_compactOpacity = cop;
                            g_settingsDirty = true;
                            g_windowStyleDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                            "How see-through the compact chat panel is.\n"
                            "Lower = more of the game shows through.");
                    }
                    {
                        float cfs = 17.0f;
                        { std::lock_guard lock(g_settingsMtx); cfs = g_compactFontSize; }
                        ImGui::SetNextItemWidth(160);
                        if (ImGui::SliderFloat("Compact font size", &cfs, 10.0f, 28.0f, "%.0f")) {
                            std::lock_guard lock(g_settingsMtx);
                            g_compactFontSize = cfs;
                            g_settingsDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                            "Font height used by the compact GDI chat overlay only.\n"
                            "Independent of the full UI font size. Line height scales with it.");
                    }
                    {
                        int cml = 80;
                        { std::lock_guard lock(g_settingsMtx); cml = g_compactMaxLines; }
                        ImGui::SetNextItemWidth(160);
                        if (ImGui::SliderInt("Compact max lines", &cml, 20, 300)) {
                            std::lock_guard lock(g_settingsMtx);
                            g_compactMaxLines = cml;
                            g_settingsDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                            "How many recent chat lines the compact overlay keeps for painting.\n"
                            "Lower = less work / memory if the log is very long.");
                    }
                    {
                        float cpx = 10.0f;
                        { std::lock_guard lock(g_settingsMtx); cpx = g_compactPadX; }
                        ImGui::SetNextItemWidth(160);
                        if (ImGui::SliderFloat("Compact padding", &cpx, 4.0f, 32.0f, "%.0f px")) {
                            std::lock_guard lock(g_settingsMtx);
                            g_compactPadX = cpx;
                            g_settingsDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                            "Left/right text padding inside the compact panel.");
                    }

                }
                if (ImGui::Checkbox("Always on top", &top)) { ch = true; styleCh = true; }
                if (ImGui::SliderFloat("Window opacity", &opacity, 0.30f, 1.00f, "%.2f")) { ch = true; styleCh = true; }
                const char* themes[] = { "Default", "Purple", "Green", "Blue", "Crimson", "Custom" };
                if (ImGui::Combo("Theme preset", &theme, themes, 6)) { ch = true; g_themeDirty = true; }
                if (theme == 5) {
                    ImGui::Indent(12.0f);
                    bool accentCh = false;
                    if (ImGui::ColorEdit3("Accent color", g_customAccent,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) {
                        accentCh = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Pick any color — every window element (buttons, tabs, headers,\n"
                        "scrollbar, highlights) is generated from this one accent.");
                    if (ImGui::Checkbox("Darker background", &g_customAccentDark)) accentCh = true;
                    if (accentCh) {
                        ch = true;
                        g_themeDirty = true;
                        g_settingsDirty = true;
                    }
                    ImGui::Unindent(12.0f);
                }
                {
                    float fs = g_fontSize, us = g_uiScale, ur = g_uiRounding;
                    if (ImGui::SliderFloat("Font size", &fs, 12.0f, 22.0f, "%.0f")) {
                        g_fontSize = fs; g_fontDirty = true; ch = true;
                    }
                    if (ImGui::SliderFloat("UI scale", &us, 0.85f, 1.35f, "%.2f")) {
                        g_uiScale = us; ch = true;
                        ImGui::GetIO().FontGlobalScale = g_uiScale;
                    }
                    if (ImGui::SliderFloat("Rounding", &ur, 0.0f, 12.0f, "%.0f")) {
                        g_uiRounding = ur; g_themeDirty = true; ch = true;
                    }
                }
                if (ch) {
                    std::lock_guard lock(g_settingsMtx);
                    g_botMuted = muted; g_alwaysOnTop = top; g_windowOpacity = opacity;
                    g_blockLeadingSlash = blockSlash; g_thinkEnabled = think;
                    g_thinkLevel = thinkLvl;
                    strncpy_s(g_thinkLevelBuf, thinkLvl.c_str(), 15);
                    g_whitelistMode = wlMode;
                    if (g_themePreset != theme) {
                        g_themePreset = theme;
                        g_themeDirty = true;
                    }
                    else {
                        g_themePreset = theme;
                    }
                    g_settingsDirty = true;
                    if (styleCh) g_windowStyleDirty = true;
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            SectionLabel("Private encrypted chat");
            if (ImGui::CollapsingHeader("Encrypted messaging")) {
                ImGui::TextWrapped(
                    "Shared passphrase with friends who also run Josebot. AES-256-GCM ciphertext looks like ~jb1.... "
                    "Prefer /msg so only that player sees the wire — still unreadable without the key.");
                ImGui::InputText("Passphrase", g_privatePassBuf, sizeof(g_privatePassBuf),
                    ImGuiInputTextFlags_Password);
                ImGui::SameLine();
                if (ImGui::Button("Set Key", ImVec2(80, 0))) {
                    std::string pass = TrimCopy(g_privatePassBuf);
                    Extras::SetPrivatePassphrase(pass);
                    bool ok = Extras::HasPrivateKey();
                    std::string fp = Extras::PrivateKeyFingerprint();
                    LogToFile(ok
                        ? ("Private key set from UI  fp=" + fp +
                            " pass_len=" + std::to_string(pass.size()))
                        : "Private key set failed from UI");
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear Key", ImVec2(90, 0))) {
                    Extras::ClearPrivateKey();
                    g_privateMode = false;
                    g_privatePassBuf[0] = 0;
                    LogToFile("Private key cleared from UI");
                }
                if (Extras::HasPrivateKey()) {
                    ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f),
                        "Key loaded  fp=%s", Extras::PrivateKeyFingerprint().c_str());
                }
                else {
                    ImGui::TextDisabled("No key loaded");
                }
                ImGui::Checkbox("Decrypt private msgs in overlay", &g_privateDecryptDisplay);
                ImGui::Checkbox("Send via /msg (recommended)", &g_privateViaMsg);
                ImGui::InputTextWithHint("##privtgt", "Target player for /msg…", g_privateTargetBuf, sizeof(g_privateTargetBuf));
                ImGui::InputTextWithHint("##privmsg", "Private message to send…", g_privateMsgBuf, sizeof(g_privateMsgBuf));
                ImGui::SameLine();
                if (ImGui::Button("Send Private", ImVec2(110, 0)) && g_privateMsgBuf[0]) {
                    if (!Extras::HasPrivateKey()) {
                        LogToFile("Send Private: no key");
                    }
                    else {
                        std::string tgt = TrimCopy(g_privateTargetBuf);
                        bool via = g_privateViaMsg && !tgt.empty();
                        if (!QueuePrivateEncrypted(g_privateMsgBuf, tgt, via))
                            LogToFile("Send Private failed (too long or crypto)");
                        else
                            g_privateMsgBuf[0] = 0;
                    }
                }
            }

            SectionLabel("Actions");
            if (ImGui::Button("Save Settings", ImVec2(130, 0)) || g_settingsDirty) {
                SaveSettings();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear AI Memory", ImVec2(130, 0))) {
                std::lock_guard lock(g_mtx);
                g_memory.clear();
                g_globalChat.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear Chat Log", ImVec2(130, 0))) {
                std::lock_guard lock(g_mtx);
                g_log.clear();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.20f, 0.20f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.28f, 0.28f, 1.0f));
            if (ImGui::Button("Cancel AI", ImVec2(110, 0))) {
                CancelAI();
            }
            ImGui::PopStyleColor(2);

            ImGui::Spacing();
            if (ImGui::CollapsingHeader("Commands reference")) {
                std::string cmdPrefix = "josebot";
                { std::lock_guard lock(g_settingsMtx); if (!g_wakeWord.empty()) cmdPrefix = g_wakeWord; }
                ImGui::BulletText("%s ignore <name>", cmdPrefix.c_str());
                ImGui::BulletText("%s unignore <name>", cmdPrefix.c_str());
                ImGui::BulletText("%s ignorelist", cmdPrefix.c_str());
                ImGui::BulletText("%s clearmemory", cmdPrefix.c_str());
                ImGui::BulletText("%s cancel  (stop AI + clear send queue)", cmdPrefix.c_str());
                ImGui::BulletText("%s reloadprompt", cmdPrefix.c_str());
                ImGui::BulletText("%s live (toggle auto translate)", cmdPrefix.c_str());
                ImGui::BulletText("%s color (toggle &a green replies)", cmdPrefix.c_str());
                ImGui::BulletText("%s nick (toggle /nickname)", cmdPrefix.c_str());
                ImGui::BulletText("%s mute / think / sendlang <lang>", cmdPrefix.c_str());
                ImGui::BulletText("%s echo <text with ${math(1+1)} ${time} ${uptime}>", cmdPrefix.c_str());
                ImGui::BulletText("%s context  (dump AI prompt to file)", cmdPrefix.c_str());
                ImGui::BulletText("%s reminders / cancelrem <id>", cmdPrefix.c_str());
                ImGui::BulletText("%s privkey <pass> / privsend <player> <msg>", cmdPrefix.c_str());
                ImGui::BulletText("%s privsend public <msg>  (encrypted in public chat)", cmdPrefix.c_str());
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("AI & Translation")) {
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
        if (ImGui::BeginTabItem("Test Chat")) {
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

            for (const auto& m : copy) {
                if (m.role == "user") {
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "You:");
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.90f, 0.95f, 1.0f));
                    ImGui::TextWrapped("%s", m.content.c_str());
                    ImGui::PopStyleColor();
                }
                else {
                    ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f), "Josebot:");
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.95f, 0.80f, 1.0f));
                    ImGui::TextWrapped("%s", m.content.c_str());
                    ImGui::PopStyleColor();
                }
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
        if (ImGui::BeginTabItem("Tools")) {
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
                ImGui::TextUnformatted(g_toolResult.c_str());
            if (g_toolScroll) {
                ImGui::SetScrollHereY(0.0f);
                g_toolScroll = false;
            }
            ImGui::EndChild();

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    if (g_settingsDirty)
        SaveSettings();

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

        // Keep rounded clip region in sync when the HWND is resized in compact mode
        {
            bool compact = false;
            { std::lock_guard lock(g_settingsMtx); compact = g_compactChatMode; }
            if (compact)
                ApplyWindowShape(hWnd, 16);
        }
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

void OverlayThread() {
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
    wc.lpszClassName = L"JosebotOverlayClass";
    RegisterClassExW(&wc);

    // Borderless look (NCCALCSIZE clears chrome) but keep THICKFRAME so Windows
    // treats the HWND as resizable; actual grips come from WM_NCHITTEST.
    g_hwndOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_APPWINDOW, wc.lpszClassName, L"Josebot",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN | WS_VISIBLE,
        100, 100, 920, 720,
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

    ShowWindow(g_hwndOverlay, SW_SHOWDEFAULT);
    UpdateWindow(g_hwndOverlay);

    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(g_hwndOverlay);
    ImGui_ImplDX11_Init(g_pd3dDeviceOverlay, g_pd3dDeviceContextOverlay);

    LoadSettings();
    LoadIgnored();
    LoadWhitelist();
    ReloadSystemPrompt(true);

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

    // Load custom font BEFORE the render loop (never rebuild mid-frame).
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
        if (GetFileAttributesA(fontPath) == INVALID_FILE_ATTRIBUTES)
            fontPath = "C:\\Windows\\Fonts\\arial.ttf";
        ImFont* font = nullptr;
        if (GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES)
            font = io.Fonts->AddFontFromFileTTF(fontPath, g_fontSize);
        if (!font)
            io.Fonts->AddFontDefault();
        io.FontGlobalScale = g_uiScale;
        g_fontDirty = false;
        LogToFile(std::string("Overlay font ready size=") + std::to_string(g_fontSize));
    }

    g_startTime = std::chrono::steady_clock::now();
    ApplyWindowStyle();

    {
        bool wantCompact = false;
        { std::lock_guard lock(g_settingsMtx); wantCompact = g_compactChatMode; }
        g_compactUi.store(wantCompact);
        if (wantCompact)
            EnterCompactChatMode();
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

        // F8 — toggle compact (Shaeldor-style) chat mode
        static bool f8Pressed = false;
        if (GetAsyncKeyState(VK_F8) & 0x8000) {
            if (!f8Pressed) {
                f8Pressed = true;
                ToggleCompactChatMode();
            }
        }
        else {
            f8Pressed = false;
        }

        // Handle INSERT hotkey to toggle window visibility
        static bool insertPressed = false;
        if (GetAsyncKeyState(VK_INSERT) & 0x8000) {
            if (!insertPressed) {
                g_show = !g_show;
                insertPressed = true;
                if (g_show) {
                    ShowWindow(g_hwndOverlay, SW_SHOW);
                    SetForegroundWindow(g_hwndOverlay);
                }
                else {
                    ShowWindow(g_hwndOverlay, SW_HIDE);
                }
            }
        }
        else {
            insertPressed = false;
        }

        // Save GPU and CPU if window is hidden
        if (!g_show) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // Font size change: rebuild atlas BEFORE NewFrame (rebuilding after NewFrame crashes).
        if (g_fontDirty) {
            ImGuiIO& io = ImGui::GetIO();
            io.Fonts->Clear();
            const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
            if (GetFileAttributesA(fontPath) == INVALID_FILE_ATTRIBUTES)
                fontPath = "C:\\Windows\\Fonts\\arial.ttf";
            ImFont* font = nullptr;
            if (GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES)
                font = io.Fonts->AddFontFromFileTTF(fontPath, g_fontSize);
            if (!font)
                io.Fonts->AddFontDefault();
            io.FontGlobalScale = g_uiScale;
            // Drop GPU font texture; backend recreates it on the next NewFrame.
            ImGui_ImplDX11_InvalidateDeviceObjects();
            g_fontDirty = false;
        }

        const bool compactNow = g_compactUi.load();

        if (g_windowStyleDirty) {
            ApplyWindowStyle();
            g_windowStyleDirty = false;
        }
        if (g_settingsDirty)
            SaveSettings();

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
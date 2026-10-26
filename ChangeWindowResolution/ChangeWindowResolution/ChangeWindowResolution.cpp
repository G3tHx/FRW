#include <windows.h>
#include <windowsx.h>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <io.h>
#include <dwmapi.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <memory>
#include <mutex>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")

// Constants
constexpr int CUSTOM_TITLE_HEIGHT = 30;
constexpr int TITLE_MAX_LENGTH = 256;
constexpr int THREAD_REFRESH_MS = 5;

// Metro 2033 Visual Palette Constants for Win32 Drawing
constexpr COLORREF TITLE_BAR_COLOR = RGB(22, 26, 24);      // Bunker dark metallic grey-green
constexpr COLORREF TITLE_TEXT_COLOR = RGB(50, 230, 120);    // Glowing CRT phosphor green
constexpr COLORREF ACCENT_LINE_COLOR = RGB(255, 160, 0);   // Metro hazard amber

// Metro 2033 ANSI Color Codes for Terminal UI
namespace MetroTheme {
    const wchar_t* RESET        = L"\033[0m";
    const wchar_t* BOLD         = L"\033[1m";
    const wchar_t* DIM          = L"\033[2m";

    // CRT Phosphor Greens
    const wchar_t* GREEN        = L"\033[38;2;50;225;100m";
    const wchar_t* BRIGHT_GREEN = L"\033[38;2;100;255;140m";
    const wchar_t* DARK_GREEN   = L"\033[38;2;25;120;50m";

    // Hazard Ambers & Reds
    const wchar_t* AMBER        = L"\033[38;2;255;170;0m";
    const wchar_t* HAZARD_RED   = L"\033[38;2;235;60;50m";
    const wchar_t* WARN_YELLOW  = L"\033[38;2;240;220;60m";

    // Metals & Shadows
    const wchar_t* STEEL        = L"\033[38;2;140;150;145m";
    const wchar_t* DARK_STEEL   = L"\033[38;2;75;85;80m";
    const wchar_t* CYAN_GLOW    = L"\033[38;2;0;210;220m";
}

// Structures and global variables
struct WindowInfo {
    HWND hwnd;
    std::wstring title;
    DWORD processId;
};

struct ResizeData {
    int width;
    int height;
    bool keepForcing;
};

// RAII wrapper for DC handles
class DCWrapper {
public:
    DCWrapper(HWND hwnd, HRGN hrgn = nullptr) : m_hwnd(hwnd), m_hdc(nullptr) {
        if (hrgn)
            m_hdc = GetDCEx(hwnd, hrgn, DCX_WINDOW | DCX_INTERSECTRGN);
        if (!m_hdc)
            m_hdc = GetWindowDC(hwnd);
    }

    ~DCWrapper() {
        if (m_hdc)
            ReleaseDC(m_hwnd, m_hdc);
    }

    operator HDC() const { return m_hdc; }

private:
    HWND m_hwnd;
    HDC m_hdc;
};

// RAII wrapper for brushes
class BrushWrapper {
public:
    BrushWrapper(COLORREF color) : m_brush(CreateSolidBrush(color)) {}
    ~BrushWrapper() { if (m_brush) DeleteObject(m_brush); }
    operator HBRUSH() const { return m_brush; }

private:
    HBRUSH m_brush;
};

// Global state protected by mutex
std::mutex g_stateMutex;
std::map<HWND, WNDPROC> g_originalWndProcs;
std::map<HWND, ResizeData> g_windowSizes;
HHOOK g_messageHook = NULL;
HHOOK g_cbtHook = NULL;
bool g_isDragging = false;
POINT g_dragStart = { 0, 0 };

// Helper Functions
void SetupConsoleForCyrillic() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }

    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
}

void DebugLog(const std::wstring& message) {
    using namespace MetroTheme;
    std::wcout << DARK_STEEL << L"[2033-LOG] " << AMBER << message << RESET << std::endl;
}

std::wstring GetProcessNameById(DWORD processId) {
    wchar_t processName[MAX_PATH] = L"Неизвестно";
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);

    if (hProcess) {
        HMODULE hMod;
        DWORD cbNeeded;

        if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), &cbNeeded)) {
            GetModuleBaseNameW(hProcess, hMod, processName, sizeof(processName) / sizeof(wchar_t));
        }
        CloseHandle(hProcess);
    }

    return processName;
}

void CenterWindowOnScreen(HWND hwnd, int width, int height) {
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    int posX = (screenWidth - width) / 2;
    int posY = (screenHeight - height) / 2;

    SetWindowPos(hwnd, NULL, posX, posY, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void DrawCustomTitleBar(HWND hwnd, HDC hdc) {
    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);
    int width = windowRect.right - windowRect.left;

    // Create rectangle for title bar
    RECT titleRect = { 0, 0, width, CUSTOM_TITLE_HEIGHT };

    // Draw title bar background in bunker dark metallic green
    BrushWrapper titleBrush(TITLE_BAR_COLOR);
    FillRect(hdc, &titleRect, titleBrush);

    // Draw hazard bottom border line
    RECT accentRect = { 0, CUSTOM_TITLE_HEIGHT - 2, width, CUSTOM_TITLE_HEIGHT };
    BrushWrapper accentBrush(ACCENT_LINE_COLOR);
    FillRect(hdc, &accentRect, accentBrush);

    // Draw window title with Metro 2033 styling prefix
    wchar_t title[TITLE_MAX_LENGTH];
    GetWindowTextW(hwnd, title, TITLE_MAX_LENGTH);

    std::wstring formattedTitle = L"[ ☢ METRO 2033 ☢ ]  " + std::wstring(title);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, TITLE_TEXT_COLOR);

    RECT textRect = { 10, 0, width - 10, CUSTOM_TITLE_HEIGHT - 2 };
    DrawTextW(hdc, formattedTitle.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER);
}

// Window enumeration functions
void FindChildWindows(HWND parentHwnd, std::vector<WindowInfo>& childWindows) {
    EnumChildWindows(parentHwnd, [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto windowList = reinterpret_cast<std::vector<WindowInfo>*>(lParam);
        wchar_t title[TITLE_MAX_LENGTH] = { 0 };

        if (IsWindowVisible(hwnd) && GetWindowTextLengthW(hwnd) > 0) {
            GetWindowTextW(hwnd, title, TITLE_MAX_LENGTH);

            DWORD processId = 0;
            GetWindowThreadProcessId(hwnd, &processId);

            windowList->push_back({ hwnd, title, processId });
        }
        return TRUE;
        }, (LPARAM)&childWindows);
}

// Hook callbacks
LRESULT CALLBACK MessageProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        MSG* msg = (MSG*)lParam;

        // Check if this is a window size-related message for a window we're tracking
        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto it = g_windowSizes.find(msg->hwnd);

        if (it != g_windowSizes.end() &&
            (msg->message == WM_SIZE || msg->message == WM_SIZING ||
                msg->message == WM_WINDOWPOSCHANGED || msg->message == WM_WINDOWPOSCHANGING ||
                msg->message == WM_GETMINMAXINFO || msg->message == WM_NCCALCSIZE)) {

            auto& sizeData = it->second;

            // Handle specific messages
            switch (msg->message) {
            case WM_GETMINMAXINFO: {
                MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(msg->lParam);
                info->ptMinTrackSize.x = sizeData.width;
                info->ptMinTrackSize.y = sizeData.height;
                info->ptMaxTrackSize.x = sizeData.width;
                info->ptMaxTrackSize.y = sizeData.height;
                return 0;
            }

            case WM_NCCALCSIZE:
                if (msg->wParam) {
                    NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
                    RECT& clientRect = params->rgrc[0];

                    if ((clientRect.right - clientRect.left) != sizeData.width ||
                        (clientRect.bottom - clientRect.top) != sizeData.height) {

                        clientRect.right = clientRect.left + sizeData.width;
                        clientRect.bottom = clientRect.top + sizeData.height;
                    }
                }
                break;

            case WM_WINDOWPOSCHANGING: {
                WINDOWPOS* pos = reinterpret_cast<WINDOWPOS*>(msg->lParam);
                if (!(pos->flags & SWP_NOSIZE)) {
                    pos->cx = sizeData.width;
                    pos->cy = sizeData.height;
                }
                break;
            }

            default: {
                // Other size-related messages
                RECT rect;
                if (GetWindowRect(msg->hwnd, &rect)) {
                    int currentWidth = rect.right - rect.left;
                    int currentHeight = rect.bottom - rect.top;

                    if (currentWidth != sizeData.width || currentHeight != sizeData.height) {
                        SetWindowPos(msg->hwnd, NULL, 0, 0, sizeData.width, sizeData.height,
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                    }
                }
                break;
            }
            }
        }
    }

    return CallNextHookEx(g_messageHook, nCode, wParam, lParam);
}

LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HCBT_CREATEWND || nCode == HCBT_ACTIVATE) {
        HWND hwnd = (HWND)wParam;

        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto it = g_windowSizes.find(hwnd);

        if (it != g_windowSizes.end()) {
            auto& sizeData = it->second;
            SetWindowPos(hwnd, NULL, 0, 0, sizeData.width, sizeData.height,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
    }

    return CallNextHookEx(g_cbtHook, nCode, wParam, lParam);
}

// Custom window procedure
LRESULT CALLBACK CustomWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto origProcIt = g_originalWndProcs.find(hwnd);

    if (origProcIt == g_originalWndProcs.end()) {
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    WNDPROC originalProc = origProcIt->second;

    switch (msg) {
    case WM_NCCALCSIZE: {
        if (wParam) {
            NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            RECT currentClient = params->rgrc[0];

            LRESULT result = CallWindowProc(originalProc, hwnd, msg, wParam, lParam);

            // Adjust client area to account for our custom title bar
            params->rgrc[0].top = currentClient.top + CUSTOM_TITLE_HEIGHT;

            auto sizeIt = g_windowSizes.find(hwnd);
            if (sizeIt != g_windowSizes.end()) {
                auto& sizeData = sizeIt->second;
                RECT& rc = params->rgrc[0];
                rc.right = rc.left + sizeData.width;
                rc.bottom = rc.top + sizeData.height - CUSTOM_TITLE_HEIGHT;
            }

            return result;
        }
        break;
    }

    case WM_NCPAINT: {
        LRESULT result = CallWindowProc(originalProc, hwnd, msg, wParam, lParam);

        DCWrapper hdc(hwnd, (HRGN)wParam);
        if (hdc) {
            DrawCustomTitleBar(hwnd, hdc);
        }

        return result;
    }

    case WM_NCLBUTTONDOWN: {
        POINT cursorPos;
        if (GetCursorPos(&cursorPos)) {
            RECT windowRect;
            if (GetWindowRect(hwnd, &windowRect)) {
                if (cursorPos.y >= windowRect.top && cursorPos.y <= windowRect.top + CUSTOM_TITLE_HEIGHT) {
                    g_isDragging = true;
                    g_dragStart = cursorPos;
                    SetCapture(hwnd);
                    return 0;
                }
            }
        }
        break;
    }

    case WM_MOUSEMOVE: {
        if (g_isDragging) {
            POINT cursorPos;
            if (GetCursorPos(&cursorPos)) {
                int deltaX = cursorPos.x - g_dragStart.x;
                int deltaY = cursorPos.y - g_dragStart.y;

                g_dragStart = cursorPos;

                RECT windowRect;
                if (GetWindowRect(hwnd, &windowRect)) {
                    SetWindowPos(hwnd, NULL,
                        windowRect.left + deltaX,
                        windowRect.top + deltaY,
                        0, 0, SWP_NOSIZE | SWP_NOZORDER);
                }
                return 0;
            }
        }
        break;
    }

    case WM_LBUTTONUP: {
        if (g_isDragging) {
            g_isDragging = false;
            ReleaseCapture();
            return 0;
        }
        break;
    }

    case WM_NCHITTEST: {
        LRESULT hit = CallWindowProc(originalProc, hwnd, msg, wParam, lParam);

        if (hit == HTCLIENT) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);

            if (pt.y >= 0 && pt.y <= CUSTOM_TITLE_HEIGHT) {
                return HTCAPTION;
            }
        }

        return hit;
    }
    }

    // Handle window size enforcement
    auto sizeIt = g_windowSizes.find(hwnd);
    if (sizeIt != g_windowSizes.end() &&
        (msg == WM_GETMINMAXINFO || msg == WM_SIZE || msg == WM_SIZING ||
            msg == WM_WINDOWPOSCHANGING || msg == WM_WINDOWPOSCHANGED)) {

        auto& sizeData = sizeIt->second;

        if (msg == WM_GETMINMAXINFO) {
            MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = sizeData.width;
            info->ptMinTrackSize.y = sizeData.height;
            info->ptMaxTrackSize.x = sizeData.width;
            info->ptMaxTrackSize.y = sizeData.height;
            return 0;
        }
        else if (msg == WM_WINDOWPOSCHANGING) {
            WINDOWPOS* pos = reinterpret_cast<WINDOWPOS*>(lParam);
            if (!(pos->flags & SWP_NOSIZE)) {
                pos->cx = sizeData.width;
                pos->cy = sizeData.height;
            }
        }
    }

    return CallWindowProc(originalProc, hwnd, msg, wParam, lParam);
}

// Window size enforcement thread
void ForceWindowSizeThread(HWND hwnd, int width, int height) {
    LONG style, exStyle, newStyle;

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_windowSizes[hwnd] = { width, height, true };

        // Get and modify styles
        style = GetWindowLong(hwnd, GWL_STYLE);
        exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);

        // Keep WS_CAPTION for title bar, remove resizing styles
        newStyle = style & ~(WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
        newStyle |= WS_CAPTION;
    }

    // Apply new styles
    SetWindowLong(hwnd, GWL_STYLE, newStyle);
    SetWindowLong(hwnd, GWL_EXSTYLE, exStyle);

    // Update window appearance
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    // Center window and set size
    CenterWindowOnScreen(hwnd, width, height);

    // Redraw window with new settings
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME | RDW_ALLCHILDREN);

    // Main enforcement loop
    bool keepForcing = true;
    while (keepForcing) {
        RECT rect;
        if (GetWindowRect(hwnd, &rect)) {
            int currentWidth = rect.right - rect.left;
            int currentHeight = rect.bottom - rect.top;

            if (currentWidth != width || currentHeight != height) {
                SetWindowLong(hwnd, GWL_STYLE, newStyle);
                SetWindowPos(hwnd, NULL, rect.left, rect.top, width, height,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(THREAD_REFRESH_MS));

        // Check if we should stop enforcing
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto it = g_windowSizes.find(hwnd);
            if (it == g_windowSizes.end() || !it->second.keepForcing) {
                keepForcing = false;
            }
        }
    }

    // Restore original styles on exit
    SetWindowLong(hwnd, GWL_STYLE, style);
    SetWindowLong(hwnd, GWL_EXSTYLE, exStyle);
}

// Main public API
bool ForceWindowSize(HWND hwnd, int width, int height) {
    if (!hwnd || !IsWindow(hwnd)) {
        using namespace MetroTheme;
        std::wcout << HAZARD_RED << L"[ ❌ ОШИБКА ]: Некорректный дескриптор окна!" << RESET << std::endl;
        return false;
    }

    // Set up hooks and window proc
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);

        // Store original window procedure
        WNDPROC oldWndProc = (WNDPROC)GetWindowLongPtr(hwnd, GWLP_WNDPROC);
        g_originalWndProcs[hwnd] = oldWndProc;

        // Set custom window procedure
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)CustomWindowProc);

        // Initialize hooks if not done yet
        if (!g_messageHook) {
            g_messageHook = SetWindowsHookEx(WH_GETMESSAGE, MessageProc, NULL, GetCurrentThreadId());
            if (!g_messageHook) {
                DebugLog(L"Не удалось установить хук WH_GETMESSAGE");
            }
        }

        if (!g_cbtHook) {
            g_cbtHook = SetWindowsHookEx(WH_CBT, CBTProc, NULL, GetCurrentThreadId());
            if (!g_cbtHook) {
                DebugLog(L"Не удалось установить хук WH_CBT");
            }
        }
    }

    // Start enforcement thread
    std::thread enforceThread(ForceWindowSizeThread, hwnd, width, height);
    enforceThread.detach();

    return true;
}

// Window enumeration
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) <= 0) {
        return TRUE;
    }

    wchar_t title[TITLE_MAX_LENGTH] = { 0 };
    GetWindowTextW(hwnd, title, TITLE_MAX_LENGTH);

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);

    auto windowList = reinterpret_cast<std::vector<WindowInfo>*>(lParam);
    windowList->push_back({ hwnd, title, processId });

    return TRUE;
}

std::vector<WindowInfo> EnumerateWindows() {
    std::vector<WindowInfo> windowList;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&windowList));
    return windowList;
}

HWND FindWindowByPartialTitle(const std::wstring& partialTitle) {
    for (const auto& window : EnumerateWindows()) {
        if (window.title.find(partialTitle) != std::wstring::npos) {
            return window.hwnd;
        }
    }
    return NULL;
}

std::vector<WindowInfo> FindWindowsForProcess(DWORD processId) {
    std::vector<WindowInfo> result;

    for (const auto& window : EnumerateWindows()) {
        if (window.processId == processId) {
            result.push_back(window);

            // Find child windows
            std::vector<WindowInfo> childWindows;
            FindChildWindows(window.hwnd, childWindows);

            for (const auto& childWindow : childWindows) {
                if (childWindow.processId == processId) {
                    result.push_back(childWindow);
                }
            }
        }
    }

    return result;
}

bool ForceWindowSizeForAllProcessWindows(DWORD processId, int width, int height) {
    bool result = false;

    for (const auto& window : FindWindowsForProcess(processId)) {
        if (ForceWindowSize(window.hwnd, width, height)) {
            result = true;
            DebugLog(L"Установлен размер для окна: " + window.title);
        }
    }

    return result;
}

// Clean up resources
void CleanupResources() {
    std::lock_guard<std::mutex> lock(g_stateMutex);

    // Stop all enforcement threads
    for (auto& pair : g_windowSizes) {
        pair.second.keepForcing = false;
    }

    // Remove hooks
    if (g_messageHook) {
        UnhookWindowsHookEx(g_messageHook);
        g_messageHook = NULL;
    }

    if (g_cbtHook) {
        UnhookWindowsHookEx(g_cbtHook);
        g_cbtHook = NULL;
    }

    // Wait for threads to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Restore original window procedures
    for (const auto& pair : g_originalWndProcs) {
        HWND hwnd = pair.first;
        WNDPROC proc = pair.second;

        if (IsWindow(hwnd)) {
            SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)proc);
        }
    }

    // Clear collections
    g_originalWndProcs.clear();
    g_windowSizes.clear();
}

// Metro 2033 Terminal UI Components
void PrintMetroBanner() {
    using namespace MetroTheme;
    std::wcout << DARK_STEEL << L"╔═════════════════════════════════════════════════════════════════════════════════════╗\n" << RESET;
    std::wcout << GREEN << BOLD;
    std::wcout << L"  █████╗  ██████╗ ██████╗ ██████╗     ██████╗ ███████╗███████╗██╗███████╗███████╗ \n";
    std::wcout << L" █╔══██╗██╔═══██╗╚════██╗╚════██╗    ██╔═══██╗██╔════╝██╔════╝██║╚══███╔╝██╔════╝ \n";
    std::wcout << L" ╚█████║██║   ██║ █████╔╝ █████╔╝    ██║   ██║███████╗█████╗  ██║  ███╔╝ █████╗   \n";
    std::wcout << L" █╔══██╗██║   ██║ ╚═══██╗ ╚═══██╗    ██║   ██║╚════██║██╔══╝  ██║ ███╔╝  ██╔══╝   \n";
    std::wcout << L" ╚█████╔╝╚██████╔╝██████╔╝██████╔╝    ╚██████╔╝███████╗███████╗██║███████╗███████╗\n";
    std::wcout << L"  ╚════╝  ╚═════╝ ╚═════╝ ╚═════╝      ╚═════╝ ╚══════╝╚══════╝╚═╝╚══════╝╚══════╝\n" << RESET;
    std::wcout << DARK_STEEL << L"╠═════════════════════════════════════════════════════════════════════════════════════╣\n" << RESET;
    std::wcout << AMBER << L"  [ ☢ МЕТРО-2033 :: FRW ТЕРМИНАЛ ПРИНУДИТЕЛЬНОГО ИЗМЕНЕНИЯ РАЗРЕШЕНИЯ ОКОН ☢ ]\n" << RESET;
    std::wcout << STEEL << L"  [ СИСТЕМА УПРАВЛЕНИЯ ОКОННЫМИ ПРОЦЕССАМИ БУНКЕРА | СЕКТОР-2033 | REVISION 4.5 ]\n" << RESET;
    std::wcout << DARK_STEEL << L"╚═════════════════════════════════════════════════════════════════════════════════════╝\n\n" << RESET;
}

// Main application
int main() {
    using namespace MetroTheme;
    try {
        // Increase process priority
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

        // Setup console for Unicode & ANSI color support
        SetupConsoleForCyrillic();

        // Print post-apocalyptic Metro 2033 Header
        PrintMetroBanner();

        // List all available windows in a structured box container
        std::wcout << AMBER << L"┌─── [ ☢ СПИСОК АКТИВНЫХ ОКОН В СИСТЕМЕ ☢ ] ───────────────────────────────────────────┐\n" << RESET;
        std::wcout << DARK_STEEL << L"│ " << AMBER << L"№   " << DARK_STEEL << L"│ "
                   << GREEN << L"НАЗВАНИЕ ОКНА                            " << DARK_STEEL << L"│ "
                   << CYAN_GLOW << L"ПРОЦЕСС (PID)                   " << DARK_STEEL << L"│\n" << RESET;
        std::wcout << DARK_STEEL << L"├───┼─────────────────────────────────────────┼─────────────────────────────────┤\n" << RESET;

        auto windows = EnumerateWindows();
        for (size_t i = 0; i < windows.size(); ++i) {
            std::wstring processName = GetProcessNameById(windows[i].processId);

            std::wstring titleDisplay = windows[i].title;
            if (titleDisplay.length() > 39) {
                titleDisplay = titleDisplay.substr(0, 36) + L"...";
            } else {
                titleDisplay.resize(39, L' ');
            }

            std::wstring procInfo = processName + L" (" + std::to_wstring(windows[i].processId) + L")";
            if (procInfo.length() > 31) {
                procInfo = procInfo.substr(0, 28) + L"...";
            } else {
                procInfo.resize(31, L' ');
            }

            wchar_t numStr[10];
            swprintf(numStr, 10, L"%02d", static_cast<int>(i + 1));

            std::wcout << DARK_STEEL << L"│ " << WARN_YELLOW << numStr << DARK_STEEL << L" │ "
                       << GREEN << titleDisplay << DARK_STEEL << L" │ "
                       << STEEL << procInfo << DARK_STEEL << L" │\n" << RESET;
        }

        std::wcout << DARK_STEEL << L"└───┴─────────────────────────────────────────┴─────────────────────────────────┘\n\n" << RESET;

        // Interactive Selection Menu
        std::wcout << AMBER << L"┌── [ ⚡ ВЫБОР ЦЕЛИ ⚡ ] ────────────────────────────────────────────────────────┐\n" << RESET;
        std::wcout << STEEL << L"│ Введите " << WARN_YELLOW << L"номер окна" << STEEL << L" из списка для прямого захвата,\n";
        std::wcout << STEEL << L"│ Введите " << WARN_YELLOW << L"0" << STEEL << L" для поиска окна по названию,\n";
        std::wcout << STEEL << L"│ Или " << WARN_YELLOW << L"-1" << STEEL << L" для захвата всех окон определенного процесса.\n";
        std::wcout << AMBER << L"└─► " << BRIGHT_GREEN << L"ВЫБОР: " << RESET;

        int windowIndex;
        std::wcin >> windowIndex;

        HWND targetWindow = NULL;
        DWORD targetProcessId = 0;
        bool forceAllWindows = false;

        if (windowIndex == -1) {
            // Process selection mode
            std::map<DWORD, std::wstring> processes;

            for (const auto& window : windows) {
                if (processes.find(window.processId) == processes.end()) {
                    processes[window.processId] = GetProcessNameById(window.processId);
                }
            }

            std::wcout << AMBER << L"\n┌─── [ ☣ СПИСОК АКТИВНЫХ ПРОЦЕССОВ ☣ ] ───────────────────────────────────────────────┐\n" << RESET;
            std::wcout << DARK_STEEL << L"│ " << AMBER << L"№   " << DARK_STEEL << L"│ "
                       << GREEN << L"НАЗВАНИЕ ПРОЦЕССА               " << DARK_STEEL << L"│ "
                       << CYAN_GLOW << L"PID             " << DARK_STEEL << L"│\n" << RESET;
            std::wcout << DARK_STEEL << L"├───┼─────────────────────────────────┼─────────────────┤\n" << RESET;

            int i = 1;
            for (const auto& proc : processes) {
                std::wstring procNameDisp = proc.second;
                if (procNameDisp.length() > 31) {
                    procNameDisp = procNameDisp.substr(0, 28) + L"...";
                } else {
                    procNameDisp.resize(31, L' ');
                }

                std::wstring pidDisp = std::to_wstring(proc.first);
                pidDisp.resize(15, L' ');

                wchar_t pNumStr[10];
                swprintf(pNumStr, 10, L"%02d", i);

                std::wcout << DARK_STEEL << L"│ " << WARN_YELLOW << pNumStr << DARK_STEEL << L" │ "
                           << GREEN << procNameDisp << DARK_STEEL << L" │ "
                           << CYAN_GLOW << pidDisp << DARK_STEEL << L" │\n" << RESET;
                i++;
            }
            std::wcout << DARK_STEEL << L"└───┴─────────────────────────────────┴─────────────────┘\n\n" << RESET;

            std::wcout << AMBER << L"┌───► " << BRIGHT_GREEN << L"Введите номер процесса: " << RESET;
            int processIndex;
            std::wcin >> processIndex;

            if (processIndex >= 1 && processIndex <= static_cast<int>(processes.size())) {
                auto it = processes.begin();
                std::advance(it, processIndex - 1);
                targetProcessId = it->first;
                forceAllWindows = true;

                std::wcout << GREEN << L"[ ✔ ] Захвачен процесс: " << WARN_YELLOW << it->second
                           << GREEN << L" (PID: " << CYAN_GLOW << targetProcessId << GREEN << L")\n" << RESET;
            }
            else {
                throw std::runtime_error("Некорректный номер процесса");
            }
        }
        else if (windowIndex == 0) {
            // Search by partial window title
            std::wstring searchTitle;
            std::wcin.ignore(); // Clear input buffer
            std::wcout << AMBER << L"┌───► " << BRIGHT_GREEN << L"Введите фрагмент заголовка окна: " << RESET;
            std::getline(std::wcin, searchTitle);

            targetWindow = FindWindowByPartialTitle(searchTitle);
            if (!targetWindow) {
                throw std::runtime_error("Окно с заданным заголовком не найдено");
            }

            GetWindowThreadProcessId(targetWindow, &targetProcessId);
            std::wcout << GREEN << L"[ ✔ ] Окно успешно найдено в реестре процессов.\n" << RESET;
        }
        else if (windowIndex >= 1 && windowIndex <= static_cast<int>(windows.size())) {
            targetWindow = windows[windowIndex - 1].hwnd;
            targetProcessId = windows[windowIndex - 1].processId;
        }
        else {
            throw std::runtime_error("Некорректный номер окна");
        }

        // Prompt for desired window dimensions
        int newWidth, newHeight;
        std::wcout << AMBER << L"\n┌── [ 📐 НАСТРОЙКА РАЗРЕШЕНИЯ (2033 ENGINE) ] ───────────────────────────────────┐\n" << RESET;
        std::wcout << STEEL << L"│ Укажите целевые параметры геометрии окна в пикселях.\n";
        std::wcout << AMBER << L"├─► " << GREEN << L"Новая ШИРИНА  (px): " << RESET;
        std::wcin >> newWidth;
        std::wcout << AMBER << L"├─► " << GREEN << L"Новая ВЫСОТА (px): " << RESET;
        std::wcin >> newHeight;
        std::wcout << AMBER << L"└───────────────────────────────────────────────────────────────────────────────┘\n" << RESET;

        if (newWidth <= 0 || newHeight <= 0) {
            throw std::runtime_error("Недопустимый размер окна");
        }

        bool success = false;

        if (forceAllWindows) {
            success = ForceWindowSizeForAllProcessWindows(targetProcessId, newWidth, newHeight);
            if (success) {
                std::wcout << GREEN << L"\n[ ✔ ] Разрешение всех окон процесса успешно изменено на "
                           << WARN_YELLOW << newWidth << L"x" << newHeight << GREEN << L" px\n" << RESET;
            }
            else {
                std::wcout << HAZARD_RED << L"\n[ ❌ ] Не удалось изменить размер окон процесса!\n" << RESET;
            }
        }
        else {
            RECT windowRect;
            GetWindowRect(targetWindow, &windowRect);
            int currentWidth = windowRect.right - windowRect.left;
            int currentHeight = windowRect.bottom - windowRect.top;

            std::wcout << STEEL << L"\n[ ℹ ] Исходный размер окна: " << WARN_YELLOW
                       << currentWidth << L"x" << currentHeight << STEEL << L" px\n" << RESET;

            if (ForceWindowSize(targetWindow, newWidth, newHeight)) {
                std::wcout << GREEN << L"[ ✔ ] Разрешение окна принудительно установлено на "
                           << WARN_YELLOW << newWidth << L"x" << newHeight << GREEN << L" px\n" << RESET;
                success = true;
            }
            else {
                std::wcout << HAZARD_RED << L"[ ❌ ] Не удалось изменить размер окна!\n" << RESET;
            }
        }

        if (success) {
            std::wcout << HAZARD_RED << L"\n╔═══════════════════════════════════════════════════════════════════════════════╗\n";
            std::wcout << L"║ ☣  СИСТЕМА СТАБИЛИЗАЦИИ РАЗРЕШЕНИЯ «МЕТРО-2033» АКТИВИРОВАНА  ☣               ║\n";
            std::wcout << L"╠═══════════════════════════════════════════════════════════════════════════════╣\n" << RESET;
            std::wcout << STEEL << L"║  Статус    : " << BRIGHT_GREEN << L"[ ПОТОК ПРИНУДЕНИЯ АКТИВЕН ]" << STEEL << L"\n";
            std::wcout << STEEL << L"║  Разрешение: " << WARN_YELLOW << newWidth << L" x " << newHeight << STEEL << L" px\n";
            std::wcout << STEEL << L"║  Интервал  : " << AMBER << THREAD_REFRESH_MS << L" ms\n";
            std::wcout << HAZARD_RED << L"╠═══════════════════════════════════════════════════════════════════════════════╣\n";
            std::wcout << L"║ 🛑 [ СТАНЦИЯ В БЕЗОПАСНОСТИ ] Для восстановления и выхода нажмите ENTER...    ║\n";
            std::wcout << L"╚═══════════════════════════════════════════════════════════════════════════════╝\n\n" << RESET;

            std::wcin.ignore();
            std::wcin.get();

            CleanupResources();
        }
        else {
            std::wcout << AMBER << L"\n[ 🛑 ] Нажмите ENTER для выхода из терминала..." << RESET << std::endl;
            std::wcin.ignore();
            std::wcin.get();
        }
    }
    catch (const std::exception& ex) {
        std::wcerr << HAZARD_RED << L"\n[ ❌ КРИТИЧЕСКИЙ СБОЙ СИСТЕМЫ 2033 ]: " << ex.what() << RESET << std::endl;
        std::wcout << AMBER << L"\n[ 🛑 ] Нажмите ENTER для выхода из терминала..." << RESET << std::endl;
        std::wcin.ignore();
        std::wcin.get();
    }

    return 0;
}
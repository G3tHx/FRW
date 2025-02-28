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
constexpr COLORREF TITLE_BAR_COLOR = RGB(50, 50, 50);
constexpr COLORREF TITLE_TEXT_COLOR = RGB(255, 255, 255);

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
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
}

void DebugLog(const std::wstring& message) {
    std::wcout << L"[DEBUG] " << message << std::endl;
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

    // Create rectangle for our title bar
    RECT titleRect = { 0, 0, windowRect.right - windowRect.left, CUSTOM_TITLE_HEIGHT };

    // Draw title bar background
    BrushWrapper titleBrush(TITLE_BAR_COLOR);
    FillRect(hdc, &titleRect, titleBrush);

    // Draw window title
    wchar_t title[TITLE_MAX_LENGTH];
    GetWindowTextW(hwnd, title, TITLE_MAX_LENGTH);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, TITLE_TEXT_COLOR);

    titleRect.left += 10; // Left padding for text
    DrawTextW(hdc, title, -1, &titleRect, DT_SINGLELINE | DT_VCENTER);
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
    // Use a local copy of data to minimize mutex contention
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
        std::wcout << L"Некорректный дескриптор окна!" << std::endl;
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

// Main application
int main() {
    try {
        // Increase process priority
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

        // Setup console for Unicode output
        SetupConsoleForCyrillic();

        std::wcout << L"Программа для принудительного изменения размера окна" << std::endl;
        std::wcout << L"-----------------------------------------------------" << std::endl;

        // List all available windows
        std::wcout << L"Список доступных окон:" << std::endl;
        std::wcout << L"ID\tНазвание окна\tПроцесс" << std::endl;
        std::wcout << L"-----------------------------------------------------" << std::endl;

        auto windows = EnumerateWindows();
        for (size_t i = 0; i < windows.size(); ++i) {
            std::wstring processName = GetProcessNameById(windows[i].processId);
            std::wcout << i + 1 << L"\t"
                << windows[i].title.substr(0, 40)
                << (windows[i].title.length() > 40 ? L"..." : L"") << L"\t"
                << processName << L" (" << windows[i].processId << L")" << std::endl;
        }

        // Window or process selection
        int windowIndex;
        std::wcout << L"\nВведите номер окна, 0 для поиска по названию, или -1 для выбора процесса: ";
        std::wcin >> windowIndex;

        HWND targetWindow = NULL;
        DWORD targetProcessId = 0;
        bool forceAllWindows = false;

        if (windowIndex == -1) {
            // Process selection
            std::map<DWORD, std::wstring> processes;

            for (const auto& window : windows) {
                if (processes.find(window.processId) == processes.end()) {
                    processes[window.processId] = GetProcessNameById(window.processId);
                }
            }

            std::wcout << L"Список доступных процессов:" << std::endl;
            std::wcout << L"ID\tНазвание процесса\tPID" << std::endl;
            std::wcout << L"-----------------------------------------------------" << std::endl;

            int i = 1;
            for (const auto& proc : processes) {
                std::wcout << i << L"\t" << proc.second << L"\t" << proc.first << std::endl;
                i++;
            }

            int processIndex;
            std::wcout << L"\nВведите номер процесса: ";
            std::wcin >> processIndex;

            if (processIndex >= 1 && processIndex <= static_cast<int>(processes.size())) {
                auto it = processes.begin();
                std::advance(it, processIndex - 1);
                targetProcessId = it->first;
                forceAllWindows = true;

                std::wcout << L"Выбран процесс: " << it->second << L" (PID: " << targetProcessId << L")" << std::endl;
            }
            else {
                throw std::runtime_error("Некорректный номер процесса");
            }
        }
        else if (windowIndex == 0) {
            // Search by title
            std::wstring searchTitle;
            std::wcin.ignore(); // Clear input buffer
            std::wcout << L"Введите часть заголовка окна для поиска: ";
            std::getline(std::wcin, searchTitle);

            targetWindow = FindWindowByPartialTitle(searchTitle);
            if (!targetWindow) {
                throw std::runtime_error("Окно с заданным заголовком не найдено");
            }

            GetWindowThreadProcessId(targetWindow, &targetProcessId);
        }
        else if (windowIndex >= 1 && windowIndex <= static_cast<int>(windows.size())) {
            targetWindow = windows[windowIndex - 1].hwnd;
            targetProcessId = windows[windowIndex - 1].processId;
        }
        else {
            throw std::runtime_error("Некорректный номер окна");
        }

        // Get desired window size
        int newWidth, newHeight;
        std::wcout << L"Введите новую ширину окна: ";
        std::wcin >> newWidth;
        std::wcout << L"Введите новую высоту окна: ";
        std::wcin >> newHeight;

        if (newWidth <= 0 || newHeight <= 0) {
            throw std::runtime_error("Недопустимый размер окна");
        }

        bool success = false;

        if (forceAllWindows) {
            success = ForceWindowSizeForAllProcessWindows(targetProcessId, newWidth, newHeight);
            if (success) {
                std::wcout << L"Размер всех окон процесса изменен на " << newWidth << L"x" << newHeight << std::endl;
            }
            else {
                std::wcout << L"Не удалось изменить размер окон процесса" << std::endl;
            }
        }
        else {
            RECT windowRect;
            GetWindowRect(targetWindow, &windowRect);
            int currentWidth = windowRect.right - windowRect.left;
            int currentHeight = windowRect.bottom - windowRect.top;

            std::wcout << L"Текущий размер окна: " << currentWidth
                << L"x" << currentHeight << std::endl;

            if (ForceWindowSize(targetWindow, newWidth, newHeight)) {
                std::wcout << L"Размер окна принудительно установлен на " << newWidth
                    << L"x" << newHeight << std::endl;
                std::wcout << L"Контролирующий поток активирован для поддержания размера" << std::endl;
                success = true;
            }
            else {
                std::wcout << L"Не удалось изменить размер окна" << std::endl;
            }
        }

        if (success) {
            std::wcout << L"\nПрограмма активно поддерживает указанный размер окна." << std::endl;
            std::wcout << L"Для выхода и восстановления исходного поведения окна нажмите любую клавишу..." << std::endl;
            std::wcin.ignore();
            std::wcin.get();

            CleanupResources();
        }
        else {
            std::wcout << L"\nНажмите любую клавишу для выхода..." << std::endl;
            std::wcin.ignore();
            std::wcin.get();
        }
    }
    catch (const std::exception& ex) {
        std::wcerr << L"Ошибка: " << ex.what() << std::endl;
        std::wcout << L"\nНажмите любую клавишу для выхода..." << std::endl;
        std::wcin.ignore();
        std::wcin.get();
    }

    return 0;
}
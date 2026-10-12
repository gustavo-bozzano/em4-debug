#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "em4_plugin.h"

static const EM4HostAPI *host;
static HWND altered_window;
static LONG original_style;
static LONG original_exstyle;
static WINDOWPLACEMENT original_placement;
static RECT original_rect;
static BOOL changed;

static void recover_minimized_window(HWND hwnd) {
    WINDOWPLACEMENT placement;
    if (!hwnd || !IsIconic(hwnd)) return;
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd, &placement)) {
        host->log("borderless", "Could not read minimized window placement");
        return;
    }
    placement.showCmd = SW_SHOWNORMAL;
    if (!SetWindowPlacement(hwnd, &placement)) {
        host->log("borderless", "Could not restore minimized window placement");
        return;
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    host->log("borderless", IsIconic(hwnd) ?
        "Window recovery failed" : "Minimized game window recovered");
}

static void log_window_state(HWND hwnd) {
    RECT rect;
    char message[160];
    if (!GetWindowRect(hwnd, &rect)) return;
    snprintf(message, sizeof(message), "Window style=0x%08lx rect=%ld,%ld %ldx%ld",
        (unsigned long)GetWindowLongA(hwnd, GWL_STYLE),
        (long)rect.left, (long)rect.top,
        (long)(rect.right - rect.left), (long)(rect.bottom - rect.top));
    host->log("borderless", message);
}

static void restore_window(void) {
    HWND z_order;
    if (!changed || !IsWindow(altered_window)) return;
    SetWindowLongA(altered_window, GWL_STYLE, original_style);
    SetWindowLongA(altered_window, GWL_EXSTYLE, original_exstyle);
    SetWindowPlacement(altered_window, &original_placement);
    z_order = (original_exstyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(altered_window, z_order, original_rect.left, original_rect.top,
        original_rect.right - original_rect.left,
        original_rect.bottom - original_rect.top,
        SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    if (!(original_style & WS_VISIBLE)) ShowWindow(altered_window, SW_HIDE);
    changed = FALSE;
    host->log("borderless", "Original window mode restored");
}

static void toggle_window(HWND hwnd) {
    MONITORINFO monitor_info;
    HMONITOR monitor;
    RECT rect;
    LONG style = GetWindowLongA(hwnd, GWL_STYLE);

    original_style = style;
    original_exstyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
    original_placement.length = sizeof(original_placement);
    if (!GetWindowPlacement(hwnd, &original_placement) ||
        !GetWindowRect(hwnd, &original_rect)) return;

    monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    monitor_info.cbSize = sizeof(monitor_info);
    if (!GetMonitorInfoA(monitor, &monitor_info)) return;
    rect = monitor_info.rcMonitor;

    altered_window = hwnd;
    changed = TRUE;
    if (style & (WS_CAPTION | WS_THICKFRAME)) {
        SetWindowLongA(hwnd, GWL_STYLE,
            (style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_POPUP);
        SetWindowLongA(hwnd, GWL_EXSTYLE,
            original_exstyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
        if (!SetWindowPos(hwnd, HWND_TOP, rect.left, rect.top,
            rect.right - rect.left, rect.bottom - rect.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE)) {
            host->log("borderless", "SetWindowPos failed");
            restore_window();
            return;
        }
        host->log("borderless", "Borderless mode enabled");
    } else {
        int width = (rect.right - rect.left) * 4 / 5;
        int height = (rect.bottom - rect.top) * 4 / 5;
        int left = rect.left + (rect.right - rect.left - width) / 2;
        int top = rect.top + (rect.bottom - rect.top - height) / 2;
        SetWindowLongA(hwnd, GWL_STYLE,
            (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowLongA(hwnd, GWL_EXSTYLE, original_exstyle & ~WS_EX_TOPMOST);
        if (!SetWindowPos(hwnd, HWND_NOTOPMOST, left, top, width, height,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE)) {
            host->log("borderless", "SetWindowPos failed");
            restore_window();
            return;
        }
        host->log("borderless", "Windowed mode enabled");
    }
    log_window_state(hwnd);
}

static DWORD WINAPI hotkey_worker(LPVOID unused) {
    BOOL last_down = FALSE;
    BOOL last_recover_down = FALSE;
    (void)unused;
    for (;;) {
        HWND hwnd = host->get_game_window();
        DWORD pid = 0;
        BOOL down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        BOOL recover_down = down &&
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000);
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (changed && hwnd != altered_window) changed = FALSE;
        if (recover_down && !last_recover_down && hwnd)
            recover_minimized_window(hwnd);
        else if (down && !last_down && !recover_down &&
                 pid == GetCurrentProcessId() && hwnd) {
            if (changed) restore_window();
            else toggle_window(hwnd);
        }
        last_down = down;
        last_recover_down = recover_down;
        Sleep(35);
    }
}

__declspec(dllexport) BOOL WINAPI EM4PluginInit(const EM4HostAPI *api) {
    HANDLE worker;
    if (!api || api->size < sizeof(EM4HostAPI) ||
        api->version != EM4_PLUGIN_API_VERSION || !api->get_game_window || !api->log)
        return FALSE;
    host = api;
    worker = CreateThread(NULL, 0, hotkey_worker, NULL, 0, NULL);
    if (!worker) return FALSE;
    CloseHandle(worker);
    host->log("borderless", "F11 toggles window mode; Ctrl+Shift+F11 restores a minimized game");
    return TRUE;
}

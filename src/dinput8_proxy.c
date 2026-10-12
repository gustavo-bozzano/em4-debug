#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <string.h>
#include "em4_plugin.h"

static volatile LONG worker_started;
static char root_dir[MAX_PATH];
static char log_path[MAX_PATH];
static EM4HostAPI host_api;

static void WINAPI host_log(const char *plugin, const char *message) {
    HANDLE file;
    DWORD written;
    SYSTEMTIME now;
    char line[1024];
    int length;

    if (!log_path[0]) return;
    GetLocalTime(&now);
    length = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u [%s] %s\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        plugin ? plugin : "host", message ? message : "");
    if (length <= 0) return;
    if (length >= (int)sizeof(line)) length = (int)sizeof(line) - 1;
    file = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, line, (DWORD)length, &written, NULL);
    CloseHandle(file);
}

static BOOL CALLBACK find_window(HWND hwnd, LPARAM result) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    *(HWND *)result = hwnd;
    return FALSE;
}

static HWND WINAPI host_get_game_window(void) {
    HWND found = NULL;
    EnumWindows(find_window, (LPARAM)&found);
    return found;
}

static DWORD WINAPI plugin_worker(LPVOID unused) {
    WIN32_FIND_DATAA entry;
    HANDLE search;
    char pattern[MAX_PATH];
    char path[MAX_PATH];
    (void)unused;

    host_api.size = sizeof(host_api);
    host_api.version = EM4_PLUGIN_API_VERSION;
    host_api.get_game_window = host_get_game_window;
    host_api.log = host_log;

    snprintf(pattern, sizeof(pattern), "%sEM4Plugins\\*.dll", root_dir);
    search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) {
        host_log("host", "EM4Plugins directory is empty or missing");
        return 0;
    }
    do {
        HMODULE plugin;
        EM4PluginInitFn init;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (snprintf(path, sizeof(path), "%sEM4Plugins\\%s", root_dir,
                entry.cFileName) >= (int)sizeof(path)) continue;
        plugin = LoadLibraryA(path);
        if (!plugin) {
            host_log("host", "Could not load plugin DLL");
            continue;
        }
        init = (EM4PluginInitFn)GetProcAddress(plugin, "EM4PluginInit");
        if (!init || !init(&host_api)) {
            host_log("host", "Plugin has no compatible EM4PluginInit or initialization failed");
            FreeLibrary(plugin);
            continue;
        }
        host_log(entry.cFileName, "loaded");
        /* Keep the plugin loaded until the game process exits. */
    } while (FindNextFileA(search, &entry));
    FindClose(search);
    return 0;
}

typedef HRESULT (WINAPI *RealDirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid,
                                   LPVOID *out, LPUNKNOWN outer) {
    static RealDirectInput8Create real_function;
    static HMODULE real_module;
    char system_dir[MAX_PATH];
    char real_path[MAX_PATH];
    HANDLE worker;
    HRESULT result;

    if (!real_function) {
        UINT size = GetSystemDirectoryA(system_dir, MAX_PATH);
        if (!size || size >= MAX_PATH ||
            snprintf(real_path, sizeof(real_path), "%s\\dinput8.dll", system_dir) >= (int)sizeof(real_path))
            return E_FAIL;
        real_module = LoadLibraryA(real_path);
        if (!real_module) return E_FAIL;
        real_function = (RealDirectInput8Create)GetProcAddress(real_module, "DirectInput8Create");
        if (!real_function) return E_FAIL;
    }

    result = real_function(instance, version, iid, out, outer);
    if (SUCCEEDED(result) && InterlockedCompareExchange(&worker_started, 1, 0) == 0) {
        worker = CreateThread(NULL, 0, plugin_worker, NULL, 0, NULL);
        if (worker) CloseHandle(worker);
        else host_log("host", "Could not start plugin loader thread");
    }
    return result;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        size_t length;
        DisableThreadLibraryCalls(module);
        {
            DWORD copied = GetModuleFileNameA(module, root_dir, MAX_PATH);
            if (!copied || copied >= MAX_PATH) return FALSE;
        }
        length = strlen(root_dir);
        while (length && root_dir[length - 1] != '\\') --length;
        root_dir[length] = '\0';
        snprintf(log_path, sizeof(log_path), "%sEM4Plugins\\em4_plugins.log", root_dir);
    }
    return TRUE;
}

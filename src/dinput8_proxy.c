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
static void WINAPI host_log(const char *plugin, const char *message);
static volatile LONG control_input_mask;
static volatile LONG control_input_ready;
static volatile LONG control_input_observed;
static void *keyboard_devices[8];
static unsigned keyboard_device_count;

typedef HRESULT (WINAPI *CreateDeviceFn)(void *, REFGUID, void **, LPUNKNOWN);
typedef HRESULT (WINAPI *GetDeviceStateFn)(void *, DWORD, LPVOID);
typedef HRESULT (WINAPI *GetDeviceDataFn)(void *, DWORD, LPDIDEVICEOBJECTDATA,
                                          LPDWORD, DWORD);
static CreateDeviceFn original_create_device;
static GetDeviceStateFn original_get_device_state;
static GetDeviceDataFn original_get_device_data;

static int same_guid(REFGUID a, const GUID *b) {
    return a && memcmp(a, b, sizeof(GUID)) == 0;
}

static int keyboard_device(void *device) {
    unsigned i;
    for (i = 0; i < keyboard_device_count; ++i)
        if (keyboard_devices[i] == device) return 1;
    return 0;
}

static int movement_scan_code(DWORD code) {
    return code == DIK_W || code == DIK_A || code == DIK_S || code == DIK_D;
}

static HRESULT WINAPI masked_get_device_state(void *device, DWORD size, LPVOID state) {
    HRESULT result = original_get_device_state(device, size, state);
    if (SUCCEEDED(result) && state && size >= 256 && keyboard_device(device) &&
        InterlockedCompareExchange(&control_input_mask, 0, 0)) {
        BYTE *keys = (BYTE *)state;
        if ((keys[DIK_W] || keys[DIK_A] || keys[DIK_S] || keys[DIK_D]) &&
            InterlockedCompareExchange(&control_input_observed, 1, 0) == 0)
            host_log("host", "WASD keyboard state masked for camera input");
        keys[DIK_W] = keys[DIK_A] = keys[DIK_S] = keys[DIK_D] = 0;
    }
    return result;
}

static HRESULT WINAPI masked_get_device_data(void *device, DWORD size,
                                               LPDIDEVICEOBJECTDATA data,
                                               LPDWORD count, DWORD flags) {
    HRESULT result = original_get_device_data(device, size, data, count, flags);
    DWORD i;
    if (SUCCEEDED(result) && data && count && size >= sizeof(*data) &&
        keyboard_device(device) &&
        InterlockedCompareExchange(&control_input_mask, 0, 0)) {
        for (i = 0; i < *count; ++i)
            if (movement_scan_code(data[i].dwOfs)) {
                if (data[i].dwData &&
                    InterlockedCompareExchange(&control_input_observed, 1, 0) == 0)
                    host_log("host", "Buffered WASD input masked for camera input");
                data[i].dwData = 0;
            }
    }
    return result;
}

static int replace_vtable_slot(void **slot, void *replacement, void **original) {
    DWORD previous, ignored;
    if (*slot == replacement) return 1;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE,
                        &previous)) return 0;
    *original = *slot;
    InterlockedExchangePointer((PVOID volatile *)slot, replacement);
    VirtualProtect(slot, sizeof(*slot), previous, &ignored);
    return 1;
}

static HRESULT WINAPI hooked_create_device(void *input, REFGUID guid,
                                            void **out, LPUNKNOWN outer) {
    /* GUID_SysKeyboard is 6F1D2B61-D5A0-11CF-BFC7-444553540000. */
    static const GUID keyboard_guids[] = {
        {0x6f1d2b61, 0xd5a0, 0x11cf,
            {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}},
        {0x6f1d2b82, 0xd5a0, 0x11cf,
            {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}},
        {0x6f1d2b83, 0xd5a0, 0x11cf,
            {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}}
    };
    HRESULT result = original_create_device(input, guid, out, outer);
    void **vtable;
    if (FAILED(result) || !out || !*out ||
        !(same_guid(guid, &keyboard_guids[0]) ||
          same_guid(guid, &keyboard_guids[1]) ||
          same_guid(guid, &keyboard_guids[2])))
        return result;
    if (keyboard_device_count < sizeof(keyboard_devices) / sizeof(keyboard_devices[0]))
        keyboard_devices[keyboard_device_count++] = *out;
    vtable = *(void ***)*out;
    if (!original_get_device_state &&
        !replace_vtable_slot(&vtable[9], masked_get_device_state,
                             (void **)&original_get_device_state))
        host_log("host", "Could not filter keyboard state");
    if (!original_get_device_data &&
        !replace_vtable_slot(&vtable[10], masked_get_device_data,
                             (void **)&original_get_device_data))
        host_log("host", "Could not filter buffered keyboard data");
    if (original_get_device_state) {
        InterlockedExchange(&control_input_ready, 1);
        host_log("host", "DirectInput keyboard filter ready");
    }
    return result;
}

__declspec(dllexport) void WINAPI EM4SetDirectControlInput(BOOL enabled) {
    InterlockedExchange(&control_input_mask, enabled ? 1 : 0);
}

__declspec(dllexport) BOOL WINAPI EM4DirectControlInputReady(void) {
    return InterlockedCompareExchange(&control_input_ready, 0, 0) != 0;
}

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
    if (SUCCEEDED(result) && out && *out && !original_create_device) {
        void **vtable = *(void ***)*out;
        if (!replace_vtable_slot(&vtable[3], hooked_create_device,
                                 (void **)&original_create_device))
            host_log("host", "Could not intercept DirectInput keyboard creation");
    }
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

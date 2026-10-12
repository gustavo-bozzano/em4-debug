#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gl/gl.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include "em4_plugin.h"

typedef BOOL (WINAPI *SwapBuffersFn)(HDC);

typedef struct DelayImportDescriptor {
    DWORD attributes;
    DWORD name;
    DWORD module_handle;
    DWORD iat;
    DWORD int_table;
    DWORD bound_iat;
    DWORD unload_iat;
    DWORD timestamp;
} DelayImportDescriptor;

static const EM4HostAPI *host;
static SwapBuffersFn real_swap;
static volatile LONG visible = 1;
static volatile LONG first_frame_logged;
static HGLRC font_context;
static GLuint font_base;
static LARGE_INTEGER frequency;
static LARGE_INTEGER last_fps_tick;
static unsigned int frame_count;
static double fps;
static SIZE_T memory_mb;

static void draw_text(float x, float y, const char *value) {
    glRasterPos2f(x, y);
    glListBase(font_base - 32);
    glCallLists((GLsizei)strlen(value), GL_UNSIGNED_BYTE, value);
}

static BOOL prepare_font(HDC dc) {
    HGLRC context = wglGetCurrentContext();
    HFONT font;
    HGDIOBJ previous;
    if (!context) return FALSE;
    if (context == font_context && font_base) return TRUE;

    font_context = context;
    font_base = glGenLists(96);
    if (!font_base) return FALSE;
    font = CreateFontA(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    if (!font) return FALSE;
    previous = SelectObject(dc, font);
    if (!wglUseFontBitmapsA(dc, 32, 96, font_base)) {
        SelectObject(dc, previous);
        DeleteObject(font);
        return FALSE;
    }
    SelectObject(dc, previous);
    DeleteObject(font);
    return TRUE;
}

static void update_metrics(void) {
    LARGE_INTEGER now;
    PROCESS_MEMORY_COUNTERS counters;
    ++frame_count;
    QueryPerformanceCounter(&now);
    if (!last_fps_tick.QuadPart) {
        last_fps_tick = now;
        return;
    }
    if (now.QuadPart - last_fps_tick.QuadPart >= frequency.QuadPart) {
        fps = (double)frame_count * (double)frequency.QuadPart /
            (double)(now.QuadPart - last_fps_tick.QuadPart);
        frame_count = 0;
        last_fps_tick = now;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
            memory_mb = counters.WorkingSetSize / (1024 * 1024);
    }
}

static void draw_overlay(HDC dc) {
    GLint viewport[4];
    GLint previous_matrix;
    HWND window;
    LONG style;
    char line[120];

    if (!prepare_font(dc)) return;
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] < 200 || viewport[3] < 100) return;
    glGetIntegerv(GL_MATRIX_MODE, &previous_matrix);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, viewport[2], viewport[3], 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.0f, 0.0f, 0.0f, 0.72f);
    glBegin(GL_QUADS);
    glVertex2f(8, 8);
    glVertex2f(246, 8);
    glVertex2f(246, 86);
    glVertex2f(8, 86);
    glEnd();

    glColor3f(0.4f, 1.0f, 0.5f);
    draw_text(16, 25, "EM4 DEBUG  [F10]");
    glColor3f(1.0f, 1.0f, 1.0f);
    snprintf(line, sizeof(line), "FPS: %.1f   RAM: %lu MB", fps, (unsigned long)memory_mb);
    draw_text(16, 45, line);
    window = host->get_game_window();
    style = window ? GetWindowLongA(window, GWL_STYLE) : 0;
    snprintf(line, sizeof(line), "%dx%d  %s", viewport[2], viewport[3],
        (style & (WS_CAPTION | WS_THICKFRAME)) ? "WINDOW" : "FULLSCREEN");
    draw_text(16, 65, line);

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(previous_matrix);
    glPopAttrib();
}

static BOOL WINAPI hooked_swap(HDC dc) {
    update_metrics();
    if (InterlockedCompareExchange(&first_frame_logged, 1, 0) == 0)
        host->log("debug", "First SwapBuffers frame observed");
    if (visible) draw_overlay(dc);
    return real_swap(dc);
}

static BOOL install_swap_hook(void) {
    HMODULE module = GetModuleHandleA("vBase71.dll");
    HMODULE gdi;
    BYTE *base;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_DATA_DIRECTORY directory;
    DelayImportDescriptor *descriptor;
    unsigned int descriptor_count;
    unsigned int d;

    if (!module) return FALSE;
    base = (BYTE *)module;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;
    directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!directory.VirtualAddress || directory.Size < sizeof(DelayImportDescriptor)) return FALSE;
    descriptor = (DelayImportDescriptor *)(base + directory.VirtualAddress);
    descriptor_count = directory.Size / sizeof(DelayImportDescriptor);

    gdi = GetModuleHandleA("gdi32.dll");
    if (!gdi) gdi = LoadLibraryA("gdi32.dll");
    if (!gdi) return FALSE;
    real_swap = (SwapBuffersFn)GetProcAddress(gdi, "SwapBuffers");
    if (!real_swap) return FALSE;

    for (d = 0; d < descriptor_count; ++d) {
        IMAGE_THUNK_DATA32 *names;
        IMAGE_THUNK_DATA32 *slots;
        unsigned int i;
        if (!descriptor[d].name) break;
        if (lstrcmpiA((char *)(base + descriptor[d].name), "GDI32.dll") != 0) continue;
        if (!(descriptor[d].attributes & 1) || !descriptor[d].int_table || !descriptor[d].iat)
            return FALSE;
        names = (IMAGE_THUNK_DATA32 *)(base + descriptor[d].int_table);
        slots = (IMAGE_THUNK_DATA32 *)(base + descriptor[d].iat);
        for (i = 0; i < 512 && names[i].u1.AddressOfData; ++i) {
            IMAGE_IMPORT_BY_NAME *name;
            DWORD previous_protection;
            DWORD unused_protection;
            if (IMAGE_SNAP_BY_ORDINAL32(names[i].u1.Ordinal)) continue;
            name = (IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData);
            if (lstrcmpA((char *)name->Name, "SwapBuffers") != 0) continue;
            if (!VirtualProtect(&slots[i].u1.Function, sizeof(DWORD), PAGE_READWRITE,
                    &previous_protection)) return FALSE;
            InterlockedExchange((volatile LONG *)&slots[i].u1.Function, (LONG)hooked_swap);
            VirtualProtect(&slots[i].u1.Function, sizeof(DWORD), previous_protection,
                &unused_protection);
            return TRUE;
        }
    }
    return FALSE;
}

static DWORD WINAPI debug_worker(LPVOID unused) {
    BOOL last_down = FALSE;
    int attempts;
    (void)unused;
    for (attempts = 0; attempts < 300 && !install_swap_hook(); ++attempts)
        Sleep(100);
    if (attempts == 300) {
        host->log("debug", "Could not find vBase71.dll SwapBuffers delay import");
        return 0;
    }
    host->log("debug", "SwapBuffers hook installed; F10 toggles overlay");
    for (;;) {
        DWORD pid = 0;
        BOOL down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (down && !last_down && pid == GetCurrentProcessId()) {
            LONG next = visible ? 0 : 1;
            InterlockedExchange(&visible, next);
            host->log("debug", next ? "Overlay shown" : "Overlay hidden");
        }
        last_down = down;
        Sleep(35);
    }
}

__declspec(dllexport) BOOL WINAPI EM4PluginInit(const EM4HostAPI *api) {
    HANDLE worker;
    if (!api || api->size < sizeof(EM4HostAPI) ||
        api->version != EM4_PLUGIN_API_VERSION || !api->get_game_window || !api->log)
        return FALSE;
    host = api;
    QueryPerformanceFrequency(&frequency);
    worker = CreateThread(NULL, 0, debug_worker, NULL, 0, NULL);
    if (!worker) return FALSE;
    CloseHandle(worker);
    return TRUE;
}

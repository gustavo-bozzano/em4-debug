#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gl/gl.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "em4_plugin.h"
#include "hot_reload.h"
#include "unit_inspector.h"
#include "path_viewer.h"
#include "direct_control.h"

#define HISTORY_SIZE 120
#define LOG_CAPACITY 96
#define LOG_WIDTH 160
#define LOG_VIEW_ROWS 9
#define INSPECTOR_VIEW_ROWS 6
#define PAGE_COUNT 7
#define PAGE_UNIT_INSPECTOR 5
#define PAGE_MAP_PATHS 6

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
static SIZE_T private_mb;
static double average_ms, p95_ms, worst_ms, cpu_percent;
static double frame_times[HISTORY_SIZE];
static unsigned frame_next, frame_valid;
static LARGE_INTEGER previous_frame_tick;
static ULONGLONG previous_cpu_ticks, started_at;
static DWORD processor_count = 1, handle_count;
static volatile LONG page, log_source, issues_only, log_scroll, inspector_scroll;
static CRITICAL_SECTION log_lock;
static char game_log_path[MAX_PATH], plugin_log_path[MAX_PATH], snapshot_path[MAX_PATH];
static char game_lines[LOG_CAPACITY][LOG_WIDTH];
static char plugin_lines[LOG_CAPACITY][LOG_WIDTH];
static char game_issues[LOG_CAPACITY][LOG_WIDTH];
static char plugin_issues[LOG_CAPACITY][LOG_WIDTH];
static unsigned game_line_count, plugin_line_count;
static unsigned game_issue_count, plugin_issue_count;
static char gl_vendor[100], gl_renderer[100], gl_version[100];
static GLint max_texture, depth_bits;
static HGLRC graphics_context;

static ULONGLONG ticks_from_filetime(FILETIME value) {
    ULARGE_INTEGER result;
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

static int compare_doubles(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

static int contains_case_insensitive(const char *text, const char *needle) {
    size_t length = strlen(needle), i;
    for (; *text; ++text) {
        for (i = 0; i < length && text[i] &&
            tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i]); ++i) {}
        if (i == length) return 1;
    }
    return 0;
}

static int is_issue(const char *line) {
    return line[0] == '!' || line[0] == '?' ||
        contains_case_insensitive(line, "error") ||
        contains_case_insensitive(line, "failed") ||
        contains_case_insensitive(line, "warning") ||
        contains_case_insensitive(line, "exception") ||
        contains_case_insensitive(line, "not found") ||
        contains_case_insensitive(line, "cannot");
}

static void read_recent_log(const char *path, char lines[LOG_CAPACITY][LOG_WIDTH],
                            unsigned *count, char issues[LOG_CAPACITY][LOG_WIDTH],
                            unsigned *issue_count) {
    HANDLE file = CreateFileA(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size, start;
    DWORD bytes_read = 0;
    static char data[524289];
    static char recent[LOG_CAPACITY][LOG_WIDTH];
    static char recent_issues[LOG_CAPACITY][LOG_WIDTH];
    unsigned found = 0, found_issues = 0;
    size_t pos, begin;
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        EnterCriticalSection(&log_lock);
        *count = 0;
        *issue_count = 0;
        LeaveCriticalSection(&log_lock);
        return;
    }
    start.QuadPart = size.QuadPart > 524288 ? size.QuadPart - 524288 : 0;
    if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN) ||
        !ReadFile(file, data, (DWORD)(size.QuadPart - start.QuadPart), &bytes_read, NULL)) {
        CloseHandle(file);
        return;
    }
    CloseHandle(file);
    pos = 0;
    if (start.QuadPart) {
        while (pos < bytes_read && data[pos] != '\n') ++pos;
        if (pos < bytes_read) ++pos;
    }
    begin = pos;
    for (; pos <= bytes_read; ++pos) {
        if (pos == bytes_read || data[pos] == '\n') {
            size_t length = pos - begin, i;
            char *target;
            if (length && data[pos - 1] == '\r') --length;
            if (!length) { begin = pos + 1; continue; }
            target = recent[found % LOG_CAPACITY];
            if (length >= LOG_WIDTH) length = LOG_WIDTH - 1;
            for (i = 0; i < length; ++i) {
                unsigned char c = (unsigned char)data[begin + i];
                target[i] = c >= 32 && c <= 126 ? (char)c : (c == '\t' ? ' ' : '?');
            }
            target[length] = '\0';
            if (is_issue(target)) {
                memcpy(recent_issues[found_issues % LOG_CAPACITY], target, LOG_WIDTH);
                ++found_issues;
            }
            ++found;
            begin = pos + 1;
        }
    }
    EnterCriticalSection(&log_lock);
    *count = found < LOG_CAPACITY ? found : LOG_CAPACITY;
    for (pos = 0; pos < *count; ++pos)
        memcpy(lines[pos], recent[(found - *count + pos) % LOG_CAPACITY], LOG_WIDTH);
    *issue_count = found_issues < LOG_CAPACITY ? found_issues : LOG_CAPACITY;
    for (pos = 0; pos < *issue_count; ++pos)
        memcpy(issues[pos], recent_issues[(found_issues - *issue_count + pos) % LOG_CAPACITY],
            LOG_WIDTH);
    LeaveCriticalSection(&log_lock);
}

static void save_snapshot(void) {
    FILE *output = fopen(snapshot_path, "wb");
    unsigned i;
    if (!output) {
        host->log("debug", "Could not write debug_snapshot.txt");
        return;
    }
    fprintf(output, "EM4 DEBUG SNAPSHOT\r\n");
    fprintf(output, "FPS: %.1f | frame avg: %.2f ms | p95: %.2f ms | worst: %.2f ms\r\n",
        fps, average_ms, p95_ms, worst_ms);
    fprintf(output, "CPU: %.1f%% | working RAM: %lu MB | private RAM: %lu MB | handles: %lu\r\n",
        cpu_percent, (unsigned long)memory_mb, (unsigned long)private_mb,
        (unsigned long)handle_count);
    fprintf(output, "OpenGL vendor: %s\r\nrenderer: %s\r\nversion: %s\r\n",
        gl_vendor, gl_renderer, gl_version);
    fprintf(output, "Max texture: %ld | depth bits: %ld\r\n",
        (long)max_texture, (long)depth_bits);
    EnterCriticalSection(&log_lock);
    fprintf(output, "\r\nRECENT GAME LOG\r\n");
    for (i = 0; i < game_line_count; ++i) fprintf(output, "%s\r\n", game_lines[i]);
    fprintf(output, "\r\nRECENT GAME ISSUES\r\n");
    for (i = 0; i < game_issue_count; ++i) fprintf(output, "%s\r\n", game_issues[i]);
    fprintf(output, "\r\nRECENT PLUGIN LOG\r\n");
    for (i = 0; i < plugin_line_count; ++i) fprintf(output, "%s\r\n", plugin_lines[i]);
    fprintf(output, "\r\nRECENT PLUGIN ISSUES\r\n");
    for (i = 0; i < plugin_issue_count; ++i) fprintf(output, "%s\r\n", plugin_issues[i]);
    LeaveCriticalSection(&log_lock);
    if (fclose(output) != 0) {
        host->log("debug", "Could not close debug_snapshot.txt");
        return;
    }
    host->log("debug", "Diagnostic snapshot saved");
}

static void draw_text(float x, float y, const char *value, unsigned width) {
    char safe[LOG_WIDTH];
    unsigned i;
    if (width >= LOG_WIDTH) width = LOG_WIDTH - 1;
    for (i = 0; i < width && value[i]; ++i) {
        unsigned char c = (unsigned char)value[i];
        safe[i] = c >= 32 && c <= 126 ? (char)c : '?';
    }
    safe[i] = '\0';
    glRasterPos2f(x, y);
    glListBase(font_base - 32);
    glCallLists((GLsizei)i, GL_UNSIGNED_BYTE, safe);
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
    PROCESS_MEMORY_COUNTERS_EX counters;
    FILETIME created, exited, kernel, user;
    double elapsed;
    ++frame_count;
    QueryPerformanceCounter(&now);
    if (previous_frame_tick.QuadPart && frequency.QuadPart) {
        double ms = (double)(now.QuadPart - previous_frame_tick.QuadPart) * 1000.0 /
            (double)frequency.QuadPart;
        if (ms >= 0.0 && ms < 10000.0) {
            frame_times[frame_next] = ms;
            frame_next = (frame_next + 1) % HISTORY_SIZE;
            if (frame_valid < HISTORY_SIZE) ++frame_valid;
        }
    }
    previous_frame_tick = now;
    if (!last_fps_tick.QuadPart) {
        last_fps_tick = now;
        return;
    }
    elapsed = (double)(now.QuadPart - last_fps_tick.QuadPart) /
        (double)frequency.QuadPart;
    if (elapsed >= 1.0) {
        double sorted[HISTORY_SIZE], sum = 0.0;
        unsigned i;
        fps = frame_count / elapsed;
        for (i = 0; i < frame_valid; ++i) {
            sorted[i] = frame_times[i];
            sum += sorted[i];
        }
        if (frame_valid) {
            qsort(sorted, frame_valid, sizeof(double), compare_doubles);
            average_ms = sum / frame_valid;
            p95_ms = sorted[(frame_valid * 95 + 99) / 100 - 1];
            worst_ms = sorted[frame_valid - 1];
        }
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters,
                sizeof(counters))) {
            memory_mb = counters.WorkingSetSize / (1024 * 1024);
            private_mb = counters.PrivateUsage / (1024 * 1024);
        }
        GetProcessHandleCount(GetCurrentProcess(), &handle_count);
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            ULONGLONG ticks = ticks_from_filetime(kernel) + ticks_from_filetime(user);
            if (previous_cpu_ticks && ticks >= previous_cpu_ticks)
                cpu_percent = (double)(ticks - previous_cpu_ticks) * 100.0 /
                    (elapsed * 10000000.0 * processor_count);
            previous_cpu_ticks = ticks;
        }
        frame_count = 0;
        last_fps_tick = now;
    }
}

static void draw_log_page(unsigned width) {
    char rows[LOG_VIEW_ROWS][LOG_WIDTH] = {{0}};
    unsigned count, i, first, end, scroll;
    char (*source)[LOG_WIDTH];
    char line[LOG_WIDTH];
    int filter = issues_only != 0;
    EnterCriticalSection(&log_lock);
    source = log_source ? (filter ? plugin_issues : plugin_lines) :
        (filter ? game_issues : game_lines);
    count = log_source ? (filter ? plugin_issue_count : plugin_line_count) :
        (filter ? game_issue_count : game_line_count);
    scroll = log_scroll < 0 ? 0 : (unsigned)log_scroll;
    if (scroll > count) scroll = count;
    end = count - scroll;
    first = end > LOG_VIEW_ROWS ? end - LOG_VIEW_ROWS : 0;
    for (i = first; i < end; ++i)
        memcpy(rows[i - first], source[i], LOG_WIDTH);
    LeaveCriticalSection(&log_lock);

    snprintf(line, sizeof(line), "%s LOG | %s | %u recent lines",
        log_source ? "PLUGIN" : "GAME", filter ? "ISSUES" : "ALL", count);
    draw_text(18, 50, line, width);
    if (!count) draw_text(18, 90, "No matching entries in the recent 512 KiB.", width);
    for (i = first; i < end; ++i) {
        int issue = is_issue(rows[i - first]);
        glColor3f(1.0f, issue ? 0.65f : 1.0f, issue ? 0.45f : 1.0f);
        draw_text(18, (float)(76 + 19 * (i - first)), rows[i - first], width);
    }
    glColor3f(0.7f, 0.85f, 1.0f);
    draw_text(18, 279, "F8 source  F7 issues  PgUp/PgDn scroll  End latest", width);
}

static void draw_overlay(HDC dc) {
    GLint viewport[4], previous_matrix;
    HWND window;
    LONG style;
    char line[LOG_WIDTH];
    unsigned width;
    int panel_right;
    const char *pages[] = { "OVERVIEW", "TIMING", "GRAPHICS", "LOGS", "HOT RELOAD", "UNIT INSPECTOR", "MAP PATHS" };
    LONG current_page = page;
    HGLRC context = wglGetCurrentContext();

    if (context && context != graphics_context) {
        const GLubyte *vendor = glGetString(GL_VENDOR);
        const GLubyte *renderer = glGetString(GL_RENDERER);
        const GLubyte *version = glGetString(GL_VERSION);
        snprintf(gl_vendor, sizeof(gl_vendor), "%s", vendor ? (const char *)vendor : "Unavailable");
        snprintf(gl_renderer, sizeof(gl_renderer), "%s", renderer ? (const char *)renderer : "Unavailable");
        snprintf(gl_version, sizeof(gl_version), "%s", version ? (const char *)version : "Unavailable");
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
        glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
        graphics_context = context;
    }
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] < 400 || viewport[3] < 340 || !prepare_font(dc)) return;
    panel_right = viewport[2] < 760 ? viewport[2] - 8 : 752;
    width = (unsigned)(panel_right - 28) / 9;
    if (width >= LOG_WIDTH) width = LOG_WIDTH - 1;
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
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColor4f(0.0f, 0.0f, 0.0f, 0.82f);
    glBegin(GL_QUADS);
    glVertex2f(8, 8);
    glVertex2f((float)panel_right, 8);
    glVertex2f((float)panel_right, 326);
    glVertex2f(8, 326);
    glEnd();

    glColor3f(0.4f, 1.0f, 0.5f);
    snprintf(line, sizeof(line), "EM4 DEBUG | %s | Ctrl+F10 next  F10 hide  F6 snapshot",
        pages[current_page >= 0 && current_page < PAGE_COUNT ? current_page : 0]);
    draw_text(18, 27, line, width);
    glColor3f(1.0f, 1.0f, 1.0f);
    if (current_page == 0) {
        snprintf(line, sizeof(line), "FPS %.1f | Frame avg %.2f ms | p95 %.2f ms",
            fps, average_ms, p95_ms);
        draw_text(18, 50, line, width);
        snprintf(line, sizeof(line), "Worst %.2f ms | CPU %.1f%% of total capacity",
            worst_ms, cpu_percent);
        draw_text(18, 72, line, width);
        snprintf(line, sizeof(line), "RAM working %lu MB | private %lu MB | handles %lu",
            (unsigned long)memory_mb, (unsigned long)private_mb,
            (unsigned long)handle_count);
        draw_text(18, 94, line, width);
        window = host->get_game_window();
        style = window ? GetWindowLongA(window, GWL_STYLE) : 0;
        snprintf(line, sizeof(line), "Viewport %dx%d | %s | Uptime %llu s",
            viewport[2], viewport[3],
            (style & (WS_CAPTION | WS_THICKFRAME)) ? "WINDOW" : "NO BORDER",
            (unsigned long long)((GetTickCount64() - started_at) / 1000));
        draw_text(18, 116, line, width);
        draw_text(18, 148, "Ctrl+F8: WASD control and camera follow for one unit", width);
        draw_text(18, 170, "Ctrl+F10: cycle diagnostics, inspector, and map paths", width);
        draw_text(18, 192, "F11: toggle window mode", width);
    } else if (current_page == 1) {
        unsigned i;
        float graph_width = (float)(panel_right - 44);
        snprintf(line, sizeof(line), "Recent %u frames | FPS %.1f", frame_valid, fps);
        draw_text(18, 50, line, width);
        snprintf(line, sizeof(line), "Average %.2f ms | p95 %.2f ms | worst %.2f ms",
            average_ms, p95_ms, worst_ms);
        draw_text(18, 72, line, width);
        draw_text(18, 103, "Frame times: green <16.7ms, amber <33.3ms, red >=33.3ms", width);
        for (i = 0; i < frame_valid; ++i) {
            unsigned index = (frame_next + HISTORY_SIZE - frame_valid + i) % HISTORY_SIZE;
            double ms = frame_times[index];
            float x = 20 + graph_width * i / HISTORY_SIZE;
            float height = (float)(ms * 2.0);
            if (height > 100) height = 100;
            if (ms >= 33.3) glColor3f(1.0f, 0.3f, 0.3f);
            else if (ms >= 16.7) glColor3f(1.0f, 0.75f, 0.25f);
            else glColor3f(0.4f, 1.0f, 0.5f);
            glBegin(GL_QUADS);
            glVertex2f(x, 245);
            glVertex2f(x + 3, 245);
            glVertex2f(x + 3, 245 - height);
            glVertex2f(x, 245 - height);
            glEnd();
        }
        glColor3f(0.7f, 0.85f, 1.0f);
        draw_text(18, 279, "Numbers refresh each second; the graph updates per frame.", width);
    } else if (current_page == 2) {
        snprintf(line, sizeof(line), "Vendor: %s", gl_vendor);
        draw_text(18, 50, line, width);
        snprintf(line, sizeof(line), "Renderer: %s", gl_renderer);
        draw_text(18, 72, line, width);
        snprintf(line, sizeof(line), "OpenGL version: %s", gl_version);
        draw_text(18, 94, line, width);
        snprintf(line, sizeof(line), "Max texture %ld | Depth %ld bits | Pixel format %d",
            (long)max_texture, (long)depth_bits, GetPixelFormat(dc));
        draw_text(18, 116, line, width);
        snprintf(line, sizeof(line), "Viewport %dx%d | Process ID %lu",
            viewport[2], viewport[3], (unsigned long)GetCurrentProcessId());
        draw_text(18, 138, line, width);
    } else if (current_page == 3) {
        draw_log_page(width);
    } else if (current_page == 4) {
        char target[260], result[160], watch_status[160];
        hot_reload_get_display(target, sizeof(target), result, sizeof(result),
            watch_status, sizeof(watch_status));
        draw_text(18, 50, "Automatic reload is ON for loaded assets.", width);
#if defined(EM4_EXPERIMENTAL_SCRIPT_RELOAD) && defined(EM4_ALLOW_UNSAFE_SCRIPT_RELOAD_TEST)
        draw_text(18, 77, "Ctrl+F7 recompiles the last changed command script.", width);
#else
        draw_text(18, 77, "Changed .script files need a game session restart.", width);
#endif
        draw_text(18, 104, "Ctrl+F6 retries the last changed asset.", width);
        draw_text(18, 131, "Mission scripts are excluded from live reload.", width);
        draw_text(18, 169, "Last changed or selected resource:", width);
        draw_text(18, 190, target, width);
        draw_text(18, 223, result, width);
        draw_text(18, 249, watch_status, width);
        glColor3f(0.7f, 0.85f, 1.0f);
        draw_text(18, 279, "Model copies stay next to source files. Avoid saving this test session.", width);
    } else if (current_page == PAGE_UNIT_INSPECTOR) {
        UnitInspectorSnapshot snapshot;
        unsigned first, i;
        unit_inspector_copy(&snapshot);
        draw_text(18, 50, snapshot.summary, width);
        first = inspector_scroll < 0 ? 0 : (unsigned)inspector_scroll;
        if (first >= snapshot.shown_count && snapshot.shown_count)
            first = snapshot.shown_count - 1;
        snprintf(line, sizeof(line), "%u attached command%s | showing %u-%u",
            snapshot.command_count, snapshot.command_count == 1 ? "" : "s",
            snapshot.shown_count ? first + 1 : 0,
            snapshot.shown_count < first + INSPECTOR_VIEW_ROWS ?
                snapshot.shown_count : first + INSPECTOR_VIEW_ROWS);
        draw_text(18, 72, line, width);
        glColor3f(0.7f, 0.85f, 1.0f);
        draw_text(18, 92, snapshot.query_result[0] ? snapshot.query_result :
            "To check a missing name: EM4Plugins/inspect_command.txt", width);
        for (i = 0; i < INSPECTOR_VIEW_ROWS && first + i < snapshot.shown_count; ++i) {
            const UnitCommandRow *row = &snapshot.rows[first + i];
            float y = (float)(116 + i * 27);
            if (strstr(row->status, "denied") || strstr(row->status, "Missing") ||
                strstr(row->status, "Different")) glColor3f(1.0f, 0.65f, 0.45f);
            else glColor3f(1.0f, 1.0f, 1.0f);
            snprintf(line, sizeof(line), "%s [group %u] | %s",
                row->name, row->group, row->status);
            draw_text(18, y, line, width);
            glColor3f(0.7f, 0.85f, 1.0f);
            draw_text(27, y + 12, row->script, width - 2);
        }
        glColor3f(0.7f, 0.85f, 1.0f);
        draw_text(18, 293, "PgUp/PgDn scroll | Status is the last engine check (10 s).", width);
    }
    glColor3f(1.0f, 0.85f, 0.45f);
    draw_text(18, 312, "Ctrl+Shift+F11: restore the game if it stays minimized.", width);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(previous_matrix);
    glPopAttrib();
}

static void draw_path_labels(HDC dc) {
    PathViewerLabel rows[PATH_VIEWER_MAX_LABELS];
    unsigned count = path_viewer_copy_labels(rows, PATH_VIEWER_MAX_LABELS);
    GLint viewport[4], previous_matrix;
    unsigned i;
    if (!count || !prepare_font(dc)) return;
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) return;
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
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_BLEND);
    for (i = 0; i < count; ++i) {
        glColor3f(0.0f, 0.0f, 0.0f);
        draw_text(rows[i].x + 1, rows[i].y + 1, rows[i].name, 63);
        glColor3f(1.0f, 1.0f, 0.75f);
        draw_text(rows[i].x, rows[i].y, rows[i].name, 63);
    }
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(previous_matrix);
    glPopAttrib();
}

static void draw_control_badge(HDC dc, const char *message) {
    GLint previous_matrix, viewport[4];
    float top, right;
    if (!prepare_font(dc)) return;
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] < 320 || viewport[3] < 100) return;
    top = visible && page != PAGE_MAP_PATHS && viewport[3] >= 380 ? 334.0f : 8.0f;
    right = viewport[2] < 500 ? (float)viewport[2] - 8 : 492.0f;
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
    glColor4f(0.0f, 0.0f, 0.0f, 0.78f);
    glBegin(GL_QUADS);
    glVertex2f(8, top);
    glVertex2f(right, top);
    glVertex2f(right, top + 25);
    glVertex2f(8, top + 25);
    glEnd();
    glColor3f(0.4f, 1.0f, 0.5f);
    draw_text(16, top + 18, message, (unsigned)((right - 20) / 9));
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(previous_matrix);
    glPopAttrib();
}

static BOOL WINAPI hooked_swap(HDC dc) {
    BOOL result;
    update_metrics();
    if (InterlockedCompareExchange(&first_frame_logged, 1, 0) == 0)
        host->log("debug", "First SwapBuffers frame observed");
    if (visible && page == PAGE_UNIT_INSPECTOR) unit_inspector_on_frame();
    if (path_viewer_is_enabled()) path_viewer_on_frame();
    direct_control_on_frame();
    path_viewer_draw_world();
    if (path_viewer_is_enabled()) draw_path_labels(dc);
    if (visible && page != PAGE_MAP_PATHS) draw_overlay(dc);
    {
        const char *control_status = direct_control_status_text();
        if (control_status) draw_control_badge(dc, control_status);
    }
    result = real_swap(dc);
    hot_reload_on_frame();
    return result;
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

static void sync_path_page(void) {
    int should_show = visible && page == PAGE_MAP_PATHS;
    if (path_viewer_is_enabled() != should_show) path_viewer_toggle();
}

static DWORD WINAPI debug_worker(LPVOID unused) {
    const int keys[] = { VK_F6, VK_F7, VK_F8, VK_F10,
        VK_PRIOR, VK_NEXT, VK_END };
    BOOL previous[7] = { FALSE };
    ULONGLONG last_log_refresh = 0;
    int attempts;
    (void)unused;
    for (attempts = 0; attempts < 300 && !install_swap_hook(); ++attempts)
        Sleep(100);
    if (attempts == 300) {
        host->log("debug", "Could not find vBase71.dll SwapBuffers delay import");
        return 0;
    }
#if defined(EM4_EXPERIMENTAL_SCRIPT_RELOAD) && defined(EM4_ALLOW_UNSAFE_SCRIPT_RELOAD_TEST)
    host->log("debug", "SwapBuffers hook installed; Ctrl+F6 asset retry, Ctrl+F7 command script reload, Ctrl+F10 pages including map paths, F10 visibility, F6 snapshot");
#else
    host->log("debug", "SwapBuffers hook installed; Ctrl+F6 asset retry, Ctrl+F7 script status, Ctrl+F10 pages including map paths, F10 visibility, F6 snapshot");
#endif
    for (;;) {
        DWORD pid = 0;
        unsigned i;
        ULONGLONG now = GetTickCount64();
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            BOOL down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            if (down && !previous[i] && pid == GetCurrentProcessId()) {
                BOOL control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                switch (keys[i]) {
                case VK_F6:
                    if (control) hot_reload_request();
                    else save_snapshot();
                    break;
                case VK_F7:
                    if (control) hot_reload_request_scripts();
                    else {
                        InterlockedExchange(&issues_only, !issues_only);
                        InterlockedExchange(&log_scroll, 0);
                    }
                    break;
                case VK_F8:
                    if (control) direct_control_request_toggle();
                    else {
                        InterlockedExchange(&log_source, !log_source);
                        InterlockedExchange(&log_scroll, 0);
                    }
                    break;
                case VK_F10:
                    if (control) {
                        InterlockedExchange(&page, (page + 1) % PAGE_COUNT);
                        InterlockedExchange(&visible, 1);
                        sync_path_page();
                    }
                    else {
                        InterlockedExchange(&visible, !visible);
                        sync_path_page();
                        host->log("debug", visible ? "Overlay shown" : "Overlay hidden");
                    }
                    break;
                case VK_PRIOR:
                    if (page == 5) {
                        if (inspector_scroll > INSPECTOR_VIEW_ROWS)
                            InterlockedExchangeAdd(&inspector_scroll, -INSPECTOR_VIEW_ROWS);
                        else InterlockedExchange(&inspector_scroll, 0);
                    } else if (page == 3)
                        InterlockedExchangeAdd(&log_scroll, LOG_VIEW_ROWS);
                    break;
                case VK_NEXT:
                    if (page == 5)
                        InterlockedExchangeAdd(&inspector_scroll, INSPECTOR_VIEW_ROWS);
                    else if (page == 3) {
                        if (log_scroll > LOG_VIEW_ROWS)
                            InterlockedExchangeAdd(&log_scroll, -LOG_VIEW_ROWS);
                        else InterlockedExchange(&log_scroll, 0);
                    }
                    break;
                case VK_END:
                    if (page == 5) InterlockedExchange(&inspector_scroll, 0);
                    else if (page == 3) InterlockedExchange(&log_scroll, 0);
                    break;
                }
            }
            previous[i] = down;
        }
        if (now - last_log_refresh >= 500) {
            read_recent_log(game_log_path, game_lines, &game_line_count,
                game_issues, &game_issue_count);
            read_recent_log(plugin_log_path, plugin_lines, &plugin_line_count,
                plugin_issues, &plugin_issue_count);
            last_log_refresh = now;
        }
        Sleep(35);
    }
}

__declspec(dllexport) BOOL WINAPI EM4PluginInit(const EM4HostAPI *api) {
    HANDLE worker;
    char executable[MAX_PATH];
    char *slash;
    SYSTEM_INFO system_info;
    if (!api || api->size < sizeof(EM4HostAPI) ||
        api->version != EM4_PLUGIN_API_VERSION || !api->get_game_window || !api->log)
        return FALSE;
    if (!GetModuleFileNameA(NULL, executable, MAX_PATH)) return FALSE;
    slash = strrchr(executable, '\\');
    if (!slash) return FALSE;
    slash[1] = '\0';
    if (snprintf(game_log_path, sizeof(game_log_path), "%slogfile.txt", executable) >= MAX_PATH ||
        snprintf(plugin_log_path, sizeof(plugin_log_path), "%sEM4Plugins\\em4_plugins.log", executable) >= MAX_PATH ||
        snprintf(snapshot_path, sizeof(snapshot_path), "%sEM4Plugins\\debug_snapshot.txt", executable) >= MAX_PATH)
        return FALSE;
    host = api;
    hot_reload_init(api, executable);
    unit_inspector_init(api);
    path_viewer_init(api);
    direct_control_init(api);
    InitializeCriticalSection(&log_lock);
    QueryPerformanceFrequency(&frequency);
    if (!frequency.QuadPart) {
        DeleteCriticalSection(&log_lock);
        return FALSE;
    }
    GetSystemInfo(&system_info);
    processor_count = system_info.dwNumberOfProcessors ? system_info.dwNumberOfProcessors : 1;
    started_at = GetTickCount64();
    worker = CreateThread(NULL, 0, debug_worker, NULL, 0, NULL);
    if (!worker) {
        DeleteCriticalSection(&log_lock);
        return FALSE;
    }
    CloseHandle(worker);
    return TRUE;
}

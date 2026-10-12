#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "hot_reload.h"

#define TARGET_CAPACITY 260
#define CHANGE_CAPACITY 64

typedef int (__attribute__((thiscall)) *ResourceCountFn)(void *);
typedef void *(__attribute__((thiscall)) *ResourceAtFn)(void *, int);
typedef const char *(__attribute__((thiscall)) *ModelFilenameFn)(void *);
typedef const char *(__attribute__((thiscall)) *TextureFilenameFn)(void *);
typedef void *(__attribute__((thiscall)) *TextureByNameFn)(void *, const char *);
typedef void (__attribute__((thiscall)) *DoReloadFn)(void *);
typedef void (__attribute__((thiscall)) *DoUnloadFn)(void *);

typedef struct ChangedFile {
    char path[TARGET_CAPACITY];
    ULONGLONG changed_at;
} ChangedFile;

static const EM4HostAPI *reload_host;
static CRITICAL_SECTION reload_lock;
static char config_path[MAX_PATH];
static char manual_target[TARGET_CAPACITY];
static char last_auto_target[TARGET_CAPACITY];
static char display_target[TARGET_CAPACITY] = "(waiting for a changed asset)";
static char display_result[160] = "Automatic watch is starting.";
static char display_watch_status[160] = "Watching Data and Mods for file changes.";
static ChangedFile changes[CHANGE_CAPACITY];
static unsigned change_count, reload_count, dropped_changes;
static volatile LONG manual_pending;

static void update_watch_status(void) {
    snprintf(display_watch_status, sizeof(display_watch_status),
        "Watching Data + Mods | %u pending | %u reloaded | %u skipped",
        change_count, reload_count, dropped_changes);
}

static void set_result(const char *message) {
    EnterCriticalSection(&reload_lock);
    snprintf(display_result, sizeof(display_result), "%s", message);
    LeaveCriticalSection(&reload_lock);
    if (reload_host) reload_host->log("debug", message);
}

static int ends_with(const char *text, const char *suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    return a >= b && _stricmp(text + a - b, suffix) == 0;
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    return slash ? slash + 1 : path;
}

static int same_resource_name(const char *left, const char *right) {
    char a[TARGET_CAPACITY], b[TARGET_CAPACITY];
    size_t i;
    if (!left || !right || strlen(left) >= sizeof(a) || strlen(right) >= sizeof(b))
        return 0;
    for (i = 0; left[i]; ++i)
        a[i] = (char)tolower((unsigned char)(left[i] == '\\' ? '/' : left[i]));
    a[i] = 0;
    for (i = 0; right[i]; ++i)
        b[i] = (char)tolower((unsigned char)(right[i] == '\\' ? '/' : right[i]));
    b[i] = 0;
    if (strncmp(a, "mod:/", 5) == 0 || strncmp(a, "base:/", 6) == 0) {
        unsigned prefix = a[0] == 'm' ? 4 : 5;
        memmove(a + prefix, a + prefix + 1, strlen(a + prefix + 1) + 1);
    }
    if (strncmp(b, "mod:/", 5) == 0 || strncmp(b, "base:/", 6) == 0) {
        unsigned prefix = b[0] == 'm' ? 4 : 5;
        memmove(b + prefix, b + prefix + 1, strlen(b + prefix + 1) + 1);
    }
    if (ends_with(a, ".v3o")) a[strlen(a) - 4] = 0;
    if (ends_with(b, ".v3o")) b[strlen(b) - 4] = 0;
    if (strcmp(a, b) == 0) return 1;
    if (strncmp(a, "mod:", 4) == 0 && strcmp(a + 4, b) == 0) return 1;
    if (strncmp(b, "mod:", 4) == 0 && strcmp(a, b + 4) == 0) return 1;
    if (strncmp(a, "base:", 5) == 0 && strcmp(a + 5, b) == 0) return 1;
    if (strncmp(b, "base:", 5) == 0 && strcmp(a, b + 5) == 0) return 1;
    return 0;
}

static int known_engine_module(HMODULE module, DWORD timestamp, DWORD image_size) {
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module;
    const IMAGE_NT_HEADERS32 *nt;
    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < 0 || dos->e_lfanew > 4096) return 0;
    nt = (const IMAGE_NT_HEADERS32 *)((const char *)module + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
        nt->FileHeader.TimeDateStamp == timestamp &&
        nt->OptionalHeader.SizeOfImage == image_size;
}

void hot_reload_init(const EM4HostAPI *host, const char *game_directory) {
    int watchers;
    reload_host = host;
    InitializeCriticalSection(&reload_lock);
    snprintf(config_path, sizeof(config_path), "%sEM4Plugins\\reload_target.txt",
        game_directory);
    watchers = hot_reload_watch_directories(game_directory);
    host->log("debug", watchers == 2 ?
        "Automatic file watch started for Data and Mods" :
        "Automatic file watch could not start for both Data and Mods");
}

void hot_reload_file_changed(const char *logical_path) {
    unsigned i;
    ULONGLONG now = GetTickCount64();
    if (!logical_path || !*logical_path || strlen(logical_path) >= TARGET_CAPACITY)
        return;
    EnterCriticalSection(&reload_lock);
    for (i = 0; i < change_count; ++i) {
        if (_stricmp(changes[i].path, logical_path) == 0) {
            changes[i].changed_at = now;
            LeaveCriticalSection(&reload_lock);
            return;
        }
    }
    if (change_count == CHANGE_CAPACITY) {
        memmove(changes, changes + 1, (CHANGE_CAPACITY - 1) * sizeof(changes[0]));
        --change_count;
        ++dropped_changes;
    }
    snprintf(changes[change_count].path, TARGET_CAPACITY, "%s", logical_path);
    changes[change_count].changed_at = now;
    ++change_count;
    update_watch_status();
    LeaveCriticalSection(&reload_lock);
}

static void request_retry_or_report(void) {
    char path[TARGET_CAPACITY];
    EnterCriticalSection(&reload_lock);
    snprintf(path, sizeof(path), "%s", last_auto_target);
    LeaveCriticalSection(&reload_lock);
    if (*path) {
        hot_reload_file_changed(path);
        set_result("F5: retrying the last changed asset.");
    } else set_result("Automatic watch is active; no changed asset to retry yet.");
}

void hot_reload_request(void) {
    FILE *file;
    char line[TARGET_CAPACITY], *start, *end;
    int ch;
    file = fopen(config_path, "rb");
    if (!file) { request_retry_or_report(); return; }
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        request_retry_or_report();
        return;
    }
    if (!strchr(line, '\n') && (ch = fgetc(file)) != EOF) {
        fclose(file);
        set_result("F5: target is too long (259 byte limit).");
        return;
    }
    fclose(file);
    start = line;
    while (*start && isspace((unsigned char)*start)) ++start;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) --end;
    *end = 0;
    if (!*start || *start == '#') { request_retry_or_report(); return; }
    if (!ends_with(start, ".v3o") && !ends_with(start, ".dds") &&
        !ends_with(start, ".tga") && !ends_with(start, ".png") &&
        !ends_with(start, ".bmp") && !ends_with(start, ".jpg") &&
        !ends_with(start, ".jpeg")) {
        set_result("F5: expected .v3o or a texture extension.");
        return;
    }
    EnterCriticalSection(&reload_lock);
    if (manual_pending) {
        LeaveCriticalSection(&reload_lock);
        set_result("F5: a reload is already queued.");
        return;
    }
    snprintf(manual_target, sizeof(manual_target), "%s", start);
    snprintf(display_target, sizeof(display_target), "%s", start);
    snprintf(display_result, sizeof(display_result), "F5: waiting for the next game frame...");
    InterlockedExchange(&manual_pending, 1);
    LeaveCriticalSection(&reload_lock);
}

void hot_reload_get_display(char *target, unsigned target_size,
                            char *result, unsigned result_size,
                            char *watch_status, unsigned watch_status_size) {
    EnterCriticalSection(&reload_lock);
    if (target_size) snprintf(target, target_size, "%s", display_target);
    if (result_size) snprintf(result, result_size, "%s", display_result);
    if (watch_status_size)
        snprintf(watch_status, watch_status_size, "%s", display_watch_status);
    LeaveCriticalSection(&reload_lock);
}

static void reload_resource(const char *target, int automatic) {
    HMODULE vision = GetModuleHandleA("vision71.dll");
    HMODULE base = GetModuleHandleA("vBase71.dll");
    ResourceCountFn resource_count;
    ResourceAtFn resource_at;
    ModelFilenameFn model_filename;
    TextureFilenameFn texture_filename;
    TextureByNameFn texture_by_name;
    DoUnloadFn do_unload;
    DoReloadFn do_reload;
    void **model_manager_ptr, *texture_manager, *texture_resources;
    int is_model = ends_with(target, ".v3o");
    int matches = 0, success = 0, i, count;
    char message[160];
    if (!vision || !base ||
        !known_engine_module(vision, 0x43fb75cb, 0x291000) ||
        !known_engine_module(base, 0x43d8f766, 0xd8000)) {
        set_result("Reload requires the verified Steam engine build.");
        return;
    }
    resource_count = (ResourceCountFn)GetProcAddress(base,
        "?GetResourceCount@VResourceManager@@QBEHXZ");
    resource_at = (ResourceAtFn)GetProcAddress(base,
        "?GetResourceByIndex@VResourceManager@@QBEPAVVManagedResource@@H@Z");
    model_filename = (ModelFilenameFn)GetProcAddress(vision,
        "?GetModelFileName@VisModelInterface_cl@@QBEPBDXZ");
    texture_filename = (TextureFilenameFn)GetProcAddress(base,
        "?GetFilename@VTextureObject@@QBEPBDXZ");
    texture_by_name = (TextureByNameFn)GetProcAddress(vision,
        "?GetTextureObjectByName@VisTextureManager_cl@@QAEPAVVTextureObject@@PBD@Z");
    do_unload = (DoUnloadFn)GetProcAddress(base,
        "?DoUnload@VManagedResource@@MAEXXZ");
    do_reload = (DoReloadFn)GetProcAddress(base,
        "?DoReload@VManagedResource@@MAEXXZ");
    model_manager_ptr = (void **)GetProcAddress(vision,
        "?g_pResourceManager@?$VisRMElementManager_cl@PAVVisModelInterface_cl@@@@1PAVVisResourceManager_cl@@A");
    texture_manager = GetProcAddress(vision,
        "?TextureManager@Vision@@2VVisTextureManager_cl@@A");
    /* GetTextureObjectByName uses this embedded VResourceManager in this build. */
    texture_resources = texture_manager ? (char *)texture_manager + 0x578 : NULL;
    if (!resource_count || !resource_at || !model_filename || !texture_filename ||
        !texture_by_name || !do_unload || !do_reload || !model_manager_ptr ||
        !*model_manager_ptr || !texture_manager || !texture_resources) {
        set_result("Reload API is not ready yet.");
        return;
    }
    count = resource_count(is_model ? *model_manager_ptr : texture_resources);
    if (count < 0 || count > 200000) {
        set_result("Resource manager returned an invalid count.");
        return;
    }
    for (i = 0; i < count; ++i) {
        void *resource = resource_at(is_model ? *model_manager_ptr : texture_resources, i);
        const char *name = resource ? (is_model ?
            model_filename((char *)resource - 8) : texture_filename(resource)) : NULL;
        if (!resource || !same_resource_name(name, target) ||
            !(((const unsigned char *)resource)[8] & 1)) continue;
        ++matches;
        do_unload(resource);
        do_reload(resource);
        if (((const unsigned char *)resource)[8] & 1) ++success;
    }
    if (!is_model && !matches) {
        void *only = NULL;
        int basename_matches = 0;
        const char *wanted = basename_of(target);
        for (i = 0; i < count; ++i) {
            void *candidate = resource_at(texture_resources, i);
            const char *name = candidate ? texture_filename(candidate) : NULL;
            if (name && _stricmp(basename_of(name), wanted) == 0 &&
                (((const unsigned char *)candidate)[8] & 1)) {
                only = candidate;
                ++basename_matches;
            }
        }
        if (basename_matches == 1) {
            ++matches;
            do_unload(only);
            do_reload(only);
            if (((const unsigned char *)only)[8] & 1) ++success;
        } else if (basename_matches > 1) {
            set_result("Texture basename is ambiguous among loaded resources.");
            return;
        }
    }
    if (!is_model && !matches && !automatic) {
        void *resource = texture_by_name(texture_manager, target);
        if (resource && (((const unsigned char *)resource)[8] & 1)) {
            ++matches;
            do_unload(resource);
            do_reload(resource);
            if (((const unsigned char *)resource)[8] & 1) ++success;
        }
    }
    if (!matches) snprintf(message, sizeof(message),
        "%s: changed file has no loaded %s resource.",
        automatic ? "AUTO" : "F5", is_model ? "model" : "texture");
    else snprintf(message, sizeof(message), "%s: reloaded %d/%d %s resource(s).",
        automatic ? "AUTO" : "F5", success, matches,
        is_model ? "model" : "texture");
    if (success) {
        EnterCriticalSection(&reload_lock);
        reload_count += (unsigned)success;
        update_watch_status();
        LeaveCriticalSection(&reload_lock);
    }
    set_result(message);
}

static void process_auto_change(void) {
    char path[TARGET_CAPACITY];
    unsigned i, selected = CHANGE_CAPACITY;
    ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&reload_lock);
    for (i = 0; i < change_count; ++i) {
        if (now - changes[i].changed_at >= 750) {
            selected = i;
            break;
        }
    }
    if (selected == CHANGE_CAPACITY) {
        LeaveCriticalSection(&reload_lock);
        return;
    }
    snprintf(path, sizeof(path), "%s", changes[selected].path);
    memmove(changes + selected, changes + selected + 1,
        (change_count - selected - 1) * sizeof(changes[0]));
    --change_count;
    snprintf(last_auto_target, sizeof(last_auto_target), "%s", path);
    snprintf(display_target, sizeof(display_target), "%s", path);
    update_watch_status();
    LeaveCriticalSection(&reload_lock);
    reload_resource(path, 1);
}

void hot_reload_on_frame(void) {
    char target[TARGET_CAPACITY];
    process_auto_change();
    EnterCriticalSection(&reload_lock);
    if (!manual_pending) {
        LeaveCriticalSection(&reload_lock);
        return;
    }
    snprintf(target, sizeof(target), "%s", manual_target);
    InterlockedExchange(&manual_pending, 2);
    LeaveCriticalSection(&reload_lock);
    reload_resource(target, 0);
    InterlockedExchange(&manual_pending, 0);
}

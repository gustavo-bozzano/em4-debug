#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
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
typedef void *(__attribute__((thiscall)) *LoadModelFn)(void *, const char *, int);
typedef int (__attribute__((thiscall)) *ModelSizeFn)(void *);
typedef void *(__attribute__((thiscall)) *EntityModelFn)(void *);
typedef int (__attribute__((thiscall)) *SetModelFn)(void *, const char *, void *);

typedef struct ChangedFile {
    char path[TARGET_CAPACITY];
    char physical[MAX_PATH];
    ULONGLONG changed_at;
} ChangedFile;

static const EM4HostAPI *reload_host;
static CRITICAL_SECTION reload_lock;
static char config_path[MAX_PATH];
static char manual_target[TARGET_CAPACITY];
static char last_auto_target[TARGET_CAPACITY];
static char last_auto_physical[MAX_PATH];
static char display_target[TARGET_CAPACITY] = "(waiting for a changed asset)";
static char display_result[160] = "Automatic watch is starting.";
static char display_watch_status[160] = "Watching Data and Mods for file changes.";
static ChangedFile changes[CHANGE_CAPACITY];
static unsigned change_count, reload_count, dropped_changes;
static volatile LONG manual_pending;
static unsigned model_generation;
typedef struct ModelAlias {
    char source[TARGET_CAPACITY];
    char original_name[TARGET_CAPACITY];
    char current_name[TARGET_CAPACITY];
    int future_swaps_disabled;
} ModelAlias;
static ModelAlias model_aliases[32];
static unsigned model_alias_count;


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

static ModelAlias *find_model_alias(const char *source) {
    unsigned i;
    for (i = 0; i < model_alias_count; ++i)
        if (same_resource_name(model_aliases[i].source, source))
            return &model_aliases[i];
    return NULL;
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

void hot_reload_file_changed(const char *logical_path, const char *physical_path) {
    unsigned i;
    ULONGLONG now = GetTickCount64();
    if (!logical_path || !*logical_path || strlen(logical_path) >= TARGET_CAPACITY ||
        !physical_path || !*physical_path || strlen(physical_path) >= MAX_PATH)
        return;
    EnterCriticalSection(&reload_lock);
    for (i = 0; i < change_count; ++i) {
        if (_stricmp(changes[i].path, logical_path) == 0) {
            changes[i].changed_at = now;
            snprintf(changes[i].physical, MAX_PATH, "%s", physical_path);
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
    snprintf(changes[change_count].physical, MAX_PATH, "%s", physical_path);
    changes[change_count].changed_at = now;
    ++change_count;
    update_watch_status();
    LeaveCriticalSection(&reload_lock);
}

static void request_retry_or_report(void) {
    char path[TARGET_CAPACITY], physical[MAX_PATH];
    EnterCriticalSection(&reload_lock);
    snprintf(path, sizeof(path), "%s", last_auto_target);
    snprintf(physical, sizeof(physical), "%s", last_auto_physical);
    LeaveCriticalSection(&reload_lock);
    if (*path && *physical) {
        hot_reload_file_changed(path, physical);
        set_result("Ctrl+F6: retrying the last changed asset.");
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
        set_result("Ctrl+F6: target is too long (259 byte limit).");
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
        set_result("Ctrl+F6: expected .v3o or a texture extension.");
        return;
    }
    EnterCriticalSection(&reload_lock);
    if (manual_pending) {
        LeaveCriticalSection(&reload_lock);
        set_result("Ctrl+F6: a reload is already queued.");
        return;
    }
    snprintf(manual_target, sizeof(manual_target), "%s", start);
    snprintf(display_target, sizeof(display_target), "%s", start);
    snprintf(display_result, sizeof(display_result), "Ctrl+F6: waiting for the next game frame...");
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

typedef struct ModelSwap {
    void *entity;
    const char *previous_name;
} ModelSwap;

static int append_model_alias(char *out, size_t capacity, const char *name,
                              unsigned generation) {
    size_t length = strlen(name);
    int written;
    if (length >= 4 && _stricmp(name + length - 4, ".v3o") == 0)
        length -= 4;
    written = snprintf(out, capacity, "%.*s.__em4debug_%04u_%04u.v3o",
        (int)length, name, (unsigned)(GetCurrentProcessId() % 10000),
        generation);
    return written > 0 && (size_t)written < capacity;
}

static void reload_model(const char *target, const char *physical, int automatic,
                         HMODULE vision, void *manager, ResourceCountFn resource_count,
                         ResourceAtFn resource_at, ModelFilenameFn model_filename) {
    const char *prefix = automatic ? "AUTO" : "Ctrl+F6";
    void *original = NULL, *loaded, *game, *table_export;
    ModelAlias *alias;
    ModelSwap *swaps = NULL;
    LoadModelFn load_model;
    ModelSizeFn vertex_count;
    EntityModelFn entity_model;
    SetModelFn set_model;
    const char *original_name;
    char logical_alias[TARGET_CAPACITY], normalized_alias[TARGET_CAPACITY];
    char physical_alias[MAX_PATH], message[160];
    char original_path[TARGET_CAPACITY], prior_alias_path[TARGET_CAPACITY];
    DWORD attributes;
    FILE *source_file;
    char header[512];
    size_t header_size;
    const char *vertex_header, *polygon_header;
    unsigned i, found = 0, entity_count, entity_matches = 0, applied = 0;
    int count;
    uintptr_t *table;
    void **entities;

    if (!physical || !*physical || !ends_with(physical, ".v3o")) {
        set_result("Model reload needs a changed file from the automatic watcher.");
        return;
    }
    attributes = GetFileAttributesA(physical);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        set_result("Model reload: changed .v3o is unavailable on disk.");
        return;
    }
    source_file = fopen(physical, "rb");
    if (!source_file) {
        set_result("Model reload: changed .v3o is still being written.");
        return;
    }
    header_size = fread(header, 1, sizeof(header) - 1, source_file);
    fclose(source_file);
    header[header_size] = 0;
    vertex_header = strstr(header, "[NUMVERTICES=");
    polygon_header = strstr(header, "[NUMPOLYGONS=");
    if (!strstr(header, "[VNUM=") || !vertex_header || !polygon_header ||
        atoi(vertex_header + 13) <= 0 ||
        atoi(polygon_header + 13) <= 0) {
        set_result("Model reload: .v3o header or geometry count is invalid.");
        return;
    }
    count = resource_count(manager);
    if (count < 0 || count > 200000) {
        set_result("Model reload: invalid resource count.");
        return;
    }
    for (i = 0; i < (unsigned)count; ++i) {
        void *resource = resource_at(manager, (int)i);
        const char *name = resource ? model_filename((char *)resource - 8) : NULL;
        if (name && same_resource_name(name, target)) {
            original = (char *)resource - 8;
            ++found;
        }
    }
    if (found != 1 || !original) {
        snprintf(message, sizeof(message), "%s: model resource match count is %u.",
            prefix, found);
        set_result(message);
        return;
    }
    original_name = model_filename(original);
    alias = find_model_alias(target);
    if (alias && !alias->current_name[0]) alias = NULL;
    if (!original_name || strlen(original_name) >= sizeof(original_path)) {
        set_result("Model reload: loaded model path is too long.");
        return;
    }
    snprintf(original_path, sizeof(original_path), "%s", original_name);
    prior_alias_path[0] = 0;
    if (alias) {
        snprintf(prior_alias_path, sizeof(prior_alias_path), "%s",
            alias->current_name);
    }
    if (!alias && model_alias_count == 32) {
        set_result("Model reload: model tracking limit reached.");
        return;
    }
    load_model = (LoadModelFn)GetProcAddress(vision,
        "?LoadModelFile@VisGame_cl@@QAEPAVVisModelInterface_cl@@PBDH@Z");
    vertex_count = (ModelSizeFn)GetProcAddress(vision,
        "?GetNumOfVertices@VisModelInterface_cl@@QBEHXZ");
    entity_model = (EntityModelFn)GetProcAddress(vision,
        "?GetModelInterface@VisBaseEntity_cl@@QBEPAVVisModelInterface_cl@@XZ");
    set_model = (SetModelFn)GetProcAddress(vision,
        "?SetModel@VisBaseEntity_cl@@QAEHPBDPAVVisVector_cl@@@Z");
    game = GetProcAddress(vision, "?Game@Vision@@2VVisGame_cl@@A");
    table_export = GetProcAddress(vision,
        "?elementTable@?$VisElementManager_cl@PAVVisBaseEntity_cl@@@@0V?$DynArray_cl@PAVVisBaseEntity_cl@@@@A");
    if (!original_name || !load_model || !vertex_count || !entity_model ||
        !set_model || !game || !table_export) {
        set_result("Model reload: engine API is unavailable.");
        return;
    }
    table = (uintptr_t *)table_export;
    entity_count = (unsigned)table[1];
    entities = (void **)table[2];
    if (entity_count > 100000 || (entity_count && !entities)) {
        set_result("Model reload: invalid entity table.");
        return;
    }
    if (entity_count) {
        swaps = (ModelSwap *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            (SIZE_T)entity_count * sizeof(*swaps));
        if (!swaps) {
            set_result("Model reload: cannot allocate entity list.");
            return;
        }
    }
    for (i = 0; i < entity_count; ++i) {
        void *entity = entities[i];
        void *current = entity ? entity_model(entity) : NULL;
        const char *current_name = current ? model_filename(current) : NULL;
        if (current == original || (alias &&
            same_resource_name(current_name, alias->current_name))) {
            swaps[entity_matches].entity = entity;
            swaps[entity_matches].previous_name = current == original ?
                original_path : prior_alias_path;
            ++entity_matches;
        }
    }
    if (model_generation >= 9999 ||
        !append_model_alias(logical_alias, sizeof(logical_alias), original_name,
                            ++model_generation) ||
        !append_model_alias(physical_alias, sizeof(physical_alias), physical,
                            model_generation)) {
        if (swaps) HeapFree(GetProcessHeap(), 0, swaps);
        set_result("Model reload: alias path is too long.");
        return;
    }
    snprintf(normalized_alias, sizeof(normalized_alias), "%s", logical_alias);
    normalized_alias[strlen(normalized_alias) - 4] = 0;
    if (!CopyFileA(physical, physical_alias, TRUE)) {
        if (swaps) HeapFree(GetProcessHeap(), 0, swaps);
        set_result("Model reload: could not copy the changed .v3o.");
        return;
    }
    loaded = load_model(game, normalized_alias, 1);
    if (!loaded || vertex_count(loaded) <= 0) {
        if (swaps) HeapFree(GetProcessHeap(), 0, swaps);
        snprintf(message, sizeof(message), "%s: the new .v3o did not load.", prefix);
        set_result(message);
        return;
    }
    for (i = 0; i < entity_matches; ++i) {
        void *current;
        const char *current_name;
        int changed = set_model(swaps[i].entity, logical_alias, NULL);
        current = entity_model(swaps[i].entity);
        current_name = current ? model_filename(current) : NULL;
        if (!changed || !same_resource_name(current_name, logical_alias)) {
            snprintf(message, sizeof(message),
                "Model swap check: SetModel=%d, loaded=%s, target=%s",
                changed, current_name ? basename_of(current_name) : "(none)",
                basename_of(logical_alias));
            if (reload_host) reload_host->log("debug", message);
            break;
        }
        ++applied;
    }
    if (applied != entity_matches) {
        unsigned j;
        for (j = 0; j < entity_matches; ++j)
            if (swaps[j].previous_name)
                set_model(swaps[j].entity, swaps[j].previous_name, NULL);
        snprintf(message, sizeof(message),
            "%s: model swap failed at entity %u/%u; restored prior models.",
            prefix, applied + 1, entity_matches);
        set_result(message);
    } else {
        if (!alias && model_alias_count < 32) {
            alias = &model_aliases[model_alias_count++];
            snprintf(alias->source, sizeof(alias->source), "%s", target);
        }
        if (alias) {
            snprintf(alias->original_name, sizeof(alias->original_name),
                "%s", original_path);
            snprintf(alias->current_name, sizeof(alias->current_name),
                "%s", logical_alias);
            alias->future_swaps_disabled = 0;
        }
        if (applied) snprintf(message, sizeof(message),
            "%s: updated %u live entity model(s) from %s; future spawns enabled.",
            prefix, applied, basename_of(target));
        else snprintf(message, sizeof(message),
            "%s: prepared %s for future spawns.",
            prefix, basename_of(target));
        EnterCriticalSection(&reload_lock);
        ++reload_count;
        update_watch_status();
        LeaveCriticalSection(&reload_lock);
        set_result(message);
    }
    if (swaps) HeapFree(GetProcessHeap(), 0, swaps);
}

static void reload_resource(const char *target, const char *physical, int automatic) {
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
    if (is_model) {
        reload_model(target, physical, automatic, vision, *model_manager_ptr,
            resource_count, resource_at, model_filename);
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
        automatic ? "AUTO" : "Ctrl+F6", is_model ? "model" : "texture");
    else snprintf(message, sizeof(message), "%s: reloaded %d/%d %s resource(s).",
        automatic ? "AUTO" : "Ctrl+F6", success, matches,
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
    char path[TARGET_CAPACITY], physical[MAX_PATH];
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
    snprintf(physical, sizeof(physical), "%s", changes[selected].physical);
    memmove(changes + selected, changes + selected + 1,
        (change_count - selected - 1) * sizeof(changes[0]));
    --change_count;
    snprintf(last_auto_target, sizeof(last_auto_target), "%s", path);
    snprintf(last_auto_physical, sizeof(last_auto_physical), "%s", physical);
    snprintf(display_target, sizeof(display_target), "%s", path);
    update_watch_status();
    LeaveCriticalSection(&reload_lock);
    reload_resource(path, physical, 1);
}

static void update_future_models(void) {
    HMODULE vision;
    ModelFilenameFn model_filename;
    EntityModelFn entity_model;
    SetModelFn set_model;
    uintptr_t *table;
    void **entities;
    unsigned entity_count, i, j, updated = 0;
    char message[160];
    if (!model_alias_count) return;
    vision = GetModuleHandleA("vision71.dll");
    if (!vision || !known_engine_module(vision, 0x43fb75cb, 0x291000))
        return;
    model_filename = (ModelFilenameFn)GetProcAddress(vision,
        "?GetModelFileName@VisModelInterface_cl@@QBEPBDXZ");
    entity_model = (EntityModelFn)GetProcAddress(vision,
        "?GetModelInterface@VisBaseEntity_cl@@QBEPAVVisModelInterface_cl@@XZ");
    set_model = (SetModelFn)GetProcAddress(vision,
        "?SetModel@VisBaseEntity_cl@@QAEHPBDPAVVisVector_cl@@@Z");
    table = (uintptr_t *)GetProcAddress(vision,
        "?elementTable@?$VisElementManager_cl@PAVVisBaseEntity_cl@@@@0V?$DynArray_cl@PAVVisBaseEntity_cl@@@@A");
    if (!model_filename || !entity_model || !set_model || !table) return;
    entity_count = (unsigned)table[1];
    entities = (void **)table[2];
    if (entity_count > 100000 || (entity_count && !entities)) return;
    for (i = 0; i < entity_count; ++i) {
        void *entity = entities[i];
        void *model = entity ? entity_model(entity) : NULL;
        const char *name = model ? model_filename(model) : NULL;
        if (!name) continue;
        for (j = 0; j < model_alias_count; ++j) {
            ModelAlias *alias = &model_aliases[j];
            if (alias->future_swaps_disabled || !alias->current_name[0] ||
                !same_resource_name(name, alias->original_name)) continue;
            if (set_model(entity, alias->current_name, NULL)) {
                model = entity_model(entity);
                name = model ? model_filename(model) : NULL;
                if (same_resource_name(name, alias->current_name)) {
                    ++updated;
                    break;
                }
            }
            set_model(entity, alias->original_name, NULL);
            alias->future_swaps_disabled = 1;
            snprintf(message, sizeof(message),
                "Future spawn model swap failed for %s; retry after editing the file.",
                basename_of(alias->original_name));
            set_result(message);
            break;
        }
    }
    if (updated) {
        snprintf(message, sizeof(message),
            "AUTO: applied updated model to %u newly spawned entity(s).", updated);
        set_result(message);
    }
}

void hot_reload_on_frame(void) {
    char target[TARGET_CAPACITY], physical[MAX_PATH] = "";
    process_auto_change();
    update_future_models();
    EnterCriticalSection(&reload_lock);
    if (!manual_pending) {
        LeaveCriticalSection(&reload_lock);
        return;
    }
    snprintf(target, sizeof(target), "%s", manual_target);
    if (same_resource_name(target, last_auto_target))
        snprintf(physical, sizeof(physical), "%s", last_auto_physical);
    InterlockedExchange(&manual_pending, 2);
    LeaveCriticalSection(&reload_lock);
    reload_resource(target, physical, 0);
    InterlockedExchange(&manual_pending, 0);
}

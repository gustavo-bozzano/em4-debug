#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "unit_inspector.h"

#define ENGINE_TIMESTAMP 0x457425fd
#define ENGINE_IMAGE_SIZE 0xa08000
#define COMMAND_VTABLE_RVA 0x5e5eec
#define WRAPPER_VTABLE_RVA 0x603c2c
#define NODE_LIMIT 1024
#define WORLD_NODE_LIMIT 32768
#define CHECK_LIMIT 256

typedef unsigned char (__attribute__((thiscall)) *CommandPossibleFn)(void *, void *);
typedef unsigned char (__attribute__((thiscall)) *IsSelectedFn)(void *);

typedef struct CommandCheck {
    void *actor;
    void *command;
    DWORD time;
    unsigned char possible;
} CommandCheck;

static const EM4HostAPI *inspector_host;
static CRITICAL_SECTION inspector_lock;
static UnitInspectorSnapshot current;
static CommandCheck checks[CHECK_LIMIT];
static unsigned next_check;
static DWORD last_capture;
static DWORD last_query_read;
static char query_path[MAX_PATH];
static char query_name[80];
#ifndef EM4_EXPERIMENTAL_SCRIPT_RELOAD
static CommandPossibleFn original_possible;
static int hook_ready;
#endif

static int readable(const void *address, SIZE_T size) {
    MEMORY_BASIC_INFORMATION info;
    uintptr_t begin = (uintptr_t)address;
    if (!address || !size || begin > UINTPTR_MAX - size ||
        !VirtualQuery(address, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return begin >= (uintptr_t)info.BaseAddress &&
        begin + size <= (uintptr_t)info.BaseAddress + info.RegionSize;
}

static int game_string(void *object, unsigned offset, char *out, size_t capacity) {
    const char *source, *end;
    MEMORY_BASIC_INFORMATION info;
    SIZE_T available;
    if (!readable((char *)object + offset, 8)) return 0;
    source = *(const char **)((char *)object + offset + 4);
    if (!source || !VirtualQuery(source, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    available = (uintptr_t)info.BaseAddress + info.RegionSize - (uintptr_t)source;
    if (available > capacity) available = capacity;
    end = (const char *)memchr(source, 0, available);
    if (!end) return 0;
    memcpy(out, source, (size_t)(end - source));
    out[end - source] = 0;
    return 1;
}

static int verified_engine(uintptr_t base) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt;
    if (!readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < 0 || dos->e_lfanew > 4096) return 0;
    nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    return readable(nt, sizeof(*nt)) && nt->Signature == IMAGE_NT_SIGNATURE &&
        nt->FileHeader.TimeDateStamp == ENGINE_TIMESTAMP &&
        nt->OptionalHeader.SizeOfImage == ENGINE_IMAGE_SIZE;
}

void unit_inspector_observe(void *command, void *actor, unsigned char possible) {
    unsigned i, slot = CHECK_LIMIT;
    if (!command || !actor || !readable((char *)actor + 0x28, 4) ||
        *(unsigned *)((char *)actor + 0x28) != 8) return;
    EnterCriticalSection(&inspector_lock);
    for (i = 0; i < CHECK_LIMIT; ++i) {
        if (checks[i].actor == actor && checks[i].command == command) {
            slot = i;
            break;
        }
    }
    if (slot == CHECK_LIMIT) slot = next_check++ % CHECK_LIMIT;
    checks[slot].actor = actor;
    checks[slot].command = command;
    checks[slot].possible = possible;
    checks[slot].time = GetTickCount();
    LeaveCriticalSection(&inspector_lock);
}

#ifndef EM4_EXPERIMENTAL_SCRIPT_RELOAD
static unsigned char __attribute__((thiscall))
inspector_possible(void *command, void *actor) {
    unsigned char result = original_possible(command, actor);
    unit_inspector_observe(command, actor, result);
    return result;
}

static void install_hook(uintptr_t base) {
    void **slot = (void **)(base + COMMAND_VTABLE_RVA + 6 * sizeof(void *));
    DWORD old_protection, ignored;
    if (hook_ready || !readable(slot, sizeof(void *)) ||
        *slot != (void *)(base + 0x1e3130) ||
        !VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE,
                        &old_protection)) return;
    original_possible = (CommandPossibleFn)*slot;
    InterlockedExchangePointer((PVOID volatile *)slot, inspector_possible);
    VirtualProtect(slot, sizeof(void *), old_protection, &ignored);
    hook_ready = 1;
    if (inspector_host) inspector_host->log("debug", "Unit command inspector armed.");
}
#endif

static int selected_entity(uintptr_t base, void **out, unsigned *out_count) {
    /* The verified build's GetSelectedGameObjects path walks world+0x24 and
       calls GameObject::IsSelected for each object. Follow the same read-only
       path, but keep only vehicles for this panel. */
    void *world = *(void **)(base + 0x693f18);
    void *stack[WORLD_NODE_LIMIT];
    unsigned top = 0, visited = 0, count = 0;
    IsSelectedFn is_selected = (IsSelectedFn)(base + 0x32570);
    if (!readable(world, 0x30)) return 0;
    if (*(void **)((char *)world + 0x28))
        stack[top++] = *(void **)((char *)world + 0x28);
    while (top) {
        void *node = stack[--top], *entity;
        if (++visited > WORLD_NODE_LIMIT || !readable(node, 0x1c)) return 0;
        if (*(void **)((char *)node + 4)) {
            if (top == WORLD_NODE_LIMIT) return 0;
            stack[top++] = *(void **)((char *)node + 4);
        }
        if (*(void **)((char *)node + 8)) {
            if (top == WORLD_NODE_LIMIT) return 0;
            stack[top++] = *(void **)((char *)node + 8);
        }
        if (*(unsigned char *)((char *)node + 0x18)) continue;
        entity = *(void **)((char *)node + 0x10);
        if (!readable(entity, 0x1bc) || !readable(*(void **)entity, 0x54) ||
            *(uintptr_t *)((char *)*(void **)entity + 0x50) != base + 0x324c0)
            continue;
        if (*(unsigned *)((char *)entity + 0x28) != 8) continue;
        if (!is_selected(entity)) continue;
        if (!count) *out = entity;
        ++count;
    }
    *out_count = count;
    return 1;
}

static int manager_has_command(uintptr_t base, void *target, const char *name) {
    void *manager = *(void **)(base + 0x694c24);
    void *stack[NODE_LIMIT];
    unsigned top = 0, visited = 0, matching_name = 0;
    if (!readable(manager, 0x10)) return 0;
    if (*(void **)((char *)manager + 8))
        stack[top++] = *(void **)((char *)manager + 8);
    while (top) {
        void *node = stack[--top], *command;
        char current_name[80];
        if (++visited > NODE_LIMIT || !readable(node, 0x20)) return 0;
        command = *(void **)((char *)node + 0x1c);
        if (target && command == target) return 2;
        if (readable(command, 0xec) &&
            game_string(command, 0xe4, current_name, sizeof(current_name)) &&
            _stricmp(current_name, name) == 0) matching_name = 1;
        if (*(void **)((char *)node + 4)) {
            if (top == NODE_LIMIT) return 0;
            stack[top++] = *(void **)((char *)node + 4);
        }
        if (*(void **)((char *)node + 8)) {
            if (top == NODE_LIMIT) return 0;
            stack[top++] = *(void **)((char *)node + 8);
        }
    }
    return matching_name ? 1 : 0;
}

static void read_query(DWORD now) {
    HANDLE file;
    DWORD bytes = 0;
    char text[sizeof(query_name)];
    unsigned i;
    if (now - last_query_read < 1000) return;
    last_query_read = now;
    query_name[0] = 0;
    if (!query_path[0]) return;
    file = CreateFileA(query_path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    if (ReadFile(file, text, sizeof(text) - 1, &bytes, NULL) && bytes) {
        text[bytes] = 0;
        for (i = 0; i < bytes; ++i)
            if (text[i] == '\r' || text[i] == '\n') { text[i] = 0; break; }
        if (text[0] && text[0] != '#')
            snprintf(query_name, sizeof(query_name), "%s", text);
    }
    CloseHandle(file);
}

static void command_status(void *entity, void *command, char *out, size_t size) {
    DWORD now = GetTickCount();
    unsigned i;
    EnterCriticalSection(&inspector_lock);
    for (i = 0; i < CHECK_LIMIT; ++i) {
        if (checks[i].actor != entity || checks[i].command != command) continue;
        if (now - checks[i].time <= 10000) {
            snprintf(out, size, "%s", checks[i].possible ? "Available" : "CheckPossible denied");
            LeaveCriticalSection(&inspector_lock);
            return;
        }
        break;
    }
    LeaveCriticalSection(&inspector_lock);
    snprintf(out, size, "Not checked recently");
}

static void append_command(UnitInspectorSnapshot *snapshot, uintptr_t base,
                           void *entity, void *command, unsigned group) {
    UnitCommandRow *row;
    void *wrapper, *script;
    int registered;
    ++snapshot->command_count;
    if (snapshot->shown_count == UNIT_INSPECTOR_ROWS) return;
    if (!readable(command, 0xec) ||
        *(uintptr_t *)command != base + COMMAND_VTABLE_RVA) return;
    row = &snapshot->rows[snapshot->shown_count];
    if (!game_string(command, 0xe4, row->name, sizeof(row->name))) return;
    row->group = group;
    wrapper = *(void **)((char *)command + 8);
    if (readable(wrapper, 0x18) &&
        *(uintptr_t *)wrapper == base + WRAPPER_VTABLE_RVA) {
        script = *(void **)((char *)wrapper + 0x14);
        if (!game_string(script, 0x34, row->script, sizeof(row->script)))
            snprintf(row->script, sizeof(row->script), "Script path unavailable");
    } else {
        snprintf(row->script, sizeof(row->script), "Engine or unknown wrapper");
    }
    registered = manager_has_command(base, command, row->name);
    if (registered == 2) command_status(entity, command, row->status, sizeof(row->status));
    else if (registered == 1)
        snprintf(row->status, sizeof(row->status), "Different manager instance");
    else snprintf(row->status, sizeof(row->status), "Missing from command manager");
    ++snapshot->shown_count;
}

static int capture_commands(UnitInspectorSnapshot *snapshot, uintptr_t base,
                            void *entity) {
    void *root = *(void **)((char *)entity + 0x1b8);
    void *stack[NODE_LIMIT];
    unsigned top = 0, visited = 0;
    if (!root) return 1;
    if (!readable(root, 0x24)) return 0;
    stack[top++] = root;
    while (top) {
        void *node = stack[--top], *item;
        unsigned item_count = 0;
        if (++visited > NODE_LIMIT || !readable(node, 0x24)) return 0;
        if (*(void **)((char *)node + 4)) {
            if (top == NODE_LIMIT) return 0;
            stack[top++] = *(void **)((char *)node + 4);
        }
        if (*(void **)((char *)node + 8)) {
            if (top == NODE_LIMIT) return 0;
            stack[top++] = *(void **)((char *)node + 8);
        }
        if (*(unsigned char *)((char *)node + 0x20)) continue;
        item = *(void **)((char *)node + 0x10);
        while (item) {
            if (++item_count > NODE_LIMIT || !readable(item, 8)) return 0;
            append_command(snapshot, base, entity,
                *(void **)((char *)item + 4),
                *(unsigned *)((char *)node + 0x0c));
            item = *(void **)item;
        }
    }
    return 1;
}

void unit_inspector_init(const EM4HostAPI *host) {
    char executable[MAX_PATH], *slash;
    inspector_host = host;
    InitializeCriticalSection(&inspector_lock);
    snprintf(current.summary, sizeof(current.summary), "Waiting for a selected vehicle.");
    if (GetModuleFileNameA(NULL, executable, sizeof(executable))) {
        slash = strrchr(executable, '\\');
        if (slash) {
            slash[1] = 0;
            snprintf(query_path, sizeof(query_path),
                "%sEM4Plugins\\inspect_command.txt", executable);
        }
    }
}

void unit_inspector_on_frame(void) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    DWORD now = GetTickCount();
    UnitInspectorSnapshot next;
    void *entity = NULL;
    unsigned count = 0;
    if (now - last_capture < 250) return;
    last_capture = now;
    read_query(now);
    memset(&next, 0, sizeof(next));
    if (!verified_engine(base)) {
        snprintf(next.summary, sizeof(next.summary), "Unsupported Em4.exe build.");
    } else {
#ifndef EM4_EXPERIMENTAL_SCRIPT_RELOAD
        install_hook(base);
#endif
        if (!selected_entity(base, &entity, &count)) {
            snprintf(next.summary, sizeof(next.summary), "Selection data unavailable.");
        } else if (!count) {
            snprintf(next.summary, sizeof(next.summary), "Select a vehicle in the game.");
        } else {
            next.selected_count = count;
            snprintf(next.summary, sizeof(next.summary),
                "Vehicle %08lx | %u selected", (unsigned long)(uintptr_t)entity, count);
            if (!capture_commands(&next, base, entity))
                snprintf(next.summary, sizeof(next.summary), "Command list could not be read safely.");
            if (query_name[0]) {
                unsigned i;
                for (i = 0; i < next.shown_count; ++i)
                    if (_stricmp(next.rows[i].name, query_name) == 0) break;
                if (i < next.shown_count)
                    snprintf(next.query_result, sizeof(next.query_result),
                        "Find %s: assigned | %s", query_name, next.rows[i].status);
                else if (manager_has_command(base, NULL, query_name))
                    snprintf(next.query_result, sizeof(next.query_result),
                        "Find %s: registered, not assigned to this vehicle", query_name);
                else snprintf(next.query_result, sizeof(next.query_result),
                        "Find %s: not registered in command manager", query_name);
            }
        }
    }
    EnterCriticalSection(&inspector_lock);
    current = next;
    LeaveCriticalSection(&inspector_lock);
}

void unit_inspector_copy(UnitInspectorSnapshot *out) {
    EnterCriticalSection(&inspector_lock);
    *out = current;
    LeaveCriticalSection(&inspector_lock);
}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#ifdef EM4_EXPERIMENTAL_SCRIPT_RELOAD
#include <dbghelp.h>
#endif
#include "hot_reload.h"
#include "unit_inspector.h"

#define TARGET_CAPACITY 260
#define CHANGE_CAPACITY 64
#define SCRIPT_CAPACITY 256
#define COMMAND_BINDING_CAPACITY 256
#define ENTITY_REF_CAPACITY 8192
#define WORLD_NODE_CAPACITY 32768

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
static char changed_script[TARGET_CAPACITY];
static char changed_script_physical[MAX_PATH];
static volatile LONG script_reload_requested;
static int script_reload_in_progress;
static unsigned script_command_count;

typedef struct ScriptEntry {
    void *object;
    unsigned id;
    unsigned state;
    char path[TARGET_CAPACITY];
} ScriptEntry;

typedef struct CommandBinding {
    void *object;
    void *slot;
    int in_tree;
    void *wrapper;
    void *script;
    char path[TARGET_CAPACITY];
    char name[TARGET_CAPACITY];
} CommandBinding;

static CommandBinding script_command_bindings[COMMAND_BINDING_CAPACITY];
#ifdef EM4_EXPERIMENTAL_SCRIPT_RELOAD
static CommandBinding new_command_bindings[COMMAND_BINDING_CAPACITY];
#endif
static int known_engine_module(HMODULE module, DWORD timestamp, DWORD image_size);

#ifdef EM4_EXPERIMENTAL_SCRIPT_RELOAD
typedef struct NativeCommandName {
    void *object;
    char name[TARGET_CAPACITY];
} NativeCommandName;

typedef struct EntityCommandRef {
    void *item;
    void *old_command;
    char name[TARGET_CAPACITY];
} EntityCommandRef;

static NativeCommandName native_command_names[COMMAND_BINDING_CAPACITY];
static EntityCommandRef entity_command_refs[ENTITY_REF_CAPACITY];
static void *world_nodes[WORLD_NODE_CAPACITY];

/* Diagnostic only: record what the engine returns when it tests commands for
   the in-game panel. The hook delegates to the original function unchanged. */
typedef unsigned char (__attribute__((thiscall)) *CommandPossibleFn)(void *, void *);
static CommandPossibleFn original_command_possible;
static const char *const command_probe_names[] = {
    "VCmdTeste", "VCmdSiren", "VCmdAutoSirenOff", "VcmdFloodLightsOn",
    "MoveTo", "GoHome", "EmptyCar"
};
static volatile LONG command_probe_allowed[7], command_probe_denied[7];
static LONG command_probe_last_allowed[7], command_probe_last_denied[7];
static DWORD command_probe_last_report;
static volatile LONG command_probe_ready;

static unsigned char __attribute__((thiscall))
command_possible_probe(void *command, void *actor) {
    unsigned char result = original_command_possible(command, actor);
    const char *name;
    unsigned i;
    unit_inspector_observe(command, actor, result);
    if (!command || !actor || *(unsigned *)((char *)actor + 0x28) != 8)
        return result;
    name = *(const char **)((char *)command + 0xe8);
    if (!name) return result;
    for (i = 0; i < sizeof(command_probe_names) / sizeof(command_probe_names[0]); ++i) {
        if (_stricmp(name, command_probe_names[i]) == 0) {
            InterlockedIncrement(result ? &command_probe_allowed[i] :
                                        &command_probe_denied[i]);
            break;
        }
    }
    return result;
}

static void command_probe_install(uintptr_t base) {
    void **slot = (void **)(base + 0x5e5eec + 6 * sizeof(void *));
    DWORD old_protect, ignored;
    if (command_probe_ready ||
        !known_engine_module((HMODULE)base, 0x457425fd, 0xa08000) ||
        *slot != (void *)(base + 0x1e3130) ||
        !VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE,
                        &old_protect)) return;
    original_command_possible = (CommandPossibleFn)*slot;
    InterlockedExchangePointer((PVOID volatile *)slot, command_possible_probe);
    VirtualProtect(slot, sizeof(void *), old_protect, &ignored);
    command_probe_ready = 1;
    if (reload_host) reload_host->log("debug", "Command availability probe armed.");
}

static void command_probe_report(void) {
    DWORD now = GetTickCount();
    unsigned i;
    if (!command_probe_ready || now - command_probe_last_report < 5000) return;
    command_probe_last_report = now;
    for (i = 0; i < sizeof(command_probe_names) / sizeof(command_probe_names[0]); ++i) {
        LONG allowed = command_probe_allowed[i], denied = command_probe_denied[i];
        char line[180];
        if (allowed == command_probe_last_allowed[i] &&
            denied == command_probe_last_denied[i]) continue;
        command_probe_last_allowed[i] = allowed;
        command_probe_last_denied[i] = denied;
        snprintf(line, sizeof(line),
            "Command check %s: allowed=%ld denied=%ld reloads=%u",
            command_probe_names[i], allowed, denied, reload_count);
        if (reload_host) reload_host->log("debug", line);
    }
}
#endif

typedef void *(__attribute__((cdecl)) *SchedulerFn)(void);
typedef int (__attribute__((thiscall)) *LoadCommandScriptFn)(void *, const WCHAR *, int, int);


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

#ifdef EM4_EXPERIMENTAL_SCRIPT_RELOAD
typedef void (WINAPI *ProbeExitProcessFn)(UINT);
typedef void (WINAPI *ProbePostQuitMessageFn)(int);
typedef void (__cdecl *ProbeCrtExitFn)(int);
typedef void (__cdecl *ProbeAbortFn)(void);
static ProbeExitProcessFn probe_exit_process;
static ProbePostQuitMessageFn probe_post_quit_message;
static ProbeCrtExitFn probe_exit, probe_quick_exit;
static ProbeAbortFn probe_abort;
static volatile LONG probe_installed;
static volatile LONG probe_logging;
static volatile LONG probe_exception_logged;

static void probe_log_exit(const char *kind, unsigned code) {
    void *frames[12];
    USHORT count, i;
    char message[512];
    int used;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (InterlockedCompareExchange(&probe_logging, 1, 0) != 0) return;
    count = CaptureStackBackTrace(1, 12, frames, NULL);
    used = snprintf(message, sizeof(message), "Exit probe: %s code=%u stack=", kind, code);
    for (i = 0; i < count && used > 0 && (size_t)used < sizeof(message) - 16; ++i) {
        uintptr_t address = (uintptr_t)frames[i];
        used += snprintf(message + used, sizeof(message) - used,
            " %08lx", (unsigned long)(address >= base && address < base + 0xa08000 ?
                address - base : address));
    }
    if (reload_host) reload_host->log("debug", message);
    InterlockedExchange(&probe_logging, 0);
}

static LONG CALLBACK probe_exception(PEXCEPTION_POINTERS info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         code == EXCEPTION_INT_DIVIDE_BY_ZERO) &&
        InterlockedCompareExchange(&probe_exception_logged, 1, 0) == 0) {
        char message[720];
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        uintptr_t address = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
        CONTEXT *context = info->ContextRecord;
        DWORD *stack = (DWORD *)(uintptr_t)context->Esp;
        MEMORY_BASIC_INFORMATION memory;
        unsigned i;
        int used = snprintf(message, sizeof(message),
            "Exception probe: code=%08lx at=%08lx exe_rva=%08lx eip=%08lx esp=%08lx ebp=%08lx eax=%08lx ecx=%08lx edx=%08lx access=%lu target=%08lx stack=",
            (unsigned long)code, (unsigned long)address,
            (unsigned long)(address >= base && address < base + 0xa08000 ?
                address - base : 0), (unsigned long)context->Eip,
            (unsigned long)context->Esp, (unsigned long)context->Ebp,
            (unsigned long)context->Eax, (unsigned long)context->Ecx,
            (unsigned long)context->Edx,
            (unsigned long)(info->ExceptionRecord->NumberParameters > 0 ?
                info->ExceptionRecord->ExceptionInformation[0] : 0),
            (unsigned long)(info->ExceptionRecord->NumberParameters > 1 ?
                info->ExceptionRecord->ExceptionInformation[1] : 0));
        if (VirtualQuery(stack, &memory, sizeof(memory)) &&
            memory.State == MEM_COMMIT &&
            !(memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (uintptr_t)stack + 24 * sizeof(DWORD) <=
                (uintptr_t)memory.BaseAddress + memory.RegionSize) {
            for (i = 0; i < 24 && used > 0 &&
                 (size_t)used < sizeof(message) - 16; ++i)
                used += snprintf(message + used, sizeof(message) - used,
                    " %08lx", (unsigned long)stack[i]);
        }
        if (reload_host) reload_host->log("debug", message);
        {
            char directory[MAX_PATH], dump_path[MAX_PATH + 48], notice[MAX_PATH + 96];
            HANDLE file;
            MINIDUMP_EXCEPTION_INFORMATION exception_info;
            DWORD length = GetTempPathA(sizeof(directory), directory);
            if (length && length < sizeof(directory)) {
                snprintf(dump_path, sizeof(dump_path),
                    "%sem4-script-reload-%lu.dmp", directory,
                    (unsigned long)GetCurrentProcessId());
                file = CreateFileA(dump_path, GENERIC_WRITE, FILE_SHARE_READ,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (file != INVALID_HANDLE_VALUE) {
                    exception_info.ThreadId = GetCurrentThreadId();
                    exception_info.ExceptionPointers = info;
                    exception_info.ClientPointers = FALSE;
                    if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                            file, MiniDumpNormal | MiniDumpWithThreadInfo,
                            &exception_info, NULL, NULL)) {
                        snprintf(notice, sizeof(notice),
                            "Exception probe: dump saved to %s", dump_path);
                        if (reload_host) reload_host->log("debug", notice);
                    }
                    CloseHandle(file);
                }
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void WINAPI probe_on_exit_process(UINT code) {
    probe_log_exit("ExitProcess", code);
    probe_exit_process(code);
}
static void WINAPI probe_on_post_quit_message(int code) {
    probe_log_exit("PostQuitMessage", (unsigned)code);
    probe_post_quit_message(code);
}
static void __cdecl probe_on_exit(int code) {
    probe_log_exit("exit", (unsigned)code);
    probe_exit(code);
}
static void __cdecl probe_on_quick_exit(int code) {
    probe_log_exit("_exit", (unsigned)code);
    probe_quick_exit(code);
}
static void __cdecl probe_on_abort(void) {
    probe_log_exit("abort", 0);
    probe_abort();
}

static int probe_hook_import(uintptr_t base, uintptr_t rva,
                             const char *module_name, const char *function_name,
                             void *replacement, void **original) {
    HMODULE module = GetModuleHandleA(module_name);
    void **cell = (void **)(base + rva);
    DWORD old_protect, ignored;
    void *expected = module ? (void *)GetProcAddress(module, function_name) : NULL;
    if (!expected || *cell != expected ||
        !VirtualProtect(cell, sizeof(*cell), PAGE_READWRITE, &old_protect))
        return 0;
    *original = *cell;
    InterlockedExchangePointer((PVOID volatile *)cell, replacement);
    VirtualProtect(cell, sizeof(*cell), old_protect, &ignored);
    return 1;
}

static void probe_install(void) {
    HMODULE exe = GetModuleHandleA(NULL);
    uintptr_t base = (uintptr_t)exe;
    unsigned hooked = 0;
    char message[96];
    if (InterlockedCompareExchange(&probe_installed, 1, 0) != 0 ||
        !known_engine_module(exe, 0x457425fd, 0xa08000)) return;
    AddVectoredExceptionHandler(1, probe_exception);
    hooked += probe_hook_import(base, 0x5d2124, "kernel32.dll", "ExitProcess",
        probe_on_exit_process, (void **)&probe_exit_process);
    hooked += probe_hook_import(base, 0x5d25a0, "user32.dll", "PostQuitMessage",
        probe_on_post_quit_message, (void **)&probe_post_quit_message);
    hooked += probe_hook_import(base, 0x5d2210, "msvcr71.dll", "exit",
        probe_on_exit, (void **)&probe_exit);
    hooked += probe_hook_import(base, 0x5d23f8, "msvcr71.dll", "_exit",
        probe_on_quick_exit, (void **)&probe_quick_exit);
    hooked += probe_hook_import(base, 0x5d23b8, "msvcr71.dll", "abort",
        probe_on_abort, (void **)&probe_abort);
    snprintf(message, sizeof(message), "Script reload exit probe armed: %u/5 imports hooked.", hooked);
    if (reload_host) reload_host->log("debug", message);
}
#endif

static int readable_range(const void *address, SIZE_T length) {
    MEMORY_BASIC_INFORMATION info;
    uintptr_t start = (uintptr_t)address;
    uintptr_t end;
    if (!address || length == 0 || start > UINTPTR_MAX - length ||
        !VirtualQuery(address, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    end = (uintptr_t)info.BaseAddress + info.RegionSize;
    return start >= (uintptr_t)info.BaseAddress && start + length <= end;
}

static int script_string(void *script, unsigned offset, char *out, size_t capacity) {
    const char *source;
    const char *terminator;
    MEMORY_BASIC_INFORMATION info;
    SIZE_T available;
    if (!readable_range((char *)script + offset, 8)) return 0;
    source = *(const char **)((char *)script + offset + 4);
    if (!source || !VirtualQuery(source, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    available = (uintptr_t)info.BaseAddress + info.RegionSize - (uintptr_t)source;
    if (available > capacity) available = capacity;
    terminator = (const char *)memchr(source, 0, available);
    if (!terminator) return 0;
    memcpy(out, source, (size_t)(terminator - source));
    out[terminator - source] = 0;
    return 1;
}

static int script_entries(void *scheduler, ScriptEntry *entries, unsigned *out_count) {
    void *node;
    unsigned count = 0;
    if (!readable_range(scheduler, 8)) return 0;
    node = *(void **)((char *)scheduler + 4);
    while (node) {
        void *script;
        if (count == SCRIPT_CAPACITY || !readable_range(node, 8)) return 0;
        script = *(void **)((char *)node + 4);
        if (!readable_range(script, 0x44)) return 0;
        entries[count].object = script;
        entries[count].id = *(unsigned *)((char *)script + 4);
        entries[count].state = *(unsigned *)((char *)script + 12);
        if (!script_string(script, 0x34, entries[count].path,
                           sizeof(entries[count].path))) return 0;
        ++count;
        node = *(void **)node;
    }
    *out_count = count;
    return 1;
}

static int script_is_command(const char *path) {
    if (_strnicmp(path, "mod:/", 5) == 0 ||
        _strnicmp(path, "base:/", 6) == 0)
        path = strchr(path, '/') + 1;
    return _strnicmp(path, "scripts/game/command/", 21) == 0 ||
        _strnicmp(path, "scripts\\game\\command\\", 21) == 0;
}

static int script_is_mission(const char *path) {
    if (_strnicmp(path, "mod:/", 5) == 0 ||
        _strnicmp(path, "base:/", 6) == 0)
        path = strchr(path, '/') + 1;
    return _strnicmp(path, "scripts/game/mission/", 21) == 0 ||
        _strnicmp(path, "scripts\\game\\mission\\", 21) == 0;
}

/* The verified Steam build keeps command objects in a tree and a list.
   Each command owns a script-call wrapper; wrapper+0x14 points at the
   scheduler's Script object. Entity command lists hold additional native
   command pointers, so rebinding these wrappers alone is insufficient and
   the older wrapper-only experiment is known to crash. These offsets must not
   be used on another executable build. */
static int capture_command_binding(void *object, void *slot, int in_tree,
                                   const ScriptEntry *entries,
                                   unsigned script_count, CommandBinding *bindings,
                                   unsigned *binding_count, uintptr_t base) {
    void *wrapper, *script;
    unsigned i;
    if (!readable_range(object, 0xe4 + 8) ||
        *(uintptr_t *)object != base + 0x5e5eec) return 0;
    wrapper = *(void **)((char *)object + 8);
    if (!readable_range(wrapper, 0x20) ||
        *(uintptr_t *)wrapper != base + 0x603c2c) return 0;
    script = *(void **)((char *)wrapper + 0x14);
    for (i = 0; i < script_count; ++i)
        if (entries[i].object == script) break;
    if (i == script_count || *binding_count == COMMAND_BINDING_CAPACITY)
        return 0;
    bindings[*binding_count].object = object;
    bindings[*binding_count].slot = slot;
    bindings[*binding_count].in_tree = in_tree;
    bindings[*binding_count].wrapper = wrapper;
    bindings[*binding_count].script = script;
    snprintf(bindings[*binding_count].path, TARGET_CAPACITY, "%s",
        entries[i].path);
    if (!script_string(object, 0xe4, bindings[*binding_count].name,
                       TARGET_CAPACITY)) return 0;
    ++*binding_count;
    return 1;
}

static int capture_command_bindings(const ScriptEntry *entries,
                                    unsigned script_count,
                                    CommandBinding *bindings,
                                    unsigned *binding_count,
                                    uintptr_t base) {
    void *manager, *stack[COMMAND_BINDING_CAPACITY * 2];
    void *node, *seen[COMMAND_BINDING_CAPACITY];
    unsigned top = 0, visited = 0, i;
    manager = *(void **)(base + 0x694c24);
    if (!readable_range(manager, 0x28)) return 0;
    *binding_count = 0;
    node = *(void **)((char *)manager + 8);
    if (node) stack[top++] = node;
    while (top) {
        node = stack[--top];
        for (i = 0; i < visited; ++i) if (seen[i] == node) break;
        if (i < visited) continue;
        if (visited == COMMAND_BINDING_CAPACITY ||
            !readable_range(node, 0x28)) return 0;
        seen[visited++] = node;
        if (*(void **)((char *)node + 0x1c) &&
            !capture_command_binding(*(void **)((char *)node + 0x1c),
                (char *)node + 0x1c, 1,
                entries, script_count, bindings, binding_count, base)) return 0;
        if (*(void **)((char *)node + 4)) {
            if (top == COMMAND_BINDING_CAPACITY * 2) return 0;
            stack[top++] = *(void **)((char *)node + 4);
        }
        if (*(void **)((char *)node + 8)) {
            if (top == COMMAND_BINDING_CAPACITY * 2) return 0;
            stack[top++] = *(void **)((char *)node + 8);
        }
    }
    node = *(void **)((char *)manager + 0x14);
    visited = 0;
    while (node) {
        if (visited++ == COMMAND_BINDING_CAPACITY ||
            !readable_range(node, 8)) return 0;
        if (*(void **)((char *)node + 4) &&
            !capture_command_binding(*(void **)((char *)node + 4),
                (char *)node + 4, 0,
                entries, script_count, bindings, binding_count, base)) return 0;
        node = *(void **)node;
    }
    return *binding_count != 0;
}

static int rebind_command_scripts(const CommandBinding *before,
                                  unsigned before_count,
                                  const CommandBinding *after,
                                  unsigned after_count,
                                  const char *target,
                                  void *new_script,
                                  uintptr_t base,
                                  unsigned *rebound) {
    typedef void *(__attribute__((thiscall)) *GameStringAssignFn)(void *,
                                                                  const void *);
    void *replacement_wrappers[COMMAND_BINDING_CAPACITY];
    unsigned i, j, matching = 0;
    *rebound = 0;
    /* The old native commands stay alive, preserving references held by
       active entities and vehicle prototypes. Validate all replacements
       before rebinding any wrapper. */
    for (i = 0; i < before_count; ++i) {
        void *wrapper;
        if (!same_resource_name(before[i].path, target)) continue;
        ++matching;
        wrapper = before[i].wrapper;
        if (!readable_range(wrapper, 0x20) ||
            *(uintptr_t *)wrapper != base + 0x603c2c ||
            *(void **)((char *)wrapper + 0x14) != before[i].script)
            return 0;
        for (j = 0; j < after_count; ++j)
            if (after[j].script == new_script &&
                _stricmp(before[i].name, after[j].name) == 0) break;
        if (j == after_count) return 0;
        replacement_wrappers[i] = after[j].wrapper;
        if (!readable_range((char *)wrapper + 4, 0x10) ||
            !readable_range((char *)after[j].wrapper + 4, 0x10) ||
            *(void **)((char *)wrapper + 4) !=
                *(void **)((char *)after[j].wrapper + 4) ||
            !readable_range(*(void **)((char *)wrapper + 4), 0x58))
            return 0;
    }
    if (!matching) return 0;
    for (i = 0; i < before_count; ++i) {
        void *name, *source;
        void **vtable;
        GameStringAssignFn assign;
        if (!same_resource_name(before[i].path, target)) continue;
        name = (char *)before[i].wrapper + 4;
        source = (char *)replacement_wrappers[i] + 4;
        vtable = *(void ***)name;
        assign = (GameStringAssignFn)vtable[0x54 / sizeof(void *)];
        assign(name, source);
        InterlockedExchangePointer((PVOID volatile *)
            ((char *)before[i].wrapper + 0x14), new_script);
        ++*rebound;
    }
    return 1;
}

#ifdef EM4_EXPERIMENTAL_SCRIPT_RELOAD
/* Loading a duplicate command moves the previous native object from the
   command-name tree to the manager's secondary list. Existing vehicles still
   refer to that previous object, while newly spawned vehicles resolve the
   new tree object. Put the previous object back in the tree and move the new
   object into its list slot, preserving one manager slot for each object. */
static int restore_command_tree_bindings(const CommandBinding *before,
                                         unsigned before_count,
                                         const CommandBinding *after,
                                         unsigned after_count,
                                         const char *target,
                                         void *new_script,
                                         unsigned *restored) {
    void *tree_slots[COMMAND_BINDING_CAPACITY];
    void *list_slots[COMMAND_BINDING_CAPACITY];
    void *old_objects[COMMAND_BINDING_CAPACITY];
    void *new_objects[COMMAND_BINDING_CAPACITY];
    unsigned i, j, k, count = 0;

    *restored = 0;
    for (i = 0; i < before_count; ++i) {
        if (!before[i].in_tree ||
            !same_resource_name(before[i].path, target)) continue;
        for (j = 0; j < after_count; ++j) {
            if (after[j].in_tree && after[j].script == new_script &&
                _stricmp(after[j].name, before[i].name) == 0) break;
        }
        for (k = 0; k < after_count; ++k) {
            if (!after[k].in_tree &&
                after[k].object == before[i].object) break;
        }
        if (j == after_count || k == after_count ||
            !readable_range(after[j].slot, sizeof(void *)) ||
            !readable_range(after[k].slot, sizeof(void *)) ||
            *(void **)after[j].slot != after[j].object ||
            *(void **)after[k].slot != before[i].object)
            return 0;
        tree_slots[count] = after[j].slot;
        list_slots[count] = after[k].slot;
        old_objects[count] = before[i].object;
        new_objects[count] = after[j].object;
        ++count;
    }
    if (!count) return 0;
    for (i = 0; i < count; ++i) {
        InterlockedExchangePointer((PVOID volatile *)list_slots[i],
                                   new_objects[i]);
        InterlockedExchangePointer((PVOID volatile *)tree_slots[i],
                                   old_objects[i]);
        ++*restored;
    }
    return 1;
}

/* The command-manager tree is the authoritative name lookup. Its secondary
   list contains older commands with duplicate names, so it cannot be used
   for name-based migration. */
static int native_command_tree(uintptr_t base, unsigned *out_count) {
    void *manager, *stack[COMMAND_BINDING_CAPACITY * 2];
    unsigned top = 0, count = 0, i;
    manager = *(void **)(base + 0x694c24);
    if (!readable_range(manager, 0x28)) return 0;
    if (*(void **)((char *)manager + 8))
        stack[top++] = *(void **)((char *)manager + 8);
    while (top) {
        void *node = stack[--top], *command;
        char name[TARGET_CAPACITY];
        if (count == COMMAND_BINDING_CAPACITY || !readable_range(node, 0x28))
            return 0;
        command = *(void **)((char *)node + 0x1c);
        if (!readable_range(command, 0xe4 + 8) ||
            *(uintptr_t *)command != base + 0x5e5eec ||
            !script_string(command, 0xe4, name, sizeof(name))) return 0;
        for (i = 0; i < count; ++i)
            if (_stricmp(native_command_names[i].name, name) == 0) return 0;
        native_command_names[count].object = command;
        snprintf(native_command_names[count].name, TARGET_CAPACITY, "%s", name);
        ++count;
        if (*(void **)((char *)node + 4)) {
            if (top == COMMAND_BINDING_CAPACITY * 2) return 0;
            stack[top++] = *(void **)((char *)node + 4);
        }
        if (*(void **)((char *)node + 8)) {
            if (top == COMMAND_BINDING_CAPACITY * 2) return 0;
            stack[top++] = *(void **)((char *)node + 8);
        }
    }
    *out_count = count;
    return count != 0;
}

static int capture_entity_command_refs(uintptr_t base, unsigned native_count,
                                       unsigned *out_count) {
    void *world, *root;
    unsigned world_top = 0, world_seen = 0, ref_count = 0;
    world = *(void **)(base + 0x693f18);
    if (!readable_range(world, 0x1c)) return 0;
    root = *(void **)((char *)world + 8);
    if (!root) return 0;
    world_nodes[world_top++] = root;
    while (world_top) {
        void *world_node = world_nodes[--world_top], *entity;
        void *command_nodes[COMMAND_BINDING_CAPACITY * 2];
        unsigned command_top = 0, command_seen = 0;
        uintptr_t vtable;
        if (++world_seen > WORLD_NODE_CAPACITY ||
            !readable_range(world_node, 0x1c)) return 0;
        if (*(void **)((char *)world_node + 4)) {
            if (world_top == WORLD_NODE_CAPACITY) return 0;
            world_nodes[world_top++] = *(void **)((char *)world_node + 4);
        }
        if (*(void **)((char *)world_node + 8)) {
            if (world_top == WORLD_NODE_CAPACITY) return 0;
            world_nodes[world_top++] = *(void **)((char *)world_node + 8);
        }
        entity = *(void **)((char *)world_node + 0x10);
        if (!readable_range(entity, 0x1bc)) continue;
        vtable = *(uintptr_t *)entity;
        if (vtable < base + 0x500000 || vtable >= base + 0x680000 ||
            !readable_range((void *)(vtable + 0x50), 4) ||
            *(uintptr_t *)(vtable + 0x50) != base + 0x324c0)
            continue;
        root = *(void **)((char *)entity + 0x1b8);
        if (!root || !readable_range(root, 0x24)) continue;
        command_nodes[command_top++] = root;
        while (command_top) {
            void *node = command_nodes[--command_top], *item;
            unsigned item_seen = 0;
            if (++command_seen > COMMAND_BINDING_CAPACITY * 2 ||
                !readable_range(node, 0x24)) return 0;
            if (*(void **)((char *)node + 4)) {
                if (command_top == COMMAND_BINDING_CAPACITY * 2) return 0;
                command_nodes[command_top++] = *(void **)((char *)node + 4);
            }
            if (*(void **)((char *)node + 8)) {
                if (command_top == COMMAND_BINDING_CAPACITY * 2) return 0;
                command_nodes[command_top++] = *(void **)((char *)node + 8);
            }
            if (*(unsigned char *)((char *)node + 0x20)) continue;
            item = *(void **)((char *)node + 0x10);
            while (item) {
                void *command;
                unsigned i;
                if (++item_seen > COMMAND_BINDING_CAPACITY * 2 ||
                    !readable_range(item, 8)) return 0;
                command = *(void **)((char *)item + 4);
                if (readable_range(command, 4) &&
                    *(uintptr_t *)command == base + 0x5e5eec) {
                    for (i = 0; i < native_count; ++i)
                        if (native_command_names[i].object == command) break;
                    if (i == native_count || ref_count == ENTITY_REF_CAPACITY)
                        return 0;
                    entity_command_refs[ref_count].item = item;
                    entity_command_refs[ref_count].old_command = command;
                    snprintf(entity_command_refs[ref_count].name,
                        TARGET_CAPACITY, "%s", native_command_names[i].name);
                    ++ref_count;
                }
                item = *(void **)item;
            }
        }
    }
    *out_count = ref_count;
    return 1;
}

static int migrate_entity_command_refs(uintptr_t base, unsigned ref_count,
                                       const ScriptEntry *scripts,
                                       unsigned script_count,
                                       unsigned *out_migrated) {
    void *new_commands[ENTITY_REF_CAPACITY];
    unsigned native_count = 0, i, j, k;
    *out_migrated = 0;
    if (!native_command_tree(base, &native_count)) return 0;
    for (i = 0; i < ref_count; ++i) {
        void *wrapper, *script;
        if (!readable_range(entity_command_refs[i].item, 8) ||
            *(void **)((char *)entity_command_refs[i].item + 4) !=
                entity_command_refs[i].old_command) return 0;
        for (j = 0; j < native_count; ++j)
            if (_stricmp(entity_command_refs[i].name,
                         native_command_names[j].name) == 0) break;
        if (j == native_count) return 0;
        new_commands[i] = native_command_names[j].object;
        wrapper = *(void **)((char *)new_commands[i] + 8);
        if (!readable_range(wrapper, 0x18) ||
            *(uintptr_t *)wrapper != base + 0x603c2c) return 0;
        script = *(void **)((char *)wrapper + 0x14);
        for (k = 0; k < script_count; ++k)
            if (scripts[k].object == script) break;
        if (k == script_count) return 0;
    }
    for (i = 0; i < ref_count; ++i) {
        InterlockedExchangePointer((PVOID volatile *)
            ((char *)entity_command_refs[i].item + 4), new_commands[i]);
        ++*out_migrated;
    }
    return 1;
}
#endif

void hot_reload_request_scripts(void) {
#if defined(EM4_EXPERIMENTAL_SCRIPT_RELOAD) && defined(EM4_ALLOW_UNSAFE_SCRIPT_RELOAD_TEST)
    probe_install();
    InterlockedExchange(&script_reload_requested, 1);
    set_result("Experimental command script update queued for the next frame.");
#elif defined(EM4_EXPERIMENTAL_SCRIPT_RELOAD)
    set_result("Command script update is disabled until an experimental build is installed.");
#else
    set_result("Script reload is disabled: the active session retains old commands. Restart the session to apply scripts.");
#endif
}

#ifdef EM4_EXPERIMENTAL_SCRIPT_RELOAD
/* Retain the old compiled file and native commands. The game's CINT
   interpreter cannot unload a command script compiled before later files.
   Retaining native commands also keeps entity and prototype references
   valid while their wrappers switch to the newly compiled Script. */
static unsigned experimental_script_loads;
static void script_reload_on_frame(void) {
    HMODULE exe = GetModuleHandleA(NULL);
    uintptr_t base = (uintptr_t)exe;
    SchedulerFn scheduler_fn;
    LoadCommandScriptFn load_command_script;
    void *scheduler;
    ScriptEntry entries[SCRIPT_CAPACITY];
    unsigned count = 0, old_count, i;
    unsigned before_bindings = 0, after_bindings = 0, target_bindings = 0;
    unsigned rebound = 0, restored = 0, newest_id = 0;
    char target[TARGET_CAPACITY], engine_target[TARGET_CAPACITY];
    char physical[MAX_PATH], message[160];
    WCHAR engine_target_wide[TARGET_CAPACITY];
    void *new_script = NULL;

    command_probe_install(base);
    command_probe_report();
    if (!script_reload_requested) return;
    InterlockedExchange(&script_reload_requested, 0);
    if (!exe || !known_engine_module(exe, 0x457425fd, 0xa08000)) {
        set_result("Command update requires the verified Steam Em4.exe build.");
        return;
    }
    if (experimental_script_loads >= 16) {
        set_result("Command update limit reached for this session. Restart before more updates.");
        return;
    }
    scheduler_fn = (SchedulerFn)(base + 0x0e3340);
    load_command_script = (LoadCommandScriptFn)(base + 0x34b150);
    scheduler = scheduler_fn();
    if (!scheduler || !script_entries(scheduler, entries, &count)) {
        set_result("Command update: scheduler inventory is unavailable.");
        return;
    }
    EnterCriticalSection(&reload_lock);
    snprintf(target, sizeof(target), "%s", changed_script);
    snprintf(physical, sizeof(physical), "%s", changed_script_physical);
    LeaveCriticalSection(&reload_lock);
    engine_target[0] = 0;
    if (!script_is_command(target) ||
        GetFileAttributesA(physical) == INVALID_FILE_ATTRIBUTES) {
        set_result("Command update: only saved Game/Command scripts are supported.");
        return;
    }
    if (count == 0 || count >= SCRIPT_CAPACITY ||
        !readable_range((char *)scheduler + 0x10, sizeof(unsigned)) ||
        *(unsigned *)((char *)scheduler + 0x10) != count) {
        set_result("Command update: scheduler count is invalid. No scripts were changed.");
        return;
    }
    for (i = 0; i < count; ++i) {
        if (same_resource_name(entries[i].path, target) &&
            entries[i].id > newest_id &&
            entries[i].state >= 1 && entries[i].state <= 3) {
            newest_id = entries[i].id;
            snprintf(engine_target, sizeof(engine_target), "%s", entries[i].path);
        }
    }
    if (!newest_id || !engine_target[0]) {
        set_result("Command update: the changed script is not active.");
        return;
    }
    if (!MultiByteToWideChar(CP_ACP, 0, engine_target, -1,
                             engine_target_wide, TARGET_CAPACITY)) {
        set_result("Command update: the active script path could not be converted.");
        return;
    }
    if (!capture_command_bindings(entries, count, script_command_bindings,
                                  &before_bindings, base)) {
        set_result("Command update: native command inventory is unavailable.");
        return;
    }
    for (i = 0; i < before_bindings; ++i)
        if (same_resource_name(script_command_bindings[i].path, target))
            ++target_bindings;
    if (!target_bindings) {
        set_result("Command update: no native command belongs to this script.");
        return;
    }
    old_count = count;
    script_reload_in_progress = 1;
    snprintf(message, sizeof(message), "Compiling %s while retaining active commands...",
             basename_of(target));
    set_result(message);
    if (!load_command_script(scheduler, engine_target_wide, 1, 0)) {
        set_result("Command update: compilation failed. Existing commands remain active.");
        script_reload_in_progress = 0;
        return;
    }
    ++experimental_script_loads;
    if (!script_entries(scheduler, entries, &count) ||
        count != old_count + 1 ||
        *(unsigned *)((char *)scheduler + 0x10) != count) {
        set_result("Command update: compilation completed, but the new script could not be identified.");
        script_reload_in_progress = 0;
        return;
    }
    for (i = 0; i < count; ++i) {
        if (!same_resource_name(entries[i].path, target) ||
            entries[i].id <= newest_id ||
            entries[i].state < 1 || entries[i].state > 3) continue;
        new_script = entries[i].object;
        break;
    }
    if (!new_script ||
        !capture_command_bindings(entries, count, new_command_bindings,
                                  &after_bindings, base)) {
        set_result("Command update: new command bindings could not be inspected.");
        script_reload_in_progress = 0;
        return;
    }
    if (!rebind_command_scripts(script_command_bindings, before_bindings,
                                new_command_bindings, after_bindings,
                                target, new_script, base, &rebound)) {
        set_result("Command update: class names changed or bindings are invalid; old commands remain.");
        script_reload_in_progress = 0;
        return;
    }
    if (!restore_command_tree_bindings(script_command_bindings,
                                       before_bindings,
                                       new_command_bindings,
                                       after_bindings,
                                       target, new_script, &restored)) {
        set_result("Command update: active vehicles updated, but future spawns still use the duplicate command.");
        script_reload_in_progress = 0;
        return;
    }
    snprintf(message, sizeof(message),
             "Recompiled %s; rebound %u command(s), restored %u spawn binding(s).",
             basename_of(target), rebound, restored);
    EnterCriticalSection(&reload_lock);
    ++reload_count;
    update_watch_status();
    LeaveCriticalSection(&reload_lock);
    set_result(message);
    script_reload_in_progress = 0;
}
#endif

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
    snprintf(display_target, sizeof(display_target), "%s", path);
    if (script_is_command(path) && ends_with(path, ".script")) {
        snprintf(changed_script, sizeof(changed_script), "%s", path);
        snprintf(changed_script_physical, sizeof(changed_script_physical),
            "%s", physical);
    } else if (!ends_with(path, ".script")) {
        snprintf(last_auto_target, sizeof(last_auto_target), "%s", path);
        snprintf(last_auto_physical, sizeof(last_auto_physical), "%s", physical);
    }
    update_watch_status();
    LeaveCriticalSection(&reload_lock);
    if (ends_with(path, ".script")) {
        if (!script_is_command(path))
            set_result("Script change ignored: only Game/Command scripts are in scope.");
#if defined(EM4_EXPERIMENTAL_SCRIPT_RELOAD) && defined(EM4_ALLOW_UNSAFE_SCRIPT_RELOAD_TEST)
        else
            set_result("Command script changed. Press Ctrl+F7 to recompile it in this session.");
#else
        else
            set_result("Command script changed. Live reload is disabled pending a safe implementation.");
#endif
        return;
    }
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
#ifdef EM4_EXPERIMENTAL_SCRIPT_RELOAD
    script_reload_on_frame();
#endif
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

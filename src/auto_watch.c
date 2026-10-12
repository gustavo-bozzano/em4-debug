#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "hot_reload.h"

typedef struct WatchRoot {
    char path[MAX_PATH];
    int is_mods;
} WatchRoot;

static WatchRoot roots[2];

static int supported_file(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!_stricmp(dot, ".v3o") || !_stricmp(dot, ".dds") ||
        !_stricmp(dot, ".tga") || !_stricmp(dot, ".png") ||
        !_stricmp(dot, ".bmp") || !_stricmp(dot, ".jpg") ||
        !_stricmp(dot, ".jpeg"));
}

static DWORD WINAPI watch_directory(LPVOID argument) {
    const WatchRoot *root = (const WatchRoot *)argument;
    HANDLE directory = CreateFileA(root->path, FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BYTE buffer[32768];
    if (directory == INVALID_HANDLE_VALUE) return 0;
    for (;;) {
        DWORD bytes = 0, offset = 0;
        if (!ReadDirectoryChangesW(directory, buffer, sizeof(buffer), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_SIZE, &bytes, NULL, NULL)) break;
        while (offset < bytes) {
            FILE_NOTIFY_INFORMATION *event = (FILE_NOTIFY_INFORMATION *)(buffer + offset);
            WCHAR wide[MAX_PATH];
            char relative[MAX_PATH];
            const char *logical;
            unsigned length = event->FileNameLength / sizeof(WCHAR);
            if (length && length < MAX_PATH) {
                memcpy(wide, event->FileName, event->FileNameLength);
                wide[length] = 0;
                if (WideCharToMultiByte(CP_ACP, 0, wide, -1, relative,
                    sizeof(relative), NULL, NULL)) {
                    logical = relative;
                    if (root->is_mods) {
                        const char *slash = strchr(relative, '\\');
                        logical = slash ? slash + 1 : "";
                    }
                    if ((event->Action == FILE_ACTION_ADDED ||
                         event->Action == FILE_ACTION_MODIFIED ||
                         event->Action == FILE_ACTION_RENAMED_NEW_NAME) &&
                        supported_file(logical) &&
                        !strstr(logical, ".__em4debug_")) {
                        char physical[MAX_PATH];
                        int written = snprintf(physical, sizeof(physical),
                            "%s\\%s", root->path, relative);
                        if (written > 0 && written < (int)sizeof(physical))
                            hot_reload_file_changed(logical, physical);
                    }
                }
            }
            if (!event->NextEntryOffset) break;
            offset += event->NextEntryOffset;
        }
    }
    CloseHandle(directory);
    return 0;
}

int hot_reload_watch_directories(const char *game_directory) {
    HANDLE thread;
    int started = 0, i;
    snprintf(roots[0].path, sizeof(roots[0].path), "%sData", game_directory);
    roots[0].is_mods = 0;
    snprintf(roots[1].path, sizeof(roots[1].path), "%sMods", game_directory);
    roots[1].is_mods = 1;
    for (i = 0; i < 2; ++i) {
        thread = CreateThread(NULL, 0, watch_directory, &roots[i], 0, NULL);
        if (thread) {
            CloseHandle(thread);
            ++started;
        }
    }
    return started;
}

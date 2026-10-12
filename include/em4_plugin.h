#ifndef EM4_PLUGIN_H
#define EM4_PLUGIN_H

#include <windows.h>

#define EM4_PLUGIN_API_VERSION 1u

typedef struct EM4HostAPI {
    unsigned int size;
    unsigned int version;
    HWND (WINAPI *get_game_window)(void);
    void (WINAPI *log)(const char *plugin, const char *message);
} EM4HostAPI;

/* The loader calls this once after DirectInput has initialized. */
typedef BOOL (WINAPI *EM4PluginInitFn)(const EM4HostAPI *host);

#endif

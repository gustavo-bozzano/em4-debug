#ifndef EM4_HOT_RELOAD_H
#define EM4_HOT_RELOAD_H

#include <windows.h>
#include "em4_plugin.h"

void hot_reload_init(const EM4HostAPI *host, const char *game_directory);
void hot_reload_request(void);
void hot_reload_request_scripts(void);
void hot_reload_on_frame(void);
void hot_reload_file_changed(const char *logical_path, const char *physical_path);
int hot_reload_watch_directories(const char *game_directory);
void hot_reload_get_display(char *target, unsigned target_size,
                            char *result, unsigned result_size,
                            char *watch_status, unsigned watch_status_size);

#endif

#ifndef EM4_PATH_VIEWER_H
#define EM4_PATH_VIEWER_H

#include "em4_plugin.h"

#define PATH_VIEWER_MAX_LABELS 256

typedef struct PathViewerLabel {
    char name[64];
    float x, y;
} PathViewerLabel;

void path_viewer_init(const EM4HostAPI *api);
void path_viewer_on_frame(void);
int path_viewer_is_enabled(void);
void path_viewer_draw_world(void);
void path_viewer_toggle(void);
unsigned path_viewer_copy_labels(PathViewerLabel *out, unsigned capacity);

#endif

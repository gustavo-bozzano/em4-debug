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
int path_viewer_camera_axes(float right[2], float forward[2]);
int path_viewer_camera_position(float position[3]);
int path_viewer_camera_forward(float forward[3]);
int path_viewer_camera_center_offset(const float target[3], float offset[2]);

#endif

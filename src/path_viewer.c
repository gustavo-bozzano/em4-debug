#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gl/gl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "path_viewer.h"

#define ENGINE_TIMESTAMP 0x457425fd
#define ENGINE_IMAGE_SIZE 0xa08000
#define VISION_TIMESTAMP 0x43fb75cb
#define VISION_IMAGE_SIZE 0x291000
#define WORLD_NODE_LIMIT 32768
#define PATH_LIMIT 256
#define POINT_LIMIT 1024

typedef struct PathPoint { float x, y, z; } PathPoint;
typedef struct PathRow {
    char name[64];
    unsigned count;
    PathPoint points[POINT_LIMIT];
} PathRow;

static const EM4HostAPI *path_host;
static PathRow paths[PATH_LIMIT];
static unsigned path_count;
static volatile LONG enabled;
static DWORD last_scan;
static int engine_ok;
static int camera_ready;
static int camera_logged;
static DWORD camera_tick;
static GLfloat camera_projection[16], camera_modelview[16];
static uintptr_t vision_base;
static PathViewerLabel labels[PATH_VIEWER_MAX_LABELS];
static unsigned label_count;

static int readable(const void *address, SIZE_T size) {
    MEMORY_BASIC_INFORMATION info;
    uintptr_t begin = (uintptr_t)address;
    if (!address || !size || begin > UINTPTR_MAX - size ||
        !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return begin >= (uintptr_t)info.BaseAddress &&
        begin + size <= (uintptr_t)info.BaseAddress + info.RegionSize;
}

static int verified_image(uintptr_t base, DWORD timestamp, DWORD image_size) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt;
    if (!readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < 0 || dos->e_lfanew > 4096) return 0;
    nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    return readable(nt, sizeof(*nt)) && nt->Signature == IMAGE_NT_SIGNATURE &&
        nt->FileHeader.TimeDateStamp == timestamp &&
        nt->OptionalHeader.SizeOfImage == image_size;
}

static void copy_name(void *path, char *out, size_t capacity) {
    const WCHAR *wide;
    size_t i;
    out[0] = 0;
    if (!readable((char *)path + 8, sizeof(void *))) return;
    wide = *(const WCHAR **)((char *)path + 8);
    if (!wide) return;
    for (i = 0; i + 1 < capacity; ++i) {
        WCHAR ch;
        if (!readable(wide + i, sizeof(WCHAR))) break;
        ch = wide[i];
        if (!ch) break;
        out[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    out[i] = 0;
}

static void add_path(void *object, uintptr_t base) {
    PathRow *row;
    void *node;
    unsigned expected, i;
    if (path_count >= PATH_LIMIT || !readable(object, 0x90) ||
        *(uintptr_t *)object != base + 0x5d77a0 ||
        *(unsigned *)((char *)object + 0x28) != 0x100) return;
    expected = *(unsigned *)((char *)object + 0x8c);
    node = *(void **)((char *)object + 0x84);
    if (!expected || expected > POINT_LIMIT || !node) return;
    row = &paths[path_count];
    memset(row, 0, sizeof(*row));
    copy_name(object, row->name, sizeof(row->name));
    if (!row->name[0]) snprintf(row->name, sizeof(row->name), "Path %u", path_count + 1);
    for (i = 0; i < expected; ++i) {
        PathPoint *point;
        if (!readable(node, 12)) break;
        point = *(PathPoint **)((char *)node + 4);
        if (!readable(point, sizeof(*point)) ||
            !(point->x > -1000000 && point->x < 1000000 &&
              point->y > -1000000 && point->y < 1000000 &&
              point->z > -1000000 && point->z < 1000000)) break;
        row->points[row->count++] = *point;
        node = *(void **)node;
        if (!node) break;
    }
    if (row->count < 2) return;
    ++path_count;
}

static void scan_paths(uintptr_t base) {
    void *world, *stack[WORLD_NODE_LIMIT];
    unsigned top = 0, visited = 0;
    path_count = 0;
    if (!engine_ok) return;
    if (!readable((void *)(base + 0x693f18), 4)) return;
    world = *(void **)(base + 0x693f18);
    if (!readable(world, 12)) return;
    if (*(void **)((char *)world + 8))
        stack[top++] = *(void **)((char *)world + 8);
    while (top && visited < WORLD_NODE_LIMIT) {
        void *node = stack[--top], *object;
        void *left, *right;
        if (!readable(node, 0x1c)) continue;
        ++visited;
        left = *(void **)((char *)node + 4);
        right = *(void **)((char *)node + 8);
        if (left && top < WORLD_NODE_LIMIT) stack[top++] = left;
        if (right && top < WORLD_NODE_LIMIT) stack[top++] = right;
        object = *(void **)((char *)node + 0x10);
        if (readable(object, 0x2c) &&
            *(unsigned *)((char *)object + 0x28) == 0x100)
            add_path(object, base);
    }
}

static void update_camera(void) {
    void *camera;
    const GLfloat *projection, *modelview;
    camera_ready = 0;
    if (!vision_base ||
        !readable((void *)(vision_base + 0x144a14), sizeof(void *))) return;
    camera = *(void **)(vision_base + 0x144a14);
    if (!camera) return;
    projection = (const GLfloat *)((char *)camera + 0xbc);
    modelview = (const GLfloat *)(vision_base + 0x137fb0);
    if (!readable(projection, sizeof(camera_projection)) ||
        !readable(modelview, sizeof(camera_modelview)) ||
        !(projection[0] > 0.00001f && projection[5] > 0.00001f &&
          projection[15] > -0.01f && projection[15] < 0.01f &&
          modelview[15] > 0.99f && modelview[15] < 1.01f)) return;
    memcpy(camera_projection, projection, sizeof(camera_projection));
    memcpy(camera_modelview, modelview, sizeof(camera_modelview));
    camera_tick = GetTickCount();
    camera_ready = 1;
}

void path_viewer_init(const EM4HostAPI *api) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    path_host = api;
    engine_ok = verified_image(base, ENGINE_TIMESTAMP, ENGINE_IMAGE_SIZE);
    vision_base = (uintptr_t)GetModuleHandleA("vision71.dll");
    if (!verified_image(vision_base, VISION_TIMESTAMP, VISION_IMAGE_SIZE))
        vision_base = 0;
    if (path_host) path_host->log("debug", engine_ok ?
        "Path viewer ready for verified Em4.exe." :
        "Path viewer disabled: unsupported Em4.exe build.");
    if (path_host) path_host->log("debug", vision_base ?
        "Path viewer found verified Vision camera matrices." :
        "Path viewer cannot locate verified Vision camera matrices.");
}

void path_viewer_on_frame(void) {
    DWORD now = GetTickCount();
    if (engine_ok) update_camera();
    if (!engine_ok || now - last_scan < 1000) return;
    last_scan = now;
    scan_paths((uintptr_t)GetModuleHandleA(NULL));
}

int path_viewer_is_enabled(void) { return enabled != 0; }

static void transform(const GLfloat *m, const float in[4], float out[4]) {
    unsigned i;
    for (i = 0; i < 4; ++i)
        out[i] = m[i] * in[0] + m[4 + i] * in[1] +
                 m[8 + i] * in[2] + m[12 + i] * in[3];
}

static int project_point(const PathPoint *point, const GLint viewport[4],
                         float *screen_x, float *screen_y) {
    float in[4] = { point->x, point->y, point->z + 15.0f, 1.0f };
    float eye[4], clip[4];
    transform(camera_modelview, in, eye);
    transform(camera_projection, eye, clip);
    if (!(clip[3] > 0.0f && clip[0] >= -clip[3] && clip[0] <= clip[3] &&
          clip[1] >= -clip[3] && clip[1] <= clip[3] &&
          clip[2] >= -clip[3] && clip[2] <= clip[3])) return 0;
    *screen_x = viewport[0] + (clip[0] / clip[3] + 1.0f) * viewport[2] * 0.5f;
    *screen_y = viewport[3] -
                (viewport[1] + (clip[1] / clip[3] + 1.0f) * viewport[3] * 0.5f);
    return 1;
}

static int overlaps_label(float x, float y, unsigned width) {
    unsigned i;
    for (i = 0; i < label_count; ++i) {
        unsigned other_width = (unsigned)strlen(labels[i].name) * 9;
        if (x < labels[i].x + other_width + 5 &&
            x + width + 5 > labels[i].x &&
            y < labels[i].y + 17 && y + 17 > labels[i].y) return 1;
    }
    return 0;
}

static void collect_labels(const GLint viewport[4]) {
    unsigned i;
    label_count = 0;
    for (i = 0; i < path_count && label_count < PATH_VIEWER_MAX_LABELS; ++i) {
        const PathRow *row = &paths[i];
        unsigned j, first = POINT_LIMIT, last = 0, target, width, attempt;
        float x = 0, y = 0, label_x, label_y;
        for (j = 0; j < row->count; ++j) {
            if (project_point(&row->points[j], viewport, &x, &y)) {
                if (first == POINT_LIMIT) first = j;
                last = j;
            }
        }
        if (first == POINT_LIMIT) continue;
        target = (first + last) / 2;
        for (j = 0; j < row->count; ++j) {
            unsigned before = target >= j ? target - j : POINT_LIMIT;
            unsigned after = target + j;
            if (before < row->count &&
                project_point(&row->points[before], viewport, &x, &y)) break;
            if (after < row->count &&
                project_point(&row->points[after], viewport, &x, &y)) break;
        }
        width = (unsigned)strlen(row->name) * 9;
        label_x = x - width * 0.5f;
        if (label_x < 4) label_x = 4;
        if (label_x + width > viewport[2] - 4)
            label_x = (float)(viewport[2] - 4 - width);
        if (label_x < 4) label_x = 4;
        label_y = y - 10;
        if (label_y < 18) label_y = 18;
        if (label_y > viewport[3] - 6) label_y = (float)(viewport[3] - 6);
        for (attempt = 0; attempt < 8 &&
             overlaps_label(label_x, label_y, width); ++attempt) {
            float offset = (float)((attempt / 2 + 1) * 18);
            float candidate = label_y + (attempt & 1 ? -offset : offset);
            if (candidate >= 18 && candidate <= viewport[3] - 6 &&
                !overlaps_label(label_x, candidate, width)) {
                label_y = candidate;
                break;
            }
        }
        snprintf(labels[label_count].name, sizeof(labels[label_count].name),
                 "%s", row->name);
        labels[label_count].x = label_x;
        labels[label_count].y = label_y;
        ++label_count;
    }
}

void path_viewer_draw_world(void) {
    GLint mode, viewport[4];
    unsigned i, j;
    label_count = 0;
    if (!enabled || !path_count) return;
    if (!camera_ready || GetTickCount() - camera_tick > 500) return;
    if (!camera_logged && path_host) {
        char line[180];
        snprintf(line, sizeof(line), "Path camera captured: projection %.3f, %.3f; model translation %.1f, %.1f, %.1f",
                 camera_projection[0], camera_projection[5],
                 camera_modelview[12], camera_modelview[13], camera_modelview[14]);
        path_host->log("debug", line);
        camera_logged = 1;
    }
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] > 0 && viewport[3] > 0) collect_labels(viewport);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(camera_projection);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadMatrixf(camera_modelview);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_TEXTURE_1D);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    glLineWidth(3.0f);
    for (i = 0; i < path_count; ++i) {
        glColor3f(0.25f, 0.85f, 1.0f);
        glBegin(GL_LINE_STRIP);
        for (j = 0; j < paths[i].count; ++j)
            glVertex3f(paths[i].points[j].x, paths[i].points[j].y,
                       paths[i].points[j].z + 15.0f);
        glEnd();
    }
    glPointSize(6.0f);
    glColor3f(1.0f, 0.78f, 0.2f);
    glBegin(GL_POINTS);
    for (i = 0; i < path_count; ++i)
        for (j = 0; j < paths[i].count; ++j)
            glVertex3f(paths[i].points[j].x, paths[i].points[j].y,
                       paths[i].points[j].z + 15.0f);
    glEnd();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(mode);
    glPopAttrib();
}

void path_viewer_toggle(void) {
    InterlockedExchange(&enabled, !enabled);
    if (path_host) path_host->log("debug", enabled ?
        "Path rendering enabled." : "Path rendering disabled.");
}

unsigned path_viewer_copy_labels(PathViewerLabel *out, unsigned capacity) {
    unsigned count = label_count < capacity ? label_count : capacity;
    if (out && count) memcpy(out, labels, count * sizeof(*out));
    return count;
}

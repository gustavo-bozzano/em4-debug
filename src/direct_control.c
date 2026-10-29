#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "direct_control.h"
#include "unit_inspector.h"
#include "path_viewer.h"

/* Verified Steam Em4.exe. Movement routines operate on a native GameObject. */
#define MOVE_DIRECT_RVA 0x53140
#define CLEAR_ACTIONS_RVA 0x32a20
#define CREATE_OBJECT_RVA 0x4b9820
#define SET_OBJECT_POSITION_RVA 0x4fd90
#define MOVE_FOLLOW_RVA 0x467ed0
#define DELETE_OWNER_RVA 0x101f40
#define CHECK_COLLISION_RVA 0x3f0f0
#define GET_GROUND_HEIGHT_RVA 0x4b8820
#define SET_ROTATION_RVA 0x4fdb0
#define SET_CAMERA_RVA 0x4bd970
#define LOOK_AT_RVA 0x4bdbc0
#define ACTION_NEWLIST 4
#define ACTION_REPLACEFIRST 5
#define VEHICLE_DISTANCE 1050.0f
#define VEHICLE_FOLLOW_DISTANCE 750.0f
#define VEHICLE_CALIBRATE_MS 650
#define VEHICLE_CALIBRATE_DISTANCE 35.0f
#define VEHICLE_MAX_SPEED 330.0f
#define VEHICLE_REVERSE_SPEED 150.0f
#define VEHICLE_ACCELERATION 520.0f
#define VEHICLE_TURN_RAD_PER_SEC 1.15f
#define VEHICLE_MIN_STEP_SECONDS 0.012f
#define PERSON_DISTANCE 140.0f
#define VEHICLE_STEER 0.36f
#define VEHICLE_LOOK_HEIGHT 120.0f
#define PERSON_LOOK_HEIGHT 70.0f
#define HEADING_SAMPLE_MS 90
#define CAMERA_FOCUS_MS 85.0f
#define CAMERA_POSITION_MS 165.0f
#define CAMERA_TURN_MS 210.0f
#define CAMERA_MAX_TURN_RAD_PER_MS 0.00314f
#define VEHICLE_REPLAN_NEAR_DISTANCE 700.0f
#define VEHICLE_REPLAN_MIN_MS 900
#define VEHICLE_REPLAN_STUCK_MS 8000
#define PERSON_REPLAN_MS 350
#define SELECTION_CHECK_MS 120

typedef void (__attribute__((thiscall)) *MoveDirectFn)(void *, int,
                                                        const float *, int);
typedef void (__attribute__((thiscall)) *ClearActionsFn)(void *);
typedef void (__cdecl *CreateObjectFn)(void *, const char *, const char *);
typedef void (__attribute__((thiscall)) *SetObjectPositionFn)(void *, const float *);
typedef void (__attribute__((thiscall)) *MoveFollowFn)(void *, int, void *, float, float);
typedef void (__attribute__((thiscall)) *DeleteOwnerFn)(void *, int);
typedef void (__attribute__((thiscall)) *WrapObjectFn)(void *, void *);
typedef unsigned char (__attribute__((thiscall)) *CheckCollisionFn)(void *, const float *);
typedef float (__cdecl *GetGroundHeightFn)(float, float);
typedef void (__attribute__((thiscall)) *SetRotationFn)(void *, const float *);
typedef void (__cdecl *SetCameraFn)(const float *);
typedef void (__cdecl *LookAtFn)(const float *, const float *);
typedef void (WINAPI *SetInputMaskFn)(BOOL);
typedef BOOL (WINAPI *InputReadyFn)(void);

static const EM4HostAPI *control_host;
static volatile LONG toggle_requests;
static int active;
static int moving;
static int vehicle_follow_mode;
#ifdef EM4_MANUAL_VEHICLE
static int vehicle_manual_mode;
static DWORD vehicle_calibrate_at;
static float vehicle_calibrate_position[2];
static float vehicle_manual_heading[2];
static float vehicle_initial_heading[2];
static float vehicle_initial_rotation[9];
static float vehicle_manual_speed;
static float vehicle_last_rotation_angle;
static LARGE_INTEGER vehicle_manual_counter;
#ifdef EM4_VEHICLE_MOTION_TRACE
static double vehicle_cost_ticks[4];
static unsigned vehicle_cost_samples;
#endif
#endif
static int follow_unavailable;
static uintptr_t follow_target_wrapper[4];
static void *follow_target_native;
static unsigned follow_target_sequence;
static void *unit;
static unsigned unit_type;
static int camera_following;
static int camera_follow_logged;
static float chase_heading[2];
static float desired_heading[2];
static float chase_camera[3];
static float chase_focus[3];
static float heading_sample_position[3];
static float original_camera_offset[3];
static float original_camera_forward[3];
static float chase_back;
static float chase_height;
static float chase_look_height;
static DWORD last_camera_tick;
static DWORD last_heading_sample;
static LARGE_INTEGER camera_frequency;
static LARGE_INTEGER last_camera_counter;
static SetInputMaskFn set_input_mask;
static InputReadyFn input_ready;
static unsigned last_keys;
static DWORD last_plan;
static float last_vehicle_target[2];
static int last_vehicle_target_valid;
#ifdef EM4_VEHICLE_MOTION_TRACE
static DWORD last_vehicle_trace;
static float trace_position[2];
#endif
static DWORD last_selection_check;
static DWORD status_at;
static char status_text[112];

static float clamp_float(float value, float lower, float upper) {
    if (value < lower) return lower;
    if (value > upper) return upper;
    return value;
}

static void log_status(const char *message) {
    snprintf(status_text, sizeof(status_text), "%s", message);
    status_at = GetTickCount();
    if (control_host) control_host->log("control", message);
}

static void release_follow_target(void) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (follow_target_native &&
        unit_inspector_entity_valid(follow_target_native, ~0u))
        ((DeleteOwnerFn)(base + DELETE_OWNER_RVA))(
            follow_target_native, ACTION_NEWLIST);
    memset(follow_target_wrapper, 0, sizeof(follow_target_wrapper));
    follow_target_native = NULL;
    vehicle_follow_mode = 0;
}

static int position_follow_target(const float target[4]) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    char name[48];
    if (!follow_target_native ||
        !unit_inspector_entity_valid(follow_target_native, ~0u)) {
        memset(follow_target_wrapper, 0, sizeof(follow_target_wrapper));
        follow_target_native = NULL;
        if (follow_unavailable) return 0;
        snprintf(name, sizeof(name), "EM4_WASD_TARGET_%u", ++follow_target_sequence);
        ((CreateObjectFn)(base + CREATE_OBJECT_RVA))(
            follow_target_wrapper,
            "mod:Prototypes/Objects/Misc/empty.e4p", name);
        follow_target_native = (void *)follow_target_wrapper[3];
        if (!follow_target_native ||
            !unit_inspector_entity_valid(follow_target_native, ~0u)) {
            follow_target_native = NULL;
            follow_unavailable = 1;
            if (control_host)
                control_host->log("control", "Vehicle follow target unavailable; using path movement.");
            return 0;
        }
        *(unsigned char *)((char *)follow_target_native + 0x100) = 0;
        if (control_host)
            control_host->log("control", "Vehicle follow target created.");
    }
    ((SetObjectPositionFn)(base + SET_OBJECT_POSITION_RVA))(
        follow_target_native, target);
    return 1;
}

static void start_vehicle_follow(void) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t unit_wrapper[4] = {0};
    ((WrapObjectFn)(base + 0x4632c0))(unit_wrapper, unit);
    ((MoveFollowFn)(base + MOVE_FOLLOW_RVA))(
        unit_wrapper, ACTION_NEWLIST, follow_target_wrapper, 50.0f, 20.0f);
    vehicle_follow_mode = 1;
    if (control_host)
        control_host->log("control", "Vehicle moving target follow action started.");
}

#ifdef EM4_MANUAL_VEHICLE
#ifdef EM4_VEHICLE_MOTION_TRACE
static void report_vehicle_costs(void) {
    if (++vehicle_cost_samples >= 20 &&
        camera_frequency.QuadPart > 0 && control_host) {
        char line[180];
        double scale = 1000.0 /
            ((double)camera_frequency.QuadPart * vehicle_cost_samples);
        snprintf(line, sizeof(line),
            "Vehicle cost ms/frame: ground %.2f, collision %.2f, position %.2f, rotation %.2f.",
            vehicle_cost_ticks[0] * scale,
            vehicle_cost_ticks[1] * scale,
            vehicle_cost_ticks[2] * scale,
            vehicle_cost_ticks[3] * scale);
        control_host->log("control", line);
        memset(vehicle_cost_ticks, 0, sizeof(vehicle_cost_ticks));
        vehicle_cost_samples = 0;
    }
}
#endif

static int calibrate_manual_vehicle(const float position[3], DWORD now) {
    float dx, dy, distance;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (vehicle_manual_mode || !vehicle_follow_mode ||
        now - vehicle_calibrate_at < VEHICLE_CALIBRATE_MS) return 0;
    dx = position[0] - vehicle_calibrate_position[0];
    dy = position[1] - vehicle_calibrate_position[1];
    distance = sqrtf(dx * dx + dy * dy);
    if (distance < VEHICLE_CALIBRATE_DISTANCE) return 0;
    vehicle_manual_heading[0] = vehicle_initial_heading[0] = dx / distance;
    vehicle_manual_heading[1] = vehicle_initial_heading[1] = dy / distance;
    memcpy(vehicle_initial_rotation, (char *)unit + 0xb8,
           sizeof(vehicle_initial_rotation));
    vehicle_manual_speed = clamp_float(distance * 1000.0f /
        (float)(now - vehicle_calibrate_at), 0.0f, VEHICLE_MAX_SPEED);
    vehicle_last_rotation_angle = 0.0f;
    ((ClearActionsFn)(base + CLEAR_ACTIONS_RVA))(unit);
    release_follow_target();
    vehicle_manual_mode = 1;
    vehicle_manual_counter.QuadPart = 0;
    if (camera_frequency.QuadPart > 0)
        QueryPerformanceCounter(&vehicle_manual_counter);
    if (control_host)
        control_host->log("control", "Vehicle direct movement calibrated.");
    return 1;
}

static void move_vehicle_manually(const float position[3], unsigned keys,
                                  DWORD now) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    float delta_seconds = (float)(now - last_plan) * 0.001f;
    float target_speed, direction, steer, angle, c, s;
    float next[4], rotation[9];
    LARGE_INTEGER counter;
#ifdef EM4_VEHICLE_MOTION_TRACE
    LARGE_INTEGER cost_start, cost_end;
#endif
    unsigned i;
    if (camera_frequency.QuadPart > 0 && QueryPerformanceCounter(&counter)) {
        if (vehicle_manual_counter.QuadPart > 0 &&
            counter.QuadPart >= vehicle_manual_counter.QuadPart)
            delta_seconds = (float)((double)(counter.QuadPart -
                vehicle_manual_counter.QuadPart) /
                (double)camera_frequency.QuadPart);
        if (delta_seconds < VEHICLE_MIN_STEP_SECONDS) return;
        vehicle_manual_counter = counter;
    }
    delta_seconds = clamp_float(delta_seconds, 0.0f, 0.05f);
    if (!(delta_seconds > 0.0f)) return;
    direction = (keys & 1) ? 1.0f : -1.0f;
    target_speed = direction > 0.0f ? VEHICLE_MAX_SPEED :
                                     VEHICLE_REVERSE_SPEED;
    vehicle_manual_speed = clamp_float(vehicle_manual_speed +
        VEHICLE_ACCELERATION * delta_seconds, 0.0f, target_speed);
    steer = ((keys & 8) ? 1.0f : 0.0f) - ((keys & 4) ? 1.0f : 0.0f);
    angle = steer * direction * VEHICLE_TURN_RAD_PER_SEC * delta_seconds;
    c = cosf(angle);
    s = sinf(angle);
    {
        float x = vehicle_manual_heading[0];
        float y = vehicle_manual_heading[1];
        vehicle_manual_heading[0] = x * c + y * s;
        vehicle_manual_heading[1] = y * c - x * s;
    }
    next[0] = position[0] + vehicle_manual_heading[0] *
              vehicle_manual_speed * delta_seconds * direction;
    next[1] = position[1] + vehicle_manual_heading[1] *
              vehicle_manual_speed * delta_seconds * direction;
    next[2] = position[2];
    next[3] = 0.0f;
    {
#ifdef EM4_VEHICLE_MOTION_TRACE
        QueryPerformanceCounter(&cost_start);
#endif
        float ground = ((GetGroundHeightFn)(base + GET_GROUND_HEIGHT_RVA))(
            next[0], next[1]);
#ifdef EM4_VEHICLE_MOTION_TRACE
        QueryPerformanceCounter(&cost_end);
        vehicle_cost_ticks[0] +=
            (double)(cost_end.QuadPart - cost_start.QuadPart);
#endif
        if (ground > -100000.0f && ground < 100000.0f &&
            fabsf(ground - position[2]) < 80.0f)
            next[2] = ground;
    }
#ifdef EM4_VEHICLE_MOTION_TRACE
    QueryPerformanceCounter(&cost_start);
#endif
    {
        int collision = ((CheckCollisionFn)(base + CHECK_COLLISION_RVA))(
            unit, next);
#ifdef EM4_VEHICLE_MOTION_TRACE
        QueryPerformanceCounter(&cost_end);
        vehicle_cost_ticks[1] +=
            (double)(cost_end.QuadPart - cost_start.QuadPart);
#endif
        if (collision) {
        vehicle_manual_speed = 0.0f;
        last_plan = now;
#ifdef EM4_VEHICLE_MOTION_TRACE
        report_vehicle_costs();
#endif
        return;
        }
    }
#ifdef EM4_VEHICLE_MOTION_TRACE
    QueryPerformanceCounter(&cost_start);
#endif
    ((SetObjectPositionFn)(base + SET_OBJECT_POSITION_RVA))(unit, next);
#ifdef EM4_VEHICLE_MOTION_TRACE
    QueryPerformanceCounter(&cost_end);
    vehicle_cost_ticks[2] +=
        (double)(cost_end.QuadPart - cost_start.QuadPart);
#endif
    angle = atan2f(vehicle_manual_heading[1], vehicle_manual_heading[0]) -
            atan2f(vehicle_initial_heading[1], vehicle_initial_heading[0]);
    c = cosf(angle);
    s = sinf(angle);
    if (fabsf(angle - vehicle_last_rotation_angle) >= 0.006f) {
        for (i = 0; i < 3; ++i) {
            rotation[i] = c * vehicle_initial_rotation[i] -
                          s * vehicle_initial_rotation[i + 3];
            rotation[i + 3] = s * vehicle_initial_rotation[i] +
                              c * vehicle_initial_rotation[i + 3];
            rotation[i + 6] = vehicle_initial_rotation[i + 6];
        }
#ifdef EM4_VEHICLE_MOTION_TRACE
        QueryPerformanceCounter(&cost_start);
#endif
        ((SetRotationFn)(base + SET_ROTATION_RVA))(unit, rotation);
#ifdef EM4_VEHICLE_MOTION_TRACE
        QueryPerformanceCounter(&cost_end);
        vehicle_cost_ticks[3] +=
            (double)(cost_end.QuadPart - cost_start.QuadPart);
#endif
        vehicle_last_rotation_angle = angle;
    }
    desired_heading[0] = vehicle_manual_heading[0];
    desired_heading[1] = vehicle_manual_heading[1];
    if (!moving) log_status("WASD movement started.");
    moving = 1;
    last_keys = keys;
    last_plan = now;
#ifdef EM4_VEHICLE_MOTION_TRACE
    report_vehicle_costs();
#endif
}
#endif

static int game_in_foreground(void) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void stop_camera_follow(void) {
    float position[3], from[4], to[4];
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    unsigned i;
    if (camera_following && unit_inspector_entity_valid(unit, 8 | 16)) {
        for (i = 0; i < 3; ++i) {
            position[i] = *(float *)((char *)unit + 0xa8 + i * sizeof(float));
            from[i] = position[i] + original_camera_offset[i];
            to[i] = from[i] + original_camera_forward[i] * 100.0f;
        }
        from[3] = to[3] = 0.0f;
        ((SetCameraFn)(base + SET_CAMERA_RVA))(from);
        ((LookAtFn)(base + LOOK_AT_RVA))(from, to);
    }
    camera_following = 0;
}

static int start_camera_follow(const float position[3]) {
    float camera_position[3], right[2], forward[2];
    float elevation;
    unsigned i;
    if (!path_viewer_camera_position(camera_position) ||
        !path_viewer_camera_forward(original_camera_forward) ||
        !path_viewer_camera_axes(right, forward)) return 0;
    chase_heading[0] = forward[0];
    chase_heading[1] = forward[1];
    desired_heading[0] = forward[0];
    desired_heading[1] = forward[1];
    elevation = camera_position[2] - position[2];
    if (!(elevation > 100.0f && elevation < 20000.0f)) elevation = 4000.0f;
    chase_height = unit_type == 16 ?
        clamp_float(elevation * 0.20f, 650.0f, 1100.0f) :
        clamp_float(elevation * 0.24f, 950.0f, 1700.0f);
    chase_back = chase_height * (unit_type == 16 ? 1.25f : 1.35f);
    chase_look_height = unit_type == 16 ?
        PERSON_LOOK_HEIGHT : VEHICLE_LOOK_HEIGHT;
    for (i = 0; i < 3; ++i) {
        chase_camera[i] = camera_position[i];
        chase_focus[i] = position[i];
        original_camera_offset[i] = camera_position[i] - position[i];
        heading_sample_position[i] = position[i];
    }
    last_camera_tick = GetTickCount();
    last_heading_sample = last_camera_tick;
    if (camera_frequency.QuadPart > 0)
        QueryPerformanceCounter(&last_camera_counter);
    camera_following = 1;
    camera_follow_logged = 0;
    return 1;
}

static void update_camera_follow(void) {
    float current[3], from[4], to[4];
    float back, height, look_height, heading_weight, focus_weight;
    float camera_weight, length, current_angle, target_angle, angle_delta, max_turn;
    DWORD now = GetTickCount();
    float elapsed = (float)(now - last_camera_tick);
    LARGE_INTEGER counter;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    unsigned i;
    if (!camera_following || !unit_inspector_entity_valid(unit, 8 | 16)) return;
    for (i = 0; i < 3; ++i) {
        current[i] = *(float *)((char *)unit + 0xa8 + i * sizeof(float));
        if (!(current[i] > -1000000.0f && current[i] < 1000000.0f)) return;
    }
    if (camera_frequency.QuadPart > 0 && QueryPerformanceCounter(&counter)) {
        if (last_camera_counter.QuadPart > 0 &&
            counter.QuadPart >= last_camera_counter.QuadPart)
            elapsed = (float)((double)(counter.QuadPart - last_camera_counter.QuadPart) *
                              1000.0 / (double)camera_frequency.QuadPart);
        last_camera_counter = counter;
    }
    elapsed = clamp_float(elapsed, 0.0f, 100.0f);
    last_camera_tick = now;
    heading_weight = 1.0f - expf(-(float)elapsed / CAMERA_TURN_MS);
    focus_weight = 1.0f - expf(-(float)elapsed / CAMERA_FOCUS_MS);
    camera_weight = 1.0f - expf(-(float)elapsed / CAMERA_POSITION_MS);
    if (now - last_heading_sample >= HEADING_SAMPLE_MS) {
        float dx = current[0] - heading_sample_position[0];
        float dy = current[1] - heading_sample_position[1];
        length = sqrtf(dx * dx + dy * dy);
        if (length > 0.5f && length < 500.0f) {
            desired_heading[0] = dx / length;
            desired_heading[1] = dy / length;
        }
        memcpy(heading_sample_position, current, sizeof(heading_sample_position));
        last_heading_sample = now;
    }
    current_angle = atan2f(chase_heading[1], chase_heading[0]);
    target_angle = atan2f(desired_heading[1], desired_heading[0]);
    angle_delta = atan2f(sinf(target_angle - current_angle),
                         cosf(target_angle - current_angle));
    max_turn = CAMERA_MAX_TURN_RAD_PER_MS * (float)elapsed;
    angle_delta = clamp_float(angle_delta * heading_weight, -max_turn, max_turn);
    chase_heading[0] = cosf(current_angle + angle_delta);
    chase_heading[1] = sinf(current_angle + angle_delta);
    for (i = 0; i < 3; ++i) {
        float gap = current[i] - chase_focus[i];
        chase_focus[i] += gap * (gap > 2000.0f || gap < -2000.0f ?
                               1.0f : focus_weight);
    }
    back = chase_back;
    height = chase_height;
    look_height = chase_look_height;
    chase_camera[0] += (chase_focus[0] - chase_heading[0] * back - chase_camera[0]) *
                       camera_weight;
    chase_camera[1] += (chase_focus[1] - chase_heading[1] * back - chase_camera[1]) *
                       camera_weight;
    chase_camera[2] += (chase_focus[2] + height - chase_camera[2]) * camera_weight;
    for (i = 0; i < 3; ++i) from[i] = chase_camera[i];
    to[0] = chase_focus[0];
    to[1] = chase_focus[1];
    to[2] = chase_focus[2] + look_height;
    from[3] = to[3] = 0.0f;
    ((SetCameraFn)(base + SET_CAMERA_RVA))(from);
    if (!camera_follow_logged && control_host)
        control_host->log("control", "Chase camera position set; applying look-at rotation.");
    ((LookAtFn)(base + LOOK_AT_RVA))(from, to);
    if (!camera_follow_logged && control_host) {
        char line[160];
        snprintf(line, sizeof(line),
                 "Chase camera: type %u, unit %.0f %.0f %.0f, heading %.2f %.2f, back %.0f, height %.0f.",
                 unit_type, current[0], current[1], current[2],
                 chase_heading[0], chase_heading[1], back, height);
        control_host->log("control", line);
        camera_follow_logged = 1;
    }
}

static void stop_unit(void) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (moving && unit && unit_inspector_entity_valid(unit, 8 | 16))
        ((ClearActionsFn)(base + CLEAR_ACTIONS_RVA))(unit);
    if (moving) log_status("WASD movement stopped.");
    moving = 0;
    vehicle_follow_mode = 0;
#ifdef EM4_MANUAL_VEHICLE
    vehicle_manual_speed = 0.0f;
    vehicle_manual_counter.QuadPart = 0;
#endif
    last_keys = 0;
    last_vehicle_target_valid = 0;
}

static void disengage(const char *reason) {
    if (set_input_mask) set_input_mask(FALSE);
    stop_unit();
    release_follow_target();
#ifdef EM4_MANUAL_VEHICLE
    vehicle_manual_mode = 0;
#endif
    stop_camera_follow();
    active = 0;
    unit = NULL;
    unit_type = 0;
    if (reason) log_status(reason);
}

void direct_control_init(const EM4HostAPI *host) {
    char proxy_path[MAX_PATH];
    char *name;
    HMODULE proxy = NULL;
    control_host = host;
    if (!QueryPerformanceFrequency(&camera_frequency))
        camera_frequency.QuadPart = 0;
    if (GetModuleFileNameA(NULL, proxy_path, MAX_PATH)) {
        name = strrchr(proxy_path, '\\');
        if (name && (size_t)(name - proxy_path) +
            sizeof("dinput8.dll") < sizeof(proxy_path)) {
            strcpy(name + 1, "dinput8.dll");
            proxy = GetModuleHandleA(proxy_path);
        }
    }
    set_input_mask = proxy ? (SetInputMaskFn)GetProcAddress(
        proxy, "EM4SetDirectControlInput") : NULL;
    input_ready = proxy ? (InputReadyFn)GetProcAddress(
        proxy, "EM4DirectControlInputReady") : NULL;
    log_status("Ctrl+F8 toggles WASD control for one selected unit.");
}

void direct_control_request_toggle(void) {
    InterlockedIncrement(&toggle_requests);
}

const char *direct_control_status_text(void) {
    if (active) return camera_following ?
        "WASD + CHASE CAMERA ON | Ctrl+F8 / Esc: off" :
        "WASD ON | camera follow unavailable | Ctrl+F8 / Esc: off";
    return GetTickCount() - status_at < 3500 ? status_text : NULL;
}

void direct_control_on_frame(void) {
    DWORD now = GetTickCount();
    LONG requests = InterlockedExchange(&toggle_requests, 0);
    void *selected = NULL;
    float position[3];
    float right[2], forward[2], direction[2], length;
    unsigned keys = 0;
    int foreground = game_in_foreground();

    if (requests & 1) {
        if (active) disengage("WASD control off.");
        else if (!foreground) log_status("WASD control needs the game in focus.");
        else if (!set_input_mask || !input_ready || !input_ready())
            log_status("WASD needs the updated dinput8.dll input filter.");
        else if (!unit_inspector_selected_unit(&selected, position))
            log_status("Select exactly one vehicle or person before Ctrl+F8.");
        else if (!path_viewer_camera_axes(right, forward))
            log_status("WASD control needs a loaded scene and camera.");
        else {
            unit = selected;
            unit_type = *(unsigned *)((char *)unit + 0x28);
            active = 1;
            moving = 0;
            vehicle_follow_mode = 0;
            follow_unavailable = 0;
#ifdef EM4_MANUAL_VEHICLE
            vehicle_manual_mode = 0;
#endif
            last_keys = 0;
            last_vehicle_target_valid = 0;
#ifdef EM4_VEHICLE_MOTION_TRACE
            last_vehicle_trace = now;
            trace_position[0] = position[0];
            trace_position[1] = position[1];
#endif
            last_selection_check = now;
            set_input_mask(TRUE);
            if (start_camera_follow(position))
                log_status("WASD and chase camera on. Esc, Enter, or Ctrl+F8: off.");
            else log_status("WASD on. Camera follow unavailable in this scene.");
        }
    }
    if (!active) return;
    if (!foreground) {
        set_input_mask(FALSE);
        stop_unit();
        return;
    }
    set_input_mask(TRUE);
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) ||
        (GetAsyncKeyState(VK_RETURN) & 0x8000)) {
        disengage("WASD control off.");
        return;
    }
    if (now - last_selection_check >= SELECTION_CHECK_MS) {
        if (!unit_inspector_selected_unit(&selected, position) ||
            selected != unit) {
            disengage("WASD control off: selected unit changed.");
            return;
        }
        last_selection_check = now;
    }
    update_camera_follow();
#ifdef EM4_VEHICLE_MOTION_TRACE
    if (unit_type != 16 && control_host &&
        now - last_vehicle_trace >= 500 &&
        unit_inspector_entity_valid(unit, 8)) {
        float x = *(float *)((char *)unit + 0xa8);
        float y = *(float *)((char *)unit + 0xac);
        float dx = x - trace_position[0], dy = y - trace_position[1];
        float speed = sqrtf(dx * dx + dy * dy) * 1000.0f /
                      (float)(now - last_vehicle_trace);
        float path_speed = *(float *)((char *)unit + 0x364);
        float remaining = last_vehicle_target_valid ?
            sqrtf((last_vehicle_target[0] - x) * (last_vehicle_target[0] - x) +
                  (last_vehicle_target[1] - y) * (last_vehicle_target[1] - y)) : 0.0f;
        char line[160];
        snprintf(line, sizeof(line),
                 "Vehicle motion: speed %.1f, path speed %.1f, target %.0f, keys %u, action age %lu ms.",
                 speed, path_speed, remaining, last_keys,
                 (unsigned long)(now - last_plan));
        control_host->log("control", line);
        trace_position[0] = x;
        trace_position[1] = y;
        last_vehicle_trace = now;
    }
#endif
    if (!(GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
        !(GetAsyncKeyState(VK_MENU) & 0x8000)) {
        if (GetAsyncKeyState('W') & 0x8000) keys |= 1;
        if (GetAsyncKeyState('S') & 0x8000) keys |= 2;
        if (GetAsyncKeyState('A') & 0x8000) keys |= 4;
        if (GetAsyncKeyState('D') & 0x8000) keys |= 8;
    }
    if (!(keys & 3) || (keys & 3) == 3) keys &= ~3u;
    if (!(keys & 12) || (keys & 12) == 12) keys &= ~12u;
    /* Vehicles steer while moving; a lateral key alone must not request a
       distant road on the other side of the unit. */
    if (unit_type != 16 && !(keys & 3)) keys = 0;
    if (!keys) {
        stop_unit();
        return;
    }
    if (unit_type == 16 && moving && keys == last_keys &&
        now - last_plan < PERSON_REPLAN_MS)
        return;
#ifdef EM4_MANUAL_VEHICLE
    if (vehicle_manual_mode) {
        unsigned i;
        if (!unit_inspector_entity_valid(unit, 8)) {
            disengage("WASD control off: vehicle unavailable.");
            return;
        }
        for (i = 0; i < 3; ++i)
            position[i] = *(float *)((char *)unit + 0xa8 + i * sizeof(float));
    } else
#endif
    if (!unit_inspector_selected_unit(&selected, position) || selected != unit) {
        disengage("WASD control off: selection or camera unavailable.");
        return;
    }
    if (moving && keys == last_keys && !vehicle_follow_mode
#ifdef EM4_MANUAL_VEHICLE
        && !vehicle_manual_mode
#endif
        ) {
        if (unit_type != 16 && last_vehicle_target_valid) {
            float dx = last_vehicle_target[0] - position[0];
            float dy = last_vehicle_target[1] - position[1];
            if (now - last_plan < VEHICLE_REPLAN_MIN_MS ||
                (now - last_plan < VEHICLE_REPLAN_STUCK_MS &&
                 dx * dx + dy * dy > VEHICLE_REPLAN_NEAR_DISTANCE *
                                     VEHICLE_REPLAN_NEAR_DISTANCE))
                return;
        }
    }
    if (camera_following) {
        forward[0] = chase_heading[0];
        forward[1] = chase_heading[1];
        right[0] = forward[1];
        right[1] = -forward[0];
    } else if (!path_viewer_camera_axes(right, forward)) {
        disengage("WASD control off: selection or camera unavailable.");
        return;
    }
    if (unit_type == 16) {
        direction[0] = ((keys & 8) ? right[0] : 0.0f) -
                       ((keys & 4) ? right[0] : 0.0f) +
                       ((keys & 1) ? forward[0] : 0.0f) -
                       ((keys & 2) ? forward[0] : 0.0f);
        direction[1] = ((keys & 8) ? right[1] : 0.0f) -
                       ((keys & 4) ? right[1] : 0.0f) +
                       ((keys & 1) ? forward[1] : 0.0f) -
                       ((keys & 2) ? forward[1] : 0.0f);
    } else {
        float drive = (keys & 1) ? 1.0f : -1.0f;
        float steer = ((keys & 8) ? VEHICLE_STEER : 0.0f) -
                      ((keys & 4) ? VEHICLE_STEER : 0.0f);
        direction[0] = drive * (forward[0] + right[0] * steer);
        direction[1] = drive * (forward[1] + right[1] * steer);
    }
    length = sqrtf(direction[0] * direction[0] +
                   direction[1] * direction[1]);
    if (!(length > 0.1f)) return;
    if (unit_type != 16) {
#ifdef EM4_MANUAL_VEHICLE
        if (vehicle_manual_mode) {
            move_vehicle_manually(position, keys, now);
            return;
        }
        calibrate_manual_vehicle(position, now);
        if (vehicle_manual_mode) {
            move_vehicle_manually(position, keys, now);
            return;
        }
#endif
        float follow_target[4] = {
            position[0] + direction[0] / length * VEHICLE_FOLLOW_DISTANCE,
            position[1] + direction[1] / length * VEHICLE_FOLLOW_DISTANCE,
            position[2], 0.0f
        };
        if (position_follow_target(follow_target)) {
            if (!vehicle_follow_mode) {
                start_vehicle_follow();
#ifdef EM4_MANUAL_VEHICLE
                vehicle_calibrate_at = now;
                vehicle_calibrate_position[0] = position[0];
                vehicle_calibrate_position[1] = position[1];
#endif
            }
            if (!moving) log_status("WASD movement started.");
            moving = 1;
            last_keys = keys;
            return;
        }
    }
    {
        int replace_vehicle = unit_type != 16 && moving &&
            keys == last_keys && last_vehicle_target_valid &&
            now - last_plan < VEHICLE_REPLAN_STUCK_MS;
        float target[4] = {
            position[0] + direction[0] / length *
                (unit_type == 16 ? PERSON_DISTANCE : VEHICLE_DISTANCE),
            position[1] + direction[1] / length *
                (unit_type == 16 ? PERSON_DISTANCE : VEHICLE_DISTANCE),
            position[2], 0.0f
        };
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        ((MoveDirectFn)(base + MOVE_DIRECT_RVA))(
            unit, replace_vehicle ? ACTION_REPLACEFIRST : ACTION_NEWLIST,
            target, 0);
#ifdef EM4_VEHICLE_MOTION_TRACE
        if (unit_type != 16 && control_host) {
            char line[128];
            snprintf(line, sizeof(line), "Vehicle action: %s target %.0f %.0f.",
                     replace_vehicle ? "replace" : "new", target[0], target[1]);
            control_host->log("control", line);
        }
#endif
        if (unit_type != 16) {
            last_vehicle_target[0] = target[0];
            last_vehicle_target[1] = target[1];
            last_vehicle_target_valid = 1;
        }
    }
    if (!moving) log_status("WASD movement started.");
    moving = 1;
    last_keys = keys;
    last_plan = now;
}

#ifndef EM4_UNIT_INSPECTOR_H
#define EM4_UNIT_INSPECTOR_H

#include "em4_plugin.h"

#define UNIT_INSPECTOR_ROWS 64

typedef struct UnitCommandRow {
    char name[80];
    char script[120];
    char status[56];
    unsigned group;
} UnitCommandRow;

typedef struct UnitInspectorSnapshot {
    char summary[160];
    char query_result[160];
    unsigned selected_count;
    unsigned command_count;
    unsigned shown_count;
    UnitCommandRow rows[UNIT_INSPECTOR_ROWS];
} UnitInspectorSnapshot;

void unit_inspector_init(const EM4HostAPI *host);
void unit_inspector_on_frame(void);
void unit_inspector_observe(void *command, void *actor, unsigned char possible);
void unit_inspector_copy(UnitInspectorSnapshot *out);

#endif

#ifndef EM4_DIRECT_CONTROL_H
#define EM4_DIRECT_CONTROL_H

#include "em4_plugin.h"

void direct_control_init(const EM4HostAPI *host);
void direct_control_request_toggle(void);
void direct_control_on_frame(void);
const char *direct_control_status_text(void);

#endif

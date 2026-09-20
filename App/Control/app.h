#ifndef APP_CONTROL_APP_H
#define APP_CONTROL_APP_H

#include <stdbool.h>
#include <stdint.h>

bool app_init(void);
void app_sample(void);
void app_poll(void);
void app_fault(uint32_t fault);
bool app_command(const char *line);

#endif

#pragma once
#include <stdint.h>

void plog_init(void);
void plog_append(void);
void plog_str(const char *s);
void plog_hex(const char *label, uint64_t v);

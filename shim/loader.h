#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);
/* Same, for a payload already at [rd_start, rd_end). */
const char *loader_boot_rd(uint64_t rd_start, uint64_t rd_end);

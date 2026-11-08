/* SPDX-License-Identifier: MIT */
#include "legacy_ecu.h"

#include <string.h>

static uint32_t s_speed_mm_s;
static uint32_t s_ticks;
static int s_ccvs_pending;
static uint8_t s_ccvs[8];

void legacy_ecu_init(void)
{
    s_speed_mm_s = 0;
    s_ticks = 0;
    s_ccvs_pending = 0;
    memset(s_ccvs, 0xFF, sizeof s_ccvs);
}

void legacy_ecu_set_wheel_speed(uint32_t speed_mm_s)
{
    s_speed_mm_s = speed_mm_s;
}

void legacy_ecu_tick(void)
{
    if ((s_ticks % 10u) == 0u)
    {
        /* km/h * 256 = mm/s * 3.6 / 1000 * 256 = mm/s * 0.9216 */
        uint32_t raw = (uint32_t)(((uint64_t)s_speed_mm_s * 9216u) / 10000u);
        if (raw > 0xFAFFu)
        {
            raw = 0xFAFFu;
        }
        memset(s_ccvs, 0xFF, sizeof s_ccvs);
        s_ccvs[1] = (uint8_t)(raw & 0xFFu);
        s_ccvs[2] = (uint8_t)(raw >> 8);
        s_ccvs_pending = 1;
    }
    ++s_ticks;
}

int legacy_ecu_take_ccvs(uint8_t data[8])
{
    if (!s_ccvs_pending)
    {
        return 0;
    }
    memcpy(data, s_ccvs, sizeof s_ccvs);
    s_ccvs_pending = 0;
    return 1;
}

uint32_t legacy_ecu_tick_count(void)
{
    return s_ticks;
}

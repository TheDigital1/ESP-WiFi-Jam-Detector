/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The parts of settings.h without flash access: defaults and range check
 * (also built by the host tests).
 */
#include <string.h>
#include "settings.h"

void settings_default(settings_t *s)
{
    memset(s, 0, sizeof(*s)); /* padding too: the blob is compared bytewise */
    detect_default_config(&s->det);
    s->weight[BAND_24] = 2;
    s->weight[BAND_5] = 1;
    s->gain[BAND_24] = s->gain[BAND_5] = SETTINGS_GAIN_NONE;
    s->beep = false;
    s->brightness = 80;
    s->scan[BAND_24] = s->scan[BAND_5] = true;
}

int settings_check(const settings_t *s)
{
    bool any = false;
    for (int b = 0; b < BAND_COUNT; b++) {
        if (s->weight[b] < 1 || s->weight[b] > 8) {
            return -1;
        }
        any |= s->scan[b];
    }
    if (!any) {
        return -1;
    }
    return detect_check_config(&s->det) == 0 && s->brightness >= 5 && s->brightness <= 100 ? 0 : -1;
}

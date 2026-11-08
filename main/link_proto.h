/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * USB link message format (version LINK_PROTO_VERSION): one JSON object per
 * line. Device -> host lines start with {"t": (hello, status, event, ack), so
 * a reader can tell them from ESP_LOG lines on the same port. Host -> device
 * lines are flat objects: {"c":"<command>"[,"k":"<key>","v":<int>][,"id":<int>]}.
 * Commands: hello, ping, get, relearn, set (k = a param key, "beep" or "scan").
 *
 * Formatting and parsing only, no IDF calls (host-tested in test_host/test_link.c).
 * docs/LINK.md describes the messages for the bridge side.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bandplan.h"
#include "detect.h"
#include "settings.h"

#define LINK_PROTO_VERSION 1
#define LINK_LINE_MAX      2048 /* longest line sent (hello with the param table) */
#define LINK_CMD_MAX       256  /* longest command line accepted */

typedef struct {
    char c[16];
    char k[24];
    int v;
    bool has_k, has_v;
    int id; /* echoed in the ack; 0 if absent */
} link_cmd_t;

typedef struct {
    bool ok; /* read succeeded; the rest is valid only then */
    bool vbus, battery, charging;
    int percent; /* -1 without a battery */
    uint16_t mv;
} link_power_t;

typedef struct {
    detect_status_t st[BAND_COUNT];
    bool scanned[BAND_COUNT];
    link_power_t power;
    settings_t set;
    uint32_t uptime_s, boot;
} link_status_t;

/* 0 on success, -1 if the line is not a flat JSON object with a "c" string. */
int link_parse(const char *line, link_cmd_t *out);

/* Each returns the line length including the trailing '\n', or 0 if it did not fit. */
size_t link_fmt_hello(char *buf, size_t cap, const char *id, const char *fw, const char *model);
size_t link_fmt_status(char *buf, size_t cap, const link_status_t *s);
size_t link_fmt_event(char *buf, size_t cap, band_id_t band, const detect_event_t *ev, uint32_t boot);
size_t link_fmt_ack(char *buf, size_t cap, int id, const char *err /* NULL: ok */);

/* Stable protocol names (lowercase, unlike the display names in detect.c). */
const char *link_state_name(detect_state_t s);
const char *link_kind_name(detect_kind_t k);
const char *link_band_name(band_id_t b);

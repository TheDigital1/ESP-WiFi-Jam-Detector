/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link_proto.h"
#include "params.h"

/* ---- output ---- */

typedef struct {
    char *buf;
    size_t cap, len;
    bool full;
} jw_t;

static void jw(jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jw(jw_t *w, const char *fmt, ...)
{
    if (w->full) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->full = true;
        return;
    }
    w->len += (size_t)n;
}

static size_t jw_end(jw_t *w)
{
    jw(w, "}\n");
    if (w->full) {
        if (w->cap) {
            w->buf[0] = '\0';
        }
        return 0;
    }
    return w->len;
}

static const char *tf(bool b)
{
    return b ? "true" : "false";
}

const char *link_state_name(detect_state_t s)
{
    static const char *const names[] = {"learning", "normal", "suspect", "alarm"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "unknown";
}

const char *link_kind_name(detect_kind_t k)
{
    static const char *const names[] = {"none", "wideband", "swept", "narrowband", "pulsed"};
    return (unsigned)k < sizeof(names) / sizeof(names[0]) ? names[k] : "unknown";
}

const char *link_band_name(band_id_t b)
{
    return b == BAND_24 ? "2.4" : "5";
}

size_t link_fmt_hello(char *buf, size_t cap, const char *id, const char *fw, const char *model)
{
    jw_t w = {.buf = buf, .cap = cap};
    jw(&w, "{\"t\":\"hello\",\"v\":%d,\"id\":\"%s\",\"fw\":\"%s\",\"model\":\"%s\",\"bands\":[\"2.4\",\"5\"],",
       LINK_PROTO_VERSION, id, fw, model);
    jw(&w, "\"params\":[");
    for (int p = 0; p < PARAM_COUNT; p++) {
        const param_info_t *i = &param_info[p];
        jw(&w, "%s{\"k\":\"%s\",\"n\":\"%s\",\"u\":\"%s\",\"min\":%d,\"max\":%d,\"step\":%d}", p ? "," : "", i->key,
           i->name, i->unit, i->min, i->max, i->step);
    }
    jw(&w, "]");
    return jw_end(&w);
}

size_t link_fmt_status(char *buf, size_t cap, const link_status_t *s)
{
    jw_t w = {.buf = buf, .cap = cap};
    jw(&w, "{\"t\":\"status\",\"v\":%d,\"up\":%u,\"boot\":%u,\"bands\":{", LINK_PROTO_VERSION,
       (unsigned)s->uptime_s, (unsigned)s->boot);
    for (int b = 0; b < BAND_COUNT; b++) {
        const detect_status_t *st = &s->st[b];
        jw(&w,
           "%s\"%s\":{\"scan\":%s,\"state\":\"%s\",\"kind\":\"%s\",\"learn_pct\":%u,\"alarms\":%u,"
           "\"last_alarm_kind\":\"%s\",\"floor_db\":%.2f,\"occupancy_pct\":%u,\"clipped_pct\":%u,\"duty_pct\":%u}",
           b ? "," : "", link_band_name((band_id_t)b), tf(s->scanned[b]), link_state_name(st->state),
           link_kind_name(st->state >= DETECT_SUSPECT ? st->kind : DETECT_NONE), st->learn_pct,
           (unsigned)st->alarms, link_kind_name(st->alarms ? st->last_alarm_kind : DETECT_NONE),
           st->m.floor_delta_cdb / 100.0, st->m.occupancy_pct, st->m.clipped_pct, st->m.duty_pct);
    }
    jw(&w, "}");
    if (s->power.ok) {
        jw(&w, ",\"power\":{\"vbus\":%s,\"battery\":%s,\"charging\":%s", tf(s->power.vbus), tf(s->power.battery),
           tf(s->power.charging));
        if (s->power.battery) {
            jw(&w, ",\"pct\":%d,\"mv\":%u", s->power.percent, s->power.mv);
        }
        jw(&w, "}");
    }
    jw(&w, ",\"set\":{\"%s\":%s,\"%s\":%d", PARAM_KEY_BEEP, tf(s->set.beep), PARAM_KEY_SCAN,
       param_scan_mask(&s->set));
    for (int p = 0; p < PARAM_COUNT; p++) {
        jw(&w, ",\"%s\":%d", param_info[p].key, param_get(&s->set, (param_t)p));
    }
    jw(&w, "}");
    return jw_end(&w);
}

/* Same fields as the TF-card CSV log (eventlog.c). */
size_t link_fmt_event(char *buf, size_t cap, band_id_t band, const detect_event_t *e, uint32_t boot)
{
    const detect_metrics_t *m = &e->m;
    jw_t w = {.buf = buf, .cap = cap};
    jw(&w,
       "{\"t\":\"event\",\"v\":%d,\"boot\":%u,\"up\":%.1f,\"band\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
       "\"kind\":\"%s\",\"floor_db\":%.2f,\"continuous_db\":%.2f,\"max_cell_db\":%.2f,\"max_cell_mhz\":%.1f,"
       "\"flatness_db\":%.2f,\"wide_pct\":%u,\"continuous_pct\":%u,\"occupancy_pct\":%u,\"duty_pct\":%u,"
       "\"clipped_pct\":%u,\"evidence\":%u,\"evidence_duty_pct\":%u,\"window_sweeps\":%u,\"cw_mhz\":%.2f,"
       "\"cw_bins\":%u,\"cw_db\":%.2f,\"swept_mhz\":%.2f,\"swept_mhz_per_s\":%.2f,\"swept_steps\":%u,"
       "\"swept_cover_pct\":%u",
       LINK_PROTO_VERSION, (unsigned)boot, e->t_ms / 1000.0, link_band_name(band), link_state_name(e->from),
       link_state_name(e->to), link_kind_name(e->kind), m->floor_delta_cdb / 100.0, m->cont_delta_cdb / 100.0,
       m->floor_delta_max_cdb / 100.0, m->floor_delta_max_khz / 1000.0, m->flatness_cdb / 100.0, m->wide_pct,
       m->cont_pct, m->occupancy_pct, m->duty_pct, m->clipped_pct, m->evidence, m->evidence_duty_pct,
       m->window_sweeps, m->cw_khz / 1000.0, m->cw_bins, m->cw_excess_cdb / 100.0, m->swept_khz / 1000.0,
       m->swept_khz_per_s / 1000.0, m->swept_steps, m->swept_cover_pct);
    return jw_end(&w);
}

size_t link_fmt_ack(char *buf, size_t cap, int id, const char *err)
{
    jw_t w = {.buf = buf, .cap = cap};
    jw(&w, "{\"t\":\"ack\",\"v\":%d,\"id\":%d,\"ok\":%s", LINK_PROTO_VERSION, id, tf(!err));
    if (err) {
        jw(&w, ",\"err\":\"%s\"", err);
    }
    return jw_end(&w);
}

/* ---- input: one flat object, string / integer / boolean values ---- */

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

/* "..." without escapes into out (truncation is an error). */
static const char *parse_str(const char *p, char *out, size_t cap)
{
    if (*p != '"') {
        return NULL;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' || (unsigned char)*p < 0x20 || n + 1 >= cap) {
            return NULL;
        }
        out[n++] = *p++;
    }
    if (*p != '"') {
        return NULL;
    }
    out[n] = '\0';
    return p + 1;
}

int link_parse(const char *line, link_cmd_t *out)
{
    memset(out, 0, sizeof(*out));
    bool has_c = false;
    const char *p = skip_ws(line);
    if (*p++ != '{') {
        return -1;
    }
    p = skip_ws(p);
    if (*p == '}') {
        return -1;
    }
    for (;;) {
        char key[16];
        p = parse_str(skip_ws(p), key, sizeof(key));
        if (!p) {
            return -1;
        }
        p = skip_ws(p);
        if (*p++ != ':') {
            return -1;
        }
        p = skip_ws(p);
        char sval[32];
        long ival = 0;
        bool is_str = false;
        if (*p == '"') {
            p = parse_str(p, sval, sizeof(sval));
            if (!p) {
                return -1;
            }
            is_str = true;
        } else if (!strncmp(p, "true", 4)) {
            ival = 1;
            p += 4;
        } else if (!strncmp(p, "false", 5)) {
            ival = 0;
            p += 5;
        } else if (*p == '-' || isdigit((unsigned char)*p)) {
            char *end;
            ival = strtol(p, &end, 10);
            if (end == p || *end == '.' || *end == 'e' || *end == 'E' || ival < -1000000 || ival > 1000000) {
                return -1;
            }
            p = end;
        } else {
            return -1; /* null, nested object/array, float */
        }

        if (!strcmp(key, "c")) {
            if (!is_str || strlen(sval) >= sizeof(out->c)) {
                return -1;
            }
            strcpy(out->c, sval);
            has_c = true;
        } else if (!strcmp(key, "k")) {
            if (!is_str || strlen(sval) >= sizeof(out->k)) {
                return -1;
            }
            strcpy(out->k, sval);
            out->has_k = true;
        } else if (!strcmp(key, "v")) {
            if (is_str) {
                return -1;
            }
            out->v = (int)ival;
            out->has_v = true;
        } else if (!strcmp(key, "id")) {
            if (is_str) {
                return -1;
            }
            out->id = (int)ival;
        } /* other keys: ignored, for later versions */

        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '}') {
            return -1;
        }
        p = skip_ws(p + 1);
        return *p == '\0' && has_c ? 0 : -1;
    }
}

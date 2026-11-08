/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host unit tests for main/link_proto.c and main/params.c. Writes one sample
 * of each message to argv[1] (JSON lines), which the Makefile checks with
 * Python's json module.
 */
#include <stdio.h>
#include <string.h>
#include "link_proto.h"
#include "params.h"

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static void test_parse(void)
{
    link_cmd_t c;
    CHECK(link_parse("{\"c\":\"hello\"}", &c) == 0 && !strcmp(c.c, "hello") && !c.has_k && !c.has_v && c.id == 0,
          "hello");
    CHECK(link_parse(" { \"c\" : \"set\" , \"k\":\"wide_db\", \"v\": 8, \"id\": 42 } ", &c) == 0 &&
              !strcmp(c.c, "set") && !strcmp(c.k, "wide_db") && c.has_k && c.has_v && c.v == 8 && c.id == 42,
          "set with spaces");
    CHECK(link_parse("{\"c\":\"set\",\"k\":\"beep\",\"v\":true}", &c) == 0 && c.v == 1, "true");
    CHECK(link_parse("{\"c\":\"set\",\"k\":\"beep\",\"v\":false}", &c) == 0 && c.v == 0 && c.has_v, "false");
    CHECK(link_parse("{\"c\":\"set\",\"v\":-3,\"k\":\"x\"}", &c) == 0 && c.v == -3, "negative");
    CHECK(link_parse("{\"c\":\"get\",\"future\":\"x\",\"n\":5}", &c) == 0 && !strcmp(c.c, "get"), "unknown keys");

    static const char *const bad[] = {
        "",
        "hello",
        "{}",
        "{\"k\":\"beep\"}",                       /* no c */
        "{\"c\":5}",                              /* c not a string */
        "{\"c\":\"set\",\"v\":\"8\"}",            /* v a string */
        "{\"c\":\"set\",\"v\":1.5}",              /* float */
        "{\"c\":\"set\",\"v\":null}",
        "{\"c\":\"set\",\"v\":[1]}",
        "{\"c\":\"set\",\"v\":{\"a\":1}}",
        "{\"c\":\"set\",\"v\":99999999}",         /* out of range */
        "{\"c\":\"he\\\"llo\"}",                  /* escapes not accepted */
        "{\"c\":\"hello\"",                        /* unterminated */
        "{\"c\":\"hello\"} x",                     /* trailing junk */
        "{\"c\":\"hello\",}",
        "{\"c\":\"averyveryverylongcommandname\"}", /* too long for c */
        "{\"c\":\"set\",\"k\":\"averyveryverylongsettingkey\"}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(link_parse(bad[i], &c) == -1, "accepted bad line %zu: %s", i, bad[i]);
    }
}

static void test_params(void)
{
    settings_t s;
    settings_default(&s);
    for (int p = 0; p < PARAM_COUNT; p++) {
        const param_info_t *i = &param_info[p];
        CHECK(param_find(i->key) == (param_t)p, "find %s", i->key);
        const int v = param_get(&s, (param_t)p);
        CHECK(v >= i->min && v <= i->max && (v - i->min) % i->step == 0, "default %s = %d in range", i->key, v);
        settings_t t = s;
        CHECK(param_set_key(&t, i->key, i->max) == 0 && param_get(&t, (param_t)p) == i->max, "set %s max", i->key);
        CHECK(settings_check(&t) == 0, "%s max passes settings_check", i->key);
        t = s;
        CHECK(param_set_key(&t, i->key, i->min) == 0 && settings_check(&t) == 0, "set %s min", i->key);
        t = s;
        CHECK(param_set_key(&t, i->key, i->max + i->step) == -1 && !memcmp(&t, &s, sizeof(s)), "%s above max",
              i->key);
        CHECK(param_set_key(&t, i->key, i->min - 1) == -1, "%s below min", i->key);
        if (i->step > 1) {
            CHECK(param_set_key(&t, i->key, i->min + 1) == -1, "%s off step", i->key);
        }
    }
    CHECK(param_find("nope") == PARAM_COUNT, "unknown key");
    settings_t t = s;
    CHECK(param_set_key(&t, "nope", 1) == -1, "set unknown key");
    CHECK(param_set_key(&t, PARAM_KEY_BEEP, 1) == 0 && t.beep, "beep on");
    CHECK(param_set_key(&t, PARAM_KEY_BEEP, 2) == -1, "beep 2");
    CHECK(param_set_key(&t, PARAM_KEY_SCAN, 2) == 0 && !t.scan[BAND_24] && t.scan[BAND_5] && param_scan_mask(&t) == 2,
          "scan 5 only");
    CHECK(param_set_key(&t, PARAM_KEY_SCAN, 0) == -1 && param_set_key(&t, PARAM_KEY_SCAN, 4) == -1, "scan none / bad");
    CHECK(param_clamp(PARAM_LEARN, 34) == 30 && param_clamp(PARAM_LEARN, 36) == 40 && param_clamp(PARAM_LEARN, 0) == 10,
          "clamp");
}

static void test_format(FILE *out)
{
    static char buf[LINK_LINE_MAX];
    size_t n = link_fmt_hello(buf, sizeof(buf), "aabbccddeeff", "1.1.0", "ESP32-C5-Touch-LCD-3.5");
    CHECK(n > 0 && n == strlen(buf) && buf[n - 1] == '\n' && !strncmp(buf, "{\"t\":\"hello\"", 12), "hello");
    fputs(buf, out);
    printf("hello: %zu bytes\n", n);

    link_status_t s;
    memset(&s, 0, sizeof(s));
    settings_default(&s.set);
    s.st[BAND_24].state = DETECT_ALARM;
    s.st[BAND_24].kind = DETECT_PULSED;
    s.st[BAND_24].alarms = 3;
    s.st[BAND_24].last_alarm_kind = DETECT_PULSED;
    s.st[BAND_24].m.floor_delta_cdb = -1234;
    s.st[BAND_5].state = DETECT_LEARNING;
    s.st[BAND_5].kind = DETECT_SWEPT; /* stale: reported as none while not SUSPECT/ALARM */
    s.st[BAND_5].learn_pct = 40;
    s.scanned[BAND_24] = s.scanned[BAND_5] = true;
    s.power = (link_power_t){.ok = true, .vbus = true, .battery = true, .percent = 85, .mv = 4012};
    s.uptime_s = 3600;
    s.boot = 7;
    n = link_fmt_status(buf, sizeof(buf), &s);
    CHECK(n > 0 && buf[n - 1] == '\n', "status");
    CHECK(strstr(buf, "\"2.4\":{\"scan\":true,\"state\":\"alarm\",\"kind\":\"pulsed\"") != NULL, "status 2.4: %s", buf);
    CHECK(strstr(buf, "\"5\":{\"scan\":true,\"state\":\"learning\",\"kind\":\"none\",\"learn_pct\":40") != NULL,
          "status 5: %s", buf);
    CHECK(strstr(buf, "\"floor_db\":-12.34") != NULL, "floor: %s", buf);
    CHECK(strstr(buf, "\"pct\":85,\"mv\":4012") != NULL, "battery: %s", buf);
    CHECK(strstr(buf, "\"scan\":3") != NULL && strstr(buf, "\"wide_db\":") != NULL, "settings: %s", buf);
    fputs(buf, out);
    printf("status: %zu bytes\n", n);
    s.power = (link_power_t){.ok = true, .vbus = true, .battery = false, .percent = -1};
    n = link_fmt_status(buf, sizeof(buf), &s);
    CHECK(n > 0 && !strstr(buf, "\"pct\""), "no battery: %s", buf);
    fputs(buf, out);
    s.power.ok = false;
    n = link_fmt_status(buf, sizeof(buf), &s);
    CHECK(n > 0 && !strstr(buf, "\"power\""), "power unknown: %s", buf);
    fputs(buf, out);

    detect_event_t ev = {.from = DETECT_SUSPECT, .to = DETECT_ALARM, .kind = DETECT_WIDEBAND, .t_ms = 12345};
    ev.m.floor_delta_cdb = 1550;
    ev.m.floor_delta_max_khz = 2437000;
    ev.m.swept_khz_per_s = -2500;
    n = link_fmt_event(buf, sizeof(buf), BAND_5, &ev, 7);
    CHECK(n > 0 && strstr(buf, "\"band\":\"5\",\"from\":\"suspect\",\"to\":\"alarm\",\"kind\":\"wideband\"") &&
              strstr(buf, "\"up\":12.3,") && strstr(buf, "\"max_cell_mhz\":2437.0") &&
              strstr(buf, "\"swept_mhz_per_s\":-2.50"),
          "event: %s", buf);
    fputs(buf, out);
    printf("event: %zu bytes\n", n);

    n = link_fmt_ack(buf, sizeof(buf), 5, NULL);
    CHECK(n > 0 && !strcmp(buf, "{\"t\":\"ack\",\"v\":1,\"id\":5,\"ok\":true}\n"), "ack ok: %s", buf);
    fputs(buf, out);
    n = link_fmt_ack(buf, sizeof(buf), 6, "bad key or value");
    CHECK(n > 0 && strstr(buf, "\"ok\":false,\"err\":\"bad key or value\"}"), "ack err: %s", buf);
    fputs(buf, out);

    char tiny[40];
    CHECK(link_fmt_hello(tiny, sizeof(tiny), "aabbccddeeff", "1.1.0", "x") == 0 && tiny[0] == '\0', "overflow");
}

int main(int argc, char **argv)
{
    FILE *out = fopen(argc > 1 ? argv[1] : "/dev/null", "w");
    if (!out) {
        perror("samples");
        return 1;
    }
    test_parse();
    test_params();
    test_format(out);
    fclose(out);
    if (failures) {
        printf("test_link: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_link: ok\n");
    return 0;
}

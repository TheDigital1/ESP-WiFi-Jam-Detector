/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two tasks: the writer owns all output (queue of events / acks, status on
 * request and every LINK_STATUS_MS), the reader assembles command lines and
 * runs them. Output goes through the console VFS (write() on stdout), which
 * holds one lock per write() call, so a message is never split by a log line;
 * the USB Serial/JTAG driver buffers it, and drops it if no host reads.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "board.h"
#include "detector.h"
#include "link.h"
#include "link_proto.h"
#include "settings.h"
#include "sweep.h"
#include "ui.h"

static const char *TAG = "link";

#define MODEL           "ESP32-C5-Touch-LCD-3.5"
#define QUEUE_LEN       8
#define TASK_PRIO       2
#define WRITE_PRIO      12   /* while holding the radio-quiet token (as eventlog.c) */
#define WRITER_STACK    3072 /* ~1.1 KB used (measured) */
#define READER_STACK    3072 /* ~1.5 KB used, with a settings save */
#define QUIET_MS        200  /* give up on this message if no window comes */
#define TX_DONE_MS      20   /* host reading: a 2 KB line takes ~2 ms at full speed */
#define POLL_MS         250
#define LINK_STATUS_MS  10000
#define HOST_TIMEOUT_MS 60000 /* no command this long: the host is gone, stop streaming */
#define STATUS_GAP_MS   500  /* at most two status lines per second */
#define TX_BUF          2048 /* driver ring buffers (logs share the TX one) */
#define RX_BUF          512

typedef enum { ITEM_EVENT, ITEM_HELLO, ITEM_ACK } item_kind_t;

typedef struct {
    item_kind_t kind;
    band_id_t band;
    int id;          /* ITEM_ACK */
    const char *err; /* ITEM_ACK: static string, NULL = ok */
    detect_event_t ev;
} item_t;

static QueueHandle_t s_queue;
static atomic_bool s_status_due;
static atomic_uint s_dropped;
static atomic_uint s_host_ms; /* time of the host's last command (never 0 once set) */
static char s_line[LINK_LINE_MAX];
static char s_id[13];

static bool post(const item_t *it)
{
    if (!s_queue || xQueueSend(s_queue, it, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1);
        return false;
    }
    return true;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* A host is reading (it sent a command lately). Only then are status and
 * events written: lines written without one would wait in the driver's TX
 * buffer and reach the next host minutes or hours late. */
static bool host_active(void)
{
    const uint32_t t = atomic_load(&s_host_ms);
    return t && now_ms() - t < HOST_TIMEOUT_MS;
}

void link_post_event(band_id_t band, const detect_event_t *ev)
{
    if (!host_active()) {
        return; /* still in the TF-card log */
    }
    const item_t it = {.kind = ITEM_EVENT, .band = band, .ev = *ev};
    post(&it);
    atomic_store(&s_status_due, true);
}

static void post_ack(int id, const char *err)
{
    const item_t it = {.kind = ITEM_ACK, .id = id, .err = err};
    post(&it);
}

/* One line, inside a radio-quiet window so USB traffic stays out of captures. */
static void send_line(size_t len)
{
    if (!len || !usb_serial_jtag_is_connected()) {
        return;
    }
    if (board_rf_quiet_begin(QUIET_MS) != ESP_OK) {
        atomic_fetch_add(&s_dropped, 1);
        return;
    }
    vTaskPrioritySet(NULL, WRITE_PRIO);
    if (write(fileno(stdout), s_line, len) < 0) {
        atomic_fetch_add(&s_dropped, 1);
    }
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(TX_DONE_MS)); /* times out if no host reads */
    vTaskPrioritySet(NULL, TASK_PRIO);
    board_rf_quiet_end();
}

static void send_status(void)
{
    static link_status_t s; /* ~200 B: off the stack */
    memset(&s, 0, sizeof(s));
    for (int b = 0; b < BAND_COUNT; b++) {
        detector_get_status((band_id_t)b, &s.st[b]);
        s.scanned[b] = sweep_scanned((band_id_t)b);
    }
    board_power_t p;
    if (board_power_read(&p) == ESP_OK) {
        s.power = (link_power_t){.ok = true, .vbus = p.vbus, .battery = p.battery, .charging = p.charging,
                                 .percent = p.percent, .mv = p.battery_mv};
    }
    settings_get(&s.set);
    s.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s.boot = settings_boot();
    send_line(link_fmt_status(s_line, sizeof(s_line), &s));
}

static void send_item(const item_t *it)
{
    switch (it->kind) {
    case ITEM_EVENT:
        send_line(link_fmt_event(s_line, sizeof(s_line), it->band, &it->ev, settings_boot()));
        break;
    case ITEM_HELLO:
        send_line(link_fmt_hello(s_line, sizeof(s_line), s_id, esp_app_get_description()->version, MODEL));
        atomic_store(&s_status_due, true);
        break;
    case ITEM_ACK:
        send_line(link_fmt_ack(s_line, sizeof(s_line), it->id, it->err));
        break;
    }
}

static void writer_task(void *arg)
{
    (void)arg;
    uint32_t last_status = 0, logged_dropped = 0;
    for (;;) {
        item_t it;
        if (xQueueReceive(s_queue, &it, pdMS_TO_TICKS(POLL_MS)) == pdTRUE) {
            send_item(&it);
        }
        const bool connected = host_active() && usb_serial_jtag_is_connected();
        const uint32_t t = now_ms();
        if (t - last_status >= LINK_STATUS_MS) {
            atomic_store(&s_status_due, true);
        }
        if (connected && atomic_load(&s_status_due) && t - last_status >= STATUS_GAP_MS &&
            uxQueueMessagesWaiting(s_queue) == 0) {
            atomic_store(&s_status_due, false);
            last_status = t;
            send_status();
        }
        const unsigned dropped = atomic_load(&s_dropped);
        if (dropped != logged_dropped && connected) {
            logged_dropped = dropped;
            ESP_LOGD(TAG, "%u messages dropped", dropped);
        }
    }
}

static void run(const char *line)
{
    link_cmd_t cmd;
    if (link_parse(line, &cmd) != 0) {
        post_ack(0, "parse");
        return;
    }
    atomic_store(&s_host_ms, now_ms() | 1u);
    if (!strcmp(cmd.c, "ping")) {
        post_ack(cmd.id, NULL); /* keeps the host active */
        return;
    }
    if (!strcmp(cmd.c, "hello")) {
        const item_t it = {.kind = ITEM_HELLO};
        post(&it);
        return; /* the hello is the answer */
    }
    if (!strcmp(cmd.c, "get")) {
        atomic_store(&s_status_due, true);
        post_ack(cmd.id, NULL);
        return;
    }
    if (!strcmp(cmd.c, "relearn")) {
        ESP_LOGI(TAG, "remote: re-learn baselines");
        detector_relearn();
        post_ack(cmd.id, NULL);
        atomic_store(&s_status_due, true);
        return;
    }
    if (!strcmp(cmd.c, "set")) {
        if (!cmd.has_k || !cmd.has_v) {
            post_ack(cmd.id, "missing k or v");
            return;
        }
        const esp_err_t err = ui_settings_set_key(cmd.k, cmd.v);
        post_ack(cmd.id, err == ESP_OK ? NULL
                         : err == ESP_ERR_INVALID_ARG ? "bad key or value"
                         : err == ESP_ERR_TIMEOUT ? "busy"
                                                  : "failed");
        atomic_store(&s_status_due, true);
        return;
    }
    post_ack(cmd.id, "unknown command");
}

static void reader_task(void *arg)
{
    (void)arg;
    static char line[LINK_CMD_MAX];
    size_t n = 0;
    bool overflow = false;
    for (;;) {
        uint8_t buf[64];
        const int got = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        for (int i = 0; i < got; i++) {
            const char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (n && !overflow) {
                    line[n] = '\0';
                    run(line);
                } else if (overflow) {
                    post_ack(0, "too long");
                }
                n = 0;
                overflow = false;
            } else if (n + 1 < sizeof(line)) {
                line[n++] = c;
            } else {
                overflow = true;
            }
        }
    }
}

esp_err_t link_start(void)
{
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(mac), TAG, "mac");
    snprintf(s_id, sizeof(s_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = TX_BUF;
    cfg.rx_buffer_size = RX_BUF;
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&cfg), TAG, "USB Serial/JTAG driver");
    usb_serial_jtag_vfs_use_driver(); /* console log through the driver too */

    s_queue = xQueueCreate(QUEUE_LEN, sizeof(item_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_FALSE(xTaskCreate(writer_task, "link_tx", WRITER_STACK, NULL, TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "writer task");
    ESP_RETURN_ON_FALSE(xTaskCreate(reader_task, "link_rx", READER_STACK, NULL, TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "reader task");
    ESP_LOGI(TAG, "USB link up (id %s, protocol %d)", s_id, LINK_PROTO_VERSION);
    return ESP_OK;
}

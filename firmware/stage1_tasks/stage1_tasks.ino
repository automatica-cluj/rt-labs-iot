// stage1_tasks - the same device as stage 0, built from tasks and queues
//
// Stage 0 does everything in one loop, and the 10 ms control step stops
// whenever the network code blocks. Here the work is split by urgency:
//
//   sample     prio 15  the control step, released every 10 ms on absolute
//                       time. Closes a window of min / avg / max every second
//                       and hands it to a queue. Never calls the network.
//   command    prio 12  waits on a queue for commands and applies them.
//                       Owns the LED.
//   mqtt_task  prio 5   the esp-mqtt client (created by the library). Connects,
//                       reconnects and receives. Its event handler only copies
//                       a message into a queue, the way an ISR would.
//   telemetry  prio 3   takes windows and events from a queue, formats and
//                       publishes. The only task of ours that calls the network.
//   loop()     prio 1   serial keys and reports.
//
// The Wi-Fi driver (23) and esp_timer (22) run above all of these.
//
// Nothing here waits for the network: setup() starts Wi-Fi and the MQTT client
// and returns. No heap is used by our code after setup(), and no String.
//
// Keys in the serial monitor, the same as in stage 0:
//   a   measure the control period for 20 s of normal operation
//   x   the same with the broker made unreachable for 15 s
//   s   stack and heap figures
//   h   this list
//
// Each run of a or x ends with one VERDICT line.
//
// Needs a secrets.h (copy secrets.h.example). No extra library.

#include <WiFi.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "rt.h"
#include "secrets.h"

// ---- EXPERIMENT KNOBS ----
const uint32_t kControlPeriodMs = 10;
const uint32_t kWindowSteps = 100;      // control steps in one window: 1 s
const uint32_t kTelemetryWindows = 5;   // windows in one telemetry round: 5 s
// The chip read 43 to 50 C with Wi-Fi on when this was written. Set the
// threshold below the value you see to watch the alarm topic change.
const int32_t kAlarmOnMc = 55000;   // alarm on above this, in millidegrees C
const int32_t kAlarmOffMc = 53000;  // and off again below this
const int kFilterShift = 4;         // filter weight 1/16 for each new sample
const uint32_t kMeasureSeconds = 20;
const uint32_t kFaultSeconds = 15;
const char* kBadUri = "mqtt://no-such-broker.invalid:1883";

#define PRIO_SAMPLE 15
#define PRIO_COMMAND 12
#define PRIO_MQTT 5
#define PRIO_TELEMETRY 3

// ---------------------------------------------------------- messages ------

// What travels to the telemetry task. One queue carries all kinds, so the
// telemetry task has a single place to wait.
#define TELE_WINDOW 1     // a closed window from the sample task
#define TELE_LED 2        // the LED changed (from the command task)
#define TELE_LINK_UP 3    // MQTT connected (from the MQTT event handler)
#define TELE_LINK_DOWN 4  // MQTT disconnected

struct tele_msg {
    int kind;
    // TELE_WINDOW
    int64_t sum_mc;
    uint32_t n;
    int32_t min_mc, max_mc;
    bool alarm;
    int32_t max_gap_us;  // longest gap between two control steps in the window
    uint32_t late;       // gaps of two periods or more
    uint32_t dropped;    // windows the sample task could not queue before this one
    // TELE_LED
    bool led_on;
};

#define CMD_LED 1

struct cmd_msg {
    int kind;
    char data[16];
};

static QueueHandle_t g_tele_q = NULL;
static QueueHandle_t g_cmd_q = NULL;
static esp_mqtt_client_handle_t g_client = NULL;
static TaskHandle_t g_h_sample, g_h_command, g_h_telemetry;

// Topic strings and other text that never changes, built once in setup().
static char g_device_id[13];
static char g_mac_text[18];
static char g_uri[96];
static char g_t_status[48], g_t_rssi[48], g_t_heap[48], g_t_channel[48], g_t_ip[48], g_t_mac[48];
static char g_t_led_state[48], g_t_alarm[48], g_t_temperature[48], g_t_led[48];

// ------------------------------------------------------- measurement ------

// The sample task writes these and loop() reads and resets them, so they sit
// behind a mutex. A FreeRTOS mutex has priority inheritance: if loop() holds
// it when the sample task needs it, loop() runs at priority 15 until it lets
// go. loop() only copies under the lock and prints afterwards.
struct measurement {
    bool active;
    struct rt_stats gap;
    uint32_t late;
};

static SemaphoreHandle_t g_meas_lock = NULL;
static struct measurement g_meas;

static bool meas_is_active(void) {
    xSemaphoreTake(g_meas_lock, portMAX_DELAY);
    bool a = g_meas.active;
    xSemaphoreGive(g_meas_lock);
    return a;
}

// ------------------------------------------------------- sample task ------

static void sample_task(void* arg) {
    const TickType_t period = pdMS_TO_TICKS(kControlPeriodMs);
    int32_t filt_mc = (int32_t)(temperatureRead() * 1000.0f);
    bool alarm = false;
    int64_t last_us = 0;
    uint32_t dropped = 0;
    struct tele_msg w;
    memset(&w, 0, sizeof(w));
    w.kind = TELE_WINDOW;
    w.min_mc = INT32_MAX;
    w.max_mc = INT32_MIN;

    TickType_t next = xTaskGetTickCount();
    for (;;) {
        next += period;
        rt_sleep_until_tick(next);  // absolute: no drift, whatever the step costs

        int64_t t = rt_now_us();
        if (last_us != 0) {
            int64_t gap64 = t - last_us;
            int32_t gap = gap64 > INT32_MAX ? INT32_MAX : (int32_t)gap64;
            bool is_late = gap > (int32_t)(2 * kControlPeriodMs * 1000);
            if (gap > w.max_gap_us) w.max_gap_us = gap;
            if (is_late) w.late++;
            xSemaphoreTake(g_meas_lock, portMAX_DELAY);
            if (g_meas.active) {
                rt_stats_add(&g_meas.gap, gap);
                if (is_late) g_meas.late++;
            }
            xSemaphoreGive(g_meas_lock);
        }
        last_us = t;

        // The control step, the same as in stage 0.
        int32_t raw_mc = (int32_t)(temperatureRead() * 1000.0f);
        filt_mc += (raw_mc - filt_mc) >> kFilterShift;
        if (filt_mc > kAlarmOnMc) alarm = true;
        if (filt_mc < kAlarmOffMc) alarm = false;

        if (filt_mc < w.min_mc) w.min_mc = filt_mc;
        if (filt_mc > w.max_mc) w.max_mc = filt_mc;
        w.sum_mc += filt_mc;
        w.n++;

        if (w.n >= kWindowSteps) {
            w.alarm = alarm;
            w.dropped = dropped;
            // Timeout 0: this task never waits for the telemetry task. A full
            // queue costs one window, and the next one says so.
            if (xQueueSend(g_tele_q, &w, 0) == pdTRUE)
                dropped = 0;
            else
                dropped++;
            memset(&w, 0, sizeof(w));
            w.kind = TELE_WINDOW;
            w.min_mc = INT32_MAX;
            w.max_mc = INT32_MIN;
        }
    }
}

// ------------------------------------------------------ command task ------

static void command_task(void* arg) {
    bool led_on = false;
    struct cmd_msg c;
    struct tele_msg ev;
    for (;;) {
        xQueueReceive(g_cmd_q, &c, portMAX_DELAY);
        if (c.kind != CMD_LED) continue;
        if (strcmp(c.data, "on") == 0)
            led_on = true;
        else if (strcmp(c.data, "off") == 0)
            led_on = false;
        else
            continue;
        digitalWrite(LED_PIN, led_on ? LOW : HIGH);  // lit when LOW

        // Report the change through the telemetry task; this task stays off
        // the network.
        memset(&ev, 0, sizeof(ev));
        ev.kind = TELE_LED;
        ev.led_on = led_on;
        xQueueSend(g_tele_q, &ev, 0);
    }
}

// ---------------------------------------------------- telemetry task ------

static uint32_t g_pub_failed = 0;  // used by the telemetry task only

static void pub(const char* topic, const char* payload) {
    // QoS 0, not retained. Returns -1 at once when the client is not connected.
    if (esp_mqtt_client_publish(g_client, topic, payload, 0, 0, 0) < 0) g_pub_failed++;
}

// 43512 -> "43.5"
static void fmt_mc(char* buf, size_t size, int32_t mc) {
    int32_t tenths = (mc + (mc >= 0 ? 50 : -50)) / 100;
    int32_t a = tenths < 0 ? -tenths : tenths;
    snprintf(buf, size, "%s%ld.%ld", tenths < 0 ? "-" : "", (long)(a / 10), (long)(a % 10));
}

// One telemetry round: nine publishes. Called by the telemetry task only.
static void publish_round(bool led_on, bool alarm, const char* t_avg) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)(rt_now_us() / 1000000));
    pub(g_t_status, buf);
    snprintf(buf, sizeof(buf), "%d", (int)WiFi.RSSI());
    pub(g_t_rssi, buf);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)ESP.getFreeHeap());
    pub(g_t_heap, buf);
    snprintf(buf, sizeof(buf), "%ld", (long)WiFi.channel());
    pub(g_t_channel, buf);
    IPAddress ip = WiFi.localIP();
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    pub(g_t_ip, buf);
    pub(g_t_mac, g_mac_text);
    pub(g_t_led_state, led_on ? "on" : "off");
    pub(g_t_alarm, alarm ? "on" : "off");
    // Temperature goes last: the dashboard stores a history row when it
    // arrives, with the other values as they are at that moment.
    pub(g_t_temperature, t_avg);
}

static void telemetry_task(void* arg) {
    struct tele_msg m;
    bool led_on = false, link_up = false, alarm = false;
    // The round being collected: several windows merged.
    int64_t sum_mc = 0;
    uint32_t n = 0, windows = 0, late = 0, dropped = 0;
    int32_t min_mc = INT32_MAX, max_mc = INT32_MIN, max_gap_us = 0;
    char t_avg[12], t_min[12], t_max[12];

    for (;;) {
        xQueueReceive(g_tele_q, &m, portMAX_DELAY);

        if (m.kind == TELE_LED) {
            led_on = m.led_on;
            pub(g_t_led_state, led_on ? "on" : "off");
            if (!meas_is_active()) Serial.printf("LED %s\n", led_on ? "on" : "off");
            continue;
        }
        if (m.kind == TELE_LINK_UP || m.kind == TELE_LINK_DOWN) {
            link_up = (m.kind == TELE_LINK_UP);
            if (!meas_is_active()) Serial.printf("MQTT %s\n", link_up ? "connected" : "disconnected");
            continue;
        }
        if (m.kind != TELE_WINDOW) continue;

        sum_mc += m.sum_mc;
        n += m.n;
        if (m.min_mc < min_mc) min_mc = m.min_mc;
        if (m.max_mc > max_mc) max_mc = m.max_mc;
        if (m.max_gap_us > max_gap_us) max_gap_us = m.max_gap_us;
        late += m.late;
        dropped += m.dropped;
        alarm = m.alarm;
        if (++windows < kTelemetryWindows) continue;

        // A round is complete. With the link down it is not published: a
        // publish would wait for the client while the client is busy trying to
        // connect, and the windows would pile up in the queue behind it.
        fmt_mc(t_avg, sizeof(t_avg), (int32_t)(sum_mc / (int64_t)n));
        int64_t t0 = rt_now_us();
        g_pub_failed = 0;
        if (link_up) publish_round(led_on, alarm, t_avg);
        long round_ms = (long)((rt_now_us() - t0) / 1000);

        if (!meas_is_active()) {
            fmt_mc(t_min, sizeof(t_min), min_mc);
            fmt_mc(t_max, sizeof(t_max), max_mc);
            Serial.printf("Temp: %s (min %s max %s, %lu samples) | alarm %s | RSSI: %d | Heap: %lu | Uptime: %lus | MQTT %s\n",
                          t_avg, t_min, t_max, (unsigned long)n, alarm ? "on" : "off", (int)WiFi.RSSI(),
                          (unsigned long)ESP.getFreeHeap(), (unsigned long)(rt_now_us() / 1000000),
                          link_up ? "up" : "down");
            Serial.printf("control: longest gap %ld ms, %lu late steps in this round | %lu windows dropped | ",
                          (long)(max_gap_us / 1000), (unsigned long)late, (unsigned long)dropped);
            if (link_up)
                Serial.printf("publishing took %ld ms, %lu of 9 failed\n", round_ms, (unsigned long)g_pub_failed);
            else
                Serial.println("not published, link down");
        }
        sum_mc = 0;
        n = windows = late = dropped = 0;
        min_mc = INT32_MAX;
        max_mc = INT32_MIN;
        max_gap_us = 0;
    }
}

// ------------------------------------------------ MQTT event handler ------

// Runs in mqtt_task. It does the minimum and never waits: whatever has to be
// done about an event is done by the task that receives the message.
static void on_mqtt(void* arg, esp_event_base_t base, int32_t id, void* data) {
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    struct tele_msg ev;
    struct cmd_msg c;

    if (id == MQTT_EVENT_CONNECTED) {
        esp_mqtt_client_subscribe(g_client, g_t_led, 0);
        memset(&ev, 0, sizeof(ev));
        ev.kind = TELE_LINK_UP;
        xQueueSend(g_tele_q, &ev, 0);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        memset(&ev, 0, sizeof(ev));
        ev.kind = TELE_LINK_DOWN;
        xQueueSend(g_tele_q, &ev, 0);
    } else if (id == MQTT_EVENT_DATA) {
        // topic is not terminated by 0, and a long message may come in pieces.
        if (e->current_data_offset != 0 || e->topic == NULL) return;
        if (e->topic_len != (int)strlen(g_t_led) || memcmp(e->topic, g_t_led, e->topic_len) != 0) return;
        if (e->data_len <= 0 || e->data_len >= (int)sizeof(c.data)) return;
        memset(&c, 0, sizeof(c));
        c.kind = CMD_LED;
        memcpy(c.data, e->data, e->data_len);
        xQueueSend(g_cmd_q, &c, 0);
    }
}

// ------------------------------------------------------- experiments ------

static uint32_t g_measure_end_ms = 0;  // used by loop() only; 0 = none running
static uint32_t g_measure_steps_due = 0;
static uint32_t g_fault_end_ms = 0;  // 0 = the broker address is the good one

static void measure_start(uint32_t seconds) {
    xSemaphoreTake(g_meas_lock, portMAX_DELAY);
    rt_stats_init(&g_meas.gap, 2000);
    g_meas.late = 0;
    g_meas.active = true;
    xSemaphoreGive(g_meas_lock);
    g_measure_steps_due = seconds * 1000 / kControlPeriodMs;
    g_measure_end_ms = millis() + seconds * 1000;
    if (g_measure_end_ms == 0) g_measure_end_ms = 1;
}

static void measure_report(void) {
    static struct rt_stats gap;  // a copy, so that printing happens outside the lock
    uint32_t late;
    char line[96];

    xSemaphoreTake(g_meas_lock, portMAX_DELAY);
    g_meas.active = false;
    gap = g_meas.gap;
    late = g_meas.late;
    xSemaphoreGive(g_meas_lock);

    rt_stats_print(&gap, "gap between control steps", "us");
    rt_stats_print_hist(&gap, "us");
    Serial.printf("steps run: %lu of %lu due\n", (unsigned long)gap.n, (unsigned long)g_measure_steps_due);
    // Two ways to lose the period: a gap of two periods or more, or fewer steps
    // than the window holds (2 % is allowed for the edges of the window).
    bool short_count = gap.n < g_measure_steps_due - g_measure_steps_due / 50;
    long max_ms = gap.n ? (long)(gap.max / 1000) : 0;
    if (gap.n == 0)
        snprintf(line, sizeof(line), "control period LOST: the control step did not run at all");
    else if (late == 0 && !short_count)
        snprintf(line, sizeof(line), "control period kept: longest gap %ld ms", max_ms);
    else
        snprintf(line, sizeof(line), "control period LOST: %lu of %lu steps ran, longest gap %ld ms",
                 (unsigned long)gap.n, (unsigned long)g_measure_steps_due, max_ms);
    rt_verdict(line);
}

// The library copies the address, which uses the heap. That is accepted here:
// it happens on a key press, in loop(), for the experiment only.
static void fault_start(void) {
    Serial.printf("broker address set to %s for %lu s\n", kBadUri, (unsigned long)kFaultSeconds);
    g_fault_end_ms = millis() + kFaultSeconds * 1000;
    if (g_fault_end_ms == 0) g_fault_end_ms = 1;
    esp_mqtt_client_set_uri(g_client, kBadUri);
    esp_mqtt_client_disconnect(g_client);  // the client then reconnects by itself
}

static void fault_poll(void) {
    if (g_fault_end_ms != 0 && (int32_t)(millis() - g_fault_end_ms) >= 0) {
        g_fault_end_ms = 0;
        esp_mqtt_client_set_uri(g_client, g_uri);
        Serial.println("broker address restored");
    }
}

static void print_figures(void) {
    Serial.printf("stack never used, bytes: sample %lu, command %lu, telemetry %lu\n",
                  (unsigned long)uxTaskGetStackHighWaterMark(g_h_sample),
                  (unsigned long)uxTaskGetStackHighWaterMark(g_h_command),
                  (unsigned long)uxTaskGetStackHighWaterMark(g_h_telemetry));
    Serial.printf("heap: %lu free now, %lu at the lowest point since boot\n", (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMinFreeHeap());
}

static void print_help(void) {
    Serial.println("keys: a = measure 20 s, x = measure with the broker unreachable, s = stack and heap, h = help");
}

// ------------------------------------------------------ setup / loop ------

static void build_topic(char* out, size_t size, const char* field) {
    snprintf(out, size, "devices/%s/%s", g_device_id, field);
}

void setup() {
    rt_begin("stage 1: tasks and queues");
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED off at start (lit when LOW)

    // ESP-IDF reports every failed connection on this serial port. The sketch
    // reports the link itself, so those lines are switched off.
    esp_log_level_set("*", ESP_LOG_NONE);

    // The device id is the MAC address. Reading it does not need Wi-Fi up.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_device_id, sizeof(g_device_id), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    snprintf(g_mac_text, sizeof(g_mac_text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    snprintf(g_uri, sizeof(g_uri), "mqtt://%s:%d", MQTT_HOST, (int)MQTT_PORT);
    build_topic(g_t_status, sizeof(g_t_status), "status");
    build_topic(g_t_rssi, sizeof(g_t_rssi), "rssi");
    build_topic(g_t_heap, sizeof(g_t_heap), "heap");
    build_topic(g_t_channel, sizeof(g_t_channel), "wifi_channel");
    build_topic(g_t_ip, sizeof(g_t_ip), "ip");
    build_topic(g_t_mac, sizeof(g_t_mac), "mac");
    build_topic(g_t_led_state, sizeof(g_t_led_state), "led_state");
    build_topic(g_t_alarm, sizeof(g_t_alarm), "alarm");
    build_topic(g_t_temperature, sizeof(g_t_temperature), "temperature");
    build_topic(g_t_led, sizeof(g_t_led), "led");
    Serial.printf("Device ID: %s, broker %s\n", g_device_id, g_uri);

    g_tele_q = xQueueCreate(8, sizeof(struct tele_msg));
    g_cmd_q = xQueueCreate(4, sizeof(struct cmd_msg));
    g_meas_lock = xSemaphoreCreateMutex();
    if (g_tele_q == NULL || g_cmd_q == NULL || g_meas_lock == NULL) rt_fatal("out of memory at start-up");
    rt_stats_init(&g_meas.gap, 2000);

    // Wi-Fi: start the join and go on. The driver joins, and joins again after
    // a loss, by itself.
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // MQTT: the same. The client task connects when the network allows it and
    // tries again every 2 s when it does not.
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = g_uri;
    cfg.credentials.client_id = g_device_id;
    cfg.network.reconnect_timeout_ms = 2000;
    cfg.network.timeout_ms = 5000;
    cfg.task.priority = PRIO_MQTT;
    cfg.task.stack_size = 6144;
    g_client = esp_mqtt_client_init(&cfg);
    if (g_client == NULL) rt_fatal("esp_mqtt_client_init failed");
    esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY, on_mqtt, NULL);
    esp_mqtt_client_start(g_client);

    g_h_telemetry = rt_start_task(telemetry_task, "telemetry", PRIO_TELEMETRY, NULL);
    g_h_command = rt_start_task(command_task, "command", PRIO_COMMAND, NULL);
    g_h_sample = rt_start_task(sample_task, "sample", PRIO_SAMPLE, NULL);
    print_help();
}

void loop() {
    fault_poll();

    if (g_measure_end_ms != 0 && (int32_t)(millis() - g_measure_end_ms) >= 0) {
        g_measure_end_ms = 0;
        measure_report();
    }

    int key = rt_read_key();
    if (key == 'a' && g_measure_end_ms == 0) {
        Serial.printf("measuring for %lu s...\n", (unsigned long)kMeasureSeconds);
        measure_start(kMeasureSeconds);
    } else if (key == 'x' && g_measure_end_ms == 0) {
        measure_start(kFaultSeconds + 10);
        fault_start();
    } else if (key == 's') {
        print_figures();
    } else if (key == 'h') {
        print_help();
    }

    delay(10);
}

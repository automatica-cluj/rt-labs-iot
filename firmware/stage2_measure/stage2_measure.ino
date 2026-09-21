// stage2_measure - the stage 1 device, measuring itself
//
// The tasks, priorities and queues are those of stage 1:
//
//   sample     prio 15  the 10 ms control step. Never calls the network
//                       (except in experiment c, on purpose).
//   command    prio 12  applies commands, owns the LED.
//   mqtt_task  prio 5   the esp-mqtt client; its event handler only queues.
//   telemetry  prio 3   formats and publishes.
//   loop()     prio 1   serial keys, experiments and reports.
//
// New here is what the device knows about its own timing:
//
//   release jitter   how late the sample task starts, against the ideal
//                    release time t0 + k * 10 ms
//   period misses    releases that came a whole period late or more
//   step time        from the start to the end of one control step. It is
//                    elapsed time: a step that the Wi-Fi driver interrupts
//                    comes out longer, though it did the same work.
//   command latency  from the MQTT event handler to the command task having
//                    applied the command
//   round trip       of a message sent to the broker and received back
//
// Everything is recorded during a run and printed after it.
//
// Keys in the serial monitor:
//   a   radio off: the baseline jitter of the control step
//   b   connected, publishing through the queue: the normal design
//   c   publishing from inside the control task, the superloop way, with the
//       broker made unreachable for 15 s in the middle
//   d   the normal design with the broker made unreachable for 15 s (x too,
//       the key it has in stages 0 and 1)
//   e   command latency and round trip, with 20 messages sent to ourselves
//   s   stack and heap figures
//   h   this list
//
// Each experiment ends with one VERDICT line. a and d need no network.
//
// New topics, ignored by the dashboard: jitter_max_us, period_misses,
// queue_drops, cmd_latency_us (device to broker) and ping (a command).
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
const uint32_t kWindowSteps = 100;     // control steps in one window: 1 s
const uint32_t kTelemetryWindows = 5;  // windows in one telemetry round: 5 s
// The chip read 43 to 58 C with Wi-Fi connected when this was written, and
// more with the radio off or searching. Set the threshold to suit what you see.
const int32_t kAlarmOnMc = 55000;   // alarm on above this, in millidegrees C
const int32_t kAlarmOffMc = 53000;  // and off again below this
const int kFilterShift = 4;         // filter weight 1/16 for each new sample
const uint32_t kMeasureSeconds = 20;
const uint32_t kFaultSeconds = 15;
const int32_t kJitterBucketUs = 200;  // width of one histogram bucket
const int kPings = 20;
// An address nothing answers from: the connection attempt runs into its
// timeout, which is what a broker that is down looks like.
const char* kBadUri = "mqtt://10.255.255.1:1883";

#define PRIO_SAMPLE 15
#define PRIO_COMMAND 12
#define PRIO_MQTT 5
#define PRIO_TELEMETRY 3

// ---------------------------------------------------------- messages ------

#define TELE_WINDOW 1     // a closed window from the sample task
#define TELE_LED 2        // the LED changed (from the command task)
#define TELE_CMD 3        // another command was applied (from the command task)
#define TELE_LINK_UP 4    // MQTT connected (from the MQTT event handler)
#define TELE_LINK_DOWN 5  // MQTT disconnected

struct tele_msg {
    int kind;
    // TELE_WINDOW
    int64_t sum_mc;
    uint32_t n;
    int32_t min_mc, max_mc;
    bool alarm;
    int32_t max_late_us;  // worst release jitter in the window
    uint32_t misses;      // releases a whole period late or more
    uint32_t dropped;     // windows the sample task could not queue before this one
    // TELE_LED
    bool led_on;
    // TELE_LED and TELE_CMD
    int32_t latency_us;
};

#define CMD_LED 1
#define CMD_PING 2

struct cmd_msg {
    int kind;
    char data[16];
    int64_t t_event_us;  // taken in the MQTT event handler
};

static QueueHandle_t g_tele_q = NULL;
static QueueHandle_t g_cmd_q = NULL;
static esp_mqtt_client_handle_t g_client = NULL;
static TaskHandle_t g_h_sample, g_h_command, g_h_telemetry;

// Text that never changes, built once in setup().
static char g_device_id[13];
static char g_mac_text[18];
static char g_uri[96];
static char g_t_status[48], g_t_rssi[48], g_t_heap[48], g_t_channel[48], g_t_ip[48], g_t_mac[48];
static char g_t_led_state[48], g_t_alarm[48], g_t_temperature[48], g_t_led[48], g_t_ping[48];
static char g_t_jitter[48], g_t_misses[48], g_t_drops[48], g_t_cmd_latency[48];

// ------------------------------------------------------- shared state ------

// Everything that more than one task touches, behind one mutex. A FreeRTOS
// mutex has priority inheritance: a low task that holds it runs at the
// priority of the highest task waiting for it. Every holder only copies or
// adds a number under the lock; nothing is printed or sent while holding it.
struct shared {
    bool measuring;          // the sample task records into jitter and step_time
    bool publish_in_sample;  // experiment c
    bool link_up;            // written by the telemetry task
    struct rt_stats jitter;  // us
    struct rt_stats step_time;  // us, elapsed from start to end of a step
    uint32_t steps, misses;
    uint32_t rounds_published;
    // experiment e
    int64_t ping_sent_us;  // 0 = no ping waiting for its answer
    uint32_t pings_answered;
    struct rt_stats cmd_latency;  // us
    struct rt_stats round_trip;   // us
};

static SemaphoreHandle_t g_lock = NULL;
static struct shared g_sh;

static void lock(void) { xSemaphoreTake(g_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(g_lock); }

static bool is_measuring(void) {
    lock();
    bool v = g_sh.measuring;
    unlock();
    return v;
}

// -------------------------------------------------------- publishing ------

// QoS 0, not retained. Returns false when the message was not sent.
static bool pub(const char* topic, const char* payload) {
    return esp_mqtt_client_publish(g_client, topic, payload, 0, 0, 0) >= 0;
}

// 43512 -> "43.5"
static void fmt_mc(char* buf, size_t size, int32_t mc) {
    int32_t tenths = (mc + (mc >= 0 ? 50 : -50)) / 100;
    int32_t a = tenths < 0 ? -tenths : tenths;
    snprintf(buf, size, "%s%ld.%ld", tenths < 0 ? "-" : "", (long)(a / 10), (long)(a % 10));
}

// One telemetry round. led is 1, 0, or -1 when the caller does not know it.
// Returns how many publishes failed.
static uint32_t publish_round(int led, bool alarm, int32_t avg_mc) {
    char buf[32];
    uint32_t failed = 0;
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)(rt_now_us() / 1000000));
    if (!pub(g_t_status, buf)) failed++;
    snprintf(buf, sizeof(buf), "%d", (int)WiFi.RSSI());
    if (!pub(g_t_rssi, buf)) failed++;
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)ESP.getFreeHeap());
    if (!pub(g_t_heap, buf)) failed++;
    snprintf(buf, sizeof(buf), "%ld", (long)WiFi.channel());
    if (!pub(g_t_channel, buf)) failed++;
    IPAddress ip = WiFi.localIP();
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    if (!pub(g_t_ip, buf)) failed++;
    if (!pub(g_t_mac, g_mac_text)) failed++;
    if (led >= 0 && !pub(g_t_led_state, led ? "on" : "off")) failed++;
    if (!pub(g_t_alarm, alarm ? "on" : "off")) failed++;
    // Temperature goes last: the dashboard stores a history row when it
    // arrives, with the other values as they are at that moment.
    fmt_mc(buf, sizeof(buf), avg_mc);
    if (!pub(g_t_temperature, buf)) failed++;
    if (failed == 0) {
        lock();
        g_sh.rounds_published++;
        unlock();
    }
    return failed;
}

// ------------------------------------------------------- sample task ------

static void sample_task(void* arg) {
    const TickType_t period = pdMS_TO_TICKS(kControlPeriodMs);
    const int32_t period_us = (int32_t)kControlPeriodMs * 1000;
    int32_t filt_mc = (int32_t)(temperatureRead() * 1000.0f);
    bool alarm = false;
    uint32_t dropped = 0;
    // experiment c: a round of our own, collected like the telemetry task does
    int64_t c_sum = 0;
    uint32_t c_n = 0, c_windows = 0;
    struct tele_msg w;
    memset(&w, 0, sizeof(w));
    w.kind = TELE_WINDOW;
    w.min_mc = INT32_MAX;
    w.max_mc = INT32_MIN;

    // The release time is known in ticks, the clock we measure with counts
    // microseconds from another starting point. The difference between the
    // two is found first: over 2 s, the smallest "now minus release tick" is
    // the release that nothing delayed.
    TickType_t next = xTaskGetTickCount();
    int64_t offset_us = INT64_MAX;
    for (int i = 0; i < 200; i++) {
        next += period;
        rt_sleep_until_tick(next);
        int64_t d = rt_now_us() - (int64_t)next * 1000;  // 1 tick = 1 ms
        if (d < offset_us) offset_us = d;
    }

    for (;;) {
        next += period;
        rt_sleep_until_tick(next);  // absolute: no drift, whatever the step costs
        int32_t late_us = (int32_t)(rt_now_us() - (int64_t)next * 1000 - offset_us);
        uint32_t c0 = rt_cycles();

        // The control step, the same as in stages 0 and 1.
        int32_t raw_mc = (int32_t)(temperatureRead() * 1000.0f);
        filt_mc += (raw_mc - filt_mc) >> kFilterShift;
        if (filt_mc > kAlarmOnMc) alarm = true;
        if (filt_mc < kAlarmOffMc) alarm = false;

        if (filt_mc < w.min_mc) w.min_mc = filt_mc;
        if (filt_mc > w.max_mc) w.max_mc = filt_mc;
        w.sum_mc += filt_mc;
        w.n++;
        int32_t step_us = (int32_t)rt_cycles_to_us(rt_cycles() - c0);  // elapsed, not CPU time

        bool miss = late_us >= period_us;
        if (late_us > w.max_late_us) w.max_late_us = late_us;
        if (miss) w.misses++;
        lock();
        bool publish_here = g_sh.publish_in_sample;
        if (g_sh.measuring) {
            rt_stats_add(&g_sh.jitter, late_us);
            rt_stats_add(&g_sh.step_time, step_us);
            g_sh.steps++;
            if (miss) g_sh.misses++;
        }
        unlock();

        if (w.n >= kWindowSteps) {
            w.alarm = alarm;
            w.dropped = dropped;
            c_sum += w.sum_mc;
            c_n += w.n;
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

            if (++c_windows >= kTelemetryWindows) {
                // Experiment c only: the network call made from the control
                // task. This is the thing not to do.
                if (publish_here) publish_round(-1, alarm, (int32_t)(c_sum / (int64_t)c_n));
                c_sum = 0;
                c_n = c_windows = 0;
            }
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
        memset(&ev, 0, sizeof(ev));

        if (c.kind == CMD_LED) {
            if (strcmp(c.data, "on") == 0)
                led_on = true;
            else if (strcmp(c.data, "off") == 0)
                led_on = false;
            else
                continue;
            digitalWrite(LED_PIN, led_on ? LOW : HIGH);  // lit when LOW
            ev.kind = TELE_LED;
            ev.led_on = led_on;
        } else if (c.kind == CMD_PING) {
            ev.kind = TELE_CMD;  // a ping has nothing to apply
        } else {
            continue;
        }

        // The command is applied. How long since the event handler saw it?
        int64_t latency = rt_now_us() - c.t_event_us;
        ev.latency_us = latency > INT32_MAX ? INT32_MAX : (int32_t)latency;
        lock();
        rt_stats_add(&g_sh.cmd_latency, ev.latency_us);
        if (c.kind == CMD_PING && g_sh.ping_sent_us != 0) {
            rt_stats_add(&g_sh.round_trip, (int32_t)(c.t_event_us - g_sh.ping_sent_us));
            g_sh.ping_sent_us = 0;
            g_sh.pings_answered++;
        }
        unlock();

        // Reported through the telemetry task; this task stays off the network.
        xQueueSend(g_tele_q, &ev, 0);
    }
}

// ---------------------------------------------------- telemetry task ------

static void telemetry_task(void* arg) {
    struct tele_msg m;
    bool led_on = false, link_up = false, alarm = false;
    // The round being collected: several windows merged.
    int64_t sum_mc = 0;
    uint32_t n = 0, windows = 0, misses = 0, dropped = 0;
    int32_t min_mc = INT32_MAX, max_mc = INT32_MIN, max_late_us = 0, cmd_latency_us = 0;
    char buf[16], t_avg[12], t_min[12], t_max[12];

    for (;;) {
        xQueueReceive(g_tele_q, &m, portMAX_DELAY);

        if (m.kind == TELE_LED || m.kind == TELE_CMD) {
            if (m.latency_us > cmd_latency_us) cmd_latency_us = m.latency_us;
            if (m.kind == TELE_CMD) continue;
            led_on = m.led_on;
            if (link_up) pub(g_t_led_state, led_on ? "on" : "off");
            if (!is_measuring()) Serial.printf("LED %s, applied %ld us after the MQTT event\n", led_on ? "on" : "off",
                                               (long)m.latency_us);
            continue;
        }
        if (m.kind == TELE_LINK_UP || m.kind == TELE_LINK_DOWN) {
            link_up = (m.kind == TELE_LINK_UP);
            lock();
            g_sh.link_up = link_up;
            unlock();
            if (!is_measuring()) Serial.printf("MQTT %s\n", link_up ? "connected" : "disconnected");
            continue;
        }
        if (m.kind != TELE_WINDOW) continue;

        sum_mc += m.sum_mc;
        n += m.n;
        if (m.min_mc < min_mc) min_mc = m.min_mc;
        if (m.max_mc > max_mc) max_mc = m.max_mc;
        if (m.max_late_us > max_late_us) max_late_us = m.max_late_us;
        misses += m.misses;
        dropped += m.dropped;
        alarm = m.alarm;
        if (++windows < kTelemetryWindows) continue;

        // A round is complete. With the link down it is not published: a
        // publish waits for the client while the client is busy trying to
        // connect, and the windows would pile up in the queue behind it.
        // In experiment c the sample task publishes the round, not this one.
        lock();
        bool sample_publishes = g_sh.publish_in_sample;
        unlock();
        int32_t avg_mc = (int32_t)(sum_mc / (int64_t)n);
        uint32_t failed = 0;
        int64_t t0 = rt_now_us();
        if (link_up) {
            if (!sample_publishes) failed = publish_round(led_on ? 1 : 0, alarm, avg_mc);
            snprintf(buf, sizeof(buf), "%ld", (long)max_late_us);
            if (!pub(g_t_jitter, buf)) failed++;
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)misses);
            if (!pub(g_t_misses, buf)) failed++;
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)dropped);
            if (!pub(g_t_drops, buf)) failed++;
            snprintf(buf, sizeof(buf), "%ld", (long)cmd_latency_us);
            if (!pub(g_t_cmd_latency, buf)) failed++;
        }
        long round_ms = (long)((rt_now_us() - t0) / 1000);

        if (!is_measuring()) {
            fmt_mc(t_avg, sizeof(t_avg), avg_mc);
            fmt_mc(t_min, sizeof(t_min), min_mc);
            fmt_mc(t_max, sizeof(t_max), max_mc);
            Serial.printf("Temp: %s (min %s max %s, %lu samples) | alarm %s | RSSI: %d | Heap: %lu | Uptime: %lus | MQTT %s\n",
                          t_avg, t_min, t_max, (unsigned long)n, alarm ? "on" : "off", (int)WiFi.RSSI(),
                          (unsigned long)ESP.getFreeHeap(), (unsigned long)(rt_now_us() / 1000000),
                          link_up ? "up" : "down");
            Serial.printf("control: jitter max %ld us, %lu misses, %lu windows dropped | ", (long)max_late_us,
                          (unsigned long)misses, (unsigned long)dropped);
            if (link_up)
                Serial.printf("publishing took %ld ms, %lu failed\n", round_ms, (unsigned long)failed);
            else
                Serial.println("not published, link down");
        }
        sum_mc = 0;
        n = windows = 0;
        min_mc = INT32_MAX;
        max_mc = INT32_MIN;
        // The timing figures are kept until a round has really been sent, so
        // that what happened while the link was down is reported afterwards.
        if (link_up && failed == 0) {
            misses = dropped = 0;
            max_late_us = cmd_latency_us = 0;
        }
    }
}

// ------------------------------------------------ MQTT event handler ------

static bool topic_is(esp_mqtt_event_handle_t e, const char* topic) {
    return e->topic_len == (int)strlen(topic) && memcmp(e->topic, topic, e->topic_len) == 0;
}

// Runs in mqtt_task. It does the minimum and never waits: whatever has to be
// done about an event is done by the task that receives the message.
static void on_mqtt(void* arg, esp_event_base_t base, int32_t id, void* data) {
    int64_t t_event_us = rt_now_us();
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    struct tele_msg ev;
    struct cmd_msg c;

    if (id == MQTT_EVENT_CONNECTED) {
        esp_mqtt_client_subscribe(g_client, g_t_led, 0);
        esp_mqtt_client_subscribe(g_client, g_t_ping, 0);
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
        if (e->data_len < 0 || e->data_len >= (int)sizeof(c.data)) return;
        memset(&c, 0, sizeof(c));
        if (topic_is(e, g_t_led))
            c.kind = CMD_LED;
        else if (topic_is(e, g_t_ping))
            c.kind = CMD_PING;
        else
            return;
        memcpy(c.data, e->data, e->data_len);
        c.t_event_us = t_event_us;
        xQueueSend(g_cmd_q, &c, 0);
    }
}

// ------------------------------------------------------- experiments ------
// All of this runs in loop(), at priority 1.

static int g_exp = 0;  // the key of the experiment that is running, 0 = none
static uint32_t g_exp_end_ms = 0;
static uint32_t g_steps_due = 0;
static uint32_t g_fault_end_ms = 0;  // 0 = the broker address is the good one

static void measure_start(uint32_t seconds, bool publish_in_sample) {
    lock();
    rt_stats_init(&g_sh.jitter, kJitterBucketUs);
    rt_stats_init(&g_sh.step_time, 10);
    g_sh.steps = g_sh.misses = g_sh.rounds_published = 0;
    g_sh.publish_in_sample = publish_in_sample;
    g_sh.measuring = true;
    unlock();
    g_steps_due = seconds * 1000 / kControlPeriodMs;
    g_exp_end_ms = millis() + seconds * 1000;
}

// Ends the measurement, prints it, and says whether the period was kept.
// max_us gets the worst jitter, rounds the telemetry rounds published.
static bool measure_report(int32_t* max_us, uint32_t* rounds) {
    static struct rt_stats jitter, step_time;  // copies, so that printing is outside the lock
    uint32_t steps, misses;

    lock();
    g_sh.measuring = false;
    g_sh.publish_in_sample = false;
    jitter = g_sh.jitter;
    step_time = g_sh.step_time;
    steps = g_sh.steps;
    misses = g_sh.misses;
    *rounds = g_sh.rounds_published;
    unlock();

    rt_stats_print(&jitter, "release jitter", "us");
    rt_stats_print_hist(&jitter, "us");
    rt_stats_print(&step_time, "start to end of one control step", "us");
    Serial.printf("steps run: %lu of %lu due, %lu of them a period late or more; %lu telemetry rounds published\n",
                  (unsigned long)steps, (unsigned long)g_steps_due, (unsigned long)misses, (unsigned long)*rounds);
    *max_us = jitter.n ? jitter.max : 0;
    // Two ways to lose the period: a release a whole period late, or fewer
    // steps than the window holds (2 % is allowed for the edges of the window).
    bool short_count = steps < g_steps_due - g_steps_due / 50;
    return steps > 0 && misses == 0 && !short_count;
}

// The library copies the address, which uses the heap. That is accepted here:
// it happens on a key press, in loop(), for the experiment only.
static void fault_start(void) {
    Serial.printf("broker address set to %s for %lu s\n", kBadUri, (unsigned long)kFaultSeconds);
    g_fault_end_ms = millis() + kFaultSeconds * 1000;
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

static void radio_off(void) {
    struct tele_msg ev;
    Serial.println("stopping the MQTT client and switching the radio off");
    esp_mqtt_client_stop(g_client);  // waits for mqtt_task to end
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    memset(&ev, 0, sizeof(ev));
    ev.kind = TELE_LINK_DOWN;  // a stopped client sends no event of its own
    xQueueSend(g_tele_q, &ev, 0);
    delay(2000);  // let things settle before measuring
}

static void radio_on(void) {
    Serial.println("radio on again, joining and connecting in the background");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    esp_mqtt_client_start(g_client);
}

static bool link_is_up(void) {
    lock();
    bool v = g_sh.link_up;
    unlock();
    return v;
}

static void experiment_start(int key) {
    if (key == 'x') key = 'd';
    if ((key == 'b' || key == 'c' || key == 'e') && !link_is_up()) {
        rt_verdict("not run: the MQTT link is down, this experiment needs it");
        return;
    }
    g_exp = key;
    if (key == 'a') {
        radio_off();
        Serial.printf("a: radio off, measuring for %lu s...\n", (unsigned long)kMeasureSeconds);
        measure_start(kMeasureSeconds, false);
    } else if (key == 'b') {
        Serial.printf("b: connected, publishing through the queue, measuring for %lu s...\n",
                      (unsigned long)kMeasureSeconds);
        measure_start(kMeasureSeconds, false);
    } else if (key == 'c') {
        Serial.println("c: publishing from inside the control task");
        measure_start(kFaultSeconds + 10, true);
        fault_start();
    } else if (key == 'd') {
        Serial.println("d: publishing through the queue");
        measure_start(kFaultSeconds + 10, false);
        fault_start();
    }
}

static void experiment_finish(void) {
    char line[128];
    int32_t max_us;
    uint32_t rounds;
    bool kept = measure_report(&max_us, &rounds);
    const char* what = g_exp == 'a'   ? "radio off"
                       : g_exp == 'b' ? "radio on, publishing through the queue"
                       : g_exp == 'c' ? "publishing from the control task, broker unreachable"
                                      : "publishing through the queue, broker unreachable";
    if (kept)
        snprintf(line, sizeof(line), "%c: control period kept (%s): jitter max %ld us", g_exp, what, (long)max_us);
    else
        snprintf(line, sizeof(line), "%c: control period LOST (%s): latest release %ld ms late", g_exp, what,
                 (long)(max_us / 1000));
    rt_verdict(line);
    if (g_exp == 'a') radio_on();
    g_exp = 0;
}

// Experiment e. Messages are sent to our own ping topic one at a time: the
// next one goes out when the previous one has come back, or after 2 s.
static void experiment_ping(void) {
    static struct rt_stats latency, trip;
    char payload[8], line[128];
    uint32_t answered;

    Serial.printf("e: sending %d messages to %s...\n", kPings, g_t_ping);
    lock();
    rt_stats_init(&g_sh.cmd_latency, 100);
    rt_stats_init(&g_sh.round_trip, 20000);
    g_sh.pings_answered = 0;
    g_sh.measuring = true;  // keeps the other tasks from printing
    unlock();

    for (int i = 0; i < kPings; i++) {
        snprintf(payload, sizeof(payload), "%d", i);
        lock();
        g_sh.ping_sent_us = rt_now_us();
        unlock();
        pub(g_t_ping, payload);
        for (int w = 0; w < 200; w++) {  // up to 2 s
            delay(10);
            lock();
            bool waiting = g_sh.ping_sent_us != 0;
            unlock();
            if (!waiting) break;
        }
        delay(100);
    }

    lock();
    g_sh.measuring = false;
    g_sh.ping_sent_us = 0;
    latency = g_sh.cmd_latency;
    trip = g_sh.round_trip;
    answered = g_sh.pings_answered;
    unlock();

    rt_stats_print(&latency, "command latency, MQTT event to command applied", "us");
    rt_stats_print_hist(&latency, "us");
    rt_stats_print(&trip, "round trip, publish to MQTT event", "us");
    rt_stats_print_hist(&trip, "us");
    if (answered == (uint32_t)kPings)
        snprintf(line, sizeof(line), "e: all %d commands came back: latency in the device max %ld us, round trip max %ld ms",
                 kPings, (long)latency.max, (long)(trip.max / 1000));
    else
        snprintf(line, sizeof(line), "e: %lu of %d commands came back, the rest were lost on the way",
                 (unsigned long)answered, kPings);
    rt_verdict(line);
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
    Serial.println("keys: a = radio off, b = connected, c = publish from the control task + broker fault,");
    Serial.println("      d (or x) = broker fault, e = command latency, s = stack and heap, h = help");
}

// ------------------------------------------------------ setup / loop ------

static void build_topic(char* out, size_t size, const char* field) {
    snprintf(out, size, "devices/%s/%s", g_device_id, field);
}

void setup() {
    rt_begin("stage 2: measurements and experiments");
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
    build_topic(g_t_ping, sizeof(g_t_ping), "ping");
    build_topic(g_t_jitter, sizeof(g_t_jitter), "jitter_max_us");
    build_topic(g_t_misses, sizeof(g_t_misses), "period_misses");
    build_topic(g_t_drops, sizeof(g_t_drops), "queue_drops");
    build_topic(g_t_cmd_latency, sizeof(g_t_cmd_latency), "cmd_latency_us");
    Serial.printf("Device ID: %s, broker %s\n", g_device_id, g_uri);

    g_tele_q = xQueueCreate(8, sizeof(struct tele_msg));
    g_cmd_q = xQueueCreate(4, sizeof(struct cmd_msg));
    g_lock = xSemaphoreCreateMutex();
    if (g_tele_q == NULL || g_cmd_q == NULL || g_lock == NULL) rt_fatal("out of memory at start-up");
    rt_stats_init(&g_sh.jitter, kJitterBucketUs);
    rt_stats_init(&g_sh.step_time, 10);
    rt_stats_init(&g_sh.cmd_latency, 100);
    rt_stats_init(&g_sh.round_trip, 20000);

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

    if (g_exp != 0 && (int32_t)(millis() - g_exp_end_ms) >= 0) experiment_finish();

    int key = rt_read_key();
    if (key == 's') {
        print_figures();
    } else if (key == 'h') {
        print_help();
    } else if (g_exp == 0 && g_fault_end_ms == 0) {
        if (key == 'e') {
            if (link_is_up())
                experiment_ping();
            else
                rt_verdict("not run: the MQTT link is down, this experiment needs it");
        } else if (key == 'a' || key == 'b' || key == 'c' || key == 'd' || key == 'x') {
            experiment_start(key);
        }
    }

    delay(10);
}

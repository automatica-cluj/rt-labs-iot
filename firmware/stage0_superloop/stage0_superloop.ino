// stage0_superloop - a usual Arduino IoT sketch with a 10 ms control step added
//
// This is the "before" picture. Everything is done the way a usual Arduino
// IoT example does it: one loop(), PubSubClient, String topics, a reconnect() that
// blocks, periods measured with millis() differences. One thing is new: a
// control step that should run every 10 ms. It samples the chip temperature,
// filters it, decides an alarm and collects min / avg / max for telemetry.
//
// The control step also measures the gap to its previous run, so the sketch
// can say whether the 10 ms period was kept.
//
// Keys in the serial monitor:
//   a   measure the control period for 20 s of normal operation
//   x   the same with the broker made unreachable for 15 s
//   h   this list
//
// Each run ends with one VERDICT line.
//
// Needs the PubSubClient library and a secrets.h (copy secrets.h.example).

#include <WiFi.h>
#include <PubSubClient.h>

#include "rt.h"
#include "secrets.h"

// ---- EXPERIMENT KNOBS ----
const uint32_t kControlPeriodMs = 10;
const uint32_t kTelemetryPeriodMs = 5000;
// The chip read 43 to 50 C with Wi-Fi on when this was written. Set the
// threshold below the value you see to watch the alarm topic change.
const int32_t kAlarmOnMc = 55000;   // alarm on above this, in millidegrees C
const int32_t kAlarmOffMc = 53000;  // and off again below this
const int kFilterShift = 4;         // filter weight 1/16 for each new sample
const uint32_t kMeasureSeconds = 20;
const uint32_t kFaultSeconds = 15;
const char* kBadHost = "no-such-broker.invalid";

WiFiClient espClient;
PubSubClient client(espClient);
String deviceId;
String topicPrefix;
bool ledOn = false;

// ----------------------------------------------------------- control ------

struct control_state {
    int32_t filt_mc;  // filtered temperature, millidegrees
    bool alarm;
    int32_t min_mc, max_mc;  // of the filtered value, since the last telemetry round
    int64_t sum_mc;
    uint32_t n;
};

static struct control_state g_ctl;

// Timing of the control step. g_gap is for the experiments; the round_ values
// are reset at every telemetry round and printed with it.
static struct rt_stats g_gap;
static uint32_t g_late = 0;
static int64_t g_last_step_us = 0;
static int32_t g_round_max_gap_us = 0;
static uint32_t g_round_late = 0;

static void control_reset_round(void) {
    g_ctl.min_mc = INT32_MAX;
    g_ctl.max_mc = INT32_MIN;
    g_ctl.sum_mc = 0;
    g_ctl.n = 0;
}

static void control_step(void) {
    int64_t t = rt_now_us();
    if (g_last_step_us != 0) {
        int64_t gap64 = t - g_last_step_us;
        int32_t gap = gap64 > INT32_MAX ? INT32_MAX : (int32_t)gap64;
        rt_stats_add(&g_gap, gap);
        if (gap > g_round_max_gap_us) g_round_max_gap_us = gap;
        if (gap > (int32_t)(2 * kControlPeriodMs * 1000)) {  // a whole period was lost
            g_late++;
            g_round_late++;
        }
    }
    g_last_step_us = t;

    int32_t raw_mc = (int32_t)(temperatureRead() * 1000.0f);
    if (g_ctl.n == 0 && g_ctl.filt_mc == 0) g_ctl.filt_mc = raw_mc;  // first sample
    g_ctl.filt_mc += (raw_mc - g_ctl.filt_mc) >> kFilterShift;

    if (g_ctl.filt_mc > kAlarmOnMc) g_ctl.alarm = true;
    if (g_ctl.filt_mc < kAlarmOffMc) g_ctl.alarm = false;

    if (g_ctl.filt_mc < g_ctl.min_mc) g_ctl.min_mc = g_ctl.filt_mc;
    if (g_ctl.filt_mc > g_ctl.max_mc) g_ctl.max_mc = g_ctl.filt_mc;
    g_ctl.sum_mc += g_ctl.filt_mc;
    g_ctl.n++;
}

// ------------------------------------------------------- experiments ------

static uint32_t g_measure_end_ms = 0;  // 0 = no measurement running
static uint32_t g_measure_steps_due = 0;  // steps a kept period gives in the window
static uint32_t g_fault_end_ms = 0;    // 0 = broker address is the good one

// g_last_step_us is not cleared here. If the loop blocks before the next
// control step, the gap across the block must still be measured.
static void measure_start(uint32_t seconds) {
    rt_stats_init(&g_gap, 2000);
    g_late = 0;
    g_measure_steps_due = seconds * 1000 / kControlPeriodMs;
    g_measure_end_ms = millis() + seconds * 1000;
    if (g_measure_end_ms == 0) g_measure_end_ms = 1;
}

static void fault_start(void) {
    Serial.printf("broker address set to %s for %lu s\n", kBadHost, (unsigned long)kFaultSeconds);
    g_fault_end_ms = millis() + kFaultSeconds * 1000;
    if (g_fault_end_ms == 0) g_fault_end_ms = 1;
    client.setServer(kBadHost, MQTT_PORT);
    client.disconnect();
}

// Called from inside reconnect() as well, otherwise the fault would never end:
// the loop that would end it is the loop that is stuck.
static void fault_poll(void) {
    if (g_fault_end_ms != 0 && (int32_t)(millis() - g_fault_end_ms) >= 0) {
        g_fault_end_ms = 0;
        client.setServer(MQTT_HOST, MQTT_PORT);
        Serial.println("broker address restored");
    }
}

static void measure_report(void) {
    char line[96];
    rt_stats_print(&g_gap, "gap between control steps", "us");
    rt_stats_print_hist(&g_gap, "us");
    long max_ms = g_gap.n ? (long)(g_gap.max / 1000) : 0;
    Serial.printf("steps run: %lu of %lu due\n", (unsigned long)g_gap.n, (unsigned long)g_measure_steps_due);
    // Two ways to lose the period: a gap of two periods or more, or fewer steps
    // than the window holds (2 % is allowed for the edges of the window).
    bool short_count = g_gap.n < g_measure_steps_due - g_measure_steps_due / 50;
    if (g_gap.n == 0)
        snprintf(line, sizeof(line), "control period LOST: the control step did not run at all");
    else if (g_late == 0 && !short_count)
        snprintf(line, sizeof(line), "control period kept: longest gap %ld ms", max_ms);
    else
        snprintf(line, sizeof(line), "control period LOST: %lu of %lu steps ran, longest gap %ld ms",
                 (unsigned long)g_gap.n, (unsigned long)g_measure_steps_due, max_ms);
    rt_verdict(line);
}

static void print_help(void) {
    Serial.println("keys: a = measure 20 s, x = measure with the broker unreachable, h = help");
}

// -------------------------------------------------------------- MQTT ------

void callback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    if (g_measure_end_ms == 0) {
        Serial.print("Received on ");
        Serial.print(topic);
        Serial.print(": ");
        Serial.println(message);
    }

    // Compare just the end of the topic
    String t = String(topic);
    if (t.endsWith("/led")) {
        if (message == "on") {
            digitalWrite(LED_PIN, LOW);
            ledOn = true;
        }
        if (message == "off") {
            digitalWrite(LED_PIN, HIGH);
            ledOn = false;
        }
    }
}

void reconnect() {
    while (!client.connected()) {
        fault_poll();
        Serial.print("Connecting to MQTT...");
        if (client.connect(deviceId.c_str())) {
            Serial.println("connected!");
            String ledTopic = topicPrefix + "/led";
            client.subscribe(ledTopic.c_str());
            Serial.println("Subscribed to: " + ledTopic);
        } else {
            Serial.print("failed (");
            Serial.print(client.state());
            Serial.println(") retrying in 2s...");
            delay(2000);
        }
    }
}

static void publish_telemetry(void) {
    char buf[32];

    snprintf(buf, sizeof(buf), "%lu", millis() / 1000);
    client.publish((topicPrefix + "/status").c_str(), buf);

    snprintf(buf, sizeof(buf), "%d", WiFi.RSSI());
    client.publish((topicPrefix + "/rssi").c_str(), buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)ESP.getFreeHeap());
    client.publish((topicPrefix + "/heap").c_str(), buf);

    snprintf(buf, sizeof(buf), "%ld", (long)WiFi.channel());
    client.publish((topicPrefix + "/wifi_channel").c_str(), buf);

    client.publish((topicPrefix + "/ip").c_str(), WiFi.localIP().toString().c_str());
    client.publish((topicPrefix + "/mac").c_str(), WiFi.macAddress().c_str());
    client.publish((topicPrefix + "/led_state").c_str(), ledOn ? "on" : "off");
    client.publish((topicPrefix + "/alarm").c_str(), g_ctl.alarm ? "on" : "off");

    // Temperature goes last: the dashboard stores a history row when it
    // arrives, with the other values as they are at that moment.
    float avg_c = g_ctl.n ? (float)(g_ctl.sum_mc / (int64_t)g_ctl.n) / 1000.0f : temperatureRead();
    snprintf(buf, sizeof(buf), "%.1f", avg_c);
    client.publish((topicPrefix + "/temperature").c_str(), buf);

    if (g_measure_end_ms == 0) {
        Serial.printf("Temp: %.1f (min %.1f max %.1f, %lu samples) | alarm %s | RSSI: %d | Heap: %lu | Uptime: %lus\n",
                      avg_c, g_ctl.min_mc / 1000.0f, g_ctl.max_mc / 1000.0f, (unsigned long)g_ctl.n,
                      g_ctl.alarm ? "on" : "off", WiFi.RSSI(), (unsigned long)ESP.getFreeHeap(), millis() / 1000);
        Serial.printf("control: longest gap %ld ms, %lu late steps in this round\n",
                      (long)(g_round_max_gap_us / 1000), (unsigned long)g_round_late);
    }
    control_reset_round();
    g_round_max_gap_us = 0;
    g_round_late = 0;
}

// ------------------------------------------------------ setup / loop ------

void setup() {
    rt_begin("stage 0: superloop with a 10 ms control step");
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED off at start (active LOW)
    rt_stats_init(&g_gap, 2000);
    control_reset_round();

    // Connect to Wi-Fi
    Serial.print("Connecting to Wi-Fi");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());

    // Build unique device ID from MAC
    deviceId = WiFi.macAddress();
    deviceId.replace(":", "");
    topicPrefix = "devices/" + deviceId;
    Serial.println("Device ID: " + deviceId);
    Serial.println("Topic prefix: " + topicPrefix);

    client.setServer(MQTT_HOST, MQTT_PORT);
    client.setCallback(callback);
    print_help();
}

void loop() {
    if (!client.connected()) {
        reconnect();
    }
    client.loop();
    fault_poll();

    static unsigned long lastControl = 0;
    if (millis() - lastControl >= kControlPeriodMs) {
        lastControl = millis();
        control_step();
    }

    static unsigned long lastMsg = 0;
    if (millis() - lastMsg > kTelemetryPeriodMs) {
        lastMsg = millis();
        publish_telemetry();
    }

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
    } else if (key == 'h') {
        print_help();
    }

    delay(1);  // yield to RTOS, as usual Arduino examples do
}

# Firmware — an IoT device in three stages

The same device is built three times. It reports telemetry over Wi-Fi and
MQTT, takes an LED command from the dashboard, and runs a control step every
10 ms. What changes from stage to stage is how the code is organised, and
with it what happens to the 10 ms step when the network misbehaves.

| Stage | Folder | What it shows |
|---|---|---|
| 0 | [stage0_superloop](stage0_superloop/) | The usual Arduino way: one `loop()` does everything. The control step stops while the network code waits. |
| 1 | [stage1_tasks](stage1_tasks/) | The same device from FreeRTOS tasks and queues. Nothing waits for the network. |
| 2 | [stage2_measure](stage2_measure/) | Stage 1 measuring itself: release jitter, missed periods, command latency. Five experiments. |

Work through them in order. Each stage has its own guide, and each is a
complete sketch that works with the dashboard, so the difference between two
stages is the lesson. Compare them with a diff tool as you go.

These sketches continue the
[ESP32-C3 real-time labs](https://github.com/automatica-cluj/rt-labs-esp32c3).
Lab 1, experiment E, ends with a question: what would you do in a real device
that needs both Wi-Fi and a tight control loop? This is one answer.

---

## 1. What you need

- An **ESP32-C3 Super Mini** and a USB cable. Nothing is wired to the board:
  the sketches use the temperature sensor inside the chip and the blue LED on
  pin 8.
- **Arduino IDE 2** with the esp32 boards package, set up as in the labs'
  [setup.md](https://github.com/automatica-cluj/rt-labs-esp32c3/blob/main/docs/setup.md).
  Board: *ESP32C3 Dev Module*, with *USB CDC On Boot* enabled (for
  `arduino-cli`, the FQBN is `esp32:esp32:esp32c3:CDCOnBoot=cdc`). The
  sketches were checked with version 3.3.12 of the esp32 package.
- For stage 0 only, the **PubSubClient** library (Library Manager, by Nick
  O'Leary). Stages 1 and 2 use the MQTT client that comes with the esp32
  package and need no library.
- A **2.4 GHz** Wi-Fi network. The ESP32-C3 has no 5 GHz radio. A phone
  hotspot works if it is set to 2.4 GHz. A phone may stop broadcasting its
  hotspot when nothing has been connected to it for a while; if the board
  stops joining, look at the phone first.
- Java 17 and Maven, or Docker, to run the dashboard.

## 2. Credentials: `secrets.h`

Every sketch folder has a `secrets.h.example`. **Copy** it to `secrets.h` in
the same folder and fill in the copy:

```c
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define MQTT_HOST "control.aut.utcluj.ro"
#define MQTT_PORT 11188
```

`secrets.h` is ignored by git. `secrets.h.example` is not: it is committed.
Never type your password into the example file.

## 3. The dashboard and the broker

Device and dashboard never talk to each other directly. Both connect to an
MQTT broker: the device publishes to `devices/{MAC}/...`, the dashboard
subscribes to `devices/#`, and the LED command travels the other way.

```
cd ../iot-dashboard
mvn spring-boot:run          # then open http://localhost:8080
```

The default broker is the faculty's, `control.aut.utcluj.ro:11188`. It has no
accounts, and everybody's devices meet on it: your dashboard shows the boards
of your colleagues too, and anybody can publish to your board's command
topic. If it cannot be reached, run a broker of your own; see *With a local
broker* in [../iot-dashboard/README.md](../iot-dashboard/README.md).

With `mosquitto_sub` installed you can watch what your board really sends,
without the dashboard in between:

```
mosquitto_sub -h control.aut.utcluj.ro -p 11188 -t 'devices/YOURMAC/#' -v
```

The board prints its device id, which is the MAC address without colons,
when it starts.

## 4. What every stage has in common

**The control step.** Every 10 ms: read the chip temperature, filter it,
compare it with a threshold to decide an alarm, and add the value to the
minimum, average and maximum of the current telemetry round. It stands for
the part of a device that has a deadline, a motor controller or a sampling
loop. The work is small; what matters in these stages is *when* it runs.

**The topics.** All payloads are plain text.

| Topic | Direction | Meaning |
|---|---|---|
| `temperature`, `rssi`, `heap`, `status` (uptime, s), `wifi_channel`, `ip`, `mac` | device to broker | telemetry, every 5 s |
| `led_state` | device to broker | what the LED really does |
| `alarm` | device to broker | `on` / `off`, decided by the control step. The dashboard ignores it. |
| `led` | broker to device | `on` / `off`, the command |

`temperature` is published last in a round, because the dashboard stores a
history row when it arrives, with the other values as they are at that
moment.

**The keys.** Each sketch reads single keys from the serial monitor and runs
an experiment. Every experiment ends with one line that starts with
`VERDICT:`. Nothing is printed while an experiment measures, because printing
takes time; the results come afterwards.

**The helper header.** `rt.h` next to each sketch is a copy of
[../common/rt.h](../common/rt.h), the toolbox of the ESP32-C3 labs:
`rt_now_us()`, `rt_sleep_until_tick()`, `struct rt_stats`, `rt_start_task()`,
`rt_verdict()`. It sits in every sketch folder because the Arduino IDE only
finds headers there.

## 5. Your report

For each stage, quote the `VERDICT:` lines you got and answer what each
experiment in its guide asks. Your numbers will differ from your colleagues':
they depend on your network. What should agree is which experiments keep the
period and which lose it, and you should be able to say why.

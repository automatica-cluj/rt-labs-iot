# Real-Time Systems — IoT labs

An IoT device built the real-time way. An **ESP32-C3 Super Mini** reports
telemetry and receives commands over Wi-Fi and MQTT, and a Spring Boot
dashboard shows the devices and sends the commands. The firmware is written
with the practices of the
[ESP32-C3 real-time labs](https://github.com/automatica-cluj/rt-labs-esp32c3):
FreeRTOS tasks with fixed priorities, absolute periodic release, queues
between the time-critical part and the network part, and measurements of its
own timing.

Students start with [firmware/README.md](firmware/README.md): setup, then
three stages, each a complete sketch with its own guide.

## Layout

| Path | What it is |
|---|---|
| [iot-dashboard/](iot-dashboard/) | The working copy of the Spring Boot dashboard (Java 17, Maven, Spring Boot 3). Taken from the IoT course example it was written for; changed here by additions only |
| [firmware/](firmware/) | The device in three stages, each with a guide: [0 superloop](firmware/stage0_superloop/), [1 tasks and queues](firmware/stage1_tasks/), [2 measurements](firmware/stage2_measure/) |
| [common/rt.h](common/rt.h) | The shared helper header, copied from `rt-labs-esp32c3` |
| [simulate-device.sh](simulate-device.sh) | Publishes fake device data with `mosquitto_pub`, to test the dashboard without a board |

## Running the dashboard

```
cd iot-dashboard
mvn spring-boot:run          # H2 in memory, http://localhost:8080
docker compose up --build    # or with PostgreSQL
docker compose -f compose.yml -f compose.local-broker.yml up --build   # with a local broker
```

The dashboard and the devices meet on the shared MQTT broker
`control.aut.utcluj.ro:11188` (anonymous). `./simulate-device.sh` makes a fake
device appear on the dashboard.

## Related repositories

- [rt-labs-esp32c3](https://github.com/automatica-cluj/rt-labs-esp32c3): the five ESP32-C3 real-time labs, source of `common/rt.h` and of the conventions used here.
- [rt-concepts](https://github.com/automatica-cluj/rt-concepts): the theory pages (Romanian).

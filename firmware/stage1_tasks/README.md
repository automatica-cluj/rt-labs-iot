# Stage 1 — Tasks and queues

The same device as stage 0, with the same topics, the same control step and
the same two experiments. The work is now split between tasks by urgency, and
the tasks talk through queues. Find out what that changes when the broker
goes away.

Setup, credentials and the dashboard are described in
[../README.md](../README.md). This stage needs no extra library.

Theory behind this stage (concept pages, in Romanian):
[Taskuri și stări](https://github.com/automatica-cluj/rt-concepts/blob/main/2-scheduling/tasks-and-states.md),
[Preempțiune cu priorități fixe](https://github.com/automatica-cluj/rt-concepts/blob/main/2-scheduling/fixed-priority-preemption.md),
[Eliberare periodică: întârziere relativă și absolută](https://github.com/automatica-cluj/rt-concepts/blob/main/1-time/periodic-release.md),
[Procesare amânată](https://github.com/automatica-cluj/rt-concepts/blob/main/4-interrupts/deferred-processing.md),
[Excludere mutuală](https://github.com/automatica-cluj/rt-concepts/blob/main/5-shared-resources/mutual-exclusion.md),
[Inversiunea de prioritate](https://github.com/automatica-cluj/rt-concepts/blob/main/5-shared-resources/priority-inversion.md).

---

## 1. The design

| Task | Priority | What it does | What it waits for |
|---|---|---|---|
| Wi-Fi driver, esp_timer | 23, 22 | the radio and the timers (part of the system) | |
| `sample` | 15 | the control step; closes a window of min / avg / max every second and sends it to a queue | the next 10 ms release, on absolute time |
| `command` | 12 | applies commands; owns the LED | the command queue |
| `mqtt_task` | 5 | the MQTT client, created by the library: connects, reconnects, receives | the network |
| `telemetry` | 3 | merges windows into a round, formats, publishes. The only task of ours that calls the network | the telemetry queue |
| `loop()` | 1 | serial keys and reports | |

```
                  windows, 1 per second
   sample (15) ---------------------------+
                                          v
                  LED changed        +-----------+     publish
   command (12) -------------------> | telemetry | -------------> broker
        ^                            |  queue    |
        | command queue              +-----------+
        |                                 ^
   MQTT event handler  -------------------+
   (runs in mqtt_task, 5)   link up / link down
```

Open [stage1_tasks.ino](stage1_tasks.ino) and find:

1. **Absolute release.** `sample_task` computes `next += period` and sleeps
   until that tick. Compare with the `millis()` test of stage 0.
2. **Timeout 0.** `sample_task` sends its window with `xQueueSend(..., 0)`. If
   the queue is full it does not wait: it counts a dropped window and goes on.
3. **The event handler does the minimum.** `on_mqtt()` runs in the MQTT
   client's task. It copies the message into a fixed-size `struct` and puts it
   in a queue, the way an interrupt handler hands work to a task.
4. **Nothing in `setup()` waits.** It starts Wi-Fi, starts the MQTT client,
   starts the tasks and returns. Joining, connecting and reconnecting happen
   in the background.
5. **State has one owner.** The LED state belongs to `command_task`; the
   telemetry task learns it from a message. The only data shared by two tasks
   is the measurement, and it sits behind a mutex. It is a FreeRTOS mutex and
   not a plain semaphore because a mutex has priority inheritance: if `loop()`
   (priority 1) holds it when `sample` (15) needs it, `loop()` runs at
   priority 15 until it lets go, so no task in between can keep `sample`
   waiting. This is the cure for priority inversion. For the same reason
   `loop()` only copies the statistics under the lock and prints them after
   releasing it.
6. **No heap after start-up.** The topic strings are built once in `setup()`
   into `char` arrays. There is no `String` anywhere.

## 2. Run it

Copy your `secrets.h` from stage 0, upload, and open the serial monitor. The
output looks like stage 0's, with a few more figures in the second line.

Switch the LED from the dashboard and compare with stage 0 how long the
dashboard takes to show the new state. Find in the sketch what makes the
difference.

Press `s`. For each task the sketch prints how much of its stack was never
used, and the free heap now and at its lowest point since boot. Press `s`
again after a few minutes and compare the heap figures.

## 3. Experiments

### Experiment A — normal operation

Press `a`, as in stage 0. Compare the minimum, average and maximum gap with
your stage 0 result. Explain the difference in the average.

### Experiment X — the broker goes away

Press `x`, as in stage 0: the broker address is wrong for 15 s.

Compare the verdict with stage 0's. Then check that the fault really
happened: run `mosquitto_sub` on your board's `temperature` topic (see
[../README.md](../README.md)) in a terminal, press `x` again, and watch the
messages stop and come back. The device was cut off from the broker, and
the control step ran on time throughout. Which property of the design gives
you this?

### Experiment W — no Wi-Fi at all

Switch your hotspot or access point off, wait until the monitor shows
`MQTT down`, and press `a`. Switch the network back on during or after the
run, and do nothing else. What does the device do? What would the stage 0
sketch do in the same situation, from power-on?

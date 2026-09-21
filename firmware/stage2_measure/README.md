# Stage 2 — The device measures itself

The tasks, priorities and queues are those of stage 1. What is new is that the
device knows its own timing and can report it: on the serial monitor after an
experiment, and over MQTT with every telemetry round. Five experiments show
what the design guarantees, what it does not, and what breaks it.

Setup, credentials and the dashboard are described in
[../README.md](../README.md). This stage needs no extra library.

Theory behind this stage (concept pages, in Romanian):
[Latență și jitter](https://github.com/automatica-cluj/rt-concepts/blob/main/1-time/latency-and-jitter.md),
[Preempțiune cu priorități fixe](https://github.com/automatica-cluj/rt-concepts/blob/main/2-scheduling/fixed-priority-preemption.md),
[Excludere mutuală](https://github.com/automatica-cluj/rt-concepts/blob/main/5-shared-resources/mutual-exclusion.md),
[Supraîncărcare și politica de depășire](https://github.com/automatica-cluj/rt-concepts/blob/main/3-analysis/overload.md).

---

## 1. What is measured

| Quantity | Meaning | Where in the sketch |
|---|---|---|
| **release jitter** | how late the `sample` task starts, against its ideal release time `t0 + k × 10 ms` | `sample_task`, `late_us` |
| **period misses** | releases that came a whole period late or more | `sample_task`, `miss` |
| **step time** | from the start to the end of one control step. It is *elapsed* time: a step that the Wi-Fi driver interrupts comes out longer, though it did the same work | `sample_task`, `step_us` |
| **command latency** | from the MQTT event handler seeing a command to the `command` task having applied it | `on_mqtt`, `command_task` |
| **round trip** | from publishing a message to our own `ping` topic to seeing it come back from the broker | `experiment_ping` |

Stages 0 and 1 measured the *gap* between two control steps. Jitter is the
sharper tool: a task that is late by the same amount every time has perfect
gaps and is still late.

Read how `sample_task` finds the ideal release time. The release is known in
ticks, the clock counts microseconds from a different starting point, and the
offset between the two is found first, as the smallest difference seen in the
first 2 s.

Everything that more than one task touches sits in one `struct shared`
behind one mutex. Every holder only copies or adds a number under the lock.

New topics, published with every round and ignored by the dashboard:
`jitter_max_us`, `period_misses`, `queue_drops`, `cmd_latency_us`. There is
also a new command topic, `ping`, used by experiment E. Watch them with
`mosquitto_sub`.

## 2. Run it

Copy your `secrets.h`, upload, open the serial monitor, press `h` for the
keys. Wait for `MQTT connected` before the experiments that need the link
(B, C and E); they refuse to start without it.

## 3. Experiments

Run them in order and keep every `VERDICT:` line.

### Experiment A — radio off

Press `a`. The MQTT client is stopped and the radio switched off, the jitter
is measured for 20 s, then both come back. This is the baseline: the
`sample` task with nothing above it but the timer.

### Experiment B — connected, publishing through the queue

Press `b`. This is the normal design at work, with telemetry rounds being
published during the measurement.

Compare the jitter histogram with A. The `sample` task has the highest
priority of our tasks, and it is late anyway. What delays it? Is the delay
bounded? This is Lab 1, experiment E, met again in a complete device.

### Experiment C — publishing from the control task

Press `c`. The telemetry round is now published *by the `sample` task
itself*, the way the superloop of stage 0 did it, and in the middle of the
run the broker is made unreachable for 15 s.

Read the verdict, the number of late releases and the latest one. The tasks
and their priorities are exactly those of experiment D below. What is the one
thing that differs?

### Experiment D — the broker goes away

Press `d` (or `x`, the key it has in stages 0 and 1). The same fault as in C,
with the normal design. Compare the verdict with C, and the jitter with B.

After C and after D, look at the `period_misses` topic with `mosquitto_sub`.
When does the device report the misses of experiment C, given that it could
not publish while they were happening? Find the lines of `telemetry_task`
that make this work.

### Experiment E — command latency

Press `e`. The board sends 20 messages to its own `ping` topic, one at a
time. Each goes to the broker and comes back as a command, through the event
handler, the command queue and the `command` task.

You get two figures. Compare their size. Which of them does your code
control, and which not? When the dashboard switches the LED, the
monitor also prints the latency of that one command; compare.

A user who clicks the button on the dashboard waits for more than either
figure. The command travels from the browser to the dashboard, to the broker,
over Wi-Fi to the board, and only then through the event handler, the queue
and the `command` task. The last three are inside the device, and their delay
is the one this design keeps small and bounded. The others depend on networks
and servers that promise nothing.

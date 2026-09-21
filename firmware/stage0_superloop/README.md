# Stage 0 — The superloop

The starting point: an IoT sketch written the usual Arduino way, with one
addition, a control step that should run every 10 ms. Find out when it does,
and when it does not.

Setup, credentials and the dashboard are described in
[../README.md](../README.md). This stage needs the **PubSubClient** library.

Theory behind this stage (concept pages, in Romanian):
[Eliberare periodică: întârziere relativă și absolută](https://github.com/automatica-cluj/rt-concepts/blob/main/1-time/periodic-release.md),
[Latență și jitter](https://github.com/automatica-cluj/rt-concepts/blob/main/1-time/latency-and-jitter.md).

---

## 1. Read the sketch

Open [stage0_superloop.ino](stage0_superloop.ino). It is an IoT example
written the usual Arduino way, the one this project started from, with the
control step and the measurements added. Find these five things:

1. `setup()` waits in a `while` until Wi-Fi is joined.
2. `loop()` begins with `if (!client.connected()) reconnect();`, and
   `reconnect()` is a `while` that tries, waits 2 s with `delay()`, and tries
   again, until it succeeds.
3. The two periods, 10 ms for the control step and 5 s for telemetry, are both
   made the same way: `if (millis() - last >= period) { last = millis(); ... }`.
4. `control_step()` notes the time of each run and records the **gap** to the
   previous run. A gap of two periods or more means a step was lost, and
   counts as *late*.
5. The topics are built with `String`, for instance
   `(topicPrefix + "/status").c_str()`. Each such line takes memory from the
   heap and gives it back, nine times in every telemetry round. A device that
   runs for months can end up with its heap cut into pieces too small to use.
   Stage 1 builds the topic strings once, at start-up.

Everything happens in one thread of execution. While any line of it waits,
nothing else in the sketch runs.

## 2. Run it

Create `secrets.h`, upload, open the serial monitor. The board joins Wi-Fi,
connects to the broker, and prints two lines every 5 s: the telemetry it
sent, and the longest gap between control steps in that round.

Start the dashboard and find your board by its MAC address. Switch the LED
from the dashboard. Notice how long the dashboard takes to show the new LED
state, and find in the sketch why.

## 3. Experiments

### Experiment A — normal operation

Press `a`. The sketch measures the gaps between control steps for 20 s, then
prints their statistics, a histogram and a verdict.

Look at the longest gap and at where in the histogram the rare long gaps
sit. Outside an experiment the sketch prints the longest gap of every
telemetry round; use that to find out what `loop()` is doing when a long gap
happens.

Look at the average gap. It is not exactly 10 000 µs. Which line of the sketch
explains that? (Lab 1, experiments A and B.)

### Experiment X — the broker goes away

Press `x`. The sketch changes the broker address to a name that does not
exist, for 15 s, and drops the connection. Then it puts the right address
back. From the device's point of view this is a broker that is down, or a
network without internet.

Watch the serial monitor while it runs, then read the verdict. How many
control steps ran, of how many that were due? How long was the longest gap?

In a real device the control step might be holding a motor at a speed, or
sampling a signal. Describe what such a device would have done during this
experiment.

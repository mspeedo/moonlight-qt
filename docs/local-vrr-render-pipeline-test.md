# Local VRR render-pipeline diagnostic (SteamOS)

This **offline** experimental mode creates a fullscreen SDL/Vulkan window, the
existing `PlVkRenderer`, and the existing `Pacer`. It has **no Sunshine
connection, transport, capture, FFmpeg decoder or stream session**. Normal
Moonlight startup and streaming are unaffected unless this explicit launch
flag is present.

## Run from inside Moonlight (recommended for SteamOS Game Mode)

Launch **Moonlight normally from Steam Game Mode** using the new Flatpak build.
On the **Computers** or **Settings** page, select **VRR Benchmark** in the
top toolbar (reachable with the controller's D-pad/left stick). This opens
a dedicated gamepad-navigable page where you can configure:

* **Producer cadence:** repeatable variable 80–110 FPS or fixed 90 FPS.
* **Swapchain acquisition:** existing early ordering or experimental late ordering.
* **Vulkan present mode:** automatic baseline preference, IMMEDIATE, MAILBOX,
  FIFO_RELAXED or FIFO. An explicitly requested unsupported mode fails the
  benchmark (exit code 10) instead of silently using another.
* **Swapchain depth:** 1, 2 (existing baseline), or 3 Vulkan images requested
  through libplacebo.
* **Presentation wait policy:** none (baseline), block on libplacebo
  `pl_swapchain_swap_buffers()` before the next acquisition, or wait after
  submitting the current frame.
* **Frame selection:** newest available (baseline) or FIFO in the Pacer render
  queue. The bounded queue still discards frames if it becomes full.
* **Duration:** 30, 60, 120 **measured** seconds or manual stop; a separate
  one-second unmeasured warm-up precedes every run.
* **Gamescope composition:** automatic selection or request forced full GPU composition
  through the existing `GAMESCOPE_COMPOSITE_FORCE` XWayland root property.
* **Diagnostic HUD:** on or off. The HUD and completed-results page both
  show the actual SDL window-system backend identified by
  `SDL_GetWindowWMInfo()` and the Vulkan swapchain's selected present mode
  read from the successfully initialized `PlVkRenderer`.

Select **Start VRR benchmark**. The Moonlight Qt interface is hidden, and a
fullscreen Vulkan diagnostic window opens in the **same Moonlight process**.
This preserves the Steam launcher and Gamescope context. When the benchmark
finishes or you press **B**, Moonlight's normal UI returns to the same page
and displays the summary including average/max intervals, drops, reported
compositor presentation intervals (when available), Gamescope composition
request/status, scanout-path evidence, SDL window subsystem and Vulkan
present mode, the selected pipeline combination, and the saved CSV path.

During the fullscreen test, **X** toggles the producer cadence, **Y** toggles
swapchain acquisition, **A** toggles HUD, and **B** returns to Moonlight.
The UI selections are the starting configuration. Use separate timed runs
without mid-run toggles for clean A/B comparisons. The benchmark renders
normally during its initial one-second warm-up, but **all timing samples,
frame/drop counts, Wayland presentation requests and CSV event rows start
after that second**. Interval baselines are first established after warm-up,
so the first measured interval never includes startup. Timed runs retain
their configured **full measured duration** (for example, 60 measured
seconds plus one second of warm-up). If a manual run ends during warm-up,
the results contain no measurement events. Menu navigation uses
Moonlight's existing SteamOS gamepad-to-Tab focus behavior.

The default Qt backend depends on the SteamOS launch environment. To
compare the confirmed XWayland renderer with a native Wayland renderer,
set the **Steam launch options** of the Moonlight shortcut to
`env QT_QPA_PLATFORM=wayland %command%`, leaving all VRR Benchmark
settings unchanged. `main.cpp` selects SDL's Wayland video driver when
Qt is running on Wayland. The benchmark's **Actual SDL window backend**
must say `Native Wayland` for the comparison to be valid. Remove the
launch-option override to return to the previous Qt/SDL display selection.
If the local Steam shortcut uses a nonstandard wrapper, set the same
environment variable before its Flatpak run command instead.

No terminal command, manually added launch argument or second Steam shortcut
is required. The experimental Flatpak grants both Wayland and X11 sockets
instead of fallback-X11, because Gamescope's composition control lives on
its XWayland root window; this is an intentional test-branch sandbox
permission expansion, not a production streaming change. The test is reachable without connecting to a Sunshine host.

## Optional command-line launching

Install the `Moonlight-feature-local-vrr-render-pipeline-test-<sha>.flatpak`
artifact from the feature branch's **Build - Moonlight Flatpak** GitHub Actions
run, using `flatpak install --user ./<downloaded-file>.flatpak`.

Launch the diagnostic with:

```sh
flatpak run com.moonlight_stream.Moonlight --local-vrr-test
```

To test another pipeline combination without changing normal streaming,
use the **VRR Benchmark** page. All selections are fixed for that run except
the existing X/Y cadence/acquisition shortcuts. For command-line testing, the
options are:

| CLI flag | Values | Baseline |
| --- | --- | --- |
| `--vrr-present-mode=N` | 0=auto, 1=Immediate, 2=Mailbox, 3=FIFO relaxed, 4=FIFO | 0 |
| `--vrr-swapchain-depth=N` | 1, 2 or 3 | 2 |
| `--vrr-present-wait=N` | 0=none, 1=before acquire, 2=after submit | 0 |
| `--vrr-queue-policy=N` | 0=newest, 1=FIFO | 0 |
| `--vrr-acquire-late` | absent=early, present=late | early |

The selected present mode refers to Vulkan's requested WSI mode, not an
independent display-flip measurement. For the existing baseline, IMMEDIATE is
preferred when the Vulkan surface supports it. A Mesa WSI environment override
may change the driver's effective choice.

For immediate 90-FPS baseline:

```sh
flatpak run com.moonlight_stream.Moonlight --local-vrr-test --vrr-constant
```

To request forced GPU composition (and automatically restore the prior
XWayland property when the benchmark finishes):

```sh
flatpak run com.moonlight_stream.Moonlight --local-vrr-test --vrr-constant --vrr-force-composition
```

The composition switch now verifies the current X11 root using the
per-XWayland-server Gamescope properties `GAMESCOPE_XWAYLAND_SERVER_ID`
and/or `GAMESCOPE_PID`, falling back to `GAMESCOPE_VRR_CAPABLE` for older
Gamescope versions. SteamOS may launch Moonlight on a *non-primary* XWayland
server; only Gamescope's primary server receives `GAMESCOPE_VRR_CAPABLE`.
Checking that property alone previously caused false code-8 failures.

If the X11 root cannot be verified as Gamescope or the control cannot be
set, a forced-composition benchmark exits with code 8 rather than silently
running as an automatic-mode comparison. The setting is restored on ordinary
benchmark exit and handled errors, but an externally killed/crashed process
cannot run its restoration logic. The control property does not encode a
separately supplied Gamescope `-c` startup flag.

To launch with the experimental ordering:

```sh
flatpak run com.moonlight_stream.Moonlight --local-vrr-test --vrr-acquire-late
```

To run in **SteamOS Game Mode**, add the installed Flatpak Moonlight app as a
non-Steam game and pass `--local-vrr-test` through its launch options. If
the shortcut invokes a wrapper, append the flag after the Flatpak application
ID. In the SteamOS performance overlay, ensure VRR is enabled and the
frame-rate cap is disabled. Do not rely on a host frame limiter; there is no
host in this mode. The app's fullscreen SDL Vulkan surface is used by Gamescope
rather than a second Qt render surface.

### Controller/keyboard controls

| Control | Action |
| --- | --- |
| X / keyboard X or C | Toggle constant 90 FPS vs repeating variable 80–110 FPS |
| Y / keyboard Y or L | Toggle early vs late swapchain acquisition |
| A / keyboard H | Toggle the telemetry HUD |
| B / Escape | Exit and save CSV |

No persistent streaming preference or Moonlight configuration is modified.

## What actually gets tested

* **Production**: an independent SDL producer thread wakes at absolute
  `steady_clock` deadlines, emits YUV420P `AVFrame` objects, and submits
  them to the real `Pacer::submitFrame()` queue.
* **Moving content**: 960×540 luma checker/stripe/test lines move at
  240 source pixels/second using *elapsed monotonic time*, not frame count.
  The exact 20-interval variation sequence repeats, comprising
  approximately 9.091–12.500 ms per update; the constant mode uses
  an 11.111 ms interval.
* **Early/baseline**: normal `Pacer::renderThread()` obtains a drawable through
  `PlVkRenderer::waitToRender()` *before* sleeping for the next output frame,
  then latches the most recent available frame, drops stale frames, and
  renders with libplacebo into the existing Vulkan swapchain.
* **Late/experimental**: `Pacer` waits for a frame *first*, then calls the
  exact same `waitToRender()`, then latches the newest frame. No added
  pacing, host timestamps, speculative VSync timing, or frame-rate matching.
* **Presentation**: the same libplacebo Vulkan swapchain, present-mode preference
  (IMMEDIATE with fallbacks), render, submit and cleanup routines as the
  streaming renderer.

The diagnostic is intentionally CPU-frame-backed: it bypasses decoding and
hardware-decoded surface mapping, but still exercises FFmpeg AVFrame ->
libplacebo upload/mapping. It does **not** validate hardware-decoder surfaces,
Sunshine or network timing.

## Telemetry and limitations

The HUD shows the most recent and maximum observed **production interval**,
**swapchain acquisition duration**, **CPU rendering duration**, **submission
interval**, and number of Pacer-dropped frames. It also counts compositor-
discarded presentations and shows observed Gamescope **presentation
intervals** when the Wayland `wp_presentation` protocol is present. These
are Wayland compositor presentation timestamps, **not direct scanout/photodiode
measurements**. Gamescope currently sends the Wayland `ZERO_COPY` presentation
flag unconditionally, including in cases that may have required GPU composition,
so that flag is **never used to infer hardware direct scanout**.

The benchmark reports the Gamescope composition request and X11 control
read-back separately from the actual scanout-path status.

The SDL backend label is measured from the **actual fullscreen SDL window**
rather than inferred from `QT_QPA_PLATFORM`. It reports native Wayland, X11
(typically XWayland under SteamOS Game Mode), or a clearly marked unverified
SDL driver name when the native subsystem is not identified. The Vulkan mode
is the mode passed to libplacebo to create the **successfully initialized**
Vulkan swapchain: `IMMEDIATE_KHR`, `FIFO_RELAXED_KHR`, `MAILBOX_KHR`, or
`FIFO_KHR`. This confirms the application's swapchain creation mode, not
the compositor's physical display presentation mode. If the environment
specifies `MESA_VK_WSI_PRESENT_MODE`, the benchmark displays its value and
marks the effective driver-selected mode as unknown: Mesa can override
`VkSwapchainCreateInfoKHR::presentMode` without a Vulkan readback API.
Both fields appear in the HUD, completed-results page and session
comparison history. **Stock Gamescope
does not publish its actual scanout-versus-GPU-composition choice through
a property readable by Moonlight.** Consequently automatic-mode results say
"Not observable with stock Gamescope", even when forced composition controls
work. This is intentional: a requested change is not an observed display path.
A future instrumented Gamescope build can publish a CARDINAL property
`GAMESCOPE_MOONLIGHT_SCANOUT_PATH` on its XWayland root window, with 1 for
direct hardware-plane scanout and 2 for full GPU composition. The benchmark
samples this property while running, when present, and marks the source.
Without such compositor-side instrumentation it does not invent an actual
scanout result.

A source patch for an optional instrumented Gamescope build is checked in as
[`docs/gamescope-scanout-path-telemetry.patch`](gamescope-scanout-path-telemetry.patch).
It publishes the actual successful DRM backend decision (1=hardware-plane
scanout, 2=GPU composition) only when the path changes, so it does not send
an X11 event on every frame. This patch targets current upstream Gamescope
and **is not present in ordinary SteamOS Gamescope installations**. It has
not yet been built or device-tested and must not be confused with working
scanout-path reporting on stock SteamOS. The Moonlight test probes the property
without requiring it, so no Gamescope system modification is necessary just
to run the A/B forced-composition comparison.

If the Wayland presentation protocol is unsupported, the HUD clearly reports
unavailable; it never substitutes CPU submit time as a measured display time.

On exit, an event-by-event CSV is written under the Flatpak application's
per-user application data directory, with columns:

```csv
monotonic_us,event,value_us,actual_present_us
```

For `compositor_presentation_interval`, `value_us` is the interval and
`actual_present_us` is the compositor-reported absolute presentation time.
Other event rows leave `actual_present_us` blank. The file contains at most
500,000 rows; longer sessions may truncate recorded events.

The HUD refresh uses a small overlay upload every 500 ms and can itself affect
timing, so repeat visually with the **HUD hidden** and inspect the CSV.
Frame-production misses are reported as measured production intervals, so
consumer/render stalls can be distinguished from a delayed producer. CPU
renderer durations include rendering submission work, not GPU execution time.
Frames dropped by the bounded Pacer queue are counted as drops; internal
compositor discards are recorded separately.

**Suggested comparison:** On the Legion Go S with 120 Hz, VRR ON,
Steam performance OSD OFF, fixed 90 FPS, and Gamescope composition set to
Automatic, begin with the default pipeline as the control. Then vary **one
pipeline dimension at a time**, using the same measured duration:

1. **Baseline:** auto present mode / depth 2 / no wait / newest / early acquire.
2. **Wait before acquire:** same except wait policy 1.
3. **Wait after submit:** same except wait policy 2.
4. **Depth 1 and then 3:** baseline wait policy, newest queue.
5. **MAILBOX then FIFO_RELAXED then FIFO:** baseline depth and wait policy.
   Skip an explicitly unsupported mode; failure is informative, not a crash.
6. **FIFO queued frames:** baseline presentation settings, queue policy 1.
7. Repeat the most promising settings with **variable 80-110 FPS** and,
   separately, Steam performance OSD ON.

Once a promising setting appears, test it with late acquisition as a
separate variable. The local diagnostic uses the same Pacer and
libplacebo/Vulkan renderer as the stream but deliberately exercises
experimental options **only in this benchmark**. It does not change
production Moonlight streaming behavior, nor does it attribute cause to
Gamescope, Mesa or AMDGPU until the test results support that conclusion.
Compare visually at steady panning and examine the CSV's producer intervals,
submit intervals, acquire spikes and presentation intervals. A smooth local
test but unsmooth streaming result points to upstream decode/transport/host
or stream-specific mapping/telemetry differences; it does not prove a
specific host-cadence issue.

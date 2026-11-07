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
* **Duration:** 30, 60, 120 seconds or manual stop.
* **Gamescope composition:** automatic selection or request forced full GPU composition
  through the existing `GAMESCOPE_COMPOSITE_FORCE` XWayland root property.
* **Diagnostic HUD:** on or off.

Select **Start VRR benchmark**. The Moonlight Qt interface is hidden, and a
fullscreen Vulkan diagnostic window opens in the **same Moonlight process**.
This preserves the Steam launcher and Gamescope context. When the benchmark
finishes or you press **B**, Moonlight's normal UI returns to the same page
and displays the summary including average/max intervals, drops, reported
compositor presentation intervals (when available), Gamescope composition
request/status, scanout-path evidence, and the saved CSV path.

During the fullscreen test, **X** toggles the producer cadence, **Y** toggles
swapchain acquisition, **A** toggles HUD, and **B** returns to Moonlight.
The UI selections are the starting configuration. Use separate timed runs
without mid-run toggles for clean A/B comparisons. Menu navigation uses
Moonlight's existing SteamOS gamepad-to-Tab focus behavior.

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

For immediate 90-FPS baseline:

```sh
flatpak run com.moonlight_stream.Moonlight --local-vrr-test --vrr-constant
```

To request forced GPU composition (and automatically restore the prior
XWayland property when the benchmark finishes):

```sh
flatpak run com.moonlight_stream.Moonlight --local-vrr-test --vrr-constant --vrr-force-composition
```

If the Gamescope XWayland root cannot be verified or the control cannot be
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
read-back separately from the actual scanout-path status. **Stock Gamescope
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

**Suggested comparison:** with SteamOS VRR ON and the performance OSD OFF,
run fixed 90 FPS for 60 seconds using automatic composition, then repeat with
forced GPU composition; retain the same acquisition order, screen refresh and
other settings. Only after that repeat with the Steam performance OSD ON.
Also compare 90 FPS early/late and variable 80–110 FPS early/late if required.
Compare visually at steady panning and examine the CSV's producer intervals,
submit intervals, acquire spikes and presentation intervals. A smooth local
test but unsmooth streaming result points to upstream decode/transport/host
or stream-specific mapping/telemetry differences; it does not prove a
specific host-cadence issue.

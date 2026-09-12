#pragma once

class NvComputer;

namespace LatencyBenchmarkControl {

// Capture the active host connection details while the streaming session is
// known to be alive. The control worker never retains the NvComputer pointer.
void configure(NvComputer* computer);

// Best-effort host control. Modified Sunshine launches/stops the helper; stock
// Sunshine simply rejects the unknown endpoint. Benchmark readiness never waits
// for or depends on either response.
void startAsync();
void stopAsync();

} // namespace LatencyBenchmarkControl

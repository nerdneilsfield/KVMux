#pragma once

#include <iosfwd>
#include <string>

namespace kvmux {

class CaptureSource;
class ControlSink;

// One `--probe-control` run. `serial` selects a local CH9329 port; otherwise
// `target` is a relay host and the sink is already a control-role client.
struct ProbeControlOptions {
  std::string target;
  int baud{9600};
  int control_port{17000};
  bool serial{true};
  // Control relay freshness has no picture. Only the probe sets this.
  bool declare_freshness{false};
  bool keys{false};
  bool mouse{false};
  int timeout_ms{5000};
};

// Prints one PASS, FAIL, or SKIP line per step. Returns 0 when every executed
// step passed, and 1 when any step failed. The caller owns the sink.
int probe_control(std::ostream& out, ControlSink& sink,
                  const ProbeControlOptions& options);

// Counts frames from an already configured video-role capture source.
// Returns 0 when at least one frame arrives, and 1 on a fault or a dry run.
int probe_video(std::ostream& out, CaptureSource& source, int seconds);

}  // namespace kvmux

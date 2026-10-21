#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

#include "control/control_queue.hpp"
#include "control/control_sink.hpp"
#include "support/diagnostics.hpp"

namespace kvmux {

struct SerialIo {
  std::function<bool(const std::string&, int)> open;
  std::function<void()> close;
  std::function<std::ptrdiff_t(std::span<const std::uint8_t>)> write;
  std::function<std::ptrdiff_t(std::span<std::uint8_t>)> read;
};

class Ch9329ControlSink final : public ControlSink {
 public:
  Ch9329ControlSink();
  // `diagnostics` (optional) receives serial-thread timings: queue age,
  // ordinary ACK RTT and ACK timeouts. It must be thread-safe.
  explicit Ch9329ControlSink(std::shared_ptr<Diagnostics> diagnostics);
  explicit Ch9329ControlSink(SerialIo io,
                             std::shared_ptr<Diagnostics> diagnostics = {});
  ~Ch9329ControlSink() override;
  Ch9329ControlSink(const Ch9329ControlSink&) = delete;
  Ch9329ControlSink& operator=(const Ch9329ControlSink&) = delete;

  void connect(std::string port, int baud_rate,
               std::uint8_t address = 0) override;
  void disconnect() noexcept override;
  void set_mouse_mode(MouseMode mode) override;
  void set_control_active(bool active) noexcept override;
  void update_ui_heartbeat() noexcept override;

  [[nodiscard]] SubmitResult submit(ControlEvent event) override;
  [[nodiscard]] SubmitResult synchronize(InputSync sync) override;
  [[nodiscard]] SubmitResult start_ascii_paste(AsciiPasteJob job) override;
  [[nodiscard]] SubmitResult prepare_ascii_paste(
      AsciiPasteRequest request) override;
  void cancel_ascii_paste() noexcept override;
  [[nodiscard]] AsciiPasteSnapshot ascii_paste_snapshot() const override;
  void release_all() noexcept override;
  [[nodiscard]] ControlSnapshot snapshot() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kvmux

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "video/capture_sample.hpp"

namespace kvmux {

enum class CaptureBackend { media_foundation, v4l2, avfoundation };

struct Rational {
  std::int32_t numerator{};
  std::int32_t denominator{1};
  friend bool operator==(const Rational&, const Rational&) = default;
};

struct DeviceInfo {
  CaptureBackend backend{};
  std::string stable_id;
  std::string display_name;
  std::optional<std::uint16_t> usb_vendor_id;
  std::optional<std::uint16_t> usb_product_id;
  std::string usb_serial;
  bool weak_match{};
};

struct CaptureMode {
  std::string device_id;
  std::uint32_t width{};
  std::uint32_t height{};
  Rational frame_rate;
  PixelFormat device_format{PixelFormat::unknown};
  PixelFormat delivered_format{PixelFormat::unknown};
  std::string device_format_name;
  friend bool operator==(const CaptureMode&, const CaptureMode&) = default;
};

enum class CaptureState {
  stopped,
  starting,
  streaming,
  stopping,
  permission_denied,
  fault
};

struct CaptureSnapshot {
  CaptureState state{CaptureState::stopped};
  std::uint64_t generation{};
  std::uint64_t received_samples{};
  std::uint64_t overwritten_samples{};
  CaptureMode actual_mode;
  std::string error;
};

class CaptureSource {
 public:
  virtual ~CaptureSource() = default;
  [[nodiscard]] virtual std::vector<DeviceInfo> enumerate_devices() = 0;
  [[nodiscard]] virtual std::vector<CaptureMode> enumerate_modes(
      const std::string& stable_id) = 0;
  // Before returning, publish the new generation and requested mode, even if
  // opening the native device continues asynchronously.
  virtual void start(const CaptureMode& mode) = 0;
  virtual void stop() noexcept = 0;
  [[nodiscard]] virtual std::optional<CaptureSample> take_latest_sample() = 0;
  // Returns when a sample is published or stopping is true. Sources that do
  // not signal publication keep the previous 1 ms poll.
  virtual void wait_for_sample(const std::atomic<bool>& stopping) {
    if (stopping.load(std::memory_order_acquire)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  virtual void wake_sample_wait() noexcept {}
  [[nodiscard]] virtual CaptureSnapshot snapshot() const = 0;
};

[[nodiscard]] std::unique_ptr<CaptureSource> create_platform_capture_source();

}  // namespace kvmux

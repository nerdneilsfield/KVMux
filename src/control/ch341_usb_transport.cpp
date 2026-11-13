#include <libusb.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "control/ch341_usb.hpp"
#include "control/control_sink.hpp"
#include "control/serial_trace.hpp"
#include "control/serial_worker.hpp"

namespace kvmux {
namespace {

constexpr std::uint8_t kRequestReadVersion = 0x5f;
constexpr std::uint8_t kRequestWriteRegister = 0x9a;
constexpr std::uint8_t kRequestSerialInit = 0xa1;
constexpr std::uint8_t kRequestModemControl = 0xa4;
constexpr std::uint16_t kDivisorValue = 0x1312;
constexpr std::uint16_t kLineControlValue = 0x2518;
constexpr std::uint16_t kLineControlIndex = 0x00c3;
constexpr std::uint16_t kFlowControlValue = 0x2727;
constexpr unsigned kAssertedLines = (1u << 5u) | (1u << 6u);
constexpr int kControlTimeoutMs = 1000;
constexpr int kOutTimeoutMs = 100;
constexpr int kDrainTimeoutMs = 2;
constexpr int kCloseSliceUs = 10000;
constexpr std::size_t kMaxPacketBytes = 512;

const char* transfer_status_name(libusb_transfer_status status) {
  switch (status) {
    case LIBUSB_TRANSFER_COMPLETED:
      return "COMPLETED";
    case LIBUSB_TRANSFER_ERROR:
      return "ERROR";
    case LIBUSB_TRANSFER_TIMED_OUT:
      return "TIMED_OUT";
    case LIBUSB_TRANSFER_CANCELLED:
      return "CANCELLED";
    case LIBUSB_TRANSFER_STALL:
      return "STALL";
    case LIBUSB_TRANSFER_NO_DEVICE:
      return "NO_DEVICE";
    case LIBUSB_TRANSFER_OVERFLOW:
      return "OVERFLOW";
  }
  return "UNKNOWN";
}

std::string port_path(const Ch341UsbAddress& address) {
  std::string text;
  for (int index = 0; index < address.port_count; ++index) {
    if (index != 0) {
      text.push_back('.');
    }
    text += std::to_string(address.ports[index]);
  }
  return text;
}

void log_device_match(const Ch341UsbAddress& address, bool found) {
  spdlog::debug(
      "CH341 direct match: bus={} path={} vid=0x{:04x} pid=0x{:04x} found={}",
      address.bus, port_path(address), kCh341DirectVendorId,
      kCh341DirectProductId, found ? "true" : "false");
}

std::string usb_failure(const char* step, int result) {
  std::string text = step;
  text += "失败: ";
  text += libusb_error_name(result);
  return text;
}

std::string open_failure(const char* step, int result) {
  if (result == LIBUSB_ERROR_BUSY || result == LIBUSB_ERROR_ACCESS) {
    return "设备被系统或厂商驱动（如 macOS 的 WCH "
           "CH34xVCPDriver）占用，或当前用户没有权限（Linux 提示 udev 规则）";
  }
#if defined(_WIN32)
  if (result == LIBUSB_ERROR_NOT_SUPPORTED) {
    return "设备没有绑定 WinUSB，需用 Zadig 绑定";
  }
#endif
  return usb_failure(step, result);
}

class Ch341DirectTransport {
 public:
  Ch341DirectTransport() = default;
  ~Ch341DirectTransport() { close(); }
  Ch341DirectTransport(const Ch341DirectTransport&) = delete;
  Ch341DirectTransport& operator=(const Ch341DirectTransport&) = delete;

  bool open(const std::string& name, int baud, std::string& error) {
    close();
    const auto address = parse_ch341_usb_name(name);
    if (!address) {
      spdlog::debug("CH341 direct open rejected: path={}", name);
      error = "CH341 direct 端口名无效";
      return false;
    }
    libusb_context* created = nullptr;
    const int init = libusb_init(&created);
    if (init != 0) {
      error = usb_failure("初始化", init);
      return false;
    }
    context_ = created;
    if (!claim_device(*address, error) || !load_endpoints(error) ||
        !allocate_transfers(error) || !configure_uart(baud, error) ||
        !discard_stale_input(error)) {
      close();
      return false;
    }
    open_ = true;
    submit_in();
    if (fault_ || !in_pending_) {
      if (error.empty()) {
        error = "提交接收失败";
      }
      close();
      return false;
    }
    spdlog::info("CH341 direct open: path={} baud={} version=0x{:02x}", name,
                 baud, version_);
    return true;
  }

  void close() noexcept {
    const bool active = context_ != nullptr;
    const bool cancel_in = in_pending_;
    const bool cancel_out = out_pending_;
    closing_ = true;
    const auto started = std::chrono::steady_clock::now();
    if (context_ && (in_pending_ || out_pending_)) {
      if (in_pending_ && in_transfer_) {
        libusb_cancel_transfer(in_transfer_);
      }
      if (out_pending_ && out_transfer_) {
        libusb_cancel_transfer(out_transfer_);
      }
      bool warned = false;
      while (in_pending_ || out_pending_) {
        timeval slice{0, kCloseSliceUs};
        libusb_handle_events_timeout_completed(context_, &slice, nullptr);
        if (!warned && std::chrono::steady_clock::now() - started >
                           std::chrono::seconds(1)) {
          spdlog::warn("CH341 direct close is still waiting for USB transfers");
          warned = true;
        }
      }
    }
    if (active) {
      const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started);
      spdlog::debug(
          "CH341 direct close: cancel_in={} cancel_out={} waited_ms={}",
          cancel_in, cancel_out, waited.count());
    }
    if (in_transfer_) {
      libusb_free_transfer(in_transfer_);
      in_transfer_ = nullptr;
    }
    if (out_transfer_) {
      libusb_free_transfer(out_transfer_);
      out_transfer_ = nullptr;
    }
    in_buffer_.clear();
    out_buffer_.clear();
    ready_.clear();
    ready_offset_ = 0;
    ready_size_ = 0;
    if (interface_claimed_ && handle_) {
      libusb_release_interface(handle_, 0);
    }
    interface_claimed_ = false;
    if (handle_) {
      libusb_close(handle_);
      handle_ = nullptr;
    }
    if (context_) {
      libusb_exit(context_);
      context_ = nullptr;
    }
    open_ = false;
    fault_ = false;
    closing_ = false;
    in_pending_ = false;
    out_pending_ = false;
    in_completed_ = false;
    out_completed_ = false;
    version_ = 0;
    in_endpoint_ = 0;
    out_endpoint_ = 0;
  }

  std::ptrdiff_t write(std::span<const std::uint8_t> bytes) {
    if (!open_) {
      return -1;
    }
    pump();
    if (fault_) {
      return -1;
    }
    if (out_pending_) {
      return 0;
    }
    if (bytes.empty() || out_buffer_.empty()) {
      return 0;
    }
    const auto count = std::min(bytes.size(), out_buffer_.size());
    std::copy_n(bytes.begin(), count, out_buffer_.begin());
    libusb_fill_bulk_transfer(out_transfer_, handle_, out_endpoint_,
                              out_buffer_.data(), static_cast<int>(count),
                              &on_out, this, kOutTimeoutMs);
    out_submitted_ = static_cast<int>(count);
    out_completed_ = false;
    const int submitted = libusb_submit_transfer(out_transfer_);
    if (submitted != 0) {
      mark_fault(std::string("submit out ") + libusb_error_name(submitted));
      return -1;
    }
    out_pending_ = true;
    log_serial_out("CH341 direct", bytes.first(count));
    return static_cast<std::ptrdiff_t>(count);
  }

  std::ptrdiff_t read(std::span<std::uint8_t> bytes) {
    if (!open_) {
      return -1;
    }
    pump();
    if (fault_) {
      return -1;
    }
    std::size_t count = 0;
    if (!bytes.empty() && ready_offset_ < ready_size_) {
      count = std::min(bytes.size(), ready_size_ - ready_offset_);
      std::copy_n(ready_.data() + ready_offset_, count, bytes.begin());
      ready_offset_ += count;
    }
    if (ready_offset_ >= ready_size_) {
      ready_offset_ = 0;
      ready_size_ = 0;
      submit_in();
    }
    if (count > 0) {
      log_serial_in("CH341 direct", bytes.first(count));
    }
    return static_cast<std::ptrdiff_t>(count);
  }

 private:
  static void on_in(libusb_transfer* transfer) {
    auto* self = static_cast<Ch341DirectTransport*>(transfer->user_data);
    self->in_status_ = transfer->status;
    self->in_actual_ = transfer->actual_length;
    self->in_pending_ = false;
    self->in_completed_ = true;
    spdlog::debug("CH341 direct in complete: status={} length={}",
                  transfer_status_name(transfer->status),
                  transfer->actual_length);
  }

  static void on_out(libusb_transfer* transfer) {
    auto* self = static_cast<Ch341DirectTransport*>(transfer->user_data);
    self->out_status_ = transfer->status;
    self->out_actual_ = transfer->actual_length;
    self->out_pending_ = false;
    self->out_completed_ = true;
    spdlog::debug("CH341 direct out complete: status={} length={}",
                  transfer_status_name(transfer->status),
                  transfer->actual_length);
  }

  bool claim_device(const Ch341UsbAddress& address, std::string& error) {
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context_, &list);
    if (count < 0) {
      log_device_match(address, false);
      error = usb_failure("枚举设备", static_cast<int>(count));
      return false;
    }
    libusb_device* matched = nullptr;
    for (ssize_t index = 0; index < count; ++index) {
      libusb_device_descriptor descriptor{};
      if (libusb_get_device_descriptor(list[index], &descriptor) != 0) {
        continue;
      }
      if (descriptor.idVendor != kCh341DirectVendorId ||
          descriptor.idProduct != kCh341DirectProductId) {
        continue;
      }
      if (same_path(list[index], address)) {
        matched = list[index];
        break;
      }
    }
    log_device_match(address, matched != nullptr);
    int opened = LIBUSB_ERROR_NO_DEVICE;
    if (matched) {
      opened = libusb_open(matched, &handle_);
    }
    libusb_free_device_list(list, 1);
    if (!matched) {
      error = "设备不存在或已拔出";
      return false;
    }
    if (opened != 0) {
      handle_ = nullptr;
      error = open_failure("打开设备", opened);
      return false;
    }
    int configuration = 0;
    const int configured = libusb_get_configuration(handle_, &configuration);
    if (configured != 0) {
      spdlog::debug("CH341 direct configuration: read_failed={}",
                    libusb_error_name(configured));
      error = usb_failure("读取配置", configured);
      return false;
    }
    int set_result = 0;
    const bool did_set = configuration != 1;
    if (did_set) {
      set_result = libusb_set_configuration(handle_, 1);
    }
    spdlog::debug(
        "CH341 direct configuration: current={} set={} result={} ({})",
        configuration, did_set ? "true" : "false", set_result,
        libusb_error_name(set_result));
    if (did_set && set_result != 0) {
      error = open_failure("设置配置", set_result);
      return false;
    }
#if defined(__linux__)
    const int detached = libusb_set_auto_detach_kernel_driver(handle_, 1);
    spdlog::debug("CH341 direct detach: result={} ({})", detached,
                  libusb_error_name(detached));
#else
    spdlog::debug("CH341 direct detach: skipped");
#endif
    const int claimed = libusb_claim_interface(handle_, 0);
    spdlog::debug("CH341 direct claim: result={} ({})", claimed,
                  libusb_error_name(claimed));
    if (claimed != 0) {
      error = open_failure("认领接口", claimed);
      return false;
    }
    interface_claimed_ = true;
    return true;
  }

  bool load_endpoints(std::string& error) {
    libusb_config_descriptor* config = nullptr;
    const int result = libusb_get_active_config_descriptor(
        libusb_get_device(handle_), &config);
    if (result != 0 || config == nullptr) {
      error = usb_failure("读取接口", result);
      return false;
    }
    const auto release = [](libusb_config_descriptor* value) {
      libusb_free_config_descriptor(value);
    };
    std::unique_ptr<libusb_config_descriptor, decltype(release)> guard(config,
                                                                       release);
    if (config->bNumInterfaces < 1 || config->interface[0].num_altsetting < 1) {
      error = "未找到 bulk 端点";
      return false;
    }
    const auto& alternate = config->interface[0].altsetting[0];
    int in_packet = 0;
    int out_packet = 0;
    for (int index = 0; index < alternate.bNumEndpoints; ++index) {
      const auto& endpoint = alternate.endpoint[index];
      if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
          LIBUSB_TRANSFER_TYPE_BULK) {
        continue;
      }
      const auto packet = static_cast<int>(
          static_cast<unsigned>(endpoint.wMaxPacketSize) & 0x07ffu);
      if (packet <= 0 || static_cast<std::size_t>(packet) > kMaxPacketBytes) {
        spdlog::debug(
            "CH341 direct endpoints: address=0x{:02x} packet={} rejected",
            endpoint.bEndpointAddress, packet);
        error = "端点包长超出 CH341 direct 限制";
        return false;
      }
      if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0) {
        in_endpoint_ = endpoint.bEndpointAddress;
        in_packet = packet;
      } else {
        out_endpoint_ = endpoint.bEndpointAddress;
        out_packet = packet;
      }
    }
    spdlog::debug(
        "CH341 direct endpoints: in=0x{:02x} in_packet={} out=0x{:02x} "
        "out_packet={}",
        in_endpoint_, in_packet, out_endpoint_, out_packet);
    if (in_endpoint_ == 0 || out_endpoint_ == 0 || in_packet == 0 ||
        out_packet == 0) {
      error = "未找到 bulk 端点";
      return false;
    }
    in_buffer_.assign(static_cast<std::size_t>(in_packet), 0);
    ready_.assign(in_buffer_.size(), 0);
    out_buffer_.assign(static_cast<std::size_t>(std::min(out_packet, 32)), 0);
    return true;
  }

  bool allocate_transfers(std::string& error) {
    in_transfer_ = libusb_alloc_transfer(0);
    out_transfer_ = libusb_alloc_transfer(0);
    if (!in_transfer_ || !out_transfer_) {
      error = "分配 USB 传输失败";
      return false;
    }
    return true;
  }

  bool control(std::uint8_t request_type, std::uint8_t request,
               std::uint16_t value, std::uint16_t index, unsigned char* data,
               std::uint16_t length, const char* step, std::string& error) {
    const int result =
        libusb_control_transfer(handle_, request_type, request, value, index,
                                data, length, kControlTimeoutMs);
    spdlog::debug(
        "CH341 direct control: step={} request=0x{:02x} wValue=0x{:04x} "
        "wIndex=0x{:04x} result={} ({})",
        step, request, value, index, result,
        result < 0 ? libusb_error_name(result) : "ok");
    const int expected = length;
    if (result != expected) {
      error = result < 0 ? usb_failure(step, result)
                         : std::string(step) + "失败: 短读取";
      return false;
    }
    return true;
  }

  bool configure_uart(int baud, std::string& error) {
    unsigned char version_bytes[2]{};
    const auto vendor_in = static_cast<std::uint8_t>(
        static_cast<unsigned>(LIBUSB_ENDPOINT_IN) |
        static_cast<unsigned>(LIBUSB_REQUEST_TYPE_VENDOR) |
        static_cast<unsigned>(LIBUSB_RECIPIENT_DEVICE));
    const auto vendor_out = static_cast<std::uint8_t>(
        static_cast<unsigned>(LIBUSB_ENDPOINT_OUT) |
        static_cast<unsigned>(LIBUSB_REQUEST_TYPE_VENDOR) |
        static_cast<unsigned>(LIBUSB_RECIPIENT_DEVICE));
    if (!control(vendor_in, kRequestReadVersion, 0, 0, version_bytes, 2,
                 "读版本", error)) {
      return false;
    }
    version_ = version_bytes[0];
    spdlog::debug("CH341 direct version: byte=0x{:02x}", version_);
    if (!control(vendor_out, kRequestSerialInit, 0, 0, nullptr, 0, "串口初始化",
                 error)) {
      return false;
    }
    const auto divisor = ch341_divisor_register(baud, version_);
    if (!divisor) {
      spdlog::debug("CH341 direct divisor: baud={} unsupported", baud);
      error = "CH341 direct 不支持该波特率";
      return false;
    }
    spdlog::debug("CH341 direct divisor: value=0x{:04x} bit7={}", *divisor,
                  (*divisor & 0x80u) != 0 ? 1 : 0);
    if (!control(vendor_out, kRequestWriteRegister, kDivisorValue, *divisor,
                 nullptr, 0, "波特率", error)) {
      return false;
    }
    if (version_ < 0x30) {
      spdlog::debug("CH341 direct lcr: skipped version=0x{:02x}", version_);
    } else if (!control(vendor_out, kRequestWriteRegister, kLineControlValue,
                        kLineControlIndex, nullptr, 0, "线路控制", error)) {
      return false;
    }
    if (!control(vendor_out, kRequestWriteRegister, kFlowControlValue, 0,
                 nullptr, 0, "流控", error)) {
      return false;
    }
    const auto modem = static_cast<std::uint16_t>(~kAssertedLines);
    return control(vendor_out, kRequestModemControl, modem, 0, nullptr, 0,
                   "modem 控制", error);
  }

  bool discard_stale_input(std::string& error) {
    std::vector<unsigned char> junk(in_buffer_.size());
    std::uint64_t discarded = 0;
    for (;;) {
      int transferred = 0;
      const int result = libusb_bulk_transfer(
          handle_, in_endpoint_, junk.data(), static_cast<int>(junk.size()),
          &transferred, kDrainTimeoutMs);
      if (result == LIBUSB_ERROR_TIMEOUT) {
        spdlog::debug("CH341 direct drain: discarded={} stop=timeout",
                      discarded);
        return true;
      }
      if (result < 0) {
        spdlog::debug("CH341 direct drain: discarded={} stop={}", discarded,
                      libusb_error_name(result));
        error = usb_failure("清空接收", result);
        return false;
      }
      if (transferred > 0) {
        discarded += static_cast<std::uint64_t>(transferred);
      }
    }
  }

  void pump() {
    if (!context_) {
      return;
    }
    timeval timeout{0, 0};
    libusb_handle_events_timeout_completed(context_, &timeout, nullptr);
    accept_in();
    accept_out();
  }

  void mark_fault(std::string_view reason) {
    if (fault_) {
      return;
    }
    fault_ = true;
    spdlog::debug("CH341 direct fault: {}", reason);
  }

  void accept_in() {
    if (!in_completed_) {
      return;
    }
    in_completed_ = false;
    if (closing_ && in_status_ == LIBUSB_TRANSFER_CANCELLED) {
      return;
    }
    if (in_status_ != LIBUSB_TRANSFER_COMPLETED) {
      mark_fault(std::string("in status=") + transfer_status_name(in_status_));
      return;
    }
    if (in_actual_ < 0 ||
        static_cast<std::size_t>(in_actual_) > in_buffer_.size()) {
      mark_fault("in length");
      return;
    }
    if (ready_offset_ < ready_size_) {
      mark_fault("in unread");
      return;
    }
    ready_size_ = static_cast<std::size_t>(in_actual_);
    ready_offset_ = 0;
    if (ready_size_ > 0) {
      std::copy_n(in_buffer_.data(), ready_size_, ready_.data());
    }
  }

  void accept_out() {
    if (!out_completed_) {
      return;
    }
    out_completed_ = false;
    if (closing_ && out_status_ == LIBUSB_TRANSFER_CANCELLED) {
      return;
    }
    if (out_status_ != LIBUSB_TRANSFER_COMPLETED) {
      mark_fault(std::string("out status=") +
                 transfer_status_name(out_status_));
      return;
    }
    if (out_actual_ != out_submitted_) {
      mark_fault("out length");
    }
  }

  void submit_in() {
    if (!open_ || closing_ || fault_ || in_pending_ ||
        ready_offset_ < ready_size_) {
      return;
    }
    libusb_fill_bulk_transfer(
        in_transfer_, handle_, in_endpoint_, in_buffer_.data(),
        static_cast<int>(in_buffer_.size()), &on_in, this, 0);
    in_completed_ = false;
    const int submitted = libusb_submit_transfer(in_transfer_);
    if (submitted != 0) {
      mark_fault(std::string("submit in ") + libusb_error_name(submitted));
      return;
    }
    in_pending_ = true;
  }

  static bool same_path(libusb_device* device, const Ch341UsbAddress& address) {
    if (libusb_get_bus_number(device) != address.bus) {
      return false;
    }
    std::uint8_t numbers[7]{};
    const int depth = libusb_get_port_numbers(device, numbers, 7);
    if (depth != address.port_count) {
      return false;
    }
    for (int index = 0; index < depth; ++index) {
      if (numbers[index] != address.ports[index]) {
        return false;
      }
    }
    return true;
  }

  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  libusb_transfer* in_transfer_ = nullptr;
  libusb_transfer* out_transfer_ = nullptr;
  std::vector<unsigned char> in_buffer_;
  std::vector<unsigned char> out_buffer_;
  std::vector<unsigned char> ready_;
  std::size_t ready_offset_ = 0;
  std::size_t ready_size_ = 0;
  libusb_transfer_status in_status_ = LIBUSB_TRANSFER_COMPLETED;
  libusb_transfer_status out_status_ = LIBUSB_TRANSFER_COMPLETED;
  int in_actual_ = 0;
  int out_actual_ = 0;
  int out_submitted_ = 0;
  std::uint8_t in_endpoint_ = 0;
  std::uint8_t out_endpoint_ = 0;
  std::uint8_t version_ = 0;
  bool interface_claimed_ = false;
  bool open_ = false;
  bool closing_ = false;
  bool fault_ = false;
  bool in_pending_ = false;
  bool out_pending_ = false;
  bool in_completed_ = false;
  bool out_completed_ = false;
};

bool same_device(libusb_device* device, std::string& name) {
  libusb_device_descriptor descriptor{};
  if (libusb_get_device_descriptor(device, &descriptor) != 0 ||
      descriptor.idVendor != kCh341DirectVendorId ||
      descriptor.idProduct != kCh341DirectProductId) {
    return false;
  }
  const int bus = libusb_get_bus_number(device);
  if (bus < 1 || bus > 255) {
    return false;
  }
  std::uint8_t numbers[7]{};
  const int depth = libusb_get_port_numbers(device, numbers, 7);
  if (depth < 1) {
    return false;
  }
  name = format_ch341_usb_name(
      static_cast<std::uint8_t>(bus),
      std::span<const std::uint8_t>(numbers, static_cast<std::size_t>(depth)));
  return !name.empty();
}

}  // namespace

std::vector<SerialPortInfo> enumerate_ch341_usb_ports() {
  // Descriptors, bus, and port numbers only. The GUI calls this every second
  // on the main thread, so this path must not libusb_open.
  libusb_context* context = nullptr;
  if (libusb_init(&context) != 0) {
    spdlog::debug("CH341 direct enumeration skipped: libusb_init failed");
    return {};
  }
  const auto exit_context = [](libusb_context* value) { libusb_exit(value); };
  std::unique_ptr<libusb_context, decltype(exit_context)> guard(context,
                                                                exit_context);
  libusb_device** list = nullptr;
  const ssize_t count = libusb_get_device_list(context, &list);
  if (count < 0) {
    spdlog::debug("CH341 direct enumeration skipped: {}",
                  libusb_error_name(static_cast<int>(count)));
    return {};
  }
  std::vector<SerialPortInfo> ports;
  for (ssize_t index = 0; index < count; ++index) {
    std::string name;
    if (!same_device(list[index], name)) {
      continue;
    }
    SerialPortInfo info;
    info.name = std::move(name);
    info.description = "CH341 (USB direct)";
    info.usb_vendor_id = kCh341DirectVendorId;
    info.usb_product_id = kCh341DirectProductId;
    ports.push_back(std::move(info));
  }
  libusb_free_device_list(list, 1);
  return ports;
}

SerialIo make_ch341_usb_io() {
  auto transport = std::make_shared<Ch341DirectTransport>();
  return {
      [transport](const std::string& name, int baud, std::string& error) {
        return transport->open(name, baud, error);
      },
      [transport] { transport->close(); },
      [transport](std::span<const std::uint8_t> bytes) {
        return transport->write(bytes);
      },
      [transport](std::span<std::uint8_t> bytes) {
        return transport->read(bytes);
      },
  };
}

}  // namespace kvmux

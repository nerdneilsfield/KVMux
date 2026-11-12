#include <libusb.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "control/ch341_usb.hpp"
#include "control/control_sink.hpp"
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
    spdlog::debug("CH341 direct open: path={} baud={} version=0x{:02x}", name,
                  baud, version_);
    return true;
  }

  void close() noexcept {
    closing_ = true;
    if (context_ && (in_pending_ || out_pending_)) {
      if (in_pending_ && in_transfer_) {
        libusb_cancel_transfer(in_transfer_);
      }
      if (out_pending_ && out_transfer_) {
        libusb_cancel_transfer(out_transfer_);
      }
      const auto started = std::chrono::steady_clock::now();
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
      fault_ = true;
      return -1;
    }
    out_pending_ = true;
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
    return static_cast<std::ptrdiff_t>(count);
  }

 private:
  static void on_in(libusb_transfer* transfer) {
    auto* self = static_cast<Ch341DirectTransport*>(transfer->user_data);
    self->in_status_ = transfer->status;
    self->in_actual_ = transfer->actual_length;
    self->in_pending_ = false;
    self->in_completed_ = true;
  }

  static void on_out(libusb_transfer* transfer) {
    auto* self = static_cast<Ch341DirectTransport*>(transfer->user_data);
    self->out_status_ = transfer->status;
    self->out_actual_ = transfer->actual_length;
    self->out_pending_ = false;
    self->out_completed_ = true;
  }

  bool claim_device(const Ch341UsbAddress& address, std::string& error) {
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context_, &list);
    if (count < 0) {
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
      error = usb_failure("读取配置", configured);
      return false;
    }
    if (configuration != 1) {
      const int set = libusb_set_configuration(handle_, 1);
      if (set != 0) {
        error = open_failure("设置配置", set);
        return false;
      }
    }
#if defined(__linux__)
    libusb_set_auto_detach_kernel_driver(handle_, 1);
#endif
    const int claimed = libusb_claim_interface(handle_, 0);
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
    if (!control(vendor_out, kRequestSerialInit, 0, 0, nullptr, 0, "串口初始化",
                 error)) {
      return false;
    }
    const auto divisor = ch341_divisor_register(baud, version_);
    if (!divisor) {
      error = "CH341 direct 不支持该波特率";
      return false;
    }
    if (!control(vendor_out, kRequestWriteRegister, kDivisorValue, *divisor,
                 nullptr, 0, "波特率", error)) {
      return false;
    }
    if (version_ >= 0x30 &&
        !control(vendor_out, kRequestWriteRegister, kLineControlValue,
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
    for (;;) {
      int transferred = 0;
      const int result = libusb_bulk_transfer(
          handle_, in_endpoint_, junk.data(), static_cast<int>(junk.size()),
          &transferred, kDrainTimeoutMs);
      if (result == LIBUSB_ERROR_TIMEOUT) {
        return true;
      }
      if (result < 0) {
        error = usb_failure("清空接收", result);
        return false;
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

  void accept_in() {
    if (!in_completed_) {
      return;
    }
    in_completed_ = false;
    if (closing_ && in_status_ == LIBUSB_TRANSFER_CANCELLED) {
      return;
    }
    if (in_status_ != LIBUSB_TRANSFER_COMPLETED || in_actual_ < 0 ||
        static_cast<std::size_t>(in_actual_) > in_buffer_.size() ||
        ready_offset_ < ready_size_) {
      fault_ = true;
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
    if (out_status_ != LIBUSB_TRANSFER_COMPLETED ||
        out_actual_ != out_submitted_) {
      fault_ = true;
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
      fault_ = true;
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

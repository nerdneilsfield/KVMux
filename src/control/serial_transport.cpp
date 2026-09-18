#include <libserialport.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "control/control_sink.hpp"
#include "control/serial_worker.hpp"

namespace kvmux {
namespace {

bool serial_result(const char* operation, const int result) {
  if (result >= SP_OK) {
    return true;
  }
  // Only SP_ERR_FAIL carries an OS error; other results must not read stale
  // errno.
  if (result == SP_ERR_FAIL) {
    const int code = sp_last_error_code();
    char* message = sp_last_error_message();
    spdlog::debug("Serial {} failed: result={} os_error={} reason={}",
                  operation, result, code, message ? message : "unavailable");
    if (message) {
      sp_free_error_message(message);
    }
  } else {
    const char* reason = result == SP_ERR_ARG    ? "invalid argument"
                         : result == SP_ERR_MEM  ? "allocation failed"
                         : result == SP_ERR_SUPP ? "operation not supported"
                                                 : "unknown result";
    spdlog::debug("Serial {} failed: result={} reason={}", operation, result,
                  reason);
  }
  return false;
}

struct PortDeleter {
  void operator()(sp_port* port) const noexcept {
    if (port) {
      sp_free_port(port);
    }
  }
};

struct PortListDeleter {
  void operator()(sp_port** ports) const noexcept {
    if (ports) {
      sp_free_port_list(ports);
    }
  }
};

class LibserialportTransport {
 public:
  bool open(const std::string& name, const int baud_rate) {
    close();
    spdlog::debug("Serial open: path={} baud={} format=8N1 flow_control=none",
                  name, baud_rate);
    sp_port* candidate = nullptr;
    if (!serial_result("get_port_by_name",
                       sp_get_port_by_name(name.c_str(), &candidate))) {
      return false;
    }
    port_.reset(candidate);
    if (!serial_result("open", sp_open(port_.get(), SP_MODE_READ_WRITE)) ||
        !serial_result("set_baudrate",
                       sp_set_baudrate(port_.get(), baud_rate)) ||
        !serial_result("set_bits", sp_set_bits(port_.get(), 8)) ||
        !serial_result("set_parity",
                       sp_set_parity(port_.get(), SP_PARITY_NONE)) ||
        !serial_result("set_stopbits", sp_set_stopbits(port_.get(), 1)) ||
        !serial_result("set_flowcontrol",
                       sp_set_flowcontrol(port_.get(), SP_FLOWCONTROL_NONE))) {
      close();
      return false;
    }
    (void)serial_result("flush", sp_flush(port_.get(), SP_BUF_BOTH));
    spdlog::debug("Serial open succeeded: path={} baud={}", name, baud_rate);
    return true;
  }

  void close() noexcept {
    if (port_) {
      (void)serial_result("close", sp_close(port_.get()));
    }
    port_.reset();
  }

  std::ptrdiff_t write(const std::span<const std::uint8_t> bytes) {
    if (!port_) {
      return -1;
    }
    const auto result =
        sp_nonblocking_write(port_.get(), bytes.data(), bytes.size());
    (void)serial_result("write", result);
    return result;
  }

  std::ptrdiff_t read(const std::span<std::uint8_t> bytes) {
    if (!port_) {
      return -1;
    }
    const auto result =
        sp_nonblocking_read(port_.get(), bytes.data(), bytes.size());
    (void)serial_result("read", result);
    return result;
  }

 private:
  std::unique_ptr<sp_port, PortDeleter> port_;
};

}  // namespace

std::vector<SerialPortInfo> enumerate_serial_ports() {
  sp_port** raw = nullptr;
  if (!serial_result("list_ports", sp_list_ports(&raw))) {
    return {};
  }
  std::unique_ptr<sp_port*, PortListDeleter> ports(raw);
  std::vector<SerialPortInfo> result;
  for (std::size_t index = 0; raw[index]; ++index) {
    const char* name = sp_get_port_name(raw[index]);
    const char* description = sp_get_port_description(raw[index]);
    SerialPortInfo info{
        name ? name : "", description ? description : "", {}, {}};
    int vid{}, pid{};
    if (sp_get_port_transport(raw[index]) == SP_TRANSPORT_USB &&
        sp_get_port_usb_vid_pid(raw[index], &vid, &pid) == SP_OK && vid >= 0 &&
        vid <= 65535 && pid >= 0 && pid <= 65535) {
      info.usb_vendor_id = static_cast<std::uint16_t>(vid);
      info.usb_product_id = static_cast<std::uint16_t>(pid);
    }
    result.push_back(std::move(info));
  }
  return result;
}

SerialIo make_libserialport_io() {
  auto transport = std::make_shared<LibserialportTransport>();
  return {
      [transport](const std::string& name, int baud) {
        return transport->open(name, baud);
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

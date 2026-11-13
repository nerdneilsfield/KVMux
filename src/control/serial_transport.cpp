#include <libserialport.h>
#include <spdlog/spdlog.h>

#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "control/ch341_usb.hpp"
#include "control/control_sink.hpp"
#include "control/serial_trace.hpp"
#include "control/serial_worker.hpp"

#ifndef KVMUX_HAS_CH341_USB
#define KVMUX_HAS_CH341_USB 0
#endif

namespace kvmux {
#if KVMUX_HAS_CH341_USB
SerialIo make_ch341_usb_io();
std::vector<SerialPortInfo> enumerate_ch341_usb_ports();
#endif

namespace {

bool serial_result(const char* operation, const int result,
                   std::string* error) {
  if (result >= SP_OK) {
    return true;
  }
  std::string reason;
  // Only SP_ERR_FAIL carries an OS error; other results must not read stale
  // errno.
  if (result == SP_ERR_FAIL) {
    const int code = sp_last_error_code();
    char* message = sp_last_error_message();
    reason = message && message[0] != '\0' ? message : "serial port error";
    spdlog::debug("Serial {} failed: result={} os_error={} reason={}",
                  operation, result, code, reason);
    if (message) {
      sp_free_error_message(message);
    }
  } else {
    reason = result == SP_ERR_ARG    ? "invalid argument"
             : result == SP_ERR_MEM  ? "allocation failed"
             : result == SP_ERR_SUPP ? "operation not supported"
                                     : "unknown result";
    spdlog::debug("Serial {} failed: result={} reason={}", operation, result,
                  reason);
  }
  if (error) {
    *error = std::move(reason);
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
  bool open(const std::string& name, const int baud_rate, std::string& error) {
    close();
    error.clear();
    spdlog::debug("Serial open: path={} baud={} format=8N1 flow_control=none",
                  name, baud_rate);
    sp_port* candidate = nullptr;
    if (!serial_result("get_port_by_name",
                       sp_get_port_by_name(name.c_str(), &candidate), &error)) {
      return false;
    }
    port_.reset(candidate);
    if (!serial_result("open", sp_open(port_.get(), SP_MODE_READ_WRITE),
                       &error) ||
        !serial_result("set_baudrate", sp_set_baudrate(port_.get(), baud_rate),
                       &error) ||
        !serial_result("set_bits", sp_set_bits(port_.get(), 8), &error) ||
        !serial_result("set_parity", sp_set_parity(port_.get(), SP_PARITY_NONE),
                       &error) ||
        !serial_result("set_stopbits", sp_set_stopbits(port_.get(), 1),
                       &error) ||
        !serial_result("set_flowcontrol",
                       sp_set_flowcontrol(port_.get(), SP_FLOWCONTROL_NONE),
                       &error)) {
      close();
      return false;
    }
    (void)serial_result("flush", sp_flush(port_.get(), SP_BUF_BOTH), nullptr);
    spdlog::debug("Serial open succeeded: path={} baud={}", name, baud_rate);
    return true;
  }

  void close() noexcept {
    if (port_) {
      (void)serial_result("close", sp_close(port_.get()), nullptr);
    }
    port_.reset();
  }

  std::ptrdiff_t write(const std::span<const std::uint8_t> bytes) {
    if (!port_) {
      return -1;
    }
    const auto result =
        sp_nonblocking_write(port_.get(), bytes.data(), bytes.size());
    if (result > 0 && static_cast<std::size_t>(result) <= bytes.size()) {
      log_serial_out("serial", bytes.first(static_cast<std::size_t>(result)));
    }
    (void)serial_result("write", result, nullptr);
    return result;
  }

  std::ptrdiff_t read(const std::span<std::uint8_t> bytes) {
    if (!port_) {
      return -1;
    }
    const auto result =
        sp_nonblocking_read(port_.get(), bytes.data(), bytes.size());
    if (result > 0 && static_cast<std::size_t>(result) <= bytes.size()) {
      log_serial_in("serial", bytes.first(static_cast<std::size_t>(result)));
    }
    (void)serial_result("read", result, nullptr);
    return result;
  }

 private:
  std::unique_ptr<sp_port, PortDeleter> port_;
};

enum class OpenBackend { none, system, direct };

class SerialDispatch {
 public:
  bool open(const std::string& name, int baud, std::string& error) {
    close();
    if (name.starts_with(kCh341UsbPrefix)) {
#if KVMUX_HAS_CH341_USB
      if (!direct_.open(name, baud, error)) {
        return false;
      }
      backend_ = OpenBackend::direct;
      return true;
#else
      error = "未编译 CH341 direct 支持";
      return false;
#endif
    }
    if (!system_->open(name, baud, error)) {
      return false;
    }
    backend_ = OpenBackend::system;
    return true;
  }

  void close() noexcept {
    system_->close();
#if KVMUX_HAS_CH341_USB
    direct_.close();
#endif
    backend_ = OpenBackend::none;
  }

  std::ptrdiff_t write(std::span<const std::uint8_t> bytes) {
#if KVMUX_HAS_CH341_USB
    if (backend_ == OpenBackend::direct) {
      return direct_.write(bytes);
    }
#endif
    if (backend_ == OpenBackend::system) {
      return system_->write(bytes);
    }
    return -1;
  }

  std::ptrdiff_t read(std::span<std::uint8_t> bytes) {
#if KVMUX_HAS_CH341_USB
    if (backend_ == OpenBackend::direct) {
      return direct_.read(bytes);
    }
#endif
    if (backend_ == OpenBackend::system) {
      return system_->read(bytes);
    }
    return -1;
  }

 private:
  std::shared_ptr<LibserialportTransport> system_ =
      std::make_shared<LibserialportTransport>();
#if KVMUX_HAS_CH341_USB
  SerialIo direct_ = make_ch341_usb_io();
#endif
  OpenBackend backend_ = OpenBackend::none;
};

}  // namespace

std::vector<SerialPortInfo> enumerate_serial_ports() {
  std::vector<SerialPortInfo> result;
  sp_port** raw = nullptr;
  if (serial_result("list_ports", sp_list_ports(&raw), nullptr)) {
    std::unique_ptr<sp_port*, PortListDeleter> ports(raw);
    for (std::size_t index = 0; raw[index]; ++index) {
      const char* name = sp_get_port_name(raw[index]);
      const char* description = sp_get_port_description(raw[index]);
      SerialPortInfo info{
          name ? name : "", description ? description : "", {}, {}};
      int vid{}, pid{};
      if (sp_get_port_transport(raw[index]) == SP_TRANSPORT_USB &&
          sp_get_port_usb_vid_pid(raw[index], &vid, &pid) == SP_OK &&
          vid >= 0 && vid <= 65535 && pid >= 0 && pid <= 65535) {
        info.usb_vendor_id = static_cast<std::uint16_t>(vid);
        info.usb_product_id = static_cast<std::uint16_t>(pid);
      }
      result.push_back(std::move(info));
    }
  }
#if KVMUX_HAS_CH341_USB
  auto direct = enumerate_ch341_usb_ports();
  result.insert(result.end(), std::make_move_iterator(direct.begin()),
                std::make_move_iterator(direct.end()));
#endif
  return result;
}

SerialIo make_serial_io() {
  auto dispatch = std::make_shared<SerialDispatch>();
  return {
      [dispatch](const std::string& name, int baud, std::string& error) {
        return dispatch->open(name, baud, error);
      },
      [dispatch] { dispatch->close(); },
      [dispatch](std::span<const std::uint8_t> bytes) {
        return dispatch->write(bytes);
      },
      [dispatch](std::span<std::uint8_t> bytes) {
        return dispatch->read(bytes);
      },
  };
}

}  // namespace kvmux

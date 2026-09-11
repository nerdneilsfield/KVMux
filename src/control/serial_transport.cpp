#include "control/control_sink.hpp"
#include "control/serial_worker.hpp"

#include <libserialport.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace kvmux {
namespace {

struct PortDeleter {
    void operator()(sp_port* port) const noexcept {
        if (port) { sp_free_port(port); }
    }
};

struct PortListDeleter {
    void operator()(sp_port** ports) const noexcept {
        if (ports) { sp_free_port_list(ports); }
    }
};

class LibserialportTransport {
public:
    bool open(const std::string& name, const int baud_rate) {
        close();
        sp_port* candidate = nullptr;
        if (sp_get_port_by_name(name.c_str(), &candidate) != SP_OK) { return false; }
        port_.reset(candidate);
        if (sp_open(port_.get(), SP_MODE_READ_WRITE) != SP_OK ||
            sp_set_baudrate(port_.get(), baud_rate) != SP_OK ||
            sp_set_bits(port_.get(), 8) != SP_OK ||
            sp_set_parity(port_.get(), SP_PARITY_NONE) != SP_OK ||
            sp_set_stopbits(port_.get(), 1) != SP_OK ||
            sp_set_flowcontrol(port_.get(), SP_FLOWCONTROL_NONE) != SP_OK) {
            close();
            return false;
        }
        (void)sp_flush(port_.get(), SP_BUF_BOTH);
        return true;
    }

    void close() noexcept {
        if (port_) { (void)sp_close(port_.get()); }
        port_.reset();
    }

    std::ptrdiff_t write(const std::span<const std::uint8_t> bytes) {
        if (!port_) { return -1; }
        return sp_nonblocking_write(port_.get(), bytes.data(), bytes.size());
    }

    std::ptrdiff_t read(const std::span<std::uint8_t> bytes) {
        if (!port_) { return -1; }
        return sp_nonblocking_read(port_.get(), bytes.data(), bytes.size());
    }

private:
    std::unique_ptr<sp_port, PortDeleter> port_;
};

}  // namespace

std::vector<SerialPortInfo> enumerate_serial_ports() {
    sp_port** raw = nullptr;
    if (sp_list_ports(&raw) != SP_OK) { return {}; }
    std::unique_ptr<sp_port*, PortListDeleter> ports(raw);
    std::vector<SerialPortInfo> result;
    for (std::size_t index = 0; raw[index]; ++index) {
        const char* name = sp_get_port_name(raw[index]);
        const char* description = sp_get_port_description(raw[index]);
        SerialPortInfo info{name ? name : "", description ? description : "", {}, {}};
        int vid{}, pid{};
        if (sp_get_port_transport(raw[index]) == SP_TRANSPORT_USB &&
            sp_get_port_usb_vid_pid(raw[index], &vid, &pid) == SP_OK &&
            vid >= 0 && vid <= 65535 && pid >= 0 && pid <= 65535) {
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
        [transport](const std::string& name, int baud) { return transport->open(name, baud); },
        [transport] { transport->close(); },
        [transport](std::span<const std::uint8_t> bytes) { return transport->write(bytes); },
        [transport](std::span<std::uint8_t> bytes) { return transport->read(bytes); },
    };
}

}  // namespace kvmux

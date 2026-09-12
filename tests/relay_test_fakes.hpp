#pragma once
#include "network/relay_server.hpp"
#include "control/ch9329_protocol.hpp"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
using namespace kvmux;
using namespace std::chrono_literals;
struct FakeSerial {
    std::mutex mutex;
    bool opened{};
    std::size_t write_limit{3};
    bool answer{true};
    bool hold_mouse_ack{};
    std::deque<std::uint8_t> held_ack;
    std::deque<std::uint8_t> incoming;
    kvmux::ch9329::Parser requests;
    std::vector<kvmux::ch9329::Frame> received;

    kvmux::SerialIo io() {
        return {
            [this](const std::string&, int) { std::lock_guard lock(mutex); opened = true; return true; },
            [this] { std::lock_guard lock(mutex); opened = false; },
            [this](std::span<const std::uint8_t> bytes) -> std::ptrdiff_t {
                std::lock_guard lock(mutex);
                if (!opened) { return -1; }
                const auto count = std::min(write_limit, bytes.size());
                auto frames = requests.feed(bytes.first(count));
                for (const auto& request : frames) {
                    received.push_back(request);
                    if (!answer) { continue; }
                    kvmux::ch9329::Frame response{request.address,
                        static_cast<std::uint8_t>(request.command | 0x80U), {0}};
                    if (request.command == 0x01U) {
                        response.data = {0x31, 1, 0, 0, 0, 0, 0, 0};
                    } else if (request.command == 0x08U) {
                        response.data.assign(50, 0);
                        response.data[0] = 0x80;
                        response.data[1] = 0x80;
                    }
                    const auto encoded = kvmux::ch9329::encode(response);
                    auto& destination = hold_mouse_ack && (request.command == 0x04U || request.command == 0x05U) ? held_ack : incoming;
                    destination.insert(destination.end(), encoded.begin(), encoded.end());
                }
                return static_cast<std::ptrdiff_t>(count);
            },
            [this](std::span<std::uint8_t> bytes) -> std::ptrdiff_t {
                std::lock_guard lock(mutex);
                if (!opened) { return -1; }
                const auto count = std::min(bytes.size(), incoming.size());
                for (std::size_t i = 0; i < count; ++i) {
                    bytes[i] = incoming.front(); incoming.pop_front();
                }
                return static_cast<std::ptrdiff_t>(count);
            },
        };
    }
};


struct FakeCapture final:CaptureSource {
    std::vector<std::uint8_t> jpeg;std::uint64_t sequence{};
    std::atomic<bool> stalled{false};
    std::chrono::steady_clock::time_point previous{};
    std::vector<DeviceInfo> enumerate_devices() override{return{};}
    std::vector<CaptureMode> enumerate_modes(const std::string&) override{return{};}
    void start(const CaptureMode&) override{}
    void stop() noexcept override{}
    std::optional<CaptureSample> take_latest_sample() override {
        auto now=std::chrono::steady_clock::now();
        if(stalled||now-previous<20ms)return{};previous=now;
        return CaptureSample::make_mjpeg(1,++sequence,now,16,16,jpeg);
    }
    CaptureSnapshot snapshot() const override {
        CaptureSnapshot out;out.state=CaptureState::streaming;
        out.actual_mode={"fake",16,16,{50,1},PixelFormat::mjpeg,PixelFormat::mjpeg,"MJPEG"};return out;
    }
};

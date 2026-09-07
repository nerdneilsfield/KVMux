#include "video/capture_sample.hpp"
#include "video/video_mailbox.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

}  // namespace

int main() {
    using namespace kvmux;
    const auto now = std::chrono::steady_clock::now();

    std::vector<std::uint8_t> raw_bytes(26, 0x4a);
    const std::array planes{PlaneLayout{4, 8, 6, 3}};
    auto raw = CaptureSample::make_raw(2, 10, now, 3, 3, PixelFormat::yuy2,
                                       planes, raw_bytes);
    require(raw.has_value() && raw->raw->bytes.data() != raw_bytes.data(),
            "raw sample owns a copy");
    raw_bytes[4] = 0;
    require(raw->raw->bytes[4] == 0x4a, "source mutation cannot change sample");

    const std::array bad_plane{PlaneLayout{20, 8, 8, 2}};
    require(!CaptureSample::make_raw(2, 11, now, 3, 3, PixelFormat::yuy2,
                                     bad_plane, raw_bytes),
            "out-of-bounds plane rejected");
    require(!CaptureSample::make_raw(2, 12, now, 1921, 1, PixelFormat::yuy2,
                                     planes, raw_bytes),
            "oversized dimensions rejected");

    const std::array<std::uint8_t, 4> jpeg{0xff, 0xd8, 0xff, 0xd9};
    auto mjpeg = CaptureSample::make_mjpeg(2, 13, now, 640, 480, jpeg);
    require(mjpeg.has_value() && mjpeg->mjpeg->payload_size == jpeg.size(),
            "mjpeg accepted");
    require(mjpeg->mjpeg->bytes.size() == jpeg.size() + kInputPaddingBytes,
            "mjpeg padding allocated");
    for (std::size_t i = jpeg.size(); i < mjpeg->mjpeg->bytes.size(); ++i) {
        require(mjpeg->mjpeg->bytes[i] == 0, "mjpeg padding is zero");
    }

    GenerationMailbox<CaptureSample> mailbox;
    mailbox.set_generation(2);
    mailbox.publish(std::move(*raw));
    mailbox.publish(std::move(*mjpeg));
    require(mailbox.overwritten() == 1, "new sample overwrites old sample");
    auto newest = mailbox.take();
    require(newest && newest->sequence == 13, "consumer gets newest sample");
    require(!mailbox.take(), "sample is consumed once");

    auto stale = CaptureSample::make_mjpeg(2, 14, now, 640, 480, jpeg);
    mailbox.set_generation(3);
    mailbox.publish(std::move(*stale));
    require(!mailbox.take(), "stale generation is rejected");

    return EXIT_SUCCESS;
}

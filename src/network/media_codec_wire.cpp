#include "network/media_codec_wire.hpp"

#include <bit>
#include <chrono>
#include <limits>

namespace kvmux::relay {
namespace {
template <typename T>
void append_be(std::vector<std::uint8_t>& out, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t i = sizeof(T); i > 0; --i) out.push_back(static_cast<std::uint8_t>(value >> ((i - 1U) * 8U)));
}
template <typename T>
std::optional<T> read_be(std::span<const std::uint8_t> data, std::size_t& pos) {
    static_assert(std::is_unsigned_v<T>);
    if (data.size() - pos < sizeof(T)) return std::nullopt;
    T result{};
    for (std::size_t i = 0; i < sizeof(T); ++i) result = static_cast<T>((result << 8U) | data[pos++]);
    return result;
}
std::optional<std::uint8_t> byte(std::span<const std::uint8_t> data, std::size_t& pos) {
    if (pos == data.size()) return std::nullopt; return data[pos++];
}
bool annex_b_prefix(std::span<const std::uint8_t> bytes) {
    // Framing only. NAL/reference-chain validation belongs to the decoder.
    const std::size_t prefix = bytes.size() >= 3 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 1 ? 3 :
        bytes.size() >= 4 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1 ? 4 : 0;
    return prefix != 0 && bytes.size() >= prefix + 2;
}
bool valid_annex_b_metadata(const EncodedAccessUnit& unit) {
    return (unit.codec == VideoCodec::hevc || unit.codec == VideoCodec::h264) && valid_dimensions(unit.width, unit.height) &&
        unit.sample_aspect_ratio.num > 0 && unit.sample_aspect_ratio.den > 0 &&
        static_cast<unsigned>(unit.color_range) <= 2U &&
        static_cast<unsigned>(unit.color_space) <= 17U && unit.color_space != AVCOL_SPC_RESERVED &&
        ((unit.color_primaries >= AVCOL_PRI_BT709 && unit.color_primaries <= AVCOL_PRI_SMPTE432 &&
          unit.color_primaries != AVCOL_PRI_RESERVED) || unit.color_primaries == AVCOL_PRI_EBU3213) &&
        unit.color_transfer >= AVCOL_TRC_BT709 && static_cast<unsigned>(unit.color_transfer) <= 18U &&
        unit.color_transfer != AVCOL_TRC_RESERVED;
}
}  // namespace

std::vector<std::uint8_t> encode_mjpeg(const CaptureSample& sample) {
    if (!sample.mjpeg || !valid_dimensions(sample.width, sample.height) || sample.mjpeg->payload_size == 0 || sample.mjpeg->payload_size > kMaxCompressedSampleBytes || sample.mjpeg->payload_size > sample.mjpeg->bytes.size() || static_cast<unsigned>(sample.color_range) > static_cast<unsigned>(ColorRange::unknown) || static_cast<unsigned>(sample.color_matrix) > static_cast<unsigned>(ColorMatrix::unknown)) return {};
    std::vector<std::uint8_t> out; out.reserve(28 + sample.mjpeg->payload_size); append_be(out,sample.sequence); append_be<std::uint32_t>(out,sample.width); append_be<std::uint32_t>(out,sample.height); out.push_back(static_cast<std::uint8_t>(sample.color_range)); out.push_back(static_cast<std::uint8_t>(sample.color_matrix)); append_be<std::uint16_t>(out,0); append_be<std::uint32_t>(out,static_cast<std::uint32_t>(sample.mjpeg->payload_size)); out.insert(out.end(),sample.mjpeg->bytes.begin(),sample.mjpeg->bytes.begin()+static_cast<std::ptrdiff_t>(sample.mjpeg->payload_size)); return out;
}
std::optional<CaptureSample> decode_mjpeg(std::span<const std::uint8_t> data, std::uint64_t generation) {
    std::size_t pos{};
    const auto sequence = read_be<std::uint64_t>(data, pos);
    const auto width = read_be<std::uint32_t>(data, pos);
    const auto height = read_be<std::uint32_t>(data, pos);
    const auto range = byte(data, pos);
    const auto matrix = byte(data, pos);
    const auto reserved = read_be<std::uint16_t>(data, pos);
    const auto size = read_be<std::uint32_t>(data, pos);
    if(!sequence||!width||!height||!range||!matrix||!reserved||!size||*reserved!=0||*range>static_cast<std::uint8_t>(ColorRange::unknown)||*matrix>static_cast<std::uint8_t>(ColorMatrix::unknown)||data.size()-pos!=*size) return {};
    auto sample=CaptureSample::make_mjpeg(generation,*sequence,std::chrono::steady_clock::now(),*width,*height,data.subspan(pos)); if(!sample)return{}; sample->color_range=static_cast<ColorRange>(*range);sample->color_matrix=static_cast<ColorMatrix>(*matrix);return sample;
}
std::vector<std::uint8_t> encode_annex_b(const EncodedAccessUnit& unit) {
    if (!valid_annex_b_metadata(unit) || unit.bytes.size() > kMaxCompressedSampleBytes ||
        !annex_b_prefix(unit.bytes)) return {};
    std::vector<std::uint8_t> out;
    out.reserve(58 + unit.bytes.size());
    append_be(out, unit.generation);
    append_be(out, unit.encoded_sequence);
    append_be(out, unit.capture_sequence);
    append_be(out, std::bit_cast<std::uint64_t>(unit.pts_ns));
    append_be(out, unit.width); append_be(out, unit.height);
    append_be(out, static_cast<std::uint32_t>(unit.sample_aspect_ratio.num));
    append_be(out, static_cast<std::uint32_t>(unit.sample_aspect_ratio.den));
    out.push_back(static_cast<std::uint8_t>(unit.color_range));
    out.push_back(static_cast<std::uint8_t>(unit.color_space));
    out.push_back(static_cast<std::uint8_t>(unit.color_primaries));
    out.push_back(static_cast<std::uint8_t>(unit.color_transfer));
    out.push_back(unit.idr ? 1 : 0); out.push_back(0);
    append_be(out, static_cast<std::uint32_t>(unit.bytes.size()));
    out.insert(out.end(), unit.bytes.begin(), unit.bytes.end());
    return out;
}
std::optional<EncodedAccessUnit> decode_annex_b(std::span<const std::uint8_t> data, VideoCodec codec) {
    if (data.size() < 58 || data.size() > 58 + kMaxCompressedSampleBytes) return {};
    EncodedAccessUnit unit;
    std::size_t pos{};
    unit.codec = codec;
    unit.generation = *read_be<std::uint64_t>(data, pos);
    unit.encoded_sequence = *read_be<std::uint64_t>(data, pos);
    unit.capture_sequence = *read_be<std::uint64_t>(data, pos);
    unit.pts_ns = std::bit_cast<std::int64_t>(*read_be<std::uint64_t>(data, pos));
    unit.width = *read_be<std::uint32_t>(data, pos);
    unit.height = *read_be<std::uint32_t>(data, pos);
    const auto sar_num = *read_be<std::uint32_t>(data, pos);
    const auto sar_den = *read_be<std::uint32_t>(data, pos);
    if (sar_num > static_cast<unsigned>(std::numeric_limits<int>::max()) ||
        sar_den > static_cast<unsigned>(std::numeric_limits<int>::max())) return {};
    unit.sample_aspect_ratio = {static_cast<int>(sar_num), static_cast<int>(sar_den)};
    unit.color_range = static_cast<AVColorRange>(*byte(data, pos));
    unit.color_space = static_cast<AVColorSpace>(*byte(data, pos));
    unit.color_primaries = static_cast<AVColorPrimaries>(*byte(data, pos));
    unit.color_transfer = static_cast<AVColorTransferCharacteristic>(*byte(data, pos));
    const auto idr = *byte(data, pos), reserved = *byte(data, pos);
    const auto size = *read_be<std::uint32_t>(data, pos);
    if (idr > 1 || reserved != 0 || size != data.size() - pos ||
        !valid_annex_b_metadata(unit) || !annex_b_prefix(data.subspan(pos))) return {};
    unit.idr = idr != 0;
    // All lengths and metadata are checked before allocating compressed storage.
    unit.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end());
    unit.arrival = std::chrono::steady_clock::now();
    return unit;
}
}  // namespace kvmux::relay

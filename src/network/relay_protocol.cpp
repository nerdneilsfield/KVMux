#include "network/relay_protocol.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>

namespace kvmux::relay {
namespace {
constexpr std::array<std::uint8_t, 4> kMagic{'K', 'V', 'M', 'X'};
constexpr std::size_t kHeaderSize = 12;

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
void append_double(std::vector<std::uint8_t>& out, double value) { append_be(out, std::bit_cast<std::uint64_t>(value)); }
std::optional<double> read_double(std::span<const std::uint8_t> data, std::size_t& pos) {
    const auto bits = read_be<std::uint64_t>(data, pos); if (!bits) return std::nullopt;
    return std::bit_cast<double>(*bits);
}
std::optional<std::uint8_t> byte(std::span<const std::uint8_t> data, std::size_t& pos) {
    if (pos == data.size()) return std::nullopt; return data[pos++];
}
bool finite(double value) { return value >= -std::numeric_limits<double>::max() && value <= std::numeric_limits<double>::max(); }
}  // namespace

std::vector<std::uint8_t> encode_packet(PacketType type, std::span<const std::uint8_t> payload) {
    if (payload.size() > kMaxPacketBytes || type == PacketType{}) return {};
    std::vector<std::uint8_t> result; result.reserve(kHeaderSize + payload.size());
    result.insert(result.end(), kMagic.begin(), kMagic.end()); append_be<std::uint16_t>(result, kProtocolVersion);
    result.push_back(static_cast<std::uint8_t>(type)); result.push_back(0); append_be<std::uint32_t>(result, static_cast<std::uint32_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end()); return result;
}
std::optional<Packet> decode_packet(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) return std::nullopt;
    std::size_t pos = 4; const auto version = read_be<std::uint16_t>(bytes, pos); const auto raw_type = byte(bytes, pos); const auto reserved = byte(bytes, pos); const auto size = read_be<std::uint32_t>(bytes, pos);
    if (!version || !raw_type || !reserved || !size || *version != kProtocolVersion || *reserved != 0 || *size > kMaxPacketBytes || bytes.size() != kHeaderSize + *size) return std::nullopt;
    if (*raw_type < static_cast<std::uint8_t>(PacketType::hello) || *raw_type > static_cast<std::uint8_t>(PacketType::heartbeat)) return std::nullopt;
    return Packet{static_cast<PacketType>(*raw_type), {bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.end()}};
}
std::vector<std::uint8_t> encode_control(const ControlEvent& event) {
    std::vector<std::uint8_t> out; append_be(out, event.epoch); append_be(out, event.sequence);
    std::visit([&out](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, KeyEdge>) { out.push_back(1); out.push_back(payload.usage); out.push_back(payload.pressed ? 1 : 0); }
        else if constexpr (std::is_same_v<T, AbsoluteMotion>) { out.push_back(2); append_double(out, payload.x); append_double(out, payload.y); }
        else if constexpr (std::is_same_v<T, RelativeMotion>) { out.push_back(3); append_double(out, payload.dx); append_double(out, payload.dy); }
        else if constexpr (std::is_same_v<T, ButtonEdge>) { out.push_back(4); out.push_back(payload.button); out.push_back(payload.pressed ? 1 : 0); append_double(out, payload.x); append_double(out, payload.y); }
        else { out.push_back(5); append_double(out, payload.steps); append_double(out, payload.x); append_double(out, payload.y); }
    }, event.payload); return out;
}
std::optional<ControlEvent> decode_control(std::span<const std::uint8_t> data) {
    std::size_t pos{}; const auto epoch = read_be<std::uint64_t>(data, pos); const auto sequence = read_be<std::uint64_t>(data, pos); const auto kind = byte(data, pos);
    if (!epoch || !sequence || !kind) return std::nullopt; ControlEvent event{*epoch, *sequence, std::chrono::steady_clock::now(), {}};
    if (*kind == 1) { const auto usage=byte(data,pos), pressed=byte(data,pos); if (!usage || !pressed || *pressed > 1) return {}; event.payload=KeyEdge{*usage,*pressed != 0}; }
    else if (*kind == 2 || *kind == 3) { const auto x=read_double(data,pos), y=read_double(data,pos); if (!x||!y||!finite(*x)||!finite(*y)) return {}; event.payload=*kind==2 ? ControlPayload{AbsoluteMotion{*x,*y}} : ControlPayload{RelativeMotion{*x,*y}}; }
    else if (*kind == 4) { const auto button=byte(data,pos), pressed=byte(data,pos); const auto x=read_double(data,pos),y=read_double(data,pos); if(!button||!pressed||*pressed>1||!x||!y||!finite(*x)||!finite(*y))return{}; event.payload=ButtonEdge{*button,*pressed!=0,*x,*y}; }
    else if (*kind == 5) { const auto steps=read_double(data,pos),x=read_double(data,pos),y=read_double(data,pos); if(!steps||!x||!y||!finite(*steps)||!finite(*x)||!finite(*y))return{}; event.payload=VerticalWheel{*steps,*x,*y}; }
    else return {};
    return pos == data.size() ? std::optional<ControlEvent>{std::move(event)} : std::nullopt;
}
std::vector<std::uint8_t> encode_mjpeg(const CaptureSample& sample) {
    if (!sample.mjpeg || sample.width == 0 || sample.height == 0 || sample.mjpeg->payload_size > sample.mjpeg->bytes.size()) return {};
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
}  // namespace kvmux::relay

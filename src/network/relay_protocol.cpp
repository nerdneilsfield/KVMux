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
bool valid_packet_size(PacketType type, std::size_t size) {
    switch (type) {
    case PacketType::hello: return size == 9;
    case PacketType::video_mjpeg: return size > 24 && size <= kMaxCompressedSampleBytes + 24;
    case PacketType::video_hevc: return size >= 63 && size <= kMaxCompressedSampleBytes + 58;
    case PacketType::control: return size >= 19 && size <= 49;
    case PacketType::heartbeat: return size == 26;
    case PacketType::status: return size >= 21 && size <= 533;
    case PacketType::release: return size == 8;
    case PacketType::mouse_mode: return size == 9;
    case PacketType::keyframe_request: return size == 16;
    }
    return false;
}
bool valid_codec(VideoCodec codec) {
    return codec == VideoCodec::mjpeg || codec == VideoCodec::hevc;
}
bool annex_b_prefix(std::span<const std::uint8_t> bytes) {
    // Framing only. NAL/reference-chain validation belongs to the decoder.
    const std::size_t prefix = bytes.size() >= 3 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 1 ? 3 :
        bytes.size() >= 4 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1 ? 4 : 0;
    return prefix != 0 && bytes.size() >= prefix + 2;
}
bool valid_hevc_metadata(const EncodedAccessUnit& unit) {
    return unit.codec == VideoCodec::hevc && valid_dimensions(unit.width, unit.height) &&
        unit.sample_aspect_ratio.num > 0 && unit.sample_aspect_ratio.den > 0 &&
        static_cast<unsigned>(unit.color_range) <= 2U &&
        static_cast<unsigned>(unit.color_space) <= 17U && unit.color_space != AVCOL_SPC_RESERVED &&
        ((unit.color_primaries >= AVCOL_PRI_BT709 && unit.color_primaries <= AVCOL_PRI_SMPTE432 &&
          unit.color_primaries != AVCOL_PRI_RESERVED) || unit.color_primaries == AVCOL_PRI_EBU3213) &&
        unit.color_transfer >= AVCOL_TRC_BT709 && static_cast<unsigned>(unit.color_transfer) <= 18U &&
        unit.color_transfer != AVCOL_TRC_RESERVED;
}
}  // namespace

std::vector<std::uint8_t> encode_packet(PacketType type, std::span<const std::uint8_t> payload) {
    if (!valid_packet_size(type, payload.size())) return {};
    std::vector<std::uint8_t> result; result.reserve(kHeaderSize + payload.size());
    result.insert(result.end(), kMagic.begin(), kMagic.end()); append_be<std::uint16_t>(result, kProtocolVersion);
    result.push_back(static_cast<std::uint8_t>(type)); result.push_back(0); append_be<std::uint32_t>(result, static_cast<std::uint32_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end()); return result;
}
std::optional<Packet> decode_packet(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) return std::nullopt;
    std::size_t pos = 4; const auto version = read_be<std::uint16_t>(bytes, pos); const auto raw_type = byte(bytes, pos); const auto reserved = byte(bytes, pos); const auto size = read_be<std::uint32_t>(bytes, pos);
    if (!version || !raw_type || !reserved || !size || *version != kProtocolVersion || *reserved != 0 || *size > kMaxPacketBytes || bytes.size() != kHeaderSize + *size) return std::nullopt;
    if (!valid_packet_size(static_cast<PacketType>(*raw_type), *size)) return std::nullopt;
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
std::vector<std::uint8_t> encode_hello(const Hello& value) {
    if (!value.session || !valid_codec(value.codec)) return {};
    auto out = encode_session(value.session);
    out.push_back(static_cast<std::uint8_t>(value.codec));
    return out;
}
std::optional<Hello> decode_hello(std::span<const std::uint8_t> data) {
    if (data.size() != 9) return {};
    const auto session = decode_session(data.first(8));
    const auto codec = static_cast<VideoCodec>(data[8]);
    if (!*session || !valid_codec(codec)) return {};
    return Hello{*session, codec};
}
std::vector<std::uint8_t> encode_keyframe_request(const KeyframeRequest& value) {
    if (!value.session) return {};
    auto out = encode_session(value.session);
    append_be(out, value.generation);
    return out;
}
std::optional<KeyframeRequest> decode_keyframe_request(std::span<const std::uint8_t> data) {
    if (data.size() != 16) return {};
    std::size_t pos{};
    const auto session = read_be<std::uint64_t>(data, pos);
    const auto generation = read_be<std::uint64_t>(data, pos);
    if (!*session) return {};
    return KeyframeRequest{*session, *generation};
}
std::vector<std::uint8_t> encode_hevc(const EncodedAccessUnit& unit) {
    if (!valid_hevc_metadata(unit) || unit.bytes.size() > kMaxCompressedSampleBytes ||
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
std::optional<EncodedAccessUnit> decode_hevc(std::span<const std::uint8_t> data) {
    if (data.size() < 58 || data.size() > 58 + kMaxCompressedSampleBytes) return {};
    EncodedAccessUnit unit;
    std::size_t pos{};
    unit.codec = VideoCodec::hevc;
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
        !valid_hevc_metadata(unit) || !annex_b_prefix(data.subspan(pos))) return {};
    unit.idr = idr != 0;
    // All lengths and metadata are checked before allocating compressed storage.
    unit.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end());
    unit.arrival = std::chrono::steady_clock::now();
    return unit;
}
std::vector<std::uint8_t> encode_session(std::uint64_t id) {
    std::vector<std::uint8_t> out; append_be(out,id); return out;
}
std::optional<std::uint64_t> decode_session(std::span<const std::uint8_t> data) {
    std::size_t pos{}; if(data.size()!=8)return{}; return read_be<std::uint64_t>(data,pos);
}
std::vector<std::uint8_t> encode_session_control(const SessionControl& value) {
    auto out=encode_session(value.session); auto event=encode_control(value.event);
    out.insert(out.end(),event.begin(),event.end()); return out;
}
std::optional<SessionControl> decode_session_control(std::span<const std::uint8_t> data) {
    if(data.size()<8)return{}; auto id=decode_session(data.first(8)); auto event=decode_control(data.subspan(8));
    if(!id||!event)return{}; return SessionControl{*id,*event};
}
std::vector<std::uint8_t> encode_heartbeat(const Heartbeat& value) {
    auto out=encode_session(value.session); append_be(out,value.epoch);
    out.push_back(value.gui_active?1:0); out.push_back(value.video_fresh?1:0);
    append_be(out,value.video_sequence); return out;
}
std::optional<Heartbeat> decode_heartbeat(std::span<const std::uint8_t> data) {
    if(data.size()!=26)return{}; std::size_t p{};
    auto id=read_be<std::uint64_t>(data,p),epoch=read_be<std::uint64_t>(data,p);
    auto active=byte(data,p),fresh=byte(data,p); auto seq=read_be<std::uint64_t>(data,p);
    if(*active>1||*fresh>1)return{}; return Heartbeat{*id,*epoch,*active!=0,*fresh!=0,*seq};
}
std::vector<std::uint8_t> encode_status(const Status& value) {
    auto out=encode_session(value.session); const auto& c=value.control; append_be(out,c.epoch);
    out.push_back(static_cast<std::uint8_t>(c.state)); out.push_back(c.target_usb_ready?1:0);
    out.push_back(c.release_confirmed?1:0);
    const auto size=std::min<std::size_t>(c.error.size(),512); append_be<std::uint16_t>(out,static_cast<std::uint16_t>(size));
    out.insert(out.end(),c.error.begin(),c.error.begin()+static_cast<std::ptrdiff_t>(size)); return out;
}
std::optional<Status> decode_status(std::span<const std::uint8_t> data) {
    if(data.size()<21)return{}; std::size_t p{}; Status result;
    result.session=*read_be<std::uint64_t>(data,p); result.control.epoch=*read_be<std::uint64_t>(data,p);
    auto state=byte(data,p),usb=byte(data,p),released=byte(data,p); auto size=read_be<std::uint16_t>(data,p);
    if(*state>static_cast<unsigned>(ControlConnectionState::stopping)||*usb>1||*released>1||*size>512||data.size()-p!=*size)return{};
    result.control.state=static_cast<ControlConnectionState>(*state); result.control.target_usb_ready=*usb!=0;
    result.control.release_confirmed=*released!=0; result.control.error.assign(data.begin()+static_cast<std::ptrdiff_t>(p),data.end()); return result;
}
std::vector<std::uint8_t> encode_mouse_mode(const SessionMouseMode& value) {
    auto out=encode_session(value.session); out.push_back(value.mode==MouseMode::absolute?0:1); return out;
}
std::optional<SessionMouseMode> decode_mouse_mode(std::span<const std::uint8_t> data) {
    if(data.size()!=9||data[8]>1)return{};
    return SessionMouseMode{*decode_session(data.first(8)),data[8]==0?MouseMode::absolute:MouseMode::relative};
}
std::optional<Packet> receive_packet(const tcp::Socket& socket,std::chrono::milliseconds timeout) {
    const auto end=std::chrono::steady_clock::now()+timeout;
    auto header=socket.receive_exact(kHeaderSize,timeout); if(!header)return{};
    if(!std::equal(kMagic.begin(),kMagic.end(),header->begin()))return{};
    std::size_t p=4; auto version=read_be<std::uint16_t>(*header,p); auto type=byte(*header,p),reserved=byte(*header,p);
    auto size=read_be<std::uint32_t>(*header,p);
    if(*version!=kProtocolVersion||*reserved!=0||!valid_packet_size(static_cast<PacketType>(*type),*size))return{};
    auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(end-std::chrono::steady_clock::now());
    if(remaining.count()<=0)return{};
    auto payload=socket.receive_exact(*size,remaining); if(!payload)return{};
    return Packet{static_cast<PacketType>(*type),std::move(*payload)};
}
tcp::SendResult send_packet(const tcp::Socket& socket,PacketType type,std::span<const std::uint8_t> payload,std::chrono::milliseconds timeout) {
    auto bytes=encode_packet(type,payload);
    if(bytes.empty())return {};
    return socket.send_all(bytes,timeout);
}
}  // namespace kvmux::relay

#include "network/relay_protocol.hpp"
#include <cassert>
#include <vector>
using namespace kvmux;
int main() {
    ControlEvent edge{7, 9, std::chrono::steady_clock::now(), KeyEdge{0x04, true}};
    auto decoded = relay::decode_control(relay::encode_control(edge)); assert(decoded && decoded->epoch == 7 && std::get<KeyEdge>(decoded->payload).pressed);
    ControlEvent pointer{7, 10, std::chrono::steady_clock::now(), AbsoluteMotion{.25, .75}};
    decoded = relay::decode_control(relay::encode_control(pointer)); assert(decoded && std::get<AbsoluteMotion>(decoded->payload).y == .75);
    const auto wire = relay::encode_packet(relay::PacketType::control, relay::encode_control(edge)); auto packet=relay::decode_packet(wire); assert(packet && packet->type==relay::PacketType::control);
    std::vector<std::uint8_t> jpeg{0xff,0xd8,0xff,0xd9}; auto sample=CaptureSample::make_mjpeg(2,5,std::chrono::steady_clock::now(),16,16,jpeg); assert(sample); sample->color_matrix=ColorMatrix::bt601; auto restored=relay::decode_mjpeg(relay::encode_mjpeg(*sample),2); assert(restored && restored->sequence==5 && restored->mjpeg->payload_size==4);
    auto heartbeat=relay::decode_heartbeat(relay::encode_heartbeat({12,7,true,true,5}));
    assert(heartbeat&&heartbeat->session==12&&heartbeat->epoch==7&&heartbeat->video_sequence==5);
    auto session_event=relay::decode_session_control(relay::encode_session_control({12,edge}));
    assert(session_event&&session_event->event.sequence==9&&session_event->session==12);
    auto mode=relay::decode_mouse_mode(relay::encode_mouse_mode({12,MouseMode::relative}));
    assert(mode&&mode->mode==MouseMode::relative);
    auto bad_heartbeat=relay::encode_heartbeat({12,7,true,true,5});bad_heartbeat[16]=2;
    assert(!relay::decode_heartbeat(bad_heartbeat));
    relay::Status status;status.session=12;status.control.epoch=7;
    status.control.error=std::string(1000,'x');
    auto state=relay::decode_status(relay::encode_status(status));
    assert(state&&state->control.error.size()==512&&state->control.epoch==7);
    auto invalid=wire; invalid[0]='X'; assert(!relay::decode_packet(invalid));

    for (const auto codec : {VideoCodec::mjpeg, VideoCodec::hevc}) {
        const auto hello = relay::decode_hello(relay::encode_hello({12, codec}));
        assert(hello && hello->session == 12 && hello->codec == codec);
    }
    assert(!relay::decode_hello(relay::encode_session(12))); // v1 is not accepted
    auto bad_hello = relay::encode_hello({12, VideoCodec::hevc});
    bad_hello[8] = 255; assert(!relay::decode_hello(bad_hello));
    assert(relay::encode_hello({0, VideoCodec::hevc}).empty());
    auto request = relay::decode_keyframe_request(relay::encode_keyframe_request({12, 91}));
    assert(request && request->session == 12 && request->generation == 91);
    assert(!relay::decode_keyframe_request(relay::encode_session(12)));
    EncodedAccessUnit au;
    au.bytes = {0,0,0,1,0x26,1,0xaa,0,0,1,0x02,1,0xbb};
    au.width = 1920; au.height = 1080; au.pts_ns = -123456789;
    au.generation = 91; au.encoded_sequence = 3; au.capture_sequence = 27; au.idr = true;
    au.sample_aspect_ratio = {4,3}; au.color_range = AVCOL_RANGE_MPEG;
    au.color_space = AVCOL_SPC_BT709; au.color_primaries = AVCOL_PRI_BT709;
    au.color_transfer = AVCOL_TRC_BT709;
    const auto hevc = relay::encode_hevc(au);
    assert(hevc.size() == 58 + au.bytes.size());
    const auto decoded_au = relay::decode_hevc(hevc);
    assert(decoded_au && decoded_au->bytes == au.bytes && decoded_au->idr);
    assert(decoded_au->encoded_sequence == 3 && decoded_au->capture_sequence == 27 && decoded_au->generation == 91);
    assert(decoded_au->pts_ns == au.pts_ns && decoded_au->width == 1920 && decoded_au->height == 1080);
    assert(decoded_au->sample_aspect_ratio.num == 4 && decoded_au->sample_aspect_ratio.den == 3);
    assert(decoded_au->color_range == au.color_range && decoded_au->color_space == au.color_space &&
           decoded_au->color_primaries == au.color_primaries && decoded_au->color_transfer == au.color_transfer);
    auto hevc_packet = relay::encode_packet(relay::PacketType::video_hevc, hevc);
    assert(relay::decode_packet(hevc_packet));
    for (std::size_t n = 0; n < 58; ++n) assert(!relay::decode_hevc(std::span(hevc).first(n)));
    for (const auto offset : {32U,36U,40U,44U}) {
        auto bad = hevc; std::fill_n(bad.begin() + offset, 4, 0);
        assert(!relay::decode_hevc(bad)); // zero dimensions or SAR
    }
    for (const auto offset : {32U,36U,40U,44U,48U,49U,50U,51U,52U,53U,54U}) {
        auto bad = hevc; bad[offset] = 255; assert(!relay::decode_hevc(bad));
    }
    for (const auto offset : {49U,50U,51U}) {
        auto bad = hevc; bad[offset] = 3; assert(!relay::decode_hevc(bad)); // reserved color codes
    }
    auto bad = hevc; bad[58] = 1; assert(!relay::decode_hevc(bad)); // not Annex B
    bad = hevc; bad.pop_back(); assert(!relay::decode_hevc(bad));
    bad = hevc; bad.push_back(0); assert(!relay::decode_hevc(bad));
    au.idr = false; au.encoded_sequence = 4; au.capture_sequence = 55;
    assert(!relay::decode_hevc(relay::encode_hevc(au))->idr);
    au.bytes.resize(kMaxCompressedSampleBytes, 0);
    assert(relay::decode_hevc(relay::encode_hevc(au)));
    au.bytes.push_back(0); assert(relay::encode_hevc(au).empty());
    auto oversize = hevc; oversize.resize(59 + kMaxCompressedSampleBytes);
    assert(!relay::decode_hevc(oversize));
    assert(relay::encode_packet(static_cast<relay::PacketType>(255), hevc).empty());
    assert(relay::encode_packet(relay::PacketType::hello, hevc).empty());
    hevc_packet[5] = 1; assert(!relay::decode_packet(hevc_packet));
    hevc_packet[5] = 2; hevc_packet[7] = 1; assert(!relay::decode_packet(hevc_packet));
    auto mjpeg_bytes = relay::encode_mjpeg(*sample);
    mjpeg_bytes[8] = 255; assert(!relay::decode_mjpeg(mjpeg_bytes, 2));

}

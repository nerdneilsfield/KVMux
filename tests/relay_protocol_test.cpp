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
}

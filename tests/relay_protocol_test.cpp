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
    auto invalid=wire; invalid[0]='X'; assert(!relay::decode_packet(invalid));
}

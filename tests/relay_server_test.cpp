#include "network/relay_server.hpp"
#include "network/relay_protocol.hpp"
#include "control/ch9329_protocol.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace kvmux;
using namespace kvmux::relay;
using namespace std::chrono_literals;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
#include "relay_test_fakes.hpp"
template<class P> bool eventually(P predicate,std::chrono::milliseconds timeout=1500ms) {
    auto end=std::chrono::steady_clock::now()+timeout;
    while(std::chrono::steady_clock::now()<end){if(predicate())return true;std::this_thread::sleep_for(2ms);}return false;
}
int main(int argc,char** argv) {
    require(argc==2,"jpeg fixture required");
    FakeCapture capture;std::ifstream file(argv[1],std::ios::binary);
    capture.jpeg.assign(std::istreambuf_iterator<char>(file),{});
    FakeSerial serial;Ch9329ControlSink sink(serial.io());sink.connect("fake",57600);
    require(eventually([&]{return sink.snapshot().state==ControlConnectionState::ready;}),"serial ready");
    RelayServer server(capture,sink);std::string error;
    require(server.start({"127.0.0.1",0,0},error),"listen");
    for(int scenario=0;scenario<5;++scenario) {
        capture.stalled=false;
        auto control=tcp::connect("127.0.0.1",server.control_port(),1s,error);require(control.has_value(),"connect control");
        auto hello=receive_packet(*control,1s);require(hello&&hello->type==PacketType::hello,"session hello");
        const auto id=decode_session(hello->payload);require(id.has_value(),"session id");
        if(scenario==0) {
            auto stranger=tcp::connect("127.0.0.1",server.video_port(),1s,error);
            require(stranger.has_value(),"unrelated video connects");
            require(send_packet(*stranger,PacketType::hello,encode_session(*id+1)),"wrong pairing hello");
            require(!receive_packet(*stranger,150ms),"wrong session video rejected");
        }
        auto video=tcp::connect("127.0.0.1",server.video_port(),1s,error);require(video.has_value(),"video connect");
        require(send_packet(*video,PacketType::hello,encode_session(*id)),"pair video");
        auto jpeg=receive_packet(*video,1s);require(jpeg&&jpeg->type==PacketType::video_mjpeg,"receive JPEG");
        auto sample=decode_mjpeg(jpeg->payload,1);require(sample&&sample->mjpeg->payload_size==capture.jpeg.size(),"complete JPEG");
        std::atomic<bool> drain{true};
        std::thread reader([&]{while(drain&&receive_packet(*video,1s)){};});
        auto status=receive_packet(*control,1s);require(status&&status->type==PacketType::status,"initial status");
        auto state=decode_status(status->payload);require(state.has_value(),"decode status");
        auto beat=[&] {
            require(send_packet(*control,PacketType::heartbeat,encode_heartbeat({*id,state->control.epoch,true,true,sample->sequence})),"heartbeat");
            auto response=receive_packet(*control,1s);require(response&&response->type==PacketType::status,"status response");
            state=decode_status(response->payload);require(state.has_value(),"status decode");
        };
        while(state->control.state!=ControlConnectionState::ready){std::this_thread::sleep_for(10ms);beat();}
        beat();
        if(scenario==0) {
            auto second=tcp::connect("127.0.0.1",server.control_port(),1s,error);
            require(second.has_value(),"second TCP connection");
            beat();
            require(!receive_packet(*second,100ms),"second controller gets no session");
        }
        const auto old_epoch=state->control.epoch;
        auto send_key=[&](std::uint64_t epoch,std::uint64_t seq,std::uint8_t key) {
            require(send_packet(*control,PacketType::control,encode_session_control({*id,{epoch,seq,std::chrono::steady_clock::now(),KeyEdge{key,true}}})),"send key");
            auto response=receive_packet(*control,1s);require(response.has_value(),"input status");
        };
        send_key(old_epoch,1,4);
        require(eventually([&]{std::lock_guard lock(serial.mutex);return std::ranges::any_of(serial.received,[](auto& f){return f.command==2&&f.data.size()==8&&f.data[2]==4;});}),"network input reaches HID");
        require(send_packet(*control,PacketType::release,encode_session(*id)),"release");
        auto response=receive_packet(*control,1s);require(response.has_value(),"release status");state=decode_status(response->payload);
        require(state&&state->control.epoch!=old_epoch,"release invalidates epoch");
        do {std::this_thread::sleep_for(20ms);beat();}
        while(state->control.state!=ControlConnectionState::ready||!state->control.release_confirmed);
        beat();
        send_key(old_epoch,2,5);
        std::this_thread::sleep_for(25ms);
        {std::lock_guard lock(serial.mutex);require(std::ranges::none_of(serial.received,[](auto& f){return f.command==2&&f.data.size()==8&&f.data[2]==5;}),"old epoch never emits HID");}
        send_key(state->control.epoch,3,6);
        require(eventually([&]{std::lock_guard lock(serial.mutex);return std::ranges::any_of(serial.received,[](auto& f){return f.command==2&&f.data.size()==8&&f.data[2]==6;});}),"new epoch reaches HID");
        const auto held_epoch=state->control.epoch;
        std::size_t before;{std::lock_guard lock(serial.mutex);before=serial.received.size();}
        if(scenario==0)control->close();
        // scenario 1 stops GUI heartbeats while video continues; scenario 2 loses video.
        if(scenario==2){drain=false;reader.join();video->close();}
        if(scenario==3) {
            capture.stalled=true;
            // Keep sending GUI heartbeats: source staleness, not GUI loss, ends this session.
            const auto until=std::chrono::steady_clock::now()+900ms;
            while(std::chrono::steady_clock::now()<until) {
                if(!send_packet(*control,PacketType::heartbeat,encode_heartbeat({*id,state->control.epoch,true,true,sample->sequence})))break;
                if(!receive_packet(*control,350ms))break;
                std::this_thread::sleep_for(40ms);
            }
        }
        if(scenario==4) {
            // Fresh capture and GUI heartbeat cannot renew control using an
            // already-consumed video sequence, including during congestion.
            const auto until=std::chrono::steady_clock::now()+700ms;
            while(std::chrono::steady_clock::now()<until) {
                beat();
                std::this_thread::sleep_for(40ms);
            }
            require(state->control.epoch!=held_epoch,"stale consumed sequence invalidates control epoch");
        }
        require(eventually([&]{std::lock_guard lock(serial.mutex);return std::any_of(serial.received.begin()+static_cast<std::ptrdiff_t>(before),serial.received.end(),[](auto& f){return f.command==2&&f.data.size()==8&&std::ranges::all_of(f.data,[](auto v){return v==0;});});}),"lease loss sends serial ReleaseAll");
        drain=false;if(reader.joinable())reader.join();
        control->close();video->close();
        {std::lock_guard lock(serial.mutex);serial.received.clear();}
    }
    // A frame larger than the loopback buffers makes progress, then blocks.
    // Its deadline must end the session: skipping the remainder corrupts framing.
    capture.stalled=false;
    auto control=tcp::connect("127.0.0.1",server.control_port(),1s,error);
    require(control.has_value(),"partial-send control connect");
    auto hello=receive_packet(*control,1s);
    require(hello&&hello->type==PacketType::hello,"partial-send hello");
    auto id=decode_session(hello->payload);require(id.has_value(),"partial-send id");
    // The previous session has joined, and the new video worker waits for pairing.
    capture.jpeg.resize(kMaxCompressedSampleBytes,7);
    auto video=tcp::connect("127.0.0.1",server.video_port(),1s,error);
    require(video.has_value(),"partial-send video connect");
    require(send_packet(*video,PacketType::hello,encode_session(*id)),"partial-send pairing");
    bool closed=false;
    const auto deadline=std::chrono::steady_clock::now()+1s;
    while(std::chrono::steady_clock::now()<deadline) {
        auto response=receive_packet(*control,350ms);
        if(!response){closed=true;break;}
        auto state=decode_status(response->payload);require(state.has_value(),"partial-send status");
        if(!send_packet(*control,PacketType::heartbeat,encode_heartbeat({*id,state->control.epoch,true,true,0}))) {
            closed=true;break;
        }
        std::this_thread::sleep_for(20ms);
    }
    require(closed,"partial video send closes session despite GUI heartbeats");
    const auto header=video->receive_exact(12,1s);
    require(header.has_value(),"partial video send made header progress");
    require(!video->receive_exact(kMaxCompressedSampleBytes+24,1s),"partial video packet is not continued");
    server.stop();sink.disconnect();
}

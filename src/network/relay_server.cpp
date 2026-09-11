#include "network/relay_server.hpp"
#include "network/relay_protocol.hpp"
#include <atomic>
#include <spdlog/spdlog.h>
#include "input/input_router.hpp"
#include <cmath>
#include <thread>
namespace kvmux::relay {
namespace {
using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;
bool valid_event(const ControlEvent& e) {
    return std::visit([](const auto& p) {
        using T=std::decay_t<decltype(p)>;
        const auto unit=[](double v){return std::isfinite(v)&&v>=0&&v<=1;};
        const auto delta=[](double v){return std::isfinite(v)&&std::abs(v)<=32767;};
        if constexpr(std::is_same_v<T,KeyEdge>) return supported_usb_keyboard_usage(p.usage);
        else if constexpr(std::is_same_v<T,RelativeMotion>) return delta(p.dx)&&delta(p.dy);
        else if constexpr(std::is_same_v<T,AbsoluteMotion>) return unit(p.x)&&unit(p.y);
        else if constexpr(std::is_same_v<T,ButtonEdge>) return p.button<3&&unit(p.x)&&unit(p.y);
        else return delta(p.steps)&&unit(p.x)&&unit(p.y);
    },e.payload);
}
}
struct RelayServer::Impl {
    CaptureSource& capture; Ch9329ControlSink& sink;
    std::optional<tcp::Listener> control_listener,video_listener;
    std::atomic<bool> stopping{false}; std::thread worker;
    std::uint64_t next_session{};
    Impl(CaptureSource& c,Ch9329ControlSink& s):capture(c),sink(s){}
    void session(tcp::Socket control) {
        const auto id=++next_session;
        spdlog::debug("Relay server: control connected, session={}",id);
        sink.set_control_active(false); sink.release_all();
        if(!send_packet(control,PacketType::hello,encode_session(id))){spdlog::debug("Relay server session={}: handshake send failed or timed out",id);return;}
        std::atomic<bool> done{false},paired{false}; std::atomic<std::uint64_t> sent_sequence{0};
        std::thread video([&] {
            std::optional<tcp::Socket> socket;
            const auto pairing_deadline=Clock::now()+2s;
            while(!done&&!stopping&&Clock::now()<pairing_deadline) {
                auto candidate=video_listener->accept(20ms); if(!candidate)continue;
                auto hello=receive_packet(*candidate,100ms);
                if(hello&&hello->type==PacketType::hello&&decode_session(hello->payload)==id) {
                    socket=std::move(candidate);break;
                }
            }
            if(!socket){spdlog::debug("Relay server session={}: video pairing ended: {}",id,stopping ? "server stopping" : done ? "control channel ended" : "pairing timeout (2s)");done=true;sink.release_all();return;}
            spdlog::debug("Relay server session={}: video channel paired",id);
            paired=true;
            auto last=Clock::now();
            auto drop_log=last;
            std::size_t dropped{};
            std::optional<std::uint64_t> generation;
            while(!done&&!stopping) {
                // Never transfer the occupied video channel to another peer.
                if(auto extra=video_listener->accept(1ms))extra->close();
                auto sample=capture.take_latest_sample();
                if(sample) {
                    if(!sample->mjpeg||(generation&&*generation!=sample->generation)||Clock::now()-sample->arrival>500ms){spdlog::debug("Relay server session={}: video ended: {}",id,!sample->mjpeg ? "sample is not MJPEG" : (generation&&*generation!=sample->generation) ? "capture generation changed" : "sample older than 500ms");done=true;break;}
                    generation=sample->generation;
                    last=sample->arrival; // Capture freshness is independent of send progress.
                    auto bytes=encode_mjpeg(*sample);
                    const auto send_started=Clock::now();
                    const auto sent=bytes.empty() ? tcp::SendResult{} : send_packet(*socket,PacketType::video_mjpeg,bytes,100ms);
                    if(sent.unsent_deadline()) {
                        ++dropped;
                        if(Clock::now()-drop_log>=1s) {
                            spdlog::debug("Relay server session={}: dropped {} unsent video frames on 100ms deadline, latest_sequence={}",id,dropped,sample->sequence);
                            dropped=0;drop_log=Clock::now();
                        }
                        continue; // No header bytes were sent; fetch the newest capture sample.
                    }
                    if(!sent){
                        spdlog::debug("Relay server session={}: video ended: {}, socket={}, sequence={}, payload_bytes={}, elapsed_us={}",
                            id,bytes.empty() ? "MJPEG encoding failed" : "video send failed or timed out (100ms)",
                            socket->native_handle(),sample->sequence,bytes.size(),
                            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-send_started).count());
                        done=true;break;
                    }
                    sent_sequence=sample->sequence;
                } else {
                    if(Clock::now()-last>500ms){spdlog::debug("Relay server session={}: no video sample for 500ms",id);done=true;break;}
                    std::this_thread::sleep_for(2ms);
                }
            }
            done=true;sink.release_all();
        });
        auto heartbeat=Clock::now();
        std::uint64_t sequence{}, consumed_sequence{}; bool active=false;
        auto consumed_at=Clock::now();
        while(!done&&!stopping) {
            if(auto extra=control_listener->accept(1ms))extra->close();
            if(!send_packet(control,PacketType::status,encode_status({id,sink.snapshot()}))){spdlog::debug("Relay server session={}: status send failed or timed out",id);break;}
            const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(heartbeat+250ms-Clock::now());
            if(left<=0ms){spdlog::debug("Relay server session={}: heartbeat expired (250ms)",id);break;}
            auto packet=receive_packet(control,left);
            if(!packet||done||stopping){spdlog::debug("Relay server session={}: control loop ended: {}",id,stopping ? "server stopping" : done ? "video channel ended" : "control receive failed, invalid packet, or heartbeat timeout");break;}
            if(packet->type==PacketType::heartbeat) {
                auto value=decode_heartbeat(packet->payload); if(!value||value->session!=id){spdlog::debug("Relay server session={}: invalid heartbeat or session mismatch",id);break;}
                heartbeat=Clock::now();
                if(value->video_sequence>consumed_sequence) {
                    consumed_sequence=value->video_sequence;consumed_at=Clock::now();
                }
                const auto snapshot=sink.snapshot();
                const bool allow=value->gui_active&&value->video_fresh&&paired&&value->video_sequence>0&&
                    value->video_sequence<=sent_sequence&&Clock::now()-consumed_at<=500ms&&value->epoch==snapshot.epoch&&
                    snapshot.state==ControlConnectionState::ready&&snapshot.target_usb_ready&&snapshot.release_confirmed;
                sink.update_ui_heartbeat();
                if(active!=allow)spdlog::debug("Relay server session={}: control authorization {}",id,allow ? "enabled" : "disabled by heartbeat safety checks");
                if(active&&!allow)sink.release_all();
                active=allow;sink.set_control_active(active);
            } else if(packet->type==PacketType::release) {
                if(decode_session(packet->payload)!=id){spdlog::debug("Relay server session={}: invalid release session",id);break;}
                active=false;sink.set_control_active(false);sink.release_all();
            } else if(packet->type==PacketType::mouse_mode) {
                auto value=decode_mouse_mode(packet->payload);if(!value||value->session!=id){spdlog::debug("Relay server session={}: invalid mouse mode or session mismatch",id);break;}
                active=false;sink.set_control_active(false);sink.release_all();sink.set_mouse_mode(value->mode);
            } else if(packet->type==PacketType::control) {
                auto value=decode_session_control(packet->payload);
                if(!value||value->session!=id||!valid_event(value->event)){spdlog::debug("Relay server session={}: invalid control event or session mismatch",id);break;}
                if(active&&value->event.epoch==sink.snapshot().epoch&&value->event.sequence>sequence) {
                    sequence=value->event.sequence;
                    auto result=sink.submit(value->event);
                    if(result==SubmitResult::overloaded){spdlog::debug("Relay server session={}: control queue overloaded",id);break;}
                }
            } else {spdlog::debug("Relay server session={}: unexpected control packet type",id);break;}
        }
        done=true;sink.set_control_active(false);sink.release_all();control.close();
        video.join();
        spdlog::debug("Relay server session={}: disconnected",id);
    }
    void run() {
        while(!stopping) {
            if(auto socket=control_listener->accept(20ms))session(std::move(*socket));
        }
    }
};
RelayServer::RelayServer(CaptureSource& c,Ch9329ControlSink& s):impl_(std::make_unique<Impl>(c,s)){}
RelayServer::~RelayServer(){stop();}
bool RelayServer::start(const ServerOptions& options,std::string& error) {
    if(impl_->worker.joinable()){error="Relay already running";return false;}
    if(impl_->capture.snapshot().actual_mode.delivered_format!=PixelFormat::mjpeg) {
        error="LAN relay requires native MJPEG; raw-only capture modes are unsupported";return false;
    }
    impl_->control_listener=tcp::Listener::bind(options.bind_address,options.control_port,error);
    if(!impl_->control_listener){spdlog::debug("Relay server: control bind failed: {}",error);return false;}
    impl_->video_listener=tcp::Listener::bind(options.bind_address,options.video_port,error);
    if(!impl_->video_listener){spdlog::debug("Relay server: video bind failed: {}",error);impl_->control_listener.reset();return false;}
    spdlog::debug("Relay server: listening on {}, control port={}, video port={}",options.bind_address,control_port(),video_port());
    impl_->stopping=false;impl_->worker=std::thread([this]{impl_->run();});return true;
}
void RelayServer::stop() noexcept {
    if(impl_->worker.joinable())spdlog::debug("Relay server: stop requested");
    impl_->stopping=true;
    impl_->sink.set_control_active(false);impl_->sink.release_all();
    if(impl_->worker.joinable())impl_->worker.join();
    impl_->control_listener.reset();impl_->video_listener.reset();
}
std::uint16_t RelayServer::control_port() const{return impl_->control_listener->local_port().value_or(0);}
std::uint16_t RelayServer::video_port() const{return impl_->video_listener->local_port().value_or(0);}
}

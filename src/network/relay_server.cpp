#include "network/relay_server.hpp"
#include "network/relay_protocol.hpp"
#include "network/media_codec_wire.hpp"
#include "network/relay_selection.hpp"
#include "video/video_processor.hpp"
extern "C" {
#include <libswscale/swscale.h>
}
#include <stdexcept>
#include <atomic>
#include <spdlog/spdlog.h>
#include "input/input_router.hpp"
#include <cmath>
#include <thread>
namespace kvmux::relay {
namespace {
using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;
const char* backend_name(CodecBackend backend) {
    switch(backend) {
    case CodecBackend::automatic:return "auto";
    case CodecBackend::jetson_gstreamer:return "jetson";
    case CodecBackend::videotoolbox:return "videotoolbox";
    case CodecBackend::ffmpeg_software:return "software";
    }
    return "unknown";
}
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
    ServerOptions options;
    EncoderFactory encoder_factory;
    Impl(CaptureSource& c,Ch9329ControlSink& s,EncoderFactory factory):capture(c),sink(s),encoder_factory(std::move(factory)){}
    CodecConfig config(std::uint64_t generation) const {
        const auto mode=capture.snapshot().actual_mode;
        CodecConfig result;
        result.width=mode.width;result.height=mode.height;
        result.fps_numerator=static_cast<std::uint32_t>(mode.frame_rate.numerator);
        result.fps_denominator=static_cast<std::uint32_t>(mode.frame_rate.denominator);
        result.bitrate=options.bitrate;result.generation=generation;
        return result;
    }
    std::unique_ptr<VideoEncoder> encoder(std::uint64_t generation,std::string& error) {
        auto result=encoder_factory(options.encoder_backend,error);
        if(result) {
            const auto settings=config(generation);
            auto configured=result->configure(settings);
            if(!configured.ok()){error=configured.message;result->shutdown();return {};}
            const auto diagnostic=result->diagnostic();
            spdlog::info("Relay HEVC encoder configured: backend={}, hardware_active={}, hardware_verified={}, size={}x{}, bitrate={} bits/s, detail={}",
                backend_name(diagnostic.backend),diagnostic.hardware_active,diagnostic.hardware_verified,
                settings.width,settings.height,settings.bitrate,diagnostic.detail);
        }
        return result;
    }
    void hevc_video(tcp::Socket& socket,std::uint64_t id,std::atomic<bool>& done,
                    std::atomic<std::uint64_t>& sent_sequence,std::atomic<bool>& keyframe) {
        std::string error;
        auto codec=encoder(id,error);
        if(!codec)throw std::runtime_error("HEVC encoder: "+error);
        struct Shutdown { VideoEncoder& codec; ~Shutdown(){codec.shutdown();} } shutdown{*codec};
        VideoProcessor processor;
        std::unique_ptr<SwsContext,decltype(&sws_freeContext)> scaler(nullptr,sws_freeContext);
        auto last=Clock::now();
        std::optional<std::uint64_t> capture_generation;
        std::uint64_t encoded_sequence{};
        bool waiting_idr=true,reported_output=false;
        while(!done&&!stopping) {
            if(auto extra=video_listener->accept(1ms))extra->close();
            if(keyframe.exchange(false)) {
                auto result=codec->request_keyframe();
                if(!result.ok())throw std::runtime_error(result.message);
            }
            // Drain ordered output before admitting another latest raw sample.
            for(unsigned i=0;i<16;++i) {
                EncodedAccessUnit unit;
                const auto result=codec->poll(unit);
                if(result.status==CodecStatus::again)break;
                if(!result.ok())throw std::runtime_error(result.message);
                if(!reported_output) {
                    const auto diagnostic=codec->diagnostic();
                    spdlog::info("Relay HEVC session={} first encoded output: backend={}, hardware_active={}, hardware_verified={}, detail={}",
                        id,backend_name(diagnostic.backend),diagnostic.hardware_active,diagnostic.hardware_verified,diagnostic.detail);
                    reported_output=true;
                }
                unit.encoded_sequence=++encoded_sequence;
                if(unit.generation!=id)throw std::runtime_error("HEVC encoder generation mismatch");
                if(Clock::now()-unit.arrival>500ms)throw std::runtime_error("HEVC encoded output stale for 500ms");
                if(waiting_idr&&!unit.idr)continue;
                const auto bytes=encode_hevc(unit);
                if(bytes.empty())throw std::runtime_error("Invalid HEVC access unit");
                const auto sent=send_packet(socket,PacketType::video_hevc,bytes,100ms);
                if(sent.unsent_deadline()) {
                    waiting_idr=true;
                    const auto request=codec->request_keyframe();
                    if(!request.ok())throw std::runtime_error(request.message);
                    continue;
                }
                if(!sent)throw std::runtime_error("HEVC partial send or transport failure");
                waiting_idr=false;sent_sequence=unit.capture_sequence;
            }
            auto sample=capture.take_latest_sample();
            if(sample) {
                if(!sample->raw||sample->mjpeg||Clock::now()-sample->arrival>500ms||
                   (capture_generation&&*capture_generation!=sample->generation))
                    throw std::runtime_error("HEVC capture changed, is compressed, or is stale");
                capture_generation=sample->generation;last=sample->arrival;
                const auto mode=config(id);
                if(sample->width!=mode.width||sample->height!=mode.height)
                    throw std::runtime_error("HEVC capture dimensions changed");
                auto frame=processor.process(*sample);
                if(!frame)throw std::runtime_error(processor.last_error());
                auto input=frame->frame;
                if(input->format!=AV_PIX_FMT_NV12&&input->format!=AV_PIX_FMT_YUV420P) {
                    AvFramePtr converted(av_frame_alloc(),[](AVFrame* f){av_frame_free(&f);});
                    if(!converted)throw std::runtime_error("HEVC frame allocation failed");
                    converted->format=AV_PIX_FMT_NV12;converted->width=input->width;converted->height=input->height;
                    if(av_frame_get_buffer(converted.get(),32)<0||av_frame_copy_props(converted.get(),input.get())<0)
                        throw std::runtime_error("HEVC frame storage allocation failed");
                    scaler.reset(sws_getCachedContext(scaler.release(),input->width,input->height,
                        static_cast<AVPixelFormat>(input->format),input->width,input->height,AV_PIX_FMT_NV12,
                        SWS_FAST_BILINEAR,nullptr,nullptr,nullptr));
                    const int matrix=input->colorspace==AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
                    const bool rgb=input->format==AV_PIX_FMT_BGRA||input->format==AV_PIX_FMT_RGBA;
                    const int full=rgb||input->color_range==AVCOL_RANGE_JPEG ? 1 : 0;
                    converted->color_range=full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
                    converted->colorspace=matrix==SWS_CS_ITU709 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
                    if(!scaler||sws_setColorspaceDetails(scaler.get(),sws_getCoefficients(matrix),full,
                        sws_getCoefficients(matrix),full,0,1<<16,1<<16)<0||
                        sws_scale(scaler.get(),input->data,input->linesize,0,input->height,
                        converted->data,converted->linesize)!=input->height)
                        throw std::runtime_error("HEVC raw conversion failed");
                    input=std::move(converted);
                }
                EncoderInput value;
                value.frame=std::move(input);value.generation=id;value.capture_sequence=sample->sequence;
                value.arrival=sample->arrival;
                value.pts_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(sample->arrival.time_since_epoch()).count();
                const auto result=codec->submit(value);
                // again did not accept this raw frame. The next capture replaces it.
                if(!result.ok()&&result.status!=CodecStatus::again)throw std::runtime_error(result.message);
            }
            if(Clock::now()-last>500ms)throw std::runtime_error("HEVC capture stale for 500ms");
            if(!sample)std::this_thread::sleep_for(2ms);
        }
    }
    void session(tcp::Socket control) {
        const auto id=++next_session;
        spdlog::debug("Relay server: control connected, session={}",id);
        sink.set_control_active(false); sink.release_all();
        if(!send_packet(control,PacketType::hello,encode_hello({id,options.codec}))){spdlog::debug("Relay server session={}: handshake send failed or timed out",id);return;}
        std::atomic<bool> done{false},paired{false},keyframe{false}; std::atomic<std::uint64_t> sent_sequence{0};
        std::thread video([&] {
            std::optional<tcp::Socket> socket;
            const auto pairing_deadline=Clock::now()+2s;
            while(!done&&!stopping&&Clock::now()<pairing_deadline) {
                auto candidate=video_listener->accept(20ms); if(!candidate)continue;
                auto hello=receive_packet(*candidate,100ms);
                auto value=hello&&hello->type==PacketType::hello ? decode_hello(hello->payload) : std::nullopt;
                if(value&&value->session==id&&value->codec==options.codec) {
                    socket=std::move(candidate);break;
                }
            }
            if(!socket){spdlog::debug("Relay server session={}: video pairing ended: {}",id,stopping ? "server stopping" : done ? "control channel ended" : "pairing timeout (2s)");done=true;sink.release_all();return;}
            spdlog::debug("Relay server session={}: video channel paired",id);
            paired=true;
            if(options.codec==VideoCodec::hevc) {
                try { hevc_video(*socket,id,done,sent_sequence,keyframe); }
                catch(const std::exception& error){spdlog::error("Relay HEVC session={}: {}",id,error.what());}
                done=true;sink.release_all();return;
            }
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
            } else if(packet->type==PacketType::keyframe_request) {
                const auto request=decode_keyframe_request(packet->payload);
                if(!request||request->session!=id||options.codec!=VideoCodec::hevc)break;
                if(request->generation==id)keyframe=true;
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
RelayServer::RelayServer(CaptureSource& c,Ch9329ControlSink& s,EncoderFactory factory):impl_(std::make_unique<Impl>(c,s,std::move(factory))){}
RelayServer::~RelayServer(){stop();}
bool RelayServer::start(const ServerOptions& options,std::string& error) {
    if(impl_->worker.joinable()){error="Relay already running";return false;}
    error.clear();
    impl_->options=options;
    const auto mode=impl_->capture.snapshot().actual_mode;
    try { select_mode(std::span<const CaptureMode>(&mode,1),0,options.codec); }
    catch(const std::exception& e){error=e.what();return false;}
    if(options.codec==VideoCodec::hevc) {
        if(!options.bitrate||options.bitrate>100'000'000){error="HEVC bitrate must be 1..100000000 bits/s";return false;}
        // Lifecycle probing stays off the caller thread and fails before listening.
        std::thread probe([&]{
            try { auto codec=impl_->encoder(1,error);if(codec)codec->shutdown();
                  else if(error.empty())error="HEVC hardware encoder unavailable"; }
            catch(const std::exception& e){error=e.what();}
        });
        probe.join();if(!error.empty())return false;
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

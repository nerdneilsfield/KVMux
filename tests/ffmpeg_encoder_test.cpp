#include "video/codec/ffmpeg_encoder.hpp"
#include "video/codec/ffmpeg_decoder.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
}
namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void check(const kvmux::CodecResult& result) { require(result.ok(),result.message); }
kvmux::EncoderInput input(int n, bool nv12=false) {
    kvmux::EncoderInput value;
    value.frame={av_frame_alloc(),[](AVFrame* p){av_frame_free(&p);}};
    auto* f=value.frame.get(); require(f,"allocate input");
    f->width=64; f->height=64; f->format=nv12?AV_PIX_FMT_NV12:AV_PIX_FMT_YUV420P;
    require(av_frame_get_buffer(f,32)>=0,"allocate input planes");
    for (int y=0;y<64;++y) std::memset(f->data[0]+y*f->linesize[0],32+n,64);
    for (int y=0;y<32;++y) {
        std::memset(f->data[1]+y*f->linesize[1],128,nv12?64:32);
        if (!nv12) std::memset(f->data[2]+y*f->linesize[2],128,32);
    }
    f->color_range=AVCOL_RANGE_MPEG; f->colorspace=AVCOL_SPC_BT709;
    f->sample_aspect_ratio={4,3};
    value.pts_ns=1000+n*16'666'667LL; value.generation=7; value.capture_sequence=100+n;
    value.arrival=std::chrono::steady_clock::now(); return value;
}
}
void run_case(kvmux::VideoCodec codec, kvmux::EncodingPriority priority) {
    using namespace kvmux;
    std::string error;
    auto encoder=create_ffmpeg_encoder(CodecBackend::ffmpeg_software,error);
    const char* name=codec==VideoCodec::h264 ? "libx264" : "libx265";
    if (!avcodec_find_encoder_by_name(name)) {
        require(!encoder || encoder->configure(CodecConfig{codec,priority,64,64,60,1,8'000'000,30,7}).status==CodecStatus::unsupported,
            "unavailable requested encoder fails clearly");
        std::cout<<"SKIP: "<<name<<" unavailable\n"; return;
    }
    require(bool(encoder),error);
    CodecConfig config; config.codec=codec; config.priority=priority; config.width=64; config.height=64; config.generation=7; config.keyframe_interval=30;
    auto invalid=config; invalid.width=63;
    require(encoder->configure(invalid).status==CodecStatus::invalid_input,"odd size rejected");
    check(encoder->configure(config));
    auto bad=input(0); bad.generation=8;
    require(encoder->submit(bad).status==CodecStatus::invalid_input,"generation rejected");
    bad=input(0); bad.frame->linesize[0]=1;
    require(encoder->submit(bad).status==CodecStatus::invalid_input,"short stride rejected");
    std::vector<EncodedAccessUnit> units;
    auto receive=[&]() {
        for (int i=0;i<20;++i) {
            EncodedAccessUnit au; auto result=encoder->poll(au);
            if (result.status==CodecStatus::again || result.status==CodecStatus::end_of_stream) return result.status;
            check(result); units.push_back(std::move(au));
        }
        throw std::runtime_error("bounded receive progress");
    };
    bool backpressure=false;
    for (int n=0;n<12;++n) {
        if (n==5) check(encoder->request_keyframe());
        auto value=input(n,n%2);
        bool accepted=false;
        for (int retry=0;retry<4;++retry) {
            auto result=encoder->submit(value);
            if (result.status==CodecStatus::again) { backpressure=true; receive(); continue; }
            check(result); accepted=true; break;
        }
        require(accepted,"send retry progress");
        require(encoder->submit(value).status==CodecStatus::invalid_input,"duplicate PTS rejected");
    }
    bool finished=false;
    for (int retry=0;retry<4;++retry) {
        auto result=encoder->finish();
        if (result.status==CodecStatus::again) { receive(); continue; }
        check(result); finished=true; break;
    }
    require(finished && receive()==CodecStatus::end_of_stream,"drain EOS");
    require(backpressure && units.size()==12,"bounded send retries retain all inputs");
    require(units[0].idr && units[5].idr,"initial and forced IDR");
    for (std::size_t i=0;i<units.size();++i) {
        require(units[i].capture_sequence==100+i && units[i].generation==7 &&
            units[i].pts_ns==1000+static_cast<std::int64_t>(i)*16'666'667 &&
            units[i].encoded_sequence==i+1,"encoder metadata matches input");
    }
    require(!encoder->diagnostic().hardware_active && encoder->diagnostic().hardware_verified,
        "verified CPU diagnostic");
    auto decoder=create_ffmpeg_decoder(CodecBackend::ffmpeg_software,error); require(bool(decoder),error);
    check(decoder->configure(config));
    std::size_t decoded=0; AvFramePtr retained;
    auto decode_receive=[&]() {
        for (int i=0;i<20;++i) {
            VideoFrame frame; auto result=decoder->poll(frame);
            if (result.status==CodecStatus::again || result.status==CodecStatus::end_of_stream) return result.status;
            check(result);
            require(frame.sequence==100+decoded && frame.device_timestamp==units.at(decoded).pts_ns,
                "roundtrip metadata");
            require(frame.frame->pict_type!=AV_PICTURE_TYPE_B && !frame.frame->hw_frames_ctx,"CPU decode without B frames");
            require(frame.color_range==ColorRange::limited && frame.color_matrix==ColorMatrix::bt709 &&
                frame.sample_aspect_ratio_numerator==4 && frame.sample_aspect_ratio_denominator==3,"roundtrip color/SAR");
            retained=frame.frame; ++decoded;
        }
        throw std::runtime_error("bounded decode progress");
    };
    for (const auto& au:units) { check(decoder->submit(au)); decode_receive(); }
    check(decoder->finish());
    require(decode_receive()==CodecStatus::end_of_stream && decoded==12,"roundtrip complete");
    require(encoder->submit(input(20)).status==CodecStatus::failed,"submit after EOS rejected");
    check(encoder->reset()); check(encoder->submit(input(20))); check(encoder->reset());
    check(encoder->submit(input(21,true))); check(encoder->finish()); receive();
    require(units.size()==13 && units.back().idr && units.back().encoded_sequence==13 &&
        units.back().capture_sequence==121,"reset drops pending, preserves output sequence and starts IDR");
    // A forced IDR must be independently decodable after discarding references.
    check(decoder->reset()); check(decoder->submit(units[5]));
    VideoFrame recovered;
    std::size_t recovered_count=0;
    auto recover_receive=[&]() {
        for (;;) {
            const auto result=decoder->poll(recovered);
            if (result.status==CodecStatus::again || result.status==CodecStatus::end_of_stream) return result.status;
            check(result);
            require(recovered.sequence==105,"forced IDR includes VPS/SPS/PPS");
            ++recovered_count;
        }
    };
    for (int retry=0;;++retry) {
        require(retry<3,"recovery finish makes progress");
        const auto result=decoder->finish();
        if (result.status==CodecStatus::again) { recover_receive(); continue; }
        check(result); break;
    }
    require(recover_receive()==CodecStatus::end_of_stream && recovered_count==1,"forced IDR drains once");
    encoder->shutdown(); decoder->shutdown();
    require(retained && retained->data[0] && !units[0].bytes.empty(),"owned outputs survive shutdown");
    require(encoder->diagnostic().detail.find(priority==EncodingPriority::quality ? "priority=quality" : "priority=size")!=std::string::npos,
        "priority is observable in diagnostic");
    std::cout<<name<<" CPU encode/decode priority="<<(priority==EncodingPriority::quality ? "quality" : "size")
        <<": 12 synthetic 64x64 frames, EAGAIN, forced IDR, reset, EOS passed; no throughput claim\n";
}
int main() {
    using namespace kvmux;
    std::string error;
    require(!create_ffmpeg_encoder(CodecBackend::automatic,error),"helper rejects Auto");
    for (auto codec : {VideoCodec::h264, VideoCodec::hevc})
        for (auto priority : {EncodingPriority::quality, EncodingPriority::size}) run_case(codec,priority);
}

#include "video/codec/jetson_encoder.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace kvmux;
static void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
static bool nal(const EncodedAccessUnit& au, unsigned type) {
    for (std::size_t i=0; i+4<au.bytes.size(); ++i)
        if (!au.bytes[i] && !au.bytes[i+1] && au.bytes[i+2]==1 && ((au.bytes[i+3]>>1)&63)==type) return true;
    return false;
}
int main(int argc, char** argv) try {
    std::string error;
    auto encoder = create_jetson_encoder(error);
    require(bool(encoder), error);
    CodecConfig config{1920,1080,60000,1001,8'000'000,600,7};
    auto invalid_rate = config;
    invalid_rate.fps_numerator = 241001;
    invalid_rate.fps_denominator = 1000;
    auto result = encoder->configure(invalid_rate);
    require(result.status == CodecStatus::invalid_input, "accepted frame rate above 240fps");
    result=encoder->configure(config); require(result.ok(), result.message);
    require(!encoder->diagnostic().hardware_active,"hardware claimed before output");
    AVFrame* raw=av_frame_alloc(); require(raw, "alloc frame");
    AvFramePtr frame(raw, [](AVFrame* p){av_frame_free(&p);});
    frame->width=1920; frame->height=1080; frame->format=AV_PIX_FMT_NV12;
    frame->color_range=AVCOL_RANGE_MPEG; frame->colorspace=AVCOL_SPC_BT709;
    require(av_frame_get_buffer(frame.get(),32)>=0,"frame buffer");
    std::ofstream output(argc>1 ? argv[1] : "/tmp/kvmux-native-encoder.h265",std::ios::binary);
    unsigned submitted=0,received=0,idrs=0; bool requested=false,finished=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(25);
    while (std::chrono::steady_clock::now()<deadline) {
        // Empty the pipeline before the dynamic request, proving it affects new input.
        if (submitted==30 && received==30 && !requested) {
            result=encoder->request_keyframe(); require(result.ok(),result.message); requested=true;
        }
        if (submitted<60 && (submitted<30 || requested)) {
            for(int y=0;y<1080;++y) for(int x=0;x<1920;++x) frame->data[0][y*frame->linesize[0]+x]=static_cast<unsigned char>(16+(x+y+submitted)%200);
            for(int y=0;y<540;++y) std::fill_n(frame->data[1]+y*frame->linesize[1],1920,128);
            EncoderInput input{frame,1'000'000'000LL+static_cast<std::int64_t>(submitted)*16'666'667,submitted*2,7,{}};
            result=encoder->submit(input);
            require(result.ok() || result.status==CodecStatus::again,result.message);
            if(result.ok()) ++submitted;
        }
        if(submitted==60 && !finished) {result=encoder->finish();require(result.ok(),result.message);finished=true;}
        EncodedAccessUnit au;
        result=encoder->poll(au);
        if(result.status==CodecStatus::end_of_stream) break;
        require(result.ok() || result.status==CodecStatus::again,result.message);
        if(result.ok()) {
            require(au.encoded_sequence==received+1,"encoded sequence mismatch");
            require(au.capture_sequence==received*2 && au.generation==7,"metadata sequence/generation");
            require(au.pts_ns==1'000'000'000LL+static_cast<std::int64_t>(received)*16'666'667,"PTS mismatch");
            require(au.color_range==AVCOL_RANGE_MPEG && au.color_space==AVCOL_SPC_BT709,"color metadata");
            if(au.idr) {++idrs;require(nal(au,32)&&nal(au,33)&&nal(au,34),"IDR lacks VPS/SPS/PPS");}
            if(received==30) require(au.idr,"dynamic force-IDR did not apply");
            output.write(reinterpret_cast<const char*>(au.bytes.data()),static_cast<std::streamsize>(au.bytes.size()));
            ++received;
        } else std::this_thread::yield();
    }
    require(submitted==60 && received==60 && idrs>=2,"60-frame drain/IDR test failed");
    require(encoder->diagnostic().hardware_active && encoder->diagnostic().hardware_verified,"hardware output diagnostic");
    result=encoder->reset();require(result.ok(),result.message);
    EncoderInput input{frame,0,1000,7,{}};
    result=encoder->submit(input);require(result.ok(),result.message);
    result=encoder->finish();require(result.ok(),result.message);
    bool reset_idr=false;
    while(std::chrono::steady_clock::now()<deadline) {
        EncodedAccessUnit au;result=encoder->poll(au);
        if(result.ok()){require(au.idr && au.encoded_sequence==61 && au.capture_sequence==1000 && au.pts_ns==0,"reset leaked old chain");reset_idr=true;}
        else if(result.status==CodecStatus::end_of_stream)break;
        else require(result.status==CodecStatus::again,result.message);
    }
    require(reset_idr,"reset output missing");
    std::cout<<"frames="<<received<<" IDRs="<<idrs<<" pts=matched sequence=matched force_IDR=passed reset=passed backend=jetson_gstreamer\n";
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}

#include "video/codec/ffmpeg_decoder.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void check(const kvmux::CodecResult& result) { require(result.ok(), result.message); }
std::vector<kvmux::EncodedAccessUnit> read_units(const char* path) {
    using namespace kvmux;
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "open HEVC test fixture");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    const auto size=bytes.size();
    bytes.resize(size+AV_INPUT_BUFFER_PADDING_SIZE);
    auto* parser=av_parser_init(AV_CODEC_ID_HEVC);
    auto* context=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_HEVC));
    require(parser && context, "HEVC parser allocation");
    std::vector<EncodedAccessUnit> units;
    auto parse=[&](const std::uint8_t* data, int count) {
        std::uint8_t* packet=nullptr; int packet_size=0;
        const int used=av_parser_parse2(parser, context, &packet, &packet_size, data, count,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        require(used>=0 && (used || packet_size || !count), "HEVC parse progress");
        if (packet_size) {
            EncodedAccessUnit au;
            au.bytes.assign(packet, packet+packet_size);
            au.width=1920; au.height=1080; au.generation=7;
            au.capture_sequence=100+units.size(); au.pts_ns=static_cast<std::int64_t>(units.size())*16'666'667;
            au.idr=parser->key_frame==1;
            au.color_range=AVCOL_RANGE_MPEG; au.color_space=AVCOL_SPC_BT709;
            au.sample_aspect_ratio={4,3};
            units.push_back(std::move(au));
        }
        return used;
    };
    std::size_t offset=0;
    while (offset<size) offset+=parse(bytes.data()+offset, static_cast<int>(size-offset));
    parse(nullptr,0);
    av_parser_close(parser); avcodec_free_context(&context);
    return units;
}
}

int main(int argc, char** argv) {
    using namespace kvmux;
    std::string error;
    require(!create_ffmpeg_decoder(CodecBackend::automatic,error), "backend helper rejects automatic");
    auto decoder=create_ffmpeg_decoder(CodecBackend::ffmpeg_software,error);
    require(bool(decoder),error);
    CodecConfig config; config.width=1920; config.height=1080; config.generation=7;
    check(decoder->configure(config));
    VideoFrame frame;
    check(decoder->finish());
    check(decoder->finish());
    require(decoder->poll(frame).status==CodecStatus::end_of_stream,"empty configured decoder drains");
    check(decoder->reset());
    check(decoder->finish());
    require(decoder->poll(frame).status==CodecStatus::end_of_stream,"empty reset decoder drains");
    check(decoder->reset());
    EncodedAccessUnit invalid; invalid.width=1920; invalid.height=1080; invalid.generation=7;
    invalid.bytes={0,0,1,2,1,0x80};
    require(decoder->submit(invalid).status==CodecStatus::invalid_input,"initial non-IDR rejected");
    invalid.idr=true; invalid.bytes={0,0,1,38,1,0x80};
    require(decoder->submit(invalid).status==CodecStatus::invalid_input,"IDR without parameter sets rejected");
    invalid.bytes={1,2,3};
    require(decoder->submit(invalid).status==CodecStatus::invalid_input,"malformed Annex B rejected");
    check(decoder->reset());
    decoder->shutdown();
    require(decoder->poll(frame).status==CodecStatus::failed,"poll after shutdown rejected");
    if (argc<2) { std::cout<<"decoder lifecycle checks passed; native stream not supplied\n"; return 0; }
    auto units=read_units(argv[1]);
    require(units.size()==60 && units[0].idr && units[30].idr,"fixture has 60 AUs and two IDRs");
    const auto selected=argc>2 && std::string(argv[2])=="videotoolbox" ? CodecBackend::videotoolbox : CodecBackend::ffmpeg_software;
    decoder=create_ffmpeg_decoder(selected,error); require(bool(decoder),error);
    check(decoder->configure(config));
    check(decoder->submit(units[0]));
    check(decoder->reset()); // Discard pending work explicitly, never leak its metadata.
    require(decoder->submit(units[1]).status==CodecStatus::invalid_input,"in-flight reset requires IDR");
    std::size_t count=0;
    AvFramePtr retained;
    auto receive=[&]() {
        for (;;) {
            const auto result=decoder->poll(frame);
            if (result.status==CodecStatus::again || result.status==CodecStatus::end_of_stream) return result.status;
            check(result);
            const auto& au=units.at(count);
            require(frame.sequence==au.capture_sequence && frame.generation==au.generation &&
                    frame.device_timestamp==au.pts_ns && frame.frame->pts==au.pts_ns,"ordered metadata survives decode");
            require(!frame.frame->hw_frames_ctx && frame.frame->data[0],"owned CPU pixels");
            require(frame.color_range==ColorRange::limited && frame.color_matrix==ColorMatrix::bt709 &&
                    frame.sample_aspect_ratio_numerator==4 && frame.sample_aspect_ratio_denominator==3,"color/SAR metadata");
            require(frame.frame->pict_type!=AV_PICTURE_TYPE_B,"low delay stream has no B frames");
            retained=frame.frame; ++count;
        }
    };
    auto submit=[&](const EncodedAccessUnit& au) {
        for (int retry=0; retry<3; ++retry) {
            const auto result=decoder->submit(au);
            if (result.status==CodecStatus::again) { receive(); continue; }
            check(result); return;
        }
        throw std::runtime_error("decoder made no submit progress");
    };
    // Force send-side EAGAIN and retry without dropping a compressed AU.
    for (const auto& au:units) submit(au);
    for (int retry=0;;++retry) {
        require(retry<3,"finish makes progress");
        const auto result=decoder->finish();
        if (result.status==CodecStatus::again) { receive(); continue; }
        check(result); break;
    }
    require(receive()==CodecStatus::end_of_stream && count==60,"all 60 frames drained");
    std::cout<<decoder->diagnostic().detail<<" frames="<<count<<'\n';
    require(decoder->submit(units[0]).status==CodecStatus::failed,"no submit after finish");
    check(decoder->reset());
    require(decoder->submit(units[1]).status==CodecStatus::invalid_input,"reset requires fresh IDR");
    count=30;
    for (std::size_t i=30;i<units.size();++i) { submit(units[i]); receive(); }
    check(decoder->finish());
    require(receive()==CodecStatus::end_of_stream && count==60,"IDR reset decodes final 30 frames");
    decoder->shutdown();
    require(retained && retained->data[0],"output survives decoder shutdown");
    volatile auto luma=retained->data[0][0];
    (void)luma;
    std::cout<<"reset_IDR_frames=30 ownership=retained\n";
}

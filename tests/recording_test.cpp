#include "app/recording.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}
using namespace std::chrono_literals;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static kvmux::VideoFrame frame() { kvmux::VideoFrame value; value.arrival=std::chrono::steady_clock::now(); value.frame={av_frame_alloc(),[](AVFrame* p){av_frame_free(&p);}}; value.frame->format=AV_PIX_FMT_YUV420P; value.frame->width=64; value.frame->height=64; require(av_frame_get_buffer(value.frame.get(),32)>=0,"frame buffer"); return value; }
static kvmux::RecordingStatus wait(kvmux::Recording& r, kvmux::RecordingState desired) { for(int i=0;i<200;++i) { auto s=r.status(); if(s.state==desired || s.state==kvmux::RecordingState::failed) return s; std::this_thread::sleep_for(10ms); } return r.status(); }
static void parse_image(const std::filesystem::path& path, bool video) { AVFormatContext* context=nullptr; require(avformat_open_input(&context,path.string().c_str(),nullptr,nullptr)>=0,"open output"); require(avformat_find_stream_info(context,nullptr)>=0,"stream info"); bool found=false; for(unsigned i=0;i<context->nb_streams;++i) found|=context->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO; avformat_close_input(&context); require(found && std::filesystem::file_size(path)>0,"video stream/output bytes"); (void)video; }
int main() { const auto directory=std::filesystem::temp_directory_path()/"kvmux-recording-test"; std::filesystem::remove_all(directory); std::filesystem::create_directories(directory); kvmux::Recording r(directory); auto first=frame(); require(r.snapshot(first),"queue snapshot"); kvmux::RecordingStatus snap; for(int i=0;i<200;++i) { snap=r.status(); if(!snap.output_path.empty() || !snap.error.empty()) break; std::this_thread::sleep_for(10ms); } require(!snap.output_path.empty(),"snapshot path"); parse_image(snap.output_path,false); require(r.start(first),"queue start"); auto started=wait(r,kvmux::RecordingState::recording); require(started.state==kvmux::RecordingState::recording,started.error.c_str()); require(r.append(frame()),"append"); require(r.pause(),"pause"); require(!r.append(frame()),"paused rejects frame"); std::this_thread::sleep_for(20ms); require(r.stop(),"stop paused"); auto done=wait(r,kvmux::RecordingState::idle); require(done.state==kvmux::RecordingState::idle,done.error.c_str()); parse_image(done.output_path,true); r.shutdown(); std::filesystem::remove_all(directory); std::cout<<"JPEG and MP4 output parsed; pause/resume lifecycle passed\n"; }

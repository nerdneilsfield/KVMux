#include "network/relay_selection.hpp"
#include <cassert>
#include <stdexcept>
using namespace kvmux;
using namespace kvmux::relay;
template<class F> void fails(F call, const std::string& text) {
    try { call(); assert(false); }
    catch (const std::runtime_error& error) { assert(std::string(error.what()).find(text) != std::string::npos); }
}
CaptureMode mode(unsigned w, unsigned h, Rational fps) {
    return {"camera", w, h, fps, PixelFormat::mjpeg, PixelFormat::mjpeg, "MJPG"};
}
int main() {
    DeviceInfo a; a.stable_id="camera-a"; a.display_name="A";
    DeviceInfo b; b.stable_id="camera-b"; b.display_name="B";
    std::vector<DeviceInfo> devices{a};
    assert(select_device(devices)=="camera-a");
    fails([]{select_device({});}, "No capture");
    devices.push_back(b);
    fails([&]{select_device(devices);}, "camera-b (B)");
    assert(select_device(devices,"camera-b")=="camera-b");
    fails([&]{select_device(devices,"missing");}, "not found");
    std::vector<CaptureMode> modes{
        mode(1920,1200,{90,1}), mode(1280,720,{30000,1001}),
        mode(1920,1080,{30000,1001}), mode(1280,720,{60000,1001}),
        mode(1920,1080,{60000,1001})};
    assert(select_mode(modes)==4);
    assert(modes[select_mode(modes)].frame_rate == (Rational{60000,1001}));
    modes.pop_back(); assert(select_mode(modes)==3);
    modes.pop_back(); assert(select_mode(modes)==2);
    modes.pop_back(); assert(select_mode(modes)==1);
    modes.pop_back(); assert(select_mode(modes)==0);
    modes.push_back(mode(1920,1200,{100,1})); assert(select_mode(modes)==1);
    modes.push_back(mode(640,480,{200,1})); assert(select_mode(modes)==1);
    assert(select_mode(modes,2)==2);
    modes.push_back(mode(1920,1080,{5994,100})); assert(select_mode(modes)==3);
    modes.back().delivered_format=PixelFormat::bgra; assert(select_mode(modes)==1);
    fails([&]{select_mode(modes,3);}, "MJPEG");
    modes.back().delivered_format=PixelFormat::mjpeg;
    modes.back().device_format=PixelFormat::yuy2; assert(select_mode(modes)==1);
    fails([&]{select_mode(modes,3);}, "MJPEG");
    fails([&]{select_mode(modes,50);}, "out of range");
    std::vector<CaptureMode> raw{modes.back()};
    fails([&]{select_mode(raw);}, "No usable");
    fails([]{select_mode({});}, "No usable");
    raw.front().delivered_format=PixelFormat::yuy2;
    assert(select_mode(raw,{},VideoCodec::hevc)==0);
    fails([&]{select_mode(modes,0,VideoCodec::hevc);}, "raw");
    raw.front().width=1279;
    fails([&]{select_mode(raw,0,VideoCodec::hevc);}, "even dimensions");
    std::vector<SerialPortInfo> ports{
        {"/dev/ttyTHS0","board",{},{}}, {"/dev/ttyUSB0","other",0x0403,0x6001},
        {"/dev/ttyUSB1","unknown WCH",0x1a86,0xffff},
        {"/dev/ttyCH341USB0","CH340",0x1a86,0x7523}};
    assert(select_serial(ports)=="/dev/ttyCH341USB0");
    assert(select_serial(ports,"explicit")=="explicit");
    ports.pop_back(); fails([&]{select_serial(ports);}, "specify --serial");
    ports.push_back({"COM3","CH343",0x1a86,0x55d3});
    assert(select_serial(ports)=="COM3");
    ports.push_back({"COM4","CH341",0x1a86,0x5523});
    fails([&]{select_serial(ports);}, "COM4 (CH341)");
    fails([]{select_serial({});}, "No known");
}

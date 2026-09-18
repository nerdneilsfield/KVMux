#include "network/media_codec_wire.hpp"

#include <algorithm>
#include <cassert>
#include <vector>
using namespace kvmux;
int main() {
  std::vector<std::uint8_t> jpeg{0xff, 0xd8, 0xff, 0xd9};
  auto sample = CaptureSample::make_mjpeg(
      2, 5, std::chrono::steady_clock::now(), 16, 16, jpeg);
  assert(sample);
  sample->color_matrix = ColorMatrix::bt601;
  auto restored = relay::decode_mjpeg(relay::encode_mjpeg(*sample), 2);
  assert(restored && restored->sequence == 5 &&
         restored->mjpeg->payload_size == 4);
  EncodedAccessUnit au;
  au.bytes = {0, 0, 0, 1, 0x26, 1, 0xaa, 0, 0, 1, 0x02, 1, 0xbb};
  au.width = 1920;
  au.height = 1080;
  au.pts_ns = -123456789;
  au.generation = 91;
  au.encoded_sequence = 3;
  au.capture_sequence = 27;
  au.idr = true;
  au.sample_aspect_ratio = {4, 3};
  au.color_range = AVCOL_RANGE_MPEG;
  au.color_space = AVCOL_SPC_BT709;
  au.color_primaries = AVCOL_PRI_BT709;
  au.color_transfer = AVCOL_TRC_BT709;
  const auto hevc = relay::encode_annex_b(au);
  assert(hevc.size() == 58 + au.bytes.size());
  const auto decoded_au = relay::decode_annex_b(hevc, VideoCodec::hevc);
  assert(decoded_au && decoded_au->bytes == au.bytes && decoded_au->idr);
  assert(decoded_au->encoded_sequence == 3 &&
         decoded_au->capture_sequence == 27 && decoded_au->generation == 91);
  assert(decoded_au->pts_ns == au.pts_ns && decoded_au->width == 1920 &&
         decoded_au->height == 1080);
  assert(decoded_au->sample_aspect_ratio.num == 4 &&
         decoded_au->sample_aspect_ratio.den == 3);
  assert(decoded_au->color_range == au.color_range &&
         decoded_au->color_space == au.color_space &&
         decoded_au->color_primaries == au.color_primaries &&
         decoded_au->color_transfer == au.color_transfer);
  for (std::size_t n = 0; n < 58; ++n)
    assert(!relay::decode_annex_b(std::span(hevc).first(n), VideoCodec::hevc));
  for (const auto offset : {32U, 36U, 40U, 44U}) {
    auto bad = hevc;
    std::fill_n(bad.begin() + offset, 4, 0);
    assert(!relay::decode_annex_b(bad,
                                  VideoCodec::hevc));  // zero dimensions or SAR
  }
  for (const auto offset :
       {32U, 36U, 40U, 44U, 48U, 49U, 50U, 51U, 52U, 53U, 54U}) {
    auto bad = hevc;
    bad[offset] = 255;
    assert(!relay::decode_annex_b(bad, VideoCodec::hevc));
  }
  for (const auto offset : {49U, 50U, 51U}) {
    auto bad = hevc;
    bad[offset] = 3;
    assert(
        !relay::decode_annex_b(bad, VideoCodec::hevc));  // reserved color codes
  }
  au.codec = VideoCodec::h264;
  const auto h264 = relay::encode_annex_b(au);
  const auto decoded_h264 = relay::decode_annex_b(h264, VideoCodec::h264);
  assert(decoded_h264 && decoded_h264->codec == VideoCodec::h264 &&
         decoded_h264->bytes == au.bytes);
  assert(!relay::decode_annex_b(h264, VideoCodec::mjpeg));
  au.codec = VideoCodec::hevc;
  auto bad = hevc;
  bad[58] = 1;
  assert(!relay::decode_annex_b(bad, VideoCodec::hevc));  // not Annex B
  bad = hevc;
  bad.pop_back();
  assert(!relay::decode_annex_b(bad, VideoCodec::hevc));
  bad = hevc;
  bad.push_back(0);
  assert(!relay::decode_annex_b(bad, VideoCodec::hevc));
  au.idr = false;
  au.encoded_sequence = 4;
  au.capture_sequence = 55;
  assert(!relay::decode_annex_b(relay::encode_annex_b(au), au.codec)->idr);
  au.bytes.resize(kMaxCompressedSampleBytes, 0);
  assert(relay::decode_annex_b(relay::encode_annex_b(au), au.codec));
  au.bytes.push_back(0);
  assert(relay::encode_annex_b(au).empty());
  auto oversize = hevc;
  oversize.resize(59 + kMaxCompressedSampleBytes);
  assert(!relay::decode_annex_b(oversize, VideoCodec::hevc));
  auto mjpeg_bytes = relay::encode_mjpeg(*sample);
  mjpeg_bytes[8] = 255;
  assert(!relay::decode_mjpeg(mjpeg_bytes, 2));
}

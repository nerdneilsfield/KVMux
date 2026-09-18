#include "video/capture/capture_source.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kvmux {

struct AvfState;

static std::string to_utf8(NSString* value) {
  if (value == nil) return {};
  const char* text = value.UTF8String;
  return text == nullptr ? std::string{} : std::string{text};
}

static Rational frame_rate_from_duration(CMTime duration) {
  if (!CMTIME_IS_NUMERIC(duration) || duration.value <= 0 || duration.timescale <= 0) return {};
  std::int64_t numerator = duration.timescale;
  std::int64_t denominator = duration.value;
  const std::int64_t divisor = std::gcd(numerator, denominator);
  numerator /= divisor;
  denominator /= divisor;
  while (numerator > std::numeric_limits<std::int32_t>::max() ||
         denominator > std::numeric_limits<std::int32_t>::max()) {
    numerator = (numerator + 1) / 2;
    denominator = (denominator + 1) / 2;
  }
  return {static_cast<std::int32_t>(numerator), static_cast<std::int32_t>(denominator)};
}

static std::string fourcc_name(OSType code) {
  char text[5] = {static_cast<char>((code >> 24) & 0xff), static_cast<char>((code >> 16) & 0xff),
                  static_cast<char>((code >> 8) & 0xff), static_cast<char>(code & 0xff), 0};
  for (int i = 0; i != 4; ++i)
    if (text[i] < 0x20 || text[i] > 0x7e) text[i] = '?';
  return std::string{text, 4};
}

static PixelFormat pixel_format(OSType code) {
  switch (code) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
      return PixelFormat::nv12;
    case kCVPixelFormatType_422YpCbCr8:
      return PixelFormat::uyvy;
    case kCVPixelFormatType_32BGRA:
      return PixelFormat::bgra;
    case kCMVideoCodecType_JPEG:
      return PixelFormat::mjpeg;
    default:
      return PixelFormat::unknown;
  }
}

struct AvfState {
  mutable std::mutex mutex;
  CaptureSnapshot snapshot;
  std::optional<CaptureSample> latest;
  std::uint64_t next_sequence{};
  std::atomic<std::uint64_t> generation{0};
  std::atomic<bool> accepting{false};
  AVCaptureSession* session = nil;
  AVCaptureVideoDataOutput* output = nil;
  id delegate = nil;
};

}  // namespace kvmux

using namespace kvmux;

@interface KVMuxVideoDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
 @private
  std::weak_ptr<kvmux::AvfState> _state;
  std::uint64_t _generation;
}
- (instancetype)initWithState:(const std::shared_ptr<kvmux::AvfState>&)state
                   generation:(std::uint64_t)generation;
- (void)silence;
@end

static ColorMatrix color_matrix(CVImageBufferRef image) {
  CFTypeRef value = CVBufferCopyAttachment(image, kCVImageBufferYCbCrMatrixKey, nullptr);
  const ColorMatrix result = value == kCVImageBufferYCbCrMatrix_ITU_R_709_2 ? ColorMatrix::bt709
                             : value == kCVImageBufferYCbCrMatrix_ITU_R_601_4
                                 ? ColorMatrix::bt601
                                 : ColorMatrix::unknown;
  if (value != nullptr) CFRelease(value);
  return result;
}

@implementation KVMuxVideoDelegate
- (instancetype)initWithState:(const std::shared_ptr<kvmux::AvfState>&)state
                   generation:(std::uint64_t)generation {
  self = [super init];
  if (self != nil) {
    _state = state;
    _generation = generation;
  }
  return self;
}

- (void)silence {
  _state.reset();
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample_buffer
           fromConnection:(AVCaptureConnection*)connection {
  (void)output;
  (void)connection;
  @autoreleasepool {
    const auto state = _state.lock();
    if (!state || !state->accepting.load(std::memory_order_acquire) ||
        state->generation.load(std::memory_order_acquire) != _generation)
      return;

    CVPixelBufferRef image = CMSampleBufferGetImageBuffer(sample_buffer);
    if (image == nullptr ||
        CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
      return;

    std::optional<CaptureSample> result;
    do {
      const std::size_t width = CVPixelBufferGetWidth(image);
      const std::size_t height = CVPixelBufferGetHeight(image);
      if (width > std::numeric_limits<std::uint32_t>::max() ||
          height > std::numeric_limits<std::uint32_t>::max() ||
          !valid_dimensions(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)))
        break;

      const PixelFormat format = pixel_format(CVPixelBufferGetPixelFormatType(image));
      if (format == PixelFormat::unknown) break;

      std::vector<PlaneLayout> planes;
      std::vector<std::uint8_t> bytes;
      const bool planar = CVPixelBufferIsPlanar(image);
      const std::size_t count = planar ? CVPixelBufferGetPlaneCount(image) : 1;
      if ((format == PixelFormat::nv12 && count != 2) ||
          (format != PixelFormat::nv12 && count != 1))
        break;

      bool valid = true;
      for (std::size_t plane = 0; plane < count; ++plane) {
        void* base = planar ? CVPixelBufferGetBaseAddressOfPlane(image, plane)
                            : CVPixelBufferGetBaseAddress(image);
        const std::size_t rows = planar ? CVPixelBufferGetHeightOfPlane(image, plane) : height;
        const std::size_t stride = planar ? CVPixelBufferGetBytesPerRowOfPlane(image, plane)
                                          : CVPixelBufferGetBytesPerRow(image);
        std::size_t row_bytes = 0;
        if (format == PixelFormat::nv12)
          row_bytes = width;
        else if (format == PixelFormat::uyvy && width <= SIZE_MAX / 2)
          row_bytes = width * 2;
        else if (format == PixelFormat::bgra && width <= SIZE_MAX / 4)
          row_bytes = width * 4;
        if (base == nullptr || row_bytes == 0 || stride < row_bytes || rows == 0 ||
            stride > kMaxRawSampleBytes / rows ||
            bytes.size() > kMaxRawSampleBytes - stride * rows) {
          valid = false;
          break;
        }
        const std::size_t offset = bytes.size();
        bytes.resize(offset + stride * rows);
        std::memcpy(bytes.data() + offset, base, stride * rows);
        planes.push_back({offset, static_cast<std::ptrdiff_t>(stride), row_bytes, rows});
      }
      if (!valid) break;

      result = CaptureSample::make_raw(_generation, 0, std::chrono::steady_clock::now(),
                                       static_cast<std::uint32_t>(width),
                                       static_cast<std::uint32_t>(height), format, planes, bytes);
      if (!result) break;
      const CMTime timestamp = CMSampleBufferGetPresentationTimeStamp(sample_buffer);
      if (CMTIME_IS_NUMERIC(timestamp) && timestamp.timescale > 0) {
        result->device_timestamp = timestamp.value;
        result->device_time_base_numerator = 1;
        result->device_time_base_denominator = timestamp.timescale;
      }
      result->color_range =
          CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
              ? ColorRange::full
              : (format == PixelFormat::nv12 || format == PixelFormat::uyvy ? ColorRange::limited
                                                                            : ColorRange::unknown);
      result->color_matrix = color_matrix(image);
    } while (false);
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);

    if (!result || !state->accepting.load(std::memory_order_acquire) ||
        state->generation.load(std::memory_order_acquire) != _generation)
      return;
    std::optional<CaptureSample> replaced;
    {
      std::lock_guard lock(state->mutex);
      if (!state->accepting.load(std::memory_order_relaxed) ||
          state->generation.load(std::memory_order_relaxed) != _generation)
        return;
      result->sequence = ++state->next_sequence;
      replaced.swap(state->latest);
      state->latest.emplace(std::move(*result));
      ++state->snapshot.received_samples;
      if (replaced) ++state->snapshot.overwritten_samples;
    }
  }
}
@end

namespace kvmux {

static NSArray<AVCaptureDevice*>* video_devices() {
  AVCaptureDeviceDiscoverySession* discovery = [AVCaptureDeviceDiscoverySession
      discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeExternal ]
                            mediaType:AVMediaTypeVideo
                             position:AVCaptureDevicePositionUnspecified];
  return discovery.devices;
}

static AVCaptureDevice* find_device(const std::string& stable_id) {
  NSString* wanted = [NSString stringWithUTF8String:stable_id.c_str()];
  if (wanted == nil) return nil;
  for (AVCaptureDevice* device in video_devices()) {
    if ([device.uniqueID isEqualToString:wanted]) return device;
  }
  return nil;
}

static void set_failure(const std::shared_ptr<AvfState>& state, std::uint64_t generation,
                        CaptureState status, std::string error) {
  if (state->generation.load(std::memory_order_acquire) != generation) return;
  state->accepting.store(false, std::memory_order_release);
  std::lock_guard lock(state->mutex);
  state->snapshot.state = status;
  state->snapshot.error = std::move(error);
}

static void tear_down(const std::shared_ptr<AvfState>& state, dispatch_queue_t callback_queue) {
  state->accepting.store(false, std::memory_order_release);
  KVMuxVideoDelegate* delegate = (KVMuxVideoDelegate*)state->delegate;
  [state->output setSampleBufferDelegate:nil queue:nullptr];
  dispatch_sync(callback_queue, ^{
                });
  [delegate silence];
  [state->session stopRunning];
  state->delegate = nil;
  state->output = nil;
  state->session = nil;
}

static void configure(const std::shared_ptr<AvfState>& state, CaptureMode requested,
                      std::uint64_t generation, dispatch_queue_t callback_queue) {
  if (state->generation.load(std::memory_order_acquire) != generation) return;
  tear_down(state, callback_queue);
  AVCaptureDevice* device = find_device(requested.device_id);
  if (device == nil) {
    set_failure(state, generation, CaptureState::fault, "capture device not found");
    return;
  }

  if (requested.frame_rate.numerator <= 0 || requested.frame_rate.denominator <= 0) {
    set_failure(state, generation, CaptureState::fault, "invalid requested frame rate");
    return;
  }
  AVCaptureDeviceFormat* selected = nil;
  CMTime frame_duration = kCMTimeInvalid;
  for (AVCaptureDeviceFormat* format in device.formats) {
    CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    if (size.width <= 0 || size.height <= 0 ||
        static_cast<std::uint32_t>(size.width) != requested.width ||
        static_cast<std::uint32_t>(size.height) != requested.height)
      continue;
    const CMTime candidate =
        CMTimeMake(requested.frame_rate.denominator, requested.frame_rate.numerator);
    for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges) {
      if (CMTIME_IS_NUMERIC(candidate) && CMTimeCompare(candidate, range.minFrameDuration) >= 0 &&
          CMTimeCompare(candidate, range.maxFrameDuration) <= 0) {
        selected = format;
        frame_duration = candidate;
        break;
      }
    }
    if (selected != nil) break;
  }
  if (selected == nil) {
    set_failure(state, generation, CaptureState::fault, "requested capture mode is unavailable");
    return;
  }

  NSError* error = nil;
  AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
  if (input == nil) {
    set_failure(state, generation, CaptureState::fault, to_utf8(error.localizedDescription));
    return;
  }
  if (![device lockForConfiguration:&error]) {
    set_failure(state, generation, CaptureState::fault, to_utf8(error.localizedDescription));
    return;
  }
  device.activeFormat = selected;
  device.activeVideoMinFrameDuration = frame_duration;
  device.activeVideoMaxFrameDuration = frame_duration;
  [device unlockForConfiguration];

  AVCaptureSession* session = [[AVCaptureSession alloc] init];
  AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
  output.alwaysDiscardsLateVideoFrames = YES;
  if (![session canAddInput:input] || ![session canAddOutput:output]) {
    set_failure(state, generation, CaptureState::fault, "cannot attach capture input or output");
    return;
  }
  [session beginConfiguration];
  [session addInput:input];
  [session addOutput:output];

  OSType chosen = 0;
  NSArray<NSNumber*>* available = output.availableVideoCVPixelFormatTypes;
  const OSType preferences[] = {kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                                kCVPixelFormatType_422YpCbCr8, kCVPixelFormatType_32BGRA};
  for (OSType candidate : preferences) {
    if ([available containsObject:@(candidate)]) {
      chosen = candidate;
      break;
    }
  }
  if (chosen == 0) {
    [session commitConfiguration];
    set_failure(state, generation, CaptureState::fault, "no supported video output pixel format");
    return;
  }
  output.videoSettings = @{(NSString*)kCVPixelBufferPixelFormatTypeKey : @(chosen)};
  [session commitConfiguration];

  KVMuxVideoDelegate* delegate = [[KVMuxVideoDelegate alloc] initWithState:state
                                                                generation:generation];
  [output setSampleBufferDelegate:delegate queue:callback_queue];
  state->session = session;
  state->output = output;
  state->delegate = delegate;
  state->accepting.store(true, std::memory_order_release);
  [session startRunning];
  if (!session.running || state->generation.load(std::memory_order_acquire) != generation) {
    tear_down(state, callback_queue);
    set_failure(state, generation, CaptureState::fault, "capture session failed to start");
    return;
  }

  CaptureMode actual = requested;
  actual.frame_rate = frame_rate_from_duration(device.activeVideoMinFrameDuration);
  actual.device_format =
      pixel_format(CMFormatDescriptionGetMediaSubType(selected.formatDescription));
  actual.delivered_format = pixel_format(chosen);
  actual.device_format_name =
      fourcc_name(CMFormatDescriptionGetMediaSubType(selected.formatDescription));
  std::lock_guard lock(state->mutex);
  state->snapshot.actual_mode = std::move(actual);
  state->snapshot.state = CaptureState::streaming;
  state->snapshot.error.clear();
}

class AvfoundationCaptureSource final : public CaptureSource {
 public:
  AvfoundationCaptureSource()
      : state_(std::make_shared<AvfState>()),
        session_queue_(dispatch_queue_create("io.kvmux.capture.session", DISPATCH_QUEUE_SERIAL)),
        callback_queue_(dispatch_queue_create("io.kvmux.capture.callback", DISPATCH_QUEUE_SERIAL)) {
    dispatch_queue_set_specific(session_queue_, this, this, nullptr);
  }

  ~AvfoundationCaptureSource() override {
    const auto state = state_;
    const auto callback_queue = callback_queue_;
    const auto work = ^{
      tear_down(state, callback_queue);
    };
    if (dispatch_get_specific(this) == this)
      work();
    else
      dispatch_sync(session_queue_, work);
  }

  std::vector<DeviceInfo> enumerate_devices() override {
    std::vector<DeviceInfo> result;
    for (AVCaptureDevice* device in video_devices()) {
      result.push_back({CaptureBackend::avfoundation,
                        to_utf8(device.uniqueID),
                        to_utf8(device.localizedName),
                        std::nullopt,
                        std::nullopt,
                        {},
                        false});
    }
    return result;
  }

  std::vector<CaptureMode> enumerate_modes(const std::string& stable_id) override {
    std::vector<CaptureMode> result;
    AVCaptureDevice* device = find_device(stable_id);
    if (device == nil) return result;
    for (AVCaptureDeviceFormat* format in device.formats) {
      const CMVideoDimensions size =
          CMVideoFormatDescriptionGetDimensions(format.formatDescription);
      if (size.width <= 0 || size.height <= 0 ||
          !valid_dimensions(static_cast<std::uint32_t>(size.width),
                            static_cast<std::uint32_t>(size.height)))
        continue;
      const std::string name =
          fourcc_name(CMFormatDescriptionGetMediaSubType(format.formatDescription));
      for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges) {
        Rational fps = frame_rate_from_duration(range.minFrameDuration);
        if (fps.numerator <= 0) continue;
        const PixelFormat native =
            pixel_format(CMFormatDescriptionGetMediaSubType(format.formatDescription));
        CaptureMode mode{stable_id,
                         static_cast<std::uint32_t>(size.width),
                         static_cast<std::uint32_t>(size.height),
                         fps,
                         native,
                         PixelFormat::unknown,
                         name};
        if (std::find(result.begin(), result.end(), mode) == result.end())
          result.push_back(std::move(mode));
      }
    }
    return result;
  }

  void start(const CaptureMode& mode) override {
    const std::uint64_t generation = state_->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    state_->accepting.store(false, std::memory_order_release);
    {
      std::lock_guard lock(state_->mutex);
      state_->latest.reset();
      state_->next_sequence = 0;
      state_->snapshot = {};
      state_->snapshot.state = CaptureState::starting;
      state_->snapshot.generation = generation;
      state_->snapshot.actual_mode = mode;
    }
    const auto state = state_;
    const auto session_queue = session_queue_;
    const auto callback_queue = callback_queue_;
    const auto authorized = ^{
      configure(state, mode, generation, callback_queue);
    };
    const AVAuthorizationStatus authorization =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (authorization == AVAuthorizationStatusAuthorized) {
      dispatch_async(session_queue, authorized);
    } else if (authorization == AVAuthorizationStatusNotDetermined) {
      [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                               completionHandler:^(BOOL granted) {
                                 if (granted)
                                   dispatch_async(session_queue, authorized);
                                 else
                                   set_failure(state, generation, CaptureState::permission_denied,
                                               "camera access was denied");
                               }];
    } else if (authorization == AVAuthorizationStatusDenied ||
               authorization == AVAuthorizationStatusRestricted) {
      set_failure(state, generation, CaptureState::permission_denied,
                  "camera access is denied or restricted");
    } else {
      set_failure(state, generation, CaptureState::fault, "unknown camera authorization state");
    }
  }

  void stop() noexcept override {
    const std::uint64_t generation = state_->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    state_->accepting.store(false, std::memory_order_release);
    {
      std::lock_guard lock(state_->mutex);
      state_->snapshot.generation = generation;
      state_->snapshot.state = CaptureState::stopping;
      state_->latest.reset();
    }
    const auto state = state_;
    const auto callback_queue = callback_queue_;
    dispatch_async(session_queue_, ^{
      tear_down(state, callback_queue);
      std::lock_guard lock(state->mutex);
      if (state->generation.load(std::memory_order_relaxed) == generation) {
        state->snapshot.state = CaptureState::stopped;
        state->snapshot.error.clear();
      }
    });
  }

  std::optional<CaptureSample> take_latest_sample() override {
    std::optional<CaptureSample> result;
    std::lock_guard lock(state_->mutex);
    result.swap(state_->latest);
    return result;
  }

  CaptureSnapshot snapshot() const override {
    std::lock_guard lock(state_->mutex);
    return state_->snapshot;
  }

 private:
  std::shared_ptr<AvfState> state_;
  dispatch_queue_t session_queue_;
  dispatch_queue_t callback_queue_;
};

std::unique_ptr<CaptureSource> create_platform_capture_source() {
  return std::make_unique<AvfoundationCaptureSource>();
}

}  // namespace kvmux

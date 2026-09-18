#ifdef _WIN32

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "video/capture/capture_source.hpp"

namespace kvmux {
namespace {

using Microsoft::WRL::ComPtr;

std::string hresult_text(const char* operation, HRESULT hr) {
  char value[32]{};
  std::snprintf(value, sizeof(value), "0x%08lX",
                static_cast<unsigned long>(hr));
  return std::string(operation) + " failed (" + value + ")";
}

void check(HRESULT hr, const char* operation) {
  if (FAILED(hr)) {
    throw std::runtime_error(hresult_text(operation, hr));
  }
}

std::string utf8(const wchar_t* text, UINT32 length) {
  if (text == nullptr || length == 0) return {};
  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                        static_cast<int>(length), nullptr, 0,
                                        nullptr, nullptr);
  if (count <= 0) return {};
  std::string result(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                      static_cast<int>(length), result.data(), count, nullptr,
                      nullptr);
  return result;
}

struct FormatDescription {
  PixelFormat format{PixelFormat::unknown};
  const char* name{"unknown"};
};

FormatDescription describe_format(REFGUID subtype) {
  if (subtype == MFVideoFormat_YUY2) return {PixelFormat::yuy2, "YUY2"};
  if (subtype == MFVideoFormat_UYVY) return {PixelFormat::uyvy, "UYVY"};
  if (subtype == MFVideoFormat_NV12) return {PixelFormat::nv12, "NV12"};
  if (subtype == MFVideoFormat_ARGB32 || subtype == MFVideoFormat_RGB32)
    return {PixelFormat::bgra, "BGRA"};
  if (subtype == MFVideoFormat_MJPG) return {PixelFormat::mjpeg, "MJPEG"};
  return {};
}

bool media_type_mode(IMFMediaType* type, const std::string& device_id,
                     CaptureMode& mode) {
  GUID major{};
  GUID subtype{};
  UINT32 width = 0, height = 0, numerator = 0, denominator = 0;
  if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) ||
      major != MFMediaType_Video ||
      FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
      FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height)) ||
      FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator,
                                 &denominator)) ||
      denominator == 0 || !valid_dimensions(width, height)) {
    return false;
  }
  const auto description = describe_format(subtype);
  if (description.format == PixelFormat::unknown) return false;
  mode.device_id = device_id;
  mode.width = width;
  mode.height = height;
  mode.frame_rate = {static_cast<std::int32_t>(numerator),
                     static_cast<std::int32_t>(denominator)};
  mode.device_format = description.format;
  mode.delivered_format = description.format;
  mode.device_format_name = description.name;
  return true;
}

bool same_mode(const CaptureMode& left, const CaptureMode& right) {
  return left.device_id == right.device_id && left.width == right.width &&
         left.height == right.height && left.frame_rate == right.frame_rate &&
         left.device_format == right.device_format;
}

class MediaFoundationCapture final : public CaptureSource {
 public:
  MediaFoundationCapture() : worker_([this] { worker_main(); }) {
    std::unique_lock lock(init_mutex_);
    init_cv_.wait(lock, [this] { return initialized_; });
    if (init_error_) {
      worker_.join();
      std::rethrow_exception(init_error_);
    }
  }

  ~MediaFoundationCapture() override {
    stop();
    {
      std::lock_guard lock(command_mutex_);
      quitting_ = true;
    }
    command_cv_.notify_one();
    if (worker_.joinable()) worker_.join();
  }

  std::vector<DeviceInfo> enumerate_devices() override {
    return invoke([this] { return enumerate_devices_on_worker(); });
  }

  std::vector<CaptureMode> enumerate_modes(
      const std::string& stable_id) override {
    return invoke(
        [this, stable_id] { return enumerate_modes_on_worker(stable_id); });
  }

  void start(const CaptureMode& mode) override {
    {
      std::lock_guard lock(state_mutex_);
      snapshot_.state = CaptureState::starting;
      snapshot_.error.clear();
    }
    post([this, mode] {
      try {
        stop_on_worker();
        start_on_worker(mode);
      } catch (const std::exception& error) {
        stop_on_worker();
        set_fault(error.what());
      }
    });
  }

  void stop() noexcept override {
    if (!worker_.joinable() || std::this_thread::get_id() == worker_.get_id())
      return;
    try {
      invoke([this] { stop_on_worker(); });
    } catch (...) {
    }
  }

  std::optional<CaptureSample> take_latest_sample() override {
    std::lock_guard lock(sample_mutex_);
    auto result = std::move(latest_sample_);
    latest_sample_.reset();
    return result;
  }

  CaptureSnapshot snapshot() const override {
    std::lock_guard lock(state_mutex_);
    return snapshot_;
  }

 private:
  class ReaderCallback final : public IMFSourceReaderCallback {
   public:
    explicit ReaderCallback(MediaFoundationCapture& owner) : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
      if (object == nullptr) return E_POINTER;
      if (iid == __uuidof(IUnknown) ||
          iid == __uuidof(IMFSourceReaderCallback)) {
        *object = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
      }
      *object = nullptr;
      return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
      const ULONG remaining = --references_;
      if (remaining == 0) delete this;
      return remaining;
    }

    STDMETHODIMP OnReadSample(HRESULT status, DWORD stream_index, DWORD flags,
                              LONGLONG timestamp, IMFSample* sample) override {
      owner_.on_read_sample(status, stream_index, flags, timestamp, sample);
      return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override {
      owner_.on_flush();
      return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

   private:
    std::atomic<ULONG> references_{1};
    MediaFoundationCapture& owner_;
  };

  struct CallbackScope {
    explicit CallbackScope(MediaFoundationCapture& owner) : owner(owner) {
      owner.callbacks_in_progress_.fetch_add(1, std::memory_order_acq_rel);
    }
    ~CallbackScope() {
      owner.callbacks_in_progress_.fetch_sub(1, std::memory_order_acq_rel);
      owner.callback_cv_.notify_all();
    }
    MediaFoundationCapture& owner;
  };

  template <typename Function>
  auto invoke(Function&& function) -> decltype(function()) {
    using Result = decltype(function());
    auto task = std::make_shared<std::packaged_task<Result()>>(
        std::forward<Function>(function));
    auto future = task->get_future();
    post([task] { (*task)(); });
    return future.get();
  }

  void post(std::function<void()> command) {
    {
      std::lock_guard lock(command_mutex_);
      if (quitting_) throw std::runtime_error("capture thread is stopping");
      commands_.push_back(std::move(command));
    }
    command_cv_.notify_one();
  }

  void worker_main() noexcept {
    HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    try {
      check(com_result, "CoInitializeEx");
      check(MFStartup(MF_VERSION, MFSTARTUP_FULL), "MFStartup");
      {
        std::lock_guard lock(init_mutex_);
        initialized_ = true;
      }
      init_cv_.notify_one();

      for (;;) {
        std::function<void()> command;
        {
          std::unique_lock lock(command_mutex_);
          command_cv_.wait(lock,
                           [this] { return quitting_ || !commands_.empty(); });
          if (quitting_ && commands_.empty()) break;
          command = std::move(commands_.front());
          commands_.pop_front();
        }
        command();
      }
      stop_on_worker();
      MFShutdown();
      if (uninitialize_com) CoUninitialize();
    } catch (...) {
      {
        std::lock_guard lock(init_mutex_);
        initialized_ = true;
        init_error_ = std::current_exception();
      }
      init_cv_.notify_one();
      if (uninitialize_com) CoUninitialize();
    }
  }

  std::vector<ComPtr<IMFActivate>> activate_devices() {
    ComPtr<IMFAttributes> attributes;
    check(MFCreateAttributes(&attributes, 1), "MFCreateAttributes");
    check(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
          "SetGUID(video capture source)");
    IMFActivate** raw = nullptr;
    UINT32 count = 0;
    check(MFEnumDeviceSources(attributes.Get(), &raw, &count),
          "MFEnumDeviceSources");
    std::vector<ComPtr<IMFActivate>> devices;
    devices.reserve(count);
    for (UINT32 index = 0; index < count; ++index) {
      devices.emplace_back(raw[index]);
    }
    CoTaskMemFree(raw);
    return devices;
  }

  static std::string activation_string(IMFActivate* activate, REFGUID key) {
    wchar_t* value = nullptr;
    UINT32 length = 0;
    if (FAILED(activate->GetAllocatedString(key, &value, &length))) return {};
    std::string result = utf8(value, length);
    CoTaskMemFree(value);
    return result;
  }

  std::vector<DeviceInfo> enumerate_devices_on_worker() {
    std::vector<DeviceInfo> result;
    for (auto& device : activate_devices()) {
      const auto id = activation_string(
          device.Get(),
          MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
      if (id.empty()) continue;
      result.push_back({CaptureBackend::media_foundation,
                        id,
                        activation_string(device.Get(),
                                          MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME),
                        std::nullopt,
                        std::nullopt,
                        {},
                        false});
    }
    return result;
  }

  ComPtr<IMFMediaSource> open_source(const std::string& stable_id) {
    for (auto& device : activate_devices()) {
      if (activation_string(
              device.Get(),
              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK) ==
          stable_id) {
        ComPtr<IMFMediaSource> source;
        check(device->ActivateObject(IID_PPV_ARGS(&source)), "ActivateObject");
        return source;
      }
    }
    throw std::runtime_error("Media Foundation capture device is not present");
  }

  ComPtr<IMFSourceReader> make_reader(IMFMediaSource* source,
                                      IMFSourceReaderCallback* callback) {
    ComPtr<IMFAttributes> attributes;
    check(MFCreateAttributes(&attributes, callback ? 4 : 3),
          "MFCreateAttributes");
    check(attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE),
          "disable Source Reader converters");
    check(
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE),
        "disable Source Reader video processing");
    check(attributes->SetUINT32(
              MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE),
          "configure Source Reader shutdown");
    if (callback) {
      check(attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback),
            "set Source Reader callback");
    }
    ComPtr<IMFSourceReader> reader;
    check(
        MFCreateSourceReaderFromMediaSource(source, attributes.Get(), &reader),
        "MFCreateSourceReaderFromMediaSource");
    return reader;
  }

  std::vector<CaptureMode> enumerate_modes(const std::string& stable_id,
                                           IMFSourceReader* reader) {
    std::vector<CaptureMode> modes;
    for (DWORD index = 0;; ++index) {
      ComPtr<IMFMediaType> type;
      const HRESULT hr = reader->GetNativeMediaType(
          MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &type);
      if (hr == MF_E_NO_MORE_TYPES) break;
      check(hr, "GetNativeMediaType");
      CaptureMode mode;
      if (media_type_mode(type.Get(), stable_id, mode) &&
          std::find(modes.begin(), modes.end(), mode) == modes.end()) {
        modes.push_back(std::move(mode));
      }
    }
    return modes;
  }

  std::vector<CaptureMode> enumerate_modes_on_worker(
      const std::string& stable_id) {
    auto source = open_source(stable_id);
    auto reader = make_reader(source.Get(), nullptr);
    auto modes = enumerate_modes(stable_id, reader.Get());
    reader.Reset();
    source->Shutdown();
    return modes;
  }

  ComPtr<IMFMediaType> find_native_type(IMFSourceReader* reader,
                                        const CaptureMode& requested) {
    for (DWORD index = 0;; ++index) {
      ComPtr<IMFMediaType> type;
      const HRESULT hr = reader->GetNativeMediaType(
          MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &type);
      if (hr == MF_E_NO_MORE_TYPES) break;
      check(hr, "GetNativeMediaType");
      CaptureMode mode;
      if (media_type_mode(type.Get(), requested.device_id, mode) &&
          same_mode(mode, requested))
        return type;
    }
    throw std::runtime_error(
        "requested Media Foundation native mode is unavailable");
  }

  void start_on_worker(const CaptureMode& requested) {
    source_ = open_source(requested.device_id);
    callback_.Attach(new ReaderCallback(*this));
    reader_ = make_reader(source_.Get(), callback_.Get());
    check(reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE),
          "disable Source Reader streams");
    check(
        reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE),
        "enable Source Reader video stream");
    auto native_type = find_native_type(reader_.Get(), requested);
    check(reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                       nullptr, native_type.Get()),
          "SetCurrentMediaType");
    CaptureMode actual;
    check(media_type_mode(native_type.Get(), requested.device_id, actual)
              ? S_OK
              : E_FAIL,
          "read selected media type");
    UINT32 stride_bits = 0;
    native_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_bits);
    UINT32 sar_numerator = 1, sar_denominator = 1;
    MFGetAttributeRatio(native_type.Get(), MF_MT_PIXEL_ASPECT_RATIO,
                        &sar_numerator, &sar_denominator);
    UINT32 nominal_range = MFNominalRange_Unknown;
    native_type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominal_range);
    UINT32 yuv_matrix = MFVideoTransferMatrix_Unknown;
    native_type->GetUINT32(MF_MT_YUV_MATRIX, &yuv_matrix);
    {
      std::lock_guard lock(state_mutex_);
      ++snapshot_.generation;
      snapshot_.actual_mode = actual;
      native_stride_ = static_cast<LONG>(stride_bits);
      sar_numerator_ = sar_numerator;
      sar_denominator_ = sar_denominator == 0 ? 1 : sar_denominator;
      color_range_ = nominal_range == MFNominalRange_0_255 ? ColorRange::full
                     : nominal_range == MFNominalRange_16_235
                         ? ColorRange::limited
                         : ColorRange::unknown;
      color_matrix_ =
          yuv_matrix == MFVideoTransferMatrix_BT709   ? ColorMatrix::bt709
          : yuv_matrix == MFVideoTransferMatrix_BT601 ? ColorMatrix::bt601
                                                      : ColorMatrix::unknown;
      snapshot_.state = CaptureState::streaming;
      snapshot_.error.clear();
    }
    sequence_.store(0, std::memory_order_release);
    rearm_.store(true, std::memory_order_release);
    request_sample();
  }

  void stop_on_worker() noexcept {
    rearm_.store(false, std::memory_order_release);
    if (reader_) {
      {
        std::lock_guard lock(state_mutex_);
        snapshot_.state = CaptureState::stopping;
      }
      flush_done_.store(false, std::memory_order_release);
      const HRESULT hr = reader_->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
      if (FAILED(hr)) flush_done_.store(true, std::memory_order_release);
      std::unique_lock lock(callback_mutex_);
      callback_cv_.wait(lock, [this] {
        return flush_done_.load(std::memory_order_acquire) &&
               callbacks_in_progress_.load(std::memory_order_acquire) == 0 &&
               !read_outstanding_.load(std::memory_order_acquire);
      });
    }
    reader_.Reset();
    if (source_) source_->Shutdown();
    source_.Reset();
    callback_.Reset();
    {
      std::lock_guard lock(sample_mutex_);
      latest_sample_.reset();
    }
    std::lock_guard lock(state_mutex_);
    if (snapshot_.state != CaptureState::fault)
      snapshot_.state = CaptureState::stopped;
  }

  void request_sample() noexcept {
    if (!rearm_.load(std::memory_order_acquire) || !reader_) return;
    bool expected = false;
    if (!read_outstanding_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel))
      return;
    const HRESULT hr =
        reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                            nullptr, nullptr, nullptr);
    if (FAILED(hr)) {
      read_outstanding_.store(false, std::memory_order_release);
      set_fault(hresult_text("ReadSample", hr));
      callback_cv_.notify_all();
    }
  }

  void update_current_type() {
    ComPtr<IMFMediaType> type;
    check(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                       &type),
          "GetCurrentMediaType");
    CaptureMode current;
    std::string device_id;
    {
      std::lock_guard lock(state_mutex_);
      device_id = snapshot_.actual_mode.device_id;
    }
    if (!media_type_mode(type.Get(), device_id, current))
      throw std::runtime_error("unsupported Media Foundation format change");
    UINT32 stride_bits = 0;
    type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_bits);
    UINT32 sar_numerator = 1, sar_denominator = 1;
    MFGetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, &sar_numerator,
                        &sar_denominator);
    UINT32 nominal_range = MFNominalRange_Unknown;
    type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominal_range);
    UINT32 yuv_matrix = MFVideoTransferMatrix_Unknown;
    type->GetUINT32(MF_MT_YUV_MATRIX, &yuv_matrix);
    {
      std::lock_guard lock(state_mutex_);
      snapshot_.actual_mode = current;
      native_stride_ = static_cast<LONG>(stride_bits);
      sar_numerator_ = sar_numerator;
      sar_denominator_ = sar_denominator == 0 ? 1 : sar_denominator;
      color_range_ = nominal_range == MFNominalRange_0_255 ? ColorRange::full
                     : nominal_range == MFNominalRange_16_235
                         ? ColorRange::limited
                         : ColorRange::unknown;
      color_matrix_ =
          yuv_matrix == MFVideoTransferMatrix_BT709   ? ColorMatrix::bt709
          : yuv_matrix == MFVideoTransferMatrix_BT601 ? ColorMatrix::bt601
                                                      : ColorMatrix::unknown;
      ++snapshot_.generation;
    }
    std::lock_guard sample_lock(sample_mutex_);
    latest_sample_.reset();
  }

  std::optional<CaptureSample> copy_sample(IMFSample* sample,
                                           LONGLONG timestamp) {
    CaptureMode mode;
    std::uint64_t generation;
    LONG native_stride;
    UINT32 sar_numerator, sar_denominator;
    ColorRange color_range;
    ColorMatrix color_matrix;
    {
      std::lock_guard lock(state_mutex_);
      mode = snapshot_.actual_mode;
      generation = snapshot_.generation;
      native_stride = native_stride_;
      sar_numerator = sar_numerator_;
      sar_denominator = sar_denominator_;
      color_range = color_range_;
      color_matrix = color_matrix_;
    }
    if (!valid_dimensions(mode.width, mode.height)) return std::nullopt;
    ComPtr<IMFMediaBuffer> buffer;
    check(sample->ConvertToContiguousBuffer(&buffer),
          "ConvertToContiguousBuffer");
    const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto arrival = std::chrono::steady_clock::now();
    std::optional<CaptureSample> result;

    if (mode.delivered_format == PixelFormat::mjpeg) {
      BYTE* bytes = nullptr;
      DWORD maximum = 0, length = 0;
      check(buffer->Lock(&bytes, &maximum, &length), "IMFMediaBuffer::Lock");
      try {
        result = CaptureSample::make_mjpeg(
            generation, sequence, arrival, mode.width, mode.height,
            std::span<const std::uint8_t>(bytes, length));
      } catch (...) {
        buffer->Unlock();
        throw;
      }
      buffer->Unlock();
    } else {
      std::size_t row_bytes = 0;
      std::size_t rows = mode.height;
      switch (mode.delivered_format) {
        case PixelFormat::yuy2:
        case PixelFormat::uyvy:
          row_bytes = static_cast<std::size_t>(mode.width) * 2U;
          break;
        case PixelFormat::bgra:
          row_bytes = static_cast<std::size_t>(mode.width) * 4U;
          break;
        case PixelFormat::nv12:
          row_bytes = mode.width;
          rows += mode.height / 2U;
          break;
        default:
          return std::nullopt;
      }
      if (row_bytes == 0 || rows > kMaxRawSampleBytes / row_bytes)
        return std::nullopt;
      std::vector<std::uint8_t> owned(row_bytes * rows);
      ComPtr<IMF2DBuffer> buffer2d;
      if (SUCCEEDED(buffer.As(&buffer2d))) {
        BYTE* scanline = nullptr;
        LONG pitch = 0;
        check(buffer2d->Lock2D(&scanline, &pitch), "IMF2DBuffer::Lock2D");
        const auto absolute_pitch = static_cast<std::size_t>(
            pitch < 0 ? -static_cast<std::int64_t>(pitch) : pitch);
        if (absolute_pitch < row_bytes) {
          buffer2d->Unlock2D();
          return std::nullopt;
        }
        for (std::size_t row = 0; row < rows; ++row)
          std::memcpy(owned.data() + row * row_bytes,
                      scanline + static_cast<std::ptrdiff_t>(row) * pitch,
                      row_bytes);
        buffer2d->Unlock2D();
      } else {
        BYTE* bytes = nullptr;
        DWORD maximum = 0, length = 0;
        check(buffer->Lock(&bytes, &maximum, &length), "IMFMediaBuffer::Lock");
        LONG stride =
            native_stride != 0 ? native_stride : static_cast<LONG>(row_bytes);
        const auto absolute_stride = static_cast<std::size_t>(
            stride < 0 ? -static_cast<std::int64_t>(stride) : stride);
        if (absolute_stride < row_bytes ||
            rows > 0 &&
                (rows - 1U) > (length >= row_bytes
                                   ? (length - row_bytes) / absolute_stride
                                   : 0)) {
          buffer->Unlock();
          return std::nullopt;
        }
        const BYTE* first = bytes;
        if (stride < 0) first += (rows - 1U) * absolute_stride;
        for (std::size_t row = 0; row < rows; ++row)
          std::memcpy(owned.data() + row * row_bytes,
                      first + static_cast<std::ptrdiff_t>(row) * stride,
                      row_bytes);
        buffer->Unlock();
      }
      std::vector<PlaneLayout> planes;
      if (mode.delivered_format == PixelFormat::nv12) {
        planes.push_back({0, static_cast<std::ptrdiff_t>(row_bytes), row_bytes,
                          mode.height});
        planes.push_back({row_bytes * mode.height,
                          static_cast<std::ptrdiff_t>(row_bytes), row_bytes,
                          mode.height / 2U});
      } else {
        planes.push_back({0, static_cast<std::ptrdiff_t>(row_bytes), row_bytes,
                          mode.height});
      }
      result = CaptureSample::make_raw(generation, sequence, arrival,
                                       mode.width, mode.height,
                                       mode.delivered_format, planes, owned);
    }
    if (result) {
      result->device_timestamp = timestamp;
      result->device_time_base_numerator = 1;
      result->device_time_base_denominator = 10'000'000;
      result->sample_aspect_ratio_numerator = sar_numerator;
      result->sample_aspect_ratio_denominator = sar_denominator;
      result->color_range = color_range;
      result->color_matrix = color_matrix;
    }
    return result;
  }

  void on_read_sample(HRESULT status, DWORD, DWORD flags, LONGLONG timestamp,
                      IMFSample* sample) noexcept {
    CallbackScope scope(*this);
    read_outstanding_.store(false, std::memory_order_release);
    try {
      if (FAILED(status))
        throw std::runtime_error(hresult_text("OnReadSample", status));
      if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0)
        update_current_type();
      if ((flags & MF_SOURCE_READERF_ERROR) != 0)
        throw std::runtime_error(
            "Media Foundation Source Reader reported an error");
      if (sample != nullptr) {
        auto copied = copy_sample(sample, timestamp);
        if (!copied)
          throw std::runtime_error("invalid Media Foundation video sample");
        {
          std::lock_guard lock(sample_mutex_);
          if (latest_sample_) {
            std::lock_guard state_lock(state_mutex_);
            ++snapshot_.overwritten_samples;
          }
          latest_sample_ = std::move(copied);
        }
        std::lock_guard lock(state_mutex_);
        ++snapshot_.received_samples;
      }
    } catch (const std::exception& error) {
      rearm_.store(false, std::memory_order_release);
      set_fault(error.what());
    }
    request_sample();
    callback_cv_.notify_all();
  }

  void on_flush() noexcept {
    CallbackScope scope(*this);
    read_outstanding_.store(false, std::memory_order_release);
    flush_done_.store(true, std::memory_order_release);
    callback_cv_.notify_all();
  }

  void set_fault(std::string error) noexcept {
    std::lock_guard lock(state_mutex_);
    snapshot_.state = CaptureState::fault;
    snapshot_.error = std::move(error);
  }

  std::thread worker_;
  std::mutex init_mutex_;
  std::condition_variable init_cv_;
  bool initialized_{};
  std::exception_ptr init_error_;

  std::mutex command_mutex_;
  std::condition_variable command_cv_;
  std::deque<std::function<void()>> commands_;
  bool quitting_{};

  ComPtr<IMFMediaSource> source_;
  ComPtr<IMFSourceReader> reader_;
  ComPtr<ReaderCallback> callback_;
  std::atomic<bool> rearm_{};
  std::atomic<bool> read_outstanding_{};
  std::atomic<bool> flush_done_{true};
  std::atomic<unsigned> callbacks_in_progress_{};
  std::mutex callback_mutex_;
  std::condition_variable callback_cv_;

  mutable std::mutex state_mutex_;
  CaptureSnapshot snapshot_;
  LONG native_stride_{};
  UINT32 sar_numerator_{1};
  UINT32 sar_denominator_{1};
  ColorRange color_range_{ColorRange::unknown};
  ColorMatrix color_matrix_{ColorMatrix::unknown};
  std::atomic<std::uint64_t> sequence_{};
  std::mutex sample_mutex_;
  std::optional<CaptureSample> latest_sample_;
};

}  // namespace

std::unique_ptr<CaptureSource> create_platform_capture_source() {
  return std::make_unique<MediaFoundationCapture>();
}

}  // namespace kvmux

#endif

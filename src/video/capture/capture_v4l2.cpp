#include "video/capture/capture_source.hpp"

#ifdef __linux__

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kvmux {
namespace {

constexpr std::uint32_t kRequestedBuffers = 3;

int xioctl(int fd, unsigned long request, void* argument) {
  int result;
  do {
    result = ::ioctl(fd, request, argument);
  } while (result < 0 && errno == EINTR);
  return result;
}

std::string errno_text(const char* operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

struct FormatInfo {
  std::uint32_t fourcc;
  PixelFormat pixel_format;
  const char* name;
};

constexpr FormatInfo kFormats[] = {
    {V4L2_PIX_FMT_YUYV, PixelFormat::yuy2, "YUYV"},
    {V4L2_PIX_FMT_UYVY, PixelFormat::uyvy, "UYVY"},
    {V4L2_PIX_FMT_NV12, PixelFormat::nv12, "NV12"},
    {V4L2_PIX_FMT_MJPEG, PixelFormat::mjpeg, "MJPEG"},
};

std::optional<std::uint32_t> fourcc_for(PixelFormat format) {
  const auto it = std::find_if(
      std::begin(kFormats), std::end(kFormats),
      [format](const auto& f) { return f.pixel_format == format; });
  if (it == std::end(kFormats)) return std::nullopt;
  return it->fourcc;
}

bool supported_size(std::uint32_t width, std::uint32_t height) {
  return width > 0 && height > 0 && width <= kMaxCaptureWidth &&
         height <= kMaxCaptureHeight;
}

bool supported_rate(std::uint32_t numerator, std::uint32_t denominator) {
  return numerator != 0 && denominator != 0 &&
         static_cast<std::uint64_t>(denominator) <= 60ULL * numerator;
}

std::vector<std::string> device_paths() {
  namespace fs = std::filesystem;
  std::vector<std::string> result;
  std::unordered_set<std::string> targets;
  std::error_code ec;

  const fs::path by_id{"/dev/v4l/by-id"};
  if (fs::is_directory(by_id, ec)) {
    std::vector<fs::path> links;
    for (fs::directory_iterator it(by_id, ec), end; !ec && it != end;
         it.increment(ec)) {
      if (it->is_symlink(ec)) links.push_back(it->path());
    }
    std::sort(links.begin(), links.end());
    for (const auto& link : links) {
      const auto canonical = fs::weakly_canonical(link, ec);
      if (!ec && targets.insert(canonical.string()).second)
        result.push_back(link.string());
      ec.clear();
    }
  }

  std::vector<fs::path> nodes;
  const fs::path dev{"/dev"};
  for (fs::directory_iterator it(dev, ec), end; !ec && it != end;
       it.increment(ec)) {
    const auto name = it->path().filename().string();
    if (name.starts_with("video") && name.size() > 5 &&
        std::all_of(name.begin() + 5, name.end(),
                    [](unsigned char c) { return c >= '0' && c <= '9'; })) {
      nodes.push_back(it->path());
    }
  }
  std::sort(nodes.begin(), nodes.end());
  for (const auto& node : nodes) {
    const auto canonical = fs::weakly_canonical(node, ec);
    if (!ec && targets.insert(canonical.string()).second)
      result.push_back(node.string());
    ec.clear();
  }
  return result;
}

bool capture_capable(const v4l2_capability& cap) {
  const auto flags = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                         ? cap.device_caps
                         : cap.capabilities;
  return (flags & V4L2_CAP_VIDEO_CAPTURE) && (flags & V4L2_CAP_STREAMING) &&
         !(flags & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
}

class V4l2CaptureSource final : public CaptureSource {
 public:
  ~V4l2CaptureSource() override { stop(); }

  std::vector<DeviceInfo> enumerate_devices() override {
    std::vector<DeviceInfo> devices;
    for (const auto& path : device_paths()) {
      const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0) continue;
      v4l2_capability cap{};
      const bool usable =
          xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && capture_capable(cap);
      ::close(fd);
      if (!usable) continue;
      DeviceInfo info;
      info.backend = CaptureBackend::v4l2;
      info.stable_id = path;
      info.display_name = reinterpret_cast<const char*>(cap.card);
      info.weak_match = !path.starts_with("/dev/v4l/by-id/");
      devices.push_back(std::move(info));
    }
    return devices;
  }

  std::vector<CaptureMode> enumerate_modes(
      const std::string& stable_id) override {
    std::vector<CaptureMode> modes;
    const int fd = ::open(stable_id.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return modes;
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0 || !capture_capable(cap)) {
      ::close(fd);
      return modes;
    }

    for (const auto& known : kFormats) {
      v4l2_fmtdesc desc{};
      desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      for (desc.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0;
           ++desc.index) {
        if (desc.pixelformat != known.fourcc) continue;
        v4l2_frmsizeenum size{};
        size.pixel_format = known.fourcc;
        for (size.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0;
             ++size.index) {
          std::vector<std::pair<std::uint32_t, std::uint32_t>> dimensions;
          if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            dimensions.emplace_back(size.discrete.width, size.discrete.height);
          } else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
                     size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
            dimensions.emplace_back(
                std::min(size.stepwise.max_width, kMaxCaptureWidth),
                std::min(size.stepwise.max_height, kMaxCaptureHeight));
          }
          for (const auto [width, height] : dimensions) {
            if (!supported_size(width, height)) continue;
            v4l2_frmivalenum interval{};
            interval.pixel_format = known.fourcc;
            interval.width = width;
            interval.height = height;
            for (interval.index = 0;
                 xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0;
                 ++interval.index) {
              std::uint32_t numerator = 0;
              std::uint32_t denominator = 0;
              if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                numerator = interval.discrete.numerator;
                denominator = interval.discrete.denominator;
              } else if (interval.type == V4L2_FRMIVAL_TYPE_STEPWISE ||
                         interval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
                numerator = interval.stepwise.min.numerator;
                denominator = std::min(interval.stepwise.min.denominator,
                                       60U * std::max(1U, numerator));
              }
              if (!supported_rate(numerator, denominator) ||
                  numerator > static_cast<std::uint32_t>(
                                  std::numeric_limits<std::int32_t>::max()) ||
                  denominator > static_cast<std::uint32_t>(
                                    std::numeric_limits<std::int32_t>::max())) {
                if (interval.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
                continue;
              }
              CaptureMode mode;
              mode.device_id = stable_id;
              mode.width = width;
              mode.height = height;
              mode.frame_rate = {static_cast<std::int32_t>(denominator),
                                 static_cast<std::int32_t>(numerator)};
              mode.device_format = known.pixel_format;
              mode.delivered_format = known.pixel_format;
              mode.device_format_name = known.name;
              modes.push_back(std::move(mode));
              if (interval.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
            }
          }
        }
      }
    }
    ::close(fd);
    return modes;
  }

  void start(const CaptureMode& mode) override {
    stop();
    {
      std::lock_guard lock(mutex_);
      ++snapshot_.generation;
      snapshot_.state = CaptureState::starting;
      snapshot_.error.clear();
      snapshot_.actual_mode = mode;
      latest_.reset();
      stop_requested_.store(false, std::memory_order_release);
    }
    worker_ = std::thread([this, mode] { run(mode); });
  }

  void stop() noexcept override {
    stop_requested_.store(true, std::memory_order_release);
    const int event_fd = stop_fd_.load(std::memory_order_acquire);
    if (event_fd >= 0) {
      const std::uint64_t one = 1;
      (void)::write(event_fd, &one, sizeof(one));
    }
    if (worker_.joinable()) worker_.join();
    std::optional<CaptureSample> discarded;
    {
      std::lock_guard lock(mutex_);
      discarded.swap(latest_);
      snapshot_.state = CaptureState::stopped;
    }
  }

  std::optional<CaptureSample> take_latest_sample() override {
    std::optional<CaptureSample> result;
    std::lock_guard lock(mutex_);
    result.swap(latest_);
    return result;
  }

  CaptureSnapshot snapshot() const override {
    std::lock_guard lock(mutex_);
    return snapshot_;
  }

 private:
  struct Mapping {
    void* address{MAP_FAILED};
    std::size_t length{};
  };

  void fail(std::string message, int saved_errno = 0) {
    std::lock_guard lock(mutex_);
    snapshot_.state = (saved_errno == EACCES || saved_errno == EPERM)
                          ? CaptureState::permission_denied
                          : CaptureState::fault;
    snapshot_.error = std::move(message);
  }

  void run(const CaptureMode requested) {
    int video_fd = -1;
    int event_fd = -1;
    bool streaming = false;
    std::vector<Mapping> mappings;
    auto cleanup = [&] {
      if (streaming) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)xioctl(video_fd, VIDIOC_STREAMOFF, &type);
      }
      for (const auto& mapping : mappings) {
        if (mapping.address != MAP_FAILED)
          ::munmap(mapping.address, mapping.length);
      }
      if (video_fd >= 0) ::close(video_fd);
      stop_fd_.store(-1, std::memory_order_release);
      if (event_fd >= 0) ::close(event_fd);
    };

    if (stop_requested_.load(std::memory_order_acquire)) return;
    event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0) {
      fail(errno_text("eventfd"), errno);
      return;
    }
    stop_fd_.store(event_fd, std::memory_order_release);
    if (stop_requested_.load(std::memory_order_acquire)) {
      cleanup();
      return;
    }
    video_fd =
        ::open(requested.device_id.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (video_fd < 0) {
      const int e = errno;
      fail(errno_text("open"), e);
      cleanup();
      return;
    }
    v4l2_capability cap{};
    if (xioctl(video_fd, VIDIOC_QUERYCAP, &cap) != 0 || !capture_capable(cap)) {
      fail(errno_text("VIDIOC_QUERYCAP"), errno);
      cleanup();
      return;
    }
    const auto fourcc = fourcc_for(requested.device_format);
    if (!fourcc || !supported_size(requested.width, requested.height) ||
        requested.frame_rate.numerator <= 0 ||
        requested.frame_rate.denominator <= 0 ||
        !supported_rate(
            static_cast<std::uint32_t>(requested.frame_rate.denominator),
            static_cast<std::uint32_t>(requested.frame_rate.numerator))) {
      fail("unsupported V4L2 capture mode");
      cleanup();
      return;
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = requested.width;
    format.fmt.pix.height = requested.height;
    format.fmt.pix.pixelformat = *fourcc;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(video_fd, VIDIOC_S_FMT, &format) != 0) {
      fail(errno_text("VIDIOC_S_FMT"), errno);
      cleanup();
      return;
    }
    if (format.fmt.pix.width != requested.width ||
        format.fmt.pix.height != requested.height ||
        format.fmt.pix.pixelformat != *fourcc ||
        format.fmt.pix.field == V4L2_FIELD_INTERLACED ||
        format.fmt.pix.field == V4L2_FIELD_INTERLACED_TB ||
        format.fmt.pix.field == V4L2_FIELD_INTERLACED_BT) {
      fail("V4L2 device did not accept the requested mode");
      cleanup();
      return;
    }

    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameters.parm.capture.timeperframe.numerator =
        static_cast<std::uint32_t>(requested.frame_rate.denominator);
    parameters.parm.capture.timeperframe.denominator =
        static_cast<std::uint32_t>(requested.frame_rate.numerator);
    if (xioctl(video_fd, VIDIOC_S_PARM, &parameters) != 0) {
      fail(errno_text("VIDIOC_S_PARM"), errno);
      cleanup();
      return;
    }

    CaptureMode actual = requested;
    if (parameters.parm.capture.timeperframe.numerator != 0 &&
        parameters.parm.capture.timeperframe.denominator != 0) {
      const auto numerator = parameters.parm.capture.timeperframe.numerator;
      const auto denominator = parameters.parm.capture.timeperframe.denominator;
      if (!supported_rate(numerator, denominator) ||
          numerator > static_cast<std::uint32_t>(
                          std::numeric_limits<std::int32_t>::max()) ||
          denominator > static_cast<std::uint32_t>(
                            std::numeric_limits<std::int32_t>::max())) {
        fail("V4L2 device returned an unsupported frame rate");
        cleanup();
        return;
      }
      actual.frame_rate = {static_cast<std::int32_t>(denominator),
                           static_cast<std::int32_t>(numerator)};
    }
    if (actual.frame_rate != requested.frame_rate) {
      fail("V4L2 device did not accept the requested frame rate");
      cleanup();
      return;
    }

    v4l2_requestbuffers request{};
    request.count = kRequestedBuffers;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(video_fd, VIDIOC_REQBUFS, &request) != 0 || request.count < 1) {
      fail(errno_text("VIDIOC_REQBUFS"), errno);
      cleanup();
      return;
    }
    mappings.resize(request.count);
    for (std::uint32_t i = 0; i < request.count; ++i) {
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = i;
      if (xioctl(video_fd, VIDIOC_QUERYBUF, &buffer) != 0) {
        fail(errno_text("VIDIOC_QUERYBUF"), errno);
        cleanup();
        return;
      }
      mappings[i].length = buffer.length;
      mappings[i].address =
          ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                 video_fd, buffer.m.offset);
      if (mappings[i].address == MAP_FAILED) {
        fail(errno_text("mmap"), errno);
        cleanup();
        return;
      }
      if (xioctl(video_fd, VIDIOC_QBUF, &buffer) != 0) {
        fail(errno_text("VIDIOC_QBUF"), errno);
        cleanup();
        return;
      }
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(video_fd, VIDIOC_STREAMON, &type) != 0) {
      fail(errno_text("VIDIOC_STREAMON"), errno);
      cleanup();
      return;
    }
    streaming = true;
    {
      std::lock_guard lock(mutex_);
      snapshot_.actual_mode = actual;
      snapshot_.state = CaptureState::streaming;
    }

    while (!stop_requested_.load(std::memory_order_acquire)) {
      pollfd fds[2]{{video_fd, POLLIN | POLLPRI, 0}, {event_fd, POLLIN, 0}};
      const int poll_result = ::poll(fds, 2, -1);
      if (poll_result < 0) {
        if (errno == EINTR) continue;
        fail(errno_text("poll"), errno);
        break;
      }
      if (fds[1].revents & POLLIN) break;
      if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        fail("V4L2 device disconnected");
        break;
      }
      if (!(fds[0].revents & (POLLIN | POLLPRI))) continue;

      std::optional<v4l2_buffer> newest;
      for (;;) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(video_fd, VIDIOC_DQBUF, &buffer) != 0) {
          if (errno == EAGAIN) break;
          if (errno == EIO) continue;
          fail(errno_text("VIDIOC_DQBUF"), errno);
          stop_requested_.store(true, std::memory_order_release);
          break;
        }
        if (buffer.index >= mappings.size()) {
          fail("V4L2 returned an invalid buffer index");
          stop_requested_.store(true, std::memory_order_release);
          break;
        }
        const bool complete = !(buffer.flags & V4L2_BUF_FLAG_ERROR) &&
                              buffer.bytesused != 0 &&
                              buffer.bytesused <= mappings[buffer.index].length;
        if (!complete) {
          if (xioctl(video_fd, VIDIOC_QBUF, &buffer) != 0) {
            fail(errno_text("VIDIOC_QBUF"), errno);
            stop_requested_.store(true, std::memory_order_release);
            break;
          }
          continue;
        }
        if (newest) {
          auto old = *newest;
          if (xioctl(video_fd, VIDIOC_QBUF, &old) != 0) {
            fail(errno_text("VIDIOC_QBUF"), errno);
            stop_requested_.store(true, std::memory_order_release);
            break;
          }
        }
        newest = buffer;
      }
      if (!newest) continue;
      auto buffer = *newest;
      auto sample =
          copy_sample(actual, format.fmt.pix, buffer, mappings[buffer.index]);
      if (xioctl(video_fd, VIDIOC_QBUF, &buffer) != 0) {
        fail(errno_text("VIDIOC_QBUF"), errno);
        break;
      }
      if (sample) publish(std::move(*sample));
    }
    {
      std::lock_guard lock(mutex_);
      if (snapshot_.state == CaptureState::streaming)
        snapshot_.state = CaptureState::stopping;
    }
    cleanup();
  }

  std::optional<CaptureSample> copy_sample(const CaptureMode& mode,
                                           const v4l2_pix_format& format,
                                           const v4l2_buffer& buffer,
                                           const Mapping& mapping) {
    if (buffer.bytesused == 0 || buffer.bytesused > mapping.length)
      return std::nullopt;
    const auto* bytes = static_cast<const std::uint8_t*>(mapping.address);
    std::optional<CaptureSample> sample;
    std::uint64_t generation;
    {
      std::lock_guard lock(mutex_);
      generation = snapshot_.generation;
    }
    const auto arrival = std::chrono::steady_clock::now();
    const auto sequence = next_sequence_++;
    if (mode.device_format == PixelFormat::mjpeg) {
      if (buffer.bytesused > kMaxCompressedSampleBytes) return std::nullopt;
      sample =
          CaptureSample::make_mjpeg(generation, sequence, arrival, mode.width,
                                    mode.height, {bytes, buffer.bytesused});
    } else {
      std::vector<PlaneLayout> planes;
      const std::size_t stride = format.bytesperline;
      if (stride == 0) return std::nullopt;
      if (mode.device_format == PixelFormat::yuy2 ||
          mode.device_format == PixelFormat::uyvy) {
        if (stride < static_cast<std::size_t>(mode.width) * 2U)
          return std::nullopt;
        planes.push_back({0, static_cast<std::ptrdiff_t>(stride),
                          static_cast<std::size_t>(mode.width) * 2U,
                          mode.height});
      } else if (mode.device_format == PixelFormat::nv12) {
        if (stride < mode.width) return std::nullopt;
        if (mode.height > std::numeric_limits<std::size_t>::max() / stride)
          return std::nullopt;
        const std::size_t y_size = stride * mode.height;
        if (y_size > buffer.bytesused) return std::nullopt;
        planes.push_back(
            {0, static_cast<std::ptrdiff_t>(stride), mode.width, mode.height});
        planes.push_back({y_size, static_cast<std::ptrdiff_t>(stride),
                          mode.width,
                          (static_cast<std::size_t>(mode.height) + 1U) / 2U});
      } else {
        return std::nullopt;
      }
      if (buffer.bytesused > kMaxRawSampleBytes) return std::nullopt;
      sample = CaptureSample::make_raw(
          generation, sequence, arrival, mode.width, mode.height,
          mode.device_format, planes, {bytes, buffer.bytesused});
    }
    if (sample) {
      sample->device_timestamp =
          static_cast<std::int64_t>(buffer.timestamp.tv_sec) * 1'000'000LL +
          buffer.timestamp.tv_usec;
      sample->device_time_base_numerator = 1;
      sample->device_time_base_denominator = 1'000'000;
    }
    return sample;
  }

  void publish(CaptureSample sample) {
    std::optional<CaptureSample> discarded;
    {
      std::lock_guard lock(mutex_);
      ++snapshot_.received_samples;
      if (latest_) {
        ++snapshot_.overwritten_samples;
        discarded.swap(latest_);
      }
      latest_ = std::move(sample);
    }
  }

  mutable std::mutex mutex_;
  CaptureSnapshot snapshot_;
  std::optional<CaptureSample> latest_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<int> stop_fd_{-1};
  std::uint64_t next_sequence_{0};
};

}  // namespace

std::unique_ptr<CaptureSource> create_platform_capture_source() {
  return std::make_unique<V4l2CaptureSource>();
}

}  // namespace kvmux

#endif  // __linux__

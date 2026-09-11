#include "app/kvm_session.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace kvmux {
namespace {
constexpr auto kCapturePoll = std::chrono::milliseconds(25);
constexpr auto kStaleAfter = std::chrono::milliseconds(500);
constexpr std::array kReconnectBackoff{
    std::chrono::milliseconds(500), std::chrono::milliseconds(1000),
    std::chrono::milliseconds(2000), std::chrono::milliseconds(5000)};
}

KvmSession::KvmSession()
    : KvmSession(create_platform_capture_source(), std::make_unique<Ch9329ControlSink>()) {}

KvmSession::KvmSession(std::unique_ptr<CaptureSource> capture,
                       std::unique_ptr<ControlSink> control)
    : capture_(std::move(capture)), control_(std::move(control)),
      video_(*capture_), input_(*control_),
      capture_worker_([this] { capture_loop(); }) {}

KvmSession::~KvmSession() { shutdown(); }

bool KvmSession::preview_only() const noexcept {
    return input_.state() == InputState::preview;
}

void KvmSession::note_session_error(std::string error) {
    std::lock_guard lock(mutex_);
    session_error_ = std::move(error);
}


std::future<std::vector<DeviceInfo>> KvmSession::enumerate_capture_devices() {
    auto task = std::make_shared<std::packaged_task<std::vector<DeviceInfo>()>>(
        [this] { return capture_->enumerate_devices(); });
    auto future = task->get_future();
    {
        std::lock_guard lock(mutex_);
        capture_tasks_.push_back([task] { (*task)(); });
    }
    wake_.notify_one();
    return future;
}

std::future<std::vector<CaptureMode>> KvmSession::enumerate_capture_modes(std::string stable_id) {
    auto task = std::make_shared<std::packaged_task<std::vector<CaptureMode>()>>(
        [this, stable_id = std::move(stable_id)] { return capture_->enumerate_modes(stable_id); });
    auto future = task->get_future();
    {
        std::lock_guard lock(mutex_);
        capture_tasks_.push_back([task] { (*task)(); });
    }
    wake_.notify_one();
    return future;
}

bool KvmSession::select_capture(const DeviceInfo& device, const CaptureMode& mode) {
    if (!preview_only() || mode.device_id != device.stable_id || device.stable_id.empty()) return false;
    request_release();
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_) return false;
        selected_device_ = device;
        selected_mode_ = mode;
        requested_mode_ = mode;
        command_ = CaptureCommand::start;
        video_state_ = SessionVideoState::stopping;
        session_error_.clear();
        video_fresh_ = false;
        last_sample_arrival_ = {};
    }
    input_.set_video_fresh(false);
    wake_.notify_one();
    return true;
}

bool KvmSession::stop_capture() {
    if (!preview_only()) return false;
    request_release();
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_) return false;
        command_ = CaptureCommand::stop;
        requested_mode_.reset();
        selected_mode_.reset();
        selected_device_.reset();
        video_state_ = SessionVideoState::stopping;
        video_fresh_ = false;
        last_sample_arrival_ = {};
    }
    input_.set_video_fresh(false);
    wake_.notify_one();
    return true;
}

bool KvmSession::connect_control(std::string port, int baud_rate, std::uint8_t address) {
    if (!preview_only() || port.empty() || baud_rate <= 0) return false;
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_) return false;
    }
    control_->connect(std::move(port), baud_rate, address);
    return true;
}

bool KvmSession::disconnect_control() {
    if (!preview_only()) return false;
    request_release();
    control_->disconnect();
    return true;
}

void KvmSession::set_host_key(std::uint16_t usage) noexcept { input_.set_host_key(usage); }
void KvmSession::set_relative_gain(double gain) noexcept { input_.set_relative_gain(gain); }
bool KvmSession::send_special(SpecialKeys keys) { return video_fresh_ && input_.send_special(keys); }

bool KvmSession::set_mouse_mode(MouseMode mode) {
    if (!preview_only()) return false;
    request_release();
    input_.set_mouse_mode(mode);
    control_->set_mouse_mode(mode);
    return true;
}

void KvmSession::set_video_rect(Rect rect) noexcept { input_.set_video_rect(rect); }
void KvmSession::handle_input(const InputEvent& event) {
    control_->set_control_active(input_.captured() || input_.special_active());
    input_.handle(event);
}

void KvmSession::request_release() noexcept {
    input_.release();
    control_->set_control_active(false);
    control_->release_all();
}

void KvmSession::focus_lost() noexcept { request_release(); input_.focus_lost(); }
void KvmSession::minimized() noexcept { request_release(); input_.minimized(); }
void KvmSession::suspended() noexcept { request_release(); input_.focus_lost(); }
void KvmSession::release_control() noexcept { request_release(); }

void KvmSession::update_ui_heartbeat() noexcept { control_->update_ui_heartbeat(); }

void KvmSession::tick(Clock::time_point now) {
    control_->update_ui_heartbeat();
    const auto capture = capture_->snapshot();
    bool release = false;
    bool fresh = false;
    {
        std::lock_guard lock(mutex_);
        if (capture.generation != observed_generation_) {
            release = observed_generation_ != 0;
            observed_generation_ = capture.generation;
            observed_samples_ = capture.received_samples;
            last_sample_arrival_ = capture.state == CaptureState::streaming ? now : Clock::time_point{};
            received_current_generation_ = false;
            video_fresh_ = false;
            video_.set_generation(capture.generation);
        }
        if (capture.state == CaptureState::streaming &&
            observed_capture_state_ != CaptureState::streaming) {
            last_sample_arrival_ = now;
        }
        observed_capture_state_ = capture.state;
        if (capture.state == CaptureState::streaming &&
            (!observed_actual_mode_ || *observed_actual_mode_ != capture.actual_mode)) {
            release = observed_actual_mode_.has_value();
            observed_actual_mode_ = capture.actual_mode;
            video_.set_generation(capture.generation);
            video_fresh_ = false;
            received_current_generation_ = false;
            last_sample_arrival_ = now;
        }
        if (capture.received_samples != observed_samples_) {
            observed_samples_ = capture.received_samples;
            last_sample_arrival_ = now;
            received_current_generation_ = true;
            video_fresh_ = true;
        }
        fresh = received_current_generation_ && last_sample_arrival_ != Clock::time_point{} &&
                now - last_sample_arrival_ < kStaleAfter &&
                capture.state == CaptureState::streaming;
        if (video_fresh_ && !fresh) release = true;
        video_fresh_ = fresh;
        if (capture.state == CaptureState::permission_denied) {
            video_state_ = SessionVideoState::permission_denied;
        } else if (capture.state == CaptureState::fault) {
            video_state_ = SessionVideoState::fault;
        } else if (capture.state == CaptureState::streaming) {
            const bool timed_out = last_sample_arrival_ != Clock::time_point{} &&
                                   now - last_sample_arrival_ >= kStaleAfter;
            video_state_ = fresh ? SessionVideoState::streaming :
                           (timed_out ? SessionVideoState::stale : SessionVideoState::connecting);
        }
    }
    const auto decoded = video_.snapshot();
    input_.set_video_fresh(fresh && decoded.processed_frames > 0 &&
        decoded.latest_arrival != Clock::time_point{} && now - decoded.latest_arrival < kStaleAfter);
    if ((input_.captured() || input_.special_active()) &&
        control_->snapshot().state != ControlConnectionState::ready) release = true;
    if (release) request_release();
    control_->set_control_active(input_.captured() || input_.special_active());
    input_.clear_fault();
    input_.tick(now);
    control_->set_control_active(input_.captured() || input_.special_active());
}

std::optional<VideoFrame> KvmSession::take_latest_frame() {
    auto frame = video_.take_latest_frame();
    if (frame) control_->video_presented(frame->sequence);
    return frame;
}

KvmSessionSnapshot KvmSession::snapshot() const {
    KvmSessionSnapshot value;
    {
        std::lock_guard lock(mutex_);
        value.video_state = video_state_;
        value.video_fresh = video_fresh_;
        value.shutting_down = shutting_down_;
        value.serial_shutdown_timed_out = serial_shutdown_timed_out_;
        value.session_error = session_error_;
    }
    value.capture = capture_->snapshot();
    value.video = video_.snapshot();
    value.control = control_->snapshot();
    value.input_state = input_.state();
    value.pointer = input_.pointer_snapshot();
    return value;
}

void KvmSession::capture_loop() {
    std::size_t retry_index = 0;
    auto retry_at = Clock::time_point::max();
    for (;;) {
        CaptureCommand command;
        std::optional<CaptureMode> requested;
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, kCapturePoll, [this] {
                return command_ != CaptureCommand::none || !capture_tasks_.empty();
            });
            command = command_;
            requested = requested_mode_;
            command_ = CaptureCommand::none;
            if (!capture_tasks_.empty()) {
                task = std::move(capture_tasks_.front());
                capture_tasks_.pop_front();
            }
        }
        if (task) task();
        if (command == CaptureCommand::shutdown) {
            capture_->stop();
            return;
        }
        if (command == CaptureCommand::stop || command == CaptureCommand::start) {
            capture_->stop(); // stops callback publication before the video generation changes
            video_.set_generation(capture_->snapshot().generation + 1);
            if (command == CaptureCommand::stop) {
                std::lock_guard lock(mutex_);
                video_state_ = SessionVideoState::stopped;
                retry_at = Clock::time_point::max();
                continue;
            }
            if (requested) {
                {
                    std::lock_guard lock(mutex_);
                    video_state_ = SessionVideoState::connecting;
                }
                capture_->start(*requested);
                const auto generation = capture_->snapshot().generation;
                video_.set_generation(generation);
                video_.start(generation);
                retry_index = 0;
                retry_at = Clock::time_point::max();
            }
        }

        const auto status = capture_->snapshot();
        if (status.state != CaptureState::fault) continue;
        std::optional<DeviceInfo> device;
        std::optional<CaptureMode> mode;
        {
            std::lock_guard lock(mutex_);
            device = selected_device_;
            mode = selected_mode_;
        }
        if (!device || !mode || device->weak_match) continue;
        const auto now = Clock::now();
        if (retry_at == Clock::time_point::max()) {
            retry_at = now + kReconnectBackoff[retry_index];
            retry_index = std::min(retry_index + 1, kReconnectBackoff.size() - 1);
            continue;
        }
        if (now < retry_at) continue;

        const auto devices = capture_->enumerate_devices();
        std::size_t exact_matches = 0;
        for (const auto& candidate : devices) {
            if (!candidate.weak_match && candidate.stable_id == device->stable_id) ++exact_matches;
        }
        if (exact_matches != 1) {
            retry_at = now + kReconnectBackoff[retry_index];
            retry_index = std::min(retry_index + 1, kReconnectBackoff.size() - 1);
            continue;
        }
        capture_->stop();
        capture_->start(*mode);
        const auto generation = capture_->snapshot().generation;
        video_.set_generation(generation);
        retry_at = Clock::time_point::max();
    }
}

void KvmSession::shutdown(std::chrono::milliseconds serial_timeout) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_) return;
        shutting_down_ = true;
        video_state_ = SessionVideoState::stopping;
        video_fresh_ = false;
    }
    request_release();
    input_.set_video_fresh(false);
    {
        std::lock_guard lock(mutex_);
        command_ = CaptureCommand::shutdown;
    }
    wake_.notify_one();
    if (capture_worker_.joinable()) capture_worker_.join();
    video_.stop();

    control_->disconnect();
    const auto deadline = Clock::now() + serial_timeout;
    while (Clock::now() < deadline) {
        if (control_->snapshot().state == ControlConnectionState::disconnected) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool timed_out = control_->snapshot().state != ControlConnectionState::disconnected;
    std::lock_guard lock(mutex_);
    serial_shutdown_timed_out_ = timed_out;
    video_state_ = SessionVideoState::stopped;
}

}  // namespace kvmux

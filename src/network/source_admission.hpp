#pragma once
#include "network/udp_media.hpp"
#include <algorithm>

namespace kvmux::relay {
// Network-owner policy. A latest sample and one evaluated baseline bound storage.
// It changes source admission only; encoder polling and control never wait here.
class SourceAdmission {
public:
    SourceAdmission(std::uint64_t generation, std::chrono::microseconds nominal)
        : generation_(generation), nominal_(nominal), interval_(nominal) {}
    bool feedback(std::uint64_t generation, const MediaStats& stats, MediaTime now) {
        if (generation != generation_ || (latest_ && regressed(stats, *latest_))) return false;
        if (latest_ && !changed(stats, *latest_)) return false;
        const bool first = !latest_;
        latest_ = stats; progress_at_ = now; outage_ = false;
        if (first) { baseline_ = stats; evaluated_at_ = now; }
        return true;
    }
    void poll(MediaTime now) {
        using namespace std::chrono_literals;
        if (!latest_) return;
        if (now - progress_at_ >= 1s) {
            healthy_since_.reset();
            if (!outage_) { slow(); outage_ = true; }
            return;
        }
        if (now - evaluated_at_ < 500ms || !changed(*latest_, baseline_)) return;
        delta_ = {};
        delta_.received_frames = latest_->received_frames - baseline_.received_frames;
        delta_.lost_frames = latest_->lost_frames - baseline_.lost_frames;
        delta_.age_losses = latest_->age_losses - baseline_.age_losses;
        delta_.capacity_losses = latest_->capacity_losses - baseline_.capacity_losses;
        delta_.gap_losses = latest_->gap_losses - baseline_.gap_losses;
        const bool pressure = delta_.lost_frames || delta_.age_losses || delta_.capacity_losses ||
            delta_.gap_losses || (latest_->waiting_idr && !delta_.received_frames);
        baseline_ = *latest_; evaluated_at_ = now;
        if (pressure) { slow(); healthy_since_.reset(); }
        else if (delta_.received_frames) {
            if (!healthy_since_) healthy_since_ = now;
            if (now - *healthy_since_ >= 2s && now - recovered_at_ >= 1s) {
                interval_ = std::max(nominal_, interval_ * 9 / 10); recovered_at_ = now;
            }
        }
    }
    std::chrono::microseconds interval() const { return interval_; }
    const std::optional<MediaStats>& latest() const { return latest_; }
    MediaStats delta() const { return delta_; }
private:
    static bool regressed(const MediaStats& a, const MediaStats& b) {
        return a.received_frames < b.received_frames || a.recovered_fragments < b.recovered_fragments ||
            a.recovered_frames < b.recovered_frames || a.lost_frames < b.lost_frames ||
            a.age_losses < b.age_losses || a.capacity_losses < b.capacity_losses ||
            a.gap_losses < b.gap_losses || a.unrecoverable < b.unrecoverable || a.last_completed < b.last_completed;
    }
    static bool changed(const MediaStats& a, const MediaStats& b) {
        return regressed(b, a) || a.waiting_idr != b.waiting_idr;
    }
    void slow() {
        interval_ = std::min(std::max(nominal_, std::chrono::microseconds(200000)), interval_ * 5 / 4);
    }
    std::uint64_t generation_{};
    std::chrono::microseconds nominal_, interval_;
    std::optional<MediaStats> latest_;
    MediaStats baseline_{}, delta_{};
    MediaTime progress_at_{}, evaluated_at_{}, recovered_at_{};
    std::optional<MediaTime> healthy_since_;
    bool outage_{};
};
} // namespace kvmux::relay

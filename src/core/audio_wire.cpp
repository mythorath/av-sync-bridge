// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_wire.hpp"

#include <limits>
#include <stdexcept>

namespace avsync::wire {
namespace {
constexpr Nanoseconds maximum_anchor_age = 250'000'000;
constexpr Nanoseconds maximum_future = 100'000'000;

bool valid_record(const AudioRecord& record) noexcept
{
    return record.epoch.valid() && record.clock_epoch != 0 &&
        record.source_rate >= 8'000 && record.source_rate <= 384'000 &&
        record.capture_ns >= 0 && record.calibration_revision != 0 &&
        record.qpc_100ns <= static_cast<std::uint64_t>(std::numeric_limits<Nanoseconds>::max() / 100) &&
        nominal_wire_position(record.device_position, record.device_origin, 0, record.source_rate).has_value();
}

void put(EncodedAudioRecord& bytes, std::size_t offset, std::size_t width, std::uint64_t value) noexcept
{
    for (std::size_t i = 0; i < width; ++i)
        bytes[offset + i] = static_cast<std::byte>((value >> ((width - i - 1) * 8)) & 0xffU);
}

std::uint64_t get(std::span<const std::byte> bytes, std::size_t offset, std::size_t width) noexcept
{
    std::uint64_t value{};
    for (std::size_t i = 0; i < width; ++i)
        value = (value << 8) | std::to_integer<unsigned>(bytes[offset + i]);
    return value;
}

bool same_anchor(const AudioRecord& a, const AudioRecord& b) noexcept
{
    // Generation-level fields are checked separately. Packet start is not part
    // of anchor identity; every other selected-anchor value is immutable.
    return a.anchor_sequence == b.anchor_sequence && a.device_position == b.device_position &&
        a.capture_ns == b.capture_ns && a.qpc_100ns == b.qpc_100ns &&
        a.calibration_revision == b.calibration_revision;
}
} // namespace

std::optional<EncodedAudioRecord> encode(const AudioRecord& record) noexcept
{
    if (!valid_record(record)) return std::nullopt;
    EncodedAudioRecord bytes{};
    bytes[0] = std::byte{1};
    put(bytes, 2, 2, audio_record_size);
    put(bytes, 4, 4, record.source_rate);
    put(bytes, 8, 8, record.epoch.session);
    put(bytes, 16, 8, record.epoch.generation);
    put(bytes, 24, 8, record.clock_epoch);
    put(bytes, 32, 8, record.device_origin);
    put(bytes, 40, 4, record.rtp_zero);
    put(bytes, 48, 8, record.packet_wire_start);
    put(bytes, 56, 8, record.anchor_sequence);
    put(bytes, 64, 8, record.device_position);
    put(bytes, 72, 8, static_cast<std::uint64_t>(record.capture_ns));
    put(bytes, 80, 8, record.qpc_100ns);
    put(bytes, 88, 8, record.calibration_revision);
    return bytes;
}

std::optional<AudioRecord> decode(std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() != audio_record_size || bytes[0] != std::byte{1} || bytes[1] != std::byte{0} ||
        get(bytes, 2, 2) != audio_record_size || get(bytes, 44, 4) != 0)
        return std::nullopt;
    const auto capture = get(bytes, 72, 8);
    if (capture > static_cast<std::uint64_t>(std::numeric_limits<Nanoseconds>::max()))
        return std::nullopt;
    AudioRecord record;
    record.source_rate = static_cast<std::uint32_t>(get(bytes, 4, 4));
    record.epoch = {get(bytes, 8, 8), get(bytes, 16, 8)};
    record.clock_epoch = get(bytes, 24, 8);
    record.device_origin = get(bytes, 32, 8);
    record.rtp_zero = static_cast<std::uint32_t>(get(bytes, 40, 4));
    record.packet_wire_start = get(bytes, 48, 8);
    record.anchor_sequence = get(bytes, 56, 8);
    record.device_position = get(bytes, 64, 8);
    record.capture_ns = static_cast<Nanoseconds>(capture);
    record.qpc_100ns = get(bytes, 80, 8);
    record.calibration_revision = get(bytes, 88, 8);
    if (!valid_record(record)) return std::nullopt;
    return record;
}

AudioReceiverValidator::AudioReceiverValidator(std::uint64_t expected_clock_epoch)
    : expected_clock_epoch_(expected_clock_epoch)
{
    if (!expected_clock_epoch) throw std::invalid_argument("expected provider clock epoch must be nonzero");
}

AudioValidationResult AudioReceiverValidator::reject(AudioWireStatus status, bool poison) noexcept
{
    if (poison) faulted_ = true;
    return {status, false, false, std::nullopt};
}

AudioValidationResult AudioReceiverValidator::reject_missing() noexcept
{
    return reject(faulted_ ? AudioWireStatus::requires_reset : AudioWireStatus::malformed);
}

bool AudioReceiverValidator::timing_only_stale(const AudioRecord& record,std::uint32_t ssrc,
        std::uint32_t timestamp,std::uint32_t frames,Nanoseconds now) const noexcept {
    const auto age=checked_sub(now,record.capture_ns);
    if (faulted_ || !latest_ || !age || *age<=maximum_anchor_age) return false;
    if (tracker_ && tracker_->diagnostics().measurements && !tracker_->current_estimate(record.capture_ns))
        return false; // A repeated anchor cannot hide an already disqualified rate.
    // Isolated metadata probe: the real stale packet remains rejected. Using
    // its capture date only bypasses freshness in this COPY, not consistency,
    // ordering, identity or rate qualification. No PCM/timestamp is published.
    auto probe=*this;
    const auto result=probe.observe(record,ssrc,timestamp,frames,record.capture_ns);
    return result.accepted && (!result.estimate || result.estimate->within_correction_limit);
}

AudioValidationResult AudioReceiverValidator::observe(const AudioRecord& record, std::uint32_t ssrc,
    std::uint32_t rtp_timestamp, std::uint32_t payload_frames, Nanoseconds now) noexcept
{
    using S = AudioWireStatus;
    // A stray generation is not a recovery request and cannot poison or replace
    // the active one. Global SSRC admission still owns restart coordination.
    if (latest_ && record.epoch != latest_->epoch) return reject(S::wrong_epoch, false);
    if (latest_ && ssrc != ssrc_) return reject(S::wrong_ssrc, false);
    if (faulted_) return reject(S::requires_reset);
    if (!valid_record(record) || now < 0 || payload_frames == 0 || payload_frames > maximum_audio_payload_frames)
        return reject(S::malformed);
    if (record.clock_epoch != expected_clock_epoch_) return reject(S::wrong_clock);
    if (latest_ && (record.source_rate != latest_->source_rate ||
                    record.device_origin != latest_->device_origin || record.rtp_zero != latest_->rtp_zero))
        return reject(S::changed_format);
    if (!latest_ && (record.packet_wire_start != 0 || record.anchor_sequence != 0 ||
                     record.device_position != record.device_origin))
        return reject(S::bad_start);
    if (record.packet_wire_start > std::numeric_limits<std::uint64_t>::max() - payload_frames)
        return reject(S::overflow);
    const auto timestamp = static_cast<std::uint32_t>(static_cast<std::uint64_t>(record.rtp_zero) +
                                                     static_cast<std::uint32_t>(record.packet_wire_start));
    if (rtp_timestamp != timestamp) return reject(S::bad_timestamp);
    if (record.packet_wire_start != next_wire_frame_) return reject(S::frame_gap);

    const auto position = nominal_wire_position(record.device_position, record.device_origin, 0, record.source_rate);
    // valid_record already rejects an unrepresentable rational association.
    if (!position) return reject(S::overflow);
    if (position->whole > record.packet_wire_start ||
        (position->whole == record.packet_wire_start && position->remainder != 0))
        return reject(S::anchor_ahead);
    const auto age = checked_sub(now, record.capture_ns);
    if (!age) return reject(S::overflow);
    if (*age > maximum_anchor_age) return reject(S::stale);
    if (*age < -maximum_future) return reject(S::future);

    const bool repeated = latest_ && record.anchor_sequence == latest_->anchor_sequence;
    if (repeated) {
        if (!same_anchor(record, *latest_)) return reject(S::conflicting_anchor);
        // Do not call observe(), refresh capture age, or advance estimator
        // sequence for a repeated anchor riding on another PCM packet.
    } else if (latest_) {
        if (latest_->anchor_sequence == std::numeric_limits<std::uint64_t>::max() ||
            record.anchor_sequence != latest_->anchor_sequence + 1 ||
            record.qpc_100ns <= latest_->qpc_100ns ||
            record.calibration_revision < latest_->calibration_revision)
            return reject(S::anchor_gap);
    }

    AudioAnchorStatus anchor_status = AudioAnchorStatus::waiting;
    std::optional<RateEstimate> new_estimate;
    if (!tracker_) {
        AudioAnchorConfig config;
        config.rate.nominal_rate = record.source_rate;
        config.device_frame_origin = record.device_origin;
        config.wire_frame_origin = 0;
        config.max_anchor_age_ns = maximum_anchor_age;
        config.max_future_ns = maximum_future;
        // All configuration values have been checked or are positive constants;
        // construction has no dynamic allocation and cannot fail these checks.
        tracker_.emplace(record.epoch, config);
    }
    if (!repeated) {
        const AudioCaptureAnchor anchor{record.epoch, record.anchor_sequence, record.device_position,
                                         record.capture_ns, record.source_rate, false};
        const auto observation = tracker_->observe(anchor, now);
        anchor_status = observation.status;
        new_estimate = observation.estimate;
        switch (anchor_status) {
        case AudioAnchorStatus::priming:
        case AudioAnchorStatus::waiting:
        case AudioAnchorStatus::measured: break;
        case AudioAnchorStatus::stale: return reject(S::stale);
        case AudioAnchorStatus::future: return reject(S::future);
        case AudioAnchorStatus::overflow: return reject(S::overflow);
        default: return reject(S::anchor_gap);
        }
    }
    ssrc_ = ssrc;
    latest_ = record;
    next_wire_frame_ = record.packet_wire_start + payload_frames;
    return {repeated ? S::repeated : anchor_status == AudioAnchorStatus::measured ? S::measured : S::accepted,
             true, repeated, new_estimate};
}

std::optional<RateEstimate> AudioReceiverValidator::current_estimate(Nanoseconds now) const noexcept
{
    return !faulted_ && tracker_ ? tracker_->current_estimate(now) : std::nullopt;
}

AudioStreamAdmission::AudioStreamAdmission(std::uint64_t expected_clock_epoch,
        std::optional<std::uint64_t> expected_sender_session)
    : expected_clock_epoch_(expected_clock_epoch), expected_sender_session_(expected_sender_session)
{
    if (!expected_clock_epoch) throw std::invalid_argument("expected provider clock epoch must be nonzero");
    if (expected_sender_session && !*expected_sender_session)
        throw std::invalid_argument("expected sender session must be nonzero");
}

bool AudioStreamAdmission::admit(const AudioRecord& record, std::uint32_t ssrc) noexcept
{
    if (!ssrc || !valid_record(record) || record.clock_epoch != expected_clock_epoch_) return false;
    if (expected_sender_session_ && record.epoch.session != *expected_sender_session_) return false;
    if (active_epoch_) {
        if (record.epoch == *active_epoch_) return ssrc == active_ssrc_;
        if (record.epoch.session != active_epoch_->session ||
            record.epoch.generation <= active_epoch_->generation)
            return false;
    }
    if (record.packet_wire_start != 0 || record.anchor_sequence != 0 ||
        record.device_position != record.device_origin || count_ == history_.size())
        return false;
    for (std::size_t i = 0; i < count_; ++i) if (history_[i] == ssrc) return false;
    history_[count_++] = ssrc;
    active_epoch_ = record.epoch;
    active_ssrc_ = ssrc;
    return true;
}

} // namespace avsync::wire

// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_wire.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace {
using namespace avsync;
using namespace avsync::wire;
using Status = AudioWireStatus;
unsigned checks{};
void check(bool result, const char* expression, int line)
{
    ++checks;
    if (!result) throw std::runtime_error(std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

AudioRecord origin(std::uint32_t rate = 192'000)
{
    return {{7, 1}, 31, 1'000'000, rate, 0xffffffe0U,
            0, 0, 1'000'000, 10'000'000'000LL, 100'000'000, 1};
}
std::uint32_t timestamp(const AudioRecord& record)
{
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(record.rtp_zero) +
                                      static_cast<std::uint32_t>(record.packet_wire_start));
}
AudioValidationResult accept(AudioReceiverValidator& validator, const AudioRecord& record,
                              std::uint32_t frames = 180, Nanoseconds now = 10'000'000'000LL)
{
    return validator.observe(record, 99, timestamp(record), frames, now);
}
void advance(AudioReceiverValidator& validator, AudioRecord& record, std::uint64_t target)
{
    while (record.packet_wire_start < target) {
        const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(180, target - record.packet_wire_start));
        CHECK(accept(validator, record, frames).accepted);
        record.packet_wire_start += frames;
    }
}

void codec()
{
    AudioRecord record{{0x0102030405060708ULL, 0x1112131415161718ULL}, 0x2122232425262728ULL,
        0x3132333435363738ULL, 192'000, 0x41424344U, 0x5152535455565758ULL,
        0x6162636465666768ULL, 0x7172737475767778ULL, 0x0102030405060708LL,
        0x0001020304050607ULL, 0x8182838485868788ULL};
    constexpr std::array<unsigned char, 96> golden{
        0x01,0x00,0x00,0x60,0x00,0x02,0xee,0x00,
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
        0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
        0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,
        0x41,0x42,0x43,0x44,0x00,0x00,0x00,0x00,
        0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,
        0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,
        0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88};
    const auto encoded = encode(record);
    CHECK(encoded);
    for (std::size_t i = 0; i < golden.size(); ++i)
        CHECK(std::to_integer<unsigned char>((*encoded)[i]) == golden[i]);
    CHECK(decode(*encoded) == record);
    for (std::size_t size = 0; size < 96; ++size) CHECK(!decode(std::span(*encoded).first(size)));
    std::array<std::byte, 97> oversized{};
    std::copy(encoded->begin(), encoded->end(), oversized.begin());
    CHECK(!decode(oversized));
    for (const auto index : {0U, 1U, 2U, 3U, 44U, 45U, 46U, 47U}) {
        auto bad = *encoded;
        bad[index] ^= std::byte{0x40};
        CHECK(!decode(bad));
    }
    auto bad = *encoded;
    bad[72] = std::byte{0x80};
    CHECK(!decode(bad));
    for (auto index : {8U, 16U, 24U, 88U}) {
        bad = *encoded;
        std::fill(bad.begin() + index, bad.begin() + index + 8, std::byte{0});
        CHECK(!decode(bad));
    }
    auto invalid = origin(); invalid.capture_ns = -1; CHECK(!encode(invalid));
    invalid = origin(); invalid.epoch.session = 0; CHECK(!encode(invalid));
    invalid = origin(); invalid.epoch.generation = 0; CHECK(!encode(invalid));
    invalid = origin(); invalid.clock_epoch = 0; CHECK(!encode(invalid));
    invalid = origin(); invalid.calibration_revision = 0; CHECK(!encode(invalid));
    invalid = origin(); invalid.source_rate = 7999; CHECK(!encode(invalid));
    invalid = origin(); invalid.source_rate = 384001; CHECK(!encode(invalid));
    invalid = origin(); invalid.device_position = invalid.device_origin - 1; CHECK(!encode(invalid));
    invalid = origin(8000); invalid.device_origin = 0;
    invalid.device_position = std::numeric_limits<std::uint64_t>::max(); CHECK(!encode(invalid));
    invalid = origin(); invalid.qpc_100ns = std::numeric_limits<std::uint64_t>::max(); CHECK(!encode(invalid));
    invalid = origin(); invalid.capture_ns = std::numeric_limits<Nanoseconds>::max();
    invalid.qpc_100ns = static_cast<std::uint64_t>(std::numeric_limits<Nanoseconds>::max() / 100);
    CHECK(decode(*encode(invalid)) == invalid);
}

void ordered_packets()
{
    AudioReceiverValidator validator(31);
    auto record = origin();
    CHECK(!validator.admitted());
    auto result = accept(validator, record);
    CHECK(result.accepted && !result.repeated && !result.estimate);
    CHECK(validator.admitted());
    CHECK(validator.next_wire_frame() == 180);
    record.packet_wire_start = 180;
    CHECK(timestamp(record) == 148); // 32-bit RTP wrap is ordinary progression.
    result = accept(validator, record);
    CHECK(result.accepted && result.repeated && result.status == Status::repeated);
    CHECK(validator.latest_record() == record);
    record.packet_wire_start = 360;
    CHECK(accept(validator, record, 1).accepted);
    record.packet_wire_start = 361;
    CHECK(accept(validator, record, 179).accepted);
    record.packet_wire_start = 540;
    auto stray = record; ++stray.epoch.generation;
    CHECK(accept(validator, stray).status == Status::wrong_epoch);
    stray = record; ++stray.epoch.session;
    CHECK(accept(validator, stray).status == Status::wrong_epoch);
    CHECK(validator.observe(record, 100, timestamp(record), 180, record.capture_ns).status == Status::wrong_ssrc);
    CHECK(!validator.faulted());
    CHECK(accept(validator, record).accepted);
    CHECK(validator.reject_missing().status == Status::malformed);
    CHECK(validator.faulted());
    record.packet_wire_start += 180;
    CHECK(accept(validator, record).status == Status::requires_reset);
    CHECK(!validator.current_estimate(record.capture_ns));
}

template<class Mutation> void second_packet_fault(Mutation mutation, Status expected)
{
    AudioReceiverValidator validator(31);
    auto record = origin();
    CHECK(accept(validator, record).accepted);
    record.packet_wire_start = 180;
    mutation(record);
    CHECK(accept(validator, record).status == expected);
    CHECK(validator.faulted());
}

void faults()
{
    bool thrown{};
    try { AudioReceiverValidator invalid(0); } catch (const std::invalid_argument&) { thrown = true; }
    CHECK(thrown);
    for (auto frames : {0U, 181U, std::numeric_limits<std::uint32_t>::max()}) {
        AudioReceiverValidator validator(31);
        CHECK(accept(validator, origin(), frames).status == Status::malformed);
        CHECK(validator.faulted());
    }
    {
        AudioReceiverValidator validator(31);
        auto record = origin(); record.packet_wire_start = 1;
        CHECK(accept(validator, record).status == Status::bad_start);
    }
    {
        AudioReceiverValidator validator(31);
        auto record = origin(); record.anchor_sequence = 1;
        CHECK(accept(validator, record).status == Status::bad_start);
    }
    {
        AudioReceiverValidator validator(31);
        auto record = origin(); ++record.device_position;
        CHECK(accept(validator, record).status == Status::bad_start);
    }
    {
        AudioReceiverValidator validator(31);
        const auto record = origin();
        CHECK(validator.observe(record, 99, timestamp(record) + 1, 180, record.capture_ns).status == Status::bad_timestamp);
    }
    second_packet_fault([](auto& r) { r.clock_epoch = 32; }, Status::wrong_clock);
    second_packet_fault([](auto& r) { r.source_rate = 48'000; }, Status::changed_format);
    second_packet_fault([](auto& r) { --r.device_origin; }, Status::changed_format);
    second_packet_fault([](auto& r) { ++r.rtp_zero; }, Status::changed_format);
    second_packet_fault([](auto& r) { r.packet_wire_start = 181; }, Status::frame_gap);
    second_packet_fault([](auto& r) { r.packet_wire_start = 0; }, Status::frame_gap);
    second_packet_fault([](auto& r) { r.packet_wire_start = 360; }, Status::frame_gap);
    second_packet_fault([](auto& r) { ++r.capture_ns; }, Status::conflicting_anchor);
    second_packet_fault([](auto& r) { ++r.qpc_100ns; }, Status::conflicting_anchor);
    second_packet_fault([](auto& r) { ++r.calibration_revision; }, Status::conflicting_anchor);
    second_packet_fault([](auto& r) { ++r.device_position; }, Status::conflicting_anchor);
    second_packet_fault([](auto& r) { r.anchor_sequence = 2; }, Status::anchor_gap);
    second_packet_fault([](auto& r) { r.packet_wire_start = std::numeric_limits<std::uint64_t>::max(); }, Status::overflow);
    // Exact rational association must not be rounded down into an earlier packet.
    {
        AudioReceiverValidator validator(31);
        auto record = origin(44'100);
        CHECK(accept(validator, record, 1).accepted);
        record.packet_wire_start = 1;
        record.anchor_sequence = 1;
        ++record.device_position;
        record.capture_ns += 22'675;
        record.qpc_100ns += 227;
        CHECK(accept(validator, record, 1).status == Status::anchor_ahead); // 48000/44100 > 1
    }
    {
        AudioReceiverValidator validator(31);
        auto record = origin(44'100);
        CHECK(accept(validator, record, 2).accepted);
        record.packet_wire_start = 2;
        record.anchor_sequence = 1;
        ++record.device_position;
        record.capture_ns += 22'675;
        record.qpc_100ns += 227;
        CHECK(accept(validator, record, 1).accepted);
    }
    for (unsigned mutation = 0; mutation != 6; ++mutation) {
        AudioReceiverValidator validator(31);
        auto record = origin(); record.calibration_revision = 2;
        advance(validator, record, 960);
        record.anchor_sequence = 1;
        record.device_position += 3840;
        record.capture_ns += 20'000'000;
        record.qpc_100ns += 200'000;
        if (mutation == 0) record.qpc_100ns -= 200'000; // Same QPC.
        if (mutation == 1) record.qpc_100ns -= 200'001; // Backward QPC.
        if (mutation == 2) record.calibration_revision = 1;
        if (mutation == 3) record.capture_ns -= 20'000'000;
        if (mutation == 4) record.device_position -= 3840;
        if (mutation == 5) record.anchor_sequence = 3;
        CHECK(accept(validator, record).status == Status::anchor_gap);
        CHECK(validator.faulted());
    }
}

void freshness()
{
    const auto first = origin();
    for (const auto now : {first.capture_ns - 100'000'000, first.capture_ns + 250'000'000}) {
        AudioReceiverValidator validator(31);
        CHECK(accept(validator, first, 180, now).accepted);
    }
    {
        AudioReceiverValidator validator(31);
        CHECK(accept(validator, first, 180, first.capture_ns - 100'000'001).status == Status::future);
    }
    {
        AudioReceiverValidator validator(31);
        CHECK(accept(validator, first, 180, first.capture_ns + 250'000'001).status == Status::stale);
    }
    {
        AudioReceiverValidator validator(31);
        auto record = first;
        CHECK(accept(validator, record).accepted);
        record.packet_wire_start = 180;
        CHECK(accept(validator, record, 180, first.capture_ns + 250'000'000).accepted);
        record.packet_wire_start = 360;
        CHECK(accept(validator, record, 180, first.capture_ns + 250'000'001).status == Status::stale);
        CHECK(validator.faulted()); // Repetition did not refresh age.
    }
    {
        AudioReceiverValidator validator(31);
        CHECK(accept(validator, first, 180, -1).status == Status::malformed);
    }
}

void original_clock_not_nominal()
{
    for (std::uint32_t actual_rate : {191'904U, 192'000U, 192'096U, 192'192U}) {
        AudioReceiverValidator validator(31);
        auto record = origin();
        std::optional<RateEstimate> measured;
        unsigned measurements{}, repeated{};
        for (std::uint64_t frame = 0; frame < 144'000; frame += 160) {
            record.packet_wire_start = frame;
            if (frame % 960 == 0) {
                record.anchor_sequence = frame / 960;
                record.device_position = record.device_origin + frame * 4;
                const auto elapsed = static_cast<Nanoseconds>(frame * 4 * 1'000'000'000ULL / actual_rate);
                record.capture_ns = 10'000'000'000LL + elapsed;
                record.qpc_100ns = 100'000'000 + static_cast<std::uint64_t>(elapsed / 100);
                record.calibration_revision = record.anchor_sequence / 5 + 1;
            }
            const auto arrival = 10'050'000'000LL + static_cast<Nanoseconds>(frame * 4 * 1'000'000'000ULL / actual_rate);
            const auto result = accept(validator, record, 160, arrival);
            CHECK(result.accepted);
            CHECK(result.repeated == (frame % 960 != 0));
            CHECK(result.estimate.has_value() == (result.status == Status::measured));
            if (result.repeated) ++repeated;
            if (result.status == Status::measured) ++measurements;
            if (result.estimate) measured = result.estimate;
        }
        CHECK(repeated == 750);
        CHECK(measurements >= 2);
        if (actual_rate == 192'192) {
            CHECK(measured && !measured->within_correction_limit);
            CHECK(!validator.current_estimate(record.capture_ns)); // Diagnostics are not correction commands.
            auto repeated_bad_rate=record; repeated_bad_rate.packet_wire_start+=160;
            auto probe=validator;
            const auto repeat=accept(probe,repeated_bad_rate,160,record.capture_ns);
            CHECK(repeat.accepted && !repeat.estimate);
            CHECK(!validator.timing_only_stale(repeated_bad_rate,99,timestamp(repeated_bad_rate),160,
                record.capture_ns+250'000'001));
        } else {
            CHECK(measured);
            const double expected_ppm = (static_cast<double>(actual_rate) / 192'000 - 1.0) * 1'000'000;
            CHECK(std::abs(measured->source_rate_error_ppm - expected_ppm) < 0.01);
        }
        // Passing stale now to the accessor cannot keep an earlier estimate alive.
        CHECK(!validator.current_estimate(record.capture_ns + 250'000'001));
    }
}

void admission()
{
    AudioStreamAdmission admission(31);
    auto record = origin();
    CHECK(!admission.active_epoch());
    CHECK(!admission.active_ssrc());
    CHECK(!admission.admit(record, 0));
    auto bad = record; bad.clock_epoch = 32;
    CHECK(!admission.admit(bad, 99));
    bad = record; bad.packet_wire_start = 180;
    CHECK(!admission.admit(bad, 99));
    CHECK(admission.admitted_ssrc_count() == 0);
    CHECK(admission.admit(record, 99));
    CHECK(admission.active_epoch() == record.epoch);
    CHECK(admission.active_ssrc() == 99);
    record.packet_wire_start = 180;
    CHECK(admission.admit(record, 99));
    CHECK(!admission.admit(record, 100));
    CHECK(admission.admitted_ssrc_count() == 1);
    auto next = origin(); next.epoch.generation = 3;
    CHECK(!admission.admit(next, 99)); // Cannot reuse the current SSRC.
    CHECK(admission.admit(next, 100));
    CHECK(!admission.admit(record, 99)); // Delayed old generation cannot switch back.
    CHECK(!admission.admit(record, 101));
    auto foreign = next; ++foreign.epoch.session;
    CHECK(!admission.admit(foreign, 101));
    CHECK(admission.active_epoch() == next.epoch);
    next.epoch.generation = 4;
    CHECK(!admission.admit(next, 99)); // A retired SSRC remains retired.
    CHECK(admission.admit(next, 101));
    for (std::uint32_t ssrc = 102; ssrc <= 106; ++ssrc) {
        ++next.epoch.generation;
        CHECK(admission.admit(next, ssrc));
    }
    CHECK(admission.admitted_ssrc_count() == AudioStreamAdmission::maximum_ssrcs);
    const auto last = next;
    ++next.epoch.generation;
    CHECK(!admission.admit(next, 107));
    CHECK(admission.active_epoch() == last.epoch);
    CHECK(admission.admit(last, 106)); // Capacity does not stop the existing generation.
    bool thrown{};
    try { AudioStreamAdmission invalid(0); } catch (const std::invalid_argument&) { thrown = true; }
    CHECK(thrown);
    thrown = false;
    try { AudioStreamAdmission invalid(31, 0); } catch (const std::invalid_argument&) { thrown = true; }
    CHECK(thrown);

    // A supervisor pins identity BEFORE the first packet. A foreign process
    // cannot take first-packet ownership, nor consume the generation budget.
    AudioStreamAdmission pinned(31, 7);
    auto expected = origin(), unexpected = expected;
    unexpected.epoch.session = 8;
    CHECK(!pinned.admit(unexpected, 99));
    CHECK(!pinned.active_epoch() && pinned.admitted_ssrc_count() == 0);
    unexpected = expected; unexpected.clock_epoch = 32;
    CHECK(!pinned.admit(unexpected, 99));
    CHECK(pinned.admit(expected, 99));
    ++expected.epoch.generation;
    CHECK(pinned.admit(expected, 100));
    CHECK(!pinned.admit(origin(), 99));
    unexpected = expected; ++unexpected.epoch.session; ++unexpected.epoch.generation;
    CHECK(!pinned.admit(unexpected, 101));
    CHECK(pinned.active_epoch() == expected.epoch && pinned.admitted_ssrc_count() == 2);
}
void stale_retirement_classification() {
    auto record=origin(); AudioReceiverValidator validator(record.clock_epoch);
    CHECK(accept(validator,record).accepted); record.packet_wire_start=180;
    const auto late=record.capture_ns+250'000'001;
    CHECK(validator.timing_only_stale(record,99,timestamp(record),180,late));
    CHECK(!validator.faulted() && validator.next_wire_frame()==180);
    CHECK(!validator.timing_only_stale(record,99,timestamp(record),180,late-1));
    for (unsigned bad=0;bad<8;++bad) {
        auto invalid=record;
        if (bad==0) ++invalid.epoch.session;
        if (bad==1) ++invalid.epoch.generation;
        if (bad==2) ++invalid.clock_epoch;
        if (bad==3) ++invalid.qpc_100ns; // Conflicting repetition, despite plausible stale age.
        if (bad==4) --invalid.capture_ns;
        if (bad==5) ++invalid.anchor_sequence;
        if (bad==6) ++invalid.packet_wire_start;
        if (bad==7) ++invalid.device_position;
        CHECK(!validator.timing_only_stale(invalid,99,timestamp(invalid),180,late));
    }
    CHECK(!validator.timing_only_stale(record,100,timestamp(record),180,late));
    CHECK(!validator.timing_only_stale(record,99,timestamp(record)+1,180,late));
    CHECK(!validator.timing_only_stale(record,99,timestamp(record),0,late));
    CHECK(accept(validator,record,180,late).status==Status::stale);
    CHECK(!validator.timing_only_stale(record,99,timestamp(record),180,late));
}
} // namespace

int main()
{
    try {
        codec();
        ordered_packets();
        faults();
        freshness();
        original_clock_not_nominal();
        admission();
        stale_retirement_classification();
        std::cout << "audio wire checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio wire test failed: " << error.what() << '\n';
        return 1;
    }
}

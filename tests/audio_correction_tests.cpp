// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_correction.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks{};
void check(bool okay, int line) { ++checks; if (!okay) throw std::runtime_error("correction check line " + std::to_string(line)); }
#define CHECK(x) check(static_cast<bool>(x), __LINE__)
constexpr avsync::SessionToken epoch{3,1};
constexpr std::uint64_t clock_epoch = 77;
constexpr std::uint32_t zero = 0xfffffff0U, ssrc = 9;
struct Feed {
    std::uint64_t frame{};
    double ppm{100};
    avsync::SessionToken token{epoch};
    avsync::Nanoseconds now{};
    avsync::Nanoseconds timestamp_bias{};
    std::array<float,360> pcm{};
    avsync::wire::AudioRecord record() const {
        avsync::wire::AudioRecord r;
        r.epoch = token; r.clock_epoch = clock_epoch; r.source_rate = 192000;
        r.device_origin = 1234; r.rtp_zero = zero; r.packet_wire_start = frame;
        r.anchor_sequence = frame / 960; r.device_position = r.device_origin + r.anchor_sequence * 3840;
        r.capture_ns = 1'000'000'000 + static_cast<avsync::Nanoseconds>(std::floor(
            r.anchor_sequence * 960 * 1e9L / (48000.L * (1 + ppm / 1e6L))));
        r.capture_ns += timestamp_bias;
        r.qpc_100ns = static_cast<std::uint64_t>(r.capture_ns / 100); r.calibration_revision = 1;
        return r;
    }
    auto push(avsync::AudioCorrectionWorker& worker, unsigned frames = 180, bool silence = false) {
        now = 1'000'000'000 + static_cast<avsync::Nanoseconds>(std::floor(
            (frame + frames) * 1e9L / (48000.L * (1 + ppm / 1e6L)))) + 2'000'000;
        for (unsigned i = 0; i < frames; ++i) {
            pcm[2*i] = silence ? 0 : static_cast<float>(0.5L * std::sin(2*std::numbers::pi_v<long double>*
                1000 * (frame+i)/(48000.L*(1+ppm/1e6L)))); pcm[2*i+1] = 0;
        }
        auto result = worker.push(record(), ssrc, static_cast<std::uint32_t>(zero+frame),
            std::span(pcm).first(frames*2), now, true);
        frame += frames; return result;
    }
};
void prime(avsync::AudioCorrectionWorker& w, Feed& f) {
    while (w.state() == avsync::CorrectionState::priming && f.frame < 180000) {
        const auto result = f.push(w);
        CHECK(result == avsync::CorrectionPush::priming_discard || result == avsync::CorrectionPush::accepted);
        CHECK(w.diagnostics().produced_frames == 0);
    }
    CHECK(w.state() == avsync::CorrectionState::running);
    CHECK(w.diagnostics().estimator_windows == 3);
    CHECK(w.diagnostics().priming_discarded_frames == w.origin_wire_frame());
}
std::vector<float> run(bool varied, bool partial_reads) {
    auto w = std::make_unique<avsync::AudioCorrectionWorker>(epoch, clock_epoch);
    Feed f; std::vector<float> samples; std::uint64_t delivered{};
    constexpr std::array<unsigned,6> pattern{1,17,180,71,149,32}; unsigned n{};
    std::array<float,960> out{};
    while (f.frame < 6 * 48000) {
        const auto result = f.push(*w, varied ? pattern[n++%pattern.size()] : 180);
        CHECK(result != avsync::CorrectionPush::rejected);
        const auto produced = w->dispatch(f.now, true);
        CHECK(produced <= 3840 && produced%480 == 0);
        while (const auto block = w->pull(std::span(out).first(partial_reads ? 34 : 960), f.now, true)) {
            CHECK(block->epoch == epoch && block->first_frame == delivered);
            CHECK(block->capture_grid_ns == avsync::RationalTimeline(*w->origin_ns(),48000).at(delivered));
            samples.insert(samples.end(), out.begin(), out.begin()+block->frames*2);
            delivered += block->frames;
        }
        const auto old = w->command_ppm();
        for (int i=0;i<3;++i) CHECK(w->dispatch(f.now,true) == 0);
        CHECK(old == w->command_ppm());
    }
    CHECK(w->state() == avsync::CorrectionState::running);
    CHECK(w->diagnostics().peak_input_frames <= w->input_capacity);
    CHECK(w->diagnostics().peak_output_frames <= w->output_capacity);
    CHECK(w->diagnostics().maximum_command_step_ppm <= .99 + 1e-10);
    CHECK(delivered > 120000);
    CHECK(w->diagnostics().produced_frames == delivered);
    return samples;
}
void faults() {
    auto w = std::make_unique<avsync::AudioCorrectionWorker>(epoch, clock_epoch); Feed f;
    prime(*w,f);
    auto wrong = f.record(); wrong.epoch.generation++;
    CHECK(w->push(wrong,ssrc,static_cast<std::uint32_t>(zero+f.frame),f.pcm,f.now,true) == avsync::CorrectionPush::wrong_epoch);
    CHECK(w->state() == avsync::CorrectionState::running);
    CHECK(!w->reset(epoch)); CHECK(w->state() == avsync::CorrectionState::running);
    CHECK(w->dispatch(f.now+251'000'000,true) == 0);
    CHECK(w->fault() == avsync::CorrectionFault::stale);
    CHECK(w->pending_input() == 0 && w->pending_output() == 0);
    CHECK(w->reset({3,2})); f = {}; f.token={3,2}; prime(*w,f);
    while (!w->pending_output()) { CHECK(f.push(*w) == avsync::CorrectionPush::accepted); (void)w->dispatch(f.now,true); }
    CHECK(w->dispatch(f.now,false) == 0 && w->fault() == avsync::CorrectionFault::health);
    CHECK(w->pending_input() == 0 && w->pending_output() == 0);
    CHECK(w->reset({3,3})); f = {}; f.token={3,3};
    prime(*w,f);
    while (w->state() == avsync::CorrectionState::running) (void)f.push(*w);
    CHECK(w->fault() == avsync::CorrectionFault::overflow || w->fault() == avsync::CorrectionFault::stale);
    CHECK(w->pending_input() == 0);
    CHECK(w->reset({3,4})); f = {}; f.token={3,4};
    CHECK(f.push(*w) == avsync::CorrectionPush::priming_discard);
    f.frame += 180; CHECK(f.push(*w) == avsync::CorrectionPush::rejected);
    CHECK(w->fault() == avsync::CorrectionFault::metadata);
    CHECK(w->reset({3,5})); f = {}; f.token={3,5}; f.pcm[0]=std::numeric_limits<float>::quiet_NaN();
    CHECK(w->push(f.record(),ssrc,zero,f.pcm,1'002'000'000,true) == avsync::CorrectionPush::rejected);
    CHECK(w->fault() == avsync::CorrectionFault::pcm);
    CHECK(w->reset({3,6})); f = {}; f.token={3,6}; f.ppm=600;
    while (w->state() == avsync::CorrectionState::priming) (void)f.push(*w);
    CHECK(w->fault() == avsync::CorrectionFault::rate);
    // No old waveform survives filter/queue reset; priming silence is valid.
    CHECK(w->reset({3,7})); f = {}; f.token={3,7}; std::array<float,960> out{}; unsigned count{};
    while (f.frame < 5*48000) {
        CHECK(f.push(*w,180,true) != avsync::CorrectionPush::rejected); (void)w->dispatch(f.now,true);
        while (const auto b=w->pull(out,f.now,true)) {
            CHECK(std::all_of(out.begin(),out.begin()+b->frames*2,[](float x){return x==0;})); ++count;
        }
    }
    CHECK(count > 100);
}
void backpressure_and_ownership() {
    auto w=std::make_unique<avsync::AudioCorrectionWorker>(epoch,clock_epoch); Feed f;
    prime(*w,f);
    // Fill output without reading; processing stops at a full quantum boundary.
    while (w->pending_output()<w->output_capacity) {
        CHECK(f.push(*w)==avsync::CorrectionPush::accepted); (void)w->dispatch(f.now,true);
    }
    const auto command=w->command_ppm(), target=w->target_ppm();
    const auto calls=w->diagnostics().backend_calls;
    for (int i=0;i<10;++i) CHECK(w->dispatch(f.now,true)==0);
    CHECK(w->command_ppm()==command && w->target_ppm()==target && w->diagnostics().backend_calls==calls);
    std::array<float,2> one{};
    CHECK(w->pull(one,f.now,true)->frames==1);
    CHECK(w->dispatch(f.now,true)==0); // One slot is not a full DSP quantum.
    CHECK(w->diagnostics().backend_calls==calls);
    // Output may not be read after its residence deadline, even with no new input.
    CHECK(!w->pull(one,f.now+201'000'000,true));
    CHECK(w->fault()==avsync::CorrectionFault::stale && w->pending_output()==0);
    CHECK(w->reset({3,2})); f={}; f.token={3,2}; prime(*w,f);
    CHECK(w->dispatch(f.now-1,true)==0 && w->fault()==avsync::CorrectionFault::health);
    CHECK(w->reset({3,3})); f={}; f.token={3,3}; prime(*w,f);
    // Copy-in ownership: overwriting caller storage cannot change pending PCM.
    while (!w->pending_output()) {
        CHECK(f.push(*w,180,true)==avsync::CorrectionPush::accepted);
        f.pcm.fill(std::numeric_limits<float>::quiet_NaN());
        (void)w->dispatch(f.now,true);
    }
    CHECK(w->state()==avsync::CorrectionState::running);
    // Reset while both queues contain data; no implicit old-epoch drain.
    CHECK(w->pending_input()>0 && w->pending_output()>0);
    CHECK(w->reset({3,4}));
    CHECK(w->pending_input()==0 && w->pending_output()==0 && !w->origin_ns());
    CHECK(w->diagnostics().received_frames==0 && !w->pull(one,f.now,true));
    CHECK(w->reset({3,5})); f={}; f.token={3,5}; prime(*w,f);
    while (w->pending_input()<w->input_offer)
        CHECK(f.push(*w,static_cast<unsigned>(std::min<std::size_t>(180,w->input_offer-w->pending_input())))
            ==avsync::CorrectionPush::accepted);
    CHECK(w->dispatch(f.now,true)==w->quantum);
    std::array<float,960> quantum{};
    CHECK(w->pull(quantum,f.now,true)->frames==w->quantum);
    CHECK(w->pending_input()==0 && w->pending_output()==0);
    // Private backend history still exists. Empty public queues cannot bypass
    // the no-progress deadline (the latest anchor is less than 250ms old).
    CHECK(w->dispatch(f.now+201'000'000,true)==0);
    CHECK(w->fault()==avsync::CorrectionFault::stale);
}
void phase_faults() {
    auto w=std::make_unique<avsync::AudioCorrectionWorker>(epoch,clock_epoch); Feed f;
    prime(*w,f);
    const auto held=f.record();
    for (unsigned i=0;i<18;++i) {
        auto r=held; r.packet_wire_start=f.frame; f.now+=3'750'000;
        CHECK(w->push(r,ssrc,static_cast<std::uint32_t>(zero+f.frame),f.pcm,f.now,true)==avsync::CorrectionPush::accepted);
        f.frame+=180;
        CHECK(w->dispatch(f.now,true)==0);
    }
    CHECK(w->diagnostics().phase_waits>0 && w->diagnostics().backend_calls==0);
    CHECK(w->diagnostics().produced_frames==0 && w->state()==avsync::CorrectionState::running);
    CHECK(w->reset({3,2})); f={}; f.token={3,2}; prime(*w,f);
    const auto next_anchor=(f.frame/960+1)*960;
    std::array<float,960> out{};
    while (w->state()==avsync::CorrectionState::running && f.frame<next_anchor+12000) {
        if (f.frame>=next_anchor) f.timestamp_bias=11'000'000;
        (void)f.push(*w); (void)w->dispatch(f.now,true);
        while (w->pull(out,f.now,true)) {}
    }
    CHECK(w->fault()==avsync::CorrectionFault::phase);
    CHECK(w->diagnostics().maximum_predicted_phase_ns>10'000'000);
    CHECK(w->pending_input()==0 && w->pending_output()==0);
    CHECK(!w->pull(out,f.now,true) && w->dispatch(f.now,true)==0);
    CHECK(f.push(*w)==avsync::CorrectionPush::rejected);
    CHECK(w->reset({3,3})); f={}; f.token={3,3}; prime(*w,f);
    CHECK(w->diagnostics().maximum_predicted_phase_ns==0 && w->diagnostics().phase_checks==0);
}
}
int main() {
    try {
        const auto full = run(false,false), partial = run(false,true);
        CHECK(full == partial); // Output-reader partitions cannot change DSP.
        (void)run(true,true); // Input partition changes the post-prime origin.
        faults();
        backpressure_and_ownership();
        phase_faults();
        std::cout << checks << " correction worker checks passed; generated PCM only\n"; return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

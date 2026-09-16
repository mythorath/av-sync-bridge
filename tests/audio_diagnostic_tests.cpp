// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_diagnostic.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
unsigned checks{};
void check(bool okay,int line) { ++checks; if (!okay) throw std::runtime_error("diagnostic check line "+std::to_string(line)); }
#define CHECK(x) check(static_cast<bool>(x),__LINE__)
avsync::DiagnosticAudioPacket packet(std::uint64_t wire, std::uint64_t generation=1,
        avsync::Nanoseconds base=1'000'000'000,unsigned frames=180,bool sr=true) {
    avsync::DiagnosticAudioPacket p;
    auto& r=p.record; r.epoch={9,generation}; r.clock_epoch=7;
    r.device_origin=1234; r.source_rate=192000; r.rtp_zero=0xfffffff0U;
    r.packet_wire_start=wire; r.anchor_sequence=wire/960; r.device_position=1234+r.anchor_sequence*3840;
    r.capture_ns=base+static_cast<avsync::Nanoseconds>(std::floor(r.anchor_sequence*960*1e9L/48004.8L));
    r.qpc_100ns=static_cast<std::uint64_t>(r.capture_ns/100); r.calibration_revision=1;
    p.ssrc=static_cast<std::uint32_t>(generation+10); p.timestamp=static_cast<std::uint32_t>(r.rtp_zero+wire);
    p.frames=frames; p.arrival_ns=base+static_cast<avsync::Nanoseconds>((wire+frames)*1e9L/48004.8L)+2'000'000;
    p.last_report_ns=sr ? p.arrival_ns:-1;
    std::array<std::byte,1080> raw{};
    for (unsigned i=0;i<frames;++i) raw[i*6]=std::byte{0x20}; // Exactly .25 left, silent right.
    CHECK(avsync::decode_l24(std::span(raw).first(frames*6),p));
    return p;
}
avsync::Nanoseconds feed(avsync::AudioCorrectionDiagnostic& d,std::uint64_t generation=1,
        avsync::Nanoseconds base=1'000'000'000,bool reports=true,bool varied=true) {
    constexpr std::array<unsigned,8> counts{1,17,180,71,149,32,113,7};
    std::uint64_t wire{},n{}; avsync::Nanoseconds now{};
    while (wire<6*48000) {
        const auto frames=static_cast<unsigned>(std::min<std::uint64_t>(varied?counts[n%counts.size()]:180,6*48000-wire));
        auto p=packet(wire,generation,base,frames,reports);
        now=p.arrival_ns;
        CHECK(d.submit(p));
        // Ownership: mutating the submitted caller buffer must not affect output.
        p.pcm.fill(1);
        d.tick(now,true);
        CHECK(!d.failed()); wire+=frames; ++n;
    }
    return now;
}
void decode() {
    avsync::DiagnosticAudioPacket p; p.frames=2;
    const std::array<std::byte,12> raw{std::byte{0x80},std::byte{0},std::byte{0},
        std::byte{0x7f},std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff},
        std::byte{0},std::byte{0},std::byte{1}};
    CHECK(avsync::decode_l24(raw,p));
    CHECK(p.pcm[0]==-1.F && p.pcm[1]==8388607.F/8388608.F);
    CHECK(p.pcm[2]==-1.F/8388608.F && p.pcm[3]==1.F/8388608.F);
    CHECK(!avsync::decode_l24(std::span(raw).first(11),p));
    p.frames=0; CHECK(!avsync::decode_l24({},p));
    p.frames=181; CHECK(!avsync::decode_l24(raw,p));
}
void verdicts() {
    using avsync::CorrectionState; using avsync::CorrectionFault;
    std::array<avsync::DiagnosticAudioSession,3> s{};
    for (auto& x:s) { x.state=CorrectionState::running; x.delivered_frames=48000; }
    CHECK(avsync::correction_diagnostic_pass(std::span(s).first(1),false));
    CHECK(!avsync::correction_diagnostic_pass(std::span(s).first(1),true));
    CHECK(!avsync::correction_diagnostic_pass(std::span(s).first(2),false));
    s[0].state=CorrectionState::faulted; s[0].fault=CorrectionFault::health; s[0].first_fault_ns=1'000'000'000;
    CHECK(avsync::correction_diagnostic_pass(std::span(s).first(2),false,1'000'000'000));
    CHECK(!avsync::correction_diagnostic_pass(s,false,1'000'000'000)); // Extra recovery is NOT a pass.
    CHECK(!avsync::correction_diagnostic_pass(std::span(s).first(2),false,1'000'000'001));
    CHECK(!avsync::correction_diagnostic_pass(std::span(s).first(2),false,899'999'999));
    s[0].fault=CorrectionFault::stale;
    CHECK(!avsync::correction_diagnostic_pass(std::span(s).first(2),false,1'000'000'000));
    s[0].fault=CorrectionFault::health; s[1].fault=CorrectionFault::phase;
    CHECK(!avsync::correction_diagnostic_pass(std::span(s).first(2),false,1'000'000'000));
}
void lifecycle() {
    auto d=std::make_unique<avsync::AudioCorrectionDiagnostic>(7);
    auto now=feed(*d);
    CHECK(d->sessions().size()==1);
    const auto first=d->sessions()[0];
    CHECK(first.state==avsync::CorrectionState::running && first.fault==avsync::CorrectionFault::none);
    CHECK(first.delivered_frames>2*48000 && first.peak<.3 && first.peak>.24);
    CHECK(first.correction.received_frames==6*48000 && first.first_frame==0);
    CHECK(first.dispatch_calls_max<=8 && first.correction.maximum_predicted_phase_ns<1000);
    CHECK(d->queue_peak()==1);
    d->tick(now+1,false);
    CHECK(d->sessions()[0].fault==avsync::CorrectionFault::health);
    d->tick(now+2,true);
    CHECK(d->sessions()[0].delivered_frames==first.delivered_frames);
    now=feed(*d,2,now+1'000'000'000,true,false);
    CHECK(d->sessions().size()==2 && d->sessions()[1].state==avsync::CorrectionState::running);
    CHECK(d->sessions()[1].first_frame==0 && d->sessions()[1].first_capture_ns>first.last_capture_ns);
    auto retired=packet(0,1); retired.arrival_ns=now;
    CHECK(d->submit(retired)); d->tick(now,true);
    CHECK(d->sessions().size()==2 && d->sessions()[1].state==avsync::CorrectionState::running);
    d->tick(now+251'000'000,true);
    CHECK(d->sessions()[1].fault==avsync::CorrectionFault::stale);
}
void failures() {
    auto missing=std::make_unique<avsync::AudioCorrectionDiagnostic>(7);
    feed(*missing,1,1'000'000'000,false);
    CHECK(missing->sessions()[0].fault==avsync::CorrectionFault::health);
    CHECK(!missing->sessions()[0].delivered_frames);
    auto gap=std::make_unique<avsync::AudioCorrectionDiagnostic>(7);
    auto a=packet(0),b=packet(360);
    CHECK(gap->submit(a)); gap->tick(a.arrival_ns,true);
    CHECK(gap->submit(b)); gap->tick(b.arrival_ns,true);
    CHECK(gap->sessions()[0].fault==avsync::CorrectionFault::metadata);
    auto late=std::make_unique<avsync::AudioCorrectionDiagnostic>(7);
    CHECK(late->submit(a)); late->tick(a.arrival_ns+100'000'001,true); CHECK(late->failed());
    auto full=std::make_unique<avsync::AudioCorrectionDiagnostic>(7);
    for (unsigned i=0;i<full->capacity;++i) CHECK(full->submit(a));
    CHECK(!full->submit(a)); full->tick(a.arrival_ns,true);
    CHECK(full->failed() && full->sessions().empty());
    auto backwards=std::make_unique<avsync::AudioCorrectionDiagnostic>(7);
    backwards->tick(100,true); backwards->tick(99,true); CHECK(backwards->failed());
    auto future=std::make_unique<avsync::AudioCorrectionDiagnostic>(7);
    CHECK(future->submit(a)); future->tick(a.arrival_ns-1,true);
    CHECK(future->sessions().empty()); future->tick(a.arrival_ns,true); CHECK(future->sessions().size()==1);
}
}
int main() {
    try { decode(); verdicts(); lifecycle(); failures(); std::cout<<checks<<" diagnostic checks passed\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

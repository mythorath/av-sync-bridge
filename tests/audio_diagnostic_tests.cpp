// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_diagnostic.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
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
void output_hooks() {
    struct Sink {
        std::uint64_t frames{}; unsigned revoked{}; bool reject{};
        static bool consume(void* p,const avsync::CorrectedAudio& b,std::span<const float> pcm,
                avsync::Nanoseconds) noexcept {
            auto& s=*static_cast<Sink*>(p);
            if (s.reject || s.revoked || b.first_frame!=s.frames || pcm.size()!=b.frames*2) return false;
            s.frames+=b.frames; return true;
        }
        static void revoke(void* p) noexcept { ++static_cast<Sink*>(p)->revoked; }
    } sink;
    auto d=std::make_unique<avsync::AudioCorrectionDiagnostic>(7,
        avsync::DiagnosticAudioOutput{&sink,Sink::consume,Sink::revoke});
    auto now=feed(*d); CHECK(sink.frames>48000 && !sink.revoked);
    d->tick(now+1,false); CHECK(d->failed() && sink.revoked);
    sink={};
    d=std::make_unique<avsync::AudioCorrectionDiagnostic>(7,
        avsync::DiagnosticAudioOutput{&sink,Sink::consume,Sink::revoke});
    now=feed(*d);
    auto newer=packet(0,2,now+1'000'000'000);
    CHECK(d->submit(newer)); d->tick(newer.arrival_ns,true);
    CHECK(d->failed() && sink.revoked); // No old buffer stays online while a new generation primes.
    sink={}; sink.reject=true;
    d=std::make_unique<avsync::AudioCorrectionDiagnostic>(7,
        avsync::DiagnosticAudioOutput{&sink,Sink::consume,Sink::revoke});
    for (std::uint64_t wire=0;wire<5*48000 && !d->failed();wire+=180) {
        const auto p=packet(wire); CHECK(d->submit(p)); d->tick(p.arrival_ns,true);
    }
    CHECK(d->failed() && sink.revoked && !sink.frames);
}
struct RecoverySink {
    avsync::SessionToken epoch{};
    std::uint64_t frames{},total_frames{};
    avsync::Nanoseconds last_capture{-1};
    unsigned begins{},revocations{};
    bool revoked{true},reject_begin{};
    static bool begin(void* p,avsync::SessionToken next) noexcept {
        auto& s=*static_cast<RecoverySink*>(p);
        if (s.reject_begin || !s.revoked || !next.valid() ||
            (s.epoch.valid() && (s.epoch.session!=next.session || s.epoch.generation>=next.generation))) return false;
        s.epoch=next; s.frames=0; s.revoked=false; ++s.begins; return true;
    }
    static bool consume(void* p,const avsync::CorrectedAudio& b,std::span<const float> pcm,
            avsync::Nanoseconds) noexcept {
        auto& s=*static_cast<RecoverySink*>(p);
        if (s.revoked || b.epoch!=s.epoch || b.first_frame!=s.frames || pcm.size()!=b.frames*2 ||
            b.capture_grid_ns<=s.last_capture) return false;
        s.last_capture=b.capture_grid_ns; s.frames+=b.frames; s.total_frames+=b.frames; return true;
    }
    static void revoke(void* p) noexcept {
        auto& s=*static_cast<RecoverySink*>(p);
        if (!s.revoked) ++s.revocations;
        s.revoked=true;
    }
    avsync::DiagnosticAudioOutput hooks() { return {this,consume,revoke,begin}; }
};
void recovery() {
    using avsync::DesktopRecovery;
    using avsync::CorrectionFault; using avsync::CorrectionStaleReason;
    CHECK(avsync::desktop_fault_recoverable(CorrectionFault::health,CorrectionStaleReason::none));
    CHECK(avsync::desktop_fault_recoverable(CorrectionFault::stale,CorrectionStaleReason::no_progress));
    CHECK(avsync::desktop_fault_recoverable(CorrectionFault::stale,CorrectionStaleReason::anchor_age));
    for (const auto reason:{CorrectionStaleReason::none,CorrectionStaleReason::input_queue,CorrectionStaleReason::output_queue})
        CHECK(!avsync::desktop_fault_recoverable(CorrectionFault::stale,reason));
    for (const auto fault:{CorrectionFault::metadata,CorrectionFault::pcm,CorrectionFault::rate,CorrectionFault::overflow,
            CorrectionFault::backend,CorrectionFault::timeline,CorrectionFault::phase,CorrectionFault::none})
        CHECK(!avsync::desktop_fault_recoverable(fault,CorrectionStaleReason::no_progress));
    RecoverySink sink;
    auto d=std::make_unique<avsync::AudioCorrectionDiagnostic>(7,sink.hooks(),DesktopRecovery::same_sender_session);
    auto now=feed(*d);
    CHECK(sink.begins==1 && !sink.revoked && sink.frames>48000);
    const auto first_frames=sink.total_frames;
    d->tick(now+1,false);
    CHECK(!d->failed() && d->awaiting_generation() && sink.revoked && sink.revocations==1);
    auto old=packet(6*48000); old.arrival_ns=now+10'000'000;
    CHECK(d->submit(old)); d->tick(old.arrival_ns,true);
    CHECK(!d->failed() && d->awaiting_generation() && sink.total_frames==first_frames);
    CHECK(d->retired_packets()==1 && sink.revocations==1);
    now=feed(*d,2,now+1'000'000'000);
    CHECK(!d->awaiting_generation() && sink.begins==2 && !sink.revoked);
    CHECK(d->sessions().size()==2 && d->sessions()[1].first_frame==0 && sink.total_frames>first_frames);
    // A new admitted generation retires an active predecessor even before its watchdog fires.
    now=feed(*d,3,now+1'000'000'000);
    CHECK(sink.begins==3 && sink.revocations==2 && d->sessions().size()==3);
    old=packet(0,1); old.arrival_ns=now;
    CHECK(d->submit(old)); d->tick(now,true);
    CHECK(!d->failed() && sink.begins==3 && d->retired_packets()==2);
    d->tick(now+201'000'000,true);
    CHECK(!d->failed() && d->awaiting_generation() && sink.revoked);
    CHECK(d->sessions()[2].fault==avsync::CorrectionFault::stale);
    now=feed(*d,4,now+1'000'000'000);
    CHECK(sink.begins==4 && !sink.revoked);
    // Fixed storage/admission budget: eight generations, never an unbounded restart loop.
    for (std::uint64_t generation=5;generation<=8;++generation) now=feed(*d,generation,now+1'000'000'000);
    const auto ninth=packet(0,9,now+1'000'000'000);
    CHECK(d->submit(ninth)); d->tick(ninth.arrival_ns,true);
    CHECK(d->failed() && !d->awaiting_generation() && sink.revoked && sink.begins==8);
    for (unsigned invalid=0;invalid<5;++invalid) {
        sink={};
        d=std::make_unique<avsync::AudioCorrectionDiagnostic>(7,sink.hooks(),DesktopRecovery::same_sender_session);
        now=feed(*d);
        auto next=packet(0,2,now+1'000'000'000);
        if (invalid==0) next.record.epoch.session=10;
        if (invalid==1) next.ssrc=11; // Reused transport identity.
        if (invalid==2) next=packet(180,2,now+1'000'000'000); // Missing frame zero.
        if (invalid==3) sink.reject_begin=true;
        if (invalid==4) next.pcm[0]=std::numeric_limits<float>::quiet_NaN();
        CHECK(d->submit(next)); d->tick(next.arrival_ns,true);
        CHECK(d->failed() && sink.revoked && !d->awaiting_generation());
    }
    bool rejected{};
    try { avsync::AudioCorrectionDiagnostic missing(7,{},DesktopRecovery::same_sender_session); }
    catch (const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);
    sink={};
    d=std::make_unique<avsync::AudioCorrectionDiagnostic>(7,sink.hooks(),DesktopRecovery::same_sender_session);
    const auto first=packet(0); CHECK(d->submit(first)); d->tick(first.arrival_ns,true);
    d->tick(first.record.capture_ns+250'000'001,true);
    CHECK(d->awaiting_generation() && !d->failed() && sink.revoked);
    CHECK(d->sessions()[0].correction.stale_reason==CorrectionStaleReason::anchor_age);
    feed(*d,2,2'000'000'000);
    CHECK(!d->failed() && !d->awaiting_generation() && sink.begins==2 && sink.total_frames>48000);
}
void ordered_receiver_retirement() {
    // Exercise the SAME pre-validator fence and expiry-only classification used
    // by the RTP callback, not just direct submissions into the DSP diagnostic.
    for (const bool watchdog_first : {false,true}) {
        RecoverySink sink;
        avsync::AudioCorrectionDiagnostic d(7,sink.hooks(),avsync::DesktopRecovery::same_sender_session);
        avsync::wire::AudioReceiverValidator branch(7);
        avsync::Nanoseconds now{};
        CHECK(!d.retire_stale_transport({9,1})); // No admitted session yet.
        for (std::uint64_t wire=0;wire<6*48000;wire+=180) {
            const auto p=packet(wire); now=p.arrival_ns;
            CHECK(branch.observe(p.record,p.ssrc,p.timestamp,p.frames,now).accepted);
            CHECK(d.submit(p)); d.tick(now,true); CHECK(!d.failed());
        }
        CHECK(!d.retire_stale_transport({10,1}));
        CHECK(!d.retire_stale_transport({9,2})); // Not admitted, even if generation increases.
        const auto late=packet(6*48000);
        now=late.record.capture_ns+251'000'000;
        if (watchdog_first) {
            d.tick(now,true);
            CHECK(d.retired_generation(late.record.epoch) && d.awaiting_generation());
            // Callback drops BEFORE the validator, which otherwise latches stale.
            auto probe=branch;
            CHECK(probe.observe(late.record,late.ssrc,late.timestamp,late.frames,now).status==
                avsync::wire::AudioWireStatus::stale);
            CHECK(branch.next_wire_frame()==6*48000 && !branch.faulted());
        } else {
            CHECK(!d.retired_generation(late.record.epoch));
            CHECK(branch.timing_only_stale(late.record,late.ssrc,late.timestamp,late.frames,now));
            CHECK(branch.observe(late.record,late.ssrc,late.timestamp,late.frames,now).status==
                avsync::wire::AudioWireStatus::stale);
            CHECK(d.retire_stale_transport(late.record.epoch));
            CHECK(!sink.revoked); // Callback only posts an atomic request; no DSP/IPC I/O.
            d.tick(now,true);
        }
        CHECK(!d.failed() && d.awaiting_generation() && sink.revoked);
        CHECK(d.retired_generation({9,1}) && !d.retired_generation({9,2}) && !d.retired_generation({10,1}));
        const auto old_frames=sink.total_frames;
        auto pending=late; pending.arrival_ns=now-101'000'000;
        CHECK(d.submit(pending)); d.tick(now,true);
        CHECK(!d.failed() && sink.total_frames==old_frames); // A queued retired packet cannot poison recovery.
        avsync::wire::AudioReceiverValidator successor(7);
        const auto base=now+1'000'000'000;
        for (std::uint64_t wire=0;wire<6*48000;wire+=180) {
            const auto p=packet(wire,2,base); now=p.arrival_ns;
            CHECK(successor.observe(p.record,p.ssrc,p.timestamp,p.frames,now).accepted);
            CHECK(d.submit(p)); d.tick(now,true); CHECK(!d.failed());
        }
        CHECK(!d.awaiting_generation() && !sink.revoked && sink.begins==2 && sink.total_frames>old_frames);
        CHECK(d.retired_generation({9,1}) && !d.retired_generation({9,2}));
    }
}
}
int main() {
    try { decode(); verdicts(); lifecycle(); failures(); output_hooks(); recovery(); ordered_receiver_retirement();
        std::cout<<checks<<" diagnostic checks passed\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

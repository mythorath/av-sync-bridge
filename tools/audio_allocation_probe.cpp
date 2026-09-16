// SPDX-License-Identifier: GPL-2.0-or-later
#include "allocation_audit.hpp"
#include "avsync/audio_correction.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
void require(bool okay,const char* text) { if (!okay) throw std::runtime_error(text); }
long double capture(std::uint64_t frame) {
    const auto x=frame/48000.L;
    if (x<=8) return x;
    if (x<=20) return 8+(x-8)/1.000499L;
    if (x<=36) return 8+12/1.000499L+(x-20)/.999501L;
    return 8+12/1.000499L+16/.999501L+x-36;
}
void feed(avsync::AudioCorrectionWorker& w, unsigned seconds, std::uint64_t generation) {
    std::array<float,360> pcm{};
    std::array<float,960> out{};
    constexpr std::array<unsigned,8> counts{1,17,180,71,149,32,113,7};
    std::uint64_t frame{}, packet{};
    avsync::Nanoseconds now{};
    while (frame<static_cast<std::uint64_t>(seconds)*48000) {
        const auto n=static_cast<unsigned>(std::min<std::uint64_t>(counts[packet%counts.size()],seconds*48000ULL-frame));
        avsync::wire::AudioRecord r;
        r.epoch={9,generation}; r.clock_epoch=7; r.source_rate=192000; r.device_origin=1234;
        r.rtp_zero=0xffffff00U; r.packet_wire_start=frame; r.anchor_sequence=frame/960;
        r.device_position=1234+r.anchor_sequence*3840;
        r.capture_ns=1'000'000'000+static_cast<avsync::Nanoseconds>(std::floor(capture(r.anchor_sequence*960)*1e9L));
        r.qpc_100ns=static_cast<std::uint64_t>(r.capture_ns/100); r.calibration_revision=1;
        now=std::max(now,1'002'000'000+static_cast<avsync::Nanoseconds>(std::floor(capture(frame+n)*1e9L))+
                          static_cast<avsync::Nanoseconds>((packet*7919)%4'000'001));
        for (unsigned i=0;i<n;++i) { pcm[i*2]=.125F; pcm[i*2+1]=0; }
        require(w.push(r,11,static_cast<std::uint32_t>(r.rtp_zero+frame),std::span(pcm).first(n*2),now,true)
            !=avsync::CorrectionPush::rejected,"worker rejected generated packet");
        (void)w.dispatch(now,true);
        while (const auto block=w.pull(out,now,true))
            for (std::size_t i=0;i<block->frames;++i) require(std::isfinite(out[i*2]) && out[i*2+1]==0,"invalid PCM");
        require(w.state()!=avsync::CorrectionState::faulted,"worker fault");
        frame+=n; ++packet;
    }
    require(w.diagnostics().delivered_frames>48000,"insufficient correction output");
}
}
int main(int argc,char** argv) {
    try {
        unsigned seconds=45;
        if (argc!=1) {
            require(argc==3 && std::string(argv[1])=="--seconds","expected --seconds 45..600");
            const std::string value=argv[2]; std::size_t used{}; const auto n=std::stoul(value,&used);
            require(used==value.size() && n>=45 && n<=600,"invalid seconds"); seconds=static_cast<unsigned>(n);
        }
        // Warm exception/runtime/loader machinery OUTSIDE the audited worker.
        try { throw std::runtime_error("warmup"); } catch (...) {}
        { auto warm=std::make_unique<avsync::AudioCorrectionWorker>(avsync::SessionToken{9,1},7); }
        require(avsync_audit_begin(0),"audit start failed");
        auto worker=std::make_unique<avsync::AudioCorrectionWorker>(avsync::SessionToken{9,1},7);
        avsync_audit_enable(false);
        const auto initial=avsync_audit_stats();
        require(initial.attempts>=4 && initial.bytes>=sizeof(*worker) && !initial.table_overflow,
                "allocator interposition did not observe worker and shared-library construction");
        avsync_audit_enable(true);
        feed(*worker,seconds,1);
        avsync_audit_enable(false);
        const auto steady=avsync_audit_stats();
        avsync_audit_enable(true);
        for (std::uint64_t generation=2;generation<=5;++generation) {
            require(worker->reset({9,generation}),"reset failed");
            feed(*worker,8,generation);
        }
        avsync_audit_enable(false);
        const auto reset=avsync_audit_stats();
        worker.reset(); const auto released=avsync_audit_stats();
        require(steady.attempts==initial.attempts && reset.attempts==initial.attempts,
                "steady-state or reset allocated");
        require(reset.bytes==initial.bytes && reset.peak<8*1024*1024 && !reset.table_overflow && !released.live,
                "memory budget, growth, or release failure");
        std::cout<<"{\"seconds\":"<<seconds<<",\"constructor_allocations\":"<<initial.attempts
            <<",\"requested_heap_bytes\":"<<initial.bytes<<",\"peak_requested_heap_bytes\":"<<reset.peak
            <<",\"steady_allocations\":"<<steady.attempts-initial.attempts
            <<",\"reset_allocations\":"<<reset.attempts-steady.attempts<<",\"live_after_destroy\":"<<released.live<<"}\n";
        // Fail each observed constructor allocation exactly once, including all
        // shared-library calloc calls. Exceptions must escape, not create unity DSP.
        for (std::uint64_t index=1;index<=initial.attempts;++index) {
            require(avsync_audit_begin(index),"failure audit start failed");
            bool threw{};
            try { auto failed=std::make_unique<avsync::AudioCorrectionWorker>(avsync::SessionToken{9,1},7); }
            catch (const std::exception&) { threw=true; }
            avsync_audit_enable(false);
            const auto s=avsync_audit_stats();
            require(threw && s.failures==1 && !s.live && !s.table_overflow,"initialization failure not explicit/clean");
        }
        std::cout<<"{\"initialization_failure_controls\":"<<initial.attempts<<",\"passed\":true}\n";
    } catch (const std::exception& e) { avsync_audit_enable(false); std::cerr<<e.what()<<'\n'; return 1; }
}

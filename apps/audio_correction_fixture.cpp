// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_correction.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Options {
    std::string scenario{"constant"};
    double ppm{};
    unsigned seconds{12}, pull_frames{480};
    bool varied{}, incorrect_anchors{};
};
// Independent analytic clock: integrate dt/dx = 1/(1 + ppm(x)/1e6),
// where x is nominal device time. No estimator/worker output enters this model.
struct Clock {
    std::string scenario;
    long double ppm;
    static long double segment(long double x, long double initial, long double slope = 0) {
        const auto a = 1 + initial / 1e6L, b = slope / 1e6L;
        return b == 0 ? x / a : std::log1p(b * x / a) / b;
    }
    long double at(long double x) const {
        if (scenario == "constant") return segment(x, ppm);
        if (scenario == "step") {
            if (x <= 8) return x;
            if (x <= 20) return 8 + segment(x-8, 499);
            if (x <= 36) return 8 + segment(12, 499) + segment(x-20, -499);
            return 8 + segment(12, 499) + segment(16, -499) + (x-36);
        }
        if (x <= 6) return segment(x, -300);
        if (x <= 120) return segment(6, -300) + segment(x-6, -300, 600.L/114);
        return segment(6, -300) + segment(114, -300, 600.L/114) + segment(x-120, 300);
    }
    long double sample(std::uint64_t frame) const { return at(frame / 48'000.L); }
};
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
Options parse(int argc, char** argv) {
    Options o;
    for (int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if (arg=="--varied-packets") { o.varied=true; continue; }
        if (arg=="--incorrect-anchors") { o.incorrect_anchors=true; continue; }
        require(i+1<argc,"missing option value"); const std::string value=argv[++i];
        std::size_t used{};
        if (arg=="--scenario") o.scenario=value;
        else if (arg=="--ppm") {
            o.ppm=std::stod(value,&used); require(used==value.size(),"invalid ppm");
        } else if (arg=="--seconds" || arg=="--pull-frames") {
            const auto number=std::stoul(value,&used);
            require(used==value.size() && number<=3840,"invalid integer");
            if (arg=="--seconds") o.seconds=static_cast<unsigned>(number);
            else o.pull_frames=static_cast<unsigned>(number);
        } else throw std::runtime_error("unknown option");
    }
    require(o.scenario=="constant" || o.scenario=="step" || o.scenario=="ramp","unknown scenario");
    require(std::isfinite(o.ppm) && std::abs(o.ppm)<=499,"ppm must be in [-499,499]");
    require(o.seconds>=8 && o.seconds<=600 && o.pull_frames>=1,"invalid duration or output partition");
    require(o.scenario!="step" || o.seconds>=45,"step requires at least 45 seconds");
    require(o.scenario!="ramp" || o.seconds>=125,"ramp requires at least 125 seconds");
    require(!o.incorrect_anchors || (o.scenario=="constant" && o.ppm==-499 && o.seconds==60),
            "negative control requires constant -499 ppm and exactly 60 seconds");
    return o;
}
int run(const Options& o) {
    constexpr avsync::SessionToken epoch{9,1};
    constexpr std::uint64_t clock_epoch=7;
    constexpr std::uint32_t zero=0xffffff00U, ssrc=11;
    constexpr avsync::Nanoseconds base=1'000'000'000;
    const Clock truth{o.scenario,o.ppm};
    const Clock metadata=o.incorrect_anchors ? Clock{"constant",499} : truth;
    const auto duration=truth.at(o.seconds);
    constexpr std::array<long double,6> fractions{.12L,.22L,.365L,.49L,.665L,.84L};
    std::array<long double,12> markers{}, peak_times{};
    std::array<float,12> peaks{};
    for (std::size_t i=0;i<fractions.size();++i) markers[i]=4+(duration-4)*fractions[i];
    // Long runs must measure transitions themselves, not just their settled tail.
    const std::array<long double,6> extra=o.scenario=="step"
        ? std::array<long double,6>{7.321L,9.673L,17.231L,22.917L,31.319L,38.773L}
        : o.scenario=="ramp"
        ? std::array<long double,6>{8.213L,24.317L,47.619L,71.733L,102.431L,119.813L}
        : std::array<long double,6>{4+(duration-4)*.053L,4+(duration-4)*.175L,
            4+(duration-4)*.291L,4+(duration-4)*.541L,4+(duration-4)*.737L,4+(duration-4)*.921L};
    std::copy(extra.begin(),extra.end(),markers.begin()+6);
    std::sort(markers.begin(),markers.end());
    auto w=std::make_unique<avsync::AudioCorrectionWorker>(epoch,clock_epoch);
    std::array<float,360> pcm{};
    std::array<float,7680> out{};
    constexpr std::array<unsigned,8> partitions{1,17,180,71,149,32,113,7};
    std::uint64_t wire{}, delivered{};
    unsigned packet{};
    avsync::Nanoseconds now{};
    std::vector<double> call_us;
    call_us.reserve(o.seconds*400);
    double maximum_step{};
    float leakage{};
    std::size_t detected{};
    float event_peak{};
    long double event_time{};
    while (wire<static_cast<std::uint64_t>(o.seconds)*48000) {
        const auto count=static_cast<unsigned>(std::min<std::uint64_t>(
            o.varied ? partitions[packet%partitions.size()] : 180,
            static_cast<std::uint64_t>(o.seconds)*48000-wire));
        avsync::wire::AudioRecord r;
        r.epoch=epoch; r.clock_epoch=clock_epoch; r.device_origin=1234;
        r.source_rate=192000; r.rtp_zero=zero; r.packet_wire_start=wire;
        r.anchor_sequence=wire/960; r.device_position=r.device_origin+r.anchor_sequence*3840;
        r.capture_ns=base+static_cast<avsync::Nanoseconds>(std::floor(metadata.sample(r.anchor_sequence*960)*1e9L));
        r.qpc_100ns=static_cast<std::uint64_t>(r.capture_ns/100); r.calibration_revision=1;
        // A deterministic 0..4ms sawtooth perturbs arrival only; monotonic caller time.
        now=std::max(now,base+static_cast<avsync::Nanoseconds>(std::floor(truth.sample(wire+count)*1e9L))+
            2'000'000+static_cast<avsync::Nanoseconds>((packet*7919U)%4000001U));
        for (unsigned i=0;i<count;++i) {
            const auto t=truth.sample(wire+i);
            long double value{};
            for (const auto marker:markers) {
                const auto distance=t-marker;
                if (std::abs(distance)<.003L) value+=.6L*std::exp(-.5L*distance*distance/(.0003L*.0003L));
            }
            pcm[2*i]=static_cast<float>(value); pcm[2*i+1]=0;
        }
        require(w->push(r,ssrc,static_cast<std::uint32_t>(zero+wire),std::span(pcm).first(count*2),now,true)
            !=avsync::CorrectionPush::rejected,"worker rejected generated media");
        const auto before=w->command_ppm();
        const auto calls=w->diagnostics().backend_calls;
        const auto begin=std::chrono::steady_clock::now();
        const auto produced=w->dispatch(now,true);
        const auto end=std::chrono::steady_clock::now();
        require(w->state()!=avsync::CorrectionState::faulted,"worker dispatch faulted");
        const auto executed=w->diagnostics().backend_calls-calls;
        if (executed) call_us.push_back(std::chrono::duration<double,std::micro>(end-begin).count());
        else require(w->command_ppm()==before,"no-progress call changed command");
        require(executed<=w->max_calls && produced==executed*w->quantum,"dispatch budget mismatch");
        require(std::abs(w->command_ppm()-before)<=executed*w->command_step_ppm+1e-8,"command slew exceeded");
        maximum_step=std::max(maximum_step,w->diagnostics().maximum_command_step_ppm);
        while (const auto b=w->pull(std::span(out).first(o.pull_frames*2),now,true)) {
            require(b->epoch==epoch && b->first_frame==delivered,"output index discontinuity");
            require(b->capture_grid_ns==avsync::RationalTimeline(*w->origin_ns(),48000).at(delivered),"output timeline discontinuity");
            for (std::size_t i=0;i<b->frames;++i) {
                const auto value=out[2*i];
                require(std::isfinite(value) && std::isfinite(out[2*i+1]),"non-finite output");
                leakage=std::max(leakage,std::abs(out[2*i+1]));
                const auto t=(*w->origin_ns()-base)/1e9L+(delivered+i)/48000.L;
                if (value>.1F) {
                    if (value>event_peak) { event_peak=value; event_time=t; }
                } else if (event_peak>0) {
                    require(detected<markers.size(),"extra marker");
                    peaks[detected]=event_peak; peak_times[detected]=event_time;
                    ++detected; event_peak=0;
                }
            }
            delivered+=b->frames;
        }
        wire+=count; ++packet;
    }
    require(w->state()==avsync::CorrectionState::running && delivered>48000,"insufficient output");
    require(detected==markers.size() && event_peak==0,"missing or unfinished marker");
    long double worst{};
    for (std::size_t m=0;m<markers.size();++m) {
        require(peaks[m]>.5F,"missing marker");
        worst=std::max(worst,std::abs(peak_times[m]-markers[m])*1000);
    }
    std::sort(call_us.begin(),call_us.end());
    const auto& d=w->diagnostics();
    const auto expected_final=o.scenario=="constant" ? o.ppm : o.scenario=="step" ? 0 : 300;
    const bool phase_ok=worst<=5;
    const bool rate_ok=std::abs(w->command_ppm()-expected_final)<=5;
    const bool accepted=phase_ok && rate_ok && leakage<=1e-5 && maximum_step<=.99+1e-10;
    std::cout<<std::fixed<<std::setprecision(6)
        <<"{\"scenario\":\""<<o.scenario<<"\",\"seconds\":"<<o.seconds<<",\"ppm\":"<<o.ppm
        <<",\"varied_packets\":"<<(o.varied?"true":"false")<<",\"pull_frames\":"<<o.pull_frames
        <<",\"incorrect_anchors\":"<<(o.incorrect_anchors?"true":"false")
        <<",\"origin_wire_frame\":"<<w->origin_wire_frame()<<",\"delivered_frames\":"<<delivered
        <<",\"marker_count\":"<<markers.size()<<",\"max_marker_error_ms\":"<<static_cast<double>(worst)
        <<",\"final_command_ppm\":"<<w->command_ppm()<<",\"max_command_step_ppm\":"<<maximum_step
        <<",\"channel_leakage\":"<<leakage<<",\"peak_input_frames\":"<<d.peak_input_frames
        <<",\"peak_output_frames\":"<<d.peak_output_frames<<",\"backend_calls\":"<<d.backend_calls
        <<",\"dispatch_p99_us\":"<<call_us[(call_us.size()-1)*99/100]<<",\"dispatch_max_us\":"<<call_us.back()
        <<",\"worker_object_bytes\":"<<sizeof(*w)<<",\"marker_errors_ms\":[";
    for (std::size_t i=0;i<markers.size();++i) std::cout<<(i?",":"")<<static_cast<double>((peak_times[i]-markers[i])*1000);
    std::cout<<"],\"accepted\":"<<(accepted?"true":"false")<<"}\n";
    // Explicit negative-control mode succeeds only for a measured phase failure,
    // not for crashes, missing markers or some unrelated runtime exception.
    return o.incorrect_anchors ? (worst>5 ? 0:1) : (accepted ? 0:1);
}
}
int main(int argc,char** argv) {
    if (argc==2 && std::string(argv[1])=="--help") {
        std::cout<<"Offline generated PCM only; no devices, network, files or OBS.\n"
            <<"--scenario constant|step|ramp --ppm [-499,499] --seconds [8,600]\n"
            <<"--varied-packets --pull-frames [1,3840] --incorrect-anchors\n"; return 0;
    }
    try { return run(parse(argc,argv)); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

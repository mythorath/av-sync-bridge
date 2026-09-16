// SPDX-License-Identifier: GPL-2.0-or-later
// Analytic signals only. No capture, network, playback, recordings, or OBS.
#include "avsync/asrc.hpp"
#include "avsync/audio_phase.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void require(bool okay, const char* text) { if (!okay) throw std::runtime_error(text); }
double db(double value) { return 20 * std::log10(std::max(value, 1e-15)); }
struct Timing {
    std::array<std::uint64_t,10001> bins{}; // 10 us, with an explicit overflow bin.
    std::uint64_t calls{}, over_quantum{};
    double maximum_us{};
    void add(double us) {
        ++calls; if (us > 10000) ++over_quantum;
        ++bins[std::min<std::size_t>(static_cast<std::size_t>(us/10), bins.size()-1)];
        maximum_us = std::max(maximum_us, us);
    }
    double p99() const {
        std::uint64_t total{};
        for (std::size_t i=0;i<bins.size();++i) {
            total+=bins[i];
            if (total >= (calls*99+99)/100) return i+1==bins.size() ? maximum_us : (i+1)*10.;
        }
        return 100010;
    }
};
struct Signal { const char* name; double frequency; unsigned channel; };
constexpr std::array<Signal,10> signals{{
    {"tone",1000,0},{"tone",10000,0},{"tone",18000,0},
    {"tone",1000,1},{"tone",10000,1},{"tone",18000,1},
    {"multitone",0,2},{"dc",0,0},{"silence",0,0},{"impulses",0,0}}};
constexpr std::array<std::uint64_t,3> impulses{48000,110351,181237};
double wave(const Signal& s, long double source, unsigned channel) {
    if (s.channel != 2 && s.channel != channel) return 0;
    const std::string_view name(s.name);
    if (name=="silence") return 0;
    if (name=="dc") return .25;
    if (name=="impulses") {
        for (auto impulse:impulses) if (source==impulse) return .5;
        return 0;
    }
    const auto tone=[&](long double frequency) {
        return std::sin(2*std::numbers::pi_v<long double>*frequency*source/48000);
    };
    if (name=="tone") return static_cast<double>(.5L*tone(s.frequency));
    // Distinct channel spectra; theoretical source peak <= .4, measured too.
    return static_cast<double>(.1L*(tone(channel ? 700:997)+tone(channel ? 3100:5003)+
                                    tone(channel ? 11003:10007)+tone(channel ? 17011:18013)));
}
bool run(const Signal& signal, unsigned seconds, bool paced, bool negative) {
    avsync::AsrcStereo backend(-499);
    avsync::SincPhaseModel phase;
    require(phase.supports(backend.backend_version()) && phase.reset(0,-499), "unsupported phase model");
    std::array<float,4096> input{};
    std::array<float,960> output{};
    std::array<avsync::PredictedSourcePosition,480> positions{};
    std::array<double,3> impulse_peak{}, impulse_error{};
    std::uint64_t consumed{}, compared{}, wake_late{};
    double command=-499, direction=1, maximum_error{}, leak{}, peak{}, input_peak{};
    long double residual{}, power{}, reference_power{};
    Timing timing;
    // All continuous test frequencies are integer Hz. Build one exact nominal
    // second before pacing rather than recomputing 2,048 look-ahead frames of
    // expensive long-double trigonometry for every 480-frame call. Fractional
    // output comparisons still use the independent analytic function below.
    std::vector<float> period(48000*2);
    if (std::string_view(signal.name)!="impulses")
        for (std::size_t i=0;i<48000;++i) for (unsigned ch=0;ch<2;++ch)
            period[2*i+ch]=static_cast<float>(wave(signal,i,ch));
    const auto started=Clock::now();
    const std::uint64_t calls=static_cast<std::uint64_t>(seconds)*100;
    for (std::uint64_t call=0;call<calls;++call) {
        if (command>=499) direction=-1;
        if (command<=-499) direction=1;
        command=std::clamp(command+direction*.99,-499.,499.);
        for (std::size_t i=0;i<input.size()/2;++i) for (unsigned ch=0;ch<2;++ch) {
            input[2*i+ch]=std::string_view(signal.name)=="impulses"
                ? static_cast<float>(wave(signal,consumed+i,ch)) : period[2*((consumed+i)%48000)+ch];
            input_peak=std::max(input_peak,std::abs(static_cast<double>(input[2*i+ch])));
        }
        if (paced) {
            const auto due=started+std::chrono::milliseconds(call*10);
            std::this_thread::sleep_until(due);
            if (Clock::now()-due>std::chrono::milliseconds(10)) ++wake_late;
        }
        const auto begin=Clock::now();
        const auto result=backend.process(input,output,command);
        const auto end=Clock::now();
        timing.add(std::chrono::duration<double,std::micro>(end-begin).count());
        require(result.status==avsync::AsrcStatus::progress && result.output_frames_generated==480,
                "partial/nonprogress backend call");
        require(phase.advance(command,positions),"phase prediction failed");
        consumed+=result.input_frames_used;
        // Signal corruption control: the oracle and rate model stay unchanged.
        if (negative) for (auto& sample:output) sample*=.98F;
        for (std::size_t i=0;i<480;++i) for (unsigned ch=0;ch<2;++ch) {
            const double value=output[2*i+ch];
            require(std::isfinite(value),"nonfinite output");
            peak=std::max(peak,std::abs(value));
            if (signal.channel!=2 && signal.channel!=ch) leak=std::max(leak,std::abs(value));
            const auto source=static_cast<long double>(positions[i].whole)+positions[i].fraction;
            if (std::string_view(signal.name)=="impulses") {
                if (ch==0) for (std::size_t k=0;k<impulses.size();++k)
                    if (std::abs(source-impulses[k])<8 && std::abs(value)>impulse_peak[k]) {
                        impulse_peak[k]=std::abs(value); impulse_error[k]=static_cast<double>(std::abs(source-impulses[k]));
                    }
                continue;
            }
            if (phase.frames()-480+i<12000) continue; // Declared 250 ms startup guard only.
            const auto reference=wave(signal,source,ch);
            const auto error=value-reference;
            maximum_error=std::max(maximum_error,std::abs(error));
            // Do not dilute the isolated-channel residual with its silent channel.
            if (signal.channel==2 || signal.channel==ch) {
                residual+=error*error; power+=value*value; reference_power+=reference*reference; ++compared;
            }
        }
    }
    const auto rms=db(std::sqrt(static_cast<double>(residual/std::max<std::uint64_t>(1,compared))));
    const auto gain=reference_power ? 10*std::log10(static_cast<double>(power/reference_power)) : 0;
    bool quality=peak<=1 && leak<=1e-5 && rms<=-80 && db(maximum_error)<=-50 && std::abs(gain)<=.2;
    if (std::string_view(signal.name)=="impulses")
        for (std::size_t i=0;i<impulses.size();++i) quality &= impulse_peak[i]>.25 && impulse_error[i]<=1.0;
    if (std::string_view(signal.name)=="silence") quality &= peak<=1e-7;
    const bool budget=timing.p99()<2000;
    std::cout<<std::fixed<<std::setprecision(6)<<"{\"signal\":\""<<signal.name
        <<"\",\"frequency\":"<<signal.frequency<<",\"channel\":"<<signal.channel<<",\"seconds\":"<<seconds
        <<",\"paced\":"<<(paced?"true":"false")<<",\"negative_control\":"<<(negative?"true":"false")
        <<",\"compared_samples\":"<<compared<<",\"rms_residual_dbfs\":"<<rms
        <<",\"peak_residual_dbfs\":"<<db(maximum_error)<<",\"gain_db\":"<<gain
        <<",\"leak_dbfs\":"<<db(leak)<<",\"input_peak\":"<<input_peak<<",\"output_peak\":"<<peak
        <<",\"backend_calls\":"<<timing.calls<<",\"call_p99_upper_us\":"<<timing.p99()
        <<",\"call_max_us\":"<<timing.maximum_us<<",\"calls_over_10ms\":"<<timing.over_quantum
        <<",\"paced_wakes_over_10ms_late\":"<<wake_late<<",\"impulse_max_source_error_frames\":"
        <<*std::max_element(impulse_error.begin(),impulse_error.end())
        <<",\"quality_pass\":"<<(quality?"true":"false")<<",\"cpu_budget_pass\":"<<(budget?"true":"false")<<"}\n";
    require(negative ? !quality && rms>-80 : quality,"unexpected quality verdict");
    return budget;
}
}
int main(int argc,char** argv) {
    try {
        unsigned seconds=24; bool paced{}, budget_required{};
        for (int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if (arg=="--help") { std::cout<<"Generated quality/individual-call timing; no devices/network/OBS.\n"
                <<"--seconds 8..600 --paced (multitone only) --require-budget\n"; return 0; }
            if (arg=="--paced") { paced=true; continue; }
            if (arg=="--require-budget") { budget_required=true; continue; }
            require(arg=="--seconds" && i+1<argc,"invalid argument");
            const std::string value=argv[++i]; std::size_t used{}; const auto n=std::stoul(value,&used);
            require(used==value.size() && n>=8 && n<=600,"invalid seconds"); seconds=static_cast<unsigned>(n);
        }
        bool budget=true;
        for (const auto& signal:signals) if (!paced || std::string_view(signal.name)=="multitone")
            budget &= run(signal,seconds,paced,false);
        (void)run(signals[0],8,false,true);
        return budget_required && !budget ? 2 : 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Generated signals only: no capture, network, playback, OBS, or PCM files.
#include "avsync/asrc.hpp"
#include "avsync/audio_anchors.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
double numeric(const char* value, double low, double high) {
    const std::string_view text(value); double number{};
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),number);
    require(error==std::errc{} && end==text.data()+text.size() && std::isfinite(number) &&
            number>=low && number<=high,"invalid numeric argument");
    return number;
}
struct Options {
    unsigned seconds{8}; double ppm{}, frequency{1000};
    std::optional<double> command; bool offline{}, estimate{}, varied{};
    std::string signal{"tone"};
};
void help() {
    std::cout << "avsync-asrc-fixture --offline [--seconds 4..600] [--ppm -500..500]\n"
                 " [--command-ppm -500..500 | --estimate] [--frequency 100..18000]\n"
                 " [--signal tone|markers|silence|dc] [--chunk-pattern fixed|varied]\n"
                 "Generated stereo only, right channel silent. No devices/network/playback/files.\n"
                 "Known-rate or original-anchor preflight, not a live adaptive controller.\n"
                 "Reports analytic signal/count gates; a deliberately wrong command should fail.\n";
}
Options parse(int argc,char** argv) {
    Options o; unsigned seen{};
    for (int i=1;i<argc;++i) {
        const std::string_view arg(argv[i]);
        if (arg=="--offline" && !o.offline) { o.offline=true; continue; }
        if (arg=="--estimate" && !o.estimate) { o.estimate=true; continue; }
        require(i+1<argc,"missing argument value"); const auto* value=argv[++i]; unsigned bit{};
        if (arg=="--seconds") { bit=1; const auto n=numeric(value,4,600); require(std::floor(n)==n,"seconds must be integral"); o.seconds=static_cast<unsigned>(n); }
        else if (arg=="--ppm") { bit=2; o.ppm=numeric(value,-500,500); }
        else if (arg=="--command-ppm") { bit=4; o.command=numeric(value,-500,500); }
        else if (arg=="--frequency") { bit=8; o.frequency=numeric(value,100,18000); }
        else if (arg=="--signal") { bit=16; o.signal=value; require(o.signal=="tone" || o.signal=="markers" || o.signal=="silence" || o.signal=="dc","invalid generated signal"); }
        else if (arg=="--chunk-pattern") { bit=32; const std::string_view v(value); require(v=="fixed" || v=="varied","invalid chunk pattern"); o.varied=v=="varied"; }
        else throw std::runtime_error("unknown or repeated option");
        require(!(seen&bit),"repeated option"); seen|=bit;
    }
    require(o.offline && !(o.estimate && o.command),"explicit offline required; choose estimate or command, not both");
    return o;
}

constexpr std::array<long double,6> marker_fraction{0.12L,0.22L,0.365L,0.49L,0.665L,0.84L};
double signal(const Options& o,long double time) {
    if (o.signal=="silence") return 0;
    if (o.signal=="dc") return 0.25;
    if (o.signal=="tone") return static_cast<double>(0.5L*std::sin(2*std::numbers::pi_v<long double>*o.frequency*time));
    long double value{};
    for (const auto fraction:marker_fraction) {
        const auto distance=(time-fraction*o.seconds)/0.001L;
        if (std::abs(distance)<10) value+=0.5L*std::exp(-0.5L*distance*distance);
    }
    return static_cast<double>(value);
}

// This is an analytic metadata preflight, not transport or a live priming queue.
double estimate_rate(double actual_ppm) {
    avsync::AudioAnchorConfig config; config.rate.nominal_rate=192000;
    const avsync::SessionToken epoch{1,1}; avsync::AudioAnchorTracker anchors(epoch,config);
    std::int64_t last_time{};
    const long double actual=192000.L*(1+static_cast<long double>(actual_ppm)/1e6L);
    for (std::uint64_t i=0;i<=410;++i) {
        const auto position=i*1920;
        last_time=1'000'000'000+static_cast<std::int64_t>(std::floor(position*1e9L/actual));
        const avsync::AudioCaptureAnchor anchor{epoch,i,position,last_time,192000,false};
        const auto observation=anchors.observe(anchor,last_time+1'000'000);
        require(observation.wire_position.has_value(),"generated original anchor was rejected");
    }
    const auto estimate=anchors.current_estimate(last_time+1'000'000);
    require(anchors.diagnostics().measurements>=3 && estimate.has_value(),"original capture anchors do not authorize correction");
    return estimate->source_rate_error_ppm;
}

double db(double value) { return 20*std::log10(std::max(value,1e-15)); }
struct Metrics {
    std::uint64_t frames{}, compared{}, calls{}, no_progress{}, regions{};
    long double residual_squared{}, output_squared{}, reference_squared{};
    double maximum_error{}, right_peak{}, output_peak{}, maximum_call_ms{};
    std::array<std::uint64_t,1001> call_histogram{}; // 10 us bins; final bin overflow.
    bool in_marker{}; double region_peak{}; std::uint64_t region_peak_index{};
    std::array<std::uint64_t,6> peak_indices{};
    void finish_region() {
        if (!in_marker) return;
        if (regions<peak_indices.size()) peak_indices[regions]=region_peak_index;
        ++regions; in_marker=false; region_peak=0;
    }
    void consume(const Options& options,std::span<const float> samples) {
        for (std::size_t i=0;i<samples.size();i+=2,++frames) {
            const double value=samples[i], right=samples[i+1];
            require(std::isfinite(value) && std::isfinite(right),"nonfinite generated output");
            output_peak=std::max(output_peak,std::abs(value)); right_peak=std::max(right_peak,std::abs(right));
            if (options.signal=="markers") {
                if (value>0.1) {
                    in_marker=true;
                    if (value>region_peak) { region_peak=value; region_peak_index=frames; }
                } else finish_region();
            }
            const long double time=frames/48000.L;
            if (time>=0.25L && time<options.seconds-0.25L) {
                const auto reference=signal(options,time); const double error=value-reference;
                maximum_error=std::max(maximum_error,std::abs(error));
                residual_squared+=error*error; output_squared+=value*value; reference_squared+=reference*reference;
                ++compared;
            }
        }
    }
};

int run(const Options& options) {
    const auto started=std::chrono::steady_clock::now();
    const double command=options.estimate ? estimate_rate(options.ppm) : options.command.value_or(options.ppm);
    avsync::AsrcStereo converter(command);
    std::array<float,1920*2> input{};
    std::array<float,480*2> output{};
    constexpr std::array<std::size_t,8> input_pattern{1,17,479,1920,73,960,11,480};
    constexpr std::array<std::size_t,8> output_pattern{480,31,127,7,480,241,1,479};
    const long double rate=48000.L*(1+static_cast<long double>(options.ppm)/1e6L);
    const auto input_total=static_cast<std::uint64_t>(std::floor(rate*options.seconds));
    std::uint64_t generated{},consumed{},chunk_index{}; std::size_t pending{},offset{};
    Metrics metrics; unsigned stalled{}; bool finished{};
    while (!finished) {
        require(metrics.calls<5'000'000,"fixture call budget exceeded");
        if (metrics.calls%256==0)
            require(std::chrono::steady_clock::now()-started<std::chrono::seconds(120),"fixture wall-time budget exceeded");
        if (!pending && generated<input_total) {
            const auto wanted=options.varied ? input_pattern[chunk_index++%input_pattern.size()] : 480;
            pending=static_cast<std::size_t>(std::min<std::uint64_t>(wanted,input_total-generated)); offset=0;
            for (std::size_t n=0;n<pending;++n) {
                input[2*n]=static_cast<float>(signal(options,(generated+n)/rate)); input[2*n+1]=0;
            }
            generated+=pending;
        }
        const auto output_capacity=options.varied ? output_pattern[metrics.calls%output_pattern.size()] : 480;
        const auto before=std::chrono::steady_clock::now();
        const auto result=converter.process(std::span<const float>(input.data()+offset*2,pending*2),
            std::span<float>(output.data(),output_capacity*2),command,generated==input_total);
        const auto duration=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
        metrics.maximum_call_ms=std::max(metrics.maximum_call_ms,duration);
        ++metrics.call_histogram[std::min<std::size_t>(1000,static_cast<std::size_t>(duration*100))];
        ++metrics.calls;
        require(result.status!=avsync::AsrcStatus::invalid && result.status!=avsync::AsrcStatus::failed,
                "ASRC rejected or failed generated processing");
        require(result.input_frames_used<=pending && result.output_frames_generated<=output_capacity,
                "ASRC count exceeded provided storage");
        pending-=result.input_frames_used; offset+=result.input_frames_used; consumed+=result.input_frames_used;
        metrics.consume(options,std::span<const float>(output.data(),result.output_frames_generated*2));
        if (!result.input_frames_used && !result.output_frames_generated) { ++metrics.no_progress; ++stalled; }
        else stalled=0;
        finished=result.status==avsync::AsrcStatus::finished;
        require(finished || stalled<=8,"ASRC made no progress on available fixture input");
    }
    metrics.finish_region();
    require(consumed==input_total && metrics.compared>0,"incomplete input consumption or comparison");
    const auto expected=static_cast<std::int64_t>(std::floor(input_total/(1+static_cast<long double>(command)/1e6L)));
    const auto count_error=static_cast<std::int64_t>(metrics.frames)-expected;
    // Independent oscillator oracle, not the ratio supplied to the backend.
    // Stationary silence/DC cannot reveal wrong timing through waveform error.
    const auto source_expected=static_cast<std::int64_t>(std::floor(input_total/(1+static_cast<long double>(options.ppm)/1e6L)));
    const auto source_duration_error=static_cast<std::int64_t>(metrics.frames)-source_expected;
    const auto residual_rms=std::sqrt(static_cast<double>(metrics.residual_squared/metrics.compared));
    const auto reference_rms=std::sqrt(static_cast<double>(metrics.reference_squared/metrics.compared));
    const auto output_rms=std::sqrt(static_cast<double>(metrics.output_squared/metrics.compared));
    const auto gain=reference_rms>1e-12 ? db(output_rms/reference_rms) : 0;
    double marker_error{};
    if (options.signal=="markers") {
        marker_error=metrics.regions==marker_fraction.size() ? 0 : 1e9;
        for (std::size_t i=0;i<marker_fraction.size() && i<metrics.regions;++i)
            marker_error=std::max(marker_error,static_cast<double>(std::abs(
                metrics.peak_indices[i]-marker_fraction[i]*options.seconds*48000.L)));
    }
    const bool passed=std::abs(count_error)<=1 && std::abs(source_duration_error)<=1 &&
        residual_rms<=1e-4 && metrics.maximum_error<=0.003162278 &&
        metrics.right_peak<=1e-5 && std::abs(gain)<=0.2 && marker_error<=1 && metrics.output_peak<=1.0 &&
        (options.signal!="silence" || metrics.output_peak<=1e-7);
    std::uint64_t cumulative{}; std::size_t p99_bin{};
    for (;p99_bin<metrics.call_histogram.size();++p99_bin) {
        cumulative+=metrics.call_histogram[p99_bin];
        if (cumulative*100>=metrics.calls*99) break;
    }
    std::cout.precision(12);
    std::cout << "{\"schema\":1,\"generated_only\":true,\"live_controller\":false,\"passed\":" << (passed?"true":"false")
        << ",\"signal\":\"" << options.signal << "\",\"seconds\":" << options.seconds << ",\"source_ppm\":" << options.ppm
        << ",\"command_ppm\":" << command << ",\"original_anchor_preflight\":" << (options.estimate?"true":"false")
        << ",\"chunk_pattern\":\"" << (options.varied?"varied":"fixed") << "\",\"frequency_hz\":" << options.frequency
        << ",\"input_frames\":" << input_total << ",\"output_frames\":" << metrics.frames << ",\"count_error_frames\":" << count_error
        << ",\"source_duration_error_frames\":" << source_duration_error
        << ",\"guard_seconds\":0.25,\"compared_frames\":" << metrics.compared << ",\"residual_rms_dbfs\":" << db(residual_rms)
        << ",\"maximum_residual_dbfs\":" << db(metrics.maximum_error) << ",\"gain_db\":" << gain
        << ",\"right_peak_dbfs\":" << db(metrics.right_peak) << ",\"output_peak\":" << metrics.output_peak
        << ",\"marker_regions\":" << metrics.regions << ",\"max_marker_error_samples\":" << marker_error
        << ",\"calls\":" << metrics.calls << ",\"no_progress_calls\":" << metrics.no_progress
        << ",\"max_call_ms\":" << metrics.maximum_call_ms << ",\"p99_call_bin_ms\":" << (p99_bin+1)*0.01
        << ",\"p99_bin_overflow\":" << (p99_bin>=1000?"true":"false")
        << ",\"wall_seconds\":" << std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() << "}\n";
    return passed ? 0 : 3;
}
}
int main(int argc,char** argv) {
    if (argc==1) { help(); return 0; }
    for (int i=1;i<argc;++i) if (std::string_view(argv[i])=="--help") { help(); return 0; }
    try { return run(parse(argc,argv)); }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 2; }
}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_phase.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace avsync {
namespace {
bool valid(PredictedSourcePosition p) noexcept {
    return std::isfinite(p.fraction) && p.fraction>=0 && p.fraction<1;
}
bool less(PredictedSourcePosition a, PredictedSourcePosition b) noexcept {
    return a.whole<b.whole || (a.whole==b.whole && a.fraction<b.fraction);
}
// Subtract integers BEFORE conversion: high absolute source positions retain
// their fractional resolution on platforms where long double equals double.
long double distance(PredictedSourcePosition a, PredictedSourcePosition b) noexcept {
    return static_cast<long double>(a.whole-b.whole)+a.fraction-b.fraction;
}
bool ppm_valid(double p) noexcept { return std::isfinite(p) && std::abs(p)<=500; }
}
bool SincPhaseModel::supports(std::string_view version) noexcept {
    return version.starts_with("libsamplerate-0.2.2 ");
}
bool SincPhaseModel::reset(std::uint64_t origin, double ppm) noexcept {
    if (!ppm_valid(ppm)) return false;
    next_={origin,0}; ratio_=1/(1+ppm/1e6); frames_=0; initialized_=true; return true;
}
bool SincPhaseModel::advance(double ppm, std::span<PredictedSourcePosition> positions) noexcept {
    if (!initialized_ || !ppm_valid(ppm) || positions.size()!=quantum || !positions.data() ||
        frames_>maximum_frames-quantum) return false;
    const double target=1/(1+ppm/1e6);
    const double difference=target-ratio_;
    auto cursor=next_;
    double reached=ratio_;
    // The library advances its input cursor by the reciprocal of each sample's
    // interpolated output/input ratio. Counts consumed into its private history
    // play no role. Preserve the audited tiny-change deadband and endpoint lag.
    for (std::size_t i=0;i<quantum;++i) {
        positions[i]=cursor;
        reached=std::abs(difference)>1e-10 ? ratio_+difference*(static_cast<double>(i)/quantum) : ratio_;
        const auto advance=cursor.fraction+1/reached;
        const auto whole=static_cast<std::uint64_t>(std::floor(advance));
        if (cursor.whole>std::numeric_limits<std::uint64_t>::max()-whole) return false;
        cursor.whole+=whole; cursor.fraction=advance-static_cast<double>(whole);
    }
    next_=cursor; ratio_=reached; frames_+=quantum; return true;
}
PhaseStatus AudioPhaseLedger::add(PredictedSourcePosition position, Nanoseconds time,
                                 PredictedSourcePosition retain) noexcept {
    if (!valid(position) || !valid(retain) || time<0) return PhaseStatus::invalid;
    if (size_) {
        const auto& previous=at(size_-1);
        if (!less(previous.source,position) || time<=previous.time) return PhaseStatus::invalid;
        const auto gap=checked_sub(time,previous.time);
        if (!gap || *gap>maximum_anchor_gap_ns) return PhaseStatus::gap;
    }
    // Keep the anchor at/before the earliest not-yet-processed source position.
    while (size_>=2 && !less(retain,at(1).source)) { head_=(head_+1)%capacity; --size_; }
    if (size_==capacity) return PhaseStatus::capacity;
    anchors_[(head_+size_)%capacity]={position,time}; ++size_; return PhaseStatus::ready;
}
std::optional<Nanoseconds> AudioPhaseLedger::time_at(PredictedSourcePosition p) const noexcept {
    if (!valid(p) || !size_ || less(p,at(0).source) || less(at(size_-1).source,p)) return {};
    std::size_t left=0,right=size_-1;
    while (left<right) {
        const auto middle=left+(right-left+1)/2;
        if (!less(p,at(middle).source)) left=middle; else right=middle-1;
    }
    const auto& a=at(left);
    if (!less(a.source,p)) return a.time;
    if (left+1>=size_) return {};
    const auto& b=at(left+1);
    const auto total=distance(b.source,a.source), elapsed=distance(p,a.source);
    const auto time_span=checked_sub(b.time,a.time);
    if (!time_span || total<=0 || elapsed<0 || elapsed>total) return {};
    const auto ns=std::floor(elapsed/total* *time_span);
    if (!std::isfinite(ns) || ns<0 || ns>*time_span) return {};
    return checked_add(a.time,static_cast<Nanoseconds>(ns));
}
PhaseAssessment AudioPhaseLedger::assess(std::span<const PredictedSourcePosition> positions,
                                       Nanoseconds origin, std::uint64_t first) const noexcept {
    if (positions.empty() || positions.size()>SincPhaseModel::quantum || !positions.data() || origin<0 ||
        first>std::numeric_limits<std::uint64_t>::max()-positions.size()) return {PhaseStatus::invalid};
    PhaseAssessment result{PhaseStatus::ready};
    const RationalTimeline grid(origin,48000);
    for (std::size_t i=0;i<positions.size();++i) {
        if (!valid(positions[i]) || (i && !less(positions[i-1],positions[i]))) return {PhaseStatus::invalid};
        const auto predicted=time_at(positions[i]);
        if (!predicted) return {PhaseStatus::waiting};
        const auto expected=grid.at(first+i);
        const auto phase=expected ? checked_sub(*predicted,*expected) : std::nullopt;
        if (!phase || *phase==std::numeric_limits<Nanoseconds>::min()) return {PhaseStatus::invalid};
        result.maximum_absolute_ns=std::max(result.maximum_absolute_ns,std::abs(*phase));
        if (i==0) result.first_ns=*phase;
        result.last_ns=*phase;
    }
    if (result.maximum_absolute_ns>phase_limit_ns) result.status=PhaseStatus::mismatch;
    return result;
}
} // namespace avsync

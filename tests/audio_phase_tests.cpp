// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_phase.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
unsigned checks{};
void check(bool value,int line) { ++checks; if (!value) throw std::runtime_error("phase check line "+std::to_string(line)); }
#define CHECK(x) check(static_cast<bool>(x),__LINE__)
void model() {
    using namespace avsync;
    CHECK(SincPhaseModel::supports("libsamplerate-0.2.2 (c) test"));
    CHECK(!SincPhaseModel::supports("libsamplerate-0.2.20 (c) test"));
    CHECK(!SincPhaseModel::supports("libsamplerate-0.3.0 (c) test"));
    SincPhaseModel m; std::array<PredictedSourcePosition,480> p{};
    CHECK(!m.advance(0,p));
    CHECK(!m.reset(0,std::numeric_limits<double>::quiet_NaN()));
    for (const double ppm:{-500.,-100.,0.,100.,500.}) {
        constexpr std::uint64_t start=1ULL<<60;
        CHECK(m.reset(start,ppm));
        for (unsigned block=0;block<200;++block) {
            CHECK(m.advance(ppm,p));
            for (unsigned i=0;i<480;++i) {
                const auto expected=(block*480+i)*(1+ppm/1e6L);
                const auto actual=static_cast<long double>(p[i].whole-start)+p[i].fraction;
                CHECK(std::abs(actual-expected)<1e-6);
            }
        }
    }
    const auto before=m.next(); const auto frames=m.frames();
    CHECK(!m.advance(501,p)); CHECK(!m.advance(0,std::span(p).first(479)));
    CHECK(m.next().whole==before.whole && m.next().fraction==before.fraction && m.frames()==frames);
    CHECK(m.reset(std::numeric_limits<std::uint64_t>::max()-479,0));
    CHECK(!m.advance(0,p)); CHECK(m.frames()==0);
    CHECK(m.reset(0,0)); CHECK(m.advance(499,p));
    CHECK(m.reached_ppm()>497 && m.reached_ppm()<499); // Last-sample endpoint lag.
    CHECK(m.reset(0,0)); CHECK(m.advance(.00001,p));
    CHECK(m.reached_ppm()==0); // Audited 1e-10 ratio deadband.
}
void ledger() {
    using namespace avsync;
    constexpr std::uint64_t base=1ULL<<60;
    constexpr Nanoseconds origin=1'000'000'000;
    AudioPhaseLedger l; std::array<PredictedSourcePosition,480> p{};
    for (unsigned i=0;i<480;++i) p[i]={base+i,.25};
    CHECK(l.assess(p,origin,0).status==PhaseStatus::waiting);
    CHECK(l.add({base,.25},origin,{base,.25})==PhaseStatus::ready);
    CHECK(l.assess(p,origin,0).status==PhaseStatus::waiting); // No extrapolation.
    CHECK(l.add({base+960,.25},origin+20'000'000,{base,.25})==PhaseStatus::ready);
    auto result=l.assess(p,origin,0);
    CHECK(result.status==PhaseStatus::ready && result.maximum_absolute_ns<=1);
    CHECK(l.assess(p,origin-10'000'001,0).status==PhaseStatus::mismatch);
    CHECK(l.assess(p,origin-10'000'000,0).status==PhaseStatus::ready);
    CHECK(l.assess(p,origin+11'000'000,0).status==PhaseStatus::mismatch);
    CHECK(l.assess(p,std::numeric_limits<Nanoseconds>::max(),0).status==PhaseStatus::invalid);
    CHECK(l.assess(p,origin,std::numeric_limits<std::uint64_t>::max()).status==PhaseStatus::invalid);
    p[42].fraction=std::numeric_limits<double>::infinity();
    CHECK(l.assess(p,origin,0).status==PhaseStatus::invalid);
    CHECK(l.add({base+1920,0},origin+200'000'000,{base,0})==PhaseStatus::gap);
    CHECK(l.size()==2);
    CHECK(l.add({base+960,.25},origin+40'000'000,{base,0})==PhaseStatus::invalid);
    l.clear(); CHECK(l.size()==0);
    for (unsigned i=0;i<l.capacity;++i)
        CHECK(l.add({i,0},i*20'000,{0,0})==PhaseStatus::ready);
    CHECK(l.add({512,0},512*20'000,{0,0})==PhaseStatus::capacity);
    CHECK(l.add({512,0},512*20'000,{511,0})==PhaseStatus::ready);
    CHECK(l.size()==2);
    CHECK(l.add({513,0},513*20'000,{512,0})==PhaseStatus::ready); // Ring wrap/pruning.
    CHECK(l.size()==2);
}
}
int main() {
    try { model(); ledger(); std::cout<<checks<<" portable phase checks passed\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

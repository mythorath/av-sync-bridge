// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/asrc.hpp"
#include "avsync/audio_phase.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool x,const char* message) { if (!x) throw std::runtime_error(message); }
// Fractional source-position measurement from linear PCM, independent of the
// model's recurrence. Modular ramps measure sub-sample phase only; the separate
// NONUNIFORM marker suite is still required to detect whole-cycle mistakes.
double run(unsigned seconds,int scenario) {
    avsync::AsrcStereo backend;
    avsync::SincPhaseModel model;
    require(model.supports(backend.backend_version()),"unaudited backend version");
    const bool negative=scenario==4;
    double command=(scenario==1 || negative) ? 499 : scenario==2 ? -499 : 0;
    require(backend.reset(command) && model.reset(0,negative ? 0:command),"reset failed");
    std::array<float,4096> input{};
    std::array<float,960> output{};
    std::array<avsync::PredictedSourcePosition,480> positions{};
    constexpr std::array<std::uint64_t,2> period{48000,52739};
    std::uint64_t first_input{}, checked{}, calls{};
    double maximum{}, direction{1}, maximum_slew{};
    auto previous_reached=model.reached_ppm();
    while (model.frames()<static_cast<std::uint64_t>(seconds)*48000) {
        if (scenario==3) {
            if (command>=499) direction=-1;
            if (command<=-499) direction=1;
            command=std::clamp(command+direction*.99,-499.,499.);
        }
        for (std::size_t i=0;i<input.size()/2;++i)
            for (std::size_t ch=0;ch<2;++ch)
                input[2*i+ch]=static_cast<float>(-.4+.8*((first_input+i)%period[ch])/period[ch]);
        if (calls%200==0) require(backend.process({},output,-command).status==avsync::AsrcStatus::no_progress,
                                 "empty call unexpectedly progressed");
        const auto result=backend.process(input,output,command);
        require(result.status==avsync::AsrcStatus::progress && result.output_frames_generated==480,
                "partial backend result");
        require(model.advance(negative ? 0:command,positions),"model advance failed");
        first_input+=result.input_frames_used; ++calls;
        maximum_slew=std::max(maximum_slew,std::abs(model.reached_ppm()-previous_reached));
        previous_reached=model.reached_ppm();
        for (std::size_t i=0;i<positions.size();++i) {
            if (positions[i].whole<2048) continue; // Startup padding guard.
            for (std::size_t ch=0;ch<2;++ch) {
                const auto expected=static_cast<double>(positions[i].whole%period[ch])+positions[i].fraction;
                if (expected<2048 || expected>period[ch]-2048) continue; // Ramp wrap guard.
                const double observed=(static_cast<double>(output[2*i+ch])+.4)*period[ch]/.8;
                require(std::isfinite(observed),"non-finite output");
                maximum=std::max(maximum,std::abs(observed-expected)); ++checked;
            }
        }
    }
    require(checked>static_cast<std::uint64_t>(seconds)*48000,"insufficient ramp coverage");
    const bool accepted=maximum<=.05 && maximum_slew<=1.0;
    std::cout<<std::fixed<<std::setprecision(9)<<"{\"scenario\":"<<scenario<<",\"seconds\":"<<seconds
        <<",\"checked_channel_samples\":"<<checked<<",\"max_source_error_frames\":"<<maximum
        <<",\"max_model_endpoint_step_ppm\":"<<maximum_slew
        <<",\"negative_control\":"<<(negative?"true":"false")<<",\"accepted\":"<<(accepted?"true":"false")<<"}\n";
    require(negative ? maximum>1 : accepted,"unexpected phase-model waveform verdict"); return maximum;
}
}
int main(int argc,char** argv) {
    try {
        if (argc==2 && std::string(argv[1])=="--help") {
            std::cout<<"Generated PCM phase-model check only; no devices, network or OBS.\n--seconds [8,600]\n"; return 0;
        }
        unsigned seconds=12;
        if (argc!=1) {
            require(argc==3 && std::string(argv[1])=="--seconds","invalid arguments");
            std::size_t used{}; const std::string value=argv[2]; const auto n=std::stoul(value,&used);
            require(used==value.size() && n>=8 && n<=600,"invalid seconds"); seconds=static_cast<unsigned>(n);
        }
        for (int scenario=0;scenario<4;++scenario) (void)run(seconds,scenario);
        (void)run(12,4); // Deliberately wrong unity model, real +499 ppm DSP.
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

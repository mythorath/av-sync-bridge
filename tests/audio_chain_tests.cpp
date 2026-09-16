// SPDX-License-Identifier: GPL-2.0-or-later
// Generated 8ch/192k -> nominal stereo/48k -> RTP/L24+original anchors -> ASRC.
// No sockets, clocks, devices, playback, PCM files or OBS.
#include "avsync/audio_diagnostic.hpp"
#include "avsync/nominal_audio.hpp"
#include "avsync/rtp_audio_anchor.hpp"
#include <gst/rtp/gstrtpbuffer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool okay,const char* text) { if (!okay) throw std::runtime_error(text); }
struct Delete { void operator()(GstBuffer* b) const { if(b) gst_buffer_unref(b); } };
void run(double ppm) {
    GstAudioInfo info; gst_audio_info_init(&info);
    gst_audio_info_set_format(&info,GST_AUDIO_FORMAT_F32,192000,8,nullptr);
    avsync::audio::NominalAudioConverter::Mix mix{}; mix[0][0]=1;
    avsync::audio::NominalAudioConverter nominal(info,mix,1920);
    auto worker=std::make_unique<avsync::AudioCorrectionWorker>(avsync::SessionToken{9,1},7);
    std::array<float,1920*8> input{};
    std::array<float,2048> converted{},out{};
    constexpr std::array<long double,5> markers{4.321L,5.987L,7.413L,9.731L,10.139L};
    constexpr std::array<unsigned,8> parts{1,17,180,71,149,32,113,7};
    std::array<long double,5> found{};
    const long double rate=192000.L*(1+ppm/1e6L);
    std::uint64_t captured{},wire{},packets{},delivered{};
    float event_peak{}; long double event_time{},maximum{}; std::size_t events{};
    while (captured<12*192000) {
        for (std::size_t i=0;i<1920;++i) {
            const auto t=(captured+i)/rate; long double value{};
            for (auto marker:markers) {
                const auto distance=(t-marker)/.0003L;
                if (std::abs(distance)<10) value+=.6L*std::exp(-.5L*distance*distance);
            }
            input[i*8]=static_cast<float>(value);
        }
        const auto conversion=nominal.process(std::as_bytes(std::span(input)),1920,converted);
        require(conversion.status==avsync::audio::NominalAudioStatus::progress && conversion.input_frames_used==1920,
                "nominal conversion failed");
        captured+=1920;
        const auto now=1'004'000'000+static_cast<avsync::Nanoseconds>(std::floor(captured*1e9L/rate));
        std::size_t offset{};
        while (offset<conversion.output_frames_generated) {
            const auto frames=static_cast<unsigned>(std::min<std::size_t>(parts[packets%parts.size()],conversion.output_frames_generated-offset));
            avsync::wire::AudioRecord r;
            r.epoch={9,1}; r.clock_epoch=7; r.source_rate=192000; r.device_origin=1234;
            r.rtp_zero=0xffffff00U; r.packet_wire_start=wire; r.anchor_sequence=wire/960;
            r.device_position=1234+r.anchor_sequence*3840;
            r.capture_ns=1'000'000'000+static_cast<avsync::Nanoseconds>(std::floor(r.anchor_sequence*3840*1e9L/rate));
            r.qpc_100ns=static_cast<std::uint64_t>(r.capture_ns/100); r.calibration_revision=1;
            auto* raw=gst_rtp_buffer_new_allocate(frames*6,0,0); require(raw,"RTP allocation failed");
            GstRTPBuffer packet=GST_RTP_BUFFER_INIT;
            require(gst_rtp_buffer_map(raw,GST_MAP_READWRITE,&packet),"RTP write map failed");
            gst_rtp_buffer_set_payload_type(&packet,96); gst_rtp_buffer_set_ssrc(&packet,11);
            gst_rtp_buffer_set_seq(&packet,static_cast<guint16>(packets));
            gst_rtp_buffer_set_timestamp(&packet,static_cast<guint32>(r.rtp_zero+wire));
            auto* payload=static_cast<guint8*>(gst_rtp_buffer_get_payload(&packet));
            for (std::size_t i=0;i<frames*2;++i) {
                const auto x=converted[offset*2+i]; require(std::abs(x)<1,"fixture lacks headroom");
                const auto bits=static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(x*8388608.F)));
                payload[i*3]=static_cast<guint8>(bits>>16); payload[i*3+1]=static_cast<guint8>(bits>>8); payload[i*3+2]=static_cast<guint8>(bits);
            }
            gst_rtp_buffer_unmap(&packet);
            require(avsync::net::add_audio_anchor(raw,r),"anchor extension failed");
            std::unique_ptr<GstBuffer,Delete> owned(raw);
            avsync::DiagnosticAudioPacket decoded;
            const auto record=avsync::net::read_audio_anchor(raw,decoded.ssrc,decoded.timestamp,decoded.frames);
            require(record.has_value(),"anchor read failed"); decoded.record=*record;
            require(gst_rtp_buffer_map(raw,GST_MAP_READ,&packet),"RTP read map failed");
            const bool ok=avsync::decode_l24(std::span(static_cast<const std::byte*>(gst_rtp_buffer_get_payload(&packet)),
                gst_rtp_buffer_get_payload_len(&packet)),decoded);
            gst_rtp_buffer_unmap(&packet); require(ok,"L24 decode failed");
            require(worker->push(decoded.record,decoded.ssrc,decoded.timestamp,
                std::span(decoded.pcm).first(frames*2),now,true)!=avsync::CorrectionPush::rejected,"correction rejected chain");
            (void)worker->dispatch(now,true);
            while (const auto block=worker->pull(out,now,true)) {
                require(block->first_frame==delivered,"output index gap");
                for (std::size_t i=0;i<block->frames;++i) {
                    const auto x=out[i*2]; require(std::isfinite(x) && out[i*2+1]==0,"invalid/leaking output");
                    const auto t=(block->capture_grid_ns-1'000'000'000)/1e9L+i/48000.L;
                    if (x>.1F) { if (x>event_peak) { event_peak=x; event_time=t; } }
                    else if (event_peak) {
                        require(events<markers.size() && event_peak>.5F,"extra/weak marker");
                        found[events++]=event_time; event_peak=0;
                    }
                }
                delivered+=block->frames;
            }
            wire+=frames; offset+=frames; ++packets;
        }
    }
    require(events==markers.size() && !event_peak,"missing/unfinished markers");
    for (std::size_t i=0;i<markers.size();++i) maximum=std::max(maximum,std::abs(found[i]-markers[i]));
    require(maximum<=1.L/48000 && worker->state()==avsync::CorrectionState::running,"combined phase failure");
    std::cout<<"{\"ppm\":"<<ppm<<",\"packets\":"<<packets<<",\"wire_frames\":"<<wire
        <<",\"delivered_frames\":"<<delivered<<",\"markers\":"<<events
        <<",\"maximum_marker_error_ms\":"<<static_cast<double>(maximum*1000)<<",\"passed\":true}\n";
}
}
int main() {
    try { gst_init(nullptr,nullptr); for (const auto ppm:{0.,499.,-499.}) run(ppm); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

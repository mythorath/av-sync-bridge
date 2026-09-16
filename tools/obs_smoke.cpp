// SPDX-License-Identifier: GPL-2.0-or-later
// Isolated libOBS render/mixer/encoder test. Never opens an OBS profile or device.
#include <obs/obs.h>
#include <obs/obs-nix-platform.h>
#include <obs/util/platform.h>
#include <X11/Xlib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
template<class T, void (*Destroy)(T*)> struct Release {
    void operator()(T* value) const { if (value) Destroy(value); }
};
using Data = std::unique_ptr<obs_data_t, Release<obs_data_t, obs_data_release>>;
using Source = std::unique_ptr<obs_source_t, Release<obs_source_t, obs_source_release>>;
void release_scene(obs_scene_t* scene) {
    // OBS 32.2's main canvas also owns a scene reference. Remove that ownership
    // while modules/graphics are still loaded, then release our own reference.
    obs_canvas_scene_remove(scene);
    obs_scene_release(scene);
}
using Scene = std::unique_ptr<obs_scene_t, Release<obs_scene_t, release_scene>>;
using Encoder = std::unique_ptr<obs_encoder_t, Release<obs_encoder_t, obs_encoder_release>>;
using Output = std::unique_ptr<obs_output_t, Release<obs_output_t, obs_output_release>>;
struct ObsLifetime { ~ObsLifetime() { obs_wait_for_destroy_queue(); obs_shutdown(); } };
struct DisplayLifetime { Display* value{}; ~DisplayLifetime() { if (value) XCloseDisplay(value); } };
struct Channels { ~Channels() { for (unsigned i = 0; i != 3; ++i) obs_set_output_source(i, nullptr); } };
void require(bool condition, const std::string& why) { if (!condition) throw std::runtime_error(why); }
void load_module(const std::string& path, const std::string& data) {
    obs_module_t* module{};
    require(obs_open_module(&module, path.c_str(), data.c_str()) == MODULE_SUCCESS,
            "Cannot open module: " + path);
    require(obs_init_module(module), "Cannot initialize module: " + path);
}
void stopped(void* opaque, calldata_t*) { static_cast<std::atomic<bool>*>(opaque)->store(true); }
struct TracePoint { std::uint64_t timestamp; std::uint32_t frames; double rms; };
struct MixTrace {
    std::vector<TracePoint> points;
    bool overflow{};
};
void observe_mix(void* opaque, size_t, audio_data* audio) {
    auto& trace = *static_cast<MixTrace*>(opaque);
    const auto* samples = reinterpret_cast<const float*>(audio->data[0]);
    if (!samples) return;
    // Bounded, preallocated test diagnostics. No disk I/O in callback.
    for (std::uint32_t start = 0; start < audio->frames; start += 48) {
        if (trace.points.size() == trace.points.capacity()) { trace.overflow = true; return; }
        const auto frames = std::min<std::uint32_t>(48, audio->frames - start);
        double squares = 0;
        for (std::uint32_t i = 0; i < frames; ++i) squares += double(samples[start+i]) * samples[start+i];
        trace.points.push_back({audio->timestamp + std::uint64_t(start) * 1'000'000'000 / 48000,
                                frames, std::sqrt(squares / frames)});
    }
}
struct TraceSubscriptions {
    std::array<MixTrace, 2> traces;
    bool attached{};
    void attach(unsigned seconds) {
        audio_convert_info conversion{};
        conversion.samples_per_sec = 48000;
        conversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
        conversion.speakers = SPEAKERS_MONO;
        for (auto& trace : traces) trace.points.reserve(std::size_t(seconds + 10) * 1100);
        for (size_t i = 0; i < traces.size(); ++i)
            obs_add_raw_audio_callback(i, &conversion, observe_mix, &traces[i]);
        attached = true;
    }
    void detach() {
        if (!attached) return;
        for (size_t i = 0; i < traces.size(); ++i)
            obs_remove_raw_audio_callback(i, observe_mix, &traces[i]);
        attached = false;
    }
    ~TraceSubscriptions() { detach(); }
    void save(const std::string& recording) {
        detach();
        for (size_t i = 0; i < traces.size(); ++i) {
            require(!traces[i].overflow, "Mixer diagnostic capacity exceeded");
            const auto path = recording + ".mix" + std::to_string(i) + ".csv";
            auto* file = std::fopen(path.c_str(), "wx");
            require(file != nullptr, "Cannot create new mixer diagnostic file");
            bool failed = std::fprintf(file, "timestamp_ns,frames,rms\n") < 0;
            for (const auto& point : traces[i].points)
                if (std::fprintf(file, "%llu,%u,%.9g\n", static_cast<unsigned long long>(point.timestamp),
                                 point.frames, point.rms) < 0) { failed = true; break; }
            if (std::fclose(file) != 0) failed = true;
            require(!failed, "Cannot write mixer diagnostics");
        }
    }
};
struct SourceTracePoint {
    std::uint64_t timestamp, callback_timestamp;
    std::uint32_t frames;
    double rms;
    bool muted;
};
struct SourceTrace {
    std::vector<SourceTracePoint> points;
    bool overflow{}, invalid{};
};
void observe_source(void* opaque, obs_source_t*, const audio_data* audio, bool muted) {
    auto& trace = *static_cast<SourceTrace*>(opaque);
    if (trace.overflow || trace.invalid) return;
    // OBS 32.2 converts source audio to the main planar-float output format
    // before this callback. attach() requires 48 kHz stereo. The callback sees
    // post-filter PCM and its timestamp, BEFORE the internal timestamp smoothing
    // and sync-offset adjustments used to place that PCM in the mixer queue.
    // It is not a device-capture timestamp and does not prove mixer placement.
    if (!audio || !audio->data[0] || !audio->data[1] || audio->frames > 48000) {
        trace.invalid = true;
        return;
    }
    const auto duration = std::uint64_t(audio->frames) * 1'000'000'000 / 48000;
    constexpr auto timestamp_limit = std::uint64_t(std::numeric_limits<std::int64_t>::max());
    if (audio->timestamp > timestamp_limit || duration > timestamp_limit - audio->timestamp) {
        trace.invalid = true;
        return;
    }
    const auto* left = reinterpret_cast<const float*>(audio->data[0]);
    const auto* right = reinterpret_cast<const float*>(audio->data[1]);
    const auto callback_timestamp = os_gettime_ns();
    // This summary deliberately keeps PCM activity even when OBS says muted;
    // muted is recorded separately so silence and suppression are not conflated.
    // No allocation, OBS re-entry, lock, or I/O in this serialized callback.
    for (std::uint32_t start = 0; start < audio->frames; start += 48) {
        if (trace.points.size() == trace.points.capacity()) { trace.overflow = true; return; }
        const auto frames = std::min<std::uint32_t>(48, audio->frames - start);
        double squares = 0;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const double sample = (double(left[start+i]) + right[start+i]) * 0.5;
            if (!std::isfinite(sample)) { trace.invalid = true; return; }
            squares += sample * sample;
        }
        trace.points.push_back({audio->timestamp + std::uint64_t(start) * 1'000'000'000 / 48000,
                                callback_timestamp, frames, std::sqrt(squares / frames), muted});
    }
}
struct SourceTraceSubscriptions {
    std::array<SourceTrace, 2> traces;
    std::array<Source, 2> sources;
    size_t attached{};
    void attach(unsigned seconds, obs_source_t* desktop, obs_source_t* microphone) {
        const auto* format = audio_output_get_info(obs_get_audio());
        require(format && format->samples_per_sec == 48000 &&
                format->format == AUDIO_FORMAT_FLOAT_PLANAR && format->speakers == SPEAKERS_STEREO,
                "Source diagnostics require 48 kHz planar-float stereo OBS audio");
        for (auto& trace : traces) trace.points.reserve(std::size_t(seconds + 10) * 1100);
        sources[0].reset(obs_source_get_ref(desktop));
        sources[1].reset(obs_source_get_ref(microphone));
        require(sources[0] && sources[1], "Cannot retain diagnostic audio sources");
        for (size_t i = 0; i < traces.size(); ++i) {
            obs_source_add_audio_capture_callback(sources[i].get(), observe_source, &traces[i]);
            ++attached;
        }
    }
    void detach() {
        // OBS removes under the same mutex held while invoking callbacks. Once
        // this returns, save/destruction cannot race an in-flight trace writer.
        while (attached) {
            const auto i = --attached;
            obs_source_remove_audio_capture_callback(sources[i].get(), observe_source, &traces[i]);
        }
    }
    ~SourceTraceSubscriptions() { detach(); }
    void save(const std::string& recording) {
        detach();
        for (size_t i = 0; i < traces.size(); ++i) {
            require(!traces[i].overflow, "Source diagnostic capacity exceeded");
            require(!traces[i].invalid, "Source diagnostic received invalid audio metadata or PCM");
            const auto path = recording + ".source" + std::to_string(i) + ".csv";
            auto* file = std::fopen(path.c_str(), "wx");
            require(file != nullptr, "Cannot create new source diagnostic file");
            bool failed = std::fprintf(file, "timestamp_ns,frames,rms,muted,callback_ns\n") < 0;
            for (const auto& point : traces[i].points)
                if (std::fprintf(file, "%llu,%u,%.9g,%u,%llu\n",
                                 static_cast<unsigned long long>(point.timestamp), point.frames,
                                 point.rms, unsigned(point.muted),
                                 static_cast<unsigned long long>(point.callback_timestamp)) < 0) {
                    failed = true;
                    break;
                }
            if (std::fclose(file) != 0) failed = true;
            require(!failed, "Cannot write source diagnostics");
        }
    }
};
struct VideoTracePoint {
    std::uint64_t timestamp, callback_timestamp;
    unsigned cyan_samples;
};
struct VideoTrace {
    std::vector<VideoTracePoint> points;
    bool overflow{}, invalid{};
};
void observe_video(void* opaque, video_data* frame) {
    auto& trace = *static_cast<VideoTrace*>(opaque);
    if (trace.overflow || trace.invalid) return;
    if (trace.points.size() == trace.points.capacity()) { trace.overflow = true; return; }
    if (!frame || !frame->data[0] || !frame->data[1] ||
        frame->linesize[0] < 640 || frame->linesize[1] < 640 ||
        frame->linesize[0] > 1'048'576 || frame->linesize[1] > 1'048'576 ||
        frame->timestamp > std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
        trace.invalid = true;
        return;
    }
    const auto callback_timestamp = os_gettime_ns();
    unsigned cyan = 0;
    // Sample an 80x45 grid directly from the configured 640x360 NV12 output.
    // No frame copies, scaling buffers, allocation, or I/O. BT.709 limited-range
    // YCbCr -> RGB uses the same cyan thresholds as measure_physical.py. Sparse
    // sampling is not that helper's area scaling, so counts need not be equal.
    for (unsigned y = 4; y < 360; y += 8) {
        const auto* luma = frame->data[0] + std::size_t(y) * frame->linesize[0];
        const auto* chroma = frame->data[1] + std::size_t(y / 2) * frame->linesize[1];
        for (unsigned x = 4; x < 640; x += 8) {
            const double luminance = (double(luma[x]) - 16) * (255.0 / 219.0);
            const double cb = (double(chroma[x & ~1u]) - 128) * (255.0 / 224.0);
            const double cr = (double(chroma[(x & ~1u) + 1]) - 128) * (255.0 / 224.0);
            const double red = std::clamp(luminance + 1.5748 * cr, 0.0, 255.0);
            const double green = std::clamp(luminance - 0.187324 * cb - 0.468124 * cr, 0.0, 255.0);
            const double blue = std::clamp(luminance + 1.8556 * cb, 0.0, 255.0);
            if (green > 125 && blue > 115 && green - red > 60 && blue - red > 50 &&
                std::abs(green - blue) < 60) ++cyan;
        }
    }
    trace.points.push_back({frame->timestamp, callback_timestamp, cyan});
}
struct VideoTraceSubscription {
    VideoTrace trace;
    bool attached{};
    void attach(unsigned seconds) {
        const auto* format = video_output_get_info(obs_get_video());
        require(format && format->format == VIDEO_FORMAT_NV12 && format->width == 640 &&
                format->height == 360 && format->colorspace == VIDEO_CS_709 &&
                format->range == VIDEO_RANGE_PARTIAL,
                "Video diagnostics require 640x360 limited-range BT.709 NV12 output");
        trace.points.reserve(std::size_t(seconds + 10) * 61);
        // Attach only once the recording is active, so diagnostics do not make
        // the raw-video pipeline start earlier than it would for the encoder.
        obs_add_raw_video_callback(nullptr, observe_video, &trace);
        attached = true;
    }
    void detach() {
        if (!attached) return;
        // video_output_disconnect takes the callback dispatch mutex, fencing
        // the last writer before this object reads or releases trace storage.
        obs_remove_raw_video_callback(observe_video, &trace);
        attached = false;
    }
    ~VideoTraceSubscription() { detach(); }
    void save(const std::string& recording) {
        detach();
        require(!trace.overflow, "Video diagnostic capacity exceeded");
        require(!trace.invalid && !trace.points.empty(), "Video diagnostic received no valid video");
        const auto path = recording + ".video.csv";
        auto* file = std::fopen(path.c_str(), "wx");
        require(file != nullptr, "Cannot create new video diagnostic file");
        bool failed = std::fprintf(file, "timestamp_ns,callback_ns,cyan_samples,sampled_pixels\n") < 0;
        for (const auto& point : trace.points)
            if (std::fprintf(file, "%llu,%llu,%u,3600\n",
                             static_cast<unsigned long long>(point.timestamp),
                             static_cast<unsigned long long>(point.callback_timestamp),
                             point.cyan_samples) < 0) { failed = true; break; }
        if (std::fclose(file) != 0) failed = true;
        require(!failed, "Cannot write video diagnostics");
    }
};
int integer(const char* value, int minimum, int maximum, const char* label) {
    std::size_t parsed{};
    const auto result = std::stoi(value, &parsed);
    require(parsed == std::string(value).size() && result >= minimum && result <= maximum,
            std::string("Invalid ") + label);
    return result;
}
int run(int argc, char** argv) {
    if (argc < 7 || (argc - 7) % 2 != 0) {
        std::cerr << "Usage: avsync-obs-smoke IPC_PATH OUTPUT.mkv ADAPTER.so OBS_PLUGIN_DIR OBS_DATA_DIR SECONDS\n"
                     " [--audio-lead-ms 0..100] [--warmup-seconds 1..30] [--stagger-ms 0..2000]\n"
                     " [--scenario baseline|hide|mute|rapid-mute]\n"
                     " [--desktop-ipc-path FILE] [--fit-video 0|1]\n"
                     "Run on a private test display (for example xvfb-run). No capture devices are used.\n";
        return 2;
    }
    const auto seconds = integer(argv[6], 3, 120, "duration (3..120 seconds)");
    int lead_ms = 40, warmup = 1, stagger_ms = 0, fit_video = 0;
    std::string scenario = "baseline";
    std::string desktop_ipc = argv[1];
    for (int i = 7; i < argc; i += 2) {
        const std::string option(argv[i]);
        if (option == "--audio-lead-ms") lead_ms = integer(argv[i+1], 0, 100, "audio lead");
        else if (option == "--warmup-seconds") warmup = integer(argv[i+1], 1, 30, "warmup");
        else if (option == "--stagger-ms") stagger_ms = integer(argv[i+1], 0, 2000, "source stagger");
        else if (option == "--scenario") scenario = argv[i+1];
        else if (option == "--desktop-ipc-path") desktop_ipc = argv[i+1];
        else if (option == "--fit-video") fit_video = integer(argv[i+1], 0, 1, "fit video");
        else throw std::runtime_error("Unknown option: " + option);
    }
    require(scenario == "baseline" || scenario == "hide" || scenario == "mute" || scenario == "rapid-mute",
            "Invalid scenario");
    require(!desktop_ipc.empty(), "Desktop IPC path must not be empty");
    require(!std::filesystem::exists(std::filesystem::symlink_status(argv[2])),
            "Refusing to overwrite an existing recording or symlink");
    require(std::filesystem::path(argv[2]).extension() == ".mkv", "Use a new .mkv output path");
    XInitThreads();
    DisplayLifetime display{XOpenDisplay(nullptr)};
    require(display.value, "No X display; run under xvfb-run or an isolated desktop");
    obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
    obs_set_nix_platform_display(display.value);
    require(obs_startup("en-US", nullptr, nullptr), "obs_startup failed");
    ObsLifetime obs;
    obs_audio_info audio{};
    audio.samples_per_sec = 48000;
    audio.speakers = SPEAKERS_STEREO;
    require(obs_reset_audio(&audio), "Audio reset failed");
    obs_video_info video{};
    video.graphics_module = "libobs-opengl";
    video.fps_num = 60; video.fps_den = 1;
    video.base_width = video.output_width = 640;
    video.base_height = video.output_height = 360;
    video.output_format = VIDEO_FORMAT_NV12;
    video.gpu_conversion = true;
    video.colorspace = VIDEO_CS_709;
    video.range = VIDEO_RANGE_PARTIAL;
    video.scale_type = OBS_SCALE_BICUBIC;
    require(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "Video reset failed");
    const std::filesystem::path plugin_dir(argv[4]), data_dir(argv[5]);
    load_module((plugin_dir / "obs-x264.so").string(), (data_dir / "obs-x264").string());
    load_module((plugin_dir / "obs-ffmpeg.so").string(), (data_dir / "obs-ffmpeg").string());
    load_module(argv[3], std::filesystem::path(argv[3]).parent_path().string());
    obs_post_load_modules();
    Data settings(obs_data_create());
    obs_data_set_string(settings.get(), "ipc_path", argv[1]);
    obs_data_set_int(settings.get(), "handoff_lead_ms", lead_ms);
    Source picture(obs_source_create("avsync_prototype_video", "Test picture", settings.get(), nullptr));
    std::cout << "AVSYNC_VIDEO_CREATED monotonic_ns=" << os_gettime_ns() << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(stagger_ms));
    Data desktop_settings(obs_data_create());
    obs_data_set_string(desktop_settings.get(), "ipc_path", desktop_ipc.c_str());
    obs_data_set_int(desktop_settings.get(), "handoff_lead_ms", lead_ms);
    Source desktop(obs_source_create("avsync_prototype_desktop", "Test desktop", desktop_settings.get(), nullptr));
    std::this_thread::sleep_for(std::chrono::milliseconds(stagger_ms));
    Source mic(obs_source_create("avsync_prototype_microphone", "Test microphone", settings.get(), nullptr));
    require(picture && desktop && mic, "Prototype sources could not be created");
    obs_source_set_audio_mixers(desktop.get(), 1u);
    obs_source_set_audio_mixers(mic.get(), 2u);
    obs_source_set_monitoring_type(desktop.get(), OBS_MONITORING_TYPE_NONE);
    obs_source_set_monitoring_type(mic.get(), OBS_MONITORING_TYPE_NONE);
    Scene scene(obs_scene_create("Isolated timing test"));
    require(scene != nullptr, "Cannot create test scene");
    auto* scene_item = obs_scene_add(scene.get(), picture.get());
    require(scene_item != nullptr, "Cannot construct test scene");
    if (fit_video) {
        // Bounds are evaluated when the source worker learns its dimensions.
        // Never crop a physical 4K source to this 640x360 test canvas.
        vec2 bounds{};
        bounds.x = static_cast<float>(video.base_width);
        bounds.y = static_cast<float>(video.base_height);
        obs_sceneitem_set_bounds_type(scene_item, OBS_BOUNDS_SCALE_INNER);
        obs_sceneitem_set_bounds(scene_item, &bounds);
    }
    obs_set_output_source(0, obs_scene_get_source(scene.get()));
    obs_set_output_source(1, desktop.get());
    obs_set_output_source(2, mic.get());
    Channels channels;
    Data encoder_settings(obs_data_create());
    obs_data_set_string(encoder_settings.get(), "rate_control", "CRF");
    obs_data_set_int(encoder_settings.get(), "crf", 16);
    obs_data_set_string(encoder_settings.get(), "preset", "ultrafast");
    obs_data_set_string(encoder_settings.get(), "tune", "zerolatency");
    Encoder picture_encoder(obs_video_encoder_create("obs_x264", "Test x264", encoder_settings.get(), nullptr));
    Data audio_settings(obs_data_create());
    obs_data_set_int(audio_settings.get(), "bitrate", 192);
    Encoder desktop_encoder(obs_audio_encoder_create("ffmpeg_aac", "Test desktop AAC", audio_settings.get(), 0, nullptr));
    Encoder mic_encoder(obs_audio_encoder_create("ffmpeg_aac", "Test mic AAC", audio_settings.get(), 1, nullptr));
    require(picture_encoder && desktop_encoder && mic_encoder, "Cannot construct test encoders");
    obs_encoder_set_video(picture_encoder.get(), obs_get_video());
    obs_encoder_set_audio(desktop_encoder.get(), obs_get_audio());
    obs_encoder_set_audio(mic_encoder.get(), obs_get_audio());
    Data output_settings(obs_data_create());
    obs_data_set_string(output_settings.get(), "path", argv[2]);
    Output output(obs_output_create("ffmpeg_muxer", "Private test recording", output_settings.get(), nullptr));
    require(output != nullptr, "Cannot create muxer output");
    obs_output_set_video_encoder(output.get(), picture_encoder.get());
    obs_output_set_audio_encoder(output.get(), desktop_encoder.get(), 0);
    obs_output_set_audio_encoder(output.get(), mic_encoder.get(), 1);
    TraceSubscriptions diagnostics;
    diagnostics.attach(static_cast<unsigned>(seconds + warmup));
    SourceTraceSubscriptions source_diagnostics;
    source_diagnostics.attach(static_cast<unsigned>(seconds + warmup), desktop.get(), mic.get());
    std::cout << "AVSYNC_READY monotonic_ns=" << os_gettime_ns() << std::endl;
    // Producer may start during warmup; no live source or profile is inspected.
    std::this_thread::sleep_for(std::chrono::seconds(warmup));
    require(obs_output_start(output.get()), "Recording did not start");
    VideoTraceSubscription video_diagnostics;
    video_diagnostics.attach(static_cast<unsigned>(seconds));
    const auto started = std::chrono::steady_clock::now();
    std::cout << "AVSYNC_RECORDING_STARTED monotonic_ns=" << os_gettime_ns()
              << " lead_ms=" << lead_ms << " scenario=" << scenario << std::endl;
    bool first_event = false, second_event = false;
    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(seconds)) {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (!first_event && elapsed >= std::chrono::seconds(5)) {
            first_event = true;
            if (scenario == "hide") obs_sceneitem_set_visible(scene_item, false);
            if (scenario == "mute" || scenario == "rapid-mute") obs_source_set_muted(mic.get(), true);
            if (scenario == "rapid-mute") obs_source_set_muted(mic.get(), false);
            if (scenario != "baseline") std::cout << "AVSYNC_EVENT first monotonic_ns=" << os_gettime_ns() << std::endl;
        }
        if (!second_event && elapsed >= std::chrono::seconds(6)) {
            second_event = true;
            if (scenario == "hide") obs_sceneitem_set_visible(scene_item, true);
            if (scenario == "mute") obs_source_set_muted(mic.get(), false);
            if (scenario != "baseline") std::cout << "AVSYNC_EVENT second monotonic_ns=" << os_gettime_ns() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::atomic<bool> finished{false};
    auto* signals = obs_output_get_signal_handler(output.get());
    signal_handler_connect(signals, "stop", stopped, &finished);
    obs_output_stop(output.get());
    for (int n = 0; n != 500 && !finished.load(); ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!finished.load()) obs_output_force_stop(output.get());
    signal_handler_disconnect(signals, "stop", stopped, &finished);
    require(finished.load(), "Output did not stop cleanly within five seconds");
    diagnostics.detach();
    source_diagnostics.detach();
    video_diagnostics.detach();
    diagnostics.save(argv[2]);
    source_diagnostics.save(argv[2]);
    video_diagnostics.save(argv[2]);
    std::cout << "Isolated recording finished; inspect encoded timing before claiming sync.\n";
    return 0;
}
}
int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Isolated libOBS render/mixer/encoder test. Never opens an OBS profile or device.
#include <obs/obs.h>
#include <obs/obs-nix-platform.h>
#include <X11/Xlib.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

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
int run(int argc, char** argv) {
    if (argc != 7) {
        std::cerr << "Usage: avsync-obs-smoke IPC_PATH OUTPUT.mkv ADAPTER.so OBS_PLUGIN_DIR OBS_DATA_DIR SECONDS\n"
                     "Run on a private test display (for example xvfb-run). No capture devices are used.\n";
        return 2;
    }
    std::size_t parsed{};
    const auto seconds = std::stoi(argv[6], &parsed);
    require(parsed == std::string(argv[6]).size() && seconds >= 3 && seconds <= 120,
            "Duration must be an integer from 3 to 120 seconds");
    require(!std::filesystem::exists(argv[2]), "Refusing to overwrite an existing recording");
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
    Source picture(obs_source_create("avsync_prototype_video", "Synthetic picture", settings.get(), nullptr));
    Source desktop(obs_source_create("avsync_prototype_desktop", "Synthetic desktop", settings.get(), nullptr));
    Source mic(obs_source_create("avsync_prototype_microphone", "Synthetic microphone", settings.get(), nullptr));
    require(picture && desktop && mic, "Prototype sources could not be created");
    obs_source_set_audio_mixers(desktop.get(), 1u);
    obs_source_set_audio_mixers(mic.get(), 2u);
    obs_source_set_monitoring_type(desktop.get(), OBS_MONITORING_TYPE_NONE);
    obs_source_set_monitoring_type(mic.get(), OBS_MONITORING_TYPE_NONE);
    Scene scene(obs_scene_create("Synthetic timing test"));
    require(scene && obs_scene_add(scene.get(), picture.get()), "Cannot construct test scene");
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
    Output output(obs_output_create("ffmpeg_muxer", "Private synthetic recording", output_settings.get(), nullptr));
    require(output != nullptr, "Cannot create muxer output");
    obs_output_set_video_encoder(output.get(), picture_encoder.get());
    obs_output_set_audio_encoder(output.get(), desktop_encoder.get(), 0);
    obs_output_set_audio_encoder(output.get(), mic_encoder.get(), 1);
    // Let the independent sources attach. Producer is responsible for priming.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    require(obs_output_start(output.get()), "Recording did not start");
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    std::atomic<bool> finished{false};
    auto* signals = obs_output_get_signal_handler(output.get());
    signal_handler_connect(signals, "stop", stopped, &finished);
    obs_output_stop(output.get());
    for (int n = 0; n != 500 && !finished.load(); ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!finished.load()) obs_output_force_stop(output.get());
    signal_handler_disconnect(signals, "stop", stopped, &finished);
    require(finished.load(), "Output did not stop cleanly within five seconds");
    std::cout << "Synthetic recording finished; inspect encoded timing before claiming sync.\n";
    return 0;
}
}
int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

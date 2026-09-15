// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental synthetic-only bridge. No capture or network code belongs here.
#include <obs/obs-module.h>
#include <obs/graphics/graphics.h>
#include <obs/util/bmem.h>

#include <avsync/ipc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Experimental shared-clock synthetic video and separate PCM inputs";
}

namespace {
using avsync::ipc::Config;
using avsync::ipc::FrameInfo;
using avsync::ipc::Reader;
using avsync::ipc::ReadResult;
constexpr auto ns_per_second = std::int64_t{1'000'000'000};
constexpr auto retry_period = std::chrono::milliseconds(250);
constexpr auto stale_video_ns = std::int64_t{250'000'000};

// Original shader. Input contract: tightly packed 8-bit NV12, BT.709 limited
// range, SDR. An HDR/other-matrix frame must not silently use this conversion.
constexpr const char *nv12_effect = R"effect(
uniform float4x4 ViewProj;
uniform texture2d luma_image;
uniform texture2d chroma_image;
uniform bool linear_output;
sampler_state linear_sampler {
    Filter = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};
struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertData VSDefault(VertData v) {
    VertData result;
    result.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);
    result.uv = v.uv;
    return result;
}
float3 srgb_to_linear(float3 c) {
    return float3(c.r <= 0.04045 ? c.r / 12.92 : pow((c.r + 0.055) / 1.055, 2.4),
                  c.g <= 0.04045 ? c.g / 12.92 : pow((c.g + 0.055) / 1.055, 2.4),
                  c.b <= 0.04045 ? c.b / 12.92 : pow((c.b + 0.055) / 1.055, 2.4));
}
float4 PSNV12(VertData v) : TARGET {
    float y = (luma_image.Sample(linear_sampler, v.uv).r - 16.0 / 255.0) * (255.0 / 219.0);
    float2 uv = chroma_image.Sample(linear_sampler, v.uv).rg - float2(128.0 / 255.0, 128.0 / 255.0);
    float3 rgb = saturate(float3(y + 1.79274107 * uv.y,
                                y - 0.21324861 * uv.x - 0.53290933 * uv.y,
                                y + 2.11240179 * uv.x));
    return float4(linear_output ? srgb_to_linear(rgb) : rgb, 1.0);
}
technique Draw {
    pass {
        vertex_shader = VSDefault(v);
        pixel_shader = PSNV12(v);
    }
}
)effect";

std::string default_path()
{
    if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return std::string(runtime) + "/avsync-bridge/media.ipc";
    // The service refuses unsafe directories. Do not fall back to a shared /tmp file.
    return {};
}

void source_defaults(obs_data_t *settings)
{
    const auto path = default_path();
    obs_data_set_default_string(settings, "ipc_path", path.c_str());
}

obs_properties_t *source_properties(void *)
{
    auto *properties = obs_properties_create();
    obs_properties_add_path(properties, "ipc_path", "Synthetic bridge IPC file",
                            OBS_PATH_FILE, "IPC files (*.ipc);;All files (*)", nullptr);
    return properties;
}

class WorkerControl {
public:
    explicit WorkerControl(obs_data_t *settings) : path_(obs_data_get_string(settings, "ipc_path")) {}
    void update(obs_data_t *settings)
    {
        {
            std::lock_guard lock(path_mutex_);
            path_ = obs_data_get_string(settings, "ipc_path");
        }
        changed_.store(true);
        wake_.notify_all();
    }
    void stop()
    {
        stopping_.store(true);
        wake_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }
protected:
    std::string path()
    {
        std::lock_guard lock(path_mutex_);
        return path_;
    }
    template<class Duration> void wait_for(Duration duration)
    {
        std::unique_lock lock(wait_mutex_);
        wake_.wait_for(lock, duration, [this] { return stopping_.load() || changed_.load(); });
    }
    std::atomic<bool> stopping_{false}, changed_{false};
    std::thread thread_;
private:
    std::mutex path_mutex_, wait_mutex_;
    std::condition_variable wake_;
    std::string path_;
};

class VideoSource final : public WorkerControl {
public:
    explicit VideoSource(obs_data_t *settings) : WorkerControl(settings)
    {
        thread_ = std::thread([this] { reconnect_worker(); });
    }
    ~VideoSource()
    {
        stop();
        obs_enter_graphics();
        gs_texture_destroy(y_texture_);
        gs_texture_destroy(uv_texture_);
        gs_effect_destroy(effect_);
        obs_leave_graphics();
    }
    std::uint32_t width() const { return width_.load(); }
    std::uint32_t height() const { return height_.load(); }
    void tick()
    {
        // Reader creation/replacement is worker-only. Never wait for that worker.
        std::unique_lock lock(reader_mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !reader_)
            return;
        const auto tick = obs_get_video_frame_time();
        if (tick > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return;
        FrameInfo frame;
        const auto result = reader_->read_latest_due_video(static_cast<std::int64_t>(tick), pixels_, frame,
                                                           stale_video_ns);
        if (result == ReadResult::ok) {
            // Defense in depth at the OBS boundary, independent of IPC validation.
            if (frame.presentation_ns < 0 || frame.presentation_ns > static_cast<std::int64_t>(tick) ||
                frame.bytes != pixels_.size())
                return;
            frame_ = frame;
            frame_changed_ = true;
        } else if (result == ReadResult::disconnected || result == ReadResult::invalid) {
            reconnect_needed_.store(true);
        }
    }
    void render()
    {
        std::unique_lock lock(reader_mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !reader_ || frame_.presentation_ns <= 0)
            return;
        const auto tick = static_cast<std::int64_t>(obs_get_video_frame_time());
        if (tick < frame_.presentation_ns || tick - frame_.presentation_ns > stale_video_ns)
            return; // Transparent rather than indefinitely showing stale content.
        if (!effect_ && !effect_failed_) {
            char *error = nullptr;
            effect_ = gs_effect_create(nv12_effect, "avsync-nv12", &error);
            if (!effect_) {
                blog(LOG_ERROR, "[avsync] NV12 shader creation failed: %s", error ? error : "unknown");
                effect_failed_ = true;
            }
            bfree(error);
        }
        if (!effect_)
            return;
        const auto width = width_.load();
        const auto height = height_.load();
        if (texture_width_ != width || texture_height_ != height) {
            gs_texture_destroy(y_texture_);
            gs_texture_destroy(uv_texture_);
            y_texture_ = gs_texture_create(width, height, GS_R8, 1, nullptr, GS_DYNAMIC);
            uv_texture_ = gs_texture_create(width / 2, height / 2, GS_R8G8, 1, nullptr, GS_DYNAMIC);
            texture_width_ = width;
            texture_height_ = height;
            frame_changed_ = true;
        }
        if (!y_texture_ || !uv_texture_)
            return;
        if (frame_changed_) {
            gs_texture_set_image(y_texture_, pixels_.data(), width, false);
            gs_texture_set_image(uv_texture_, pixels_.data() + std::size_t(width) * height, width, false);
            frame_changed_ = false;
        }
        gs_effect_set_texture(gs_effect_get_param_by_name(effect_, "luma_image"), y_texture_);
        gs_effect_set_texture(gs_effect_get_param_by_name(effect_, "chroma_image"), uv_texture_);
        const bool previous_srgb = gs_framebuffer_srgb_enabled();
        const bool linear = gs_get_linear_srgb();
        gs_enable_framebuffer_srgb(linear);
        gs_effect_set_bool(gs_effect_get_param_by_name(effect_, "linear_output"), linear);
        while (gs_effect_loop(effect_, "Draw"))
            gs_draw_sprite(y_texture_, 0, width, height);
        gs_enable_framebuffer_srgb(previous_srgb);
    }
private:
    void reconnect_worker() noexcept
    {
        while (!stopping_.load()) {
            if (changed_.exchange(false))
                reconnect_needed_.store(true);
            if (reconnect_needed_.exchange(false)) {
                try {
                    auto next = std::make_unique<Reader>(path());
                    if (!next->valid()) {
                        reconnect_needed_.store(true);
                    } else {
                        const auto config = next->config();
                        std::vector<std::uint8_t> next_pixels(avsync::ipc::video_bytes(config));
                        {
                            std::lock_guard lock(reader_mutex_);
                            reader_.swap(next);
                            pixels_.swap(next_pixels);
                            width_.store(config.width);
                            height_.store(config.height);
                            frame_ = {};
                            frame_changed_ = false;
                        } // Old mapping and allocation are destroyed on this worker.
                        blog(LOG_INFO, "[avsync] Synthetic video IPC connected (%ux%u)", config.width, config.height);
                    }
                } catch (const std::exception &e) {
                    blog(LOG_ERROR, "[avsync] Video IPC worker: %s", e.what());
                    reconnect_needed_.store(true);
                }
            }
            wait_for(retry_period);
        }
        // Remove the live mapping on the worker, not inside an OBS render callback.
        std::lock_guard lock(reader_mutex_);
        reader_.reset();
        pixels_.clear();
    }
    std::mutex reader_mutex_;
    std::unique_ptr<Reader> reader_;
    std::vector<std::uint8_t> pixels_;
    std::atomic<bool> reconnect_needed_{true};
    std::atomic<std::uint32_t> width_{640}, height_{360};
    FrameInfo frame_{};
    bool frame_changed_{false}, effect_failed_{false};
    gs_texture_t *y_texture_{}, *uv_texture_{};
    gs_effect_t *effect_{};
    std::uint32_t texture_width_{}, texture_height_{};
};

class AudioSource final : public WorkerControl {
public:
    AudioSource(obs_data_t *settings, obs_source_t *source, unsigned stream)
        : WorkerControl(settings), source_(source), stream_(stream)
    {
        thread_ = std::thread([this] { audio_worker(); });
    }
    ~AudioSource() { stop(); }
private:
    void submit(std::vector<float> &pcm, const Config &config, std::int64_t timestamp)
    {
        if (timestamp < 0 || timestamp > avsync::ipc::monotonic_ns())
            return;
        for (auto &sample : pcm)
            sample = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
        obs_source_audio audio{};
        audio.data[0] = reinterpret_cast<const std::uint8_t *>(pcm.data());
        audio.frames = config.audio_frames;
        audio.samples_per_sec = config.audio_rate;
        audio.format = AUDIO_FORMAT_FLOAT;
        audio.speakers = stream_ == 0 ? SPEAKERS_STEREO : SPEAKERS_MONO;
        audio.timestamp = static_cast<std::uint64_t>(timestamp);
        obs_source_output_audio(source_, &audio);
    }
    void fill_short_gap(std::vector<float> &silence, const Config &config,
                        std::int64_t target, std::int64_t &last_pts, std::int64_t block_ns)
    {
        if (last_pts <= 0 || target <= last_pts + block_ns)
            return;
        // OBS 32.2 smooths timestamp gaps below 70 ms to its preceding sample
        // count. Missing those samples would move all subsequent content early.
        // Fill only a bounded short interval, with silence rather than old sound.
        if (target - (last_pts + block_ns) >= 70'000'000)
            return;
        for (unsigned count = 0; count < 7 && last_pts + block_ns < target; ++count) {
            last_pts += block_ns;
            submit(silence, config, last_pts);
        }
    }
    void audio_worker() noexcept
    {
        std::unique_ptr<Reader> reader;
        std::vector<float> pcm, silence;
        Config config;
        std::int64_t block_ns = 10'000'000;
        std::int64_t next_retry = 0, last_pts = 0, next_submit = 0;
        std::int64_t mic_capture_cutoff = 0;
        bool was_muted = obs_source_muted(source_);
        std::uint64_t generation = 0;
        while (!stopping_.load()) {
            try {
                const auto now = avsync::ipc::monotonic_ns();
                if (stream_ == 1) {
                    const bool muted = obs_source_muted(source_);
                    if (muted != was_muted)
                        mic_capture_cutoff = now;
                    was_muted = muted;
                }
                if (changed_.exchange(false)) {
                    reader.reset();
                    next_retry = 0;
                }
                if (!reader && now >= next_retry) {
                    auto candidate = std::make_unique<Reader>(path());
                    if (candidate->valid()) {
                        const auto incoming = candidate->config();
                        if (incoming.audio_rate != 48000 || incoming.audio_frames != 480)
                            throw std::runtime_error("Prototype OBS PCM requires 480-frame blocks at 48 kHz");
                        config = incoming;
                        pcm.assign(avsync::ipc::audio_samples(config, stream_), 0.0f);
                        silence.assign(pcm.size(), 0.0f);
                        block_ns = std::int64_t(config.audio_frames) * ns_per_second / config.audio_rate;
                        generation = candidate->generation();
                        reader = std::move(candidate);
                        blog(LOG_INFO, "[avsync] Synthetic %s PCM IPC connected", stream_ ? "microphone" : "desktop");
                    }
                    next_retry = now + 250'000'000;
                }
                // At most one block of actual media per sample-grid interval.
                // Bounded short-gap silence below preserves OBS sample continuity.
                if (!pcm.empty() && now >= next_submit) {
                    FrameInfo frame;
                    auto result = reader ? reader->read_next_due_audio(stream_, now, pcm, frame, block_ns * 2)
                                         : ReadResult::disconnected;
                    if (result == ReadResult::disconnected || result == ReadResult::invalid) {
                        reader.reset();
                        next_retry = now + 250'000'000;
                    }
                    if (result == ReadResult::ok && frame.presentation_ns > last_pts &&
                        frame.presentation_ns <= now && frame.bytes == pcm.size() * sizeof(float) &&
                        frame.frames == config.audio_frames) {
                        if (generation != frame.generation) {
                            generation = frame.generation;
                            // The daemon supplies a new absolute timeline; never
                            // replace its timestamps with this arrival time.
                        }
                        if (stream_ == 1 && (was_muted || frame.capture_ns < mic_capture_cutoff))
                            std::fill(pcm.begin(), pcm.end(), 0.0f);
                        fill_short_gap(silence, config, frame.presentation_ns, last_pts, block_ns);
                        submit(pcm, config, frame.presentation_ns);
                        last_pts = frame.presentation_ns;
                        // Anchor wake-up eligibility to the producer's sample grid,
                        // not successive actual wakeups (which accumulate jitter).
                        next_submit = frame.presentation_ns +
                                      ((now - frame.presentation_ns) / block_ns + 1) * block_ns;
                    } else if (last_pts > 0 && now - last_pts >= block_ns * 2) {
                        // Preserve sample-grid phase, but jump over a stale gap rather
                        // than submitting a backlog. Silence has a real timestamp.
                        const auto steps = std::max<std::int64_t>(1, (now - last_pts) / block_ns);
                        const auto target_pts = last_pts + steps * block_ns;
                        fill_short_gap(silence, config, target_pts, last_pts, block_ns);
                        last_pts = target_pts;
                        std::fill(pcm.begin(), pcm.end(), 0.0f);
                        submit(pcm, config, last_pts);
                        next_submit = last_pts + block_ns;
                    }
                }
            } catch (const std::exception &e) {
                blog(LOG_ERROR, "[avsync] Audio IPC worker: %s", e.what());
                reader.reset();
                next_retry = avsync::ipc::monotonic_ns() + 250'000'000;
            }
            wait_for(std::chrono::milliseconds(1));
        }
    }
    obs_source_t *source_;
    unsigned stream_;
};

const char *video_name(void *) { return "AV Sync Prototype - Synthetic Video"; }
const char *desktop_name(void *) { return "AV Sync Prototype - Synthetic Desktop Audio"; }
const char *mic_name(void *) { return "AV Sync Prototype - Synthetic Microphone"; }

void *video_create(obs_data_t *settings, obs_source_t *)
{
    try { return new VideoSource(settings); }
    catch (const std::exception &e) { blog(LOG_ERROR, "[avsync] Create video: %s", e.what()); return nullptr; }
}
void *desktop_create(obs_data_t *settings, obs_source_t *source)
{
    try { return new AudioSource(settings, source, 0); }
    catch (const std::exception &e) { blog(LOG_ERROR, "[avsync] Create desktop: %s", e.what()); return nullptr; }
}
void *mic_create(obs_data_t *settings, obs_source_t *source)
{
    try { return new AudioSource(settings, source, 1); }
    catch (const std::exception &e) { blog(LOG_ERROR, "[avsync] Create microphone: %s", e.what()); return nullptr; }
}
void video_destroy(void *data) { delete static_cast<VideoSource *>(data); }
void audio_destroy(void *data) { delete static_cast<AudioSource *>(data); }
void video_update(void *data, obs_data_t *settings) { static_cast<VideoSource *>(data)->update(settings); }
void audio_update(void *data, obs_data_t *settings) { static_cast<AudioSource *>(data)->update(settings); }
void video_tick(void *data, float) { static_cast<VideoSource *>(data)->tick(); }
void video_render(void *data, gs_effect_t *) { static_cast<VideoSource *>(data)->render(); }
std::uint32_t video_width(void *data) { return static_cast<VideoSource *>(data)->width(); }
std::uint32_t video_height(void *data) { return static_cast<VideoSource *>(data)->height(); }
} // namespace

bool obs_module_load(void)
{
    obs_source_info video{};
    video.id = "avsync_prototype_video";
    video.type = OBS_SOURCE_TYPE_INPUT;
    video.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_DO_NOT_DUPLICATE;
    video.get_name = video_name;
    video.create = video_create;
    video.destroy = video_destroy;
    video.update = video_update;
    video.get_defaults = source_defaults;
    video.get_properties = source_properties;
    video.video_tick = video_tick;
    video.video_render = video_render;
    video.get_width = video_width;
    video.get_height = video_height;
    video.icon_type = OBS_ICON_TYPE_CAMERA;
    obs_register_source(&video);

    obs_source_info desktop{};
    desktop.id = "avsync_prototype_desktop";
    desktop.type = OBS_SOURCE_TYPE_INPUT;
    desktop.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    desktop.get_name = desktop_name;
    desktop.create = desktop_create;
    desktop.destroy = audio_destroy;
    desktop.update = audio_update;
    desktop.get_defaults = source_defaults;
    desktop.get_properties = source_properties;
    desktop.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
    obs_register_source(&desktop);

    auto microphone = desktop;
    microphone.id = "avsync_prototype_microphone";
    microphone.get_name = mic_name;
    microphone.create = mic_create;
    microphone.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
    obs_register_source(&microphone);
    blog(LOG_INFO, "[avsync] Experimental synthetic-only OBS adapter loaded");
    return true;
}

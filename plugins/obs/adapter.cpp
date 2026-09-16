// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental IPC bridge. No capture or network code belongs here.
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
    return "Experimental shared-clock bridge video and separate desktop/microphone PCM inputs";
}

namespace {
using avsync::ipc::Config;
using avsync::ipc::FrameInfo;
using avsync::ipc::Reader;
using avsync::ipc::ReadResult;
constexpr auto ns_per_second = std::int64_t{1'000'000'000};
constexpr auto retry_period = std::chrono::milliseconds(250);
constexpr auto stale_video_ns = std::int64_t{250'000'000};

bool reader_online(Reader &reader)
{
    avsync::ipc::Status status;
    return reader.valid() &&
           reader.poll_status(avsync::ipc::monotonic_ns(), status) == ReadResult::ok && status.online;
}

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
        return std::string(runtime) + "/av-sync-bridge.ipc";
    // The service refuses unsafe directories. Do not fall back to a shared /tmp file.
    return {};
}

void source_defaults(obs_data_t *settings)
{
    const auto path = default_path();
    obs_data_set_default_string(settings, "ipc_path", path.c_str());
    obs_data_set_default_int(settings, "handoff_lead_ms", 40);
}

obs_properties_t *source_properties(void *)
{
    auto *properties = obs_properties_create();
    obs_properties_add_path(properties, "ipc_path", "Experimental bridge IPC file",
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
                    if (!reader_online(*next)) {
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
                        blog(LOG_INFO, "[avsync] Experimental bridge video IPC connected (%ux%u)", config.width, config.height);
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
        lead_ns_.store(read_lead(settings));
        muted_.store(obs_source_muted(source_));
        if (stream_ == 1)
            signal_handler_connect(obs_source_get_signal_handler(source_), "mute", mute_changed, this);
        try {
            thread_ = std::thread([this] {
                try { audio_worker(); }
                catch (const std::exception &e) {
                    blog(LOG_ERROR, "[avsync] Audio worker stopped: %s", e.what());
                }
            });
        } catch (...) {
            disconnect_mute();
            throw;
        }
    }
    ~AudioSource()
    {
        // OBS signals serialize callback dispatch/disconnect under the signal's
        // mutex. Disconnect before stopping/freeing the callback's data.
        disconnect_mute();
        stop();
    }
    void update(obs_data_t *settings)
    {
        const auto lead = read_lead(settings); // Reject invalid input before applying anything.
        lead_ns_.store(lead);
        WorkerControl::update(settings);
    }
private:
    static constexpr std::uint64_t rate = 48000, block_frames = 480;
    static constexpr std::int64_t block_ns = 10'000'000;
    static constexpr std::uint64_t max_fill_frames = 4800; // 100 ms, never a long backlog.
    static constexpr std::int64_t sample_ceiling_ns = 20834;
    static constexpr unsigned max_reads_per_iteration = 8;
    struct Counters {
        std::uint64_t media_blocks{}, output_samples{}, fill_samples{}, trim_samples{};
        std::uint64_t late_skipped{}, busy_reads{}, invalid_frames{}, starvation_fills{};
        std::uint64_t reconnects{}, generations{}, discontinuities{}, privacy_blocks{};
    } stats_;

    static std::int64_t read_lead(obs_data_t *settings)
    {
        const auto value = obs_data_get_int(settings, "handoff_lead_ms");
        if (value < 0 || value > 100)
            throw std::invalid_argument("handoff_lead_ms must be an integer from 0 to 100");
        return value * 1'000'000;
    }
    static void mute_changed(void *opaque, calldata_t *params)
    {
        auto *self = static_cast<AudioSource *>(opaque);
        self->capture_cutoff_.store(avsync::ipc::monotonic_ns(), std::memory_order_release);
        self->muted_.store(calldata_bool(params, "muted"), std::memory_order_release);
        self->mute_events_.fetch_add(1, std::memory_order_relaxed);
    }
    void disconnect_mute()
    {
        if (stream_ == 1)
            signal_handler_disconnect(obs_source_get_signal_handler(source_), "mute", mute_changed, this);
    }
    // All grid arithmetic avoids multiplying an unbounded nanosecond delta by
    // sample rate. Half-sample ties round up; dates before this source's epoch
    // are rejected rather than folded into an unsigned value.
    bool sample_index(std::int64_t timestamp, std::uint64_t &index) const
    {
        if (epoch_ns_ < 0 || timestamp < epoch_ns_)
            return false;
        const auto delta = static_cast<std::uint64_t>(timestamp - epoch_ns_);
        const auto seconds = delta / ns_per_second;
        const auto remainder = delta % ns_per_second;
        if (seconds > std::numeric_limits<std::uint64_t>::max() / rate)
            return false;
        const auto whole = seconds * rate;
        const auto part = (remainder * rate + ns_per_second / 2) / ns_per_second;
        if (whole > std::numeric_limits<std::uint64_t>::max() - part)
            return false;
        index = whole + part;
        return true;
    }
    bool grid_timestamp(std::uint64_t index, std::int64_t &timestamp) const
    {
        if (epoch_ns_ < 0)
            return false;
        const auto seconds = index / rate;
        const auto remainder = index % rate;
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (seconds > maximum / ns_per_second)
            return false;
        const auto whole = seconds * ns_per_second;
        const auto part = remainder * ns_per_second / rate;
        if (whole > maximum - part)
            return false;
        const auto delta = whole + part;
        if (static_cast<std::uint64_t>(epoch_ns_) > maximum - delta)
            return false;
        timestamp = epoch_ns_ + static_cast<std::int64_t>(delta);
        return true;
    }
    static std::int64_t bounded_add(std::int64_t base, std::int64_t increment)
    {
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        return base > maximum - increment ? maximum : base + increment;
    }
    bool submit(const float *pcm, std::uint32_t frames, std::int64_t lead)
    {
        std::int64_t timestamp;
        if (!frames || !grid_timestamp(cursor_, timestamp) ||
            cursor_ > std::numeric_limits<std::uint64_t>::max() - frames)
            return false;
        const auto now = avsync::ipc::monotonic_ns();
        // One sample of tolerance covers the explicit nearest-sample conversion.
        // This is at most a short handoff lead, never the daemon's multi-second delay.
        if (timestamp > bounded_add(now, lead + sample_ceiling_ns))
            return false;
        obs_source_audio audio{};
        audio.data[0] = reinterpret_cast<const std::uint8_t *>(pcm);
        audio.frames = frames;
        audio.samples_per_sec = rate;
        audio.format = AUDIO_FORMAT_FLOAT;
        audio.speakers = stream_ == 0 ? SPEAKERS_STEREO : SPEAKERS_MONO;
        audio.timestamp = static_cast<std::uint64_t>(timestamp);
        obs_source_output_audio(source_, &audio);
        cursor_ += frames;
        stats_.output_samples += frames;
        return true;
    }
    void process_frame(std::vector<float> &pcm, const std::vector<float> &silence,
                       const FrameInfo &frame, std::int64_t lead)
    {
        if (frame.presentation_ns < 0 || frame.capture_ns < 0 ||
            frame.frames != block_frames || frame.bytes != pcm.size() * sizeof(float)) {
            ++stats_.invalid_frames;
            return;
        }
        if (epoch_ns_ < 0)
            epoch_ns_ = frame.presentation_ns;
        std::uint64_t target;
        if (!sample_index(frame.presentation_ns, target)) {
            ++stats_.invalid_frames;
            return;
        }
        if (target > cursor_) {
            const auto gap = target - cursor_;
            if (gap <= max_fill_frames) {
                if (!submit(silence.data(), static_cast<std::uint32_t>(gap), lead))
                    return;
                stats_.fill_samples += gap;
            } else {
                // Never replay seconds of missing audio. This explicit hard-gap
                // policy still requires OBS-buffer recovery validation.
                cursor_ = target;
                ++stats_.discontinuities;
            }
        }
        const auto skip = std::min<std::uint64_t>(cursor_ - target, block_frames);
        stats_.trim_samples += skip;
        if (skip == block_frames)
            return;
        if (stream_ == 1 && (muted_.load(std::memory_order_acquire) ||
                            frame.capture_ns < capture_cutoff_.load(std::memory_order_acquire))) {
            std::fill(pcm.begin(), pcm.end(), 0.0f);
            ++stats_.privacy_blocks;
        } else {
            for (auto &sample : pcm)
                sample = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
        }
        const auto channels = stream_ == 0 ? 2u : 1u;
        if (submit(pcm.data() + skip * channels, static_cast<std::uint32_t>(block_frames - skip), lead))
            ++stats_.media_blocks;
    }
    void fill_starvation(const std::vector<float> &silence, std::int64_t now, std::int64_t lead)
    {
        if (epoch_ns_ < 0)
            return;
        std::int64_t timestamp;
        if (!grid_timestamp(cursor_, timestamp))
            return;
        if (timestamp < now && now - timestamp > 100'000'000) {
            std::uint64_t present;
            if (!sample_index(now, present))
                return;
            cursor_ = present;
            ++stats_.discontinuities;
        }
        // Leave one block of breathing room for a momentarily busy producer,
        // then supply bounded silence before the OBS input queue becomes empty.
        const auto low_water = bounded_add(now, std::max<std::int64_t>(0, lead - block_ns));
        for (unsigned count = 0; count < 8; ++count) {
            if (!grid_timestamp(cursor_, timestamp) || timestamp > low_water)
                break;
            if (!submit(silence.data(), block_frames, lead))
                break;
            stats_.fill_samples += block_frames;
            ++stats_.starvation_fills;
        }
    }
    void report(std::int64_t lead, bool final) const
    {
        blog(LOG_INFO,
             "[avsync] PCM %u %s lead_ms=%lld media_blocks=%llu output_samples=%llu "
             "fill_samples=%llu trim_samples=%llu late_skipped=%llu busy=%llu invalid=%llu "
             "starvation_fills=%llu reconnects=%llu generations=%llu discontinuities=%llu "
             "privacy_blocks=%llu mute_events=%llu",
             stream_, final ? "final" : "status", static_cast<long long>(lead / 1'000'000),
             static_cast<unsigned long long>(stats_.media_blocks),
             static_cast<unsigned long long>(stats_.output_samples),
             static_cast<unsigned long long>(stats_.fill_samples),
             static_cast<unsigned long long>(stats_.trim_samples),
             static_cast<unsigned long long>(stats_.late_skipped),
             static_cast<unsigned long long>(stats_.busy_reads),
             static_cast<unsigned long long>(stats_.invalid_frames),
             static_cast<unsigned long long>(stats_.starvation_fills),
             static_cast<unsigned long long>(stats_.reconnects),
             static_cast<unsigned long long>(stats_.generations),
             static_cast<unsigned long long>(stats_.discontinuities),
             static_cast<unsigned long long>(stats_.privacy_blocks),
             static_cast<unsigned long long>(mute_events_.load()));
    }
    void audio_worker()
    {
        std::unique_ptr<Reader> reader;
        const auto channels = stream_ == 0 ? 2u : 1u;
        std::vector<float> pcm(block_frames * channels), silence(max_fill_frames * channels, 0.0f);
        std::int64_t next_retry = 0, next_status = 0, next_report = 0;
        std::uint64_t generation = 0;
        std::uint64_t last_skipped = 0;
        while (!stopping_.load()) {
            try {
                const auto now = avsync::ipc::monotonic_ns();
                const auto lead = lead_ns_.load();
                if (changed_.exchange(false)) {
                    reader.reset();
                    next_retry = 0;
                }
                if (!reader && now >= next_retry) {
                    auto candidate = std::make_unique<Reader>(path());
                    if (reader_online(*candidate)) {
                        const auto incoming = candidate->config();
                        if (incoming.audio_rate != 48000 || incoming.audio_frames != 480)
                            throw std::runtime_error("Prototype OBS PCM requires 480-frame blocks at 48 kHz");
                        if (generation != candidate->generation())
                            ++stats_.generations;
                        generation = candidate->generation();
                        last_skipped = 0;
                        reader = std::move(candidate);
                        ++stats_.reconnects;
                        blog(LOG_INFO, "[avsync] Experimental bridge PCM %u connected; handoff lead=%lld ms",
                             stream_, static_cast<long long>(lead / 1'000'000));
                    }
                    next_retry = bounded_add(now, 250'000'000);
                }
                // Drain a small, explicitly bounded handoff window. Final PTS do
                // not move forward by 'lead'; only delivery to OBS happens earlier.
                for (unsigned count = 0; reader && count < max_reads_per_iteration; ++count) {
                    FrameInfo frame;
                    const auto result = reader->read_next_audio(stream_, avsync::ipc::monotonic_ns(), lead,
                                                               pcm, frame, 20'000'000);
                    if (result == ReadResult::ok) {
                        if (frame.generation != generation) {
                            ++stats_.invalid_frames;
                            reader.reset();
                            next_retry = bounded_add(now, 250'000'000);
                            break;
                        }
                        process_frame(pcm, silence, frame, lead);
                        continue;
                    }
                    if (result == ReadResult::busy)
                        ++stats_.busy_reads;
                    if (result == ReadResult::disconnected || result == ReadResult::invalid) {
                        reader.reset();
                        next_retry = bounded_add(now, 250'000'000);
                    }
                    break;
                }
                fill_starvation(silence, avsync::ipc::monotonic_ns(), lead);
                if (reader && now >= next_status) {
                    avsync::ipc::Status status;
                    if (reader->poll_status(now, status) == ReadResult::ok) {
                        const auto skipped = status.skipped_audio[stream_];
                        if (skipped >= last_skipped)
                            stats_.late_skipped += skipped - last_skipped;
                        last_skipped = skipped;
                    }
                    next_status = bounded_add(now, ns_per_second);
                }
                if (now >= next_report) {
                    report(lead, false);
                    next_report = bounded_add(now, 5 * ns_per_second);
                }
            } catch (const std::exception &e) {
                blog(LOG_ERROR, "[avsync] Audio IPC worker: %s", e.what());
                reader.reset();
                next_retry = bounded_add(avsync::ipc::monotonic_ns(), 250'000'000);
            }
            wait_for(std::chrono::milliseconds(1));
        }
        report(lead_ns_.load(), true);
    }
    obs_source_t *source_;
    unsigned stream_;
    std::atomic<std::int64_t> lead_ns_{40'000'000}, capture_cutoff_{0};
    std::atomic<bool> muted_{false};
    std::atomic<std::uint64_t> mute_events_{0};
    std::int64_t epoch_ns_{-1};
    std::uint64_t cursor_{0};
};

const char *video_name(void *) { return "AV Sync Bridge - Video (Experimental)"; }
const char *desktop_name(void *) { return "AV Sync Bridge - Desktop Audio (Experimental)"; }
const char *mic_name(void *) { return "AV Sync Bridge - Microphone (Experimental)"; }

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
void audio_update(void *data, obs_data_t *settings)
{
    try { static_cast<AudioSource *>(data)->update(settings); }
    catch (const std::exception &e) { blog(LOG_ERROR, "[avsync] Rejected audio settings: %s", e.what()); }
}
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
    blog(LOG_INFO, "[avsync] Experimental shared-clock bridge OBS adapter loaded");
    return true;
}

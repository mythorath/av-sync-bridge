// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/v4l2_capture.hpp"
#include "avsync/capture_window.hpp"
#include "avsync/video_timing.hpp"

#include <linux/videodev2.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>

namespace avsync {
namespace {
constexpr std::uint32_t buffer_count = 4;
constexpr std::uint32_t maximum_buffer_bytes = 32U * 1024U * 1024U;
using DeadlineClock = std::chrono::steady_clock;

int call_ioctl(int fd, unsigned long request, void *value)
{
    int result;
    do { result = ::ioctl(fd, request, value); } while (result == -1 && errno == EINTR);
    return result;
}

[[noreturn]] void fail_errno(const char *operation)
{
    const int code = errno;
    // Operation strings are constants, never caller-supplied paths or IDs.
    throw std::runtime_error(std::string(operation) + " failed (errno " +
                             std::to_string(code) + ")");
}

std::int64_t monotonic_now()
{
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        fail_errno("clock_gettime");
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000 ||
        value.tv_sec > (maximum - value.tv_nsec) / 1'000'000'000)
        throw std::runtime_error("invalid monotonic clock value");
    return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000 + value.tv_nsec;
}

V4l2Format read_format(int fd, bool extended_color_supported)
{
    v4l2_format raw{};
    raw.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (extended_color_supported) raw.fmt.pix.priv = V4L2_PIX_FMT_PRIV_MAGIC;
    if (call_ioctl(fd, VIDIOC_G_FMT, &raw) != 0)
        fail_errno("VIDIOC_G_FMT");
    const auto &p = raw.fmt.pix;
    if (p.pixelformat != V4L2_PIX_FMT_NV12 || p.field != V4L2_FIELD_NONE)
        throw std::runtime_error("current format must be progressive single-planar NV12");
    if (!p.width || !p.height || p.width > 4096 || p.height > 2160 ||
        p.width % 2 || p.height % 2 || p.bytesperline < p.width || p.bytesperline % 2)
        throw std::runtime_error("unsupported NV12 dimensions or stride");
    const auto required = static_cast<std::uint64_t>(p.bytesperline) *
                          (p.height + p.height / 2U);
    if (p.sizeimage < required || p.sizeimage > maximum_buffer_bytes)
        throw std::runtime_error("invalid or oversized NV12 image allocation");
    V4l2Format result;
    result.width = p.width;
    result.height = p.height;
    result.stride = p.bytesperline;
    result.size_image = p.sizeimage;
    result.pixel_format = p.pixelformat;
    result.field = p.field;
    result.colorspace = p.colorspace;
    result.extended_color_fields_valid = extended_color_supported && p.priv == V4L2_PIX_FMT_PRIV_MAGIC;
    if (result.extended_color_fields_valid) {
        result.ycbcr_encoding = p.ycbcr_enc;
        result.quantization = p.quantization;
        result.transfer_function = p.xfer_func;
    }
    v4l2_streamparm parameter{};
    parameter.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (call_ioctl(fd, VIDIOC_G_PARM, &parameter) == 0) {
        result.frame_interval_numerator = parameter.parm.capture.timeperframe.numerator;
        result.frame_interval_denominator = parameter.parm.capture.timeperframe.denominator;
    } else if (errno != EINVAL && errno != ENOTTY) {
        fail_errno("VIDIOC_G_PARM");
    }
    return result;
}

bool same_layout(const V4l2Format &a, const V4l2Format &b)
{
    return a.width == b.width && a.height == b.height && a.stride == b.stride &&
           a.size_image == b.size_image && a.pixel_format == b.pixel_format &&
           a.field == b.field && a.colorspace == b.colorspace &&
           a.extended_color_fields_valid == b.extended_color_fields_valid &&
           a.ycbcr_encoding == b.ycbcr_encoding && a.quantization == b.quantization &&
           a.transfer_function == b.transfer_function &&
           a.frame_interval_numerator == b.frame_interval_numerator &&
           a.frame_interval_denominator == b.frame_interval_denominator;
}
} // namespace

struct V4l2Capture::Impl {
    struct Mapping { void *address = MAP_FAILED; std::size_t bytes{}; };
    int fd = -1;
    bool allocated{}, streaming{}, used{}, extended_color_supported{};
    V4l2Format format;
    V4l2CaptureStats stats;
    std::array<Mapping, buffer_count> mappings{};
    VideoTimingTracker timing;
    bool have_timestamp_flags{};
    std::int64_t run_start_ns{};

    ~Impl()
    {
        release();
        if (fd >= 0) ::close(fd);
    }

    void release() noexcept
    {
        if (streaming) {
            auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (call_ioctl(fd, VIDIOC_STREAMOFF, &type) != 0) ++stats.cleanup_errors;
            streaming = false;
        }
        for (auto &mapping : mappings) {
            if (mapping.address != MAP_FAILED) {
                if (::munmap(mapping.address, mapping.bytes) != 0) ++stats.cleanup_errors;
                mapping = {};
            }
        }
        if (allocated) {
            v4l2_requestbuffers request{};
            request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            request.memory = V4L2_MEMORY_MMAP;
            if (call_ioctl(fd, VIDIOC_REQBUFS, &request) != 0) ++stats.cleanup_errors;
            allocated = false;
        }
    }

    void initialize(const std::string &device)
    {
        if (device.empty() || device.front() != '/' || device.size() > 4096 ||
            device.find('\0') != std::string::npos)
            throw std::invalid_argument("an explicit absolute device path is required");
        fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) fail_errno("open capture device");
        struct stat info{};
        if (::fstat(fd, &info) != 0) fail_errno("fstat capture device");
        if (!S_ISCHR(info.st_mode)) throw std::runtime_error("capture device is not a character device");
        v4l2_capability capability{};
        if (call_ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) fail_errno("VIDIOC_QUERYCAP");
        const auto caps = capability.capabilities & V4L2_CAP_DEVICE_CAPS ?
                          capability.device_caps : capability.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING) ||
            (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)))
            throw std::runtime_error("device lacks supported single-planar streaming capture");
        extended_color_supported = (caps & V4L2_CAP_EXT_PIX_FORMAT) != 0;
        format = read_format(fd, extended_color_supported);
    }

    void queue(std::uint32_t index)
    {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (call_ioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
            ++stats.io_errors;
            fail_errno("VIDIOC_QBUF");
        }
    }

    void start()
    {
        v4l2_requestbuffers request{};
        request.count = buffer_count;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (call_ioctl(fd, VIDIOC_REQBUFS, &request) != 0) fail_errno("VIDIOC_REQBUFS");
        allocated = true;
        stats.allocated_buffers = request.count;
        if (request.count != buffer_count)
            throw std::runtime_error("driver did not provide the required bounded four-buffer pool");
        if (!same_layout(format, read_format(fd, extended_color_supported)))
            throw std::runtime_error("capture format changed before ownership was acquired");
        for (std::uint32_t index = 0; index < buffer_count; ++index) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (call_ioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0) fail_errno("VIDIOC_QUERYBUF");
            if (buffer.index != index || buffer.length < format.size_image ||
                buffer.length > maximum_buffer_bytes)
                throw std::runtime_error("invalid capture mapping metadata");
            auto *address = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, buffer.m.offset);
            if (address == MAP_FAILED) fail_errno("mmap capture buffer");
            mappings[index] = {address, buffer.length};
            queue(index);
        }
        auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (call_ioctl(fd, VIDIOC_STREAMON, &type) != 0) fail_errno("VIDIOC_STREAMON");
        streaming = true;
    }

    void consume(const v4l2_buffer &buffer, const FrameCallback &callback)
    {
        const auto dequeue_ns = monotonic_now();
        const auto elapsed_ns = dequeue_ns - run_start_ns;
        if (!stats.dequeued) stats.first_dequeue_elapsed_ns = elapsed_ns;
        ++stats.dequeued;
        if (buffer.index >= buffer_count || buffer.type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            buffer.memory != V4L2_MEMORY_MMAP || buffer.field != V4L2_FIELD_NONE ||
            buffer.length != mappings[buffer.index].bytes ||
            buffer.bytesused > mappings[buffer.index].bytes) {
            ++stats.invalid_payloads;
            throw std::runtime_error("invalid dequeued buffer metadata");
        }
        if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
            ++stats.driver_error_buffers;
            stats.last_driver_error_elapsed_ns = elapsed_ns;
            if (stats.error_buffer_events_recorded < stats.error_buffer_events.size()) {
                auto &event = stats.error_buffer_events[stats.error_buffer_events_recorded++];
                event.sequence = buffer.sequence;
                event.flags = buffer.flags;
                event.bytes_used = buffer.bytesused;
                event.dequeue_elapsed_ns = elapsed_ns;
                const auto clock_flags = buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
                const auto source_flags = buffer.flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
                const auto timestamp = timestamp_from_timeval(buffer.timestamp.tv_sec,
                    buffer.timestamp.tv_usec, clock_flags == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC ?
                    TimestampClock::monotonic : TimestampClock::unknown);
                if (timestamp && (source_flags == V4L2_BUF_FLAG_TSTAMP_SRC_EOF ||
                                  source_flags == V4L2_BUF_FLAG_TSTAMP_SRC_SOE) &&
                    capture_window_status(*timestamp, dequeue_ns, {1'000'000, 2'000'000'000}) ==
                    CaptureWindowStatus::accepted) {
                    event.capture_timestamp_valid = true;
                    event.capture_elapsed_ns = *timestamp - run_start_ns;
                }
            }
            return; // Corrupt content is discarded, but the mapped buffer is reusable.
        }
        const auto required = static_cast<std::uint64_t>(format.stride) *
                              (format.height + format.height / 2U);
        if (buffer.bytesused < required || buffer.bytesused > format.size_image) {
            ++stats.invalid_payloads;
            throw std::runtime_error("incomplete or oversized NV12 payload");
        }
        const auto timestamp_flags = buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
        const auto source_flags = buffer.flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
        if (timestamp_flags != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC ||
            (source_flags != V4L2_BUF_FLAG_TSTAMP_SRC_EOF &&
             source_flags != V4L2_BUF_FLAG_TSTAMP_SRC_SOE)) {
            ++stats.invalid_timestamps;
            throw std::runtime_error("capture requires known monotonic timestamps and source flags");
        }
        if (have_timestamp_flags &&
            (stats.timestamp_flags != timestamp_flags || stats.timestamp_source_flags != source_flags)) {
            ++stats.timestamp_flag_changes;
            throw std::runtime_error("capture timestamp flags changed within one generation");
        }
        stats.timestamp_flags = timestamp_flags;
        stats.timestamp_source_flags = source_flags;
        have_timestamp_flags = true;
        const auto capture_ns = timestamp_from_timeval(buffer.timestamp.tv_sec,
            buffer.timestamp.tv_usec, TimestampClock::monotonic);
        if (!capture_ns || capture_window_status(*capture_ns, dequeue_ns,
            {1'000'000, 2'000'000'000}) != CaptureWindowStatus::accepted) {
            ++stats.invalid_timestamps;
            throw std::runtime_error("capture timestamp is invalid or outside the bounded monotonic window");
        }
        const auto observation = timing.observe(buffer.sequence, *capture_ns);
        if (!observation.accepted()) {
            ++stats.chronology_errors;
            throw std::runtime_error("capture sequence or timestamp is not forward-moving");
        }
        if (observation.missing_frames) {
            ++stats.sequence_gaps;
            stats.missing_sequences += observation.missing_frames;
        }
        const auto age = dequeue_ns - *capture_ns;
        if (!stats.accepted) {
            stats.first_capture_ns = *capture_ns;
            stats.first_accepted_elapsed_ns = elapsed_ns;
            stats.first_accepted_sequence = buffer.sequence;
            stats.minimum_dequeue_age_ns = stats.maximum_dequeue_age_ns = age;
        } else {
            const auto interval = *capture_ns - stats.last_capture_ns;
            if (stats.accepted == 1) {
                stats.minimum_interval_ns = stats.maximum_interval_ns = interval;
                stats.maximum_interval_end_elapsed_ns = elapsed_ns;
            } else {
                stats.minimum_interval_ns = std::min(stats.minimum_interval_ns, interval);
                if (interval > stats.maximum_interval_ns)
                    stats.maximum_interval_end_elapsed_ns = elapsed_ns;
                stats.maximum_interval_ns = std::max(stats.maximum_interval_ns, interval);
            }
            stats.minimum_dequeue_age_ns = std::min(stats.minimum_dequeue_age_ns, age);
            stats.maximum_dequeue_age_ns = std::max(stats.maximum_dequeue_age_ns, age);
        }
        const V4l2FrameView frame{{static_cast<const std::byte *>(mappings[buffer.index].address),
                                   buffer.bytesused}, *capture_ns, dequeue_ns, buffer.sequence,
                                  buffer.flags, timestamp_flags, source_flags};
        const auto callback_start = monotonic_now();
        try { callback(frame); }
        catch (...) { ++stats.callback_errors; throw; }
        stats.maximum_callback_ns = std::max(stats.maximum_callback_ns, monotonic_now() - callback_start);
        stats.last_capture_ns = *capture_ns;
        stats.last_accepted_elapsed_ns = elapsed_ns;
        stats.last_accepted_sequence = buffer.sequence;
        ++stats.accepted;
    }
};

V4l2Capture::V4l2Capture(const std::string &device) : impl_(std::make_unique<Impl>())
{
    impl_->initialize(device);
}
V4l2Capture::~V4l2Capture() = default;
const V4l2Format &V4l2Capture::format() const noexcept { return impl_->format; }
const V4l2CaptureStats &V4l2Capture::stats() const noexcept { return impl_->stats; }

void V4l2Capture::run(std::chrono::seconds duration, const FrameCallback &callback,
                     const StopRequested &stop_requested)
{
    if (duration < std::chrono::seconds(1) || duration > std::chrono::seconds(180) || !callback)
        throw std::invalid_argument("capture duration must be 1..180 seconds with a callback");
    run_impl(duration, callback, stop_requested);
}

void V4l2Capture::run_supervised(std::chrono::seconds duration, const FrameCallback &callback,
                                const StopRequested &stop_requested)
{
    if (duration < std::chrono::seconds(1) || duration > std::chrono::seconds(43200) || !callback || !stop_requested)
        throw std::invalid_argument("supervised capture must be 1..43200 seconds with frame and stop callbacks");
    run_impl(duration, callback, stop_requested);
}

void V4l2Capture::run_impl(std::chrono::seconds duration, const FrameCallback &callback,
                         const StopRequested &stop_requested)
{
    if (impl_->used) throw std::logic_error("one capture object represents only one generation");
    impl_->used = true;
    impl_->run_start_ns = monotonic_now();
    const auto deadline = DeadlineClock::now() + duration;
    try {
        // A caller may spend significant time reserving its downstream ring
        // before run(). Do not activate the device after a queued cancellation.
        if (stop_requested && stop_requested()) {
            impl_->stats.interrupted = true;
            return;
        }
        impl_->start();
        while (DeadlineClock::now() < deadline) {
            if (stop_requested && stop_requested()) { impl_->stats.interrupted = true; break; }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - DeadlineClock::now()).count();
            if (left <= 0) break;
            pollfd descriptor{impl_->fd, POLLIN, 0};
            const auto result = ::poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(100, left)));
            if (result < 0) {
                if (errno == EINTR) continue;
                ++impl_->stats.io_errors;
                fail_errno("poll capture device");
            }
            if (!result) { ++impl_->stats.poll_timeouts; continue; }
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                ++impl_->stats.io_errors;
                throw std::runtime_error("capture device reported a poll error or disconnected");
            }
            if (!(descriptor.revents & POLLIN)) continue;
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (call_ioctl(impl_->fd, VIDIOC_DQBUF, &buffer) != 0) {
                if (errno == EAGAIN) continue;
                ++impl_->stats.io_errors;
                // EIO may leave index unspecified: stop, do not guess a QBUF index.
                fail_errno("VIDIOC_DQBUF");
            }
            impl_->consume(buffer, callback);
            // The callback has completed all use/copying before driver ownership resumes.
            impl_->queue(buffer.index);
        }
    } catch (...) {
        impl_->release(); // STREAMOFF releases ownership even if callback/metadata failed.
        throw;
    }
    impl_->release();
    if (impl_->stats.cleanup_errors) throw std::runtime_error("capture resource cleanup failed");
}

} // namespace avsync

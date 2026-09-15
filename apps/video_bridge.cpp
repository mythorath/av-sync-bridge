// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit video-only capture -> bounded memory IPC. No OBS/network/audio access.
#include "avsync/ipc.hpp"
#include "avsync/timing.hpp"
#include "avsync/v4l2_capture.hpp"
#include "avsync/video_handoff.hpp"
#include <linux/magic.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }
void require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
unsigned number(const char* value, unsigned low, unsigned high) {
    const std::string_view text(value); unsigned result{};
    const auto [end,error] = std::from_chars(text.data(),text.data()+text.size(),result);
    require(error == std::errc{} && end == text.data()+text.size() && result >= low && result <= high,
            "invalid numeric argument");
    return result;
}
void help() {
    std::cout << "avsync-video-bridge --capture --device /dev/videoN --runtime-dir NEW_TMPFS_DIRECTORY\n"
                 " --seconds 1..120 [--delay-ms 0..2000] [--verify-delivery]\n"
                 "Current NV12 format only; device must be released by its existing owner.\n"
                 "Video pixels are buffered in a private temporary memory-backed IPC file.\n"
                 "The new directory and its IPC files are removed on normal/error exit.\n"
                 "No OBS, audio, network, format changes or startup registration.\n"
                 "Optional verification reads delayed IPC pixels in memory, not a video display.\n";
}
struct Options { std::string device, directory; unsigned seconds{}, delay_ms{2000}; bool capture{}, verify{}; };
Options parse(int argc, char** argv) {
    Options o; unsigned seen{};
    for (int i=1;i<argc;++i) {
        const std::string_view arg(argv[i]);
        if (arg=="--capture" && !o.capture) { o.capture=true; continue; }
        if (arg=="--verify-delivery" && !o.verify) { o.verify=true; continue; }
        require(i+1<argc,"missing option value"); const auto* value=argv[++i];
        unsigned bit{};
        if (arg=="--device") { bit=1; o.device=value; }
        else if (arg=="--runtime-dir") { bit=2; o.directory=value; }
        else if (arg=="--seconds") { bit=4; o.seconds=number(value,1,120); }
        else if (arg=="--delay-ms") { bit=8; o.delay_ms=number(value,0,2000); }
        else throw std::runtime_error("unknown or repeated argument");
        require(!(seen&bit),"repeated argument"); seen|=bit;
    }
    require(o.capture && !o.device.empty() && !o.directory.empty() && o.seconds,
            "explicit capture, device, new runtime directory and duration required");
    return o;
}

// The caller supplies a NEW child of a private, owned tmpfs directory. Never
// recursively removes anything or overwrites a previously existing directory.
class RuntimeDirectory {
    int parent_{-1}, directory_{-1};
    std::string leaf_;
    bool created_{};
    ino_t inode_{};
    dev_t device_{};
public:
    std::string path;
    ~RuntimeDirectory() { (void)cleanup(); if (parent_>=0) close(parent_); }
    void create(const std::string& requested) {
        require(!requested.empty() && requested.front()=='/' && requested.size()<4096,
                "runtime directory must be an absolute path");
        const auto slash=requested.rfind('/');
        const auto parent=slash==0 ? std::string("/") : requested.substr(0,slash);
        leaf_=requested.substr(slash+1);
        require(!leaf_.empty() && leaf_!="." && leaf_!="..","invalid runtime directory name");
        parent_=open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        struct stat st{}; struct statfs fs{};
        require(parent_>=0 && fstat(parent_,&st)==0 && st.st_uid==geteuid() && !(st.st_mode&0077) &&
                fstatfs(parent_,&fs)==0 && fs.f_type==TMPFS_MAGIC,
                "runtime parent must be user-owned, private tmpfs (for example XDG_RUNTIME_DIR)");
        require(mkdirat(parent_,leaf_.c_str(),0700)==0,"runtime directory must not already exist");
        created_=true; path=requested;
        require(fstatat(parent_,leaf_.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISDIR(st.st_mode) &&
                st.st_uid==geteuid(),"cannot identify new runtime directory");
        inode_=st.st_ino; device_=st.st_dev;
        directory_=openat(parent_,leaf_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        require(directory_>=0 && fstat(directory_,&st)==0 && st.st_ino==inode_ && st.st_dev==device_,
                "cannot open identified runtime directory");
    }
    std::uint64_t mapping_size() {
        const int fd=openat(directory_,"media.ipc",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
        require(fd>=0,"cannot inspect reserved IPC mapping");
        struct stat st{};
        const bool valid=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
            (st.st_mode&0777)==0600 && st.st_size>0 && st.st_size<=2LL*1024*1024*1024;
        close(fd);
        require(valid,"invalid reserved IPC mapping");
        return static_cast<std::uint64_t>(st.st_size);
    }
    bool cleanup() noexcept {
        bool okay=true;
        if (directory_>=0) {
            for (const auto* name : {"media.ipc","media.ipc.lock"})
                if (unlinkat(directory_,name,0)!=0 && errno!=ENOENT) okay=false;
            close(directory_); directory_=-1;
        }
        if (created_ && parent_>=0) {
            struct stat current{};
            if (fstatat(parent_,leaf_.c_str(),&current,AT_SYMLINK_NOFOLLOW)!=0 ||
                current.st_ino!=inode_ || current.st_dev!=device_ || !S_ISDIR(current.st_mode) ||
                unlinkat(parent_,leaf_.c_str(),AT_REMOVEDIR)!=0) okay=false;
            else created_=false;
        }
        return okay;
    }
};

// Deliberately a sampled consistency check, not a cryptographic/full-frame hash.
std::uint64_t fingerprint(std::span<const std::uint8_t> pixels) {
    std::uint64_t hash=14695981039346656037ULL;
    const auto step=std::max<std::size_t>(1,(pixels.size()+4095)/4096);
    for (std::size_t i=0;i<pixels.size();i+=step) { hash^=pixels[i]; hash*=1099511628211ULL; }
    return hash;
}
struct Ledger { std::uint64_t sequence{}, digest{}; std::int64_t capture{}, presentation{}; };
struct Stats {
    std::uint64_t published{}, publication_busy{}, handoff_full{}, stale_before_publish{};
    std::uint64_t received{}, reader_busy{}, skipped{}, digest_changes{}, verified{}, failures{};
    std::int64_t min_delivery_age{std::numeric_limits<std::int64_t>::max()}, max_delivery_age{};
    std::int64_t max_publish_ns{}, max_read_ns{};
};

int run(const Options& options) {
    RuntimeDirectory runtime;
    std::unique_ptr<avsync::ipc::Writer> writer;
    std::unique_ptr<avsync::ipc::Reader> reader;
    std::unique_ptr<avsync::V4l2Capture> capture;
    std::atomic<bool> done{false}, failed{false};
    std::unique_ptr<avsync::Nv12VideoHandoff> handoff;
    std::vector<Ledger> ledger;
    std::vector<std::uint8_t> received_pixels;
    const auto delay_ns=std::int64_t(options.delay_ms)*1000000;
    std::thread worker;
    Stats stats;
    std::uint64_t mapping_bytes{};
    std::string error;
    try {
        capture=std::make_unique<avsync::V4l2Capture>(options.device);
        const auto format=capture->format();
        require(format.frame_interval_numerator && format.frame_interval_denominator,
                "current device frame interval must be known");
        const auto capacity_fps=(std::uint64_t(format.frame_interval_denominator)+
            format.frame_interval_numerator-1)/format.frame_interval_numerator;
        require(capacity_fps>=1 && capacity_fps<=120,"unsupported device cadence");
        avsync::ipc::Config config;
        config.width=format.width; config.height=format.height; config.fps=static_cast<unsigned>(capacity_fps);
        config.video_capacity=(options.delay_ms*config.fps+999)/1000+8;
        config.audio_capacity=2; // No invented silence/audio streams in video-only mode.
        std::string config_error;
        require(avsync::ipc::validate_config(config,config_error),"video dimensions/delay exceed the 2 GiB IPC budget");
        handoff=std::make_unique<avsync::Nv12VideoHandoff>(format.width,format.height,format.stride);
        runtime.create(options.directory);
        const auto ipc_path=runtime.path+"/media.ipc";
        writer=std::make_unique<avsync::ipc::Writer>(ipc_path,config,
            avsync::ipc::AllocationPolicy::reserve_and_prefault);
        require(writer->valid(),"cannot create private IPC writer");
        mapping_bytes=runtime.mapping_size();
        if (options.verify) {
            reader=std::make_unique<avsync::ipc::Reader>(ipc_path);
            require(reader->valid(),"cannot open isolated verification reader");
        }
        ledger.resize(reader ? config.video_capacity : 0);
        received_pixels.resize(reader ? avsync::ipc::video_bytes(config) : 0);
        worker=std::thread([&] {
            try {
                std::int64_t last_presentation=0, next_heartbeat=0;
                std::uint64_t last_sequence=0,last_digest=0;
                while (!failed.load()) {
                    const auto now=avsync::ipc::monotonic_ns();
                    if (const auto* packet=handoff->peek()) {
                        const auto presentation=avsync::checked_add(packet->capture_ns,delay_ns);
                        require(presentation.has_value(),"presentation timestamp overflow");
                        // Never fill the long ring with a delayed worker backlog.
                        if (now>packet->capture_ns && now-packet->capture_ns>200000000) ++stats.stale_before_publish;
                        else {
                            const auto before=avsync::ipc::monotonic_ns();
                            const auto result=writer->try_publish_video(packet->pixels,packet->capture_ns,*presentation);
                            stats.max_publish_ns=std::max(stats.max_publish_ns,avsync::ipc::monotonic_ns()-before);
                            if (result==avsync::ipc::WriteResult::busy) ++stats.publication_busy;
                            else {
                                require(result==avsync::ipc::WriteResult::ok,"IPC publication failed");
                                ++stats.published; last_presentation=*presentation;
                                if (reader) ledger[stats.published%ledger.size()]={stats.published,fingerprint(packet->pixels),
                                                                                packet->capture_ns,*presentation};
                            }
                        }
                        require(handoff->release(),"handoff ownership failed");
                    }
                    if (now>=next_heartbeat) {
                        const auto result=writer->try_heartbeat();
                        require(result==avsync::ipc::WriteResult::ok || result==avsync::ipc::WriteResult::busy,
                                "IPC heartbeat failed");
                        const auto next=avsync::checked_add(now,100000000);
                        require(next.has_value(),"heartbeat deadline overflow");
                        next_heartbeat=*next;
                    }
                    if (reader) {
                        avsync::ipc::FrameInfo info;
                        const auto before=avsync::ipc::monotonic_ns();
                        const auto result=reader->read_latest_due_video(before,received_pixels,info);
                        const auto after=avsync::ipc::monotonic_ns();
                        stats.max_read_ns=std::max(stats.max_read_ns,after-before);
                        if (result==avsync::ipc::ReadResult::busy) ++stats.reader_busy;
                        else if (result==avsync::ipc::ReadResult::ok) {
                            const auto& expected=ledger[info.sequence%ledger.size()];
                            const auto digest=fingerprint(received_pixels);
                            require(info.sequence>last_sequence && info.presentation_ns<=before &&
                                info.presentation_ns-info.capture_ns==delay_ns && expected.sequence==info.sequence &&
                                expected.capture==info.capture_ns && expected.presentation==info.presentation_ns &&
                                expected.digest==digest,"delayed IPC timing/payload verification failed");
                            if (stats.received && digest!=last_digest) ++stats.digest_changes;
                            stats.skipped+=info.sequence-last_sequence-1;
                            last_sequence=info.sequence; last_digest=digest;
                            ++stats.received; ++stats.verified;
                            stats.min_delivery_age=std::min(stats.min_delivery_age,after-info.capture_ns);
                            stats.max_delivery_age=std::max(stats.max_delivery_age,after-info.capture_ns);
                        } else require(result==avsync::ipc::ReadResult::empty || result==avsync::ipc::ReadResult::stale,
                                       "verification reader disconnected or invalid");
                    }
                    if (done.load() && !handoff->peek() && now>=last_presentation &&
                        now-last_presentation>=250000000) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } catch (...) { ++stats.failures; failed.store(true); }
        });
        std::cout << "AVSYNC_VIDEO_READY width=" << config.width << " height=" << config.height
                  << " delay_ms=" << options.delay_ms << " video_only=true\n" << std::flush;
        try {
            capture->run(std::chrono::seconds(options.seconds),[&](const avsync::V4l2FrameView& frame) {
                const auto result=handoff->try_copy(frame.pixels,frame.capture_ns);
                if (result==avsync::VideoHandoffStatus::full) ++stats.handoff_full;
                else require(result==avsync::VideoHandoffStatus::ok,"invalid capture handoff");
            },[&] { return interrupted!=0 || failed.load(); });
        } catch (...) { failed.store(true); throw; }
        done.store(true);
        worker.join();
        require(!failed.load(),"video worker failed");
        require(stats.published>=2 && (!options.verify || stats.verified>=2),"insufficient captured/delayed frames");
    } catch (const std::exception& caught) {
        error=caught.what(); failed.store(true); done.store(true);
        if (worker.joinable()) worker.join();
    }
    // Readers and writer close before removal. No consumer can reopen this epoch.
    reader.reset(); writer.reset();
    const bool cleaned=runtime.cleanup();
    const auto capture_stats=capture ? capture->stats() : avsync::V4l2CaptureStats{};
    const bool clean=error.empty() && cleaned && !capture_stats.driver_error_buffers &&
        !capture_stats.missing_sequences && !capture_stats.interrupted && !stats.handoff_full &&
        !stats.publication_busy && !stats.stale_before_publish && !stats.failures &&
        (!options.verify || stats.received==stats.published);
    std::cout << "{\"schema\":1,\"video_only\":true,\"obs_used\":false,\"status\":\""
              << (clean ? (options.verify ? "bounded_video_verified" : "video_buffered_unverified") : "degraded_or_failed")
              << "\",\"verification_requested\":" << (options.verify ? "true" : "false") << ",\"runtime_cleaned\":"
              << (cleaned ? "true" : "false") << ",\"mapping_bytes\":" << mapping_bytes
              << ",\"delay_ms\":" << options.delay_ms << ",\"captured\":" << capture_stats.accepted
              << ",\"driver_error_frames\":" << capture_stats.driver_error_buffers
              << ",\"last_driver_error_elapsed_ms\":" << capture_stats.last_driver_error_elapsed_ns/1e6
              << ",\"missing_sequences\":" << capture_stats.missing_sequences
              << ",\"max_capture_interval_ms\":" << capture_stats.maximum_interval_ns/1e6
              << ",\"max_capture_interval_end_elapsed_ms\":" << capture_stats.maximum_interval_end_elapsed_ns/1e6
              << ",\"published\":" << stats.published << ",\"handoff_full\":" << stats.handoff_full
              << ",\"publication_busy\":" << stats.publication_busy << ",\"stale_before_publish\":" << stats.stale_before_publish
              << ",\"received\":" << stats.received << ",\"verified\":" << stats.verified
              << ",\"reader_busy\":" << stats.reader_busy << ",\"reader_skipped\":" << stats.skipped
              << ",\"sampled_digest_changes\":" << stats.digest_changes
              << ",\"min_delivery_age_ms\":" << (stats.received ? stats.min_delivery_age/1e6 : 0)
              << ",\"max_delivery_age_ms\":" << stats.max_delivery_age/1e6
              << ",\"max_capture_copy_ms\":" << capture_stats.maximum_callback_ns/1e6
              << ",\"max_publish_ms\":" << stats.max_publish_ns/1e6
              << ",\"max_read_ms\":" << stats.max_read_ns/1e6 << "}\n";
    if (!error.empty()) std::cerr << error << '\n'; // All local error text is generic.
    return clean ? 0 : 3;
}
}
int main(int argc,char** argv) {
    if (argc==1) { help(); return 0; }
    for (int i=1;i<argc;++i) if (std::string_view(argv[i])=="--help") { help(); return 0; }
    try {
        const auto options=parse(argc,argv);
        std::signal(SIGINT,stop); std::signal(SIGTERM,stop);
        return run(options);
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 2; }
}

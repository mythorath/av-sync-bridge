// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_handoff.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <signal.h>
#include <stdexcept>
#include <unistd.h>
#include <sys/wait.h>

namespace avsync::ipc::testing { [[noreturn]] void briefly_hold_mutex(const char*,int,int); }

namespace {
void check(bool good) { if (!good) throw std::runtime_error("desktop handoff assertion"); }
struct Temporary {
    std::string dir,path;
    Temporary() {
        char pattern[]="/tmp/avsync-audio-handoff-XXXXXX";
        const auto p=mkdtemp(pattern); if (!p) throw std::runtime_error("mkdtemp");
        dir=p; path=dir+"/desktop";
    }
    ~Temporary() { unlink(path.c_str()); unlink((path+".lock").c_str()); rmdir(dir.c_str()); }
};

void process_replacement() {
    Temporary temp;
    int notice[2], release[2]; check(pipe(notice)==0 && pipe(release)==0);
    const auto child=fork(); check(child>=0);
    if (!child) {
        close(notice[0]); close(release[1]); alarm(5);
        try {
            avsync::CorrectedAudioHandoff producer(temp.path,100'000'000);
            std::array<float,960> pcm; pcm.fill(.25F);
            const auto now=avsync::ipc::monotonic_ns();
            if (!producer.consume({{20,1},0,now,480},pcm,now)) _exit(81);
            const char ready='R'; if (write(notice[1],&ready,1)!=1) _exit(82);
            char wait{}; if (read(release[0],&wait,1)!=1) _exit(85); _exit(83);
        } catch (...) { _exit(84); }
    }
    close(notice[1]); close(release[0]); char marker{};
    const bool notified=read(notice[0],&marker,1)==1 && marker=='R'; close(notice[0]);
    avsync::ipc::Reader previous(temp.path);
    const auto old_generation=previous.generation();
    bool live_refused=false;
    try {
        avsync::CorrectedAudioHandoff duplicate(temp.path,100'000'000,avsync::ipc::ReplacementPolicy::retire_previous);
    } catch (const std::runtime_error&) { live_refused=true; }
    const auto killed=kill(child,SIGKILL); close(release[1]); int code{};
    const auto waited=waitpid(child,&code,0);
    check(notified && live_refused && previous.valid() && killed==0 && waited==child &&
        WIFSIGNALED(code) && WTERMSIG(code)==SIGKILL);
    avsync::CorrectedAudioHandoff successor(temp.path,100'000'000,avsync::ipc::ReplacementPolicy::retire_previous);
    std::array<float,960> fresh,output; fresh.fill(-.5F); output.fill(7.F);
    const auto now=avsync::ipc::monotonic_ns();
    check(successor.consume({{21,1},0,now,480},fresh,now));
    avsync::ipc::FrameInfo info;
    using R=avsync::ipc::ReadResult;
    check(previous.read_next_due_audio(0,now+100'000'000,output,info)==R::disconnected);
    for (const auto sample:output) check(sample==7.F);
    check(previous.reconnect() && previous.generation()!=old_generation);
    check(previous.read_next_due_audio(0,now+100'000'000,output,info)==R::ok && output==fresh);
    check(info.sequence==1 && info.capture_ns==now && info.presentation_ns==now+100'000'000);
}
}
int main() {
    try {
        Temporary temp;
        std::array<float,1920> pcm{}; std::array<float,960> out{};
        for (std::size_t i=0;i<pcm.size();++i) pcm[i]=static_cast<float>(i)/4096;
        avsync::CorrectedAudioHandoff handoff(temp.path,100'000'000);
        avsync::ipc::Reader reader(temp.path); check(reader.valid());
        const auto capture=avsync::ipc::monotonic_ns();
        avsync::CorrectedAudio block{{3,1},0,capture,960};
        check(handoff.consume(block,pcm,capture+20'000'000));
        check(handoff.published_frames()==960);
        avsync::ipc::FrameInfo info;
        using R=avsync::ipc::ReadResult;
        check(reader.read_next_due_audio(0,capture+99'999'999,out,info)==R::empty);
        check(reader.read_next_due_audio(0,capture+100'000'000,out,info)==R::ok);
        check(info.capture_ns==capture && info.presentation_ns==capture+100'000'000 && info.sequence==1);
        for (std::size_t i=0;i<out.size();++i) check(out[i]==pcm[i]);
        check(reader.read_next_due_audio(0,capture+110'000'000,out,info)==R::ok);
        check(info.capture_ns==capture+10'000'000 && info.presentation_ns==capture+110'000'000 && info.sequence==2);
        for (std::size_t i=0;i<out.size();++i) check(out[i]==pcm[i+960]);
        // Exact continuous grid is mandatory; phase jumps close the old mapping.
        block.first_frame=960; block.capture_grid_ns=capture+20'000'001;
        check(!handoff.consume(block,pcm,capture+40'000'000));
        check(reader.read_next_due_audio(0,capture+120'000'000,out,info)==R::disconnected);
        for (int bad=0;bad<5;++bad) {
            avsync::CorrectedAudioHandoff next(temp.path,100'000'000);
            check(reader.reconnect());
            block={{3,1},0,capture,960};
            auto bad_pcm=pcm;
            if (bad==0) bad_pcm[0]=std::numeric_limits<float>::quiet_NaN();
            if (bad==1) block.first_frame=480;
            if (bad==2) block.frames=479;
            if (bad==3) block.epoch={};
            const auto now=capture+(bad==4 ? 201'000'000:20'000'000);
            check(!next.consume(block,bad_pcm,now)); check(!next.valid());
        }
        avsync::CorrectedAudioHandoff next(temp.path,100'000'000);
        check(reader.reconnect()); block={{3,1},0,capture,960};
        check(next.consume(block,pcm,capture+20'000'000));
        block={{3,2},960,capture+20'000'000,960};
        check(!next.consume(block,pcm,capture+40'000'000));
        check(reader.read_next_due_audio(0,capture+100'000'000,out,info)==R::disconnected);
        for (const bool expire : {false,true}) {
            for (std::size_t i=0;i<pcm.size();++i) pcm[i]=static_cast<float>(i)/4096;
            avsync::CorrectedAudioHandoff retry(temp.path,2'000'000'000);
            check(reader.reconnect());
            int notice[2],release[2]; check(pipe(notice)==0 && pipe(release)==0);
            const auto child=fork(); check(child>=0);
            if (!child) {
                close(notice[0]); close(release[1]);
                avsync::ipc::testing::briefly_hold_mutex(temp.path.c_str(),notice[1],release[0]);
            }
            close(notice[1]); close(release[0]); char marker{};
            check(read(notice[0],&marker,1)==1); close(notice[0]);
            const auto t=avsync::ipc::monotonic_ns();
            block={{4,1},0,t,960};
            const auto accepted=retry.consume(block,pcm,t+20'000'000);
            const auto count=retry.published_frames(),busy=retry.busy_retries();
            check(write(release[1],&marker,1)==1); close(release[1]);
            int status{}; check(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
            check(accepted && count==0 && busy==1 && retry.queue_peak()==2);
            pcm.fill(-1); // Retry owns its PCM, not the caller's subsequently modified span.
            if (expire) {
                check(!retry.heartbeat(t+200'000'001) && !retry.valid());
                check(reader.read_next_due_audio(0,t+201'000'000,out,info)==R::disconnected);
            } else {
                check(retry.heartbeat(t+20'000'000) && retry.published_frames()==960);
                check(reader.read_next_due_audio(0,t+2'000'000'000,out,info)==R::ok && out[0]==0 && out[1]>0);
            }
        }
        // A controller can replace a revoked mapping at the SAME trusted path.
        // Old readers never see the new generation without explicit reconnect,
        // and no future-dated PCM from the retired mapping is replayed.
        auto generation_one=std::make_unique<avsync::CorrectedAudioHandoff>(temp.path,100'000'000);
        check(reader.reconnect()); const auto old_generation=reader.generation();
        const auto t=avsync::ipc::monotonic_ns();
        pcm.fill(.25F); block={{5,1},0,t,960};
        check(generation_one->consume(block,pcm,t+20'000'000));
        generation_one->revoke();
        check(reader.read_next_due_audio(0,t+100'000'000,out,info)==R::disconnected);
        auto generation_two=std::make_unique<avsync::CorrectedAudioHandoff>(temp.path,100'000'000);
        pcm.fill(-.5F); block={{5,2},0,t+300'000'000,960};
        check(generation_two->consume(block,pcm,t+320'000'000));
        check(reader.read_next_due_audio(0,t+400'000'000,out,info)==R::disconnected);
        check(reader.reconnect() && reader.generation()!=old_generation);
        check(reader.read_next_due_audio(0,t+399'999'999,out,info)==R::empty);
        check(reader.read_next_due_audio(0,t+400'000'000,out,info)==R::ok);
        check(info.sequence==1 && info.capture_ns==t+300'000'000 && info.presentation_ns==t+400'000'000);
        for (const auto sample:out) check(sample==-.5F);
        process_replacement();
        std::cout<<"PASS desktop IPC: exact PCM/timestamps, due scheduling, split quanta, fault/epoch revocation\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

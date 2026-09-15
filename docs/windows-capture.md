# Windows capture metadata probe

`avsync-wasapi-probe` is an explicit, short observation of the default Windows render endpoint. It is not an audio sender, a microphone recorder, a clock synchronizer, or an A/V validation tool. It does not modify the existing audio bridge or OBS setup.

## Run deliberately

```text
avsync-wasapi-probe --help
avsync-wasapi-probe --loopback --seconds 5
```

No arguments only shows help; `--loopback` is mandatory to open an endpoint. Duration is a whole number from 1 to 30 seconds, default 5. The selected endpoint is the current default **render / console-role** endpoint at launch. The probe does not follow default-device changes or reconnect if the endpoint is invalidated.

The probe opens a shared-mode WASAPI loopback client with the endpoint's existing mix format, including its channel count and speaker mask. It requests a 100 ms capture buffer and polls without changing timer resolution or thread priority. It never changes endpoint format, speaker layout, mute, volume, routing, or the default device. It requests no microphone access. Microsoft documents loopback capture as a shared-mode render-endpoint operation, distinct from selecting a hardware loopback recording device. [Loopback Recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)

The application obtains each capture packet's pointer, discards the complete packet, and releases it immediately on the same thread. It never reads or copies the PCM, writes a recording, opens a network connection, or requests device names/IDs. Output is one JSON object on standard output after capture stops. The application itself writes no files; shell redirection would be a separate caller action.

## What the result means

- `format` reports mix encoding, sample rate, container/valid bits, channels, and channel mask. Eight channels alone must not be relabeled 7.1 without checking the mask. No downmix or resampling is performed. This is the shared audio engine's format, not proof of a hardware wire format. [GetMixFormat](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getmixformat)
- `packets` and `frames` count successfully consumed packets and audio frames. A frame includes one sample for each channel; it is not a byte count.
- `silent_packets` / `silent_frames` count the WASAPI silence flag only. Since PCM is not inspected, an unflagged packet is **not proof of nonzero or audible sound**. `silence_flagged_packets_only` distinguishes the all-flagged case. [Buffer flags](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/ne-audioclient-_audclnt_bufferflags)
- Discontinuity flags and timestamp-error flags are retained as observations, not automatically called transport failures. `initial_discontinuity_packets` separates a flag on the first packet; transitions can cause discontinuities. A timestamp-error flag means its timing is uncertain and must not be used blindly for clock fitting. [Buffer flags](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/ne-audioclient-_audclnt_bufferflags)
- Each timestamp pair describes the **first frame in that packet**: `device_position_frames` is the API's frame position, while `qpc_position_100ns` is the paired Windows performance-counter timestamp already converted by WASAPI to 100 ns units. It is **not raw QPC ticks, UTC, packet arrival time, or a Linux-clock timestamp**. No independent epoch is inferred or rebased here. [IAudioCaptureClient::GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer)

Metadata retention is fixed at 256 pairs: the first 8 packets, then roughly one per 250 ms, plus packets with discontinuity/timestamp-error flags while capacity remains. The final packet is retained separately. `timestamp_pairs_omitted` counts eligible samples omitted by the fixed capacity, not every intentionally unsampled packet. Nothing is allocated or printed while a packet is held. `empty_polls` is expected when nothing is available; it is not a dropout counter. The requested duration bounds the observation loop, not time spent inside a stalled system/driver API call.

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | Help, or a completed observation with packets; **not** a quality/sync pass |
| 1 | COM/WASAPI or unexpected failure; stage and HRESULT are reported when available |
| 2 | Invalid arguments; no endpoint is opened |
| 3 | Completed interval with no packets: `waiting_no_packets`, not working audio |

## Build and scope

The source is `apps/wasapi_probe.cpp`, C++20, Windows SDK / MSVC. The optional Windows target is `avsync-wasapi-probe`; `AVSYNC_BUILD_WINDOWS_PROBE` defaults on for Windows and off elsewhere. It links `ole32` and `uuid`. It does not require FFmpeg, GStreamer, an audio codec, AVRT, WinMM timer calls, or any network library.

From the repository root, with CMake and Visual Studio 2022 C++ build tools available:

```powershell
cmake -S . -B build/windows -G "Visual Studio 17 2022" -A x64 -DAVSYNC_BUILD_WINDOWS_PROBE=ON -DAVSYNC_BUILD_IPC=OFF -DBUILD_TESTING=ON
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release --output-on-failure
.\build\windows\Release\avsync-wasapi-probe.exe --help
```

The probe's CTest invokes help only and opens no audio endpoint. To perform a separate, deliberate metadata observation:

```powershell
.\build\windows\Release\avsync-wasapi-probe.exe --loopback --seconds 5
```

Validation status: the MSVC build, non-capture help test, and portable core tests passed. A separately authorized bounded loopback metadata observation also completed. This is not a PCM-content, sound-quality, channel-routing, or end-to-end synchronization result.

This probe can establish that the selected endpoint provides particular format/timestamp metadata over a bounded interval. It cannot establish useful sound, channel content, audio quality, drift, Linux clock mapping, restart recovery, downmix correctness, end-to-end latency, or synchronization with the Elgato/OBS path. Those remain separate capture/transport milestones. No host-specific results belong in this public document.

A future sender must negotiate and interpret the actual mix format and channel mask, then use a validated, explicit downmix/resampling policy when conversion is required. It must not assume that the Windows mix is stereo or already matches the bridge's sample rate. This probe intentionally performs neither conversion nor transport.

The packet lifecycle follows Microsoft's [GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer) and [ReleaseBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-releasebuffer) contracts. The implementation is original project code, not copied from a sample.

# Contributor instructions

- This is experimental software. Never describe synthetic/unit tests as hardware or end-to-end OBS validation.
- Do not touch a running OBS profile, physical capture device, firewall, startup task, microphone mute, or stream without an explicit maintenance step.
- Public repository: no real hostnames, LAN addresses, device IDs, account paths, credentials, recordings, personal OBS collections, or local deployment plans.
- Use C++20 and CMake; keep the core dependency-free and portable. Linux IPC/OBS are optional targets.
- GPL-2.0-or-later for original project code. Preserve notices on any incorporated code; prefer using libraries rather than copying implementations.
- Timestamp nanoseconds are signed 64-bit. Check arithmetic, limits and input metadata; no unbounded media queues.
- Keep capture timestamps distinct from final presentation timestamps. Use one session epoch, never independent arrival-time rebasing.
- OBS callbacks must never block waiting for IPC or I/O. No future-dated multi-second microphone submission.
- Test epoch changes, stale data, invalid sizes, late data and mute transitions. Silence is not missing media.
- Use apply_patch for edits. Do not commit or push unless the coordinating agent asks.

# Explicit same-sender desktop recovery

`avsync-network-receiver --desktop-ipc NEW_PRIVATE_FILE --recover-desktop-ipc`
opts a finite desktop-only diagnostic into recovery between media generations
of **one already admitted sender process/session**. The other required network
arguments are unchanged. The recovery flag requires desktop IPC; it does not
enable an unattended service or microphone capture, change OBS, or authorize a
new sender session. Without the flag, output-enabled diagnostics still stop on
the first generation fault.

## What is retained and what is replaced

The receiver keeps its shared monotonic clock provider and transport alive
while waiting for the sender to requalify its clock. The Windows sender already
drops unqualified capture, collects a fresh clock observation window, and starts
a new SSRC/media generation after qualification. None of those health limits
are relaxed by this receiver option.

A provider/RTCP health loss or no-progress/original-anchor-age watchdog closes
the old desktop mapping immediately on the receiver owner thread. Old DSP,
retry PCM and unpublished future-dated IPC audio are discarded. Packets from
that faulted generation cannot revive it. A new generation must pass existing
same-session admission: a strictly higher generation, unused SSRC, frame zero,
anchor zero, original device origin and the same provider clock token. Already
retired generations are ignored. Foreign sessions and invalid new admission,
metadata, PCM, phase, rate, queue or output failures remain terminal.

An atomic same-session retirement fence is also visible to the ordered RTP
callback, so late packets from an already failed generation cannot kill the
provider while it waits. If an expired packet arrives before the owner watchdog
runs, a copied-validator metadata check must first prove expiry is the only
problem; conflicting anchors, nonmonotonic metadata and foreign identities do
not qualify. The packet is still rejected. Only a retirement request crosses
to the owner, which revokes before its next DSP dispatch; no DSP, filesystem
work, or stale-PCM forwarding occurs inside the network callback.

For an admitted successor, the owner recreates the private mapping at the same
path using the existing singleton lock, private-file checks and atomic rename.
The old mapping stays offline even for readers that still hold it. The new
worker re-primes from fresh original anchors; priming PCM is never replayed.
The replacement mapping has a new IPC generation and a new sequence cursor,
but capture/presentation dates remain in the original Linux monotonic domain.
No arrival-time rebasing or retroactive timestamp changes occur. A successor
arriving before the old watchdog fires also explicitly retires the predecessor
before priming.

The existing native OBS audio worker retries a disconnected mapping every
250 ms outside OBS callbacks. It retains its output sample grid and monotonic
cursor across reconnects, supplies bounded silence, and trims outdated samples
instead of replaying a backlog. That behavior requires an encoded OBS recovery
test before it can be claimed as live-validated. Already submitted OBS audio
cannot be retracted; mapping revocation is not instantaneous downstream mute.

## Bounds and honest results

Storage and admission remain limited to eight media generations; the existing
1..180 second diagnostic duration remains the outer deadline. Recovery is not
gapless: clock acquisition, three original-clock estimator windows and the
presentation delay must complete again. No polling/restart loop is unbounded.

`same_session_recovery_enabled`, `awaiting_media_generation`,
`retired_correction_packets` and `ipc_recreations` expose state. IPC frame/retry
totals include retired mappings. Every session retains its fault history.
The existing strict verdict is unchanged: an unplanned interruption remains a
failed/degraded diagnostic even if output later recovers. A deliberate
provider-pause fixture passes only with its expected two generations and
prompt fault at the pause edge; extra recoveries are not hidden.
Final `status=recovered_with_gap` requires a running, unfaulted final generation
with at least one second of delivered PCM and qualified active timing. It is
not a clean-run verdict: an unplanned recovery still exits 3; only the strict
expected provider-pause fixture can pass with that status. If no successor has
arrived by the finite deadline, status is `awaiting_media_generation` and exit
is 3. Terminal failures exit 1. Callers must inspect both status and retained
per-generation faults, not equate process liveness or recovery with a clean run.

This is not complete reboot/process-restart recovery. A new sender process has
a new session identity, and a receiver restart has a new provider clock token;
those require a separate authorized rendezvous/startup manager. The new
[finite process-pair control fixture](process-pair-control.md) begins that work;
it is not an installed service or physical restart qualification. A receiver-only
packet/metadata failure is not repaired by pretending later packets belong to a
new generation. The current peer filter is not authentication or encryption.

## Regression boundaries

Diagnostic tests cover health and stale transitions, repeated old packets,
successors before timeout, monotonic fresh output, the eight-generation budget,
foreign/reused/missing-origin generations, failed output creation, invalid PCM
and missing hooks. The handoff test replaces a real mapping and checks that an
old reader remains disconnected, the new reader sees only the new PCM, and
capture/presentation timestamps are preserved exactly. These are software
tests, not physical-clock, acoustic A/V or OBS restart validation.

## First finite physical recovery trial

On 2026-09-15 an isolated 118-second receiver intentionally paused its provider
for five seconds, starting ten seconds into the run. The same Windows loopback
sender process reacquired the clock and created a successor media generation.
The real video path and isolated native OBS recorder remained running.

- Exactly two correction generations were retained. The first had the expected
  health fault; the second was running and fault-free at the finite deadline.
- The receiver replaced IPC once and reported `recovered_with_gap`, with the
  strict planned-pause verdict passing. It published 5,038,560 frames across
  both generations, including 4,752,000 after replacement; no network jitter
  drops or IPC failure occurred.
- OBS reported two connections/two generations, zero late skips, invalid
  blocks or discontinuities. It filled 523,682 samples per channel with silence
  (about 10.910 seconds) across the interruption/reacquisition period. This is
  recovery with a gap, not uninterrupted audio.
- Playback began after reacquisition. All six independently identified visual
  and audio events were present afterward. That recording's median A/V offset
  was +6.333 ms and passed its requested timing limits, but other unchanged
  fresh starts did not; see [the calibration failures](physical-combined-validation.md).

The pre-interruption source was silent. This trial verifies mapping replacement,
OBS reconnection and actual post-recovery audio, **not** audible stale-content
replay behavior across the gap. The software mapping tests cover retirement
with distinguishable PCM. New sender/receiver processes, machine reboots,
unplanned outage repeats, loaded operation and microphone privacy remain open.

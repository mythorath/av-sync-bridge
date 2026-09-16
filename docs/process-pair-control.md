# Finite desktop process-pair control

`tools/process_pair.py` is an **offline-tested, finite control-plane fixture**,
not an installed service, audio-health monitor, machine-reboot manager, or OBS
migration. It does not capture the microphone, change an OBS source, start a
stream, change a firewall, or install startup tasks. Native commands are supplied
explicitly by a trusted local operator for an authorized maintenance test.

The existing [same-sender recovery](desktop-recovery.md) handles a new media
generation from one sender. A whole-process interruption instead retires both
children and starts a fresh receiver/provider and a fresh sender session. This
coordinator never asks a receiver to admit an unrelated sender into an old pair.

## Agreement, not signal evidence

Each attempt generates a fresh nonzero 64-bit sender-session token. The receiver
is launched with `--expect-sender-session TOKEN --control-stdin`, and emits one
standalone stdout line after becoming ready:

```text
AVSYNC_CONTROL {"schema":1,"event":"receiver_ready","clock_epoch":"123","sender_session":"456"}
```

The sender is then launched with that clock token, the same session token,
`--loopback --sender-session TOKEN --clock-epoch TOKEN --control-stdin`. Its one
acknowledgment uses event `sender_started` and exactly the same two identities.
Tokens are decimal strings, not JSON numbers: values above JavaScript's exact
integer range must survive unchanged. Zero, noncanonical decimal forms, overflow,
duplicate JSON keys, unexpected fields/events, repeated provider tokens, stale
acknowledgments, and duplicate handshakes fail closed.

These two messages establish **only process agreement**. They do not establish
qualified clocks, captured PCM, valid correction, fresh IPC, OBS reconnection,
physical A/V timing, gapless audio, or production readiness. Native final JSON
summaries are collected separately as reported counters, never as independent
audio verification. Handshakes on stderr are not accepted. Result JSON always
labels evidence `process_agreement_only` and `media_verified=false`.

The coordinator writes exact ASCII `AVSYNC_KEEPALIVE\n` about once per second
to both child stdin pipes. Native `--control-stdin` uses a five-second lease;
EOF, malformed input, expired lease, and read failure stop the native process.
`AVSYNC_STOP\n` requests cooperative cleanup. A process is not allowed to extend
an already expired lease by consuming old buffered keepalives.

## Private configuration and launch

Use Python 3.10 or newer. The private configuration must contain command **argv
arrays**, not shell snippets. Only exact arguments `{sender_session}`,
`{clock_epoch}` (sender only), and `{seconds}` are substituted. Embedded string
interpolation is rejected; execution uses `shell=False`. Both commands must
include `--seconds {seconds}`. Windows children use `CREATE_NO_WINDOW`.

Minimal configuration shape (replace executable paths and add the native
network/output arguments appropriate to an explicitly authorized test):

```json
{
  "receiver_argv": [
    "/private/bin/receiver-wrapper",
    "--expect-sender-session", "{sender_session}",
    "--seconds", "{seconds}", "--control-stdin"
  ],
  "sender_argv": [
    "/private/bin/avsync-windows-sender",
    "--loopback", "--sender-session", "{sender_session}",
    "--clock-epoch", "{clock_epoch}",
    "--seconds", "{seconds}", "--control-stdin"
  ],
  "total_seconds": 60,
  "max_attempts": 3,
  "ready_timeout": 10,
  "ack_timeout": 10,
  "stop_grace": 1,
  "receiver_scope": "remote",
  "require_native_summaries": true
}
```

This shape is intentionally not a ready-to-deploy site configuration. Keep real
endpoints, device choices, credentials, and paths outside the public repository.
Do not put passwords or keys in argv. Launch with an absolute, stable lock path
in a private directory on a **local filesystem**:

```text
python tools/process_pair.py --config PRIVATE_CONFIG.json --lock-file ABSOLUTE_PRIVATE_LOCK_PATH
```

`receiver_scope` is `direct` (the default for local fixtures) or `remote`.
A known `ssh`/`ssh.exe` executable requires explicit `remote` scope. A trusted
wrapper that starts SSH must also be declared remote: inspecting its filename
cannot prove what it launches. This setting is a declared containment boundary,
not discovery or a sandbox. Never label an SSH wrapper direct to enable retries.

The process holds an OS lock for the run. A second coordinator using that path
cannot start children. The file is never unlinked, replaced, truncated, or
reclaimed based on a recorded PID. All operators of this pair must use the same
lock path. This is not a global host lock or a substitute for the native IPC
writer lock; two different lock paths do not provide mutual exclusion. Protect
the parent directory and file from other writers (private ACLs on Windows).
On POSIX, the final parent directory and lock must belong to the current user
with no group/other permission bits; symbolic links, hard-linked locks, and
nonregular files are rejected. The open file identity is compared with the
linked path after acquiring the OS lock. Network-filesystem locking and hostile
local users are outside this fixture.

### SSH boundary

An authenticated SSH client can be the local receiver child, with a fixed trusted
remote wrapper that validates numeric arguments and **execs** the receiver in
the foreground. Do not allocate a pseudo-terminal, detach/background the remote
receiver, or redirect away its control stdin/stdout. Configure bounded SSH
connection attempts/timeouts, noninteractive authentication, and strict host-key
verification in the private command/config. Never disable host-key verification
to make a test pass.

Although this Python program does not invoke a shell, standard SSH remote command
execution has its own shell parsing. The remote wrapper/path and command arguments
are trusted configuration, not an escaping or sandboxing interface. This fixture
does not manufacture a safe arbitrary remote command line.

The Python coordinator owns only its exact **local** `Popen` child handles. It
cannot guarantee termination of arbitrary grandchildren or remote daemons by
killing SSH. The native stdin lease requests cooperative retirement after a
disconnect; it is not a hard guarantee against a native process stuck in a
third-party call or blocked cleanup. A wrapper must not buffer/replay keepalives
or keep the pipe alive after SSH loss. Any forced local kill or failed cleanup
ends this run without another attempt: a dead SSH client is not proof that the
remote writer stopped. Validate the wrapper before a live trial. No
process-name kill, task-name kill, or remote blanket cleanup is implemented.

In remote scope, **any unexpected receiver transport exit or stdout loss** also
latches `remote_retirement_unverified` and forbids another attempt, even if the
SSH process was already dead before cleanup. The owned sender is stopped safely.
No independent remote-identity/exit-proof mechanism exists yet; a local SSH exit,
released port, expired heartbeat, or next IPC writer rejection is not that proof.

## Bounds, cleanup, and results

- The overall deadline policy is 1–180 seconds, at most three pair attempts. Cleanup time
  is reserved inside that budget; the running interval is shorter than the
  requested total. Native finite-duration limits remain in force.
- Ready and acknowledgment waits are independently bounded by 0.1–30 seconds
  and the remaining overall deadline. A timeout is not successful agreement.
- Either child exiting, losing its output pipe, or failing its control input
  retires the entire pair. A fresh attempt never overlaps live owned children.
- Cooperative stop is sent to both, then only non-exiting owned local children
  are killed and reaped. Nonzero cooperative-stop exits are retained as failures.
  A forced kill, nonzero stop, or failed reap is an unclean result; no restart
  follows any of them. Process creation, kernel scheduling, blocked external
  I/O, and bounded thread/reap waits can exceed a nominal deadline; this is a
  finite normal-path policy, not a real-time or remote-termination guarantee.
- Stdout EOF can precede publication of a child's nonzero process exit. If
  cleanup sees that child as still alive when requesting STOP, its subsequent
  nonzero exit is deliberately treated as ambiguous cleanup and ends retries.
  This conservative boundary applies even when the local child does eventually
  finish. The known-dead-child restart fixtures synchronize on the exact process
  handle; they do **not** establish automatic recovery from every real crash.
- Ordinary stdout lines are bounded to 4096 bytes, native JSON summary lines to
  65536 bytes, control messages to 1024 bytes, and combined stdout/stderr output
  to 1 MiB per child. Pending control events are bounded to 64 and queued stdin
  messages to two. Ordinary text is counted/discarded; stderr is discarded in
  fixed-size chunks. Final reader errors and unexpected control messages during
  cleanup are retained, not silently lost after the running poll ends.
- Fault history survives restart. Exit 0 means only the finite process-control
  interval completed without observed control faults. Exit 3 means it completed
  after a recorded recovery. Exit 1 means failed control/cleanup; exit 2 means
  invalid configuration. Neither 0 nor 3 is a successful audio-quality verdict.

For a restarted receiver publishing desktop IPC, its private command must also
use the native explicitly authorized replacement mechanism. The Python fixture
does not unlink mappings or override the receiver's lock/inode/protocol checks.
It does not replace a live receiver owned by another controller. Native IPC
retirement and OBS stale-read behavior need their own tests; process agreement
must not be used to mask missing data-plane validation.

## Separate native diagnostics

`require_native_summaries` is a strict boolean, default `false` so existing
control-only fake children remain valid. Each attempt retains at most one
sanitized native JSON summary per role. A summary must have schema 1 and a known
status; duplicate summaries/keys, malformed JSON, nonfinite numbers, invalid
counter types/ranges, oversized lines, or missing required summaries cannot
produce a positive diagnostic result. Python integers preserve 64-bit counter
values without a float conversion.

`native_diagnostics` contains role, attempt, fixed reason/status codes, and a
small numeric/boolean field allowlist. Unknown strings, paths, peer addresses,
clock/session identifiers and raw errors are never copied into result JSON.
Receiver entries retain at most eight `correction_sessions`, containing only
numeric `state`, `fault`, and `delivered_frames` values (invalid/missing values
become null), plus `correction_session_count` and
`correction_sessions_truncated`. They retain `ipc_failure` only from the fixed
native failure-code allowlist; any other value becomes `unknown`. The exact
native success code is `none`; empty, missing, and unknown codes are not success.
`native_summary_requirement_met` is null when optional; when required it checks
that both roles in every attempt supplied valid summaries. Error or unqualified
native statuses remain separately visible even when their summary is valid.

`reported_media_qualified` requires native reports of actual packet/anchor and
corrected-IPC progress, qualified clock/correction counters, and no selected
timing/queue/fault indicators; a successful STOP alone is insufficient. Per-role
results retain counter-only qualification, while the aggregate also requires a
completed control run. These are **native self-reports**, not observed playback,
OBS receipt, clock accuracy or physical A/V evidence. `media_verified` remains
false. Missing/invalid/unqualified summaries do not rewrite the independent
control exit code; callers that request diagnostics must inspect those fields.
Hard-killed children generally have no final summary: absence stays explicit.

## Current verification and open work

`python -m unittest discover -s tests -p test_process_pair.py` uses only local
fake child processes. It covers exact string tokens, bad schemas/configuration,
duplicate lock, sender/receiver deaths, fresh pairs, stale/foreign/reused tokens,
duplicate handshakes, ready/ack/deadline/attempt bounds, stdout loss, stderr
spoofing, output floods, cooperative stop, forced owned-child cleanup, and EOF.
Retry-count tests inject crashes only after both handshakes and wait for the
specific child's exit before coordinator polling; a separate EOF-before-exit
fixture verifies the conservative no-retry outcome described above.
It also checks bounded native summaries larger than 4 KiB, exact uint64 counters,
privacy filtering, missing/duplicate/nonfinite/unqualified reports, final-output
faults during cleanup, explicit SSH scope, and no retry after remote-wrapper
exit or stdout loss. These diagnostic cases use generated summaries only.
These fake-process tests do not contact SSH, a physical device, OBS, or a normal
audio path.

On 2026-09-16, a separate finite trial paired the actual Windows sender and Linux
receiver through the authenticated SSH control arrangement. It completed one
attempt with one acknowledged pair in 21.432 seconds, with no control faults,
forced local kills, nonzero-stop failures, or reap failures. This is **native
process-agreement evidence only**, not PCM/clock qualification, audio quality,
OBS output, physical A/V timing, or native failure/restart recovery. It did not
install startup behavior or migrate the normal OBS path.

Still required before deployment: native failure/restart tests, actual
authenticated SSH disconnect/lease behavior, crash-retired IPC and OBS reader
validation, startup ordering/exclusive ownership, loaded long-duration recovery,
the full restart/reboot matrix, production source migration and physical A/V
recalibration. The microphone remains outside this desktop-only fixture.

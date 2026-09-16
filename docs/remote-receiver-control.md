# Finite remote receiver containment

`tools/remote_receiver.py` is a Linux-only helper for explicitly authorized,
finite receiver tests. It creates one transient **user** service per attempt;
it does not install startup behavior, require root, change a firewall, touch
the microphone, or alter an OBS source. Its unit tests do not start systemd
services, capture media, contact SSH, or establish hardware/OBS verification.

The intended control transport is the existing authenticated SSH connection.
There is no new listening network port. The caller must retain strict host-key
verification, bounded connection attempts and noninteractive authentication.
Host endpoints, account paths, device arguments and credentials stay in private
deployment configuration, never in the public repository or diagnostic output.

## Why a service and a launch fence

Killing an SSH client does not prove the receiver stopped. A late SSH command
could also create a receiver *after* a naive status check found none. Every
attempt therefore has a fresh caller-generated 128-bit hexadecimal run ID, an
immutable intent, a never-replaced authorization lock, and an irreversible
cancellation tombstone. All launch gates and retirement requests use that same
lock. An attempt ID is never reused, even after success or cancellation.

The transient service uses `Type=exec`, `Restart=no`, `KillMode=control-group`,
`SendSIGKILL=yes`, `TimeoutStartSec=5`, `TimeoutStopSec=2`, and a runtime bound of
the requested native duration plus five seconds (at most 185 seconds).
`systemd-run --user --quiet --pipe --wait --expand-environment=no` preserves the
native stdin/stdout control pipes. The start helper execs this launcher directly;
it does not leave a separate wrapper-owned launcher child behind. `--scope`,
automatic restart, and installed/enabled units are deliberately not used.
[systemd-run, version 255](https://raw.githubusercontent.com/systemd/systemd/v255/man/systemd-run.xml)

The existing native five-second stdin lease is still cooperative. The independent
service runtime limit bounds a receiver stuck inside native code; a retirement
request asks the manager to stop the whole exact unit, including descendants.
The manager escalates remaining members to SIGKILL after its stop timeout.
This is not a real-time guarantee: uninterruptible kernel I/O, a hung manager,
or OS scheduling can prevent timely death. Such cases must remain unproved.
[systemd process-killing policy, version 255](https://raw.githubusercontent.com/systemd/systemd/v255/man/systemd.kill.xml)

## Private configuration and commands

The state directory must already exist on a private local filesystem, belong
to the current user, and have no group/other access bits. Keep it stable and do
not remove attempt directories, lock files, receipts, or tombstones while an
old launch might still arrive. Configuration and state files must be private,
same-owner, regular, single-link files; final symlinks/FIFOs are refused. State
records use exclusive creation, bounded reads, file/directory synchronization,
and close-on-exec descriptors. Partial records fail closed rather than being
repaired or interpreted as absence.

The only configuration key is `receiver_argv`, a trusted argv array beginning
with an absolute executable path. Required arguments are exact, single-use
`--expect-sender-session {sender_session}`, `--seconds {seconds}`, and
`--control-stdin`. Additional private native options supply the clock/network
and explicitly authorized desktop IPC settings. No shell string is evaluated.

```json
{
  "receiver_argv": [
    "/private/bin/avsync-network-receiver",
    "--expect-sender-session", "{sender_session}",
    "--seconds", "{seconds}", "--control-stdin"
  ]
}
```

The example omits required site-specific native options; it is not a ready-made
deployment configuration. The public CLI is:

```text
remote_receiver.py start --state-dir PRIVATE --config PRIVATE --run-id HEX32 --expect-sender-session DECIMAL --seconds 1..180 --control-stdin
remote_receiver.py retire --state-dir PRIVATE --run-id HEX32 --expect-sender-session DECIMAL --challenge HEX32
```

Run/session identity must be retained by the coordinator **before** launching
SSH. A retirement challenge is newly generated for each query; replaying an old
proof must not authorize another attempt. Sender-session values are canonical
nonzero uint64 decimal strings, not floating-point JSON numbers.

Both command templates **must use the same fixed authenticated host, user,
helper deployment, and absolute state directory**. Generate them from one trusted
private base configuration. Generic argv arrays cannot prove that association:
querying another empty state directory could return `fenced_never_started` for
the correct nonce without fencing the real launcher. The challenge is freshness,
not authentication or control-domain enrollment. Do not accept a retirement
template as safe merely because its response fields match.

`start` durably reserves the intent before execing the manager client. The
internal `gate` subcommand runs inside the uniquely named transient unit. Before
execing native code, it checks the same cancellation lock, boot identity,
manager-provided invocation ID, actual cgroup/MainPID, service policy, and time
bounds. It then synchronizes an immutable receipt containing the boot, unit,
invocation, and cgroup device/inode identity. The lock stays held through native
exec and is released by close-on-exec. An exec failure leaves the receipt for
normal retirement verification; it never creates a success proof.

An invocation ID identifies one runtime cycle, not just a service name. Successful
transient units may be garbage-collected even without `--collect`, so the receipt
is needed independently of the launcher's lifetime. `--remain-after-exit` cannot
be combined with this pipe/wait mode.
[Invocation identity, version 255](https://raw.githubusercontent.com/systemd/systemd/v255/man/systemd.exec.xml),
[transient-unit collection](https://raw.githubusercontent.com/systemd/systemd/v255/man/systemd-run.xml)

## Independent retirement proof

`retire` first acquires the per-attempt lock with an eight-second total query
budget, validates any existing state, and synchronizes a permanent cancellation
tombstone. It can reserve and cancel an attempt even before `start` reaches
Linux. A delayed start or gate then refuses to execute the receiver.

Only exit 0 and one ASCII line of at most 2048 bytes constitutes a proof:

```text
AVSYNC_RETIRE {"schema":1,"event":"receiver_retired","run_id":"11111111111111111111111111111111","sender_session":"123","challenge":"22222222222222222222222222222222","proof":"fenced_empty_cgroup"}
```

The coordinator must match the exact schema, run, session and fresh challenge.
The three proof kinds are:

- `fenced_never_started`: cancellation was committed under the same exclusive
  lock and no native authorization receipt exists. This is **not** inferred from
  a missing receipt alone: every native exec must first commit that receipt while
  holding the lock, and the new tombstone prevents every future authorization.
  A late service gate may still briefly start, but cannot exec the receiver.
- `fenced_prior_boot`: validated intent belongs to a prior kernel boot and the
  new cancellation fence prevents reauthorization. No current unit is signalled.
- `fenced_empty_cgroup`: a validated receipt binds this exact service invocation.
  A fresh manager query must match that invocation and show a terminal unit with
  no main/control process or pending job; the recorded cgroup must be empty or
  absent. An explicitly present empty `Job=` property is the systemd 255
  no-job representation; a missing property or nonempty pending job is not
  accepted. A collected unit also requires that recorded cgroup check. Unit absence
  alone, PID absence, released ports, heartbeat expiry and launcher exit are not
  sufficient. Cgroup inode changes, foreign invocations and inaccessible state
  are unknown, never permission to signal another process.

The recursive `populated` field covers live processes in the cgroup and its
descendants; it is stronger than reading one PID or an incomplete process list.
An empty cgroup alone is also insufficient while its unit remains active or has
a pending job. The cancellation fence is what prevents later authorized reuse.
[Linux cgroup v2 population semantics](https://docs.kernel.org/admin-guide/cgroup-v2.html)

Only the exact validated unit is stopped, and only once. There are no process-name
kills, numeric receiver-PID kills, unit globs, IPC unlinks, or forced takeover of
an unrelated writer. Queries retain bounded output and deadlines; errors produce
only a fixed reason code on stderr, no identifiers or private native text.
Native control/summary stdout remains distinct from retirement stdout.

## Trust and verification limits

This is containment for cooperating project components under one trusted local
user, not a security sandbox against that same user, root, a hostile kernel,
manual unit-name reuse, cgroup migration, state deletion, or modified helper code.
The user service manager and the private local filesystem are part of the trust
boundary. In particular, systemd's stop-by-name API has no compare-and-stop
invocation argument: identity checks plus irreversible application-level
non-reuse are required. Different coordinator lock paths are not mutual exclusion.

The generated tests cover cancellation-before-launch, delayed gates, durable
receipt-before-exec ordering, identity mismatch, collection, live descendants,
unavailable queries, prior boots, stale state and privacy bounds. Linux-only
filesystem cases check private permissions, symlinks/hardlinks and nonblocking
FIFO rejection. They do **not** validate actual systemd behavior or live media.
Independent finite manager/SSH fault tests are a separate maintenance step.
Retirement proof never certifies capture quality, shared-clock qualification,
IPC consumption, OBS output, A/V sync, or recovery across an actual reboot.

## Explicit generated-process manager check

On a Linux host with a running user service manager, unified cgroup v2 and Python
pidfd support, an operator can deliberately run:

```sh
python3 tools/check_remote_receiver_control.py --run-systemd \
  --output-dir /private/new-control-check
```

This creates fresh private test state and transient units. Unlike ordinary unit
tests, it **does** invoke the real manager. It never opens a capture device,
network listener, microphone, OBS instance or installed service. The three cases
are cancellation-before-launch, cooperative STOP, and loss of the launcher while
a stubborn generated receiver and descendant remain alive. The checker holds
kernel pidfds before the fault and independently requires both to show exit
after the retirement proof; it does not kill native numeric PIDs to make a test
pass. Exact launcher handles are cleaned up separately, and independent remote
retirement is attempted even when launcher cleanup fails.

State and cancellation tombstones are retained intentionally, not recursively
deleted after a test. A failed transient unit can leave manager status metadata;
an operator may reset only the exact known test unit after verified retirement.
Never use a unit-name glob for stopping or cleaning up other attempts.

See the [checkpoint](checkpoint-2026-09-16.md#follow-up-independent-remote-retirement)
for real-manager and two-host SSH results, retained failures and remaining limits.

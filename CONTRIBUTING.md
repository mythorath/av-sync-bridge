# Contributing

This project is in its synthetic-prototype phase. Please read the status in the
README and the [roadmap](docs/roadmap.md) before installing anything in a live rig.

Contributions are welcome under GPL-2.0-or-later. Keep changes focused, include
reproducible tests, and explain what is measured versus assumed. Do not copy code
from another project without compatible licensing and preserved attribution.

Run CMake/CTest on the portable core. Linux changes also need the IPC tests; OBS
changes need a separate profile and an encoded-output test, not only a successful
module load. Synthetic tests must not require hardware, Internet access, secrets,
root, or a normal OBS installation to be modified.

Never attach credentials, private OBS collections, real microphone recordings,
device IDs or unredacted personal paths. Use documentation IP addresses and fake
endpoints in examples. Generated build outputs and machine-specific configuration
belong outside git. Do not bundle your private bridge's scripts into this project.

For a defect report, include commit, OS/OBS versions, build options, reproduction,
expected/actual timing, and sanitized counters. State whether it was a simulation,
IPC test, synthetic OBS recording, or physical end-to-end test. A clock estimate
or process health check is not proof of actual lip sync.

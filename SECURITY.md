# Security and privacy

Experimental software; no production security guarantee or supported stable
release exists yet. The initial prototype uses same-user local IPC and synthetic
media only. Network listeners are not part of that milestone.

- Treat shared-memory contents as untrusted: validate ABI, dimensions, strides,
  lengths and epochs before copying or drawing.
- IPC is same-host/same-user, not a portable wire protocol or a sandbox against a
  malicious process with the same operating-system identity.
- Keep runtime files private. Never run the prototype as root.
- Do not put credentials, recordings, session configurations, private keys, or
  authentication tokens in this repository or issue attachments.
- Do not report a flaw with an exploit targeting another user's live stream.

Use GitHub private vulnerability reporting if it is enabled. Otherwise open a
minimal issue asking the maintainer for a private channel, without disclosing
credentials, private data, or exploit details. No response-time SLA is promised.

Mic mute is a presentation privacy boundary, not a way to retract audio already
encoded or sent downstream. Until the complete OBS/filter path is verified, do
not trust this prototype with a sensitive live microphone.

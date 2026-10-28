# Security & Privacy Policy

OpenDM is built to be secure, privacy-first, and completely transparent.

---

## Privacy & Telemetry Commitment

* **Zero Telemetry**: OpenDM does not contain any tracking scripts, usage telemetry, crash reporters, or analytics beacons.
* **No Cloud Phoning**: The binary connects strictly to the download URLs provided by the user or intercepted from the browser.
* **Credential Isolation**:
  * Passwords and authentication headers stored in the Site Logon Vault (`~/.config/opendm/site_logins.cfg`) are stored locally on your filesystem and never uploaded to any remote server.
  * Browser cookies and session tokens intercepted by `opendm-native-host` are piped directly over a local Unix domain socket (`/tmp/opendm.sock`) strictly to the local `opendm` process.
* **Local Operations**: All central directory parsing, MP4/TS media stream muxing, and segment reassembly occur strictly in local memory and disk buffers.

---

## Independent Source Audit

OpenDM is 100% open-source software under the GPL-3.0 license. Anyone can independently review, verify, and compile every line of code:

* **Socket Splitting & Chunks**: [`src/core/chunk_manager.cpp`](src/core/chunk_manager.cpp)
* **Native Messaging & Wire IPC**: [`src/ipc/native_msg_host.cpp`](src/ipc/native_msg_host.cpp)
* **Local Domain Socket Server**: [`src/ipc/ipc_server.cpp`](src/ipc/ipc_server.cpp)
* **MPEG-DASH / MP4 Stream Muxing**: [`src/core/media_muxer.cpp`](src/core/media_muxer.cpp)

---

## Reporting a Vulnerability

If you discover a security vulnerability in OpenDM:

1. Please do not open a public issue.
2. Email the maintainer or open a private security advisory through GitHub.
3. Include detailed steps to reproduce the issue and any relevant system logs.
4. We will investigate and publish a patch promptly.

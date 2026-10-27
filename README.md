# OpenDM (Open Download Manager)

**OpenDM** is a high-speed dynamic multipart download accelerator for Linux, built in modern **C++20** and **GTK4**.

It was developed through a comprehensive clean-room reverse-engineering analysis of **Internet Download Manager (IDM v6.43.15)** (`idman643build15.exe`), faithfully implementing IDM's proprietary algorithms, scheduling math, remote ZIP inspection, Tonec browser wire protocols, and media stream remuxing without any proprietary runtime binary dependencies.

---

## Key Features

### 1. Dynamic Segmentation Core (`chunk_manager.cpp`)
* **Algorithmic Threshold (`NeedNewConnection`)**:
  $$\text{FreeBytes} \ge 2 \times \text{MinChunkSize} + 3 = 204,803\text{ bytes (where } \text{MinChunkSize} = 100\text{ KB)}$$
  Enforces IDM's exact threshold before spawning additional chunk connections.
* **Non-Disruptive Midpoint Carving (`split_chunk`)**:
  Truncates an active chunk's boundary at the midpoint of remaining bytes without tearing down the established TCP socket. A new worker takes over the upper half $[\text{midpoint}, \text{end}]$.
* **Sequential 64 KB Assembler (`join_file`)**:
  Combines downloaded parts in ascending offset order using buffered I/O, validating byte-for-byte stream integrity.

### 2. CSpeedLimiter Token Bucket Throttler (`speed_limiter.cpp`)
* Faithfully models Tonec's delta-millisecond token bucket algorithm to enforce steady, sub-millisecond bandwidth pacing.

### 3. Remote ZIP File Previewer (`zip_previewer.cpp`)
* **HTTP Range Tail Probe**: Fetches only the last 65,536 bytes of a remote `.zip` archive (`Range: bytes=-65536`).
* **Backward Scan for EOCD**: Scans backward for End of Central Directory signature (`PK\x05\x06`).
* **Selective Extraction**: Inflates any selected file from the Central Directory in memory via HTTP Range without downloading the entire archive.

### 4. Media Stream Remuxer & DASH Parser (`media_muxer.cpp`, `dash_parser.cpp`)
* **ISOBMFF Container Muxer**: Replicates `idmvconv.dll` (`MuxMp4AndMp4ToMp4`, `ConvertTsToMp4`), combining video-only and audio-only elementary streams (e.g. YouTube DASH 1080p/4K) into a unified `.mp4` container without re-encoding by updating `moov`, `mvhd`, track IDs, and sample chunk offsets (`stco`/`co64`).
* **MPEG-DASH MPD Parser**: Replicates `IDMVMPrs.dll`, resolving multi-bitrate XML manifests into video and audio representations.
* **MPEG-TS Segment Concatenator**: Assembles 188-byte synchronized transport stream segments for HLS downloads.

### 5. Tonec Native Wire Protocol & Browser Extension (`opendm-native-host`)
* Supports Tonec's native wire messaging protocol (`MSG#...` frames with tags `6=url`, `100=filename`, `50=referer`, `51=cookies`, `54=userAgent`).
* Fully compatible with the official **IDM Integration Module** browser extension (Manifest V3) for Chrome, Chromium, Brave, and Firefox.
* Standard I/O 32-bit length-prefixed IPC host with automatic daemon detachment and socket lifecycle management.

### 6. Subsystem Integration
* **Site Logon Vault (`site_logon.cpp`)**: Per-domain credential store supporting Basic, Digest, and Bearer authentication with wildcard host matching.
* **Batch Importer & Exporter (`batch_importer.cpp`)**: Native serialization and parsing of IDM `.ef2` batch files and plain text URL lists.
* **Site Grabber Spider (`site_grabber.cpp`)**: Recursive multi-depth web spider with domain boundary controls and file category filters.
* **Post-Download Management (`post_download.cpp`)**: Native FreeDesktop desktop notifications, antivirus scanner hooks (e.g. `clamscan`), and system shutdown scheduler actions.
* **GTK4 Desktop GUI**: Full desktop interface with toolbar, category sidebar, downloads queue table, and real-time per-chunk visualizer bars.

---

## Building from Source

### Dependencies
On Debian / Ubuntu / Linux Mint:
```bash
sudo apt-get install -y cmake ninja-build g++ pkg-config libgtk-4-dev libcurl4-openssl-dev zlib1g-dev libssl-dev
```

### Compile
```bash
cmake -B build -S . -G Ninja
cmake --build build
```

### Run Automated Tests
```bash
ctest --test-dir build --output-on-failure
```
OpenDM includes 5 test suites covering all core algorithms:
- `ChunkManagerTest`: Dynamic chunk splitting, 204,803-byte threshold, and speed limiter pacing.
- `ZipPreviewerTest`: HTTP Range backward scanning, Central Directory parsing, and selective deflate inflation.
- `MediaMuxerTest`: ISOBMFF container muxing, `stco`/`co64` offset rewriting, and TS segment concatenation.
- `IntegrationTest`: Site Logon credential vault, IDM `.ef2` import/export, and CLI options.
- `DashParserTest`: MPEG-DASH manifest parsing, stream selection, and post-download notifications.

---

## Installation

Run the desktop installer script to install binaries, application menu entries, and icons:
```bash
./install_desktop.sh
```
This installs:
- Binary: `~/.local/bin/opendm`
- Native messaging host: `~/.local/bin/opendm-native-host`
- Desktop entry: `~/.local/share/applications/opendm.desktop`
- Scalable icon: `~/.local/share/icons/hicolor/scalable/apps/opendm.svg`
- Browser native messaging manifests for Chrome, Chromium, Brave, Edge, and Firefox.

---

## Usage

### 1. Desktop Interface
Launch `opendm` from any terminal or open **OpenDM** from your desktop Application Menu.

### 2. Command Line Interface (IDM-Compatible Switches)
OpenDM supports the complete standard IDM command-line argument suite:

```bash
# Direct download with 16 concurrent connections
opendm -d https://speed.hetzner.de/100MB.bin -p ~/Downloads -c 16

# Add URL to queue without starting immediately
opendm -a -d https://speed.hetzner.de/100MB.bin -p ~/Downloads

# Apply bandwidth limit (e.g. 500 KB/s)
opendm -d https://speed.hetzner.de/100MB.bin -l 500

# Import IDM batch queue file (.ef2)
opendm -i /path/to/downloads.ef2 -s

# Silent mode (headless CLI execution)
opendm -n -d https://example.com/archive.zip -p /tmp
```

### 3. Browser Extension Setup
1. Run `./install_desktop.sh` (or `./install_native_host.sh`).
2. Open `chrome://extensions/` in Chrome, Chromium, or Brave.
3. Enable **Developer mode** (top-right toggle).
4. Click **Load unpacked** and select the [`extension_extracted/`](file:///home/nader/opendm/extension_extracted) directory.
5. All downloads and video playback in the browser will now be captured and accelerated by OpenDM.

---

## Reverse Engineering Documentation

Detailed reverse-engineering notes on IDM v6.43.15 algorithms, decompilation analysis, and binary audits are available in:
[`docs/idman_reverse_engineering_analysis.md`](file:///home/nader/opendm/docs/idman_reverse_engineering_analysis.md).

---

## License

GPL-3.0 / MIT (Clean-room implementation). All algorithms were reverse-engineered and independently authored for Linux.

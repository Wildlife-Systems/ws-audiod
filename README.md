# ws-audiod

A multi-consumer audio capture daemon for Raspberry Pi sensor networks.

In distributed acoustic monitoring systems, the principal challenge is not
sample acquisition but rather the concurrent distribution of captured audio to
multiple consumers. Species classifiers, acoustic event detectors, sound level
meters, spectrogram generators, and archival systems each require access to the
same audio data, potentially at different times and in different formats.

ws-audiod provides continuous capture from ALSA audio devices with concurrent
multi-consumer access through a single device interface. Audio clips may be
extracted from any point in the rolling buffer, including samples that have
already elapsed.

## Motivation

On Linux-based embedded systems, an ALSA PCM device can typically only be opened
by a single process at any given time. The `dsnoop` plugin provides limited
software mixing, but introduces latency, restricts format negotiation, and is
unsuitable for low-latency or high-sample-rate acoustic monitoring. PulseAudio
and PipeWire add unnecessary complexity to headless embedded deployments.

ws-audiod operates as infrastructure rather than an end-user application. The
daemon maintains exclusive ALSA device ownership; consumers connect via shared
memory (zero-copy sample access) or Unix domain socket (control interface).
Consumer process failures are isolated and do not
affect the daemon or other connected consumers.

ws-audiod is intended for custom deployments that require reliable, shared audio
infrastructure without reimplementing low-level ALSA capture and distribution logic.

## Features

- Continuous capture with zero sample loss
- Concurrent multi-consumer access (e.g. with provided C++ and Python client libraries)
- On-demand audio clip extraction from rolling buffer
- Pre-event and post-event clip extraction
- Zero-copy sample distribution via POSIX shared memory
- Gapless block recording (consecutive X-minute files with zero inter-file sample gap)
- Configurable sample rate, bit depth, and channel count
- WAV and FLAC output formats
- Gain control and DC offset removal
- Multi-device support (multiple daemon instances)

## Architecture

```
  ┌──────────────┐
  │  ALSA Device  │
  │  (hw:X,Y)     │
  └──────┬───────┘
         │ PCM samples
         ▼
  ┌──────────────┐
  │ AlsaCapture   │  Opens device, reads interleaved PCM
  └──────┬───────┘
         │
         ▼
  ┌──────────────┐     ┌──────────────┐
  │  RingBuffer   │────▶│ ClipExtractor │──▶ WAV/FLAC files
  │  (PCM)        │     └──────────────┘
  └──────┬───────┘
         │
    ┌────┴────┐
    ▼         ▼
┌────────┐ ┌──────────┐
│  SHM   │ │ TCP/HTTP  │
│Publisher│ │ Streamer  │
└────────┘ └──────────┘
    │           │
    ▼           ▼
 Consumer    Remote
 processes   clients
```

## Building from Source

### Prerequisites

The following packages are required on Raspberry Pi OS:

```bash
sudo apt update
sudo apt install -y cmake build-essential libasound2-dev libsndfile1-dev pkg-config
```

### Compilation

```bash
git clone https://github.com/Wildlife-Systems/ws-audiod.git
cd ws-audiod
mkdir build && cd build
cmake ..
make -j4
```

### Installation

```bash
sudo make install
sudo mkdir -p /var/ws/audiod/clips
sudo cp config/audio-daemon.service /etc/systemd/system/
sudo systemctl daemon-reload
```

## Usage

```bash
# Run directly
./ws-audiod

# With options
./ws-audiod -D hw:1,0 -r 48000 -c 1 -b 16 -d

# As a service
sudo systemctl start ws-audiod
```

### Options

| Flag | Description |
|------|-------------|
| `-c, --config FILE` | Configuration file path |
| `-s, --socket PATH` | Control socket path |
| `-D, --device DEVICE` | ALSA device (default: `default`) |
| `-r, --rate RATE` | Sample rate in Hz (default: 48000) |
| `-C, --channels N` | Number of channels (default: 1) |
| `-b, --bits N` | Bit depth: 16 or 24 (default: 16) |
| `-g, --gain DB` | Input gain in dB (default: 0.0) |
| `-p, --period-size N` | ALSA period size in frames (default: 1024) |
| `-B, --block-record SECS` | Enable gapless block recording with SECS-second files |
| `-d, --debug` | Enable debug logging |

### Commands

The daemon accepts commands via the Unix domain control socket:

```bash
python3 examples/audio_client.py clip -5 5
```

| Command | Description |
|---------|-------------|
| `CLIP <start> <end> [format]` | Extract an audio clip (offsets in seconds; format: `wav` or `flac`) |
| `SET <key> <value>` | Modify a capture parameter at runtime |
| `GET STATUS` | Retrieve current daemon status |
| `GET LEVEL` | Get current audio levels (peak, RMS) |

Clip offset examples:

- `CLIP -5 5` — 10-second clip spanning 5 seconds before and 5 seconds after now
- `CLIP -10 0` — 10-second clip drawn entirely from the buffer
- `CLIP -30 0 flac` — 30-second FLAC clip from the buffer

All responses are returned in JSON format:

```json
{"ok":true,"path":"/var/ws/audiod/clips/clip_20260305_143021.wav"}
{"ok":true,"data":{"running":true,"capture":{"samples":1440000,"rate":48000,"channels":1}}}
{"ok":false,"error":"Invalid command"}
```

### Client Examples

Python client library:

```python
from ws_audiod import AudioClient

with AudioClient() as client:
    response = client.capture_clip(-5, 5)
    print(response.path)

    status = client.get_status()
    print(status.data)
```

Shared memory consumer (zero-copy):

```bash
./audio_consumer
python3 examples/audio_client.py stream
```

## Configuration

`/etc/ws/audiod/ws-audiod.conf`:

```ini
[daemon]
socket_path = /run/ws-audiod/control.sock
clips_dir = /var/ws/audiod/clips
ring_buffer_seconds = 60

[audio]
device = default
rate = 48000
channels = 1
format = S16_LE
period_size = 1024
buffer_periods = 4
# gain_db = 0.0
# dc_remove = false

[block_recorder]
# enabled = false
# output_dir = /var/ws/audiod/blocks
# block_duration_seconds = 300
# format = wav
# max_blocks = 0
```

## Block Recording

The block recorder writes consecutive audio files of a fixed duration with
**zero inter-file sample gap**. When a block reaches its configured length, the
daemon closes the file and immediately opens the next one — no samples are
lost or duplicated at the boundary.

```bash
# Enable via command line (5-minute blocks)
./ws-audiod -B 300

# Or via config file
```

```ini
[block_recorder]
enabled = true
output_dir = /var/ws/audiod/blocks
block_duration_seconds = 300
format = wav
```

Output files are named with their UTC start time:

```
block_20260305_143000.wav   (started at 14:30:00 UTC)
block_20260305_143500.wav   (started at 14:35:00 UTC)
block_20260305_144000.wav   (started at 14:40:00 UTC)
...
```

### How gapless recording works

The ALSA capture thread delivers PCM periods to the block recorder inline.
Each period is written directly to the current output file. When the frame
count reaches `block_duration_seconds * sample_rate`, the recorder closes the
file handle and opens a new one. The transition is a file handle swap — the
next `sf_writef_short()` call goes to the new file with no intermediate
buffering. This guarantees that every captured sample appears in exactly one
output file with no gaps and no overlaps.

### Use cases

- **Archival recording**: Continuous unattended recording for later offline
  analysis (e.g. 24-hour soundscape monitoring producing 288 × 5-minute WAV
  files per day).
- **Species inventories**: Recording all audio in a habitat for later
  identification by classifiers.
- **Compliance monitoring**: Meeting regulatory requirements for continuous
  acoustic monitoring at construction or industrial sites.

## Multi-Device Deployment

Multiple daemon instances may be run concurrently with distinct configurations.
Each audio device requires a unique control socket path and shared memory
identifier.

```ini
# /etc/ws/audiod/garden_mic.conf
[daemon]
socket_path = /run/ws-audiod/garden_mic.sock
clips_dir = /var/ws/audiod/garden_mic/clips
shm_name = /ws_audiod_samples_garden_mic

[audio]
device = hw:1,0
rate = 48000
channels = 1
```

A systemd template unit simplifies management of multiple instances:

```bash
sudo cat > /etc/systemd/system/ws-audiod@.service << 'EOF'
[Unit]
Description=ws-audiod (%i)
After=sound.target

[Service]
Type=simple
ExecStart=/usr/bin/ws-audiod -c /etc/ws/audiod/%i.conf
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now ws-audiod@garden_mic
sudo systemctl enable --now ws-audiod@pond_hydrophone
```

To enumerate available ALSA capture devices: `arecord -l`

## License

MIT

# Client microphone uplink (UDP/RTP)

## Goals

- Real media-plane uplink (not control-stream `0x3003`).
- Compatible wire with Foundation Sunshine clients (`base+12`, RTSP SETUP `mic`).
- Host sink: **Steam Streaming Microphone only** (no VB-Cable auto-install).
- Advertise **Capable / Ready / Port** in `/serverinfo` (not RS-FEC).
- **Opportunistic RS-FEC**: no capability flag; if a client sends parity shards, use them per-session.

## Ports

| Offset | Role |
|--------|------|
| +9 | video |
| +10 | control (ENet) |
| +11 | audio downlink |
| +12 | **mic uplink** (`MIC_STREAM_PORT`) |

## Discovery (`/serverinfo`, HTTPS)

| Field | Meaning |
|-------|---------|
| `MicrophoneCapable` | Feature enabled in config (Windows build with mic uplink). |
| `MicrophoneReady` | Steam Streaming Microphone render endpoint is present **now**. |
| `MicrophonePort` | `net::map_port(12)` (informational; RTSP SETUP is authoritative). |

**Not advertised:** RS-FEC. New clients may send parity without probing a flag.

Legacy Foundation clients ignore unknown nodes.

## RTSP

### DESCRIBE (when `stream_mic` and policy allows)

```
m=audio <mic_port> RTP/AVP 96
a=rtpmap:96 opus/48000/2
a=fmtp:96 minptime=10;useinbandfec=1
```

### SETUP

`SETUP .../mic/...` → `Transport: server_port=<MIC_STREAM_PORT>`, sets session `mic.enabled`.

### Encryption

- Support `SS_ENC_MIC` (0x08) when the client enables it (Foundation-compatible IV scheme).
- Do not require encryption for baseline plaintext Foundation clients.

## Wire

### Baseline (Foundation clients)

1. UDP to mic port.
2. RTP (PT 96/97) or 16-bit extended header type `0x5504`.
3. Sequence number: **little-endian** (Foundation client quirk).
4. Payload: Opus mono @ 48 kHz.

### Opportunistic RS-FEC (new clients)

- Data: same as baseline.
- FEC: RTP `packetType=127` + `AUDIO_FEC_HEADER` + parity (mirror downlink audio layout).
- Geometry: `RTPA_DATA_SHARDS=4`, `RTPA_FEC_SHARDS=2`.
- **Per-session**: first FEC shard seen enables RS assembly for that session only.
- Incomplete blocks fall back to PLC / skip; never break baseline sessions.

## Host path

```
UDP recv → classify data vs FEC
  → (optional SS_ENC_MIC decrypt)
  → (optional RS recover if session saw FEC)
  → Opus decode → float PCM
  → Steam Streaming Microphone (WASAPI render)
  → optional default capture switch to Microphone (Steam Streaming Microphone)
```

## Config (`sunshine.conf`)

| Key | Default | Meaning |
|-----|---------|---------|
| `stream_mic` | `true` | Master enable |
| `mic_require_steam` | `true` | If Ready=0, skip DESCRIBE mic / reject useful uplink |
| `mic_sink` | `Speakers (Steam Streaming Microphone)` | Render endpoint patterns |
| `mic_capture_device` | `Microphone (Steam Streaming Microphone)` | Default capture switch (empty = don't switch) |
| `mic_buffer_ms` | `50` | WASAPI render buffer hint |
| `mic_buffer_packets` | `2` | Jitter prebuffer depth |

## Non-goals

- No VB-Cable download/install.
- No control-stream mic as the primary path.
- No RS-FEC capability advertisement.
- No claiming Opus in-band FEC is transport RS-FEC.

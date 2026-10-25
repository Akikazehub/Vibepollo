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
| `MicrophoneProtocol` / `MicrophoneProtocolVersions` | `moonlight-mic` / `1`. |

**Not advertised:** RS-FEC. New clients may send parity without probing a flag.

Legacy Foundation clients ignore unknown nodes.

## RTSP

### DESCRIBE (when `stream_mic` and policy allows)

```
m=audio <mic_port> RTP/AVP 97 127
a=rtpmap:97 opus/48000/2
a=fmtp:97 minptime=20;useinbandfec=1;stereo=0;sprop-stereo=0
a=rtpmap:127 moonlight-rs-fec/48000
a=x-ss-mic-protocol:moonlight-mic
a=x-ss-mic-versions:1
```

### SETUP

`SETUP .../mic/...` → `Transport: server_port=<MIC_STREAM_PORT>`, sets session `enable_mic`.
An `X-SS-Mic-Protocol: moonlight-mic/1` request header locks the versioned wire protocol
(unsupported values are rejected with 461); without it the Foundation legacy wire format
is used. The negotiated protocol is echoed back as `X-SS-Mic-Protocol`.

### Encryption

- Support `SS_ENC_MIC` (0x08) when the client enables it (Foundation-compatible IV scheme).
- Do not require encryption for baseline plaintext Foundation clients.

## Wire

### Baseline (Foundation clients)

1. UDP to mic port.
2. RTP (PT 96/97) or 16-bit extended header type `0x5504`.
3. Sequence number: **little-endian** (Foundation client quirk).
4. Payload: Opus mono @ 48 kHz.

### Opportunistic RS-FEC (moonlight-mic/1 clients)

- Data: RTP PT 97 (big-endian header, SSRC-tracked).
- FEC: RTP `packetType=127` + `AUDIO_FEC_HEADER` + 12-byte mic extension + parity (mirrors downlink audio layout).
- Geometry: `RTPA_DATA_SHARDS=4`, `RTPA_FEC_SHARDS=2`, fixed OpenFEC/NVIDIA parity matrix.
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

The mic UDP socket is bound lazily when the first mic session starts, and the
WASAPI backend is kept warm across sessions until the last mic session stops.

## Config (`sunshine.conf`)

| Key | Default | Meaning |
|-----|---------|---------|
| `stream_mic` | `true` | Master enable |
| `mic_require_steam` | `true` | If Ready=0, skip DESCRIBE mic advertisement (SETUP stays accepted) |
| `mic_sink` | `Speakers (Steam Streaming Microphone)` | Render endpoint patterns |
| `mic_capture_device` | `Microphone (Steam Streaming Microphone)` | Default capture switch (empty = don't switch) |
| `mic_buffer_ms` | `50` | WASAPI render buffer hint |
| `mic_buffer_packets` | `2` | Jitter prebuffer depth |

## Non-goals

- No VB-Cable download/install.
- No control-stream mic as the primary path.
- No RS-FEC capability advertisement.
- No claiming Opus in-band FEC is transport RS-FEC.

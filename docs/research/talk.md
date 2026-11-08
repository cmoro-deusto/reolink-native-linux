# Two-way talk over Baichuan — verified facts

What we have **confirmed on our own hardware**, with the evidence. Each fact is
verified here before code relies on it.

Test setup: a Reolink Home Hub Pro (firmware v3.3.0.605) with two Argus 4
Pro (battery) cameras and one E1 Pro (mains-powered indoor pan/tilt). All
three advertise `talk` = 1 in `GetAbility` (`abilityChn[n].talk.ver`).

## Verified

### Talk ability — msg 10 (2026-11-08)

Request: modern header, class `0x6414`, `ch_id = channel + 1` (as for every
per-channel request we send to the hub; `BaichuanControl::transact`), an
AES-encrypted Extension `<Extension version="1.1"><channelId>N</channelId></Extension>`,
no payload.

Reply: status 200 for all three channels — including the two sleeping
battery cameras, so the hub answers this itself. Identical body for all three:

```xml
<TalkAbility version="1.1">
  <duplexList><duplex>FDX</duplex></duplexList>
  <audioStreamModeList><audioStreamMode>followVideoStream</audioStreamMode></audioStreamModeList>
  <audioConfigList>
    <audioConfig>
      <priority>0</priority>
      <audioType>adpcm</audioType>
      <sampleRate>16000</sampleRate>
      <samplePrecision>16</samplePrecision>
      <lengthPerEncoder>1024</lengthPerEncoder>
      <soundTrack>mono</soundTrack>
    </audioConfig>
  </audioConfigList>
</TalkAbility>
```

So talk audio is ADPCM, 16 kHz, 16-bit, mono, full duplex.

### Downlink audio for comparison (2026-11-08)

The same cameras send their microphone audio to us as **AAC** (ADTS, 16 kHz
mono, `05wb` media frames), not ADPCM, inside the preview stream (msg 3). The
hub encrypts that media with AES ("full AES": the Extension carries
`<encryptLen>`). The talk format is therefore not simply "what the camera
sends".

### Talk session — msgs 201, 202, 11 (2026-11-08)

Tested with our own spike against the E1 Pro (camera firmware v3.0.0.716):

- **msg 201 `TalkConfig`** echoing the advertised config (`channelId`,
  `duplex`, `audioStreamMode`, `audioConfig` without `priority`), XML payload
  and channel Extension both AES-encrypted: **200** (speaker reserved).
  `lengthPerEncoder` other than the advertised 1024 (512, 2048, 4096) is
  refused with **400**.
- **msg 202** audio messages: the payload is accepted **unencrypted** (no
  `<encryptLen>`), and the hub **answers every 202 with a header-only 200**.
- **msg 11** with the channel Extension: **200** (speaker released).

### Talk audio format (2026-11-08)

Observed in my LAN's traffic: a talk session that played cleanly, captured
with a logging TCP proxy and decrypted with our own key derivation; then
confirmed by decoding its blocks back to the source tone at 31.5 dB (35 dB
per block).

Each msg 202 carries one `01wb` media frame of 528 bytes:

| Offset | Bytes | Value |
|---|---|---|
| 0 | 4 | `30 31 77 62` ("01wb") |
| 4 | 2 | u16 LE 520 (payload size: sub-header + block) |
| 6 | 2 | u16 LE 520 (repeated) |
| 8 | 4 | sub-header `00 01 02 00` |
| 12 | 2 | i16 LE predictor = the **last sample before** this block |
| 14 | 1 | u8 IMA step index for that predictor |
| 15 | 1 | 0 |
| 16 | 512 | 1024 IMA ADPCM nibbles, **high nibble first** in each byte |

- Blocks advance by exactly **1024 samples** (`lengthPerEncoder`); the header
  is the decoder state carried into the block, not an extra output sample.
- One block per message, sent at **real time** (one every ~64 ms, no lead).
- The msg 202 Extension is `<binaryData>1</binaryData>` then `<channelId>`,
  AES-encrypted; the binary payload is sent in the clear.
- The session used a dedicated connection for talk (login → 10 → 201 →
  202… → logout, msg 2) and did not send msg 11 before logging out.

Sending the same tone with the low nibble first and a 1025-sample
(header-as-first-sample) layout played recognisably but rough, with a pop at
every 1024-sample block boundary (measured from a room recording).

Our own standard IMA encoder producing exactly this layout (state header,
1024 samples, high nibble first, sub-header `00 01 02 00`, one block per
message at real time) plays cleanly on the E1 Pro, with no pops: confirmed by
ear on 2026-11-08, with a test tone and then 20 s of recorded speech (clear,
no glitches, kept pace to the end; 345 messages, all acknowledged).

The app's talk feature (microphone → this layout over its own connection)
works on both the E1 Pro and an Argus 4 Pro (battery) behind the Home Hub
Pro: confirmed by ear on 2026-11-08. The battery camera needed no extra
wake-up step; the hub handles it.

# DRC media packet datasheet

This document describes the packets Barista sends to the Wii U GamePad during a
media session. It is intended to make packet captures readable and to separate
three different failure classes:

1. an invalid UDP media envelope;
2. a valid envelope containing incompatible or low-quality H.264 bytes; and
3. valid host packets which are late, lost, or rejected by the GamePad.

It records the protocol as implemented by Barista and compared with the
authenticated real-console reference capture. Fields labelled **observed** are
reproducible capture facts but do not yet have a complete semantic explanation.

## Packet stack

```text
802.11 QoS data frame
    CCMP (WPA2 encryption/authentication on the air)
        LLC/SNAP
            IPv4
                UDP
                    50120: VSTRM video
                    50121: ASTRM PCM audio or video format
```

Runtime addresses are normally `192.168.1.10` (host/AP) and `192.168.1.11`
(GamePad). The UDP payload is encrypted in an over-air radiotap capture. The
companion `*-ip.pcap` produced by `scripts/capture-drcd-session.sh` captures it
after decryption on the host and is the best input for envelope inspection.

MAC acknowledgement or BlockAck only establishes link-layer receipt. It does
not prove that the GamePad accepted the IP packet, assembled a frame, or
displayed it.

## VSTRM video (`UDP dst port 50120`)

Every video datagram has a fixed 16-byte header followed by up to 1694 payload
bytes. The payload is a contiguous part of one logical H.264 slice.

```text
byte:   0         1       2             3        4..7       8..15       16..
        +---------+-------+-------------+--------+----------+-----------+------+
value:  F+seq[9:8] seq[7:0] flags+len[10:8] len[7:0] timestamp extensions payload
        +---------+-------+-------------+--------+----------+-----------+------+
```

| Byte(s) | Encoding | Meaning | Status |
| --- | --- | --- | --- |
| 0, 1 | 10-bit big-endian sequence | Sequence number, modulo 1024. | Confirmed |
| 0 high nibble | `0xf` | VSTRM signature. | Confirmed |
| 2 bit 7 | `0x80` | `init`. Barista sets it on the initial frame; an experiment can also set it on recovery IDRs. | Confirmed wire position; receiver semantics partly inferred |
| 2 bit 6 | `0x40` | First packet of a video frame. | Confirmed |
| 2 bit 5 | `0x20` | Final packet of one six-row chunk. | Confirmed |
| 2 bit 4 | `0x10` | Final packet of the whole frame. | Confirmed |
| 2 bit 3 | `0x08` | Timestamp-present marker. Barista always sets it. | Confirmed wire position |
| 2 low bits, 3 | 11-bit big-endian integer | H.264 payload byte count. Must equal UDP payload length minus 16. | Confirmed |
| 4..7 | big-endian `uint32_t` | Media timestamp in microseconds, wrapping at 32 bits. All datagrams of one frame use the same value. | Confirmed |
| 8..15 | byte options | Decoder/video geometry options described below. | Partly confirmed |

### Extension/options area

The area is terminated by zero padding. The real console and current Barista
use the following values, although the real console orders them differently
from early Barista revisions.

| Bytes | Meaning | Evidence |
| --- | --- | --- |
| `80` | IDR frame marker. Present on an IDR frame's packets. | Capture and GamePad firmware analysis |
| `82 00` | 59.94 Hz frame rate selection. | Existing Wii U dissector and firmware timing table |
| `83` | Force decoding. | Existing Wii U dissector; exact hardware effect remains unknown |
| `85 06` | Six macroblock rows in the logical chunk. | Capture and five-chunk geometry |
| `00` | End/padding. | Existing Wii U dissector |

For an IDR, the reference order is normally:

```text
80 82 00 83 85 06 00 00
```

For a P frame it is normally:

```text
82 00 83 85 06 00 00 00
```

An annotated first packet from an IDR with sequence `0x321`, timestamp
`0x12345678`, and a 1694-byte one-packet first chunk begins:

```text
f3 21  ee 9e  12 34 56 78  80 82 00 83 85 06 00 00  [1694 payload bytes]
|  |   |  |   |-----------|  |---------------------|
|  |   |  |       TSF-like timestamp     options
|  |   |  +-- 0x6 9e = 1694-byte payload length
|  |   +----- init + frame-begin + chunk-end + timestamp-present
|  +--------- low 8 bits of sequence
+------------ VSTRM signature + high 2 bits of sequence
```

The `chunk-end` flag ends one logical chunk, not necessarily one UDP datagram.
A large chunk produces multiple packets: only its last packet has `chunk-end`.
The first packet of a frame alone has `frame-begin`; the last packet of chunk 5
alone has `frame-end`.

## One complete video frame

The GamePad expects one continuous H.264 picture split into five logical
six-macroblock-row chunks:

```text
864 x 480 pixels = 54 x 30 macroblocks

chunk 0: macroblock rows  0.. 5     packet(s), then chunk-end
chunk 1: macroblock rows  6..11     packet(s), then chunk-end
chunk 2: macroblock rows 12..17     packet(s), then chunk-end
chunk 3: macroblock rows 18..23     packet(s), then chunk-end
chunk 4: macroblock rows 24..29     packet(s), then chunk-end and frame-end
```

The transmitted bytes are **not** a normal self-contained H.264 file:

- the UDP payload does not include Annex-B start codes;
- SPS, PPS, and the slice header are not sent in VSTRM; and
- emulation-prevention bytes are inserted only when reconstructing an ordinary
  H.264 stream for desktop tools.

The GamePad has a fixed decoder configuration for the 864x480 surface. The
offline reconstruction tool prepends the known configuration and synthetic
slice headers; a successful FFmpeg decode proves that this reconstructed stream
is syntactically valid, but does not prove GamePad compatibility.

## ASTRM PCM audio (`UDP dst port 50121`)

PCM packets use an 8-byte header followed by 1664 bytes: 416 stereo `s16le`
frames at 48 kHz. Their nominal period is 8.666666 ms.

```text
byte:   0       1       2..3             4..7             8..
        +-------+-------+----------------+----------------+----------------+
value:  flags+sequence   PCM length       timestamp        interleaved PCM
        +-------+-------+----------------+----------------+----------------+
```

| Byte(s) | Meaning |
| --- | --- |
| 0..1 | Big-endian field: format `0x20` for 48 kHz stereo PCM, vibration bit `0x08`, message/type bit `0x04`, then a 10-bit modulo-1024 sequence. |
| 2..3 | Big-endian payload length. For PCM this is `0x0680` = 1664. |
| 4..7 | Little-endian timestamp. |
| 8.. | 416 frames of interleaved signed 16-bit little-endian stereo PCM. |

For sequence `0x321` and timestamp `0x12345678`, a non-vibrating packet starts
with `23 21 06 80 78 56 34 12`.

## Video-format message (`UDP dst port 50121`)

The same UDP port carries a 32-byte format message. It has an 8-byte ASTRM-like
outer header and a 24-byte body. It is distinguishable because byte 0 has bit
`0x04` set.

```text
04 00 00 00  11 11 81 08  tt tt tt tt  80 3e 00 00  80 3e 00 00
08 52 00 00  08 52 00 00  00 00 00 00
|-----------|  |----------|  |----------|  |----------|  |----------|
outer header    timestamp LE     16000 LE       16000 LE       21000 LE ...
```

| Offset | Value/encoding | Meaning |
| --- | --- | --- |
| 0 | `0x04` | Format-message type. |
| 2..3 | `0x0000` | Declared outer payload length. The real console deliberately leaves it zero despite the following 24-byte body. Do not flag this alone as malformed. |
| 4..7 | Capture-dependent bytes | **Observed** outer bytes. The historical authenticated reference used `11 11 81 08`; local Vanilla controls use `11 11 81 88`; the fresh authenticated capture uses `10 10 81 08`. Their meaning is not established. |
| 8..11 | little-endian `uint32_t` | Format timestamp. The matching video frame normally uses the same timestamp in big-endian form. |
| 12..15, 16..19 | little-endian `16000` | Reference clock fields. The firmware selects one as the video start delay. |
| 20..23, 24..27 | little-endian `21000` | Reference clock fields; semantics are not completely established. |
| 28..31 | zero | Observed padding/final field. |

Barista publishes formats at 59.94 Hz. It stamps the format at AP TSF minus
1250 microseconds and sends the matching video about 5000 microseconds later,
so the first video packet nominally carries a timestamp 6250 microseconds old.
The real capture's median format-to-first-video lead was about 5.1 ms.

After an IDR, the console pattern is:

```text
format + IDR video, then one format-only slot, then format + P video
```

This avoids sending a P frame immediately after an IDR while the receiver is
recovering. It is a scheduling rule, not an additional packet type.

## Transport versus media timing

The usual nominal video packet windows are measured from video send start:

| Chunk | Start | Final packet deadline |
| --- | ---: | ---: |
| 0 | 0 ms | 2.5 ms |
| 1 | 3 ms | 5 ms |
| 2 | 6 ms | 7.5 ms |
| 3 | 9 ms | 10 ms |
| 4 | 11 ms | 13 ms |

Large chunks spread their UDP packets evenly through that window. The schedule
has two purposes: it gets the first two completed chunks to the receiver early
enough to begin decoder feed, and it avoids a single large-frame burst. A host
timestamp in a pcap records when the host captured/submitted a packet; it is
not a direct measurement of radio airtime or GamePad receipt.

## Capture checks

Run these against a companion decrypted host capture:

```sh
python3 scripts/analyze-media-capture.py local-captures/live-home-native-wlan1-1-ip.pcap
python3 scripts/reconstruct-media-capture.py local-captures/live-home-native-wlan1-1-ip.pcap > /tmp/barista.h264
ffmpeg -v error -xerror -apply_cropping 0 -c:v h264 -f h264 -i /tmp/barista.h264 -f null -
```

`analyze-media-capture.py` verifies packet length, 10-bit sequence continuity,
frame timestamps, five chunk ends, frame end, and host cadence. It cannot show
whether the GamePad received or decoded the packets.

For a radio capture, inspect PHY classification separately:

```sh
python3 scripts/summarize-wifi-rates.py ../mac-real-165 \
  --ap 40:d2:8a:bf:fc:a8 --pad b2:82:b9:cd:5c:fd
```

The currently available `mac-real-165` capture is encrypted and belongs to
GamePad MAC `b2:82:b9:cd:5c:fd`. It is an already-active media interval: the
current parser sees zero EAPOL key packets and no target association. It cannot
derive a session key from this capture, even with a matching EEPROM dump. The
earlier decrypted reference instead used GamePad MAC `40:d2:8a:ab:90:00` and
included authenticated handshakes. Do not treat the old decrypted findings as a
byte-for-byte decode of this newer capture. A new reference capture must begin
before association and retain the complete EAPOL exchange.

### Reanalysis of the available real radio captures

`mac-real-165` is useful for radio behavior despite its encrypted payload. It
contains 35.106540 seconds of traffic from AP `40:d2:8a:bf:fc:a8` to the
GamePad above:

| Observation | Count | Interpretation |
| --- | ---: | --- |
| AP-to-GamePad QoS data frames | 17,166 | 17,156 were protected; the remaining ten were unprotected observations. |
| AP TID 5 frames | 17,051 | 99.33% of observed AP data; this is the media traffic class. |
| AP HT MCS 6, long guard interval | 17,154 | 99.93% of observed AP data. |
| AP retry-bit observations | 1 | A capture observation, not a reliable packet-loss percentage. |
| AP-originated RTS directed to GamePad | 16,936 | Real media is almost always RTS/CTS protected. |
| GamePad CTS directed to AP | 16,889 | Corresponds closely to the RTS count. |
| GamePad BlockAck directed to AP | 16,894 | Corresponds closely to the AP media-frame count. |
| GamePad-to-AP QoS data frames | 7,115 | Mostly control/input traffic: 7,079 TID 0 and 36 TID 7. |
| GamePad retry-bit observations | 767 | 10.78% of those observations; this remains an observation rate, not loss. |

`mac-real-36` contains no frames for that AP/GamePad pair and has 1,093
bad-FCS observations. It is not a usable DRC media reference.

### Fresh authenticated reference capture

`/tmp/wiiu-real-fresh-165.pcap` was started before the console and GamePad. The
capture-script argument used an old GamePad MAC, so its final metadata reports
zero matches, but the unfiltered capture itself contains the active pair
`40:d2:8a:bf:fc:a8` and `40:d2:8a:ab:90:00`. It is therefore valid and was
authenticated with the existing slot-0 EEPROM dump:

| Check | Result |
| --- | --- |
| EAPOL key packets | 8 |
| Verified Nintendo-rotated PTKs | 2 |
| Authenticated CCMP observations | 43,251 |
| Extracted IPv4 packets | 43,247 |
| VSTRM video packets | 21,457 |
| ASTRM video-format packets | 3,282 |
| ASTRM PCM packets | 6,696 |
| Real VSTRM maximum raw payload | 1,694 bytes |

The fresh real packets use format outer bytes `04 00 00 00 10 10 81 08`, the
same VSTRM IDR/P option layouts as the Vanilla controls, and 1664-byte PCM
payloads. Their most common raw VSTRM payload is exactly 1694 bytes. The
successful Vanilla controls use the same limit, while the former Barista live
sender capped raw VSTRM payloads at 1400 bytes despite configuring an 1800-byte
session MTU. Barista now uses the observed 1694-byte boundary.

The aggregate analyzer reports video and PCM sequence discontinuities in this
whole capture. It includes two authenticated key exchanges and observed
retransmissions, so those aggregate discontinuities are not by themselves proof
of a real-console media failure. Analyze a selected intact chain when comparing
payloads frame-for-frame.

### Local Vanilla-control comparison

The six `local-captures/vanilla-control*-ip.pcap` files are decrypted host
captures of the same replay/control family. They are a useful control group
because they use the same AP and GamePad identities but vary the adapter, clock,
and recovery policy. Their shared wire properties are stronger evidence than a
single capture:

- after masking the four-byte format timestamp, every format message in all six
  captures is byte-for-byte `04 00 00 00 11 11 81 88` followed by the same
  `16000, 16000, 21000, 21000` little-endian clock fields and zero tail;
- IDR options are always `80 82 00 83 85 06 00 00`, while P options are always
  `82 00 83 85 06 00 00 00`;
- every complete frame has five chunk ends, a matching previously published
  format timestamp, and continuous outbound VSTRM sequence numbers; and
- all six reconstructed streams pass strict FFmpeg H.264 decoding. This checks
  the reconstructed syntax, not GamePad display acceptance.

| Capture | Complete frames | IDR / P frame starts | Median format-to-frame lead | Final logged state | Final logged resync count |
| --- | ---: | ---: | ---: | --- | ---: |
| `vanilla-control-20260922-1` | 786 | 1 / 786 | 5.072 ms | waiting | 700 |
| `vanilla-control-apclock` | 736 | 1 / 735 | 5.077 ms | waiting | 600 |
| `vanilla-control-oldengine-wlan0` | 737 | 1 / 737 | 5.033 ms | waiting | 700 |
| `vanilla-control-wlan0` | 946 | 1 / 946 | 5.029 ms | waiting | 700 |
| `vanilla-control-wlan1-recover` | 1,133 | 2 / 1,132 | 4.983 ms | active | 1,000 |
| `vanilla-control-wlan1-recover-v2` | 1,830 | 2 / 1,828 | 5.025 ms | active | 1 |

The ordinary frame packet span is consistent across the group: medians are
11.163–11.249 ms and maxima 14.028–15.368 ms. `recover-v2` does contain one
12-microsecond format/video lead outlier, but its median is normal; that isolated
sample does not explain its much better receiver outcome.

The paired radio captures also show that all six streams classify almost all
AP-to-GamePad data as QoS TID 5 (at least 99.3%) and contain no reported AP
retry bit. Their local radiotap metadata often omits the actual HT rate, and
does not reliably expose local transmitted RTS or final delivery, so it cannot
establish that they matched the real console's MCS-6 and RTS/BlockAck behavior.

The decisive contrast is therefore not the basic UDP envelope: `recover-v2`
became active with the same format body and VSTRM envelope layout that appear
in the waiting controls. This does not claim that all H.264 payload bytes were
identical. Its recovery policy restarted from the captured IDR at most once per
750 ms; the other captures either logged requests without changing the replay,
restarted only once, or waited for the next captured IDR. The generic resync
message does not identify the internal failure reason, so this is evidence that
recovery handling matters, not proof that every logged request was a distinct
rejected frame.

## What the captures establish so far

The historical authenticated real-console capture established the real outer
format layout, ASTRM packet size/cadence, VSTRM option ordering, and the
format-before-video relationship. Barista's current media constructors encode
those properties in `core/drh/encoder/media_streamer.cpp`.

Current host-side captures normally contain complete five-chunk frames with
valid lengths and continuous sequences. That eliminates many envelope bugs but
does not settle the visible-corruption question:

- Encoder quality can produce blocking/fade artifacts even in a clean local
  decode.
- The previous effective chroma-QP mismatch produced a verified offline chroma
  reconstruction error; Barista now enforces the reference effective value.
- GamePad recovery requests can result from more than one internal cause. They
  are not an unambiguous packet-loss or H.264-syntax signal.
- Corruption visible only on the GamePad while the reconstructed host video is
  clean remains evidence for delivery, timing, or receiver compatibility.

Use the exact reconstructed stream from the same run when assessing a visible
artifact. If it is present in the reconstruction, investigate source pixels and
encoder decisions. If it appears only on the GamePad, compare radio delivery,
format/video timing, and recovery episodes before changing H.264 payload code.

## Implementation pointers

| Concern | Source |
| --- | --- |
| VSTRM/ASTRM/format construction | `core/drh/encoder/media_streamer.cpp` |
| Five chunk geometry | `core/drh/encoder/encoder.h` |
| Packet timing windows | `core/drh/encoder/video_packet_schedule.h` |
| UDP sockets and ports | `core/drh/runtime_transport.cpp` |
| Host-pcap envelope analyzer | `scripts/analyze-media-capture.py` |
| H.264 reconstruction | `scripts/reconstruct-media-capture.py` |
| Receiver recovery behavior | `../wiiu-code/research/RECOVERY_PATH.md` |
| Authenticated real-capture findings | `../drc-eeprom-exporter/REFERENCE_FINDINGS.md` |

# Narrow adaptive-bit-rate (ABR) interface over Speex

A small wrapper around the Speex narrowband codec (8 kHz, 160-sample / 20 ms
frames) exposing exactly three bit-rate tiers that can be switched at run
time between any two frames.

## Why switches are click-free and lossless

- `abr_encoder_set_tier()` only issues `SPEEX_SET_BITRATE`, which changes the
  sub-mode selection. It never calls `SPEEX_RESET_STATE`, so the predictor,
  LSP and pitch memories are preserved across the switch.
- Every Speex frame starts with an in-band mode id, so each packet describes
  its own bit rate. The decoder follows tier changes automatically; no
  out-of-band signalling, re-initialisation, frame insertion or frame drop is
  involved.
- Packets are additionally prefixed with a one-byte payload length so they
  can be carried over an undelimited byte stream and re-framed on receive.

## Tiers

| Tier              | Nominal rate | Packet bytes (incl. length prefix) |
|-------------------|--------------|-------------------------------------|
| `ABR_TIER_LOW`    | 5950 bps     | 16                                  |
| `ABR_TIER_MEDIUM` | 11000 bps    | 29                                  |
| `ABR_TIER_HIGH`   | 24600 bps    | 63                                  |

Packet size is constant within a tier (CBR); only the tier switch changes
the size of subsequent packets.

## Interface

Header: `include/abr/abr_codec.h`, implementation: `abr_codec.c`.

```c
abr_encoder *enc = abr_encoder_open(ABR_TIER_LOW);
abr_decoder *dec = abr_decoder_open();

for (each 20 ms frame) {
    abr_encoder_set_tier(enc, ABR_TIER_HIGH);      /* any time, per frame */
    unsigned char pkt[ABR_MAX_PACKET];
    size_t len = sizeof(pkt);
    abr_encoder_encode(enc, pcm_frame, pkt, &len);
    /* ... transport ... */
    abr_decoder_decode(dec, pkt, len, decoded_frame);
}
abr_encoder_close(enc);
abr_decoder_close(dec);
```

Status codes: `ABR_OK`, `ABR_ERR_ARG`, `ABR_ERR_TIER`, `ABR_ERR_NOSPACE`,
`ABR_ERR_CORRUPT`. Every function validates its arguments and never aborts.

## Build and test

```sh
meson setup builddir && ninja -C builddir      # one-time, builds libspeex.a
cd abr && make && make test
```

The test (`test/abr_test.c`) runs a 40-frame 300 Hz sine through the
schedule LOW x8 -> MEDIUM x8 -> HIGH x8 -> MEDIUM x8 -> LOW x8 and checks:

- all three tiers encode/decode without error, with constant packet size per
  tier;
- boundary argument validation: NULL pointers, bad tier ids, undersized
  output buffers, truncated packets, length-prefix mismatch, garbage payload;
- no click at any of the four switch points (inter-sample step across the
  boundary never exceeds the largest step inside a constant-tier block);
- no global discontinuity that would indicate a dropped/duplicated frame;
- a freshly opened decoder, given no tier information out of band, follows
  all three tiers purely from the in-band mode ids.

## Waveform data

Written into `data/` by the test:

- `input.pcm` / `decoded.pcm`: raw signed 16-bit little-endian mono,
  8 kHz, 6400 samples (800 ms). Inspect with e.g.
  `ffplay -f s16le -ar 8000 -ac 1 data/decoded.pcm`.
- `waveform.csv`: columns `sample,tier,input_pcm,decoded_pcm,diff`. Tier
  changes fall at samples 1280, 2560, 3840 and 5120 (frame boundaries),
  making the continuity across each switch directly inspectable.

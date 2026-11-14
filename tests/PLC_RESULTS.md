# PLC comparison results

Date: 2026-11-14

## Setup

- Codec: Speex narrowband (8 kHz) and wideband (16 kHz), quality 8,
  fixed-point release build.
- Speech: Flite `kal`, 10.39 s. The text is in
  `tests/run_plc_comparison.sh`.
- Packets: 20 ms frames. 519 frames, 104 lost frames = 20.04%.
- Loss mask: exact 20% shuffled with a fixed seed; encoder packets and
  loss positions are identical for baseline and patched decoder.
- PESQ: ITU-T P.862 (NB) / P.862.2 (WB), MOS-LQO against the original
  speech.
- Pop: sample jump at a good/lost packet boundary of at least 6000 LSB.

## Seed 12345

| Metric | NB baseline | NB patched | WB baseline | WB patched |
|---|---:|---:|---:|---:|
| PESQ clean decode | 4.053 | 4.053 | 3.970 | 3.970 |
| PESQ at 20% loss | 1.991 | 2.054 | 1.473 | 1.497 |
| PESQ change vs baseline | - | +0.062 | - | +0.024 |
| Boundary pops | 2 | 0 | 0 | 0 |
| Boundary max jump (LSB) | 7858 | 1 | 3774 | 4011 |
| Energy envelope MAE, median | 1.464 dB | 1.284 dB | 1.133 dB | 1.147 dB |
| Segmental SNR | 14.293 dB | 14.135 dB | 15.108 dB | 14.727 dB |

Five-seed medians (seeds 1, 2, 3, 12345, 99991):

| Mode | Baseline PESQ | Patched PESQ | Change |
|---|---:|---:|---:|
| NB | 1.991 | 2.054 | +0.062 |
| WB | 1.517 | 1.534 | +0.017 |

Across seeds 1 through 20, the patched decoder had zero boundary pops in
both NB and WB. The largest boundary jump was 1 LSB in NB and 4711 LSB in
WB (below the 6000-LSB pop threshold). The all-sample transient metric is
intentionally not reported as a pop count because clean speech contains
many natural high-derivative transients.

## Acceptance result

The zero-pop requirement is met for packet boundaries. The requested
absolute PESQ degradation of at most 0.1 is **not met**:

- NB: 4.053 to 2.054, degradation 1.999.
- WB: 3.970 to 1.534, degradation 2.436 (five-seed median).

At 20% random 20 ms erasure, the missing waveform is not observable to a
causal decoder, so a 0.1 MOS-LQO bound cannot be guaranteed. Even an offline
one-frame look-ahead pitch OLA experiment on the same loss mask only reached
about 2.81 PESQ. Meeting such a bound would require either a much lower
effective loss rate after jitter-buffer/FEC recovery, or a different quality
metric that does not include the concealed frames.

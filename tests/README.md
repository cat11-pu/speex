# PLC regression test

`plc_eval` encodes 16-bit mono PCM, stores every Speex packet, then decodes
the same stream twice with the same deterministic loss mask:

1. `clean.raw`: no packet loss.
2. `lossy.raw`: exact frame-loss percentage (20% by default).

A sidecar `lossy.raw.loss` records one byte per 20 ms frame (1 = lost).

Build the test tool:

```sh
meson setup build -Dfixed-point=enabled -Dtest-binaries=enabled
meson compile -C build
```

Generate a reproducible 8 kHz narrowband speech sample and run 20% loss:

```sh
ffmpeg -f lavfi -i "flite=text='The quick brown fox jumps over the lazy dog. Speech quality is measured across a noisy packet network.':voice=kal" \
  -ar 8000 -ac 1 -f s16le -y speech.raw
build/tests/plc_eval -m nb -q 8 -l 20 -s 12345 \
  speech.raw clean.raw lossy.raw
python3 tests/plc_metrics.py speech.raw clean.raw lossy.raw lossy.raw.loss
```

Use `-p periodic` for evenly spaced losses (every fifth 20 ms frame) or
`-p uniform` (default) for a shuffled exact-loss mask.

For wideband, use `-m wb`, 16 kHz input, and pass `--fs 16000
--frame-size 320` to `plc_metrics.py`.

To compare the current tree with a baseline revision using the same input,
seed, and loss mask:

```sh
PESQ_CLI=/path/to/pesq MODE=nb BASE_REF=<baseline-commit> \
  tests/run_plc_comparison.sh speech.wav
```

`BASE_REF` defaults to `HEAD`; set it explicitly after committing the PLC
change so the comparison still points at the pre-change revision.

## Metrics

- PESQ: ITU-T P.862/P.862.2 MOS-LQO. Install the optional Python `pesq`
  package or provide a P.862 command line tool. The drop is
  `PESQ(clean) - PESQ(lossy)`.
- Segmental SNR: mean active-frame SNR against the no-loss decode.
- Energy envelope error: median and p95 absolute frame-RMS error in dB.
- Boundary pop: a packet-boundary sample jump at or above 6000 LSB
  (about -14.8 dBFS for 16-bit PCM).
- Click count: isolated derivative spikes above the threshold and above
  8 times the local derivative MAD.

The harness uses one encoder output for both decodes, so encoder variation
and loss positions are identical across implementations.

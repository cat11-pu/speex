# Speex fixed-point / floating-point differential tests

This directory contains a black-box differential test for the fixed-point and
floating-point Speex builds.

## Definition of pass

- A legal corpus stream is encoded once, then the **same packet bytes** are
  decoded by both builds through `speex_decode_int()`.
- The per-sample gate is `abs(fixed_sample - float_sample) <= 1`, i.e. one
  16-bit PCM least-significant bit ("one quantization level").
- Every sample of a pure digital-silence stream must be exactly zero in both
  builds.
- At encoder parameter-switch boundaries, a click is an isolated second
  difference greater than `max(256, 8 * local_median + 64)`.
- For malformed packets, the fixed and floating builds must return the same
  status/overflow result, or one build must return a stricter negative status.
  The report also tracks guaranteed-invalid structural mutations separately.

## Corpus

The deterministic legal corpus contains 3000 streams.  Half are encoded by the
fixed build and half by the floating build.  It covers:

- narrowband, wideband and ultra-wideband modes;
- quality and complexity 0 through 10;
- fixed and variable bit-rate modes;
- silence, sine, noise, sweep and mixed signals;
- parameter changes at intervals of 1, 2, 3, 4 or 6 frames;
- quality, VBR, complexity, high-pass and combined switches;
- decoder enhancement on and off.

The malformed corpus contains 1000 one-packet streams.  At least 90% are
structurally invalid by construction (invalid narrowband submode, invalid
wideband skip id, or an empty/truncated packet); the remainder are random
truncations, bit flips, byte deletions, byte insertions and first-byte
corruption.

## Implementation

`speex_diff_tool.c` is compiled twice, once against each library.  It writes a
small binary corpus/result format so that Python never loads both libraries in
one process:

- `gen`: deterministic signal generation and encoding;
- `decode`: frame-by-frame decoding with return code and `SpeexBits.overflow`
  capture.

`run_differential.py` builds the two Meson configurations, compiles the two
tool binaries, generates the corpora, compares PCM, detects silence/click
violations, analyzes malformed-packet verdicts, and minimizes failing legal
cases by monotone batched coordinate descent.

## Run

From the repository root:

```sh
python3 tests/differential/run_differential.py \
  --legal-count 3000 \
  --malformed-count 1000
```

Outputs are written to `artifacts/differential/`:

- `report.md`: human-readable summary;
- `report.json`: machine-readable summary;
- `error_distribution.csv`: exact absolute-error histogram;
- `counterexamples.jsonl`: every over-tolerance case, silence/click finding,
  malformed-packet mismatch, and the minimized parameter tuple for each legal
  sample-error counterexample.

To reuse existing builds:

```sh
python3 tests/differential/run_differential.py --skip-build
```

## Minimal parameter combination

For each failing legal case, the runner minimizes this tuple in order:

`frames, switch_every, switch_kind, signal, amplitude, frequency, mode,
quality, complexity, vbr, vbr_quality, highpass, enh, seed`.

The predicate that must remain true is "at least one decoded sample differs by
more than the configured quantization level".  Candidates are evaluated in
batches and only strict tuple improvements are accepted, so the process
terminates at a local minimum.

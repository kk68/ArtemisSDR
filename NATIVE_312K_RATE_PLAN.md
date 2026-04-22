# Native 312.5 kHz IQ Rate for SunSDR2 DX — v2.0.8

**Status (shipped on `feature/native-312k-rate`)**: working. Clean RX audio on SunSDR2 DX at the radio's native 312.5 kHz IQ rate. No intermediate resampler on the RX path. No VAC underflows. NR2 clean. Mode/band switching clean.

## Why

Before v2.0.8, Artemis resampled the radio's native 312,500 Hz IQ up to 384,000 Hz via a simple linear interpolator in `sunsdr.c` and then fed WDSP at 384 kHz. The linear interp has poor image rejection, which showed up on user reports as ghost signals in dense-spectrum conditions (GH #26 — Thomas DF2LH contest recordings; Elvis ELSDR on 40 m next to 41 m broadcast). EESDR3 on the same hardware was clean in all the same scenarios — it runs 312.5 kHz natively and has no equivalent resample stage.

Removing that resampler stage eliminates the whole class of image-aliasing artifacts that were the root of GH #26 and restores data-path parity with EESDR3.

## What shipped (three phases)

### Phase 1 — Make 312,500 a first-class Artemis sample rate (C# side)

Files: `setup.cs`, `console.cs`, `MeterManager.cs`, `ucOtherButtonsOptionsGrid.cs`.

- New `OtherButtonId.SR_312500` enum entry and grid button (row 7, col 8, "312k").
- `setup.cs`: `SetHWSampleRate` validator accepts 312500; `sunsdr_rates = { 312500 }` (was `{ 384000 }`).
- `console.cs`: mirroring case handlers for `SR_312500` in every SR_* switch/case (`setOtherButtonState`, tab-show fall-through, `GetGeneralSetting`, `GetHWSampleRate` check, `initGeneralSettings` iteration, `DoGeneralSettingAction`, `SetHWSampleRateSetting` validator + set-all-off + switch).
- `MeterManager.cs`: `SR_312500` added to `general_settings` dictionary loader.

Integer-ratio rates (every Anan / HL2 rate) unaffected.

### Phase 2 — Remove the custom RX resampler

File: `sunsdr.c`.

- Deleted `sunsdr_resample` (65 lines of linear interp + its `SUNSDR_NATIVE_RATE` / `SUNSDR_TARGET_RATE` / `SUNSDR_RESAMPLE_STEP` / `SUNSDR_RESAMPLE_MAX` defines + `sunsdr_resampler_state_t` struct + `resampler[2]` array).
- Rewrote the RX call site to feed `xrouter` directly from `sdr.rxBuf` at `SUNSDR_IQ_COMPLEX_PER_PKT` (200) samples per packet.
- Silence-buffer sizing switched from hardcoded `246` (old resampler output) to `SUNSDR_IQ_COMPLEX_PER_PKT` (native packet size).
- Removed orphan `memset(resampler, 0, sizeof(resampler))` in `SunSDRInit` that no longer compiled after the delete.

TX-path resampler (`SUNSDR_TX_INPUT_RATE` / `SUNSDR_TX_OUTPUT_RATE` at 192 k → 39062.5 k) untouched — separate downsampler, separate concerns.

### Phase 3 — WDSP and ChannelMaster support for non-integer sample-rate ratios

Five files: `wdsp/iobuffs.c`, `wdsp/channel.c`, `wdsp/emnr.c`, `ChannelMaster/cmsetup.c`, `ChannelMaster/cmaster.c`.

WDSP was authored (Warren Pratt, NR0V) assuming every in:dsp:out sample-rate ratio is a clean integer. That assumption is baked into the buffer sizing, the ring-wrap math, the polyphase resampler alignment, and the DSP block sizes. At 312,500 / 48,000 the ratio is 6.5104…, which breaks each of those assumptions in a distinct way. Five surgical patches to unblock the non-integer path, all of which collapse to identity on integer ratios so every Anan / HL2 use case is unaffected.

1. **`iobuffs.c:create_iobuffs` — LCM ring sizing.** The original sizing used `max(r1_outsize, in_size)` for the r1 ring (and similarly for r2). The ring-wrap check is `(idx += step) == active_buffsize` — only triggers at exact equality. For non-integer ratios where producer step and consumer step differ (e.g. in_size=416, r1_outsize=384 at the pre-alignment numbers), `max()` gave a buffsize that wasn't a common multiple; the wrap never fired at the right index, and subsequent memcpys walked past the allocation. AV in `wdsp.dll!dexchange+0xf9`, diagnosed from the first crash dump. Replaced with `LCM(r1_outsize, in_size)` which reduces to `max()` when the two steps are equal.

2. **`channel.c:pre_main_build` — multiply before dividing.** The original block-size math was `dsp_insize = dsp_size * (in_rate / dsp_rate)`. The rate ratio divides first in integer arithmetic, truncating to floor. For 312500/48000 that floors to 6 instead of 6.51, giving `dsp_insize = 64 * 6 = 384` when it should be 417 — an ~8% rate mis-scaling through every downstream FFT, filter and demod. Changed to `(dsp_size * in_rate) / dsp_rate` in a 64-bit intermediate (avoids overflow at large dsp_size × 1.5 MHz). Integer ratios: identical result.

3. **`channel.c:adjust_dsp_size_for_rate_ratio` — round `dsp_size` up to a stable LCM across common dsp_rates.** WDSP's `rsmpin` polyphase resampler only emits a deterministic number of output samples per call when the input block length is a multiple of M = `in_rate / gcd(in_rate, dsp_rate)`. Without that alignment, rsmpin emits 95 samples on one iteration and 97 on the next — a ±1-sample discontinuity at every block boundary, audible as a ~750 Hz buzz. The fix rounds `dsp_size` up to a multiple of L = `dsp_rate / gcd`, restoring determinism. *Critically*, it rounds up to `LCM(L_at_48kHz, L_at_192kHz)` rather than the current dsp_rate's L alone — so when the mode switches to FM (which bumps dsp_rate 48 k → 192 k), `dsp_size` is already aligned and WDSP doesn't need to rebuild its FFTW_PATIENT plans. Without that LCM stability, mode switches caused ~4-second UI-thread freezes that Windows flagged as "not responding" and WER killed. For in_rate=312500: `dsp_size = 384` (LCM of 96 and 384). For Anan/HL2 integer ratios: L = 1 at every dsp_rate, LCM = 1, no change.

4. **`cmsetup.c:getbuffsize` — round `in_size` up to multiple of M.** Same requirement as (3) but applied to `in_size` (the block size `fexchange` consumes per call). `getbuffsize(rate)` was `64 * rate / 48000` — fine for integer ratios, non-aligned for 312500. Rounded up to next multiple of M so rsmpin's input side is also deterministic. For 312500: `in_size = 625` (was 416). For 156250: 625. For 78125: 625 (all three SunSDR rates share M = 625 against the 48 kHz base). Integer ratios unchanged.

5. **`cmaster.c:SetXcmInrate` and `cmsetup.c:set_cmdefault_rates` — propagate sizes to the audio side.** `rcvr[i].ch_outsize` (the audio-output block forwarded by `pipe.c:189` memcpy) and `pcm->audio_outsize` (the AAMixer ring sizing) were being computed as `getbuffsize(48000) = 64`, which under-samples the 96-sample blocks WDSP now emits per call at 312500 → 48000 conversion. Result was 33% of audio samples dropped at `pipe.c`'s memcpy, VAC perpetually under-fed, thousands of underflows. Worse, `SetXcmInrate` — the runtime entry point that's called when SunSDR switches from its initial 192 kHz mock-rate to the actual 312.5 kHz — updated WDSP-input-side sizes but did not refresh the audio-output side. Both init-time and runtime paths now compute `ch_outsize = xcm_insize × ch_outrate / xcm_inrate`, and `audio_outsize` is set to match `rcvr[0].ch_outsize`. `SetXcmInrate` additionally propagates via `SetIVACaudioSize`, `SetTCIRxAudioSize`, `SetAAudioRingInsize`, `SetAAudioRingOutsize`. All algebraically identical to the original `getbuffsize(audio_outrate)` formula for integer-ratio rates.

6. **`emnr.c:calc_emnr` — output accumulator = LCM(bsize, incr).** EMNR (the NR2 algorithm) uses a fixed 4096-point FFT with 4× overlap-save (`incr = 1024`). It writes `incr` output samples to `outaccum[oainidx..oainidx+incr-1]` per FFT fire. Reader drains `bsize` samples per xemnr call. The original sizing used `max(bsize, incr) = incr = 1024` which forces writer to overwrite the same 1024-slot region every FFT. For integer ratios where bsize divides incr, reader completes a full ring drain in one FFT cycle and the overwrite lines up. For bsize=384 at 312.5 k, 1024 is not a multiple of 384 — reader's drain straddles the overwrite boundary every FFT cycle, leaving a sample-level discontinuity once per ~20 ms overlap period. Audible as click train when NR2 is enabled. LCM-sized ring (3072 at bsize=96, larger at bsize=384) lets reader and writer cycle through the full ring in sync, writer always ahead of reader. Integer-ratio: LCM equals `max()`, no change.

## Data-flow before vs after

```
BEFORE (v2.0.7):
  SunSDR 312.5 k IQ → sunsdr_resample (linear interp, no anti-alias)
                    → 384 k into WDSP → rmatch → 48 k audio

AFTER (v2.0.8):
  SunSDR 312.5 k IQ → xrouter → WDSP at 312.5 k → rmatch → 48 k audio
                                                 (no intermediate resampler)
```

## Zero-regression claim

Every patch reduces to the original behaviour when `gcd(in_rate, dsp_rate) == dsp_rate` — i.e. when the rate ratio is a clean integer. Every Anan / HL2 supported rate (48, 96, 192, 384, 768, 1536 kHz) satisfies this against the 48 kHz DSP rate, so Anan / HL2 users see no change in WDSP behaviour.

## Known limitations

- **156.25 kHz and 78.125 kHz SunSDR rates**: the LCM/precision patches already handle them, but the wire-protocol `SetSampleRate` opcode isn't reverse-engineered yet. Radio still delivers its default (312.5 kHz). Future work — capture ExpertSDR3 switching rates, decode opcode, wire `SunSDRSetSampleRate()` into C#. `sunsdr_rates` array in `setup.cs` then extends to `{ 78125, 156250, 312500 }`.
- **39.0625 kHz SunSDR rate**: half-integer Hz, clashes with WDSP's `int samplerate` API. Either extend the API to `double` or internally feed it as upsampled 78.125 kHz. Out of scope for v2.0.8.
- **Per-band state persistence** (NR1/NR2/ANF/NB/…) was inconsistent before this refactor and remains so. Separate audit + fix for a later release.

## Debugging tooling invested

During implementation we built a Python-based crash-dump analysis pipeline (`minidump` library + `dbghelp.dll` via ctypes) that resolves x64 exception records, walks thread stacks, and maps RVAs to function names via the `wdsp.pdb` and `ChannelMaster.pdb` symbol files. This stays in `c:/tmp/` (not checked in; personal tooling) but documented here as a resource for future similar debugging. Pipeline usage:

```
python c:/tmp/analyze_dump.py <path to .dmp>      # exception + crashing-thread stack
python c:/tmp/scan_threads.py <path to .dmp>     # all threads; useful for deadlocks / hangs
```

It was essential for decoding the mid-refactor AV at `dexchange+0xf9` (turned out to be the ring-wrap bug) and for confirming that the second round of "crashes" were actually UI-thread freezes from FFTW_PATIENT plan creation rather than native faults.

## Release notes stub (for v2.0.8)

> **SunSDR2 DX**: Artemis now runs the RX path at the radio's native 312.5 kHz IQ rate end to end. The upsampler to 384 kHz is gone; the resulting image-aliasing artifacts (ghost CW under SSB on dense-spectrum bands, 41 m broadcast bleeding into 40 m amateur — GH #26) are eliminated structurally. Requires Artemis 2.0.8 and the normal EE-published firmware for your radio. Anan / HL2 users: no changes to your path.

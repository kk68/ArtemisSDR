# Anti-alias Resampler Plan — FALLBACK

> **Status (2026-04-22): FALLBACK.** Strategic decision is to pursue the **native 312.5 kHz refactor** instead. See [`NATIVE_312K_RATE_PLAN.md`](NATIVE_312K_RATE_PLAN.md) on `feature/native-312k-rate` — that approach removes the resampler entirely, which is architecturally cleaner and achieves EESDR3 parity. This plan is kept as the **fallback** if the native refactor hits an unrecoverable snag (most likely in VAC phase-lock or rmatch rate-alignment) that forces a pivot to patching the existing resampler instead of removing it.
>
> **Do not start Phase 0 here unless the native refactor is abandoned.** If you are reading this while native is in progress and failing, this doc is your plan B.

---

Targeted fix for GH #26 (ghost signals). Replace the linear-interpolation upsampler on the SunSDR RX path with a properly bandlimited resampler so strong adjacent-band content (41 m broadcast next to 40 m amateur, contest-density on 20 m, etc.) stops producing phantom image copies in the display window.

**Status**: FALLBACK (see header). Planning only. No code changes yet.
**Branch**: `feature/antialias-prefilter` (this branch).
**Related**: [`NATIVE_312K_RATE_PLAN.md`](NATIVE_312K_RATE_PLAN.md) on `feature/native-312k-rate` — the primary strategy; removes the resample stage entirely. Mutually exclusive with this plan; ship one, not both.

## Problem

The SunSDR radio delivers IQ at 312,500 Hz. Artemis currently upsamples that to 384,000 Hz before feeding WDSP, using simple linear interpolation between consecutive samples (see `Project Files/Source/ChannelMaster/sunsdr.c` line 1146, `sunsdr_resample()`). Linear interp attenuates high-frequency content in a sinc² shape and has poor image rejection — strong content near the upper edge of the 312.5 kHz source window produces visible and audible image copies in the target spectrum.

EESDR3 runs 312.5 kHz natively through its DSP chain. There is no equivalent upsample stage. That single structural difference is consistent with every "clean on EESDR3 / ESDR2 / HL2-Thetis but ghosts on Artemis" report we have.

## Scope

- **In scope**: the 312.5 → 384 kHz RX upsample only. Specifically, the internals of `sunsdr_resample()` and its state.
- **Out of scope**: the TX-side 192 → 39.0625 kHz downsampler (different function, different story; revisit later if TX-path aliasing becomes a concern).
- **Out of scope**: WDSP's internal `rmatch` stage that drops to 48 kHz audio. That is already polyphase and appears fine.
- **Out of scope**: the broader native-312k refactor. If that ever lands, this plan becomes moot.

## Candidate approaches

| Option | Summary | Pros | Cons | Effort |
|---|---|---|---|---|
| **A. WDSP `rmatch` standalone** | Instantiate a WDSP polyphase resampler for the 312.5 → 384 ratio | In-tree, proven correct on non-integer ratios, no new deps | Must verify `rmatch` can be instantiated outside its normal WDSP-pipeline context | Medium |
| **B. Hand-rolled polyphase FIR** | Write a small polyphase FIR resampler specifically for this ratio | Full control, zero deps, tunable | Filter design work, broader test coverage | Medium-high |
| **C. External library (`libsoxr`)** | Link a best-in-class SRC library | Top-tier quality, well-tested | Adds a runtime DLL and build dep; packaging work in the installer | Medium |
| **D. Anti-alias LP before current linear interp** | Keep linear interp, prepend a LP filter | Minimal change | Addresses wrong problem — the linear interp's weakness is imaging *during* upsample, not source aliasing; unlikely to help | Low but likely ineffective |

**Recommendation**: start with **A**. If `rmatch`'s API isn't cleanly instantiable from `sunsdr.c`, fall back to **B**. **C** is a last resort to avoid adding a new DLL to the installer. **D** is listed only to rule it out.

## Phase 0 — Measurement baseline (~2 h)

Before any code change, quantify the current behavior.

- Build a small test harness (offline, no UI): feed synthetic complex sinusoids at N frequencies spanning 0 → ±Nyquist of source through `sunsdr_resample()`.
- Measure output spectrum; record image-rejection dB at each test frequency.
- Also feed a two-tone IMD test to see if anything nonlinear surfaces.
- Save the numbers in `tools/antialias/baseline.md` (new file) as the "before" reference.

**Exit criterion**: baseline numbers recorded. No code change yet.

## Phase 1 — Feasibility (~2–4 h)

- Read the `rmatch` headers/source in `Project Files/Source/wdsp/`. Identify the constructor/destructor API, sample-rate parameters, and whether state can live outside of WDSP's radio/channel structs.
- If `rmatch` is instantiable from `sunsdr.c`: write a 10-line smoke test that creates one, pushes a few buffers through, destroys it. No integration yet.
- If not: draft a polyphase FIR filter spec. Target: 64-tap prototype, `>=60 dB` image rejection, passband flat to source-Nyquist × 0.9, group delay `< 0.5 ms` at 384 kHz output rate.

**Exit criterion**: a clear go/no-go between A and B. Document the chosen approach in this file.

## Phase 2 — Implementation (~8–16 h)

- Replace `sunsdr_resample()` internals with the chosen resampler. **Keep the outer function signature unchanged** so no call site is touched.
- Maintain any required state on `sunsdr_resampler_state_t[2]` (RX1 + RX2). Resampler state initialized when the first IQ packet arrives after PowerOn; reset on the same trigger that currently resets the linear interp.
- Re-run the Phase-0 harness against the new implementation.

**Exit criterion**: image rejection ≥ 60 dB at the test frequencies. No regression vs Phase-0 baseline on passband response.

## Phase 3 — Regression & performance (~4–8 h)

- Regression test matrix: CW, SSB, AM, FM, FT8/digital on RX1 + RX2, all bands. Expectation: identical subjective audio except the ghost-producing conditions, where ghosts should now be absent or much weaker.
- Bench CPU overhead: on a mid-tier laptop target (i5-8xxx class) the full RX path must stay under its current budget headroom. If FIR pushes us over, tune tap count or consider SIMD.
- Bench added latency: group delay must stay below the existing timing budget. Any full-duplex mode (TX/RX simultaneous) sensitive to RX-latency drift — TUNE, Mic-monitor, CAT-PTT roundtrip — must be unchanged.
- Real-signal A/B: if @DF2LH or @ELSDR can share a recording that reliably triggers a ghost, play it through both baseline and new-resampler builds, compare.

**Exit criterion**: no regression on any of the above. Ghost behavior on the real-signal recording is visibly improved.

## Phase 4 — Ship (~2 h)

- Gate the new resampler behind a setup-time feature flag (`UseLinearInterpResampler`, default `false`) so we can flip back to the old path in the field if an edge case surfaces.
- Release note referencing #26. Ping @DF2LH and @ELSDR on the issue with the release build and ask them to confirm.
- On confirmation, close #26.

**Exit criterion**: release shipped, #26 closed.

## Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| `rmatch` API isn't externally instantiable | Medium | Fall back to hand-rolled polyphase FIR (Option B) |
| FIR adds CPU that pushes the RX thread over budget on weaker hardware | Medium | Tune tap count; consider SIMD; measure on target class early |
| Added group delay breaks a timing-sensitive full-duplex mode | Low | Group-delay budget explicit in Phase 2 exit criterion |
| Fix reveals secondary aliasing elsewhere (TX path) | Low | Out of scope here; track as new issue if surfaced |
| Fresh ghosts somewhere else due to design error in new resampler | Low | Phase-0 baseline + Phase-2 harness catch this before ship |

## Estimate

| Phase | Estimate |
|---|---|
| 0 | 2 h |
| 1 | 2–4 h |
| 2 | 8–16 h |
| 3 | 4–8 h |
| 4 | 2 h |
| **Total** | **18–32 h**, LARGE |

Dominant risk on estimate: Phase 1 result determining approach A vs B.

## Relationship to `NATIVE_312K_RATE_PLAN.md`

- The 312k refactor **removes** the resample stage; this plan **fixes** it.
- Both address GH #26 but at different architectural levels.
- Ship this one if we want the shortest path to a user-facing fix.
- Ship the 312k refactor later if we also want the cleaner architecture (one resample stage instead of two) and the CPU savings that come with it.
- Do **not** ship both simultaneously — pick one.

## Gating data (before Phase 0 starts)

Hold Phase 0 until at least one of the following is in hand:

- @DF2LH or @ELSDR confirms the ghost persists at **−20 dB ATT** (data point that rules in DSP-side aliasing as the dominant cause).
- Or a local reproducer: a recorded RF capture on the SunSDR2 DX that shows a ghost under Artemis and does not under EESDR3 on the same IQ.

Without gating data, Phase 0 wastes time on a problem that might be elsewhere.

## Next step

Wait on `−20 dB` data from either reporter. On confirmation, run Phase 0 and update this doc with the baseline numbers.

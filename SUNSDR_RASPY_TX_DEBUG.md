# SunSDR Raspy TX Debug Tracker

Date started: 2026-04-14

## Problem

Intermittent raspy tone/audio during SunSDR TUNE and MOX/TX. Current report is roughly 50-60% occurrence. Working hypothesis: a stream timing or state-transition sync issue around active TX entry, TX keepalive, and RX silence padding.

## Current Status

- Status: seventh fixed-drive TUNE-only run analyzed. Drive stayed at UI drive 1/raw drive 30. Stream counters, broad audio/VAC counters, and managed TUNE generator state still do not explain clean vs raspy attempts. The next focused candidate is the duplicate native TUNE state transition around the TUNE-to-MOX/PTT handoff.
- Primary file: `Project Files/Source/ChannelMaster/sunsdr.c`
- First reproduction target: repeated TUNE on/off cycles. TUNE-on time is usually 2-3 seconds; TUNE-off time may be 10-30 seconds so results can be recorded manually.
- Keep this tracker updated after every test or code change until the issue is resolved.
- Commit and push every meaningful stable checkpoint to `kk68/feature/sunsdr2dx`. Do not push to upstream `ramdor/Thetis`.

## Instrumentation Added

The 2026-04-14 pass added diagnostics for:

- TX attempt boundaries: `TX_ATTEMPT_BEGIN`, `TX_ATTEMPT_0x06_SENT`, `TX_ATTEMPT_END`
- first active TX packet timing: `TX_FIRST_FD`
- TX packet sequence and interval metrics: active `0xFD` count, sequence gaps, gap min/avg/max
- keepalive races: `TX_KEEPALIVE_RACE`, `feDuringTx`, `keepaliveRaces`
- read-thread feed rates: `txFeed`, `keepaliveFE`, `rxSilence`, `realFE`, `realFD`, `xrouterReal`, `xrouterTotal`, `rxAccum`

The next 2026-04-14 audio pass added diagnostics for:

- TX audio callback aggregate health in `TX_ATTEMPT_END`: `txCb`, `txCbSilent`, `txCbNonfinite`, `txCbRmsMin`, `txCbRmsAvg`, `txCbRmsMax`, `txCbPostPeakMax`, and `txCbMaxGapMs`
- VAC/ASIO/rmatch snapshots at attempt boundaries: `TX_AUDIO_DIAG BEGIN` and `TX_AUDIO_DIAG END`
- CMA/ASIO state: `audioCodecId`, `run`, `block`, `lockMode`, direct over/underflow counters, and rmatch event counters when available
- VAC1/VAC2 state: run/mox/monitor/bypass/combine flags, rates/sizes, latencies, rmatch pointers, underflows, overflows, var, ring size, and ring fill count

The next TUNE-generator pass added diagnostics for:

- Managed TUNE generator state in native log lines: `TUNE_AUDIO_STATE`
- Labels around TUNE on/off ordering: `TUNE_ON_POSTGEN_SET`, `TUNE_ON_POST_POWER`, `TUNE_ON_PRE_NATIVE_TUNE`, `TUNE_ON_POST_NATIVE_TUNE_PRE_MOX`, `TUNE_ON_AFTER_CHKMOX`, `TUNE_OFF_PRE_NATIVE_TUNE`, `TUNE_OFF_POST_NATIVE_TUNE`, `TUNE_OFF_AFTER_CHKMOX`, and `TUNE_OFF_POSTGEN_STOP`
- Fields include checkbox/MOX state, `_tuning`, `_mox`, TX DSP mode, current TX DSP mode, post-generator run/mode/tone frequency/tone magnitude, pulse mode flags, tune power source, power, new power, native attempt/PTT/TUNE state, TX sequence, TX packet count, TX phase, and raw drive.

The incomplete placeholder `extern double rx_silence_accum_public;` was removed. Real source-0 packets now debit the local RX silence accumulator during TX so the diagnostic path can expose overfeed.

## How To Test

1. Build `ChannelMaster` Debug x64.
2. Start Thetis with SunSDR2 DX selected.
3. Confirm RX is working before TX tests.
4. Run at least 10 repeated TUNE cycles:
   - TUNE on for 2-3 seconds.
   - TUNE off for 2-3 seconds.
   - Record each attempt number as clean or raspy.
5. Collect `sunsdr_debug.log`.

## What To Compare

For clean vs raspy attempts, compare:

- `TX_ATTEMPT_END ... feDuringTx`
- `TX_ATTEMPT_END ... keepaliveRaces`
- `TX_ATTEMPT_END ... firstFdDelayMs`
- `TX_ATTEMPT_END ... fdGapMin/fdGapAvg/fdGapMax`
- `TX_ATTEMPT_END ... seqGaps`
- per-second `IQ status` fields during the attempt, especially `rxSilence + realFD` versus expected feed behavior
- `TX_ATTEMPT_END ... txCb*` fields, especially non-finite samples, silent callback count, RMS min/avg/max, and callback gap spikes
- `TX_AUDIO_DIAG BEGIN/END` VAC/ASIO underflow/overflow deltas for raspy attempts versus clean attempts

## Current Hypotheses

- 2026-04-14 TUNE data did not support the idle keepalive race as the active failure: attempts 1-20 all ended with `feDuringTx=0`, `keepaliveRaces=0`, and `seqGaps=0`.
- TX packet cadence also looked stable in the first run: `fdGapAvg` was about 5.10-5.12 ms and `fdGapMax` was 16 ms.
- Run 2 confirmed the RX silence cap fix works mechanically: `rxSilence` is now about 1562-1563/sec during TX and `rxAccum` stays bounded below 1. This improved RX/VAC behavior, but TUNE raspiness remains.
- Run 3 confirmed the TX IQ gate fix works mechanically: `TX_FIRST_FD` now has `cmd_sent=1`, `firstFdBefore0x06=0`, and no `0xFE` keepalive races. TUNE raspiness still occurs, so the remaining visible `sunsdr.c` stream counters no longer correlate.
- Run 4 showed that pre-priming one active silence `0xFD` after `0x06=1` made the result worse: 12/20 TUNE attempts were raspy. That candidate is rejected and removed.
- Run 5 showed a clear high-drive effect for attempts 1-5, but did not explain the remaining low-drive failures. Current phase: expand from broad TX callback/VAC health into more specific managed Thetis TUNE generator state and TX monitor/RX path state around each TUNE attempt.
- Run 6 held UI drive at 1/raw drive 30 and still reproduced intermittent TUNE raspiness. Stream and broad audio/VAC counters again did not correlate.
- Run 7 added `TUNE_AUDIO_STATE` coverage and still did not show a clean-vs-raspy generator-state difference. The visible remaining suspect is state-transition ordering: each TUNE on/off currently sends native TUNE state twice, first in the TUNE handler and again through the MOX/PTT path.

## 2026-04-14 TUNE Run 1

User test: 20 TUNE cycles, roughly 3 seconds each.

Reported results:

- Raspy: attempts 1, 2, 3, 4, 10, 11, 14, 15, 19.
- Clean: attempts 5, 6, 7, 8, 9, 12, 13, 16, 17, 18, 20.
- Several raspy/partly raspy RX cases reported VAC1 overflow count increasing; cycling VAC cleared RX. Attempt 20 was mostly clean but had a short break in TUNE.

Log correlations:

- No active-TX keepalive race observed: `feDuringTx=0`, `keepaliveRaces=0` for attempts 1-20.
- No TX sequence discontinuities observed: `seqGaps=0` for attempts 1-20.
- Active TX `0xFD` cadence was stable: `fdGapAvg` about 5.10-5.12 ms, `fdGapMax=16 ms`.
- First `0xFD` delay after `0x06=1` was usually 15-16 ms. Attempts 11, 16, and 20 logged 0 ms; attempt 20 also logged the first active TX packet before the `0x06` marker, but this did not correlate cleanly with raspiness.
- During TX, per-second logs showed `rxSilence` around 968-976/sec, `realFD` around 195-196/sec, and `xrouterTotal` around 968-976/sec. `rxAccum` grew instead of staying bounded, indicating RX silence injection was falling behind.

Code change after this run:

- Raised the RX silence injection per-loop cap in `sunsdr.c` from 4 to 16. This is intended to let the TX-paced read loop keep RX/VAC fed near the expected 1562.5 buffers/sec while still bounding bursts after scheduler delays.

## 2026-04-14 TUNE Run 2

User test: 20 TUNE cycles after commit `40823f1b`.

Reported results:

- Raspy: attempts 1, 2, 3, 10, 11, 18, 19, 20.
- Clean: attempts 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17.
- RX was clean on most attempts. Attempt 3 reported RX raspy; the earlier VAC-overflow pattern was not generally present.

Log correlations:

- RX silence injection is now on target during TX: sustained TX seconds show `rxSilence=1562-1563`, `xrouterTotal=1562-1563`, and `rxAccum` around 0.125-0.875 instead of growing into the thousands.
- No active-TX keepalive race observed: `feDuringTx=0`, `keepaliveRaces=0` for logged attempts.
- No TX sequence discontinuities observed: `seqGaps=0`.
- Active TX `0xFD` cadence still looks stable: `fdGapAvg` about 5.10-5.12 ms, `fdGapMax=16 ms`.
- Early active-TX IQ before the `0x06` marker appeared on attempts 11, 13, and 18: `TX_FIRST_FD ... cmd_sent=0`. Attempt 18 was reported raspy, while attempt 13 was clean, so this is not a complete correlation but is the next concrete ordering bug to eliminate.
- The log currently contains attempts 1-19; the written user report includes attempt 20. Check whether attempt 20 was after the captured log tail or not flushed before analysis.

Code change after this run:

- Added a private TX IQ gate in `sunsdr.c`: `currentPTT` still flips before `0x06=1` so the idle keepalive path stops, but `sunsdr_tx_outbound` will not send active `0xFD` packets until after `0x06=1` completes and `TX_IQ_GATE_OPEN` is logged.
- Added `iqGateSkips` and explicit `firstFdBefore0x06` diagnostics so the next run can verify the gate is doing work and active TX packets no longer predate the command.

## 2026-04-14 TUNE Run 3

User test: 20 TUNE cycles after commit `1fba5507`.

Reported results:

- Raspy: attempts 2, 5, 6, 8, 10, 16, 19.
- Clean: attempts 1, 3, 4, 7, 9, 11, 12, 13, 14, 15, 17, 18, 20.
- RX raspy was reported after attempts 15, 16, and 17; RX was clean for the other listed attempts.

Log correlations:

- The log contains attempts 1-19; the written user report includes attempt 20 after the captured attempt list.
- TX IQ gate worked: every logged `TX_FIRST_FD` had `cmd_sent=1`, and every `TX_ATTEMPT_END` had `firstFdBefore0x06=0`.
- No active-TX keepalive race observed: `feDuringTx=0`, `keepaliveRaces=0`.
- No TX sequence discontinuities observed: `seqGaps=0`.
- RX silence injection remained healthy: sustained TX seconds stayed around `rxSilence=1562-1563`, `xrouterTotal=1562-1563`, and bounded `rxAccum`.
- `iqGateSkips` appeared on some clean and raspy attempts, so it is not a direct correlation.
- Steady-state TX audio callback amplitude/rate logs looked similar on clean and raspy attempts.

Code change after this run:

- Added `TX_PREPRIME_FD`: after `0x06=1` returns and before live TX IQ is ungated, send one active `0xFD` silence packet. This removes the initial 15-16 ms window where the radio is in TX but has not yet received any active TX IQ packet from the audio callback.

## 2026-04-14 TUNE Run 4

User test: 20 TUNE cycles after commit `e32ad68c`.

Reported results:

- Raspy: attempts 1, 2, 3, 4, 5, 6, 7, 10, 12, 13, 14, 16.
- Clean: attempts 8, 9, 11, 15, 17, 18, 19, 20.
- RX raspy was reported after attempts 10, 12, and 13; RX was clean for the other listed attempts.

Log correlations:

- The log contains attempts 1-19; the written user report includes attempt 20 after the captured attempt list.
- `TX_PREPRIME_FD` fired on every logged attempt and moved `firstFdDelayMs` to 0 ms.
- No active-TX keepalive race observed: `feDuringTx=0`, `keepaliveRaces=0`.
- No TX sequence discontinuities observed: `seqGaps=0`.
- RX silence injection remained healthy: sustained TX seconds stayed around `rxSilence=1562-1563`, `xrouterTotal=1562-1563`, and bounded `rxAccum`.
- The pre-prime candidate is rejected because TUNE raspiness worsened from 7/20 in Run 3 to 12/20 in Run 4.

Code change after this run:

- Removed the `TX_PREPRIME_FD` behavior from `sunsdr.c`. The prior RX silence cap and TX IQ gate fixes remain.

## 2026-04-14 TUNE Run 5

User test: 20 TUNE cycles after commit `4f0e7035` with audio-layer diagnostics.

Reported results:

- Raspy: attempts 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 18.
- Clean: attempts 10, 12, 13, 15, 16, 17, 19, 20.
- RX was clean except attempt 11, which was reported RX raspy.

Log correlations:

- Attempts 1-5 ran at high raw drive 196 (`drive=0.769`, `fs=1.353`) and were all raspy. They also logged `txCbPostPeakMax=0.931893`.
- Before attempt 6, the log shows a manual drive reduction sequence down to raw drive 30. Attempts 6-20 ran at raw drive 30 (`drive=0.118`, `fs=1.007`) with `txCbPostPeakMax=0.106154`.
- The high-drive state likely explains attempts 1-5, but not the remaining low-drive raspy attempts 6, 7, 8, 9, 11, 14, and 18.
- No active-TX keepalive race observed: `feDuringTx=0`, `keepaliveRaces=0` for attempts 1-20.
- No TX sequence discontinuities observed: `seqGaps=0` for attempts 1-20.
- TX IQ ordering remained clean: `firstFdBefore0x06=0` for attempts 1-20.
- TX callback health did not correlate with low-drive raspiness: `txCbNonfinite=0`, `txCbMaxGapMs=16`, and RMS/peak values were similar across low-drive clean and raspy attempts.
- VAC/ASIO diagnostics did not correlate with low-drive raspiness. VAC1 had early counters during attempts 1-12, then counters reset to 0 before attempt 13; raspy attempts still occurred with zero VAC1 underflow/overflow counters.
- Attempt 16 was clean despite `iqGateSkips=11`, so gate skips are still not a failure correlation.
- Attempts 6, 10, 18, and 20 had `firstFdDelayMs=0`; attempts 6 and 18 were raspy, while 10 and 20 were clean, so first-FD delay is still not a direct correlation.

Next validation:

- Rebuild `ChannelMaster` Debug x64.
- Repeat the same 20 TUNE cycles and record clean/raspy attempts.
- Keep raw drive consistent for the next 20 attempts so the high-drive effect does not mask the intermittent low-drive failure.
- Add next-layer diagnostics before attempting another behavior change: managed Thetis TUNE generator state, `TXPostGen*` timing, and TX monitor/RX path state around each TUNE attempt.

## 2026-04-14 TUNE Run 6

User test: 20 TUNE-only cycles after run 5. RX classification intentionally omitted to keep the test simple.

Reported results:

- Raspy: attempts 3, 4, 5, 9, 11, 16, 17, 19.
- Clean: attempts 1, 2, 6, 7, 8, 10, 12, 13, 14, 15, 18, 20.
- User confirmed drive was held at UI drive 1. Native logs show this as raw drive 30 for all attempts.

Log correlations:

- All logged attempts 1-20 ran at `rawDrive=30`, `drive=0.118`, `fs=1.007`, so this run isolated the low-drive failure.
- No active-TX keepalive race observed: `feDuringTx=0`, `keepaliveRaces=0`.
- No TX sequence discontinuities observed: `seqGaps=0`.
- TX IQ ordering remained clean: `firstFdBefore0x06=0`.
- TX callback health still did not correlate: `txCbNonfinite=0`, `txCbMaxGapMs=16`, and `txCbPostPeakMax=0.106154` on every attempt.
- VAC1 underflow/overflow counters were already nonzero at run start, but they stayed flat at `outUnder=1160`, `inOver=1159` through attempts 2-20. Raspy and clean attempts both occurred with unchanged VAC counters.
- `iqGateSkips` appeared on attempts 9, 15, and 16. Attempts 9 and 16 were raspy, but attempt 15 was clean, so this is not a direct correlation.
- `firstFdDelayMs=0` appeared on attempts 5, 13, and 18. Attempt 5 was raspy, while 13 and 18 were clean, so this is not a direct correlation.

Code change after this run:

- Added `TUNE_AUDIO_STATE` logging from the managed TUNE handler into the native SunSDR log so the next run can correlate clean/raspy attempts with `TXPostGen*` state and TUNE/MOX ordering.

Next validation:

- Rebuild `ChannelMaster` and Thetis Debug x64.
- Repeat 20 TUNE-only cycles with UI drive 1.
- Compare `TUNE_AUDIO_STATE` labels for clean vs raspy attempts before changing behavior.

## 2026-04-14 TUNE Run 7

User test: 20 TUNE-only cycles after commit `bf074812` with managed TUNE generator handoff logging.

Reported results:

- Raspy: attempts 1, 3, 6, 7, 8, 10, 11, 12, 14, 19, 20.
- Clean: attempts 2, 4, 5, 9, 13, 15, 16, 17, 18.
- User is handling build/compile steps from this point forward.

Log correlations:

- All attempts 1-20 ran at `rawDrive=30`, matching UI drive 1.
- No active-TX keepalive race observed: `feDuringTx=0`, `keepaliveRaces=0`.
- No TX sequence discontinuities observed: `seqGaps=0`.
- TX IQ ordering remained clean: `firstFdBefore0x06=0`.
- TX callback health still did not correlate: `txCbNonfinite=0`, `txCbMaxGapMs=16`, and `txCbPostPeakMax=0.106154` on every attempt.
- VAC1 counters were not a direct correlation. They stayed at `outUnder=1156`, `inOver=1156` for early attempts, then reset to zero before attempt 14; raspy attempts still occurred both before and after the reset.
- `iqGateSkips` occurred on attempts 1, 4, 8, 12, and 20. Attempts 1, 8, 12, and 20 were raspy, but attempt 4 was clean, so this is not a direct correlation.
- `firstFdDelayMs=0` occurred on attempts 6 and 13. Attempt 6 was raspy and attempt 13 was clean, so this is not a direct correlation.
- `TUNE_AUDIO_STATE` lines were consistent across clean and raspy attempts: `postGenRun=1`, `postGenMode=0`, `toneFreq=-600.000`, `toneMag=0.999990`, `pulseEnabled=0`, `pulseOn=0`, `txDspMode=0`, `currentDspMode=0`, `pwr=1`, and `newPwr=1` during TUNE-on.

Important observation:

- The new handoff logging shows that each TUNE-on sends `SunSDRSetTune(1)` twice: once from the TUNE handler before MOX/PTT, then again from the MOX/PTT path before `SunSDRSetPTT(1)`.
- Each TUNE-off similarly sends `SunSDRSetTune(0)` twice: once from the TUNE handler while PTT is still active, then again from the MOX/PTT-off path before `SunSDRSetPTT(0)`.
- This duplicate native state transition does not prove causality, but after the stream, RX silence, drive, broad audio, and TUNE generator checks failed to correlate, it is the next smallest behavior candidate to simplify.

Next validation:

- A narrowly scoped behavior change was made in `console.cs`: the explicit TUNE-handler native `SunSDRSetTune(1/0)` calls remain responsible for TUNE, while the duplicate native TUNE call in the MOX/PTT handler is skipped while `_tuning` is active.
- Normal non-TUNE MOX behavior is preserved by still sending `SunSDRSetTune(0)` for non-TUNE MOX on/off.
- After user builds, repeat 20 TUNE-only cycles with UI drive 1 and compare the clean/raspy rate.

## Verification So Far

- `git diff --check -- Project Files/Source/ChannelMaster/sunsdr.c`: passed.
- `MSBuild ChannelMaster.vcxproj /t:ClCompile /p:SelectedFiles=sunsdr.c`: passed with 0 warnings and 0 errors.
- Full `ChannelMaster` link from this invocation failed because `PA19.lib` is searched under `Project Files/Source/build/x64/Debug`, while the existing dependency libs are under `Project Files/build/x64/Debug`.
- After the RX silence cap change, `MSBuild ChannelMaster.vcxproj /t:ClCompile /p:SelectedFiles=sunsdr.c /p:Configuration=Debug /p:Platform=x64`: passed with 0 warnings and 0 errors.
- After the TX IQ gate change, `MSBuild ChannelMaster.vcxproj /t:ClCompile /p:SelectedFiles=sunsdr.c /p:Configuration=Debug /p:Platform=x64`: passed with 0 warnings and 0 errors.
- After the TX pre-prime change, `MSBuild ChannelMaster.vcxproj /t:ClCompile /p:SelectedFiles=sunsdr.c /p:Configuration=Debug /p:Platform=x64`: passed with 0 warnings and 0 errors.
- After removing the rejected TX pre-prime behavior, `MSBuild ChannelMaster.vcxproj /t:ClCompile /p:SelectedFiles=sunsdr.c /p:Configuration=Debug /p:Platform=x64`: passed with 0 warnings and 0 errors.
- After adding audio-layer diagnostics, `git diff --check -- Project Files/Source/ChannelMaster/sunsdr.c SUNSDR_RASPY_TX_DEBUG.md`: passed.
- After adding audio-layer diagnostics, `MSBuild ChannelMaster.vcxproj /t:ClCompile /p:SelectedFiles=sunsdr.c /p:Configuration=Debug /p:Platform=x64`: passed with 0 warnings and 0 errors.
- After adding TUNE generator handoff diagnostics, `git diff --check` on touched files: passed.
- After adding TUNE generator handoff diagnostics, `MSBuild ChannelMaster.vcxproj /t:ClCompile /p:SelectedFiles=sunsdr.c /p:Configuration=Debug /p:Platform=x64`: passed with 0 warnings and 0 errors.
- After adding TUNE generator handoff diagnostics, `MSBuild ChannelMaster.vcxproj /t:ClCompile /p:SelectedFiles=network.c /p:Configuration=Debug /p:Platform=x64`: passed with 0 warnings and 0 errors.
- `MSBuild Thetis.csproj /t:Build /p:Configuration=Debug /p:Platform=x64` reached the copy step with no C# compile errors reported, then failed because `bin/x64/Debug/Thetis.exe` was locked by the running `Thetis` process.
- After the duplicate TUNE transition reduction, `git diff --check -- Project Files/Source/Console/console.cs SUNSDR_RASPY_TX_DEBUG.md`: passed. No build/compile was run because the user is handling build/compile steps from this point forward.

## 2026-04-14 PS-A Hard-Disable for SUNSDR (Phase A of new plan)

Two new clarifications from the user drove a new direction:
- Raspy is heard on the Anan on-air, not in Thetis MON → variability is in wire TX IQ (upstream of our packet code, must be WDSP output).
- Severity is a gradient (clean → slight → moderate → heavy) → implicates IIR/leak-accumulator state (ALC hang/decay, FIR ring buffers) not a binary race.

Before deeper WDSP-side instrumentation, the user asked to rule out PureSignal (PS-A) completely. Investigation found NO SUNSDR gate on any PS-A state transition — `PSForm.Mox` setter fires `puresignal.SetPSMox` unconditionally on every MOX, `PSForm.PSEnabled` setter toggles `PSRunCal` with no radio check, `PSLoop` polls `SetPSControl`/`GetPSMaxTX`/`GetInfo` every 10 ms, and `xiqc(txa[channel].iqc.p0)` sits in the TX DSP chain at `wdsp/TXA.c:587` (it is a no-op when `run=0`, so keeping the PS state machine from ever setting `run=1` is sufficient to bypass it).

### Changes applied (uncommitted as of this entry)

- **`Project Files/Source/Console/PSForm.cs`**
  - `Mox` setter: when `NetworkIO.CurrentRadioProtocol == RadioProtocol.SUNSDR`, early-return before `puresignal.SetPSMox(...)`. Logs `PS_GATE_MOX_SUNSDR_SKIP` once per session.
  - `PSEnabled` setter: when SUNSDR, force `value = false` so the OFF branch always runs (PSRunCal = false, SetPureSignal(0), router bit cleared). Logs `PS_GATE_ENABLED_SUNSDR_FORCE_OFF` once.
  - `PSLoop`: when SUNSDR and `run` is true, skip the `timer1code()`/`timer2code()` calls and sleep 100 ms. Logs `PS_GATE_LOOP_SUNSDR_SKIP` once.

- **`Project Files/Source/Console/console.cs`**
  - Model-change path (around line 14871, right after `txtVFOAFreq_LostFocus`): when `HardwareSpecific.Model == HPSDRModel.SUNSDR2DX`, uncheck `chkFWCATUBypass`, force `psform.AutoCalEnabled = false` and `psform.PSEnabled = false`.
  - `chkFWCATUBypass_CheckedChanged` (around line 43879): early-return with the checkbox forced unchecked when SUNSDR is active. Logs `PS_GATE_CHECKBOX_SUNSDR_FORCE_OFF` once.

### Verification plan for Run 8

1. Rebuild ChannelMaster and Thetis Debug x64.
2. Confirm in `sunsdr_debug.log` (or the managed debug stream) that the three/four `PS_GATE_*` log lines appear once at startup/first MOX.
3. Confirm PS-A button cannot be enabled for SUNSDR.
4. Run 20 TUNE-only cycles at UI drive 1 (rawDrive 30), 2-3 seconds on, 2-3 seconds off. Record clean/raspy classification with severity grade per attempt: `0=clean`, `1=slight`, `2=moderate`, `3=heavy`.
5. Update this file with Run 8 results.

### Decision after Run 8

- If all 20 attempts are clean (or clearly cleaner than Run 7 where 11/20 were raspy): root cause confirmed as PS-A. Document and close the raspy investigation.
- If still intermittently raspy: PS-A is ruled out. Proceed to Phase B — wire capture of 20 TUNE cycles + WAV recording from the Anan + per-attempt severity grading + spectral analysis of the captured IQ to identify whether the distortion signature is harmonics (nonlinear), wideband noise (filter ring residue), amplitude modulation (AAMIX slew), or phase jitter.

## 2026-04-14 TUNE Run 8 (after PS-A hard disable — Phase A)

User test: 20 TUNE cycles after the PS-A gates added in PSForm.cs and console.cs.

Reported results:

- Raspy: attempts 3, 5, 6, 8, 14, 20 (6 / 20).
- Clean: 1, 2, 4, 9, 10, 12, 13, 15, 16, 17, 18, 19 (12 / 20).
- Clean-with-notes (partial raspy / gradient): 7, 11 (2 / 20).

Compared to Run 7: 11 raspy -> Run 8: 6 raspy. ~15 percentage-point improvement, but raspy is clearly NOT resolved. PS-A was contributing a little but is not the root cause.

Log correlations:

- Every attempt (#2-#21 in the native log) shows `fdGapMax=16.000`, `txCbMaxGapMs=16`, regardless of clean/raspy. The 16 ms max gap corresponds to the Windows scheduler tick granularity. This is the consistent jitter source across all attempts.
- `fdGapAvg` sits at 5.10-5.13 ms (expected ~5.12 ms at 195.3 pkts/sec), tight enough that average cadence is healthy.
- All previously-vetted counters remained clean: `feDuringTx=0`, `keepaliveRaces=0`, `seqGaps=0`, `firstFdBefore0x06=0`, `txCbNonfinite=0`.
- No new correlations visible in the existing counters between clean and raspy.

Decision: PS-A is largely ruled out. Given severity-gradient + on-air-only + identical counters + 16 ms scheduler ticks present on every attempt, the next hypothesis is **CPU / scheduler jitter caused by synchronous `sdr_logf` file I/O**. Each call does `fopen`/`fprintf`/`fflush`, synchronous, and can stall the caller thread for 2-50 ms depending on disk / OS state. With ~100-200 `sdr_logf` calls per second during TX (TX attempt diag, audio diag, callback, telemetry), the calling thread can be blocked inside fflush exactly when it needs to be emitting the next TX packet.

## 2026-04-14 CPU Fix Part 1: Async deferred logger

In `Project Files/Source/ChannelMaster/sunsdr.c`:

- Added a ring-buffered log writer. Producers (all `sdr_logf` call sites) now format the message with timestamp into a stack buffer, then memcpy it into a 4096-entry ring under a critical section, and signal a writer-thread wake event.
- Writer thread runs at `THREAD_PRIORITY_BELOW_NORMAL`, wakes on event (with 100 ms safety timeout), drains the ring, and issues one `fflush` per drain pass. This moves the synchronous I/O cost off the IQ read thread / TX callback / PTT handler thread.
- Lazy-start on first `sdr_logf` call so no caller-side init changes were needed. Ring-full policy drops the newest message and reports drops on next drain. Sync fallback remains for the failure path.
- Stopped cleanly from `SunSDRDestroy` before `fclose`, so exit-time messages flush.

Why: Removes the 5-10% CPU baseline from synchronous fprintf+fflush AND removes the unpredictable 2-50 ms disk stalls that coincidentally land inside TX-critical sections. Both effects were candidate contributors to the intermittent raspy pattern.

How to apply (user): rebuild ChannelMaster Debug x64 and re-run 20 TUNE cycles.

Next validation (Run 9):

- Rebuild ChannelMaster Debug x64.
- Run 20 TUNE-only cycles at UI drive 1, 2-3 seconds on/off.
- Record clean / slight / moderate / heavy per attempt.
- Compare raspy count vs Run 8.
  - If Run 9 shows raspy drop significantly (e.g. <=3 of 20): logger-induced timing jitter was the root cause (or a major contributor).
  - If Run 9 matches Run 8 (~6 raspy): logger is not the cause; move to Part 2 (gate per-sample sqrt diagnostics) and/or Phase B (wire capture + spectral diagnosis).
- Also check the log: `sdr_log: dropped N messages` lines would indicate the ring is undersized (need to grow) — shouldn't normally happen.

## Contributing Factor: High CPU Load (parked, 2026-04-14)

User raised on 2026-04-14 that this custom Thetis build is running at ~36 % CPU in normal operation. The upstream Thetis runs at roughly 7-10 % CPU in comparable conditions, and even at that baseline upstream users see occasional ADC errors on the ANAN family when a background process steals cycles. A 3-4x CPU overhead on this SUNSDR build could plausibly be causing intermittent timing problems: late TX packet emissions, late keepalive ticks, late RX silence injection, late PTT state transitions. Any of those could surface as raspy TX, raspy RX, or dropped audio that looks random to our instrumentation.

Not acting on this now — the current investigation line (PS-A disable in Phase A, spectral capture in Phase B, targeted flush in Phase C) stays. But it is explicitly ON THE LIST for later as both a detection target and an optimization target.

Future work ideas to action later:

- **Detect**: add a lightweight per-loop timing monitor in `SunSDRReadThread` that records wall-clock gaps between RX packet arrivals, keepalive ticks, and TX audio callbacks. If any gap exceeds its expected period by more than a threshold (e.g. 2 × expected), increment a counter and log once per second. Surface the counter in the existing `IQ status` log line so we can correlate clean vs raspy attempts against scheduler hiccups.
- **Detect**: log OS-level process CPU% and thread priority at startup and periodically during TX sessions. If CPU% > 50 or the IQ read thread isn't at `ABOVE_NORMAL`/`HIGH` priority, warn in the log.
- **Reduce**: audit hot paths for allocations or debug prints that could spike CPU during TX. `sdr_logf` in particular runs synchronously with `fprintf`+`fflush` on every log line — heavy logging during a TUNE test would slow the read thread. Consider a ring-buffered deferred logger.
- **Reduce**: profile where the 3-4x overhead comes from. Candidates: the xrouter silence injection doing 1562 calls/sec, the TX feed keepalive doing 750 Inbound calls/sec, the instrumented TX audio callback doing per-sample math (sqrt + boxcar), telemetry parsing, and the managed debug side of the TUNE handoff.
- **Alert**: when the debug logger detects a timing anomaly, include a marker in the TX attempt summary so the user can see it without grepping.

## 2026-04-14 Timing Hardening — Tier 1 (bundled with async logger for Run 9)

Evidence review: Run 8's `fdGapMax=16.000 ms` on every TUNE attempt — clean and raspy alike — pins a 16 ms stall event per attempt regardless of outcome. That is exactly the Windows default scheduler tick (~15.6 ms). The raspy vs clean distinction is then explained by **where** each 16 ms stall lands relative to the `rx_silence_accum` / `tx_feed_accum` state: some stalls produce a clean catch-up, others straddle a WDSP block boundary and produce a 2-3x burst silence injection that phase-shifts the rmatch resampler feeding TXA. That is the gradient-severity mechanism.

An audit of `sunsdr.c` confirmed:
- `timeBeginPeriod(1)` is never called — process runs at the 15.6 ms default tick.
- RX read thread at NORMAL priority (sunsdr.c:1867), competes with UI/background.
- RX silence injection uses `GetTickCount64()`-based `elapsed_ms` — inherits the scheduler-tick resolution.
- No `elapsed_ms` clamp — a stall of N ms yields N ms of catch-up injection.

### Changes applied (uncommitted as of this entry)

In `Project Files/Source/ChannelMaster/sunsdr.c`:

1. **`#include <mmsystem.h>` + `#pragma comment(lib, "winmm.lib")`** at the top of the file — no `ChannelMaster.vcxproj` edit needed.
2. **`timeBeginPeriod(1)` at top of `SunSDRInit`**, paired with **`timeEndPeriod(1)` in `SunSDRDestroy`** before async-logger shutdown. Logs `TIMING_INIT: timeBeginPeriod(1) rc=<code> qpcFreq=<hz>` and `TIMING_DEINIT: timeEndPeriod(1) called`. Drops process-wide scheduler tick from ~15.6 ms to ~1 ms.
3. **RX read thread priority**: after `CreateThread` for `SunSDRReadThread`, call `SetThreadPriority(..., THREAD_PRIORITY_ABOVE_NORMAL)`. Logs `TIMING_INIT: RX thread priority set rc=<ok> readback=<val>`.
4. **QPC-based pacing inside `SunSDRReadThread`**: replaced `GetTickCount64()`-driven `elapsed_ms` with `QueryPerformanceCounter`/`QueryPerformanceFrequency`. Retired `last_service_tick` / `now_tick` locals, replaced with `last_service_qpc` / `now_qpc` (LARGE_INTEGER). Logs `TIMING_INIT: RX loop QPC pacing qpcFreq=<hz> clampMs=<val> expectedPeriodMs=<val>` once at thread start.
5. **`elapsed_ms` clamp**: `elapsed_clamp_ms = 2.0 ms` (2x `expected_period_ms` of 1 ms). When `elapsed_ms > clamp`, replace with `expected_period_ms` and increment `dbg_elapsed_clamps`. Emits `TIMING_CLAMP: elapsed_ms clamped N times in last 1s (total=M). Loop stalled > 2.0 ms.` once per second, only when clamps occurred. Under-feeding WDSP by one block is benign; over-feeding bursts are what drive the raspy.

### Why bundle with the async logger

Per user decision 2026-04-14, Tier 1 is shipped **together** with the async logger (already landed) in a single Run 9 test. Trade-off accepted: if Run 9 is clean, attribution between the two changes is ambiguous. Acceptable to get to a result faster. If Run 9 is still raspy, Tier 2 (gap histogram, process priority, power request) and Phase B (spectral capture) are the fallbacks.

### Run 9 verification (user)

1. Rebuild ChannelMaster + Thetis Debug x64.
2. Start Thetis with SUNSDR2 DX selected. In `sunsdr_debug.log` confirm four `TIMING_INIT` lines appear:
   - `TIMING_INIT: timeBeginPeriod(1) rc=0 qpcFreq=<nonzero>`
   - `TIMING_INIT: RX thread priority set rc=1 readback=1`
   - `TIMING_INIT: RX loop QPC pacing qpcFreq=<nonzero> clampMs=2.000 expectedPeriodMs=1.000`
3. Confirm no `sdr_log: dropped N messages` lines.
4. Run 20 TUNE-only cycles at UI drive 1 (rawDrive 30), 2-3 s on / 2-3 s off. Record clean / slight / moderate / heavy per attempt.
5. After the test, inspect `TX_ATTEMPT_END` lines per attempt — compare `fdGapMax` distribution vs Run 8 baseline (16.000 ms on every attempt). Expect a large drop.
6. Count `TIMING_CLAMP` occurrences across the 20-cycle window. Heavy clamping means there's a stall source beyond the scheduler tick — itself a useful finding.
7. Update this file with **Run 9** results.

### Run 9 decision matrix

| Outcome | Interpretation | Next |
|---|---|---|
| Clean >= 18/20, fdGapMax << 16 ms | Timing was the root cause. Attribution ambiguous between async logger and Tier 1, but acceptable. | Close investigation; optionally revert async logger briefly to confirm attribution. |
| Clean improved vs Run 8 but not resolved | Timing is contributory but not sole driver. | Proceed to Tier 2 (gap histogram, process priority, power request), then Phase B (spectral capture). |
| No improvement, fdGapMax still ~16 ms | timeBeginPeriod(1) did not take effect, or scheduler wakes are gated elsewhere. | Check TIMING_INIT log. Add Tier 2 gap histogram to locate the stall. |
| Worse than Run 8 | Regression: thread priority or clamp introduced new jitter. | Revert Tier 1 items one at a time to narrow. |

## 2026-04-14 Run 9 result + Tier 1 clamp regression + fix (Run 9b)

User ran Run 9 after rebuilding with the bundled async logger + Tier 1 timing hardening. **Result: consistent raspy on ALL 20+ attempts**, worse than Run 8 (6/20 raspy).

Critical new observation (user): VAC panel showed **VAC1 Out Underflow** and **VAC1 In Overflow** counters climbing steadily during TUNE only — not during RX, not during normal TX voice, only TUNE.

### Root cause of the Run 9 regression

The `elapsed_ms` clamp at 2 ms that I added to Tier 1 item #4 was incorrectly calibrated.

The RX read loop is paced by radio packet arrival via `recv()`:
- **RX mode**: packets every ~640 us → `elapsed_ms ≈ 0.64 ms` → below 2 ms clamp → not clamped.
- **TX/TUNE mode**: radio throttles to ~195 pkts/sec → packets every **~5.12 ms** → above 2 ms clamp → **clamp fires every single iteration**.

Result: during TX the `elapsed_ms` value passed into the silence accumulators was always replaced with 1 ms instead of the real ~5.12 ms. That under-fed both accumulators by a factor of ~5:

- **`rx_silence_accum`** targets 1562.5 buffers/sec. Actual feed during TX collapsed to ~305/sec. WDSP RX was starved of input → VAC1 Out (RX→speakers) underflowed steadily, and the rmatch resampler, seeing severely under-paced input, corrupted the audio → **raspy on-air on EVERY attempt**.
- **`tx_feed_accum`** drives `Inbound(tx_stream, ...)` which causes `cm_main` to call `xvacIN()` draining VAC input. Under-calling by 5× meant VAC1 IN filled faster than it drained → **VAC1 In overflow counter climbing**.

Both user-visible symptoms (consistent raspy, climbing VAC counters, TUNE-only) are predicted by this single bug. The design intent of Tier 1 was correct but the threshold was wrong — I treated normal TX cadence as a stall.

### Fix (Run 9b)

In `sunsdr.c`:

- Raised `elapsed_clamp_ms` from **2.0 ms → 50.0 ms**. Catches only pathological stalls (scheduler preemption events far beyond the normal 5.12 ms TX cadence and the 16 ms scheduler tick).
- Introduced `elapsed_replace_ms = 10.0 ms` as the sentinel replacement. Removed `expected_period_ms`.
- The existing `emitted < 16` cap inside the silence-injection while-loop already bounds any one iteration's burst, so the clamp is purely a safety net for rare pathological events.
- Updated the `TIMING_INIT` log line: `clampMs=50.0 replaceMs=10.0`.

Additionally added **`VAC1 status`** line to the per-second telemetry so we can track `outUnder`, `outOver`, `outRing`, `inUnder`, `inOver`, `inRing` against PTT/TUNE state going forward. Uses existing `getIVACdiags(0, type, ...)` API.

### Run 9b verification

1. Rebuild ChannelMaster + Thetis Debug x64.
2. Confirm `TIMING_INIT` line shows `clampMs=50.0 replaceMs=10.0`.
3. With radio running in RX, confirm `VAC1 status` lines show `outUnder=0 outOver=0 inUnder=0 inOver=0` (or at least stable, not monotonically climbing).
4. Run 20 TUNE cycles at UI drive 1, 2-3 s on / 2-3 s off. Per cycle record clean / slight / moderate / heavy.
5. Per cycle, also record the delta in VAC1 outUnder and VAC1 inOver across the TUNE-on window (log delta before/after).
6. `TIMING_CLAMP` log lines should be absent or very rare (once per TUNE maximum).

### Run 9b decision matrix

| Outcome | Interpretation | Next |
|---|---|---|
| Clean >= 18/20, VAC1 counters stable during TUNE | Clamp fix restored correct silence pacing; the 2 ms clamp was the sole cause of Run 9 regression. Async logger + timeBeginPeriod + ABOVE_NORMAL + QPC alone are safe. Raspy may or may not still occur at Run 8 rate. | If raspy still 5-6/20, proceed to Tier 2 / Phase B. If fully clean, close. |
| Raspy rate returns to Run 8 level (~6/20), VAC1 counters stable | Clamp was the extra damage but the original 16 ms scheduler-tick hypothesis still stands. | Proceed to Tier 2 (gap histogram) or Phase B (spectral). |
| Raspy still present AND VAC1 counters still climbing | There's a second bug. The QPC switch, the thread-priority bump, or the timeBeginPeriod is interacting with something unexpected. | Diff Tier 1 items one at a time to narrow. First suspect: are xrouter's internal consumers actually keeping up at the correct rate now, or did QPC's sub-us `elapsed_ms` expose a math precision edge case in the accumulators? |

## Next Step

Rebuild ChannelMaster + Thetis Debug x64 and execute the **Run 9b** verification protocol above. Update this document with results.

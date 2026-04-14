# SunSDR Raspy TX Debug Tracker

Date started: 2026-04-14

## Problem

Intermittent raspy tone/audio during SunSDR TUNE and MOX/TX. Current report is roughly 50-60% occurrence. Working hypothesis: a stream timing or state-transition sync issue around active TX entry, TX keepalive, and RX silence padding.

## Current Status

- Status: fourth TUNE-cycle data analyzed; TX pre-prime made TUNE results worse and has been removed. Audio-layer diagnostics have been added for the next TUNE run.
- Primary file: `Project Files/Source/ChannelMaster/sunsdr.c`
- First reproduction target: repeated TUNE on/off cycles, 2-3 seconds on and 2-3 seconds off.
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
- Current phase: diagnostics are now expanded above `sunsdr.c` stream timing into TX callback sample health and VAC/ASIO/rmatch counters. If these do not correlate, the next layer is managed Thetis TUNE generator state and TX monitor/RX path state around each TUNE attempt.

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

Next validation:

- Rebuild `ChannelMaster` Debug x64.
- Repeat the same 20 TUNE cycles and record clean/raspy attempts.
- Confirm `TX_PREPRIME_FD` is no longer present.
- Confirm `firstFdBefore0x06=0`, `feDuringTx=0`, `keepaliveRaces=0`, and `seqGaps=0` remain clean with the TX IQ gate.
- Keep checking `rxSilence`, `xrouterTotal`, and `rxAccum` to ensure the RX/VAC underfeed fix remains intact.
- Compare the new `txCb*` and `TX_AUDIO_DIAG` fields between raspy and clean attempts before attempting another behavior change.

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

## Next Step

Run the repeated 20-cycle TUNE test and update this file with:

- number of attempts
- clean/raspy classification for each attempt
- key stream log correlations: `firstFdBefore0x06`, `feDuringTx`, `keepaliveRaces`, `seqGaps`, `rxSilence`, `xrouterTotal`, `rxAccum`
- key audio log correlations: `txCb*` fields and `TX_AUDIO_DIAG BEGIN/END` VAC/ASIO/rmatch counter deltas
- the next chosen fix candidate

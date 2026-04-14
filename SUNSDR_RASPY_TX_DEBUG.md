# SunSDR Raspy TX Debug Tracker

Date started: 2026-04-14

## Problem

Intermittent raspy tone/audio during SunSDR TUNE and MOX/TX. Current report is roughly 50-60% occurrence. Working hypothesis: a stream timing or state-transition sync issue around active TX entry, TX keepalive, and RX silence padding.

## Current Status

- Status: first TUNE-cycle data analyzed; RX silence injection cap raised for next validation run.
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

## Current Hypotheses

- 2026-04-14 TUNE data did not support the idle keepalive race as the active failure: attempts 1-20 all ended with `feDuringTx=0`, `keepaliveRaces=0`, and `seqGaps=0`.
- TX packet cadence also looked stable in the first run: `fdGapAvg` was about 5.10-5.12 ms and `fdGapMax` was 16 ms.
- Strong current candidate: RX silence injection is underfeeding during TX. The read loop is paced by incoming radio TX packets at about 195/sec, and the old `emitted < 4` safety cap limited silence injection to roughly 780-976 buffers/sec instead of the intended 1562.5/sec. Logs showed `rxAccum` growing continuously during TX into the thousands, matching RX/VAC raspiness and VAC overflow reports.
- If the next run still rasps while `rxSilence` rises to the target rate and `rxAccum` stays bounded, revisit first-packet timing and consider pre-priming one active `0xFD` silence packet around `0x06=1`.
- If none correlate, expand diagnostics into VAC/ASIO and Thetis TX audio routing.

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

Next validation:

- Rebuild `ChannelMaster` Debug x64.
- Repeat the same 20 TUNE cycles and record clean/raspy attempts.
- Compare `rxSilence`, `xrouterTotal`, and `rxAccum` against Run 1. Expected improvement is `rxSilence` near the target feed rate during TX and `rxAccum` staying bounded instead of growing by thousands.

## Verification So Far

- `git diff --check -- Project Files/Source/ChannelMaster/sunsdr.c`: passed.
- `MSBuild ChannelMaster.vcxproj /t:ClCompile /p:SelectedFiles=sunsdr.c`: passed with 0 warnings and 0 errors.
- Full `ChannelMaster` link from this invocation failed because `PA19.lib` is searched under `Project Files/Source/build/x64/Debug`, while the existing dependency libs are under `Project Files/build/x64/Debug`.
- After the RX silence cap change, `MSBuild ChannelMaster.vcxproj /t:ClCompile /p:SelectedFiles=sunsdr.c /p:Configuration=Debug /p:Platform=x64`: passed with 0 warnings and 0 errors.

## Next Step

Run the repeated TUNE-cycle test and update this file with:

- number of attempts
- clean/raspy classification for each attempt
- key log correlations
- the next chosen fix candidate

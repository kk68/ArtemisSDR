# Thetis-SunSDR: SunSDR2 DX Native Protocol Support

Native SunSDR2 DX integration for the [Thetis](https://github.com/ramdor/Thetis) SDR application, built through clean-room protocol reverse engineering.

## Status: Core RX/TX/PA Working

The current fork supports practical operation on SunSDR2 DX:

- **Native control and streaming** on UDP `50001` and `50002`
- **RX1 working** with correct panadapter, waterfall, and demodulated audio
- **RX2 working** as a second receive path with independent VFO B tuning and audio
- **MOX/PTT working** with native SunSDR TX streaming
- **TUNE working** through the normal Thetis TX/tone path
- **Drive / Tune power control working** from Thetis sliders, but still under active calibration/tuning
- **RX antenna switching working**
- **TX antenna switching working** through Thetis antenna setup controls
- **PA / xPA control working** from Thetis
- **Power off/on recovery working** without losing the receive stream
- **RX band switching currently working via a temporary SunSDR-only auto power recycle workaround**
- **TX forward power meter working** via `0x1F` telemetry packet (bytes 14-15 → quadratic conversion to watts, calibrated on 40m)
- **VAC TO VAC underflows during MOX/TX largely resolved** via RX silence injection (radio reduces RX rate during TX, blocking WDSP RX with `bfo=1`)

This is no longer just an RX-only bring-up. It is a usable SunSDR2 DX port with some remaining feature gaps and one major remaining calibration item: TX power/drive linearity.

## Key Protocol Facts

| Parameter | Value |
|-----------|-------|
| Control port | UDP 50001 |
| IQ stream port | UDP 50002 |
| IQ format | 24-bit signed LE, interleaved I/Q |
| Native sample rate | 312,500 Hz |
| Samples per packet | 200 (1210 bytes: 10-byte header + 1200-byte payload) |
| Frequency encoding | Opcode 0x09 (primary) / 0x08 (DDC companion), u64 LE scaled by 10 Hz |
| PTT/MOX | Opcode 0x06, u32: 1=TX, 0=RX |
| Power on | Captured macro sequence (29 steps) |
| Stream keepalive | Bidirectional IQ — send silent TX packets or radio disconnects at ~8s |
| RX antenna selector | Opcode `0x15`, selector `0x01` = primary, `0x03` = secondary RX |
| TX antenna selector | Opcode `0x15`, selector `0x01` = primary, `0x02` = secondary TX |
| PA / xPA selector | Opcode `0x24`, u32: `1` = PA on, `0` = PA off |

## Architecture

```
SunSDR2 DX                              Thetis (this fork)
+-----------+     UDP 50001              +------------------+
|  Control  |<-------------------------->| sunsdr.c         |
|  Port     |  power-on macro, freq,     | (protocol impl)  |
|           |  mode, PTT commands        |                  |
+-----------+                            +------------------+
+-----------+     UDP 50002              +------------------+
|  IQ       |<-------------------------->| SunSDRReadThread  |
|  Stream   |  RX: 1210-byte IQ pkts    | resample 312k→384k|
|           |  TX: silence keepalive     | → xrouter → WDSP |
+-----------+                            +------------------+
                                                  |
                                         +--------v---------+
                                         | WDSP (fexchange0) |
                                         | demodulation      |
                                         +--------+---------+
                                                  |
                                         +--------v---------+
                                         | VAC mixer → PA    |
                                         | → audio output    |
                                         +------------------+
```

## Implemented Control Surface

- **RX tuning**
  - `0x09` primary frequency write
  - `0x08` companion writes for RX contexts
- **TX tuning**
  - standalone `0x09` split/TX frequency write
- **Mode control**
  - native SunSDR mode writes mapped from Thetis mode changes
- **RX2**
  - native RX2 enable/disable path
  - independent RX2 / VFO B tuning context
- **Transmit**
  - `0x06` MOX/PTT
  - `0x20` companion TX/RX state handling
  - continuous `50002` TX stream keepalive and live TX audio packetization
- **Antenna switching**
  - RX antenna selectors `0x01` / `0x03`
  - TX antenna selectors `0x01` / `0x02`
- **PA control**
  - `0x24` PA / external PA enable
- **Power control**
  - Thetis `Drive` / `Tune` sliders feed native SunSDR TX scaling

## xPA Setup

To use the main-window `xPA` button on SunSDR2 DX, Thetis must first have at least one external PA TX pin enabled in:

- `Setup -> OC Control -> HF/VHF/SWL -> Ext PA Control (xPA)`

For a simple HF amplifier test, enabling one `TXPA` pin with `Transmit Pin Action = Mox/Tune/2Tone` is sufficient to surface the `xPA` button and drive native SunSDR PA control during `MOX` and `TUNE`.

## Audio Notes

- Receive audio works.
- TX audio works for normal operation and TUNE.
- The current working path is VAC-centric when no local ASIO output is available.
- Local monitor-oriented behavior such as `MON` and `DUP` is still not fully resolved for SunSDR and should not be treated as complete.

### VAC Underflow Fix During MOX/TX (2026-04-10)

**Problem:** During MOX/TUNE, the Thetis VAC output (`TO VAC`) accumulated thousands of underflows per second, causing degraded audio and unreliable TX. The TX RF path itself worked, but the VAC pipeline starved.

**Root cause:** The SunSDR2 DX radio reduces its RX IQ packet rate from ~1562/sec to ~195/sec when transmitting. The Thetis WDSP RX channels are opened with `bfo=1` (block-for-output), so `fexchange0` blocks on `WaitForSingleObject(Sem_OutReady, INFINITE)` waiting for output that WDSP cannot produce fast enough at the reduced input rate. This blocks the `cm_main` thread, which starves the VAC mixer Input 0, which starves `rmatchOUT`, which causes the PortAudio output callback to underflow.

**Fix:** The SunSDR IQ read thread now injects silence buffers into `xrouter` during TX whenever a real RX packet hasn't arrived for >2ms. This keeps the WDSP RX input rate at the expected 384k samples/sec, so `fexchange0` doesn't block, `cm_main` keeps running, and the VAC mixer keeps producing output.

Implementation: `ChannelMaster/sunsdr.c` `SunSDRReadThread()` — `last_rx_pkt_tick` tracking + silence injection loop, capped at 32 buffers per gap to prevent runaway.

## RX Band Switching Status

SunSDR RX band switching is currently in a **workable but not final** state.

What is true now:

- live `RX1` band changes can leave the SunSDR receive stream in a bad state
- the user-discovered workaround was:
  - switch band
  - click `POWER` off/on
  - RX recovers
- the current fork now performs that same recovery automatically for SunSDR on a real `RX1` band change when not transmitting

Why this exists:

- logs show the correct new-band RX values are being sent after the band change
- logs also show the radio/stream still needs a native restart to recover cleanly
- so the remaining bug is not “wrong final frequency/mode/antenna,” but “stream/session needs a lighter-weight recovery we have not yet mapped”

Current implementation note:

- the workaround is implemented in:
  - `Console/console.cs` via a guarded SunSDR-only automatic `POWER` recycle on `RX1` band changes

This should be treated as a temporary operational fix, not the final protocol solution.

### Next RE Step For RX Band Switching

The next step is to replace the workaround with the exact native SunSDR band-switch sequence used by ExpertSDR3.

Recommended focused captures:

1. `40m -> 20m` RX-only band switch
2. `20m -> 40m` RX-only band switch

Capture constraints:

- no `MOX`
- no `TUNE`
- no antenna changes during the action window
- keep RX2 state fixed

Goal:

- identify whether ExpertSDR3 sends:
  - a cleaner ordered RX reconfigure bundle
  - or an explicit stream/session re-arm command family

Only after that should the current automatic power recycle be removed.

## TX Power Calibration Status

The SunSDR2 DX TX power path is now structurally working and close enough to move from code bring-up into actual Thetis-side calibration.

What is already true:

- `Drive` and `Tune` sliders now reach the native SunSDR TX path correctly
- `MOX` and `TUNE` both produce RF output
- the previous hard failures are fixed:
  - zero-drive muting / full-scale fallback confusion
  - band-change TX/TUNE failure until `POWER` off/on
  - PA / xPA not asserting during active TX
- Thetis `PA Gain By Band` and per-band `Offset` values are now in the active SunSDR TX power path

Current live 40m reference checkpoints from external wattmeter testing:

- `0 -> 0W`
- `10 -> 9.4W`
- `25 -> 26W`
- `50 -> 51W`
- `75 -> 70W`
- `100 -> 106W`

Interpretation:

- low and mid power are now close enough to be calibrated with normal Thetis tooling
- the remaining error is mainly upper-range shaping (`75W` low while `100W` is near full output)
- this is no longer an RX/TX transport bug hunt; it is calibration work

### What Thetis Calibration Menus Mean For SunSDR

For SunSDR2 DX, Thetis now uses the normal PA calibration path to determine actual TX drive:

- `PA Gain By Band`
- `Offset for <band>`
- `Use watts on Drive/Tune slider`
- `Actual Power @ 100% slider`

Those settings do affect real transmitted power on SunSDR.

The `Watt Meter` page is different:

- it trims Thetis's displayed TX wattmeter
- it does **not** directly set actual RF output

### Important Current Limitation: TX Metering

The Thetis `Fwd Pwr` TX meter is **not currently valid for SunSDR2 DX**.

The current SunSDR integration does not yet feed an equivalent forward/reverse power telemetry path into Thetis's `alex_fwd` / `alex_rev` / `calfwdpower` model, so the TX meter can remain at `0W` even when real RF output is present.

That means:

- use an **external wattmeter** as the source of truth for SunSDR power calibration
- do **not** rely on Thetis `Fwd Pwr` or `Watt Meter Trim` yet

### Current Implementation Notes

- Thetis calibrated power flows through:
  - `Audio.RadioVolume -> NetworkIO.SetOutputPower() -> nativeSunSDRSetDrive()`
- SunSDR-specific tuning currently exists in:
  - `Console/clsHardwareSpecific.cs` — default `PA Gain By Band`
  - `Console/setup.cs` — SunSDR legacy-profile compatibility fallbacks and default per-drive shaping
  - `ChannelMaster/sunsdr.c` — native TX amplitude scaling (`sunsdr_tx_outbound`)
- `sunsdr_debug.log` logs:
  - `SunSDRSetDrive(raw)`
  - `TX audio callback ... drive=..., full_scale=..., pre_peak=..., pre_rms=..., post_peak=..., post_rms=...`

### Recommended Next Steps

1. Freeze the native SunSDR TX path unless a new structural bug appears.
2. Calibrate one band at a time, starting with `40m`, using an external wattmeter only.
3. Use `PA Gain By Band` for coarse per-band correction.
4. Use the `Offset for <band>` table to straighten `10W..90W` checkpoints.
5. After the RF curve is acceptable, implement SunSDR forward/reverse power telemetry so Thetis `Fwd Pwr` and `Watt Meter Trim` become meaningful.

## Files Changed (from upstream Thetis)

**New files:**
- `ChannelMaster/sunsdr.c` — Complete SunSDR native protocol implementation
- `ChannelMaster/sunsdr.h` — Protocol constants, state structure, API

**Modified files:**
- `ChannelMaster/network.c`, `network.h` — P/Invoke wrappers for SunSDR functions
- `ChannelMaster/netInterface.c` — SunSDR path in `StartAudioNative()`
- `ChannelMaster/cmasio.c`, `ivac.c` — Diagnostic logging
- `ChannelMaster/ChannelMaster.vcxproj` — Added sunsdr.c/h to build
- `Console/enums.cs` — `SUNSDR2DX` model and `SUNSDR` protocol enums
- `Console/clsHardwareSpecific.cs` — SunSDR hardware model support
- `Console/HPSDR/NetworkIO.cs`, `NetworkIOImports.cs` — SunSDR init/freq routing
- `Console/cmaster.cs` — SunSDR router configuration and source routing
- `Console/console.cs` — RX2, TX, drive, and native SunSDR control wiring
- `Console/HPSDR/Alex.cs` — SunSDR antenna control integration
- `Console/setup.cs`, `setup.designer.cs` — SunSDR in radio model dropdown and antenna setup integration
- `*.vcxproj` (portaudio, wdsp, cmASIO, ChannelMaster) — PlatformToolset v145 → v143

## Known Limitations

- **Diversity is not supported on SunSDR2 DX in this fork.**
  - Thetis diversity UI can be surfaced, but ExpertSDR3 evidence shows RX2 follows the same RX antenna selection as RX1.
  - No independent per-receiver antenna/input assignment has been recovered yet.
- **Monitor/DUP path is incomplete.**
  - `MON` and `DUP` behavior is not yet reliable on SunSDR.
- **VAC behavior still needs cleanup.**
  - Some RX/TX transitions can still leave VAC in a bad state depending on configuration.
- **TX power calibration is not finished.**
  - Drive/Tune power now works, but real RF output still needs per-band and per-drive calibration tuning.
- **TX antenna behavior depends on normal Thetis “do not TX” / antenna-permit settings.**

## Next Steps

- [ ] Stabilize VAC behavior across RX/MOX/TUNE transitions
- [ ] Finish local monitor (`MON`) and `DUP` path behavior
- [ ] Finish TX drive / tune power calibration, starting with 40m reference measurements
- [ ] Replace the temporary SunSDR RX band-switch auto power recycle with the exact ExpertSDR3 reconfigure sequence
- [ ] Keep validating mode/filter edge cases
- [ ] Continue RE for any hidden per-receiver input routing that could make diversity possible

## Related Repositories

- **Protocol RE docs**: [kk68/ExploringSUN](https://github.com/kk68/ExploringSUN) — Clean-room reverse engineering documentation, Python tools, capture scripts
- **Upstream Thetis**: [ramdor/Thetis](https://github.com/ramdor/Thetis) — Original Thetis SDR application (archived)

## Building

1. Open `Project Files/Source/Thetis_VS2026.sln` in Visual Studio 2022
2. Set configuration to **Debug | x64**
3. Build → Rebuild Solution
4. Select `SunSDR2-DX` as radio model in Setup → General

Requires Visual Studio 2022 with C++ desktop development workload (v143 toolset).

## License

This project inherits the GNU General Public License v2 from upstream Thetis. See [LICENSE](LICENSE) for details.

Protocol implementation (`sunsdr.c`, `sunsdr.h`) is original work derived from clean-room reverse engineering of owned hardware.

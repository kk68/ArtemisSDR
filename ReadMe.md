# Thetis-SunSDR: SunSDR2 DX Native Protocol Support

Native SunSDR2 DX integration for the [Thetis](https://github.com/ramdor/Thetis) SDR application, built through clean-room protocol reverse engineering.

New-user setup guide:

- [START_HERE_SUNSDR2DX.md](START_HERE_SUNSDR2DX.md)

## Status: Core RX/TX/PA Working — 40m TX Power Calibrated

The current fork supports practical operation on SunSDR2 DX:

- **Native control and streaming** on UDP `50001` and `50002`
- **RX1 working** with correct panadapter, waterfall, and demodulated audio
- **RX2 working** as a second receive path with independent VFO B tuning and audio
- **MOX/PTT working** with native SunSDR TX streaming, reliable across consecutive attempts (`txSeq=0` reset per TX session matches EESDR)
- **TUNE working** through the normal Thetis TX/tone path
- **TX power calibrated linearly on 40m**: UI slider in watts maps to actual RF output within ~1 W from 10 W to 90 W; UI 100 → ~97 W (radio PA ceiling); UI 5 → ~4 W (PA low-drive dead-zone, hardware). Other bands fall back to the 40m table until separately measured
- **RX antenna switching working**
- **TX antenna switching working** through Thetis antenna setup controls
- **PA / xPA control working** from Thetis
- **AM transmit working** with WDSP AM modulator, AM Carrier Level UI active end-to-end
- **Power off/on recovery working** without losing the receive stream
- **RX band switching currently working via a temporary SunSDR-only auto power recycle workaround**
- **Radio firmware version display working** in the Thetis title bar and `Setup -> HW Select`
- **Radio serial number display working** in the Thetis title bar and `Setup -> HW Select`
- **TX forward power meter `0x1F` telemetry decoded** in `sunsdr.c` but **not yet wired into Thetis's `Fwd Pwr` UI meter** — use an external wattmeter for now
- **VAC TO VAC underflows during MOX/TX largely resolved** via RX silence injection (radio reduces RX rate during TX, blocking WDSP RX with `bfo=1`)
- **Diagnostic logging disabled by default** (`SUNSDR_DEBUG_LOG_ENABLED 0` in `sunsdr.c`); flip to 1 only when reproducing a TX/RX issue

This is no longer just an RX-only bring-up. It is a usable SunSDR2 DX port with the major remaining items being per-band TX cal extension, `Fwd Pwr` meter wiring, and MON/DUP behavior cleanup.

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

## Identity / Version Display

The current SunSDR2 DX integration now surfaces radio identity directly in Thetis:

- the main title bar shows firmware and, when available, serial number
- `Setup -> HW Select` shows the same live firmware version and serial-aware radio row

Current capture-backed sources:

- **Firmware version** comes from the ExpertSDR3 Firmware Manager style `0x1A` query/reply
  - request: `32ff1a000000000000000100000000000000`
  - reply example: `32ff1a002000000000000100000000000000ee0002005800080020210000e4070c0330c22f4148020000d70900000b000000`
  - displayed firmware: `88.8` from reply bytes `22` and `24`

- **Serial number** is currently inferred from the same `0x1A` reply and the observed identity packet family
  - year token: bytes `26-27` = `20 21` -> `2021`
  - unit suffix: bytes `38-39` = `48 02` -> `584`
  - current SunSDR2 DX serial format used in Thetis: `EED06` + `2021` + `00584` = `EED06202100584`

Important note:

- firmware is directly packet-backed
- the `EED06` serial prefix is still treated as a model-family inference derived from ExpertSDR3 behavior and user validation, not yet from a separately decoded on-wire field

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
  - mode is set by the radio via the `0x20` config block payload (NOT a dedicated mode opcode). Earlier `0x17` mode-code mapping was a misread — `0x17` is the drive command (sqrt-encoded watts). See `sunsdr-re/docs/protocol/control.md` for the corrected wire semantics.
  - Thetis-side mode changes are cached locally; `0x17` is no longer sent with a mode value
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

40m is calibrated and locked. Per-band extension and a couple of metering items remain.

### Architecture (final)

EESDR-compatible topology — wire IQ at constant full amplitude, drive byte `0x17` is the only power dial:

- `sunsdr_tx_outbound` multiplies WDSP TX output by a constant `iq_gain = 1.0 / 0.857 ≈ 1.167` so the wire IQ peaks at ~1.0 (matches EESDR captures at every drive level).
- `sunsdr_drive_raw_to_wire_byte` is a pass-through. Thetis upstream owns the calibration math: `target_dbm → GainByBand (per-band PA Gain + GetSunSDRDefaultAdjust offsets) → target_volts → raw 0..255 → wire byte`.
- `setup.cs` `GetSunSDRDefaultAdjust` holds the SunSDR-specific per-drive-level dB offsets that fall back when `PA Gain By Band` is left at its uninitialized default of 100 dB. Operator can override by entering real PA Gain dB and using the per-drive `Offset for <band>` UI fields.

### 40m measured curve (locked)

| UI W | Actual W | Note |
| ---: | ---: | --- |
| 5   | 4    | hardware low-drive dead-zone |
| 10  | 9.8  | |
| 15  | 16.3 | |
| 20  | ~20  | post-iter-13 nudge |
| 30  | 30   | exact |
| 40  | 40   | exact |
| 50  | 50   | exact |
| 60  | 60   | exact |
| 70  | 70   | exact |
| 80  | 80   | exact |
| 90  | 91   | |
| 100 | 97   | radio PA ceiling |

`GetSunSDRDefaultAdjust` array (40m fallback): `{ +0.13, -0.55, -0.84, -1.05, -1.13, -1.35, -1.40, -1.41, -1.26 }` dB at 10W..90W.

### Thetis calibration UI (when to use what)

- `Setup → PA Settings → PA Gain By Band` — coarse per-band dB. Leave at 100 to use the SunSDR built-in fallback table; set to a real dB value to switch to operator-controlled per-drive offsets.
- `Setup → PA Settings → PA Gain → Offsets for <band>` — per-drive (10W..90W) dB tweaks, ±6 dB range. Active only when `PA Gain By Band` for that band is non-default.
- `Setup → PA Settings → Watt Meter` — trims Thetis's displayed `Fwd Pwr` meter only. Does NOT change RF output.
- `Setup → General → Use watts on Drive/Tune slider` — must be on for the slider to read in watts.

### Current limitation: Fwd Pwr meter not wired

The radio sends forward power / SWR telemetry on the `0x1F` family every TX cycle. `sunsdr.c` decodes it but does **not** yet route it into Thetis's `alex_fwd` / `alex_rev` / `calfwdpower` path, so the on-screen `Fwd Pwr` meter stays near 0 W during TX. Use an external wattmeter while this is wired up.

### Next steps for power

1. Calibrate the other bands (80m, 20m, 17m, 15m, 12m, 10m, 6m). Same workflow as 40m: one 12-point sweep, fold deltas into the relevant band's adjust table or into per-band UI offsets.
2. Wire the `0x1F` forward-power telemetry into Thetis's `Fwd Pwr` meter so on-screen power matches real output.
3. Consider implementing reverse-power / SWR readback once `Fwd Pwr` is live.

## Files Changed (from upstream Thetis)

**New files:**
- `ChannelMaster/sunsdr.c` — Complete SunSDR native protocol implementation
- `ChannelMaster/sunsdr.h` — Protocol constants, state structure, API

**Modified files:**
- `ChannelMaster/network.c`, `network.h` — P/Invoke wrappers for SunSDR functions
- `ChannelMaster/netInterface.c` — SunSDR path in `StartAudioNative()` (legacy `sunsdr_trace` writer disabled)
- `ChannelMaster/cmasio.c`, `ivac.c` — Diagnostic logging
- `ChannelMaster/ChannelMaster.vcxproj` — Added sunsdr.c/h to build
- `Console/enums.cs` — `SUNSDR2DX` model and `SUNSDR` protocol enums
- `Console/clsHardwareSpecific.cs` — SunSDR hardware model support, default `PA Gain By Band`
- `Console/HPSDR/NetworkIO.cs`, `NetworkIOImports.cs` — SunSDR init/freq routing, `nativeSunSDRSetDrive`, `nativeSunSDRLogTrace`, etc.
- `Console/cmaster.cs` — SunSDR router configuration and source routing
- `Console/console.cs` — RX2, TX, drive, AM mode, and native SunSDR control wiring
- `Console/HPSDR/Alex.cs` — SunSDR antenna control integration
- `Console/setup.cs`, `setup.designer.cs` — SunSDR in radio model dropdown and antenna setup; SunSDR-specific PA Gain fallbacks and per-drive `GetSunSDRDefaultAdjust` table (40m calibrated)
- `Console/radio.cs` — wires `TXAMCarrierLevel` UI through to WDSP for SunSDR (already standard, kept untouched after diagnostic was removed)
- `*.vcxproj` (portaudio, wdsp, cmASIO, ChannelMaster) — PlatformToolset v145 → v143

## Known Limitations

- **TX power calibration only complete on 40m.** Other bands fall back to the 40m table and may be off by a few dB until separately measured.
- **Thetis `Fwd Pwr` meter not wired** to the SunSDR `0x1F` forward-power telemetry yet. Use an external wattmeter.
- **Diversity is not supported on SunSDR2 DX in this fork.**
  - Thetis diversity UI can be surfaced, but ExpertSDR3 evidence shows RX2 follows the same RX antenna selection as RX1.
  - No independent per-receiver antenna/input assignment has been recovered yet.
- **Monitor/DUP path is incomplete.**
  - `MON` and `DUP` behavior is not yet reliable on SunSDR.
- **VAC behavior still needs cleanup.**
  - Some RX/TX transitions can still leave VAC in a bad state depending on configuration.
- **Intermittent post-TX RX raspy** can occur briefly (related to VAC rmatch transient); cycling VAC clears it. Tracked separately.
- **Mode byte position inside `0x20` config block payload** is still being verified — mode currently set client-side and may not always match ExpertSDR3 exactly.
- **TX antenna behavior depends on normal Thetis "do not TX" / antenna-permit settings.**

## Next Steps

- [ ] Calibrate remaining bands (80m / 60m / 30m / 20m / 17m / 15m / 12m / 10m / 6m)
- [ ] Wire `0x1F` forward-power telemetry into Thetis's `Fwd Pwr` meter
- [ ] Replace the temporary SunSDR RX band-switch auto power recycle with the exact ExpertSDR3 reconfigure sequence
- [ ] Stabilize VAC behavior across RX/MOX/TUNE transitions; fix the post-TX rmatch raspy
- [ ] Finish local monitor (`MON`) and `DUP` path behavior
- [ ] Pin the mode byte offset inside the `0x20` config block payload
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

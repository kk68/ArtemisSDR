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

## TX Power Calibration Status

The SunSDR2 DX TX power path is now structurally working, but its calibration is still being tuned.

What is already true:

- `Drive` and `Tune` sliders now reach the native SunSDR TX path correctly
- `MOX` and `TUNE` both produce RF output
- the previous hard failures are fixed:
  - zero-drive muting / full-scale fallback confusion
  - band-change TX/TUNE failure until `POWER` off/on
  - PA / xPA not asserting during active TX

What is still open:

- output power is not yet correctly linearized against Thetis slider values
- top-end output can saturate early depending on the current calibration pass
- low/mid drive values still need refinement against real on-air wattmeter measurements

Latest observed 40m calibration checkpoints from live testing:

- `0 -> 0W`
- `10 -> 21W`
- `25 -> 62W`
- `50 -> 99W`
- `75 -> 113W`
- `100 -> 113W`

Interpretation:

- the drive path is alive and monotonic
- low/mid values are still too hot
- the top end is still flattening too early

Current implementation notes for the next agent:

- Thetis calibrated power still flows through `Audio.RadioVolume -> NetworkIO.SetOutputPower() -> nativeSunSDRSetDrive()`
- SunSDR-specific tuning currently exists in:
  - `Console/clsHardwareSpecific.cs` — default `PA Gain By Band`
  - `Console/setup.cs` — SunSDR legacy-profile compatibility fallbacks and default low-power compensation
  - `ChannelMaster/sunsdr.c` — native TX amplitude scaling (`sunsdr_tx_outbound`)
- `sunsdr_debug.log` now logs:
  - `SunSDRSetDrive(raw)`
  - `TX audio callback ... drive=..., full_scale=..., pre_peak=..., pre_rms=..., post_peak=..., post_rms=...`

Recommended next calibration workflow:

1. keep testing on one band first, currently `40m`
2. tune the native SunSDR TX full-scale curve in `sunsdr.c`
3. then refine the SunSDR default PA gain / offset shaping in `setup.cs`
4. only after 40m is sane, fan out to other bands

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

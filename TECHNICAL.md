# Thetis-SunSDR — Technical Reference

Engineering-level notes for contributors. User-facing documentation lives in [ReadMe.md](ReadMe.md).

## Architecture

```
SunSDR2 DX                              Thetis (this fork)
+-----------+     UDP 50001              +------------------+
|  Control  |<-------------------------->| sunsdr.c         |
|  Port     |  power-on macro, freq,     | (protocol impl)  |
|           |  mode, PTT, drive commands |                  |
+-----------+                            +------------------+
+-----------+     UDP 50002              +------------------+
|  IQ       |<-------------------------->| SunSDRReadThread |
|  Stream   |  RX: 1210-byte IQ pkts     | resample 312k→   |
|           |  TX: host IQ audio +       | 384k → xrouter   |
|           |       keepalive packets    | → WDSP           |
+-----------+                            +------------------+
                                                  |
                                         +--------v---------+
                                         | WDSP (fexchange0)|
                                         | demod / TXA      |
                                         +--------+---------+
                                                  |
                                         +--------v---------+
                                         | VAC mixer → PA   |
                                         | → audio output   |
                                         +------------------+
```

## Key protocol facts

| Parameter | Value |
|-----------|-------|
| Control port | UDP 50001 |
| IQ stream port | UDP 50002 |
| IQ format | 24-bit signed LE, interleaved Q-first I/Q |
| Native sample rate | 312,500 Hz |
| Samples per packet | 200 (1210 bytes: 10-byte header + 1200-byte payload) |
| Frequency encoding | Opcode 0x09 (primary) / 0x08 (DDC companion), u64 LE scaled by 10 Hz |
| PTT / MOX | Opcode 0x06, u32: 1 = TX, 0 = RX |
| Drive | Opcode 0x17, u32 (byte at offset 18): `round(sqrt(W/100) × 255)` |
| Stream keepalive | Bidirectional — host must send silent TX packets at ~195 pps during RX, or radio disconnects at ~8 s |
| RX antenna selector | Opcode 0x15, selector 0x01 = primary, 0x03 = auxiliary |
| TX antenna selector | Opcode 0x15, selector 0x01 = primary, 0x02 = auxiliary |
| PA / xPA selector | Opcode 0x24, u32: 1 = PA on, 0 = PA off |

Mode is carried inside the 0x20 config-block payload, not in a dedicated mode opcode. (An earlier interpretation of 0x17 as the mode opcode was a misread.)

## Identity / version readback

Thetis surfaces the radio's firmware version (and serial, if decodable) in the title bar and in `Setup → HW Select`.

- **Firmware** comes from a 0x1A query/reply pair:
  - Request: `32 ff 1a 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00`
  - Reply bytes 22 and 24 decode to the displayed firmware version (e.g. `88.8`).
- **Serial number** is reconstructed from the same 0x1A reply plus a model-family prefix inference:
  - Year token: bytes 26–27 (little-endian ASCII-like)
  - Unit suffix: bytes 38–39
  - Full form: `EED06YYYYNNNNN` — the `EED06` prefix is an inference from ExpertSDR3 behavior and community validation, not a separately decoded wire field.

## TX amplitude / drive architecture

The TX path matches the EESDR topology exactly:

1. **Wire IQ amplitude is constant.** `sunsdr_tx_outbound` multiplies the WDSP TX output by `iq_gain = 1.0 / 0.857 ≈ 1.167`, so the wire IQ peaks clip at ~1.0 on every drive setting (WDSP TUNE output peaks around 0.857). This matches EESDR captures at every slider value.
2. **The drive byte (0x17) is the power dial.** `sunsdr_drive_raw_to_wire_byte` is a pass-through — it forwards the integer value that Thetis has already computed through its standard `target_dbm → GainByBand → target_volts → raw` chain to the wire.
3. **Per-band calibration lives in Thetis.** `Console/setup.cs` `GetSunSDRDefaultAdjust` holds a SunSDR-specific dB offset table used when a band's `PA Gain By Band` value is left at the uninitialized default (100). Operators override by entering real PA Gain dB and using the per-drive `Offset for <band>` UI fields (±6 dB range per drive level).

### 40 m measured curve (locked)

| UI W | Actual W | Note |
|---:|---:|---|
| 5 | 4 | hardware low-drive dead-zone |
| 10 | 9.8 | |
| 15 | 16.3 | |
| 20 | ~20 | |
| 30 | 30 | exact |
| 40 | 40 | exact |
| 50 | 50 | exact |
| 60 | 60 | exact |
| 70 | 70 | exact |
| 80 | 80 | exact |
| 90 | 91 | |
| 100 | 97 | radio PA ceiling |

`GetSunSDRDefaultAdjust` 40 m dB table at 10 W..90 W: `{ +0.13, -0.55, -0.84, -1.05, -1.13, -1.35, -1.40, -1.41, -1.26 }`

## TUNE-at-dial in SSB

Thetis's standard SSB TUNE emits a PostGen tone `cw_pitch` Hz offset from baseband (`-cw_pitch` for LSB/DIGL, `+cw_pitch` for USB/DIGU). Upconverted by the radio, this puts RF `cw_pitch` below (LSB) or above (USB) the dial.

For the operator-expected "TUNE tone on the dial" behavior, `NetworkIO.VFOfreq` pre-shifts every SunSDR TX freq write by the inverse of the baseband tone offset whenever TUNE is engaged in SSB mode. The shift state lives in `NetworkIO.SunSDRTuneFreqOffsetHz` and is set/cleared from `HdwMOXChanged`.

AM/FM TUNE uses a different strategy: PostGen is suppressed so the modulator emits a pure carrier at dial (classic AM-radio TUNE behavior). Running PostGen with a +cw_pitch tone in AM would produce carrier + sidebands, which a narrow-mode receiver reads as a spike offset from dial.

## VAC underflow fix during MOX/TX

**Problem**: during MOX/TUNE, the VAC output (`TO VAC`) accumulated thousands of underflows per second, degrading audio and TX reliability.

**Root cause**: the SunSDR2 DX reduces its RX IQ packet rate from ~1562/sec to ~195/sec while transmitting. Thetis's WDSP RX channels run with `bfo=1` (block-for-output), so `fexchange0` blocks indefinitely on `WaitForSingleObject(Sem_OutReady, INFINITE)` waiting for output WDSP can't produce at the reduced input rate. That blocks the `cm_main` thread, starves the VAC mixer Input 0, starves `rmatchOUT`, and the PortAudio output callback underflows.

**Fix**: `SunSDRReadThread` injects silence buffers into `xrouter` during TX whenever a real RX packet hasn't arrived for >2 ms, capped at 32 buffers per gap to prevent runaway. This keeps the WDSP RX input rate at the expected 384 k sample/sec, so `fexchange0` doesn't block, `cm_main` keeps running, and the VAC mixer keeps producing output.

Implementation: `ChannelMaster/sunsdr.c` — `last_rx_pkt_tick` tracking + silence injection loop.

## TX reliability (raspy / zero-RF) investigation

Multi-round investigation in early development produced a set of structural fixes that now give 100 % reliable TX across consecutive attempts. Key discoveries in temporal order:

1. TX wire IQ is Q-first in the 6-byte-per-sample payload (bytes 0–2 = Q, 3–5 = I), symmetric with the RX unpack convention.
2. `0x17` is the drive command (sqrt-encoded watts), not a mode opcode. Mode is carried in the 0x20 config block. Earlier code that sent mode values via 0x17 accidentally set drive to the mode-code integer (e.g. LSB 0xBC = 188 ≈ "54 W equivalent drive"), which gave the illusion of working SSB at weird power levels and kept AM stuck at 2 W.
3. TX IQ packet sequence numbers (`txSeq`) must reset to 0 at every PTT-on. EESDR does this; carrying `txSeq` monotonically across sessions caused the radio to silently drop our FD packets as out-of-order, producing keyed-radio-with-unmodulated-carrier behavior.
4. The WDSP TX callback thread benefits from `THREAD_PRIORITY_ABOVE_NORMAL` self-elevation on first entry, and the TX packet paced emission is decoupled from the WDSP callback onto a dedicated TIME_CRITICAL pacing thread with a CREATE_WAITABLE_TIMER_HIGH_RESOLUTION driving exactly-5.12 ms inter-packet cadence. This matches EESDR's reference TX cadence and eliminates Windows-scheduler-tick (16 ms) jitter.

Most of this lives in `ChannelMaster/sunsdr.c`. `SUNSDR_DEBUG_LOG_ENABLED 0` at the top of that file is the off-switch for the diagnostic log path; flip to 1 to reproduce or investigate any future issue.

## Files changed from upstream Thetis

**New files**
- `ChannelMaster/sunsdr.c` — native SunSDR protocol implementation (control + IQ streaming + TX pacing + calibration hooks)
- `ChannelMaster/sunsdr.h` — protocol constants, shared state, exported function signatures

**Modified files**
- `ChannelMaster/network.c`, `network.h` — P/Invoke wrappers, `nativeSunSDRSetDrive`, `nativeSunSDRSetFreq`, `nativeSunSDRSetMode`, `nativeSunSDRSetPTT`, `nativeSunSDRSetTune`, `nativeSunSDRLogTrace`
- `ChannelMaster/netInterface.c` — SunSDR branch in `StartAudioNative`; legacy `sunsdr_trace` writer disabled
- `ChannelMaster/cmasio.c`, `ivac.c` — diagnostic logging refresh
- `ChannelMaster/ChannelMaster.vcxproj` — added sunsdr.c/h to the build
- `Console/enums.cs` — added `HPSDRModel.SUNSDR2DX` and `RadioProtocol.SUNSDR`
- `Console/clsHardwareSpecific.cs` — SunSDR hardware model, default `PA Gain By Band`, firmware/serial display
- `Console/HPSDR/NetworkIO.cs`, `NetworkIOImports.cs` — SunSDR init / freq routing; `SunSDRTuneFreqOffsetHz` freq-offset state
- `Console/cmaster.cs` — SunSDR router configuration and source routing
- `Console/console.cs` — RX2 wiring, TX drive path, AM mode wiring, TUNE-at-dial shift, PS-A / 2-TONE / DUP disable on SunSDR
- `Console/HPSDR/Alex.cs` — SunSDR antenna control integration
- `Console/setup.cs`, `setup.designer.cs` — SunSDR in the radio-model dropdown; antenna-setup integration; SunSDR-specific PA Gain fallbacks and the per-drive `GetSunSDRDefaultAdjust` dB table
- `Console/radio.cs` — keeps `TXAMCarrierLevel` UI → WDSP plumbing on the SunSDR path
- `*.vcxproj` (portaudio, wdsp, cmASIO, ChannelMaster) — PlatformToolset v145 → v143

## Known open items

Tracked in the main README's "Current limitations" section. Summary:

- Per-band TX calibration for bands other than 40 m
- `0x1F` forward-power telemetry → Thetis `Fwd Pwr` meter wiring
- Replace the RX band-switch auto power-recycle with an ExpertSDR3-style clean reconfigure
- MON / DUP audio routing cleanup
- Pin the mode byte offset inside the 0x20 config block payload
- Diversity: no per-receiver antenna path found so far

## Building

1. Open `Project Files/Source/Thetis_VS2026.sln` in Visual Studio 2022.
2. Select **Debug | x64**.
3. Rebuild Solution.

Requires Visual Studio 2022 with the C++ desktop development workload and the v143 platform toolset. Release | x64 also builds.

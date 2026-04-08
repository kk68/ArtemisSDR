# Thetis-SunSDR: SunSDR2 DX Native Protocol Support

Native SunSDR2 DX integration for the [Thetis](https://github.com/ramdor/Thetis) SDR application, built through clean-room protocol reverse engineering.

## Status: RX Working (`rx-working-v1`)

The receive path is fully operational:

- **IQ Streaming** — Native SunSDR protocol on ports 50001 (control) and 50002 (IQ stream)
- **Resampler** — 312,500 Hz native rate resampled to 384,000 Hz for WDSP DSP engine
- **Panadapter & Waterfall** — Live spectrum display with correct frequencies
- **Frequency Tuning** — DDC0 = IQ center frequency, DDC0 offset = 92,500 Hz from primary VFO
- **Audio Output** — Demodulated audio via VAC (PortAudio/ReaRoute ASIO)
- **Bidirectional IQ** — TX silence packets keep the radio streaming (1 TX per 8 RX packets)

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

## Audio Path Fixes

Three issues had to be resolved for audio output to work:

1. **VAC run flag timing** — `SetIVACrun(0,1)` was never called because `console.PowerOn` was false when the C# `EnableVAC1()` ran. Fixed by setting `pvac[v]->run = 1` directly in `SunSDRPowerOn()`.

2. **VAC mixer deadlock** — The mixer waits for ALL active inputs (`WaitForMultipleObjects(..., TRUE, INFINITE)`). During RX-only, TX monitor input never gets data. Fixed by feeding silence into the TX pipeline via `Inbound()`, which keeps `xcmaster(tx_stream)` running and feeds both mixer inputs.

3. **No ASIO driver** — `audioCodecId` defaults to HERMES, routing the main mixer to the network void. VAC provides the working audio path.

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
- `Console/cmaster.cs` — SunSDR router configuration (1 source, 1 stream, Inbound to RX1)
- `Console/setup.cs`, `setup.designer.cs` — SunSDR in radio model dropdown
- `*.vcxproj` (portaudio, wdsp, cmASIO, ChannelMaster) — PlatformToolset v145 → v143

## Next Steps

- [ ] TX audio path (wire PTT/MOX to SunSDR opcode 0x06)
- [ ] Validate I/Q channel order with known signal
- [ ] Power cycle cleanup (off/on without restart)
- [ ] Mode switching validation

## Related Repositories

- **Protocol RE docs**: [kk68/ExploringSUN](https://github.com/kk68/ExploringSUN) — Clean-room reverse engineering documentation, Python tools, capture scripts
- **Upstream Thetis**: [ramdor/Thetis](https://github.com/ramdor/Thetis) — Original Thetis SDR application (archived)

## Building

1. Open `Project Files/Source/Thetis_VS2026.sln` in Visual Studio 2022
2. Set configuration to **Debug | x64**
3. Build → Rebuild Solution
4. Select SunSDR2-DX as radio model in Setup → General

Requires Visual Studio 2022 with C++ desktop development workload (v143 toolset).

## License

This project inherits the GNU General Public License v2 from upstream Thetis. See [LICENSE](LICENSE) for details.

Protocol implementation (`sunsdr.c`, `sunsdr.h`) is original work derived from clean-room reverse engineering of owned hardware.

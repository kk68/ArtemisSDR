# Thetis-SunSDR

**A Thetis fork that speaks the SunSDR2 DX's native network protocol.** If you own a SunSDR2 DX and have been looking for a richer, more active alternative to ExpertSDR, this brings Thetis — with its full DSP stack, panadapter, filter set, NR/NB/notch toolkit, VAC routing, and everything else Thetis offers — directly to your radio. No ExpertSDR proxy, no bridge, no firmware changes.

---

### ⚠️ Disclaimer

This project is **not affiliated with, endorsed by, sponsored by, or otherwise connected to Expert Electronics.** "SunSDR", "SunSDR2 DX", and "ExpertSDR" are trademarks of their respective owners; they appear in this project only to identify the hardware this software is compatible with.

The implementation is the product of independent, clean-room reverse engineering — passive observation of UDP traffic between a genuine ExpertSDR instance and an owned radio. **No ExpertSDR code, binaries, firmware, artwork, or other Expert Electronics intellectual property is used.** The radio's firmware is not modified in any way; this is purely a host-side client that speaks the same wire protocol ExpertSDR does.

Distributed free of charge under the GNU General Public License v2 for the amateur radio community.

---

## Contents

- [Who this is for](#who-this-is-for)
- [What works](#what-works)
- [Current limitations](#current-limitations)
- [Getting started](#getting-started)
- [TX power calibration per band](#tx-power-calibration-per-band)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [For contributors](#for-contributors)
- [License](#license)
- [Acknowledgments](#acknowledgments)

## Who this is for

You'll get the most out of this fork if:

- You own a **SunSDR2 DX** and want to use it with Thetis instead of (or alongside) ExpertSDR.
- You're comfortable building a Visual Studio project once. There's no pre-built installer yet.
- You have an external wattmeter and dummy load handy for the first TX bring-up on each band.
- You operate with normal amateur-radio discipline — we transmit into dummy loads for testing, not onto the air.

If you're brand new to SDR in general, Thetis, or your radio, work through your radio's official documentation and a basic Thetis tutorial first. This fork assumes you already understand what panadapters, VAC, and a drive slider do.

## What works

**Receive**

- RX1 with panadapter, waterfall, and audio on every mode Thetis supports
- RX2 as an independent second receiver with its own VFO B and audio path
- RX antenna selection (primary / auxiliary inputs)
- Live firmware-version and serial-number display in the title bar and `Setup → H/W Select`

**Transmit**

- MOX and TUNE via native wire protocol, reliable across consecutive attempts
- TUNE in every mode lands on the dial frequency — SSB, CW, AM, FM
- Voice SSB, AM, CW, and digital modes transmit on the correct sideband/carrier
- External PA (`xPA`) control during MOX and TUNE
- TX power **calibrated linearly on 40 m**: drive slider in watts maps to actual RF within ~1 W across 10–90 W
- TX antenna selection (primary / auxiliary outputs)
- AM Carrier Level setting wired through to the WDSP AM modulator

**General**

- Power-on to RX in ~1-1.5 seconds, comparable to ExpertSDR3
- Sub-second band switching — native protocol, no session teardown
- All of Thetis's DSP: NR, NR4, ANF, NB/NB2, EQ, CESSB, CFC, notch, compander — everything works
- VAC audio routing (CABLE, VoiceMeeter, etc.) works on both RX and TX
- Clean Power off / Power on cycling from the Thetis UI
- Proper Thetis-native `PA Gain By Band` and per-drive offsets integration — calibrate the way you'd calibrate any Thetis radio

## Current limitations

Honest list of what's partially done or missing. None of these prevent normal operation; they're items to be aware of.

| Area | Status |
| --- | --- |
| **TX power calibration** | 40 m is locked. Other bands fall back to the 40 m curve — expect a few dB deviation until separately calibrated. |
| **Thetis `Fwd Pwr` meter** | Not yet wired to the SunSDR telemetry stream. Use an external wattmeter for now. |
| **PS-A, 2-TONE, DUP** | Grayed out on SunSDR. These depend on a feedback-loop path the radio doesn't expose; not a bug, a hardware-architecture constraint. |
| **Diversity mode** | Unsupported. RX2 follows RX1's antenna selection; no independent per-receiver antenna path has been found. |
| **MON / DUP audio routing** | Not fully settled during TX. If you need to monitor your own transmission, a second receiver is the reliable path. |
| **Occasional post-TX raspy audio** | Intermittent; cycling VAC clears it. Tracked as a polish item. |

## Getting started

A complete step-by-step walkthrough lives in **[START_HERE_SUNSDR2DX.md](START_HERE_SUNSDR2DX.md)** — covers the Windows/network prerequisites, radio discovery, first-run setup, audio routing, and first TX. Read that one after you have a build.

Short version:

1. Build the solution (see [Building from source](#building-from-source) below).
2. Launch Thetis, pick **SunSDR2DX** as the hardware model, confirm **"Use watts on Drive/Tune slider"** is on in `Setup → General`.
3. Connect the radio, hit Power in Thetis, verify you're receiving.
4. Dummy load, 25 W drive, LSB TUNE on 40 m → confirm ~25 W on an external wattmeter.

## Troubleshooting

**Thetis doesn't see the radio.** Check that the radio's IP is reachable (`ping 10.0.3.50` from the host machine). Make sure no other ExpertSDR instance is running anywhere on the network — the radio's control port is exclusive.

**No TX RF output.** Confirm `Setup → General → Use watts on Drive/Tune slider` is on. Confirm the drive slider isn't at zero. Confirm you're in a transmittable mode (not SPEC or DRM).

**Audio is garbled or robotic after a TX cycle.** Cycle VAC off and on from its sidebar (the "Enable VAC" checkbox). This clears a resampler transient that occasionally lingers after certain TX → RX transitions.

**Signals on the panadapter look unusually wide, audio quality is off, right after startup.** Rare, but seen. Power-cycle Thetis (Power off → Power on) to re-initialize the RX DSP.

**`Fwd Pwr` meter reads zero.** Expected for now. The meter isn't wired to SunSDR forward-power telemetry yet — use an external wattmeter. This is the next planned feature.

## Building from source

Source-only distribution for now. Build locally with Visual Studio 2022.

**Prerequisites**

- Windows 10 or 11, x64
- Visual Studio 2022 with the **C++ desktop development** workload
- The **v143** platform toolset installed

**Steps**

1. Clone this repository.
2. Open `Project Files/Source/Thetis_VS2026.sln`.
3. Configuration: **Debug | x64** (Release | x64 also builds).
4. Build → Rebuild Solution.
5. Run the resulting `Thetis.exe`.

## For contributors

Protocol implementation lives in `Project Files/Source/ChannelMaster/sunsdr.c` and `sunsdr.h`. These are clean-room original work — no external sources referenced.

Deeper architecture notes, opcode tables, TX power-calibration design, VAC underflow root-cause analysis, and the file-by-file changelog are in **[TECHNICAL.md](TECHNICAL.md)**.

Protocol-level reverse-engineering documentation is maintained in a separate private repository. If you're contributing at the wire-protocol level and need access, reach out directly.

Contributions welcome: bug fixes, per-band calibration data, UI polish, completion of the open limitations. Pull requests against `feature/sunsdr2dx` please.

## License

This fork inherits the **GNU General Public License, version 2** from upstream Thetis. See [LICENSE](LICENSE) for full terms. All source code must remain under GPL v2; any redistributed modifications must also be under GPL v2 and must provide full source.

The SunSDR native protocol implementation (`sunsdr.c`, `sunsdr.h`) is original work and is licensed the same way. It is derived from independent clean-room reverse engineering of owned hardware — no Expert Electronics code, binaries, firmware, or other intellectual property was used.

## Acknowledgments

- **Thetis** — Richard Samphire (MW0LGE) and the Thetis contributor community. Upstream: [ramdor/Thetis](https://github.com/ramdor/Thetis).
- **PowerSDR** — FlexRadio Systems and Doug Wigley, the ancestor of this whole lineage.
- **WDSP** — Warren Pratt (NR0V) and contributors. The DSP engine at the heart of everything.
- The amateur radio community, for decades of making radios and software talk to each other.

No affiliation with Expert Electronics is implied or claimed.

73!

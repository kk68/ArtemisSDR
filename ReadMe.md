# Thetis-SunSDR

**Native [SunSDR2 DX](https://eesdr.com/en/) support for [Thetis](https://github.com/ramdor/Thetis)**, the modern open-source Windows SDR application.

If you own a SunSDR2 DX and want a Thetis-class user experience instead of ExpertSDR, this fork adds direct native-protocol control of the radio — no ExpertSDR proxy, no bridge software, no firmware changes required.

---

## ⚠️ Disclaimer

This project is **not affiliated with, endorsed by, sponsored by, or otherwise connected to Expert Electronics**. "SunSDR", "SunSDR2 DX", "ExpertSDR", and related names are trademarks of their respective owners and are used here only in the nominative sense, to identify the hardware this software is compatible with.

This software is the result of independent, clean-room reverse engineering of the radio's network protocol, performed by passively observing UDP traffic between a genuine ExpertSDR instance and an owned radio. **No ExpertSDR code, binaries, firmware, artwork, or other Expert Electronics intellectual property was decompiled, copied, or used.** The radio's firmware is not touched — this fork is purely a Thetis-side host client that speaks the radio's wire protocol.

It is offered free of charge under GPL v2 for the amateur radio community.

---

## What works

**Receive**
- RX1 with full panadapter, waterfall, and demodulated audio on all modes Thetis supports
- RX2 as an independent second receiver with its own VFO B tuning and audio
- RX antenna selection (primary / auxiliary inputs)
- Correct firmware version and serial-number display in title bar and `Setup → HW Select`

**Transmit**
- MOX / PTT via native wire protocol — reliable across consecutive transmissions
- TUNE in all modes, landing on the dial frequency (SSB, CW, AM, FM)
- Voice SSB, AM, CW, digital modes transmit on the correct sideband / carrier
- TX antenna selection (primary / auxiliary TX outputs)
- External PA (`xPA`) control during MOX and TUNE
- Linear TX power calibrated on 40 m: drive slider in watts maps to actual RF output within ~1 W across 10–90 W

**Other**
- Full Thetis DSP stack: NR, NR4, notch filters, NB/ANF/NB2, EQ, CESSB, CFC — all work as on any other supported radio
- VAC audio path (CABLE / VoiceMeeter / etc.) routes correctly on both RX and TX
- Power off / power on cycles cleanly from the Thetis UI without losing connection
- AM Carrier Level UI in Setup → Transmit wired to the WDSP AM modulator

## Current limitations

- **TX power calibration is complete only on 40 m.** Other bands fall back to the 40 m table and may be off by a few dB until you run the same sweep on them (instructions below). Use an external wattmeter for final tuning.
- **Thetis `Fwd Pwr` meter is not yet wired** to the SunSDR's forward-power telemetry. The radio reports real power in its telemetry stream, but that data is not yet fed to Thetis's on-screen meter. An external wattmeter is the reliable source for now.
- **RX band-switching uses an automatic power recycle** as a short-term workaround. Switching bands while receiving briefly cycles the radio connection so the stream resets cleanly. Works reliably; will be replaced with a proper reconfigure sequence later.
- **PS-A (PureSignal Auto-tune), 2-TONE, and DUP** are grayed out on SunSDR2 DX — those features depend on an internal feedback-loop path the SunSDR does not expose. This is a hardware/architecture limitation, not a bug.
- **Diversity mode is not supported.** The SunSDR's RX2 follows RX1's antenna selection; independent per-receiver antenna assignment has not been recovered.
- **MON / DUP audio routing** during TX is not fully settled. Use with caution; external receive with a second radio is the reliable path if you need to monitor your own transmission.
- **Occasional post-TX audio "rasp"** on RX can occur; cycling VAC clears it. Tracked as a polish item.

## Getting started

### 1. Requirements

- Windows 10 / 11 (x64)
- A SunSDR2 DX radio with its standard firmware (no modification needed)
- A wired or wireless network link to the radio (standard SunSDR IP config — default 10.0.3.50)
- Your own wattmeter, dummy load, and amateur license — normal transmit discipline applies

### 2. Build and install

For now this fork is distributed as **source only** — build locally with Visual Studio 2022.

1. Clone this repo.
2. Open `Project Files/Source/Thetis_VS2026.sln` in Visual Studio 2022.
3. Select **Debug | x64** (Release also works; Debug is the tested config).
4. Build → Rebuild Solution.
5. Run the resulting `Thetis.exe`.

**Prerequisites**: Visual Studio 2022 with the **C++ desktop development** workload and the **v143 platform toolset**.

### 3. First-run setup

1. Launch Thetis. When the hardware selection dialog appears, choose **SunSDR2-DX**.
2. In `Setup → General`, confirm **"Use watts on Drive/Tune slider"** is checked.
3. Connect the radio (power on, network connected). Thetis should show the radio's firmware version in the title bar within a few seconds.
4. Press **Power** in Thetis to start receiving.

### 4. Verify RX

Tune to a known-active frequency (e.g., 40 m SSB on the weekend). You should see the panadapter populate and hear audio through your chosen output.

### 5. Verify TX (external wattmeter required)

1. Connect a dummy load to the radio's antenna port.
2. In Thetis, set mode to LSB or AM, set the drive slider to 25 W, point to a clear frequency.
3. Press TUNE briefly (2–3 seconds) and check the external wattmeter.
4. On 40 m, 25 W on the slider should read close to 25 W on the meter. On other bands, deviations of a few dB are expected until those bands are separately calibrated.

## TX power calibration (per-band workflow)

Each band has its own power curve. The 40 m curve is dialed in; other bands start from the same defaults and may need a tweak.

1. Connect a dummy load.
2. Select the band you want to calibrate.
3. Set mode to **LSB TUNE** (or AM, both give the same result — the underlying calibration is mode-independent).
4. Note the reading on your external wattmeter at UI slider = `5, 10, 15, 20, 30, 40, 50, 60, 70, 80, 90, 100` W. 3 seconds per point is enough.
5. In `Setup → PA Settings → PA Gain`, find the "Offsets for `<band>`" column on the right. Each row is a drive level (10–90 W) and takes a dB correction in the ±6 dB range.
6. For each UI watts point where actual ≠ UI, enter the correction `10 × log10(UI_watts / actual_watts)` dB. Positive values increase power at that drive level, negative decrease it.
7. Re-sweep. Repeat once or twice if needed — each round roughly halves the error.

> Before your per-drive offsets take effect, the band's **PA Gain By Band** dB value must be something other than `100.0`. Leave at `100.0` to use the SunSDR built-in fallback curve (40 m calibrated); set to any other value (e.g. `100.1`) to switch the band to your UI-controlled offsets.

The `Watt Meter` page in PA Settings only affects Thetis's on-screen `Fwd Pwr` meter — it does not change actual RF output. And since that meter isn't yet wired to the SunSDR telemetry anyway, those numbers aren't meaningful today.

## Troubleshooting

**Thetis doesn't see the radio.**
Check the radio's IP is reachable (ping `10.0.3.50` from the host). Check no other ExpertSDR instance is running — the radio's control port is exclusive.

**No TX RF output.**
Confirm `Setup → General → Use watts on Drive/Tune slider` is on. Confirm the drive slider isn't at 0. Confirm mode isn't one of the RX-only modes (SPEC, DRM).

**Audio is garbled / robotic after TX.**
Cycle VAC off and on from its sidebar (the "Enable VAC" box in Thetis). This clears a rmatch-resampler transient that occasionally lingers after certain TX → RX transitions.

**Signals look wide or AM-like, audio quality is off.**
Occasionally seen at power-on. Power-cycle Thetis (Power off → Power on) to re-initialize the RX DSP.

**Fwd Pwr meter reads 0.**
Expected for now — the meter is not yet wired to SunSDR telemetry. Use an external wattmeter.

## For contributors / developers

Protocol implementation lives in `Project Files/Source/ChannelMaster/sunsdr.c` and `sunsdr.h`. That is original clean-room work — no external sources were referenced.

Protocol-level documentation (opcodes, packet formats, calibration architecture, raspy-investigation arc) is maintained in a private reverse-engineering repo. If you're actively contributing protocol work and need access to that documentation, reach out directly.

Higher-level technical notes on this fork — file-by-file changes, architecture diagram, VAC underflow analysis, TX calibration internals — are in `TECHNICAL.md`.

Contributions (fixes, other-band calibration data, UI polish, new RE for the remaining items) are welcome. Please submit as pull requests against `feature/sunsdr2dx`.

## License

This fork inherits the **GNU General Public License version 2** from upstream Thetis. See [LICENSE](LICENSE) for full terms.

All source code must remain under GPL v2. Any redistributed modifications must also be under GPL v2 and must provide full source.

The SunSDR native protocol implementation (`sunsdr.c`, `sunsdr.h`) is original work and is licensed the same way. It is derived from independent clean-room reverse engineering of owned hardware — no Expert Electronics code, binaries, firmware, or other intellectual property was used.

## Acknowledgments

- **Thetis** — Richard Samphire (MW0LGE) and the extensive Thetis contributor community. Upstream: [ramdor/Thetis](https://github.com/ramdor/Thetis).
- **PowerSDR** — FlexRadio Systems and Doug Wigley, the Thetis ancestor.
- **WDSP** — Warren Pratt (NR0V) and contributors. The DSP engine is the foundation of everything Thetis does.
- The amateur radio community, for decades of making radios talk to software and software talk to radios.

No affiliation with Expert Electronics is implied or claimed.

## Contact

For bugs and feature requests, please open an issue on this repository.
For licensing questions or other correspondence, contact the repository maintainer through GitHub.

73!

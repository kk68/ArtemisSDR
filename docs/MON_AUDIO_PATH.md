# MON audio path and PRE/POST patch

This patch targets SunSDR2 DX with VAC. The native mixer fix also applies to
cmASIO and TCI, and the SunSDR family UI gate is removed for DX and PRO.
The operator reported the initial DX/VAC listening check working on 2026-09-09.
Further listening tests, including the other audio paths, are still required;
the automated tests do not open an audio device or connect to a radio.

## Operator controls

Click the main MON button to cycle **Off → PRE → POST → Off**. The button reads
`MON` when off, `PRE` or `POST` when monitoring, and its tooltip describes the
selected function. The custom meter MON button uses the same cycle.

- **PRE:** the selected mic, VAC, playback or TCI input after source routing and
  source input gain, before ChannelMaster VOX/DEXP and WDSP MIC gain, EQ,
  leveler, compression and ALC. The selected input's left channel is copied to
  both monitor channels.
- **POST:** audio after WDSP MIC gain, processing and ALC, before AM/FM
  modulation or RF IQ correction/scaling. Both ears receive the real audio
  component, rather than treating analytic I/Q as stereo. Generated TUNE and
  two-tone signals are monitored at the output generator instead.
- **CW:** the button remains an on/off sidetone control; PRE/POST never sends
  microphone audio into the CW monitor.

The AF slider while transmitting controls monitor volume (TX AF). VAC RX Gain
also scales VAC output. PRE may be much quieter than POST when MIC gain or
compression is high. Input gain in VAC or the audio device still affects PRE.

Boolean CAT/TCI/MIDI on/off controls remain compatible: enabling MON through
the Console MON property selects POST. Taps do not change the transmitted
signal, PTT, drive or RF routing. A fresh application session defaults to POST
for saved/programmatic MON activation; PRE/POST selection is not saved.

## Source map

| Layer | Main source | Responsibility |
| --- | --- | --- |
| WinForms control | `Console/console.cs`, `audio.cs` | MON cycle, CW behavior, TX AF, VAC selection and MOX |
| Native radio input | `ChannelMaster/sunsdr.c`, `router.c`, `cmbuffs.c` | SunSDR RX IQ and continuous TX input cadence |
| Audio routing | `ChannelMaster/cmaster.c`, `pipe.c` | Select cmASIO/VAC/TCI/playback input, VOX/DEXP, WDSP exchange, monitor fan-out |
| Transmit processing | `wdsp/TXA.c`, `iobuffs.c` | PRE/POST taps and companion audio buffers synchronized with TX exchange |
| Playback mixing | `ChannelMaster/aamix.c`, `ivac.c`, `tci.c` | Correct sample counts, RX/TX source selection, VAC/TCI monitor output |
| Local CW | `ChannelMaster/sidetone.c` | Substitute keyed software sidetone for voice monitor |

```text
Selected input ──┬── PRE ─────────────────────────────────────────┐
                └── VOX/DEXP → MIC/EQ/leveler/COMP/ALC ──┬─ POST ┤
                                                       │        │
                                                       │   TX audio companion
                                                       │   (same exchange clock)
                                                       │        ↓
                                                       │   CW substitution
                                                       │        ↓
                                                       │   VAC / global mixer / TCI
                                                       ↓
                                              Modulation → RF IQ → SunSDR
```

## Root causes and correction

At the native 312500 Hz RX rate, RX audio arrives in **96 samples at 48 kHz**,
while the TX exchange supplies **256 samples at 192 kHz**, equivalent to only
**64 samples at 48 kHz**. The old mixer inferred input length from its
96-sample output block. It read 384 TX samples when only 256 were supplied and
advanced the monitor stream at 1.5 times its actual rate.

`xMixAudioSized` consumes the producer's actual sample count, preserves
resampler phase/history, and queues only the samples actually produced. The
output thread is released when a complete output block is available. Existing
equal-duration callers retain `xMixAudio`; TX monitor consumers use the sized
entry point. Long input blocks are split to fit the allocated resampler buffer.

Separately, `SetIVACaudioSize` recreated VAC with only RX active. The comment
claimed MON could still mix through `what=3`, but inactive streams are rejected
by the input gate and excluded from mixing. VAC now derives both masks from
MOX/MON: RX alone when monitoring is off in receive, RX plus sidetone when MON
is armed in receive, and TX alone while transmitting. MON off during TX emits
silence at the TX clock. Recreated mixers retain routing, RX gain, AF gain and
monitor volume. VAC input dispatch is serialized with mixer replacement.

The mixer now owns a joinable worker handle and waits for that worker before
freeing or reusing its buffers. VAC destruction stops its mixer before freeing
the VAC/resampler state.

PRE input and selected monitor output have companion rings inside WDSP with
the same indices, sample counts and semaphore as TX. Reading a display siphon
or periodically copying the latest DSP block is not used for playback. Stopped
TX channels output monitor silence; monitor audio follows the TX down-slew.

## Build and automated validation

Requires Visual Studio 2022 with the v143 C++ workload, as for ArtemisSDR.

```powershell
.\tests\run-audio-monitor-tests.ps1
```

This builds the native libraries and the hardware-free test executable under
`artifacts/audio-tests`. It exercises production mixer/VAC/sidetone source and
the built WDSP DLL. The tests cover:

- Guard-page detection of TX buffer overreads, exact output counts and clean
  1 kHz audio for 48/96/192 kHz TX input and both 96- and 64-sample output blocks.
- VAC RX startup without TX input, TX monitoring without RX input, MON mute,
  receive return, repeated MOX/MON transitions, and gain/routing retention
  across buffer-size and sample-rate changes.
- PRE versus POST MIC-gain response, raw input before VOX/DEXP, RF output power
  independence from monitor selection, AM/FM and TUNE audio, CW microphone
  suppression, stopped-channel silence, and DSP block/rate reconfiguration.
- Software CW sidetone replacing the voice tap, key-up silence, and RF IQ
  remaining untouched by local sidetone generation.

For the full application, build `ChannelMaster` and `ArtemisSDR` in
`Project Files/Source/ArtemisSDR.sln`, **Release | x64**, restoring NuGet packages
if needed. Run `Project Files/bin/x64/Release/ArtemisSDR.exe` with the DLLs in
that directory. **ArtemisSDR.exe, ChannelMaster.dll and wdsp.dll must come from
the same build** because this patch adds a native monitor exchange entry point.

## DX + VAC listening check

1. Close the previous ArtemisSDR instance and launch the patched build. Use
   your normal DX/VAC configuration and headphones to avoid acoustic feedback.
2. Confirm normal RX audio with MON off. In USB or LSB, cycle MON through PRE,
   POST and off; the labels should follow each click.
3. Use your normal test-transmit setup, preferably a dummy load. While speaking
   on PTT/MOX, compare PRE and POST at a comfortable TX AF level. Change MIC
   gain/EQ/compression: PRE should retain the raw sound; POST should reflect
   those changes. RF settings must remain unchanged.
4. Switch MON off while transmitting: local voice monitoring must stop. Release
   PTT and verify immediate normal receive audio. Repeat several times.
5. Toggle VAC off/on and change away from/back to the RX sample rate. Verify
   the monitor still works and retains its volume. Also check FM/AM and CW if
   those modes are part of your operating setup.

Record the VAC host API, device, sample rate and buffer size with any report of
silence, clicks, distortion or growing VAC underflow/overflow counters. A clean
software test does not establish end-to-end sound-card/radio performance.

## MSI release packaging

Build `ArtemisSDR-Installer/ArtemisSDR-Setup.sln` under `Project Files/Source`
with MSBuild, **Release | x64**, after the application build. WiX 3.x is required.
The installer consumes the existing Release binaries without rebuilding them.
The MSI takes its version from ArtemisSDR.exe. Version 2.1.10 ships as
`ArtemisSDR-v2.1.10.x64.msi` and upgrades existing 2.1.9 installations.

The runtime merge module can be selected with
`/p:DefineConstants=VCRuntimeMergeModule=<absolute path to the .msm>`.
For the VS 2022/v143 build, use Microsoft's `Microsoft_VC143_CRT_x64.msm`
from the matching Visual C++ Redistributable MSM package. The historical
VC145 filename remains the default for other packaging environments.

The installer manifest now includes all six additional managed dependencies
referenced by the Console project. Its Start menu shortcut registry key uses
HKCU, as required for non-advertised shortcuts by Windows Installer validation.
Unpack the finished MSI and compare the payload hashes with the build output
before distributing it. First launch can take about 10 minutes if FFT plans
need to be generated on the target machine.

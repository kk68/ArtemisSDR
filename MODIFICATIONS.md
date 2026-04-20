# ArtemisSDR — Modification Notices

This file documents modifications made to upstream source files by the
ArtemisSDR fork, in satisfaction of the GNU General Public License v2, §2(a):

> *"You must cause the modified files to carry prominent notices stating that
> you changed the files and the date of any change."*

Maintaining per-file headers for every modified source file is impractical given
the breadth of the ArtemisSDR changes; this aggregated notice is the equivalent
compliance mechanism.

---

## Modification scope

All modifications documented here were made by **Kosta Kanchev (K0KOZ,
K0KOZ@philibe.com)** for the ArtemisSDR fork of Thetis (upstream
[ramdor/Thetis](https://github.com/ramdor/Thetis)).

Date of first substantive change: **2026-04-07** (initial fork).

Date of this modification notice: **2026-04-16** (v1.0.1 → v1.0.2).

Changes are ongoing; each release tag on https://github.com/kk68/ArtemisSDR
represents a snapshot of the modified source at that point in time. The full
per-commit change history is authoritative and available via `git log`.

## Files modified

The following files contain substantive modifications by Kosta Kanchev (K0KOZ)
beyond the upstream Thetis source:

### Wholly new files (authored by K0KOZ, Copyright © 2026)

- `Project Files/Source/ChannelMaster/sunsdr.c`
- `Project Files/Source/ChannelMaster/sunsdr.h`
- `Project Files/Source/Console/clsDiscord.cs` (wholesale rewrite as a no-op
  stub replacing upstream's Discord bot integration)
- `Build-ArtemisSDR.ps1` (at repo parent level — build script)
- `LICENSE-DUAL-LICENSING-K0KOZ` (dual-licensing statement for K0KOZ's own
  contributions)
- `MODIFICATIONS.md` (this file)
- `THIRD_PARTY_LICENSES.md`
- Installer artwork: `Project Files/Source/ArtemisSDR-Installer/binary/
  artemissdr_banner.bmp`, `artemissdr_background.bmp`, `ArtemisSDR.ico`
- Splash artwork: embedded PNG inside `Project Files/Source/Console/splash.resx`
  and `Project Files/Source/Console/Resources/thetis-logo2.png`

### Heavily modified files (upstream Thetis / PowerSDR / FlexRadio authorship
preserved in file headers; modifications for SunSDR2 DX integration and
ArtemisSDR rebrand):

- `Project Files/Source/Console/console.cs`
- `Project Files/Source/Console/setup.cs`
- `Project Files/Source/Console/setup.Designer.cs`
- `Project Files/Source/Console/splash.cs`
- `Project Files/Source/Console/titlebar.cs`
- `Project Files/Source/Console/frmAbout.cs`
- `Project Files/Source/Console/frmAbout.Designer.cs`
- `Project Files/Source/Console/frmAbout.resx`
- `Project Files/Source/Console/clsSingleInstance.cs`
- `Project Files/Source/Console/clsCMASIOConfig.cs`
- `Project Files/Source/Console/clsProgressLog.cs`
- `Project Files/Source/Console/clsImgeFetcher.cs`
- `Project Files/Source/Console/clsThetisSkinService.cs`
- `Project Files/Source/Console/clsDBMan.cs`
- `Project Files/Source/Console/clsAudioRecordPlayback.cs`
- `Project Files/Source/Console/MeterManager.cs`
- `Project Files/Source/Console/Memory/MemoryForm.cs`
- `Project Files/Source/Console/CAT/TCPIPcatServer.cs`
- `Project Files/Source/Console/TCIServer.cs`
- `Project Files/Source/Console/N1MM.cs`
- `Project Files/Source/Console/Firewall.cs`
- `Project Files/Source/Console/NetworkThrottle.cs`
- `Project Files/Source/Console/HPSDR/NetworkIO.cs`
- `Project Files/Source/Console/HPSDR/NetworkIOImports.cs`
- `Project Files/Source/Console/ShutdownForm.Designer.cs`
- `Project Files/Source/Console/frmSeqLog.Designer.cs`
- `Project Files/Source/Console/frmMeterDisplay.cs`
- `Project Files/Source/Console/display.cs`
- `Project Files/Source/Console/radio.cs`
- `Project Files/Source/Console/AssemblyInfo.cs`
- `Project Files/Source/ChannelMaster/network.c`
- `Project Files/Source/ChannelMaster/network.h`
- All WiX installer files under `Project Files/Source/ArtemisSDR-Installer/`
- `Project Files/Source/ArtemisSDR.sln`
- `Project Files/Source/Console/ArtemisSDR.csproj`
- `ReadMe.md`, `TECHNICAL.md`, `START_HERE_SUNSDR2DX.md`,
  `Project Files/Source/ReleaseNotes.txt`

### Nature of modifications

Modifications include (but are not limited to):

- Addition of a complete SunSDR2 DX native UDP wire-protocol implementation
  (clean C code in `sunsdr.c` / `sunsdr.h` and integration into the existing
  Thetis C#/C++ framework)
- Rebrand from "Thetis" to "ArtemisSDR", including identifiers, UI strings,
  installer, AppData paths, Windows Firewall rule names, single-instance
  mutex GUID, Start Menu shortcut, registry keys, file names, and artwork
- Removal/neutralisation of the upstream Discord bot integration
- TX power-calibration fixes (40 m locked, per-band offset support)
- Cold-start race-condition fix for RX DSP initialisation
- Band-switch workaround removal
- AM power calibration integration
- Restriction of hardware-model dropdown to SUNSDR2-DX
- Correction of PA-gain UI range (Minimum 38.8 → 0) for SunSDR default gains
- Attribution, licensing-statement, and documentation overhaul
- Numerous smaller bug fixes, UI polish, rebrand consistency changes

For a file-level changelog see `git log <path>` on the
[kk68/ArtemisSDR repository](https://github.com/kk68/ArtemisSDR).

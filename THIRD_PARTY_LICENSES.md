# Third-Party Licenses

ArtemisSDR inherits and bundles a number of third-party libraries from upstream
Thetis and PowerSDR, plus the standard .NET NuGet ecosystem. This document is
a roll-up of the notable third-party components, their authors, their licenses,
and where to find the canonical upstream.

The ArtemisSDR codebase itself (the SunSDR2 DX native protocol implementation
in `Project Files/Source/ChannelMaster/sunsdr.c` / `sunsdr.h`, the ArtemisSDR
rebrand, and related integration work authored by Kosta Kanchev K0KOZ) is
covered by `LICENSE` (GPL v2) plus the dual-licensing statement in
`LICENSE-DUAL-LICENSING-K0KOZ`.

Upstream Thetis contributions by Richard Samphire (MW0LGE) are covered by
`LICENSE` (GPL v2) plus the dual-licensing statement in `LICENSE-DUAL-LICENSING`.

---

## DSP engine

### WDSP — Warren Pratt (NR0V)
DSP engine at the heart of everything. License: **GPL v2+**.
Upstream: https://github.com/TAPR/OpenHPSDR-wdsp

### PowerSDR — FlexRadio Systems / Doug Wigley (W5WC)
Ancestor of the whole Thetis lineage. License: **GPL v2+**.
Retains copyright notices in every source file header.

### rnnoise — Xiph.Org Foundation / Jean-Marc Valin
RNN-based noise reduction. License: **BSD 3-Clause**.
Upstream: https://github.com/xiph/rnnoise
Bundled under `Project Files/lib/NR_Algorithms_x64/src/rnnoise/`

### libspecbleach — Luciano Dato
Spectral noise reduction library. License: **LGPL v3+**.
Upstream: https://github.com/lucianodato/libspecbleach
Bundled under `Project Files/lib/NR_Algorithms_x64/src/libspecbleach/`

### FFTW — Matteo Frigo & Steven G. Johnson
Fast Fourier Transform library. License: **GPL v2+**.
Upstream: https://www.fftw.org/
Bundled under `Project Files/lib/fftw_x64/`

### PortAudio — PortAudio Community
Cross-platform audio I/O library. License: **MIT**.
Upstream: http://www.portaudio.com/
Bundled under `Project Files/lib/portaudio-19.7.0/`

---

## Audio / I/O

### NAudio — Mark Heath and contributors
.NET audio library (ASIO, WASAPI, MIDI, WinForms integration). License: **MIT**.
Upstream: https://github.com/naudio/NAudio

### cmASIO — BV4 / upstream Thetis contributors
ASIO host bridge for ChannelMaster. License: **GPL v2+** (inherited).

### FTD2XX.NET — FTDI Ltd. wrapper
USB-serial bridge wrapper. License: **permissive, redistributable with
accompanying application** (see `packages/FTD2XX.Net.1.2.1/License.txt`).

---

## Graphics / display

### SharpDX — Alexandre Mutel
.NET DirectX wrapper used by the panadapter / waterfall / metering. License:
**MIT**.
Upstream: http://sharpdx.org/

### SkiaSharp — Microsoft / Google / Skia contributors
2D rendering library, used for meter rendering and image handling. License:
**MIT**.
Upstream: https://github.com/mono/SkiaSharp

### Svg — SVG.NET contributors
SVG rendering library. License: **MIT**.
Upstream: https://github.com/svg-net/SVG

---

## Text / markup / HTTP

### Newtonsoft.Json — James Newton-King
JSON serialization used across the codebase. License: **MIT**.
Upstream: https://www.newtonsoft.com/json

### Markdig — Alexandre Mutel
Markdown parser (for release-notes rendering). License: **BSD 2-Clause**.
Upstream: https://github.com/xoofx/markdig

### HtmlAgilityPack — contributors
HTML parser (used in skin-server HTML scraping). License: **MIT**.
Upstream: https://html-agility-pack.net/

### ExCSS — Tyler Brinks and contributors
CSS parser. License: **MIT**.
Upstream: https://github.com/TylerBrinks/ExCSS

### System.Reactive — .NET Foundation
Rx for .NET. License: **MIT**.
Upstream: https://github.com/dotnet/reactive

---

## System / OS integration

### WindowsFirewallHelper — Soroush Falahati
Windows Firewall rule management. License: **MIT**.
Upstream: https://github.com/falahati/WindowsFirewallHelper

### RawInput — upstream Thetis / PowerSDR contributors
Low-level Windows Raw Input wrapper. License: **GPL v2+** (inherited).

### Microsoft .NET Framework BCL packages
Various `Microsoft.*` and `System.*` packages from NuGet: **MIT** unless
otherwise noted in each package's `LICENSE.TXT` file inside
`Project Files/Source/packages/`.

---

## Trademark identification

The following third-party trademarks are mentioned in the codebase and
documentation purely to identify compatibility or heritage. No endorsement,
sponsorship, or affiliation is implied.

- **SunSDR®**, **SunSDR2 DX®**, **ExpertSDR™** — Expert Electronics
- **Thetis™** — Richard Samphire (MW0LGE)
- **ANAN®**, **Apache Labs™** — Apache Labs / Abhi Dholakia
- **Hermes**, **Hermes Lite 2™** — TAPR / OpenHPSDR / Steve Haynal
- **PowerSDR™**, **FlexRadio™**, **FLEX-6xxx** — FlexRadio Systems
- **Discord**, **Windows**, **DirectX**, **.NET**, **Visual Studio** —
  Microsoft Corporation / Discord Inc. (usage is incidental; no bundled code)

ArtemisSDR is **not affiliated with, endorsed by, or otherwise connected to**
NASA, the Artemis program, Expert Electronics, FlexRadio Systems, Apache Labs,
or any of the trademark holders above.

---

## Questions

Anything missing or incorrectly attributed? Open an issue at
https://github.com/kk68/ArtemisSDR/issues — corrections welcome.

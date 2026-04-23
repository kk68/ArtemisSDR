// SunSdrAntenna.cs
//
// Single source of truth for SunSDR2 DX antenna names, per-band validity,
// UI-slot ↔ physical-ant translation, and the one and only wire-value
// mapping. Everywhere else in Artemis that touches antennas for this
// radio must go through the helpers here — do not write your own
// "slot N means A_x on band Y" logic.
//
// Naming: we use EESDR terminology. The SunSDR2 DX has three physical
// antenna ports on the back panel:
//
//   A1 — dedicated 2 m (VHF) connector
//   A2 — HF antenna port 1
//   A3 — HF antenna port 2
//
// The radio also has a back-panel ADC bypass path on VHF (preamble 0x1E = 1)
// which Artemis does not expose as a user selector — per user spec.
//
// Wire quirks on the SunSDR2 DX:
//   * Opcode 0x15 carries the selector. 0x1E is a preamble, always
//     0 on HF and 0 on VHF-A1 / 1 on VHF-ADC.
//   * HF RX selector: A2 → 0x01, A3 → 0x03.
//   * HF TX selector: A2 → 0x01, A3 → 0x02.
//   * VHF A1: 0x15 = 0x01 regardless of RX/TX.
//
// Those exact byte values live inside the native layer (sunsdr.c);
// the wire value we return from here is the argument the native
// `nativeSunSDRSetAntenna` / `nativeSunSDRSetTxAntenna` functions
// expect (1 or 2 for HF A2 / A3; 1 for VHF A1). The native layer
// applies its own HF-vs-VHF and RX-vs-TX byte mapping on top.

using System;

namespace Thetis
{
    public enum SunSdrAntenna : byte
    {
        None = 0,
        A1   = 1,   // 2 m VHF
        A2   = 2,   // HF port 1
        A3   = 3,   // HF port 2
    }

    public static class SunSdrAntennaSpec
    {
        private static readonly SunSdrAntenna[] HF_VALID = { SunSdrAntenna.A2, SunSdrAntenna.A3 };
        private static readonly SunSdrAntenna[] VHF_VALID = { SunSdrAntenna.A1 };
        private static readonly SunSdrAntenna[] EMPTY = new SunSdrAntenna[0];

        // Antennas physically usable on the given band.
        public static SunSdrAntenna[] ValidForBand(Band band)
        {
            if (band == Band.B2M) return VHF_VALID;
            if (band >= Band.B160M && band <= Band.B6M) return HF_VALID;
            return EMPTY;
        }

        public static SunSdrAntenna DefaultForBand(Band band)
        {
            if (band == Band.B2M) return SunSdrAntenna.A1;
            if (band >= Band.B160M && band <= Band.B6M) return SunSdrAntenna.A2;
            return SunSdrAntenna.None;
        }

        public static bool IsValidForBand(SunSdrAntenna ant, Band band)
        {
            foreach (var a in ValidForBand(band))
            {
                if (a == ant) return true;
            }
            return false;
        }

        // Argument to pass to nativeSunSDRSetAntenna / nativeSunSDRSetTxAntenna.
        // Returns 0 if the physical antenna is not valid for the band — callers
        // must treat 0 as "skip the wire write".
        //
        // RX vs TX take the same argument here; the HF-RX-vs-HF-TX wire-byte
        // difference (A3 → 0x03 on RX, 0x02 on TX) is handled inside the
        // native mapping (sunsdr_map_ant_selector vs sunsdr_map_tx_ant_selector).
        public static int WireValueFor(SunSdrAntenna ant, Band band, bool isTx)
        {
            if (!IsValidForBand(ant, band)) return 0;

            if (band == Band.B2M)
            {
                return 1; // A1 on VHF -> preamble 0, selector 0x01
            }

            // HF / 6 m
            switch (ant)
            {
                case SunSdrAntenna.A2: return 1;
                case SunSdrAntenna.A3: return 2;
                default:               return 0;
            }
        }

        // UI-slot translation.
        //
        // The Setup form and the bottom-left meter render physical antenna
        // buttons in a fixed layout: column 1 = A1, column 2 = A2, column 3 = A3.
        // Per-band, only a subset of those columns is visible — the rest is
        // hidden. Callers render by iterating ValidForBand(band) and use
        // ColumnIndexFor(ant) to decide which slot to show the "on" state in.
        public static int ColumnIndexFor(SunSdrAntenna ant)
        {
            switch (ant)
            {
                case SunSdrAntenna.A1: return 0;
                case SunSdrAntenna.A2: return 1;
                case SunSdrAntenna.A3: return 2;
                default:               return -1;
            }
        }

        public static SunSdrAntenna FromColumnIndex(int col)
        {
            switch (col)
            {
                case 0: return SunSdrAntenna.A1;
                case 1: return SunSdrAntenna.A2;
                case 2: return SunSdrAntenna.A3;
                default: return SunSdrAntenna.None;
            }
        }

        public static string DisplayName(SunSdrAntenna ant)
        {
            switch (ant)
            {
                case SunSdrAntenna.A1: return "A1";
                case SunSdrAntenna.A2: return "A2";
                case SunSdrAntenna.A3: return "A3";
                default:               return string.Empty;
            }
        }
    }
}

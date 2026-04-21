//=================================================================
// RepeaterBookImport.cs
//=================================================================
// ArtemisSDR — import memory channels from a RepeaterBook CSV export.
// Copyright (C) 2026 Kosta Kanchev (K0KOZ). GPL v2.
//
// RepeaterBook CSV schema (as exported from repeaterbook.com):
//   Output Freq, Input Freq, Offset, Uplink Tone, Downlink Tone,
//   Call, Location, County, State, Modes, Digital Access
//
// We map this to Thetis MemoryRecord fields:
//   Output Freq          -> RXFreq (converted to MHz, stored as double)
//   |Input - Output|     -> RPTROffset (MHz)
//   Offset "+" / "-"     -> RPTR (High / Low); blank -> Simplex
//   Uplink Tone (CTCSS)  -> CTCSSFreq + CTCSSOn=true; "CSQ"/blank -> CTCSSOn=false
//   Call + Location      -> Name
//   State (or County)    -> Group
//   Modes "FM..."        -> DSPMode.FM (DMR / D-Star / P25 / etc. skipped)
//=================================================================

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;
using Microsoft.VisualBasic.FileIO;

namespace Thetis
{
    public static class RepeaterBookImport
    {
        private class Row
        {
            public double OutputFreqMHz;
            public double InputFreqMHz;
            public string OffsetDir = "";
            public string UplinkTone = "";
            public string Call = "";
            public string Location = "";
            public string County = "";
            public string State = "";
            public string Modes = "";
        }

        public class ImportResult
        {
            public int Imported;
            public int SkippedDuplicate;
            public int SkippedDigitalOnly;
            public int SkippedDcs;
            public int SkippedMalformed;

            public override string ToString()
            {
                var sb = new StringBuilder();
                sb.AppendLine("Imported " + Imported + " memory channels.");
                if (SkippedDuplicate > 0)     sb.AppendLine("Skipped " + SkippedDuplicate + " duplicates (same freq + CTCSS already present).");
                if (SkippedDigitalOnly > 0)   sb.AppendLine("Skipped " + SkippedDigitalOnly + " digital-only rows (DMR / D-Star / P25 / C4FM / YSF / NXDN).");
                if (SkippedDcs > 0)           sb.AppendLine("Skipped " + SkippedDcs + " rows that use DCS (digital coded squelch) — Artemis does not yet support DCS.");
                if (SkippedMalformed > 0)     sb.AppendLine("Skipped " + SkippedMalformed + " malformed rows.");
                return sb.ToString();
            }
        }

        /// <summary>
        /// Top-level entry. Shows an OpenFileDialog, parses the CSV, prompts for
        /// a group-name override, and appends rows to console.MemoryList.
        /// </summary>
        public static void ImportInteractive(Console console, IWin32Window owner)
        {
            string path;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title  = "Import from RepeaterBook CSV export";
                ofd.Filter = "RepeaterBook CSV (*.csv)|*.csv|All files (*.*)|*.*";
                if (ofd.ShowDialog(owner) != DialogResult.OK) return;
                path = ofd.FileName;
            }

            List<Row> rows;
            try
            {
                rows = ParseCsv(path);
            }
            catch (Exception ex)
            {
                MessageBox.Show(owner,
                    "Could not parse the CSV file:\n\n" + ex.Message + "\n\n" +
                    "Make sure the file is a CSV export from RepeaterBook.com and " +
                    "has the standard header row (Output Freq, Input Freq, Offset, " +
                    "Uplink Tone, Downlink Tone, Call, Location, County, State, Modes, Digital Access).",
                    "RepeaterBook Import",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (rows.Count == 0)
            {
                MessageBox.Show(owner,
                    "No data rows found in the CSV.",
                    "RepeaterBook Import",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            // Default group = state name of the majority of rows. User can override.
            string defaultGroup = rows
                .GroupBy(r => r.State ?? "")
                .OrderByDescending(g => g.Count())
                .Select(g => g.Key)
                .FirstOrDefault() ?? "";

            string group = PromptForGroup(owner, rows.Count, defaultGroup);
            if (group == null) return;  // user cancelled

            var result = AppendToMemoryList(console, rows, group);

            MessageBox.Show(owner,
                result.ToString(),
                "RepeaterBook Import",
                MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        //-------------------------------------------------------------
        // CSV parsing
        //-------------------------------------------------------------
        private static List<Row> ParseCsv(string path)
        {
            var result = new List<Row>();

            using (var parser = new TextFieldParser(path))
            {
                parser.TextFieldType = FieldType.Delimited;
                parser.SetDelimiters(",");
                parser.HasFieldsEnclosedInQuotes = true;
                parser.TrimWhiteSpace = true;

                if (parser.EndOfData) return result;

                // Header row — map column names to indices so we survive
                // RepeaterBook adding / reordering columns.
                string[] headers = parser.ReadFields();
                var idx = BuildColumnIndex(headers);

                int iOut    = idx.Get("Output Freq", -1);
                int iIn     = idx.Get("Input Freq", -1);
                int iOffDir = idx.Get("Offset", -1);
                int iUp     = idx.Get("Uplink Tone", -1);
                int iCall   = idx.Get("Call", -1);
                int iLoc    = idx.Get("Location", -1);
                int iCounty = idx.Get("County", -1);
                int iState  = idx.Get("State", -1);
                int iModes  = idx.Get("Modes", -1);

                if (iOut < 0 || iIn < 0)
                    throw new InvalidDataException(
                        "Missing required columns 'Output Freq' or 'Input Freq' in header row.");

                while (!parser.EndOfData)
                {
                    string[] f;
                    try { f = parser.ReadFields(); }
                    catch { continue; }
                    if (f == null) continue;

                    var r = new Row();
                    double d;
                    if (iOut >= 0 && iOut < f.Length &&
                        double.TryParse(f[iOut], NumberStyles.Float, CultureInfo.InvariantCulture, out d))
                        r.OutputFreqMHz = d;
                    if (iIn >= 0 && iIn < f.Length &&
                        double.TryParse(f[iIn], NumberStyles.Float, CultureInfo.InvariantCulture, out d))
                        r.InputFreqMHz = d;

                    r.OffsetDir = Safe(f, iOffDir);
                    r.UplinkTone = Safe(f, iUp);
                    r.Call       = Safe(f, iCall);
                    r.Location   = Safe(f, iLoc);
                    r.County     = Safe(f, iCounty);
                    r.State      = Safe(f, iState);
                    r.Modes      = Safe(f, iModes);

                    result.Add(r);
                }
            }
            return result;
        }

        private static string Safe(string[] f, int i)
        {
            if (i < 0 || i >= f.Length || f[i] == null) return "";
            return f[i].Trim();
        }

        private class ColumnIndex : Dictionary<string, int>
        {
            public int Get(string name, int fallback)
            {
                int v;
                return TryGetValue(name, out v) ? v : fallback;
            }
        }

        private static ColumnIndex BuildColumnIndex(string[] headers)
        {
            var ci = new ColumnIndex();
            for (int i = 0; i < headers.Length; i++)
            {
                var h = (headers[i] ?? "").Trim();
                if (h.Length > 0 && !ci.ContainsKey(h)) ci.Add(h, i);
            }
            return ci;
        }

        //-------------------------------------------------------------
        // Row -> MemoryRecord classification + mapping
        //-------------------------------------------------------------
        private enum RowDecision { Accept, DigitalOnly, Dcs, Malformed }

        private static RowDecision Classify(Row r, double[] standardTones, out double ctcssHz, out bool ctcssOn)
        {
            // The DataGridView CTCSS column is a combo-box restricted to the
            // standard EIA tone set — any value outside that set throws
            // "DataGridViewComboBoxCell value is not valid" when the row
            // binds. So we always store a value that's IN the set; CTCSSOn
            // controls whether the tone is actually used on TX.
            double defaultTone = SnapToStandard(100.0, standardTones);  // 100.0 is in EIA set

            ctcssHz = defaultTone;
            ctcssOn = false;

            if (r.OutputFreqMHz <= 0.0)
                return RowDecision.Malformed;

            string modes = (r.Modes ?? "").ToUpperInvariant();
            bool hasFm = modes.Contains("FM");  // catches "FM", "FM ", "FM EchoLink", "FM IRLP"
            if (!hasFm) return RowDecision.DigitalOnly;

            // CTCSS / DCS parsing
            string t = (r.UplinkTone ?? "").Trim();
            if (t.Length == 0 || t.Equals("CSQ", StringComparison.OrdinalIgnoreCase))
            {
                ctcssOn = false;
                ctcssHz = defaultTone;  // safe placeholder so the combo renders
            }
            else if (t.StartsWith("D", StringComparison.OrdinalIgnoreCase))
            {
                // "D023", "D025N", "D411I" — DCS (digital coded squelch). Not supported.
                return RowDecision.Dcs;
            }
            else
            {
                double f;
                if (double.TryParse(t, NumberStyles.Float, CultureInfo.InvariantCulture, out f) && f > 0.0)
                {
                    ctcssOn = true;
                    ctcssHz = SnapToStandard(f, standardTones);  // snap non-standard tones to nearest
                }
                else
                {
                    ctcssOn = false;
                    ctcssHz = defaultTone;
                }
            }

            return RowDecision.Accept;
        }

        private static double SnapToStandard(double hz, double[] table)
        {
            if (table == null || table.Length == 0) return hz;
            double best = table[0];
            double bestDiff = Math.Abs(hz - best);
            for (int i = 1; i < table.Length; i++)
            {
                double d = Math.Abs(hz - table[i]);
                if (d < bestDiff) { bestDiff = d; best = table[i]; }
            }
            return best;
        }

        private static FMTXMode ParseOffsetDir(string s)
        {
            s = (s ?? "").Trim();
            if (s == "+") return FMTXMode.High;
            if (s == "-") return FMTXMode.Low;
            return FMTXMode.Simplex;
        }

        private static string ComposeName(Row r)
        {
            string call = (r.Call ?? "").Trim();
            string loc  = (r.Location ?? "").Trim();
            if (call.Length > 0 && loc.Length > 0) return call + " — " + loc;
            if (call.Length > 0) return call;
            if (loc.Length  > 0) return loc;
            return "Repeater " + r.OutputFreqMHz.ToString("F4", CultureInfo.InvariantCulture);
        }

        //-------------------------------------------------------------
        // Append rows to MemoryList, de-duping by (RXFreq, CTCSSFreq)
        //-------------------------------------------------------------
        private static ImportResult AppendToMemoryList(Console console, List<Row> rows, string group)
        {
            var result = new ImportResult();
            var list = console.MemoryList.List;
            double[] standardTones = console.CTCSS_array;

            // Snapshot the current Drive slider value and apply it to every
            // imported memory. Thetis's MemoryRecord.Power is restored on
            // RecallMemory, so without this snapshot every imported row would
            // zero the drive on selection (MemoryRecord default is 0).
            int importDrive = console.PWR;

            // Build duplicate set from existing records.
            // Repeater identity = (freq, CTCSS, name) — NOT just (freq, CTCSS).
            // Multiple real-world repeaters legitimately share the same
            // frequency + PL tone; they differ only by callsign/location.
            var existing = new HashSet<string>();
            foreach (MemoryRecord m in list)
                existing.Add(MakeDupKey(m.RXFreq, m.CTCSSFreq, m.CTCSSOn, m.Name));

            foreach (var r in rows)
            {
                double ctcssHz; bool ctcssOn;
                switch (Classify(r, standardTones, out ctcssHz, out ctcssOn))
                {
                    case RowDecision.DigitalOnly: result.SkippedDigitalOnly++; continue;
                    case RowDecision.Dcs:         result.SkippedDcs++;         continue;
                    case RowDecision.Malformed:   result.SkippedMalformed++;   continue;
                }

                double offsetMhz = Math.Abs(r.InputFreqMHz - r.OutputFreqMHz);
                FMTXMode rpt = ParseOffsetDir(r.OffsetDir);
                if (offsetMhz < 1e-6) rpt = FMTXMode.Simplex;

                string rowName = ComposeName(r);
                string key = MakeDupKey(r.OutputFreqMHz, ctcssHz, ctcssOn, rowName);
                if (existing.Contains(key))
                {
                    result.SkippedDuplicate++;
                    continue;
                }
                existing.Add(key);

                // IMPORTANT: txfreq is the literal TX frequency used in SPLIT
                // mode. For standard FM repeater operation (split=false), the
                // actual TX frequency is computed at keydown time from
                // (VFOAFreq + RPTR direction × RPTROffset), so the TXFreq field
                // should mirror RXFreq — otherwise RecallMemory's final
                // `TXFreq = record.TXFreq` assignment clobbers VFOAFreq via the
                // TXFreq setter (see console.cs TXFreq setter), which flips
                // VFO A to the repeater input frequency.
                var rec = new MemoryRecord(
                    /*group*/      group,
                    /*rxfreq*/     r.OutputFreqMHz,
                    /*name*/       rowName,
                    /*dspmode*/    DSPMode.FM,
                    /*scan*/       true,
                    /*tune_step*/  "10Hz",
                    /*rpt_mode*/   rpt,
                    /*fm_tx_off*/  offsetMhz,
                    /*ctcss_on*/   ctcssOn,
                    /*ctcss_freq*/ ctcssHz,
                    /*power*/      importDrive,
                    /*deviation*/  5000,
                    /*split*/      false,
                    /*txfreq*/     r.OutputFreqMHz,
                    /*filter*/     Filter.F1,
                    /*flow*/       -8000,
                    /*fhigh*/       8000,
                    /*comments*/   BuildComments(r),
                    /*agc_mode*/   AGCMode.MED,
                    /*agc_thresh*/ 80,
                    /*StartDate*/  DateTime.Now,
                    /*SchedOn*/    false,
                    /*Duration*/   30,
                    /*Repeating*/  false,
                    /*Recording*/  false,
                    /*Repeatingm*/ false,
                    /*Extra*/      0);

                list.Add(rec);
                result.Imported++;
            }

            console.MemoryList.Save();
            return result;
        }

        private static string MakeDupKey(double freqMHz, double ctcssHz, bool ctcssOn, string name)
        {
            // Round freq to 5 Hz to avoid float-precision misses.
            long fKHz = (long)Math.Round(freqMHz * 1000.0 * 100.0);
            long cHz  = ctcssOn ? (long)Math.Round(ctcssHz * 10.0) : 0;
            string n = (name ?? "").Trim().ToUpperInvariant();
            return fKHz + "/" + cHz + "/" + n;
        }

        private static string BuildComments(Row r)
        {
            var parts = new List<string>();
            if (!string.IsNullOrWhiteSpace(r.County)) parts.Add(r.County);
            if (!string.IsNullOrWhiteSpace(r.State))  parts.Add(r.State);
            string loc = string.Join(", ", parts);
            string modes = (r.Modes ?? "").Trim();
            if (modes.Length > 0 && !modes.Equals("FM", StringComparison.OrdinalIgnoreCase))
                loc = (loc.Length > 0 ? loc + "  •  " : "") + modes;
            return loc;
        }

        //-------------------------------------------------------------
        // Small modal dialog: confirm import + let user override group
        //-------------------------------------------------------------
        private static string PromptForGroup(IWin32Window owner, int rowCount, string defaultGroup)
        {
            using (var dlg = new Form())
            {
                dlg.Text = "Import from RepeaterBook";
                dlg.FormBorderStyle = FormBorderStyle.FixedDialog;
                dlg.StartPosition = FormStartPosition.CenterParent;
                dlg.MinimizeBox = false;
                dlg.MaximizeBox = false;
                dlg.ShowInTaskbar = false;
                dlg.ClientSize = new System.Drawing.Size(400, 150);

                var lblInfo = new Label
                {
                    Text = string.Format(
                        "Found {0} rows. Digital-only modes (DMR / D-Star / P25 / etc.) " +
                        "will be skipped automatically.\r\n\r\nMemory channels will be added " +
                        "to the group below. You can edit individual rows after import.",
                        rowCount),
                    Location = new System.Drawing.Point(12, 10),
                    Size = new System.Drawing.Size(376, 60)
                };

                var lblGroup = new Label
                {
                    Text = "Group:",
                    Location = new System.Drawing.Point(12, 82),
                    AutoSize = true
                };
                var txt = new TextBox
                {
                    Text = defaultGroup,
                    Location = new System.Drawing.Point(60, 78),
                    Size = new System.Drawing.Size(328, 22)
                };

                var btnOk = new Button
                {
                    Text = "Import",
                    DialogResult = DialogResult.OK,
                    Location = new System.Drawing.Point(232, 115),
                    Size = new System.Drawing.Size(75, 23)
                };
                var btnCancel = new Button
                {
                    Text = "Cancel",
                    DialogResult = DialogResult.Cancel,
                    Location = new System.Drawing.Point(313, 115),
                    Size = new System.Drawing.Size(75, 23)
                };

                dlg.Controls.Add(lblInfo);
                dlg.Controls.Add(lblGroup);
                dlg.Controls.Add(txt);
                dlg.Controls.Add(btnOk);
                dlg.Controls.Add(btnCancel);
                dlg.AcceptButton = btnOk;
                dlg.CancelButton = btnCancel;

                return (dlg.ShowDialog(owner) == DialogResult.OK) ? (txt.Text ?? "").Trim() : null;
            }
        }
    }
}

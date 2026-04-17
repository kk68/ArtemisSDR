/*  clsDiscord.cs  (ArtemisSDR stub)
 *
 *  The original Thetis Discord bot integration has been removed for ArtemisSDR.
 *  Upstream Thetis fetched a config file from ramdor/Thetis on GitHub and
 *  connected to a specific Discord server on startup, which is not
 *  appropriate for this fork.
 *
 *  This stub preserves the `ThetisBotDiscord` public API surface so that
 *  callers elsewhere in the codebase (MeterManager, setup, console) still
 *  compile, but every entry point is inert:
 *    - No network calls
 *    - No Discord.Net dependencies
 *    - IsReady / IsConnected always false
 *    - Event handlers exist but never fire
 *    - SendMessage / GetMessagesString / SetCallsign / SetEnabled are no-ops
 *
 *  Removal of the dormant consumer code (clsDiscordButtonBox, AddDiscordButtons,
 *  DISCORD_BUTTONS meter-type switch branches) is deferred to a later release.
 */

using System;
using System.Threading.Tasks;

namespace Thetis
{
    public static class ThetisBotDiscord
    {
        public delegate void Connected();
        public delegate void Disconnected();
        public delegate void Ready();

        public static Connected ConnectedHandlers;
        public static Disconnected DisconnectedHandlers;
        public static Ready ReadyHandlers;

        public static bool IsConnected => false;
        public static bool IsReady => false;

        public static string UniqueIDs { get { return ""; } set { } }
        public static string Filter    { get { return ""; } set { } }
        public static string Ignore    { get { return ""; } set { } }
        public static bool IncludeTimeStamp { get { return false; } set { } }

        public static void Shutdown() { }

        public static Task SendMessage(string message, ulong channel_id = 0)
        {
            return Task.CompletedTask;
        }

        public static string GetMessagesString(ulong channel_id, int message = 0, bool include_author = true)
        {
            return "";
        }

        public static bool IsValidCallsign(string callsign, out string country)
        {
            country = "";
            return false;
        }

        public static void SetCallsign(string callsign) { }
        public static void SetEnabled(bool enabled) { }
    }
}

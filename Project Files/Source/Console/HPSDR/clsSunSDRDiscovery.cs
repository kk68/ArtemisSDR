/*  clsSunSDRDiscovery.cs

SunSDR2 DX native-protocol discovery for Artemis.

The SunSDR2 DX does NOT answer HPSDR-style `effeeffe...` broadcasts. Its
discovery path is a separate 24-byte UDP broadcast to port 50001 with
header `32 ff 00 1a`. The radio replies with a 24-byte packet starting
with `32 ff 01 1a` that carries the radio IP (bytes 10-13 big-endian)
and the control port (bytes 18-19 little-endian, = 0xc351 = 50001).

Wire layout confirmed from capture `20260311_151107_sunsdr2dx_discovery`:
  offset 0-3  : 32 ff 01 1a
  offset 4-7  : const (0x00000248, meaning unknown; may be header len)
  offset 8-9  : unknown
  offset 10-13: radio IPv4 (big-endian)
  offset 14-17: radio IPv4 (big-endian, repeated)
  offset 18-19: control port (little-endian u16)
  offset 20-21: flag / version (observed 0x0001)
  offset 22-23: counter / checksum (decreases over time; not validated)

The outbound query format is known from static analysis of SunSDR2dx.dll:
24 bytes, magic at offset 0, 16-bit checksum at offset 0x16. The middle
bytes may be left zero for pure discovery.
*/

using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;

namespace Thetis
{
    public sealed class SunSDRDiscoveryService
    {
        public const int DefaultControlPort = 50001;
        private const int QueryPacketLength = 24;

        // Query magic: `32 ff 00 1a`. The `1a` byte is understood to be a
        // model-family selector (confirmed 1a == SunSDR2 DX). Older SunSDR2
        // variants may honour other values here; we broadcast the DX family
        // query for now because that's the one we know is accepted.
        private static readonly byte[] QueryMagic = new byte[] { 0x32, 0xff, 0x00, 0x1a };
        // Reply magic: `32 ff 01 XX`. We only require the first 3 bytes to
        // match — the XX model byte varies by radio family (SunSDR2 DX = 0x1a
        // from our captures; SunSDR2 classic / PRO / QRP likely different).
        // Accepting any fourth byte lets older SunSDR2 variants appear in
        // the Discover list so we at least know they're on the wire.
        private static readonly byte[] ReplyMagic = new byte[] { 0x32, 0xff, 0x01 };

        public List<RadioInfo> Probe(IPAddress localIp, int timeoutMs)
        {
            return Probe(localIp, timeoutMs, DefaultControlPort);
        }

        /* Probe with an explicit target control port. Use this when the
         * operator has configured the radio's control port to something
         * other than the 50001 default (Setup -> Fixed listen port). */
        public List<RadioInfo> Probe(IPAddress localIp, int timeoutMs, int targetPort)
        {
            List<RadioInfo> found = new List<RadioInfo>();
            if (localIp == null) return found;
            if (timeoutMs < 100) timeoutMs = 100;
            if (targetPort <= 0 || targetPort > 65535) targetPort = DefaultControlPort;

            Socket sock = null;
            try
            {
                sock = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
                sock.EnableBroadcast = true;
                sock.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                sock.Bind(new IPEndPoint(localIp, 0));

                byte[] query = buildQueryPacket();
                IPEndPoint dest = new IPEndPoint(IPAddress.Broadcast, targetPort);
                sock.SendTo(query, dest);

                byte[] rxBuf = new byte[1500];
                DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);

                HashSet<string> seenIps = new HashSet<string>();

                while (DateTime.UtcNow < deadline)
                {
                    int remainingMs = (int)(deadline - DateTime.UtcNow).TotalMilliseconds;
                    if (remainingMs <= 0) break;

                    sock.ReceiveTimeout = remainingMs;

                    EndPoint from = new IPEndPoint(IPAddress.Any, 0);
                    int n;
                    try
                    {
                        n = sock.ReceiveFrom(rxBuf, 0, rxBuf.Length, SocketFlags.None, ref from);
                    }
                    catch (SocketException)
                    {
                        break;
                    }

                    if (n < QueryPacketLength) continue;
                    if (!matchesMagic(rxBuf, ReplyMagic)) continue;

                    IPEndPoint src = from as IPEndPoint;
                    if (src == null) continue;
                    /* Accept replies from the port we queried; the radio
                     * responds from its listen port which equals our target. */
                    if (src.Port != targetPort) continue;

                    // Byte 3 is the model-family selector (0x1a confirmed
                    // == SunSDR2 DX). Surface it via Debug so we can tell
                    // what models are being discovered on different setups.
                    byte modelByte = rxBuf[3];
                    System.Diagnostics.Debug.WriteLine(
                        "[SunSDRDiscovery] Reply from " + src.Address +
                        ":" + src.Port + " modelByte=0x" + modelByte.ToString("x2"));

                    IPAddress radioIp = parseIpBE(rxBuf, 10);
                    int port = rxBuf[18] | (rxBuf[19] << 8);
                    if (port == 0) port = targetPort;

                    string key = radioIp.ToString();
                    if (!seenIps.Add(key)) continue;

                    RadioInfo info = new RadioInfo()
                    {
                        Protocol = RadioDiscoveryRadioProtocol.P1,
                        IpAddress = radioIp,
                        MacAddress = "",
                        DeviceType = HPSDRHW.SunSDR,
                        CodeVersion = 0,
                        BetaVersion = 0,
                        Protocol2Supported = 0,
                        NumRxs = 2,
                        DiscoveryPortBase = port,
                        PortCount = 1,
                        IsApipaRadio = false,
                        IsBusy = false,
                        IsCustom = false,
                    };
                    found.Add(info);
                }
            }
            catch (Exception)
            {
                // swallow; caller treats empty result as "no radios"
            }
            finally
            {
                if (sock != null)
                {
                    try { sock.Close(); } catch { }
                }
            }

            return found;
        }

        private static byte[] buildQueryPacket()
        {
            byte[] pkt = new byte[QueryPacketLength];
            Buffer.BlockCopy(QueryMagic, 0, pkt, 0, QueryMagic.Length);
            // Middle bytes 4..21 stay zero. Checksum at 22-23: sum of 16-bit LE
            // words over bytes 0..21, one's complement. Radios accept zero
            // checksum in practice, but compute it in case firmware gets stricter.
            int sum = 0;
            for (int i = 0; i < 22; i += 2)
            {
                int w = pkt[i] | (pkt[i + 1] << 8);
                sum += w;
                if ((sum & 0x10000) != 0) sum = (sum & 0xFFFF) + 1;
            }
            int cksum = (~sum) & 0xFFFF;
            pkt[22] = (byte)(cksum & 0xFF);
            pkt[23] = (byte)((cksum >> 8) & 0xFF);
            return pkt;
        }

        private static bool matchesMagic(byte[] buf, byte[] magic)
        {
            if (buf == null || buf.Length < magic.Length) return false;
            for (int i = 0; i < magic.Length; i++)
                if (buf[i] != magic[i]) return false;
            return true;
        }

        private static IPAddress parseIpBE(byte[] buf, int offset)
        {
            byte[] ip = new byte[4];
            ip[0] = buf[offset + 0];
            ip[1] = buf[offset + 1];
            ip[2] = buf[offset + 2];
            ip[3] = buf[offset + 3];
            return new IPAddress(ip);
        }
    }
}

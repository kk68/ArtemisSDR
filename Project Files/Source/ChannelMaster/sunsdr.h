/*  sunsdr.h

SunSDR2 DX native protocol support for Thetis.

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2026 Kosta Kanchev (K0KOZ)

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

*/
#pragma once

#include <winsock2.h>
#include <windows.h>

/* ---------- Protocol constants ---------- */

/* UDP ports */
#define SUNSDR_CONTROL_PORT     50001
#define SUNSDR_STREAM_PORT      50002

/* Packet magic prefix */
#define SUNSDR_MAGIC_0          0x32
#define SUNSDR_MAGIC_1          0xFF

/* Opcodes (control, port 50001) */
#define SUNSDR_OP_STATE_SYNC    0x01
#define SUNSDR_OP_POWER_OFF     0x02
/* 0x05 carries the preamp / attenuator state as a single u32. Earlier
 * code called this SUNSDR_OP_START_IQ because the init sequence pokes
 * it once; that name is misleading. EESDR3 cycles through 4 states via
 * this opcode: bit 7 = enable, low 2 bits = state index.
 *   0x80 -> -20 dB attenuator
 *   0x81 -> -10 dB attenuator
 *   0x82 ->   0 dB (bypass)
 *   0x83 -> +10 dB preamp
 * (captures 20260418_2041xx, see docs/protocol/att-wfm-findings.md). */
#define SUNSDR_OP_PREAMP_ATT    0x05
#define SUNSDR_OP_START_IQ      SUNSDR_OP_PREAMP_ATT  /* legacy alias */
#define SUNSDR_PREAMP_ATT_M20   0x80
#define SUNSDR_PREAMP_ATT_M10   0x81
#define SUNSDR_PREAMP_ATT_0     0x82
#define SUNSDR_PREAMP_ATT_P10   0x83
#define SUNSDR_OP_MOX_PTT       0x06
#define SUNSDR_OP_INFO_QUERY    0x07
#define SUNSDR_OP_FREQ_COMP     0x08
#define SUNSDR_OP_FREQ_PRIMARY  0x09
#define SUNSDR_OP_STATE_REQ_A   0x0E
#define SUNSDR_OP_STATE_REQ_B   0x10
#define SUNSDR_OP_RX_ANT        0x15
/* 0x17 was MISIDENTIFIED as MODE. Actual semantics per AM drive
 * calibration captures 2026-04-14: 0x17 payload byte sets radio TX
 * drive level. byte = round(sqrt(watts/100) * 255). Observed bytes:
 * 10W=0x50, 25W=0x80, 50W=0xB5, 75W=0xDD, 100W=0xFF. Radio mode is
 * set via 0x20 config block, not 0x17. */
#define SUNSDR_OP_DRIVE         0x17
#define SUNSDR_OP_MODE          0x17  /* DEPRECATED alias - do not use */
#define SUNSDR_OP_KEEPALIVE     0x18
#define SUNSDR_OP_RX2_ENABLE    0x1B
#define SUNSDR_OP_QUERY_FIXED   0x1A
#define SUNSDR_OP_ANT_PREAMBLE  0x1E
#define SUNSDR_OP_CONFIG_BLOCK  0x20
#define SUNSDR_OP_STREAM_XPORT  0x22
#define SUNSDR_OP_PA_ENABLE     0x24
#define SUNSDR_OP_EXT_CTRL      0x27
#define SUNSDR_OP_STATE_REPEAT  0x5A
#define SUNSDR_OP_POWER_WAKE    0x5F

/* Opcodes for IQ stream (port 50002)
 *   0xFE = RX-state / TX-idle keepalive (byte8=0x01, byte9=0x00)
 *   0xFD = TX-active, live voice IQ audio (byte8=0x02, byte9=0x01)
 * Verified from ExpertSDR3 live voice MOX capture on 2026-04-13:
 * opcode switches FE->FD when MOX is asserted and FD->FE on release.
 */
#define SUNSDR_OP_IQ_RX_IDLE    0xFE
#define SUNSDR_OP_IQ_TX_ACTIVE  0xFD
/* Backward-compat alias: existing RX parse paths still use this name. */
#define SUNSDR_OP_IQ_STREAM     SUNSDR_OP_IQ_RX_IDLE

/* IQ stream format */
#define SUNSDR_IQ_PKT_SIZE      1210
#define SUNSDR_IQ_HDR_SIZE      10
#define SUNSDR_IQ_PAYLOAD_SIZE  1200
#define SUNSDR_IQ_COMPLEX_PER_PKT  200   /* 24-bit interleaved I/Q pairs */
#define SUNSDR_IQ_BYTES_PER_IQ  6        /* 3 bytes I + 3 bytes Q */

/* DDC companion frequency offsets (Hz) */
#define SUNSDR_DDC0_OFFSET_HZ   92500
#define SUNSDR_DDC1_OFFSET_HZ   22000

/* Frequency scaling: wire value = Hz * FREQ_SCALE */
#define SUNSDR_FREQ_SCALE       10

/* Mode codes
 * Verified from ExpertSDR3 full AM session capture 2026-04-13:
 * AM = 0x28 (matched LSB->AM write; produced RF on-air during MOX).
 * The earlier USB->AM capture showed 0x00 but that transition may not have
 * actually resulted in working AM TX. Use 0x28.
 */
#define SUNSDR_MODE_AM          0x28
#define SUNSDR_MODE_LSB         0xBC
#define SUNSDR_MODE_USB         0xF5

/* Control packet header size */
#define SUNSDR_CTL_HDR_SIZE     18

/* PureSignal feedback ring depth.
 * Stores 39 kHz-decimated TX baseband samples that get paired with the
 * radio->host 0xFD packets arriving during MOX. Sized for ~21 packet-times
 * worth (4096 samples / 200 per pkt = 20.48 packets ~= 105 ms at 195 pps),
 * comfortably above the worst-case round-trip delay between sending a TX
 * 0xFD packet and receiving its corresponding feedback 0xFD packet. */
#define SUNSDR_PS_RING_SIZE     4096
#define SUNSDR_PS_RING_MASK     (SUNSDR_PS_RING_SIZE - 1)  /* must be power-of-2 minus 1 */

/* PS-A wire feedback rate. 195.3125 packets/sec * 200 samples = 39062.5 Hz.
 * This is what we tell WDSP's PS algorithm via SetPSFeedbackRate. The PS
 * correlation/regression step is rate-agnostic in the algorithmic sense
 * but uses this value to size internal delay structures correctly. */
#define SUNSDR_PS_FEEDBACK_RATE 39062

/* Software feedback-level scaling.
 *
 * The radio->host 0xFD payload during MOX delivers RX-antenna IQ at a
 * gain that doesn't track the user's ATT button — wire-confirmed
 * 2026-04-30: changed ATT +10 -> 0 produced zero change in GetPk.
 * The feedback path bypasses the normal RX preamp.
 *
 * PS-A's algorithm target peak (set via SetPSHWPeak): 0.2899.
 * Observed feedback peak on barefoot SUNSDR2DX + Acom 1010 at 14W drive: ~1.11.
 * Ratio 1.11 / 0.2899 = 3.83  ->  scale factor 0.26 brings peaks into target range.
 *
 * Anan radios bring this ratio in via Auto-Attenuate stepping the on-board
 * 0-31 dB step attenuator. SunSDR has no such control reachable from PS-A,
 * so we scale in software here. PSCC's amplitude correction is ratio-based,
 * so a constant scale factor on the feedback channel just folds into the
 * LUT and has no effect on predistortion accuracy.
 *
 * Tune empirically: too high -> GetPk > 1 -> samples rejected as clipping.
 * Too low -> algorithm reports "feedback level too low" warning. Aim for
 * peaks around SetPk = 0.29.
 */
#define SUNSDR_PS_FB_SCALE 0.26

/* ---------- State ---------- */

typedef struct _sunsdr_state
{
    /* Sockets */
    SOCKET ctrlSock;
    SOCKET streamSock;

    /* Radio address */
    struct sockaddr_in radioAddr;
    char radioIP[64];
    int ctrlPort;
    int streamPort;

    /* Thread handles */
    HANDLE hReadThread;
    HANDLE hKeepaliveThread;
    volatile int keepRunning;

    /* Current state */
    int currentRx1FreqHz;
    int currentRx2FreqHz;
    int currentTxFreqHz;
    int currentMode;
    int currentPTT;
    int currentRX2Enabled;
    int currentRxAntenna;
    int currentTxAntenna;
    int currentPAEnabled;
    /* WDSP-ready gate. 0 at init, flipped to 1 by C# after
     * WDSP.SetChannelState(RX1, 1, 1) completes in
     * chkPower_CheckedChanged. SunSDRReadThread drops xrouter dispatch
     * while this is 0, preventing the cold-start race where WDSP
     * latches a default bad state from the first IQ packets. */
    volatile LONG rxWdspReady;
    int currentTune;
    /* Band-class tracker for the RX front-end. 0 = HF direct-sample
     * path (up to 61.44 MHz ADC Nyquist), 1 = VHF down-converter path
     * (144-148 MHz 2m band). Flipped by SunSDRSetFreq when the target
     * frequency crosses the band-class boundary; the change triggers
     * the 0x1E / 0x15 / 0x22 / 0x20 band-switch prelude on the wire
     * (see vhf-findings.md). */
    int currentBandIsVhf;
    /* 1 when the current Thetis demod mode is FM (narrow-FM on SunSDR).
     * The CONFIG_BLOCK payload bytes 4-7 must be 0 for NFM and 1 for
     * wideband modes on 2m (vhf-findings.md). Tracked in SunSDRSetMode
     * and consumed by the band-change prelude. */
    int currentIsNfm;
    int currentDriveRaw;
    /* Mic source sent via OP 0x21. Decoded enum from EESDR3 captures
     * (2026-04-20): 0 = Mic1, 1 = Mic2. VAC + XLR enum values still TBD
     * (need future capture). Default = 1 (Mic2) matches the legacy
     * init-macro hardcoded value so existing users see no behavior change
     * until they explicitly pick a different source in the UI. */
    int currentMicSource;
    /* Hardware PTT mirror: latest ptt_bit value from 0x1F/01 telemetry.
     * 0 = released, 1 = pressed. Updated on every telemetry packet by
     * the read thread; polled by C# via nativeSunSDRGetHwPttState(). */
    volatile LONG hwPttState;

    /* ADC-overload indicator. 1 = at least one IQ sample exceeded the
     * clipping threshold (|sample| > 0.95 of full scale) within the
     * last ~2 seconds; 0 = clean. Set by the read thread per-packet
     * and held for ~1000 packets (~2 s) so the UI lamp doesn't flicker
     * on transient single-packet events. Polled by C# via
     * nativeSunSDRGetAdcOverload(). At ATT +10 with strong signals the
     * direct-sample ADC saturates and creates IMD spurs (radio-side,
     * same on EESDR3); this flag tells the operator to drop ATT. */
    volatile LONG adcOverloadActive;
    int lastTxWasTune;
    int pendingTuneReleaseConfig;
    int powered;
    char firmwareVersionText[64];
    char protocolText[32];
    char serialText[64];
    int txLockInitialized;
    unsigned int txSeq;
    unsigned int txAudioPackets;
    double txPhase;
    double txPrevI;
    double txPrevQ;
    int txAccumCount;
    /* Boxcar anti-alias accumulator for TX downsampler (192k -> 39k). */
    double txAccumBoxI;
    double txAccumBoxQ;
    int txAccumBoxN;

    /* IQ buffer (double pairs for xrouter) */
    double* rxBuf;
    int rxBufSize;
    double txAccumBuf[SUNSDR_IQ_COMPLEX_PER_PKT * 2];
    CRITICAL_SECTION txLock;

    /* ===== PureSignal (PS-A) feedback path =====
     * Two streams must be delivered to WDSP's pscc() correlate function in
     * lockstep: the predistorted TX baseband we sent (input reference) and
     * the actual PA output sample observed via the configured RX antenna's
     * T/R relay leakage (output observation). Both arrive at the wire at
     * 39 kHz; we pair them packet-by-packet and dispatch as an interleaved
     * 2-stream block to xrouter source 2.
     *
     * Writer (sunsdr_tx_outbound, runs on WDSP TX thread):
     *   for each 39 kHz output sample emitted to wire, also push to ring.
     *
     * Reader (RX read thread, on radio->host 0xFD packet arrival during MOX):
     *   drain SUNSDR_IQ_COMPLEX_PER_PKT samples, interleave with freshly
     *   decoded RX feedback IQ, dispatch via xrouter(NULL, 0, 2, 400, buf).
     *
     * If reader runs ahead of writer (TX ring underflow), we skip dispatch
     * for that packet — pscc handles intermittent gaps gracefully. If writer
     * laps reader (extreme overrun), the older samples get overwritten,
     * which is also harmless: pscc re-correlates on the next packet.
     */
    double psTxRingI[SUNSDR_PS_RING_SIZE];
    double psTxRingQ[SUNSDR_PS_RING_SIZE];
    /* Monotonic counters: writer increments after writing, reader increments
     * after reading. Available = (head - tail). Ring index = count & MASK
     * where MASK = SUNSDR_PS_RING_SIZE - 1 (size must be power of 2).
     * Two's-complement subtraction handles 32-bit wraparound correctly. */
    volatile LONG psTxRingHeadCount;
    volatile LONG psTxRingTailCount;

    /* Interleaved buffer for xrouter dispatch. Layout per router.c case 2:
     *   [RX_I, RX_Q, TX_I, TX_Q] x SUNSDR_IQ_COMPLEX_PER_PKT
     *   = 4 doubles per complex-sample-pair x 200 = 800 doubles.
     * Stream 0 of the dispatch = RX feedback (PA output observation).
     * Stream 1 of the dispatch = TX baseband (predistorter input reference).
     * Mapping enforced by SetPSRxIdx(0,0) + SetPSTxIdx(0,1) in cmaster.cs. */
    double psFeedbackBuf[SUNSDR_IQ_COMPLEX_PER_PKT * 4];

    /* Diagnostic counters (drained periodically to sdr_log) */
    volatile LONG psDispatchCount;       /* successful xrouter source=2 dispatches */
    volatile LONG psRingUnderflowCount;  /* RX FD arrived but TX ring < 200 samples */

} sunsdr_state_t;

/* ---------- Exported functions ---------- */

/* Lifecycle */
int  SunSDRInit(const char* radioIP, int ctrlPort, int streamPort);
void SunSDRDestroy(void);
int  SunSDRPowerOn(void);
void SunSDRPowerOff(void);

/* Control */
void SunSDRSetFreq(int receiver, int freqHz, int isTx);
void SunSDRSetMode(int mode);
void SunSDRSetPTT(int ptt);
void SunSDRSetRX2(int enabled);
void SunSDRSetTune(int tune);
/* Preamp/attenuator 4-state cycle. state: 0=-20 dB, 1=-10 dB,
 * 2=0 dB (bypass), 3=+10 dB preamp. No-op if out of range. */
void SunSDRSetPreampAtt(int state);
void SunSDRSetMicSource(int state);
int  SunSDRGetHwPttState(void);
int  SunSDRGetAdcOverload(void);
void SunSDRLogTuneState(const char* label, int chk_tun, int chk_mox, int tuning, int mox,
    int tx_dsp_mode, int current_dsp_mode, int postgen_run, int postgen_mode,
    double tone_freq, double tone_mag, int pulse_enabled, int pulse_on,
    int tune_drive_source, int pwr, int new_pwr);
void SunSDRLogTrace(const char* msg);
void SunSDRSetDrive(int raw);
void SunSDRSetAntenna(int antenna);
void SunSDRSetTxAntenna(int antenna);
void SunSDRSetPA(int enabled);
void SunSDRSetRxWdspReady(int ready);
int  SunSDRGetVersionText(char* buffer, int maxlen);
int  SunSDRGetProtocolText(char* buffer, int maxlen);
int  SunSDRGetSerialText(char* buffer, int maxlen);

/* IQ receive thread */
DWORD WINAPI SunSDRReadThread(LPVOID param);

/*  sunsdr.h

SunSDR2 native protocol support for Thetis.

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

/* ---------- State ---------- */

typedef enum _sunsdr_variant
{
    SUNSDR_VARIANT_DX = 0,
    SUNSDR_VARIANT_PRO = 1
} sunsdr_variant_t;

typedef struct _sunsdr_macro_step
{
    const char* hex;
    int len;
    int delay_us; /* microseconds */
} sunsdr_macro_step_t;

typedef struct _sunsdr_profile
{
    sunsdr_variant_t variant;
    const char* name;
    int defaultCtrlPort;
    int defaultStreamPort;
    unsigned char magic0;
    double rxNativeRate;
    int macroStartIqValue;
    int macroMicSourceValue; /* <0 => use UI-selected source */
} sunsdr_profile_t;

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
    sunsdr_variant_t variant;
    const sunsdr_profile_t* profile;

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

} sunsdr_state_t;

/* ---------- Exported functions ---------- */

/* Lifecycle */
int  SunSDRInit(const char* radioIP, int ctrlPort, int streamPort, int modelId);
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

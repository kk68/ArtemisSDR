/*  sunsdr.h

SunSDR2 DX native protocol support for Thetis.

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2026 Kosta

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
#define SUNSDR_OP_START_IQ      0x05
#define SUNSDR_OP_MOX_PTT       0x06
#define SUNSDR_OP_INFO_QUERY    0x07
#define SUNSDR_OP_FREQ_COMP     0x08
#define SUNSDR_OP_FREQ_PRIMARY  0x09
#define SUNSDR_OP_STATE_REQ_A   0x0E
#define SUNSDR_OP_STATE_REQ_B   0x10
#define SUNSDR_OP_RX_ANT        0x15
#define SUNSDR_OP_MODE          0x17
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
    int currentTune;
    int currentDriveRaw;
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
void SunSDRLogTuneState(const char* label, int chk_tun, int chk_mox, int tuning, int mox,
    int tx_dsp_mode, int current_dsp_mode, int postgen_run, int postgen_mode,
    double tone_freq, double tone_mag, int pulse_enabled, int pulse_on,
    int tune_drive_source, int pwr, int new_pwr);
void SunSDRLogTrace(const char* msg);
void SunSDRSetDrive(int raw);
void SunSDRSetAntenna(int antenna);
void SunSDRSetTxAntenna(int antenna);
void SunSDRSetPA(int enabled);
int  SunSDRGetVersionText(char* buffer, int maxlen);
int  SunSDRGetProtocolText(char* buffer, int maxlen);
int  SunSDRGetSerialText(char* buffer, int maxlen);

/* IQ receive thread */
DWORD WINAPI SunSDRReadThread(LPVOID param);

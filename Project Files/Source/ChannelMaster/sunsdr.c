/*  sunsdr.c

SunSDR2 DX native protocol implementation for Thetis.

All SunSDR-specific protocol logic is contained in this file.
This includes: discovery, bootstrap, power on/off, frequency,
mode, PTT, and IQ stream reception.

Protocol details derived from clean-room reverse engineering.
See: sunsdr-re/docs/protocol/ for canonical documentation.

Copyright (C) 2026 Kosta

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

*/

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <ws2tcpip.h>
#include "sunsdr.h"
#include "network.h"
#include "router.h"
#include "cmsetup.h"
#include "cmaster.h"
#include "cmasio.h"
#include "ivac.h"
#include "obbuffs.h"

/* Access VAC state array from ivac.c */
extern IVAC pvac[];

/* Debug log */
static FILE* sdr_log = NULL;
static void sdr_log_open(void) {
    if (!sdr_log) sdr_log = fopen("C:\\Users\\kosta\\ham\\SUNSDR\\sunsdr_debug.log", "a");
}
static void sdr_logf(const char* fmt, ...) {
    va_list ap;
    if (!sdr_log) sdr_log_open();
    if (!sdr_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(sdr_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(sdr_log, fmt, ap);
    va_end(ap);
    fflush(sdr_log);
}
#include "cmbuffs.h"

/* ========== Internal state ========== */

static sunsdr_state_t sdr;

/* ========== Resampler: 312500 → 192000 Hz ========== */

#define SUNSDR_NATIVE_RATE   312500.0
#define SUNSDR_TARGET_RATE   384000.0
#define SUNSDR_RESAMPLE_STEP (SUNSDR_NATIVE_RATE / SUNSDR_TARGET_RATE)  /* 0.8138 */
#define SUNSDR_RESAMPLE_MAX  512  /* max output samples per call (200 * 384/312.5 = ~246) */

typedef struct _sunsdr_resampler_state
{
    double phase;
    double prev_I;
    double prev_Q;
    double out[SUNSDR_RESAMPLE_MAX * 2];
} sunsdr_resampler_state_t;

static sunsdr_resampler_state_t resampler[2];

#define SUNSDR_TX_INPUT_RATE   192000.0
#define SUNSDR_TX_OUTPUT_RATE  312500.0
#define SUNSDR_TX_RESAMPLE_STEP (SUNSDR_TX_INPUT_RATE / SUNSDR_TX_OUTPUT_RATE)

/*
 * Resample nsamples complex pairs from in[] to resample_out[].
 * Returns number of complex output samples produced.
 * Uses linear interpolation with state preserved across calls.
 */
static int sunsdr_resample(int source, const double* in, int nsamples)
{
    int out_count = 0;
    int i;
    sunsdr_resampler_state_t* rs = &resampler[source];
    double prev_I = rs->prev_I;
    double prev_Q = rs->prev_Q;

    for (i = 0; i < nsamples; i++) {
        double cur_I = in[2 * i + 0];
        double cur_Q = in[2 * i + 1];

        /* Emit output samples while phase < 1.0 (we're within this input interval) */
        while (rs->phase < 1.0 && out_count < SUNSDR_RESAMPLE_MAX) {
            double frac = rs->phase;
            rs->out[2 * out_count + 0] = prev_I + frac * (cur_I - prev_I);
            rs->out[2 * out_count + 1] = prev_Q + frac * (cur_Q - prev_Q);
            out_count++;
            rs->phase += SUNSDR_RESAMPLE_STEP;
        }
        rs->phase -= 1.0;

        prev_I = cur_I;
        prev_Q = cur_Q;
    }

    rs->prev_I = prev_I;
    rs->prev_Q = prev_Q;

    return out_count;
}

/* ========== Helpers ========== */

static const double NORM = 1.0 / 2147483648.0;
/*
 * SunSDR2 DX needs extra native TX headroom at the top end, but a flat boost
 * overdrives low power settings badly. Apply a drive-dependent headroom curve:
 * near-unity at low drive, rising toward the top end where the radio still
 * needs help to reach full output.
 */
static double sunsdr_tx_full_scale_for_drive(double drive)
{
    double t;

    if (drive < 0.0) drive = 0.0;
    if (drive > 1.0) drive = 1.0;

    /*
     * Keep almost the entire range on the calibrated Thetis power path and only
     * add extra native headroom near the very top end. Current live SunSDR2 DX
     * measurements show:
     * - 0/10/25 are now close enough
     * - 100 is close enough
     * - 50/75 are still too hot
     *
     * So do not boost the middle. Reserve the boost for the final ~15% only.
     */
    if (drive <= 0.85)
        return 1.0;

    t = (drive - 0.85) / 0.15;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    return 1.0 + (0.34 * t * t * t);
}

static struct sockaddr_in sunsdr_stream_dest(void)
{
    struct sockaddr_in dest = sdr.radioAddr;
    dest.sin_port = htons((u_short)sdr.streamPort);
    return dest;
}

static int sunsdr_quantize24(double sample)
{
    int itemp;

    if (sample > 1.0) sample = 1.0;
    if (sample < -1.0) sample = -1.0;

    itemp = sample >= 0.0 ? (int)floor(sample * 8388607.0 + 0.5) :
        (int)ceil(sample * 8388607.0 - 0.5);

    if (itemp > 8388607) itemp = 8388607;
    if (itemp < -8388608) itemp = -8388608;

    return itemp;
}

static void sunsdr_build_iq_header(unsigned char* buf, int opcode, unsigned int seq)
{
    memset(buf, 0, SUNSDR_IQ_PKT_SIZE);
    buf[0] = SUNSDR_MAGIC_0;
    buf[1] = SUNSDR_MAGIC_1;
    buf[2] = (unsigned char)opcode;
    buf[3] = 0xFF;
    buf[4] = (unsigned char)(SUNSDR_IQ_PAYLOAD_SIZE & 0xFF);
    buf[5] = (unsigned char)((SUNSDR_IQ_PAYLOAD_SIZE >> 8) & 0xFF);
    buf[6] = (unsigned char)(seq & 0xFF);
    buf[7] = (unsigned char)((seq >> 8) & 0xFF);
    buf[8] = 0x01;
    buf[9] = 0x00;
}

static void sunsdr_build_tx_packet(unsigned char* buf, unsigned int seq, const double* iq)
{
    int i;
    unsigned char* payload = buf + SUNSDR_IQ_HDR_SIZE;

    sunsdr_build_iq_header(buf, SUNSDR_OP_IQ_STREAM, seq);

    for (i = 0; i < SUNSDR_IQ_COMPLEX_PER_PKT; i++) {
        int I = sunsdr_quantize24(iq[2 * i + 0]);
        int Q = sunsdr_quantize24(iq[2 * i + 1]);
        int k = i * SUNSDR_IQ_BYTES_PER_IQ;

        /* SunSDR wire order is Q first, then I (24-bit LE each). */
        payload[k + 0] = (unsigned char)(Q & 0xFF);
        payload[k + 1] = (unsigned char)((Q >> 8) & 0xFF);
        payload[k + 2] = (unsigned char)((Q >> 16) & 0xFF);
        payload[k + 3] = (unsigned char)(I & 0xFF);
        payload[k + 4] = (unsigned char)((I >> 8) & 0xFF);
        payload[k + 5] = (unsigned char)((I >> 16) & 0xFF);
    }
}

static void sunsdr_send_tx_packet(const double* iq)
{
    unsigned char txbuf[SUNSDR_IQ_PKT_SIZE];
    struct sockaddr_in dest;

    if (sdr.streamSock == INVALID_SOCKET || sdr.streamSock == 0)
        return;

    sunsdr_build_tx_packet(txbuf, sdr.txSeq++, iq);
    dest = sunsdr_stream_dest();
    sendto(sdr.streamSock, (const char*)txbuf, SUNSDR_IQ_PKT_SIZE, 0,
        (struct sockaddr*)&dest, sizeof(dest));
    sdr.txAudioPackets++;
}

static void sunsdr_queue_tx_packet_locked(double I, double Q)
{
    int idx = sdr.txAccumCount;

    sdr.txAccumBuf[2 * idx + 0] = I;
    sdr.txAccumBuf[2 * idx + 1] = Q;
    sdr.txAccumCount++;

    if (sdr.txAccumCount >= SUNSDR_IQ_COMPLEX_PER_PKT) {
        sunsdr_send_tx_packet(sdr.txAccumBuf);
        sdr.txAccumCount = 0;
    }
}

static void sunsdr_tx_outbound(int id, int nsamples, double* buff)
{
    int i;
    static unsigned int dbg_packets = 0;
    double drive = 1.0;
    int raw_drive = -1;
    double pre_peak = 0.0;
    double post_peak = 0.0;
    double pre_sum_sq = 0.0;
    double post_sum_sq = 0.0;
    double pre_rms = 0.0;
    double post_rms = 0.0;
    double full_scale = 1.0;

    (void)id;

    if (!sdr.txLockInitialized)
        return;

    EnterCriticalSection(&sdr.txLock);

    if (!sdr.powered || !sdr.currentPTT || buff == NULL || nsamples <= 0) {
        LeaveCriticalSection(&sdr.txLock);
        return;
    }

    raw_drive = sdr.currentDriveRaw;

    if (raw_drive >= 0) {
        drive = (double)raw_drive / 255.0;
        if (drive < 0.0) drive = 0.0;
        if (drive > 1.0) drive = 1.0;
    } else {
        drive = 1.0;
    }

    full_scale = sunsdr_tx_full_scale_for_drive(drive);

    for (i = 0; i < nsamples; i++) {
        double in_I = buff[2 * i + 0];
        double in_Q = buff[2 * i + 1];
        double cur_I = in_I * drive * full_scale;
        double cur_Q = in_Q * drive * full_scale;
        double in_mag = sqrt(in_I * in_I + in_Q * in_Q);
        double out_mag = sqrt(cur_I * cur_I + cur_Q * cur_Q);

        if (in_mag > pre_peak) pre_peak = in_mag;
        if (out_mag > post_peak) post_peak = out_mag;

        pre_sum_sq += (in_I * in_I) + (in_Q * in_Q);
        post_sum_sq += (cur_I * cur_I) + (cur_Q * cur_Q);

        while (sdr.txPhase < 1.0) {
            double frac = sdr.txPhase;
            double out_I = sdr.txPrevI + frac * (cur_I - sdr.txPrevI);
            double out_Q = sdr.txPrevQ + frac * (cur_Q - sdr.txPrevQ);
            sunsdr_queue_tx_packet_locked(out_I, out_Q);
            sdr.txPhase += SUNSDR_TX_RESAMPLE_STEP;
        }

        sdr.txPhase -= 1.0;
        sdr.txPrevI = cur_I;
        sdr.txPrevQ = cur_Q;
    }

    if (nsamples > 0) {
        pre_rms = sqrt(pre_sum_sq / (2.0 * (double)nsamples));
        post_rms = sqrt(post_sum_sq / (2.0 * (double)nsamples));
    }

    if (dbg_packets != sdr.txAudioPackets && (sdr.txAudioPackets <= 5 || sdr.txAudioPackets % 250 == 0)) {
        dbg_packets = sdr.txAudioPackets;
        sdr_logf("TX audio callback: nsamples=%d, tx_audio_packets=%u, seq=%u, drive=%.3f (%d), full_scale=%.2f, pre_peak=%.6f, pre_rms=%.6f, post_peak=%.6f, post_rms=%.6f\n",
            nsamples, sdr.txAudioPackets, sdr.txSeq, drive, raw_drive, full_scale, pre_peak, pre_rms, post_peak, post_rms);
    }

    LeaveCriticalSection(&sdr.txLock);
}

/* Send a raw hex packet to the radio control port and optionally wait for reply */
static int sunsdr_send_ctrl(const unsigned char* pkt, int len)
{
    return sendto(sdr.ctrlSock, (const char*)pkt, len, 0,
        (struct sockaddr*)&sdr.radioAddr, sizeof(sdr.radioAddr));
}

static int sunsdr_send_ctrl_and_recv(const unsigned char* pkt, int len,
    unsigned char* reply, int replymax)
{
    sunsdr_send_ctrl(pkt, len);

    /* Brief wait for ACK */
    fd_set fds;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000; /* 200ms */
    FD_ZERO(&fds);
    FD_SET(sdr.ctrlSock, &fds);

    if (select(0, &fds, NULL, NULL, &tv) > 0) {
        int n = recv(sdr.ctrlSock, (char*)reply, replymax, 0);
        return n;
    }
    return 0;
}

static int sunsdr_parse_hex(const char* hex, unsigned char* buf, int maxlen)
{
    int i;
    int hexlen = (int)strlen(hex);
    int len = hexlen / 2;

    if (len > maxlen)
        len = maxlen;

    for (i = 0; i < len; i++) {
        unsigned int byte = 0;
        sscanf(hex + (i * 2), "%02x", &byte);
        buf[i] = (unsigned char)byte;
    }

    return len;
}

static void sunsdr_send_hex_pkt_u8(const char* hex, int offset, unsigned char value)
{
    unsigned char pkt[128];
    unsigned char reply[128];
    int len = sunsdr_parse_hex(hex, pkt, (int)sizeof(pkt));

    if (offset >= 0 && offset < len)
        pkt[offset] = value;

    sunsdr_send_ctrl_and_recv(pkt, len, reply, (int)sizeof(reply));
}

static void sunsdr_send_hex_pkt(const char* hex, int len_hint)
{
    unsigned char pkt[128];
    unsigned char reply[128];
    int len = sunsdr_parse_hex(hex, pkt, (int)sizeof(pkt));

    if (len_hint > 0 && len_hint < len)
        len = len_hint;

    sunsdr_send_ctrl_and_recv(pkt, len, reply, (int)sizeof(reply));
}

/* Build a standard 18-byte control packet header */
static void sunsdr_build_header(unsigned char* buf, int opcode, int sub, int decl_len)
{
    memset(buf, 0, SUNSDR_CTL_HDR_SIZE);
    buf[0] = SUNSDR_MAGIC_0;
    buf[1] = SUNSDR_MAGIC_1;
    buf[2] = (unsigned char)opcode;
    buf[3] = 0x00;
    buf[4] = (unsigned char)(decl_len & 0xFF);
    buf[5] = (unsigned char)((decl_len >> 8) & 0xFF);
    buf[6] = (unsigned char)(sub & 0xFF);
    buf[7] = (unsigned char)((sub >> 8) & 0xFF);
    buf[8] = 0x00;
    buf[9] = 0x00;
    buf[10] = 0x01;
}

/* Build and send a frequency packet (opcode 0x09 or 0x08) */
static void sunsdr_send_freq_pkt(int opcode, int sub, int freqHz)
{
    unsigned char pkt[26];
    unsigned char reply[64];
    unsigned long long scaled = (unsigned long long)freqHz * SUNSDR_FREQ_SCALE;

    sunsdr_build_header(pkt, opcode, sub, 0x08);
    /* Payload: 8-byte u64 LE at offset 18 */
    pkt[18] = (unsigned char)(scaled & 0xFF);
    pkt[19] = (unsigned char)((scaled >> 8) & 0xFF);
    pkt[20] = (unsigned char)((scaled >> 16) & 0xFF);
    pkt[21] = (unsigned char)((scaled >> 24) & 0xFF);
    pkt[22] = (unsigned char)((scaled >> 32) & 0xFF);
    pkt[23] = (unsigned char)((scaled >> 40) & 0xFF);
    pkt[24] = (unsigned char)((scaled >> 48) & 0xFF);
    pkt[25] = (unsigned char)((scaled >> 56) & 0xFF);

    sunsdr_send_ctrl_and_recv(pkt, 26, reply, sizeof(reply));
}

static int sunsdr_map_ant_selector(int antenna)
{
    switch (antenna) {
    case 1: return 0x01;
    case 2: return 0x03;
    default: return 0;
    }
}

static int sunsdr_map_tx_ant_selector(int antenna)
{
    switch (antenna) {
    case 1: return 0x01;
    case 2: return 0x02;
    default: return 0;
    }
}

static int sunsdr_map_mode_code(int mode)
{
    switch (mode) {
    case 0:  /* LSB */
    case 3:  /* CWL */
    case 9:  /* DIGL */
    case 12: /* AM_LSB */
        return SUNSDR_MODE_LSB;

    case 1:  /* USB */
    case 2:  /* DSB */
    case 4:  /* CWU */
    case 5:  /* FM */
    case 6:  /* AM */
    case 7:  /* DIGU */
    case 10: /* SAM */
    case 11: /* DRM */
    case 13: /* AM_USB */
    default:
        return SUNSDR_MODE_USB;
    }
}

/* Forward declarations for helpers used before their definitions. */
static void sunsdr_send_u32_cmd(int opcode, unsigned int value);

static void sunsdr_reassert_tx_state(void)
{
    if (!sdr.powered)
        return;

    if (sdr.currentTxFreqHz > 0) {
        sdr_logf("Reassert TX freq: %d Hz\n", sdr.currentTxFreqHz);
        sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_PRIMARY, 0, sdr.currentTxFreqHz);
    }

    if (sdr.currentMode > 0) {
        sdr_logf("Reassert TX mode: 0x%02X\n", sdr.currentMode);
        sunsdr_send_u32_cmd(SUNSDR_OP_MODE, (unsigned int)sdr.currentMode);
    }

    if (sdr.currentTxAntenna == 1 || sdr.currentTxAntenna == 2) {
        int selector = sunsdr_map_tx_ant_selector(sdr.currentTxAntenna);
        sdr_logf("Reassert TX antenna: %d selector=0x%02X\n", sdr.currentTxAntenna, selector);
        sunsdr_send_u32_cmd(SUNSDR_OP_ANT_PREAMBLE, 0);
        sunsdr_send_u32_cmd(SUNSDR_OP_RX_ANT, (unsigned int)selector);
        sunsdr_send_u32_cmd(SUNSDR_OP_KEEPALIVE, 0);
    }
}

/* Send a simple len=4 command with a u32 payload */
static void sunsdr_send_u32_cmd(int opcode, unsigned int value)
{
    unsigned char pkt[22];
    unsigned char reply[64];

    sunsdr_build_header(pkt, opcode, 0, 0x04);
    pkt[18] = (unsigned char)(value & 0xFF);
    pkt[19] = (unsigned char)((value >> 8) & 0xFF);
    pkt[20] = (unsigned char)((value >> 16) & 0xFF);
    pkt[21] = (unsigned char)((value >> 24) & 0xFF);

    sunsdr_send_ctrl_and_recv(pkt, 22, reply, sizeof(reply));
}

/* Send a zero-payload command (len=0) */
static void sunsdr_send_zero_cmd(int opcode)
{
    unsigned char pkt[18];
    unsigned char reply[64];

    sunsdr_build_header(pkt, opcode, 0, 0x00);
    sunsdr_send_ctrl_and_recv(pkt, 18, reply, sizeof(reply));
}

static const char* SUNSDR_STATE_SYNC_TEMPLATE_HEX =
    "32ff01003200000000000100000000000000320000003200000032000000320000003200000032000000320000003200000000000000010003000300322af87f000028f8";

static const char* SUNSDR_CONFIG_BLOCK_TEMPLATE_HEX =
    "32ff20003400000000000100000000000000010000000100000000000000000000006400000000000000000000001e000000bc02000007000000640000002c01000064000000";

static void sunsdr_send_state_sync_count(int rx_count)
{
    sunsdr_send_hex_pkt_u8(SUNSDR_STATE_SYNC_TEMPLATE_HEX, 0x36, (unsigned char)rx_count);
}

static void sunsdr_send_config_block_state(int rx_state)
{
    sunsdr_send_hex_pkt_u8(SUNSDR_CONFIG_BLOCK_TEMPLATE_HEX, 0x12, (unsigned char)(rx_state ? 1 : 0));
}

static unsigned int sunsdr_current_pa_wire_state(void)
{
    return (unsigned int)((sdr.currentPAEnabled && sdr.currentPTT) ? 1 : 0);
}

static void sunsdr_send_rx_tail(void)
{
    unsigned int ant_selector = (unsigned int)sunsdr_map_ant_selector(sdr.currentRxAntenna > 0 ? sdr.currentRxAntenna : 1);

    sunsdr_send_u32_cmd(0x1E, 0);
    Sleep(1);
    sunsdr_send_u32_cmd(SUNSDR_OP_RX_ANT, ant_selector);
    Sleep(1);
    sunsdr_send_hex_pkt("32ff07001a000000000001000000000000000000000000000000000000000000000000000000000000000000", 44);
    Sleep(1);
    sunsdr_send_u32_cmd(SUNSDR_OP_PA_ENABLE, sunsdr_current_pa_wire_state());
    Sleep(1);
    sunsdr_send_hex_pkt(SUNSDR_CONFIG_BLOCK_TEMPLATE_HEX, 70);
    Sleep(1);
    sunsdr_send_u32_cmd(SUNSDR_OP_KEEPALIVE, 0);
    Sleep(1);
    sunsdr_send_u32_cmd(0x26, 0);
    Sleep(1);
    sunsdr_send_hex_pkt("32ff27001000000000000100000000000000dc460300b6d20000dc460300b6d20000", 34);
    Sleep(1);
    sunsdr_send_hex_pkt("32ff22000c00000000000100000000000000000000000084d71700000000", 30);
}

static void sunsdr_reconfigure_rx_paths(int rx2_enabled)
{
    int rx_count = rx2_enabled ? 2 : 1;
    int rx1_freq = sdr.currentRx1FreqHz > 0 ? sdr.currentRx1FreqHz : 7210500;
    int rx2_freq = sdr.currentRx2FreqHz > 0 ? sdr.currentRx2FreqHz : rx1_freq;

    sunsdr_send_u32_cmd(SUNSDR_OP_POWER_OFF, 0);
    Sleep(1);
    sunsdr_send_u32_cmd(SUNSDR_OP_KEEPALIVE, 0);
    Sleep(1);
    sunsdr_send_hex_pkt("32ff5f000600000000000100000000000000000000000000", 24);
    Sleep(10);
    sunsdr_send_hex_pkt("32ff5f000600000000000100000000000000000000000000", 24);
    Sleep(10);
    sunsdr_send_hex_pkt("32ff5f000600000000000100000000000000000000000000", 24);
    Sleep(10);
    sunsdr_send_u32_cmd(0x1D, 0);
    Sleep(1);
    sunsdr_send_u32_cmd(SUNSDR_OP_RX2_ENABLE, (unsigned int)rx2_enabled);
    Sleep(1);
    sunsdr_send_u32_cmd(SUNSDR_OP_START_IQ, 0x83);
    Sleep(1);
    sunsdr_send_u32_cmd(SUNSDR_OP_KEEPALIVE, 0);
    Sleep(1);
    sunsdr_send_u32_cmd(0x19, 0xFF);
    Sleep(1);
    sunsdr_send_u32_cmd(0x21, 1);
    Sleep(1);
    sunsdr_send_zero_cmd(SUNSDR_OP_STATE_REPEAT);
    Sleep(10);
    sunsdr_send_zero_cmd(SUNSDR_OP_STATE_REPEAT);
    Sleep(10);
    sunsdr_send_zero_cmd(SUNSDR_OP_STATE_REPEAT);
    Sleep(10);
    sunsdr_send_zero_cmd(SUNSDR_OP_STATE_REPEAT);
    Sleep(10);
    sunsdr_send_zero_cmd(SUNSDR_OP_STATE_REPEAT);
    Sleep(10);
    sunsdr_send_state_sync_count(rx_count);
    Sleep(1);
    sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_PRIMARY, 0, rx1_freq - SUNSDR_DDC0_OFFSET_HZ);
    Sleep(1);
    sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_COMP, 0, rx1_freq);
    Sleep(1);
    sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_COMP, 1, rx2_freq);
    Sleep(100);
    sunsdr_send_rx_tail();
}

/* ========== Bootstrap + Power-On Macro ========== */

/* control_ready_v2 macro - validated 2026-04-07 */
static const struct {
    const char* hex;
    int len;
    int delay_us; /* microseconds */
} power_on_macro[] = {
    /* Bootstrap: primer_minimal */
    {"32ff5a000000000000000100000000000000", 18, 50000},
    {"32ff1800040000000000010000000000000000000000", 22, 50000},
    {"32ff0e000000000000000100000000000000", 18, 50000},
    /* control_ready_v2 macro (29 steps) */
    {"32ff1800040000000000010000000000000000000000", 22, 0},
    {"32ff5f000600000000000100000000000000000000000000", 24, 9000},
    {"32ff5f000600000000000100000000000000000000000000", 24, 10000},
    {"32ff5f000600000000000100000000000000000000000000", 24, 10000},
    {"32ff1d00040000000000010000000000000000000000", 22, 50000},
    {"32ff1b00040000000000010000000000000000000000", 22, 300},
    {"32ff0500040000000000010000000000000083000000", 22, 100},
    {"32ff1800040000000000010000000000000000000000", 22, 100},
    {"32ff19000400000000000100000000000000ff000000", 22, 200},
    {"32ff2100040000000000010000000000000001000000", 22, 100},
    {"32ff5a000000000000000100000000000000", 18, 9000},
    {"32ff5a000000000000000100000000000000", 18, 10000},
    {"32ff5a000000000000000100000000000000", 18, 10000},
    {"32ff5a000000000000000100000000000000", 18, 10000},
    {"32ff5a000000000000000100000000000000", 18, 10000},
    {"32ff010032000000000001000000000000003200000032000000320000003200000032000000320000003200000032000000000000000100030003008700f87f00002879", 68, 1000},
    {"32ff0900080000000000010000000000000060fd4d0400000000", 26, 600},
    {"32ff0800080000000000010000000000000098f35b0400000000", 26, 600},
    {"32ff08000800010000000100000000000000c058510400000000", 26, 104000},
    {"32ff17000400000000000100000000000000f5000000", 22, 700},
    {"32ff1e00040000000000010000000000000000000000", 22, 500},
    {"32ff1500040000000000010000000000000001000000", 22, 200},
    {"32ff07001a000000000001000000000000000000000000000000000000000000000000000000000000000000", 44, 200},
    {"32ff2400040000000000010000000000000000000000", 22, 100},
    {"32ff20003400000000000100000000000000010000000100000000000000000000006400000000000000000000001e000000bc02000007000000640000002c01000064000000", 70, 200},
    {"32ff1800040000000000010000000000000000000000", 22, 0},
    {"32ff2600040000000000010000000000000000000000", 22, 200},
    {"32ff27001000000000000100000000000000dc460300b6d20000dc460300b6d20000", 34, 200},
    {"32ff22000c00000000000100000000000000000000000084d71700000000", 30, 0},
    {NULL, 0, 0} /* sentinel */
};

static int sunsdr_run_macro(void)
{
    unsigned char pkt[128];
    unsigned char reply[128];
    int i;

    for (i = 0; power_on_macro[i].hex != NULL; i++) {
        int len = power_on_macro[i].len;
        int j;

        /* Parse hex string to bytes */
        for (j = 0; j < len && j < (int)sizeof(pkt); j++) {
            unsigned int byte;
            sscanf(power_on_macro[i].hex + j * 2, "%02x", &byte);
            pkt[j] = (unsigned char)byte;
        }

        sunsdr_send_ctrl_and_recv(pkt, len, reply, sizeof(reply));

        if (power_on_macro[i].delay_us > 0) {
            /* Sleep in microseconds (Windows minimum is ~1ms) */
            int ms = power_on_macro[i].delay_us / 1000;
            if (ms < 1) ms = 1;
            Sleep(ms);
        }
    }

    return 0;
}

/* ========== Keepalive thread ========== */

static DWORD WINAPI SunSDRKeepaliveThread(LPVOID param)
{
    int count = 0;
    int ctrl_msg_count = 0;
    unsigned char rxbuf[512];
    (void)param;
    sdr_logf("Keepalive thread started\n");

    /* Set control socket to non-blocking recv with short timeout */
    int timeout_ms = 100;
    setsockopt(sdr.ctrlSock, SOL_SOCKET, SO_RCVTIMEO,
        (char*)&timeout_ms, sizeof(timeout_ms));

    while (sdr.keepRunning) {
        /* Send keepalive bundle — mimic what ExpertSDR3 sends periodically */
        sunsdr_send_zero_cmd(SUNSDR_OP_STATE_REPEAT);   /* 0x5A - "I'm alive" */
        sunsdr_send_u32_cmd(SUNSDR_OP_KEEPALIVE, 0);    /* 0x18 - keepalive */
        count++;
        sdr_logf("Keepalive #%d sent (0x5A + 0x18)\n", count);

        /* Drain and log any incoming control messages for ~2 seconds */
        int polls;
        for (polls = 0; polls < 20 && sdr.keepRunning; polls++) {
            int n = recv(sdr.ctrlSock, (char*)rxbuf, sizeof(rxbuf), 0);
            if (n > 0) {
                ctrl_msg_count++;
                if (ctrl_msg_count <= 20 || ctrl_msg_count % 100 == 0)
                    sdr_logf("Ctrl rx #%d: %d bytes, op=0x%02X sub=0x%02X%02X\n",
                        ctrl_msg_count, n,
                        n >= 3 ? rxbuf[2] : 0,
                        n >= 8 ? rxbuf[7] : 0,
                        n >= 7 ? rxbuf[6] : 0);
            }
            Sleep(100);
        }
    }
    sdr_logf("Keepalive thread exiting (keepRunning=%d)\n", sdr.keepRunning);
    return 0;
}

/* ========== Exported functions ========== */

int SunSDRInit(const char* radioIP, int ctrlPort, int streamPort)
{
    WSADATA wsaData;
    int optval;

    sdr_logf("SunSDRInit(%s, %d, %d)\n", radioIP, ctrlPort, streamPort);
    if (sdr.ctrlSock || sdr.streamSock || sdr.hReadThread || sdr.hKeepaliveThread || sdr.rxBuf) {
        sdr_logf("SunSDRInit() cleaning up previous session state before re-init\n");
        SunSDRDestroy();
    }
    memset(&sdr, 0, sizeof(sdr));
    sdr.ctrlSock = INVALID_SOCKET;
    sdr.streamSock = INVALID_SOCKET;
    sdr.ctrlPort = ctrlPort;
    sdr.streamPort = streamPort;
    sdr.currentRxAntenna = 1;
    sdr.currentTxAntenna = 1;
    strncpy(sdr.radioIP, radioIP, sizeof(sdr.radioIP) - 1);

    /* Ensure WSA is initialized */
    if (!WSAinitialized) {
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        WSAinitialized = 1;
    }

    /* Control socket */
    sdr.ctrlSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sdr.ctrlSock == INVALID_SOCKET) {
        printf("SunSDR: failed to create control socket\n");
        return -1;
    }

    optval = 1;
    setsockopt(sdr.ctrlSock, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval));
    optval = 4 * 1024 * 1024;
    setsockopt(sdr.ctrlSock, SOL_SOCKET, SO_RCVBUF, (char*)&optval, sizeof(optval));
    setsockopt(sdr.ctrlSock, SOL_SOCKET, SO_SNDBUF, (char*)&optval, sizeof(optval));

    struct sockaddr_in localAddr;
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons((u_short)ctrlPort);

    if (bind(sdr.ctrlSock, (struct sockaddr*)&localAddr, sizeof(localAddr)) == SOCKET_ERROR) {
        printf("SunSDR: failed to bind control socket to port %d\n", ctrlPort);
        closesocket(sdr.ctrlSock);
        sdr.ctrlSock = INVALID_SOCKET;
        return -2;
    }

    /* Stream socket */
    sdr.streamSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sdr.streamSock == INVALID_SOCKET) {
        printf("SunSDR: failed to create stream socket\n");
        closesocket(sdr.ctrlSock);
        return -3;
    }

    optval = 1;
    setsockopt(sdr.streamSock, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval));
    optval = 4 * 1024 * 1024;
    setsockopt(sdr.streamSock, SOL_SOCKET, SO_RCVBUF, (char*)&optval, sizeof(optval));

    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons((u_short)streamPort);

    if (bind(sdr.streamSock, (struct sockaddr*)&localAddr, sizeof(localAddr)) == SOCKET_ERROR) {
        printf("SunSDR: failed to bind stream socket to port %d\n", streamPort);
        closesocket(sdr.ctrlSock);
        closesocket(sdr.streamSock);
        sdr.streamSock = INVALID_SOCKET;
        return -4;
    }

    /* Radio destination address */
    memset(&sdr.radioAddr, 0, sizeof(sdr.radioAddr));
    sdr.radioAddr.sin_family = AF_INET;
    sdr.radioAddr.sin_port = htons((u_short)ctrlPort);
    inet_pton(AF_INET, radioIP, &sdr.radioAddr.sin_addr);

    /* Allocate IQ buffer (double pairs: I0, Q0, I1, Q1, ...) */
    sdr.rxBufSize = SUNSDR_IQ_COMPLEX_PER_PKT;
    sdr.rxBuf = (double*)calloc(2 * sdr.rxBufSize, sizeof(double));
    InitializeCriticalSectionAndSpinCount(&sdr.txLock, 2500);
    sdr.txLockInitialized = 1;
    sdr.txSeq = 0;
    sdr.txAudioPackets = 0;
    sdr.txPhase = 0.0;
    sdr.txPrevI = 0.0;
    sdr.txPrevQ = 0.0;
    sdr.txAccumCount = 0;
    SendpOutboundTx(sunsdr_tx_outbound);

    printf("SunSDR: initialized, radio=%s ctrl=%d stream=%d\n",
        radioIP, ctrlPort, streamPort);

    return 0;
}

void SunSDRDestroy(void)
{
    sdr_logf("SunSDRDestroy() called!\n");
    sdr.keepRunning = 0;

    if (sdr.hKeepaliveThread) {
        WaitForSingleObject(sdr.hKeepaliveThread, 2000);
        CloseHandle(sdr.hKeepaliveThread);
        sdr.hKeepaliveThread = NULL;
    }
    if (sdr.hReadThread) {
        WaitForSingleObject(sdr.hReadThread, 2000);
        CloseHandle(sdr.hReadThread);
        sdr.hReadThread = NULL;
    }

    if (sdr.ctrlSock != INVALID_SOCKET && sdr.ctrlSock != 0) {
        closesocket(sdr.ctrlSock);
        sdr.ctrlSock = INVALID_SOCKET;
    }
    if (sdr.streamSock != INVALID_SOCKET && sdr.streamSock != 0) {
        closesocket(sdr.streamSock);
        sdr.streamSock = INVALID_SOCKET;
    }

    if (sdr.rxBuf) {
        free(sdr.rxBuf);
        sdr.rxBuf = NULL;
    }

    if (sdr.txLockInitialized) {
        DeleteCriticalSection(&sdr.txLock);
        sdr.txLockInitialized = 0;
    }

    SendpOutboundTx(OutBound);

    printf("SunSDR: destroyed\n");
}

/* ========== Audio Diagnostics ========== */

static void sunsdr_dump_audio_state(const char* label)
{
    sdr_logf("=== AUDIO STATE [%s] ===\n", label);

    /* Global mixer routing */
    sdr_logf("  audioCodecId = %d (0=HERMES, 1=ASIO, 2=WASAPI)\n", pcm->audioCodecId);
    sdr_logf("  OutboundRx callback = %p\n", (void*)pcm->OutboundRx);

    /* DSP rates and sizes */
    sdr_logf("  xcm_inrate[0] = %d Hz, xcm_insize[0] = %d samples\n",
        pcm->xcm_inrate[0], pcm->xcm_insize[0]);
    sdr_logf("  audio_outrate = %d Hz, audio_outsize = %d samples\n",
        pcm->audio_outrate, pcm->audio_outsize);
    sdr_logf("  rcvr[0].ch_outrate = %d Hz, ch_outsize = %d samples\n",
        pcm->rcvr[0].ch_outrate, pcm->rcvr[0].ch_outsize);

    /* ASIO state */
    sdr_logf("  ASIO: pcma->run = %d, pcma->blocksize = %d\n",
        pcma->run, pcma->blocksize);
    sdr_logf("  ASIO: underFlowsOut = %ld, overFlowsOut = %ld\n",
        pcma->underFlowsOut, pcma->overFlowsOut);

    /* VAC state (check VAC 0 and 1) */
    {
        int v;
        for (v = 0; v < 2; v++) {
            IVAC a = pvac[v];
            if (a) {
                sdr_logf("  VAC[%d]: run=%d, iq_type=%d, audio_rate=%d, vac_rate=%d, audio_size=%d\n",
                    v, a->run, a->iq_type, a->audio_rate, a->vac_rate, a->audio_size);
            } else {
                sdr_logf("  VAC[%d]: NULL (not created)\n", v);
            }
        }
    }

    /* Ring buffer accept flags */
    if (pcm->pebuff[0]) {
        sdr_logf("  Inbound ring[0]: accept=%d, run=%d\n",
            (int)_InterlockedAnd(&pcm->pebuff[0]->accept, 0xffffffff),
            (int)_InterlockedAnd(&pcm->pdbuff[0]->run, 0xffffffff));
    }

    sdr_logf("=== END AUDIO STATE ===\n");
}

int SunSDRPowerOn(void)
{
    int desired_rx2 = sdr.currentRX2Enabled;
    int desired_rx_ant = sdr.currentRxAntenna > 0 ? sdr.currentRxAntenna : 1;
    int desired_tx_ant = sdr.currentTxAntenna > 0 ? sdr.currentTxAntenna : 1;

    sdr_logf("SunSDRPowerOn() called\n");
    printf("SunSDR: running power-on macro...\n");

    int result = sunsdr_run_macro();
    if (result != 0) {
        printf("SunSDR: power-on macro failed (%d)\n", result);
        return result;
    }

    sdr.powered = 1;
    sdr.keepRunning = 1;
    sdr.currentPTT = 0;
    sdr.currentRx1FreqHz = 0;
    sdr.currentRx2FreqHz = 0;
    sdr.currentRX2Enabled = 0;
    sdr.currentTune = 0;
    sdr.currentDriveRaw = -1;
    sdr.lastTxWasTune = 0;
    sdr.pendingTuneReleaseConfig = 0;
    sdr.txSeq = 0;
    sdr.txAudioPackets = 0;
    sdr.txPhase = 0.0;
    sdr.txPrevI = 0.0;
    sdr.txPrevQ = 0.0;
    sdr.txAccumCount = 0;

    /* Reset resampler state */
    memset(resampler, 0, sizeof(resampler));

    /* Dump audio state BEFORE any fixes */
    sunsdr_dump_audio_state("before-fix");

    /*
     * FIX: SunSDR has no hardware audio codec (HERMES). If audioCodecId is
     * still HERMES (default), the global mixer routes audio to OutBound()
     * which sends it to the network — where nothing is listening.
     *
     * If ASIO loaded successfully, audioCodecId is already ASIO (cmasio.c:77).
     * If not, we force it to ASIO anyway and re-register the mixer callback
     * so audio goes to asioOUT() → local audio device.
     *
     * The VAC path (xvacOUT) is independent and handled separately.
     */
    /*
     * FIX: SunSDR has no hardware audio codec (HERMES). We need local audio.
     *
     * ASIO path: Only usable if create_cmasio() succeeded (ASIO driver loaded).
     * Check pcma->rmatchOUT != NULL as proof that ASIO is fully initialized.
     * If ASIO is available, set audioCodecId=ASIO, register asioOUT as mixer
     * callback, and call cm_asioStart() to begin the ASIO hardware.
     *
     * If no ASIO driver: leave audioCodecId alone (HERMES). The main mixer
     * audio is lost, but the VAC path (xvacOUT) works independently if
     * VAC is enabled in Thetis settings.
     */
    if (pcma->rmatchOUT != NULL) {
        /* ASIO driver was loaded and initialized */
        if (pcm->audioCodecId != ASIO) {
            sdr_logf("AUDIO FIX: ASIO initialized but audioCodecId=%d — setting to ASIO\n", pcm->audioCodecId);
            pcm->audioCodecId = ASIO;
            SendpOutboundRx(asioOUT);
        }
        if (!pcma->run) {
            sdr_logf("AUDIO FIX: starting ASIO (was not running)\n");
            long asio_result = cm_asioStart(1);  /* protocol=1: ASIO only, no network send */
            sdr_logf("AUDIO FIX: cm_asioStart returned %ld, pcma->run=%d\n", asio_result, pcma->run);
        }
    } else {
        sdr_logf("AUDIO WARNING: No ASIO driver loaded (rmatchOUT=NULL). Main mixer audio unavailable.\n");
        sdr_logf("  Audio output requires VAC to be enabled in Thetis settings.\n");
    }

    /*
     * NOTE: VAC mixer has active=3 (waits for BOTH RX audio + TX monitor).
     * That deadlock is solved by the TX pipeline keepalive in the IQ read
     * thread, which feeds silence into the TX Inbound and keeps xvacOUT()
     * supplied when VAC is actually enabled. Do not force-enable VAC here:
     * VAC run state must remain owned by the managed EnableVAC1/2() path so
     * disabled VACs stay fully inactive.
     */

    /* Dump audio state AFTER fix */
    sunsdr_dump_audio_state("after-fix");

    /* Keepalive disabled — capture shows ExpertSDR3 sends nothing for 20+ seconds */
    /* sdr.hKeepaliveThread = CreateThread(NULL, 0, SunSDRKeepaliveThread, NULL, 0, NULL); */
    sdr_logf("Keepalive thread DISABLED for testing\n");

    /* Start IQ read thread */
    sdr.hReadThread = CreateThread(NULL, 0, SunSDRReadThread, NULL, 0, NULL);

    /* Tell Thetis we have sync so it doesn't kill the connection */
    HaveSync = 1;
    sdr_logf("HaveSync set to 1\n");

    if (desired_rx2)
        SunSDRSetRX2(desired_rx2);

    SunSDRSetAntenna(desired_rx_ant);
    SunSDRSetTxAntenna(desired_tx_ant);
    SunSDRSetPA(sdr.currentPAEnabled);

    printf("SunSDR: powered on, IQ stream active\n");
    return 0;
}

void SunSDRPowerOff(void)
{
    sdr_logf("SunSDRPowerOff() called\n");
    printf("SunSDR: powering off...\n");

    sdr.keepRunning = 0;
    sdr.powered = 0;
    sdr.currentPTT = 0;
    sdr.currentTune = 0;
    sdr.currentDriveRaw = -1;
    sdr.lastTxWasTune = 0;
    sdr.pendingTuneReleaseConfig = 0;
    HaveSync = 0;

    /* Wait for threads to stop */
    if (sdr.hReadThread) {
        WaitForSingleObject(sdr.hReadThread, 2000);
        CloseHandle(sdr.hReadThread);
        sdr.hReadThread = NULL;
    }
    if (sdr.hKeepaliveThread) {
        WaitForSingleObject(sdr.hKeepaliveThread, 3000);
        CloseHandle(sdr.hKeepaliveThread);
        sdr.hKeepaliveThread = NULL;
    }

    /* Stop ASIO if we started it */
    if (pcm->audioCodecId == ASIO && pcma->run && pcma->rmatchOUT != NULL) {
        sdr_logf("Stopping ASIO\n");
        cm_asioStop();
    }

    /* Send power-off command (opcode 0x02) */
    sunsdr_send_u32_cmd(SUNSDR_OP_POWER_OFF, 0);

    printf("SunSDR: powered off\n");
}

void SunSDRSetFreq(int receiver, int freqHz, int isTx)
{
    if (isTx) {
        /* TX frequency: only VFO primary, no DDC companions */
        sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_PRIMARY, 0, freqHz);
        if (sdr.pendingTuneReleaseConfig) {
            sunsdr_send_config_block_state(1);
            sdr.pendingTuneReleaseConfig = 0;
        }
        sdr.currentTxFreqHz = freqHz;
    } else {
        if (receiver == 0) {
            /*
             * RX1 / VFO A:
             * Primary drives the analog LO and COMP[0] tracks the displayed RX1 center.
             * Do not overwrite COMP[1] here; RX2 owns that tuning context.
             */
            sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_PRIMARY, 0, freqHz - SUNSDR_DDC0_OFFSET_HZ);
            sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_COMP, 0, freqHz);
            sdr.currentRx1FreqHz = freqHz;
        } else if (receiver == 1) {
            /* RX2 / VFO B tuning context observed on 0x08 sub=1. */
            sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_COMP, 1, freqHz);
            sdr.currentRx2FreqHz = freqHz;
        }
    }
}

void SunSDRSetMode(int mode)
{
    int sunsdr_mode = sunsdr_map_mode_code(mode);
    sdr_logf("SunSDRSetMode(thetis=%d -> sunsdr=0x%02X)\n", mode, sunsdr_mode);
    sunsdr_send_u32_cmd(SUNSDR_OP_MODE, (unsigned int)sunsdr_mode);
    sdr.currentMode = sunsdr_mode;
}

void SunSDRSetRX2(int enabled)
{
    int new_enabled = enabled ? 1 : 0;

    if (!sdr.powered)
    {
        sdr.currentRX2Enabled = new_enabled;
        return;
    }

    if (sdr.currentRX2Enabled == new_enabled)
        return;

    sunsdr_reconfigure_rx_paths(new_enabled);

    sdr.currentRX2Enabled = new_enabled;
}

void SunSDRSetTune(int tune)
{
    sdr.currentTune = tune ? 1 : 0;
}

void SunSDRSetDrive(int raw)
{
    if (raw < 0) raw = 0;
    if (raw > 255) raw = 255;
    sdr.currentDriveRaw = raw;
    sdr_logf("SunSDRSetDrive(%d)\n", raw);
}

void SunSDRSetPA(int enabled)
{
    int new_enabled = enabled ? 1 : 0;
    int old_enabled = sdr.currentPAEnabled;
    unsigned int wire_state;

    sdr.currentPAEnabled = new_enabled;

    if (!sdr.powered) {
        sdr_logf("SunSDRSetPA(%d) cached while unpowered\n", new_enabled);
        return;
    }

    wire_state = sunsdr_current_pa_wire_state();
    sdr_logf("SunSDRSetPA(request=%d, old=%d, wire=%u, ptt=%d)\n", new_enabled, old_enabled, wire_state, sdr.currentPTT);
    sunsdr_send_u32_cmd(SUNSDR_OP_PA_ENABLE, wire_state);
}

void SunSDRSetAntenna(int antenna)
{
    int selector = sunsdr_map_ant_selector(antenna);

    if (antenna > 0)
        sdr.currentRxAntenna = antenna;

    if (selector == 0) {
        sdr_logf("SunSDRSetAntenna(%d) ignored: unsupported antenna\n", antenna);
        return;
    }

    if (!sdr.powered) {
        sdr_logf("SunSDRSetAntenna(%d) cached while unpowered\n", antenna);
        return;
    }

    sdr_logf("SunSDRSetAntenna(%d) selector=0x%02X\n", antenna, selector);
    sunsdr_send_u32_cmd(SUNSDR_OP_ANT_PREAMBLE, 0);
    sunsdr_send_u32_cmd(SUNSDR_OP_RX_ANT, (unsigned int)selector);
    sunsdr_send_u32_cmd(SUNSDR_OP_KEEPALIVE, 0);
}

void SunSDRSetTxAntenna(int antenna)
{
    int selector = sunsdr_map_tx_ant_selector(antenna);

    if (antenna > 0)
        sdr.currentTxAntenna = antenna;

    if (selector == 0) {
        sdr_logf("SunSDRSetTxAntenna(%d) ignored: unsupported antenna\n", antenna);
        return;
    }

    if (!sdr.powered) {
        sdr_logf("SunSDRSetTxAntenna(%d) cached while unpowered\n", antenna);
        return;
    }

    sdr_logf("SunSDRSetTxAntenna(%d) selector=0x%02X\n", antenna, selector);
    sunsdr_send_u32_cmd(SUNSDR_OP_ANT_PREAMBLE, 0);
    sunsdr_send_u32_cmd(SUNSDR_OP_RX_ANT, (unsigned int)selector);
    sunsdr_send_u32_cmd(SUNSDR_OP_KEEPALIVE, 0);
}

void SunSDRSetPTT(int ptt)
{
    int new_ptt = ptt ? 1 : 0;

    if (!sdr.powered)
    {
        sdr.currentPTT = new_ptt;
        return;
    }

    if (sdr.currentPTT == new_ptt)
        return;

    if (sdr.txLockInitialized) {
        EnterCriticalSection(&sdr.txLock);
        sdr.txAudioPackets = 0;
        sdr.txPhase = 0.0;
        sdr.txPrevI = 0.0;
        sdr.txPrevQ = 0.0;
        sdr.txAccumCount = 0;
        LeaveCriticalSection(&sdr.txLock);
    }

    sdr_logf("SunSDRSetPTT(%d) currentTune=%d\n", new_ptt, sdr.currentTune);

    if (new_ptt) {
        sunsdr_reassert_tx_state();
        sunsdr_send_config_block_state(0);
        sdr.lastTxWasTune = 0;
        sunsdr_send_u32_cmd(SUNSDR_OP_MOX_PTT, 1);
        sdr.currentPTT = new_ptt;
        sunsdr_send_u32_cmd(SUNSDR_OP_PA_ENABLE, sunsdr_current_pa_wire_state());
    } else {
        sdr.currentPTT = new_ptt;
        sunsdr_send_u32_cmd(SUNSDR_OP_PA_ENABLE, sunsdr_current_pa_wire_state());
        sunsdr_send_u32_cmd(SUNSDR_OP_MOX_PTT, 0);
        sunsdr_send_config_block_state(1);
        sdr.lastTxWasTune = 0;
        sdr.pendingTuneReleaseConfig = 0;
    }

    sdr.currentPTT = new_ptt;
}

/* ========== IQ Receive Thread ========== */

/* Build a silent TX IQ packet (1210 bytes, op=0xFE, all-zero IQ payload) */
static void sunsdr_build_tx_silence(unsigned char* buf, unsigned int seq)
{
    sunsdr_build_iq_header(buf, SUNSDR_OP_IQ_STREAM, seq);
    /* Remaining 1200 bytes are zeros (silence). */
}

DWORD WINAPI SunSDRReadThread(LPVOID param)
{
    unsigned char pktbuf[2048];
    unsigned char txbuf[1210];
    int i, k;
    int pkt_count = 0;
    int telemetry_count = 0;
    unsigned int last_tx_audio_packets = 0;
    ULONGLONG last_service_tick;
    double tx_feed_accum = 0.0;
    double tx_keepalive_accum = 0.0;
    (void)param;

    /*
     * TX pipeline keepalive: Feed silence into the TX stream's Inbound ring
     * so cm_main runs xcmaster(tx_stream), which calls xvacIN() to drain
     * the VAC input. Without this, nobody consumes VAC input → overflows.
     *
     * TX stream ID = inid(1, 0) = pcm->cmRCVR (receivers come first).
     * TX buffer size = pcm->xcm_insize[tx_stream_id].
     * Feed rate: xcm_inrate[tx] / xcm_insize[tx] buffers/sec.
     * IQ packet rate: ~1562/sec. Feed ratio = tx_bufs_per_sec / 1562.
     */
    int tx_stream_id = inid(1, 0);
    int tx_buf_size = pcm->xcm_insize[tx_stream_id];
    double* tx_silence_buf = (double*)calloc(2 * tx_buf_size, sizeof(double));
    double tx_feed_rate = (double)pcm->xcm_inrate[tx_stream_id] / (double)tx_buf_size;  /* bufs/sec */
    double tx_keepalive_rate = 1562.5 / 8.0; /* packets/sec */

    /* RX silence padding: when SunSDR reduces its IQ rate during MOX (1562 -> ~195/sec),
     * the WDSP RX channel (opened with bfo=1) blocks waiting for output that never comes,
     * starving the VAC mixer Input 0 and causing massive PortAudio underflows.
     *
     * Solution: feed silence to xrouter at the expected rate (1562 buffers/sec * 246 samples)
     * whenever packets aren't arriving fast enough. This keeps WDSP's input rate constant.
     */
    int rx_stream_id = inid(0, 0);
    int rx_resample_outsize = 246;  /* matches sunsdr_resample output size */
    double* rx_silence_buf = (double*)calloc(2 * rx_resample_outsize, sizeof(double));
    double rx_feed_rate_target = 1562.5; /* expected packets/sec */
    ULONGLONG last_rx_pkt_tick = 0;

    sdr_logf("IQ read thread started\n");
    sdr_logf("TX pipeline keepalive: stream=%d, buf_size=%d, feed_rate=%d/sec, step=%.4f\n",
        tx_stream_id, tx_buf_size, (int)tx_feed_rate, tx_feed_rate / 1000.0);
    printf("SunSDR: IQ read thread started\n");

    /* Set stream socket timeout so we can check keepRunning */
    int timeout_ms = 500;
    setsockopt(sdr.streamSock, SOL_SOCKET, SO_RCVTIMEO,
        (char*)&timeout_ms, sizeof(timeout_ms));

    DWORD last_log_time = GetTickCount();
    last_service_tick = GetTickCount64();
    int timeout_count = 0;

    while (sdr.keepRunning) {
        ULONGLONG now_tick = GetTickCount64();
        double elapsed_ms = (double)(now_tick - last_service_tick);

        if (elapsed_ms > 0.0) {
                tx_feed_accum += elapsed_ms * tx_feed_rate / 1000.0;
                while (tx_feed_accum >= 1.0) {
                    Inbound(tx_stream_id, tx_buf_size, tx_silence_buf);
                    tx_feed_accum -= 1.0;
                }

            tx_keepalive_accum += elapsed_ms * tx_keepalive_rate / 1000.0;
            last_service_tick = now_tick;
        }

        /* RX silence padding: if no real RX packet has arrived for >2ms (would
         * normally arrive every ~640us at 1562/sec), inject silence to keep WDSP
         * RX input rate at expected 384k samples/sec. This prevents fexchange0
         * from blocking on Sem_OutReady when SunSDR throttles the IQ stream
         * during TX (radio drops from 1562/sec to ~195/sec during MOX/TUNE). */
        if (sdr.currentPTT && last_rx_pkt_tick > 0) {
            ULONGLONG since_last_rx = now_tick - last_rx_pkt_tick;
            if (since_last_rx >= 2) {
                /* Inject one buffer of silence per 640us gap */
                int gaps_to_fill = (int)(since_last_rx * 1562 / 1000);
                if (gaps_to_fill > 32) gaps_to_fill = 32;  /* cap to prevent runaway */
                for (int g = 0; g < gaps_to_fill; g++) {
                    __try {
                        xrouter(NULL, 0, 0, rx_resample_outsize, rx_silence_buf);
                    } __except(EXCEPTION_EXECUTE_HANDLER) {
                        break;
                    }
                }
                last_rx_pkt_tick = now_tick;
            }
        }

        while (tx_keepalive_accum >= 1.0) {
            int send_silence = !sdr.currentPTT;

            if (sdr.currentPTT) {
                if (sdr.txAudioPackets == last_tx_audio_packets)
                    send_silence = 1;
                last_tx_audio_packets = sdr.txAudioPackets;
            }

            if (send_silence) {
                struct sockaddr_in streamDest;
                memcpy(&streamDest, &sdr.radioAddr, sizeof(streamDest));
                streamDest.sin_port = htons((u_short)sdr.streamPort);
                sunsdr_build_tx_silence(txbuf, sdr.txSeq++);
                sendto(sdr.streamSock, (const char*)txbuf, SUNSDR_IQ_PKT_SIZE, 0,
                    (struct sockaddr*)&streamDest, sizeof(streamDest));
            }

            tx_keepalive_accum -= 1.0;
        }

        int n = recv(sdr.streamSock, (char*)pktbuf, sizeof(pktbuf), 0);

        /* Log every second regardless */
        DWORD now = GetTickCount();
        if (now - last_log_time >= 1000) {
            sdr_logf("IQ status: pkts=%d, timeouts=%d, keepRunning=%d, HaveSync=%d\n",
                pkt_count, timeout_count, sdr.keepRunning, HaveSync);
            last_log_time = now;
            timeout_count = 0;
        }

        if (n <= 0) {
            timeout_count++;
            continue;
        }

        /* Verify magic */
        if (n < 4 || pktbuf[0] != SUNSDR_MAGIC_0 || pktbuf[1] != SUNSDR_MAGIC_1) {
            timeout_count++;
            continue;
        }

        /* --- 0x1F telemetry family (34 or 42 bytes, byte[3]==0x1F) --- */
        if (pktbuf[3] == 0x1F && (n == 34 || n == 42)) {
            unsigned char subtype = pktbuf[2];
            telemetry_count++;

            if (subtype == 0x00) {
                /* Recurring power/SWR telemetry — setPowerSwr path
                 * DLL handler reads packet offsets +0x12, +0x16, +0x1a
                 * as float and u16 conversions.
                 */
                /* Parse every byte pair as u16 LE for fine-grained analysis */
                unsigned short u16[17] = {0};
                for (int bi = 0; bi < 17 && (bi*2+1) < n; bi++)
                    u16[bi] = pktbuf[bi*2] | (pktbuf[bi*2+1] << 8);

                /* Also try float32 at various offsets */
                float f_08 = 0, f_0c = 0, f_10 = 0, f_14 = 0, f_18 = 0, f_1c = 0;
                if (n >= 12) memcpy(&f_08, &pktbuf[8], 4);
                if (n >= 16) memcpy(&f_0c, &pktbuf[12], 4);
                if (n >= 20) memcpy(&f_10, &pktbuf[16], 4);
                if (n >= 24) memcpy(&f_14, &pktbuf[20], 4);
                if (n >= 28) memcpy(&f_18, &pktbuf[24], 4);
                if (n >= 32) memcpy(&f_1c, &pktbuf[28], 4);

                /* Log first 20, then every 50th, plus always during TX */
                if (telemetry_count <= 20 || telemetry_count % 50 == 0 || sdr.currentPTT) {
                    /* u16 view: every 2-byte pair from the packet */
                    sdr_logf("TELEM 0x1F/00 #%d len=%d PTT=%d u16: "
                        "%u %u | %u %u | %u %u | %u %u | %u %u | %u %u | %u %u | %u %u | %u\n",
                        telemetry_count, n, sdr.currentPTT,
                        u16[0], u16[1],   /* bytes 0-3: header */
                        u16[2], u16[3],   /* bytes 4-7 */
                        u16[4], u16[5],   /* bytes 8-11 */
                        u16[6], u16[7],   /* bytes 12-15 */
                        u16[8], u16[9],   /* bytes 16-19 */
                        u16[10], u16[11], /* bytes 20-23 */
                        u16[12], u16[13], /* bytes 24-27 */
                        u16[14], u16[15], /* bytes 28-31 */
                        u16[16]);         /* bytes 32-33 */

                    /* float view at each 4-byte offset */
                    sdr_logf("TELEM 0x1F/00 #%d floats: "
                        "f08=%.6g f0c=%.6g f10=%.6g f14=%.6g f18=%.6g f1c=%.6g\n",
                        telemetry_count,
                        (double)f_08, (double)f_0c, (double)f_10,
                        (double)f_14, (double)f_18, (double)f_1c);
                }

                /* Hex dump: first 5 and every 200th */
                if (telemetry_count <= 5 || telemetry_count % 200 == 0) {
                    char hexbuf[256];
                    int hpos = 0;
                    for (int b = 0; b < n && hpos < (int)sizeof(hexbuf) - 3; b++)
                        hpos += sprintf(hexbuf + hpos, "%02x", pktbuf[b]);
                    sdr_logf("TELEM 0x1F/00 HEX #%d: %s\n", telemetry_count, hexbuf);
                }

                /* Feed power telemetry to Thetis metering.
                 * Bytes 14-15 (u16 LE) carry forward power ADC value.
                 * Confirmed 2026-04-09: u16=48 → 11.2W, u16=186 → ~96W.
                 * Linear model: watts = max(0, (value - 30) * 0.614)
                 * Conversion to watts happens in C# computeAlexFwdPower().
                 * Bytes 16-17 may carry SWR (134 RX, 125 at 96W TX).
                 */
                {
                    unsigned short fwd_raw = (n >= 16) ? (pktbuf[14] | (pktbuf[15] << 8)) : 0;
                    unsigned short swr_raw = (n >= 18) ? (pktbuf[16] | (pktbuf[17] << 8)) : 0;
                    PeakFwdPower((float)fwd_raw);
                    PeakRevPower((float)swr_raw);
                }
            }
            else if (subtype == 0x01) {
                /* One-shot operational status: PTT/TxPTT/ATU flags */
                unsigned int status_word = 0;
                if (n >= 22)
                    status_word = pktbuf[18] | (pktbuf[19]<<8) | (pktbuf[20]<<16) | (pktbuf[21]<<24);
                int ptt_bit = (status_word >> 7) & 1;
                int tx_ptt_bit = (status_word >> 8) & 1;
                int atu_flag = (status_word >> 11) & 1;
                sdr_logf("TELEM 0x1F/01 status: word=0x%08X ptt=%d tx_ptt=%d atu=%d\n",
                    status_word, ptt_bit, tx_ptt_bit, atu_flag);
            }
            else {
                sdr_logf("TELEM 0x1F/%02X len=%d (unknown subtype)\n", subtype, n);
            }
            continue;
        }

        /* Non-IQ, non-telemetry packets */
        if (n != SUNSDR_IQ_PKT_SIZE) {
            timeout_count++;
            continue;
        }

        /* Verify IQ opcode: 0xFE = RX IQ stream, 0xFD = TX-active stream.
         * Both are 1210-byte 24-bit interleaved IQ packets.
         * During TX, the radio switches to 0xFD. We MUST keep processing them
         * to feed xrouter -> xcmaster -> xvacOUT -> VAC mixer Input 0.
         * Without this, the VAC mixer thread blocks on WaitForMultipleObjects
         * waiting for Input 0, xvac_out never fires, rmatchOUT drains, and
         * the PortAudio output callback gets ~1562 underflows/sec during TX.
         */
        if (pktbuf[2] != SUNSDR_OP_IQ_STREAM && pktbuf[2] != 0xFD)
            continue;

        /* Extract 200 x 24-bit LE I/Q pairs from payload (offset 10) */
        unsigned char* payload = pktbuf + SUNSDR_IQ_HDR_SIZE;

        for (i = 0, k = 0; i < SUNSDR_IQ_COMPLEX_PER_PKT; i++, k += SUNSDR_IQ_BYTES_PER_IQ) {
            /* SunSDR 24-bit LE: byte[0]=LSB, byte[1]=MID, byte[2]=MSB */
            /* Convert to 32-bit signed (MSB-aligned) then normalize to double */
            /* NOTE: SunSDR sends Q first, I second (opposite of HPSDR) */
            sdr.rxBuf[2 * i + 0] = NORM *
                (double)((int)(payload[k + 5] << 24 |
                               payload[k + 4] << 16 |
                               payload[k + 3] << 8));     /* I (from bytes 3-5) */

            sdr.rxBuf[2 * i + 1] = NORM *
                (double)((int)(payload[k + 2] << 24 |
                               payload[k + 1] << 16 |
                               payload[k + 0] << 8));     /* Q (from bytes 0-2) */
        }

        /* Resample 312500 → 384000 Hz, then feed into Thetis DSP pipeline */
        {
            int active_sources = pktbuf[8];
            int source = 0;

            if (active_sources >= 2 && pktbuf[9] < 2)
                source = pktbuf[9];

            int out_n = sunsdr_resample(source, sdr.rxBuf, SUNSDR_IQ_COMPLEX_PER_PKT);
            if (out_n > 0) {
                __try {
                    xrouter(NULL, 0, source, out_n, resampler[source].out);
                    if (source == 0) last_rx_pkt_tick = GetTickCount64();
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    sdr_logf("CRASH in xrouter at pkt %d (source=%d, out_n=%d)! Exception=0x%08X\n",
                        pkt_count, source, out_n, GetExceptionCode());
                    Sleep(1000);
                    continue;
                }
            }
            if (pkt_count == 1 || pkt_count == 100 || pkt_count % 5000 == 0)
                sdr_logf("IQ pkts=%d, source=%d/%d, resample_out=%d, tx_sent=%d (HaveSync=%d)\n",
                    pkt_count, source, active_sources, out_n, sdr.txSeq, HaveSync);
        }
        pkt_count++;

        if (pkt_count == 100)
            sunsdr_dump_audio_state("after-100-pkts");
        if (HaveSync == 0) {
            sdr_logf("WARNING: HaveSync was reset to 0 at pkt %d! Restoring.\n", pkt_count);
            HaveSync = 1;
        }
    }

    sdr_logf("IQ read thread exiting (keepRunning=%d, pkt_count=%d)\n", sdr.keepRunning, pkt_count);
    free(tx_silence_buf);
    free(rx_silence_buf);
    printf("SunSDR: IQ read thread stopped\n");
    return 0;
}

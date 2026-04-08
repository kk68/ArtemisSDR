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
#include <string.h>
#include <ws2tcpip.h>
#include "sunsdr.h"
#include "network.h"
#include "router.h"
#include "cmsetup.h"
#include "cmaster.h"
#include "cmasio.h"
#include "ivac.h"

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

static double resample_phase = 0.0;
static double resample_prev_I = 0.0;  /* last sample from previous packet (for interpolation) */
static double resample_prev_Q = 0.0;
static double resample_out[SUNSDR_RESAMPLE_MAX * 2]; /* output I/Q pairs */

/*
 * Resample nsamples complex pairs from in[] to resample_out[].
 * Returns number of complex output samples produced.
 * Uses linear interpolation with state preserved across calls.
 */
static int sunsdr_resample(const double* in, int nsamples)
{
    int out_count = 0;
    int i;
    double prev_I = resample_prev_I;
    double prev_Q = resample_prev_Q;

    for (i = 0; i < nsamples; i++) {
        double cur_I = in[2 * i + 0];
        double cur_Q = in[2 * i + 1];

        /* Emit output samples while phase < 1.0 (we're within this input interval) */
        while (resample_phase < 1.0 && out_count < SUNSDR_RESAMPLE_MAX) {
            double frac = resample_phase;
            resample_out[2 * out_count + 0] = prev_I + frac * (cur_I - prev_I);
            resample_out[2 * out_count + 1] = prev_Q + frac * (cur_Q - prev_Q);
            out_count++;
            resample_phase += SUNSDR_RESAMPLE_STEP;
        }
        resample_phase -= 1.0;

        prev_I = cur_I;
        prev_Q = cur_Q;
    }

    resample_prev_I = prev_I;
    resample_prev_Q = prev_Q;

    return out_count;
}

/* ========== Helpers ========== */

static const double NORM = 1.0 / 2147483648.0;

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
    memset(&sdr, 0, sizeof(sdr));
    sdr.ctrlSock = INVALID_SOCKET;
    sdr.streamSock = INVALID_SOCKET;
    sdr.ctrlPort = ctrlPort;
    sdr.streamPort = streamPort;
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

    if (sdr.ctrlSock != INVALID_SOCKET) {
        closesocket(sdr.ctrlSock);
        sdr.ctrlSock = INVALID_SOCKET;
    }
    if (sdr.streamSock != INVALID_SOCKET) {
        closesocket(sdr.streamSock);
        sdr.streamSock = INVALID_SOCKET;
    }

    if (sdr.rxBuf) {
        free(sdr.rxBuf);
        sdr.rxBuf = NULL;
    }

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
    sdr_logf("SunSDRPowerOn() called\n");
    printf("SunSDR: running power-on macro...\n");

    int result = sunsdr_run_macro();
    if (result != 0) {
        printf("SunSDR: power-on macro failed (%d)\n", result);
        return result;
    }

    sdr.powered = 1;
    sdr.keepRunning = 1;

    /* Reset resampler state */
    resample_phase = 0.0;
    resample_prev_I = 0.0;
    resample_prev_Q = 0.0;

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
     * FIX: VAC timing issue. The C# EnableVAC1() calls SetIVACrun(0, 1)
     * only when console.PowerOn is true. For SunSDR, PowerOn may not be set
     * when VAC is initialized, so pvac[0]->run stays 0. The PortAudio
     * stream IS open (underflows count), resampler and mixer are created,
     * but xvacOUT() drops all data because run==0.
     *
     * Fix: if VAC has valid rates (was configured) but run==0, enable it.
     */
    {
        int v;
        for (v = 0; v < 2; v++) {
            IVAC a = pvac[v];
            if (a && !a->run && a->audio_rate > 0 && a->vac_rate > 0) {
                sdr_logf("AUDIO FIX: VAC[%d] configured (audio_rate=%d) but run=0 — enabling\n",
                    v, a->audio_rate);
                a->run = 1;
            }
            /*
             * NOTE: VAC mixer has active=3 (waits for BOTH RX audio + TX monitor).
             * Previously this caused a deadlock because TX monitor never got data.
             * Now solved by the TX pipeline keepalive in the IQ read thread —
             * it feeds silence into the TX Inbound, which runs xcmaster(tx_stream),
             * which calls xvacOUT(0, 2, ...) to feed TX monitor to the mixer.
             * Both inputs get data, so no deadlock.
             */
        }
    }

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

    printf("SunSDR: powered on, IQ stream active\n");
    return 0;
}

void SunSDRPowerOff(void)
{
    sdr_logf("SunSDRPowerOff() called\n");
    printf("SunSDR: powering off...\n");

    sdr.keepRunning = 0;
    sdr.powered = 0;
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

void SunSDRSetFreq(int freqHz, int isTx)
{
    if (isTx) {
        /* TX frequency: only VFO primary, no DDC companions */
        sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_PRIMARY, 0, freqHz);
        sdr.currentTxFreqHz = freqHz;
    } else {
        /*
         * RX frequency: DDC0 is the IQ center, Primary is the analog LO.
         * Set DDC0 = display frequency so IQ stream matches the panadapter.
         * Primary = DDC0 - offset (drives the analog frontend).
         */
        sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_PRIMARY, 0, freqHz - SUNSDR_DDC0_OFFSET_HZ);
        sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_COMP, 0, freqHz);  /* DDC0 = IQ center = display freq */
        sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_COMP, 1, freqHz - SUNSDR_DDC0_OFFSET_HZ + SUNSDR_DDC1_OFFSET_HZ);
        sdr.currentFreqHz = freqHz;
    }
}

void SunSDRSetMode(int mode)
{
    sunsdr_send_u32_cmd(SUNSDR_OP_MODE, (unsigned int)mode);
    sdr.currentMode = mode;
}

void SunSDRSetPTT(int ptt)
{
    sunsdr_send_u32_cmd(SUNSDR_OP_MOX_PTT, ptt ? 1 : 0);
    sdr.currentPTT = ptt;
}

/* ========== IQ Receive Thread ========== */

/* Build a silent TX IQ packet (1210 bytes, op=0xFE, all-zero IQ payload) */
static void sunsdr_build_tx_silence(unsigned char* buf, unsigned int seq)
{
    memset(buf, 0, SUNSDR_IQ_PKT_SIZE);
    buf[0] = SUNSDR_MAGIC_0;
    buf[1] = SUNSDR_MAGIC_1;
    buf[2] = SUNSDR_OP_IQ_STREAM;  /* 0xFE */
    buf[3] = 0xFF;
    /* Sequence counter at bytes 4-7 (LE), mimicking ExpertSDR3 pattern */
    buf[4] = (unsigned char)(seq & 0xFF);
    buf[5] = (unsigned char)((seq >> 8) & 0xFF);
    buf[6] = (unsigned char)((seq >> 16) & 0xFF);
    buf[7] = (unsigned char)((seq >> 24) & 0xFF);
    /* Remaining 1200 bytes are zeros (silence) */
}

DWORD WINAPI SunSDRReadThread(LPVOID param)
{
    unsigned char pktbuf[2048];
    unsigned char txbuf[1210];
    int i, k;
    int pkt_count = 0;
    unsigned int tx_seq = 0;
    int tx_interval = 8; /* send 1 TX packet every N RX packets */
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
    int tx_feed_rate = pcm->xcm_inrate[tx_stream_id] / tx_buf_size;  /* bufs/sec */
    /* Accumulator: feed one TX silence buf for every (1562/tx_feed_rate) IQ pkts */
    double tx_feed_accum = 0.0;
    double tx_feed_step = (double)tx_feed_rate / 1562.5;  /* ~0.48 for 48k/64 */

    sdr_logf("IQ read thread started (TX silence every %d pkts)\n", tx_interval);
    sdr_logf("TX pipeline keepalive: stream=%d, buf_size=%d, feed_rate=%d/sec, step=%.4f\n",
        tx_stream_id, tx_buf_size, tx_feed_rate, tx_feed_step);
    printf("SunSDR: IQ read thread started\n");

    /* Set stream socket timeout so we can check keepRunning */
    int timeout_ms = 500;
    setsockopt(sdr.streamSock, SOL_SOCKET, SO_RCVTIMEO,
        (char*)&timeout_ms, sizeof(timeout_ms));

    DWORD last_log_time = GetTickCount();
    int timeout_count = 0;

    while (sdr.keepRunning) {
        int n = recv(sdr.streamSock, (char*)pktbuf, sizeof(pktbuf), 0);

        /* Log every second regardless */
        DWORD now = GetTickCount();
        if (now - last_log_time >= 1000) {
            sdr_logf("IQ status: pkts=%d, timeouts=%d, keepRunning=%d, HaveSync=%d\n",
                pkt_count, timeout_count, sdr.keepRunning, HaveSync);
            last_log_time = now;
            timeout_count = 0;
        }

        if (n != SUNSDR_IQ_PKT_SIZE) {
            timeout_count++;
            continue;
        }

        /* Verify magic and opcode */
        if (pktbuf[0] != SUNSDR_MAGIC_0 || pktbuf[1] != SUNSDR_MAGIC_1)
            continue;
        if (pktbuf[2] != SUNSDR_OP_IQ_STREAM)
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
        int out_n = sunsdr_resample(sdr.rxBuf, SUNSDR_IQ_COMPLEX_PER_PKT);
        if (out_n > 0) {
            __try {
                xrouter(NULL, 0, 0, out_n, resample_out);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                sdr_logf("CRASH in xrouter at pkt %d (out_n=%d)! Exception=0x%08X\n",
                    pkt_count, out_n, GetExceptionCode());
                Sleep(1000);
                continue;
            }
        }
        pkt_count++;

        /* Feed silence into TX pipeline to keep it alive (drains VAC input) */
        tx_feed_accum += tx_feed_step;
        while (tx_feed_accum >= 1.0) {
            Inbound(tx_stream_id, tx_buf_size, tx_silence_buf);
            tx_feed_accum -= 1.0;
        }

        /* Send TX silence packet every N RX packets to keep radio streaming */
        if (pkt_count % tx_interval == 0) {
            struct sockaddr_in streamDest;
            memcpy(&streamDest, &sdr.radioAddr, sizeof(streamDest));
            streamDest.sin_port = htons((u_short)sdr.streamPort); /* port 50002 */
            sunsdr_build_tx_silence(txbuf, tx_seq++);
            sendto(sdr.streamSock, (const char*)txbuf, SUNSDR_IQ_PKT_SIZE, 0,
                (struct sockaddr*)&streamDest, sizeof(streamDest));
        }

        if (pkt_count == 1 || pkt_count == 100 || pkt_count % 5000 == 0)
            sdr_logf("IQ pkts=%d, resample_out=%d, tx_sent=%d (HaveSync=%d)\n", pkt_count, out_n, tx_seq, HaveSync);
        if (pkt_count == 100)
            sunsdr_dump_audio_state("after-100-pkts");
        if (HaveSync == 0) {
            sdr_logf("WARNING: HaveSync was reset to 0 at pkt %d! Restoring.\n", pkt_count);
            HaveSync = 1;
        }
    }

    sdr_logf("IQ read thread exiting (keepRunning=%d, pkt_count=%d)\n", sdr.keepRunning, pkt_count);
    free(tx_silence_buf);
    printf("SunSDR: IQ read thread stopped\n");
    return 0;
}

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
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
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
extern void getIVACdiags(int id, int type, int* underflows, int* overflows, double* var, int* ringsize, int* nring);
extern void getCMAevents(long* overFlowsIn, long* overFlowsOut, long* underFlowsIn, long* underFlowsOut);

/* WDSP synchronous flush entry point (added in channel.c for SUNSDR Phase C).
 * Flushes iobuffs + the RXA/TXA DSP chain for the given channel under its
 * own locks. Called on PTT-on to guarantee a clean TX DSP start regardless
 * of what state the previous MOX/TUNE session left behind. */
extern __declspec(dllimport) void FlushChannelNow (int channel);

/* Forward declarations — the iq_dump and tx_pace helpers defined
 * immediately below need these, but they're defined much later in
 * this file. */
static void sdr_logf(const char* fmt, ...);
static void sunsdr_build_tx_packet(unsigned char* buf, unsigned int seq, const double* iq);
static void sunsdr_send_tx_packet(const double* iq);
static struct sockaddr_in sunsdr_stream_dest(void);
static void sunsdr_dbg_note_tx_packet(unsigned int seq);

/* sunsdr_debug.log writer (sdr_logf path) and per-attempt IQ dumps.
 * Log temporarily re-enabled for TUNE-at-dial diagnosis. Revert to 0
 * after verifying the SUNSDR_TUNE_FREQ trace shows the expected
 * shifted TX frequency. */
#define SUNSDR_DEBUG_LOG_ENABLED 1
#define SUNSDR_IQ_DUMP_ENABLED 0

/* Global SunSDR session state — moved above the iq_dump / tx_pace
 * helpers which need sdr.currentPTT, sdr.streamSock, sdr.txSeq, etc. */
static sunsdr_state_t sdr;

/* TX IQ ground-truth recorder.
 *
 * Counters on MOX #5 (no audio on-air) and TUNE #8 (raspy, Run post-Phase C)
 * showed NO discriminator between clean and failing attempts. Run 11's
 * TX_CB_GAP_HIST proved scheduler preemption is no longer the driver. The
 * Phase C TXA flush did not close the raspy. Hypothesis space for the
 * residual failure is wide and log-inspection is no longer informative —
 * need direct capture of the I/Q content we emit on the wire during the
 * failing attempt, compared against a clean-attempt baseline from the
 * same test run, to see whether the distortion is
 *   (a) nonlinear (harmonics on the fundamental tone),
 *   (b) wideband noise (filter-ring residue),
 *   (c) amplitude modulation (AAMIX / ALC),
 *   (d) phase jitter (resampler / sample timing),
 * or something we have not considered.
 *
 * Buffer: 40000 complex = ~1024 ms at 39062.5 Hz TX rate. Captures the
 * first second of each TX attempt. Writes to sunsdr_tx_iq_<attempt>.raw
 * in the same directory as sunsdr_debug.log when the attempt ends.
 * Format: little-endian IEEE-754 double pairs (I, Q). Exactly the post-
 * scaling, post-downsampling values enqueued into the outgoing UDP
 * payload — the ground truth of what the radio sees.
 *
 * Size: 40000 complex * 16 bytes = 640 KB per attempt. Acceptable.
 */
/* 200000 complex ~= 5.12 s at 39062.5 Hz. Earlier 40000-cap (1024 ms) hid
 * the fact that the raspy might manifest LATER in an attempt — all dump
 * files ended up exactly the same size (cap hit) even though attempts
 * differed in length, so any comparison beyond 1024 ms was impossible.
 * At ~3.2 MB per attempt x ~20 attempts = ~64 MB of disk per session. OK. */
#define IQ_DUMP_MAX_COMPLEX 200000
static double iq_dump_buf[IQ_DUMP_MAX_COMPLEX * 2];
static volatile LONG iq_dump_count = 0;      /* number of complex samples captured */
static volatile LONG iq_dump_dropped = 0;    /* samples dropped because cap was hit */
static LONG iq_dump_attempt_id = 0;
static char iq_dump_dir[MAX_PATH] = {0};
static volatile LONG iq_dump_enabled = 0;    /* 1 during PTT=1, 0 otherwise */

static void iq_dump_init_path(void)
{
    char *slash;
    DWORD len;
    if (iq_dump_dir[0] != '\0') return;
    len = GetModuleFileNameA(NULL, iq_dump_dir, (DWORD)sizeof(iq_dump_dir));
    if (len == 0 || len >= sizeof(iq_dump_dir)) {
        strncpy(iq_dump_dir, ".", sizeof(iq_dump_dir) - 1);
        iq_dump_dir[sizeof(iq_dump_dir) - 1] = '\0';
        return;
    }
    slash = strrchr(iq_dump_dir, '\\');
    if (!slash) slash = strrchr(iq_dump_dir, '/');
    if (slash) *slash = '\0';
    else {
        strncpy(iq_dump_dir, ".", sizeof(iq_dump_dir) - 1);
        iq_dump_dir[sizeof(iq_dump_dir) - 1] = '\0';
    }
}

/* Background thread that deletes leftover sunsdr_tx_iq_*.raw files.
 *
 * Previously called synchronously from SunSDRInit — but that ran on the
 * Thetis managed-side init thread racing against VAC/PortAudio setup.
 * Even ~50 ms of synchronous DeleteFileA calls was enough to derail the
 * audio chain bring-up and leave startup RX with no output. Moving to a
 * detached thread so init returns instantly and the filesystem work
 * happens asynchronously. */
static DWORD WINAPI iq_dump_cleanup_thread_proc(LPVOID param)
{
    WIN32_FIND_DATAA ffd;
    HANDLE hFind;
    char pattern[MAX_PATH];
    char path[MAX_PATH];
    int deleted = 0;
    (void)param;

    iq_dump_init_path();
    _snprintf(pattern, sizeof(pattern), "%s\\sunsdr_tx_iq_*.raw", iq_dump_dir);
    pattern[sizeof(pattern) - 1] = '\0';

    hFind = FindFirstFileA(pattern, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;
    do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        _snprintf(path, sizeof(path), "%s\\%s", iq_dump_dir, ffd.cFileName);
        path[sizeof(path) - 1] = '\0';
        if (DeleteFileA(path)) deleted++;
    } while (FindNextFileA(hFind, &ffd));
    FindClose(hFind);

    if (deleted > 0) {
        sdr_logf("IQ_DUMP_CLEANUP deleted %d old sunsdr_tx_iq_*.raw files (async)\n", deleted);
    }
    return 0;
}

static void iq_dump_cleanup_old_files(void)
{
#if !SUNSDR_IQ_DUMP_ENABLED
    return;
#else
    /* Fire-and-forget background thread — do NOT block SunSDRInit. */
    HANDLE h = CreateThread(NULL, 0, iq_dump_cleanup_thread_proc, NULL, 0, NULL);
    if (h) CloseHandle(h);  /* detach; thread runs to completion on its own */
#endif
}

static void iq_dump_reset(int attempt_id)
{
#if !SUNSDR_IQ_DUMP_ENABLED
    (void)attempt_id;
    InterlockedExchange(&iq_dump_enabled, 0);
    InterlockedExchange(&iq_dump_count, 0);
    InterlockedExchange(&iq_dump_dropped, 0);
    return;
#else
    InterlockedExchange(&iq_dump_count, 0);
    InterlockedExchange(&iq_dump_dropped, 0);
    iq_dump_attempt_id = attempt_id;
    InterlockedExchange(&iq_dump_enabled, 1);
#endif
}

static void iq_dump_append(double I, double Q)
{
#if !SUNSDR_IQ_DUMP_ENABLED
    (void)I;
    (void)Q;
    return;
#else
    LONG n;
    int idx;
    if (!InterlockedAnd(&iq_dump_enabled, 0xffffffff)) return;
    n = InterlockedIncrement(&iq_dump_count);
    if (n > IQ_DUMP_MAX_COMPLEX) {
        /* Cap hit — count drops so we can tell post-hoc whether the
         * attempt was longer than the buffer. Do NOT spin-log here,
         * this is called at 39062.5 Hz. */
        InterlockedIncrement(&iq_dump_dropped);
        /* Leave iq_dump_count growing so flush-to-disk can see how many
         * samples we TRIED to capture vs how many fit. */
        return;
    }
    idx = (int)(n - 1) * 2;
    iq_dump_buf[idx + 0] = I;
    iq_dump_buf[idx + 1] = Q;
#endif
}

/* Per-attempt snapshot buffer handed off to the background writer.
 * Allocated each PTT-off, freed by the writer thread. Decouples the
 * PTT-off hot path (where SunSDRSetPTT is mid-flight and the pacing
 * thread is still emitting) from the 2-3 MB disk write. */
typedef struct iq_dump_job {
    double *buf;              /* malloc'd copy of the captured samples */
    LONG   n_complex;         /* number of complex samples in buf */
    LONG   dropped;
    LONG   attempt_id;
} iq_dump_job_t;

static DWORD WINAPI iq_dump_writer_thread_proc(LPVOID p)
{
    iq_dump_job_t *job = (iq_dump_job_t *)p;
    FILE *f;
    char path[MAX_PATH];

    iq_dump_init_path();
    _snprintf(path, sizeof(path), "%s\\sunsdr_tx_iq_%04ld.raw",
        iq_dump_dir, job->attempt_id);
    path[sizeof(path) - 1] = '\0';

    f = fopen(path, "wb");
    if (!f) {
        sdr_logf("IQ_DUMP_FAIL attempt=%ld err=%d path=%s\n",
            job->attempt_id, errno, path);
    } else {
        fwrite(job->buf, sizeof(double), (size_t)job->n_complex * 2, f);
        fclose(f);
        sdr_logf("IQ_DUMP_WROTE attempt=%ld complex_samples=%ld dropped=%ld cap=%d bytes=%zu path=%s (async)\n",
            job->attempt_id, job->n_complex, job->dropped, IQ_DUMP_MAX_COMPLEX,
            (size_t)job->n_complex * 2 * sizeof(double), path);
    }

    free(job->buf);
    free(job);
    return 0;
}

static void iq_dump_flush_to_disk(void)
{
#if !SUNSDR_IQ_DUMP_ENABLED
    InterlockedExchange(&iq_dump_enabled, 0);
    InterlockedExchange(&iq_dump_count, 0);
    InterlockedExchange(&iq_dump_dropped, 0);
    return;
#else
    LONG n;
    iq_dump_job_t *job;
    HANDLE h;

    /* Stop the producer from writing more samples first. */
    InterlockedExchange(&iq_dump_enabled, 0);

    n = InterlockedAnd(&iq_dump_count, 0xffffffff);
    if (n <= 0) return;
    if (n > IQ_DUMP_MAX_COMPLEX) n = IQ_DUMP_MAX_COMPLEX;

    /* Copy the captured samples into a dedicated heap buffer so the
     * writer thread is decoupled from iq_dump_buf. iq_dump_buf is
     * overwritten by the next attempt's iq_dump_reset. */
    job = (iq_dump_job_t *)malloc(sizeof(iq_dump_job_t));
    if (!job) {
        sdr_logf("IQ_DUMP_FAIL attempt=%ld could not alloc job\n", iq_dump_attempt_id);
        return;
    }
    job->buf = (double *)malloc((size_t)n * 2 * sizeof(double));
    if (!job->buf) {
        sdr_logf("IQ_DUMP_FAIL attempt=%ld could not alloc %zu bytes\n",
            iq_dump_attempt_id, (size_t)n * 2 * sizeof(double));
        free(job);
        return;
    }
    memcpy(job->buf, iq_dump_buf, (size_t)n * 2 * sizeof(double));
    job->n_complex = n;
    job->dropped = InterlockedAnd(&iq_dump_dropped, 0xffffffff);
    job->attempt_id = iq_dump_attempt_id;

    /* Fire-and-forget writer thread. Do NOT block SunSDRSetPTT while
     * we write 2-3 MB to disk — that was keeping the radio keyed for
     * 100+ ms past tune-off, producing an audible silent tail on
     * long attempts (attempt 13 diagnosis 2026-04-14). */
    h = CreateThread(NULL, 0, iq_dump_writer_thread_proc, job, 0, NULL);
    if (h) {
        CloseHandle(h);  /* detach */
    } else {
        sdr_logf("IQ_DUMP_FAIL attempt=%ld CreateThread err=%lu\n",
            iq_dump_attempt_id, GetLastError());
        free(job->buf);
        free(job);
    }
#endif
}

/* ============================================================
 * Tier 3 — TX packet pacing thread
 *
 * EESDR voice-MOX + TUNE wire captures (20260413_171304 and
 * 20260409_090411 in sunsdr-re/captures/) show EESDR emits TX packets
 * with stdev 0.098 ms, max 6.30 ms, ZERO gaps over 8 ms. Our Thetis
 * emission shows fdGapMax=16.000 ms on every TUNE attempt — a 16x
 * worse jitter driven by scheduler-tick events on the WDSP TX callback
 * thread. The radio's DAC expects samples at a steady 5.12 ms cadence
 * (195.3125 pps, 200 samples/pkt at 39062.5 Hz); when our emission
 * bursts (one packet 16 ms late then the next immediate), the DAC
 * either underruns or gets jammed, producing the audible "raspy"
 * distortion that only occasionally manifests depending on where the
 * burst lands in the tone/voice waveform.
 *
 * Solution: decouple wire emission from WDSP callback scheduling.
 * WDSP callback builds the packet and pushes to a small ring. A
 * dedicated pacing thread at THREAD_PRIORITY_TIME_CRITICAL waits on
 * a CREATE_WAITABLE_TIMER_HIGH_RESOLUTION timer set to 5.12 ms and
 * pops exactly one packet per tick, sending it to the wire.
 *
 * Invariant: exactly one FD packet per 5.12 ms tick while PTT=1.
 * Under-supply (ring empty): skip the tick (log underrun).
 * Over-supply (ring full): drop the incoming packet (log drop).
 */
#define TX_PACE_RING_SLOTS 32                 /* 32 pkts * 5.12 ms = 163 ms buffer depth */
#define TX_PACE_TICK_100NS ((LONGLONG)-51200) /* 5.12 ms in negative 100ns units (relative) */

/* Some older Windows SDKs don't define these flags. Provide fallbacks so
 * the code still compiles; at runtime CreateWaitableTimerExW will just
 * ignore unknown bits and we'll get the default ~15 ms timer (which is
 * why we still have timeBeginPeriod(1) for a safety net). */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef CREATE_WAITABLE_TIMER_MANUAL_RESET
#define CREATE_WAITABLE_TIMER_MANUAL_RESET    0x00000001
#endif

static unsigned char tx_pace_ring[TX_PACE_RING_SLOTS][SUNSDR_IQ_PKT_SIZE];
static volatile LONG tx_pace_head = 0;         /* next producer slot */
static volatile LONG tx_pace_tail = 0;         /* next consumer slot */
static volatile LONG tx_pace_drops = 0;        /* packets dropped due to ring full */
static volatile LONG tx_pace_underruns = 0;    /* ticks with empty ring (during PTT=1) */
static volatile LONG tx_pace_emitted = 0;      /* packets emitted by pacing thread */
static HANDLE tx_pace_timer = NULL;
static HANDLE tx_pace_thread = NULL;
static HANDLE tx_pace_ptt_event = NULL;   /* signalled on PTT=1, reset on PTT=0 */
static volatile LONG tx_pace_stop = 0;
static volatile LONG tx_pace_ready = 0;

/* Last emitted packet — repeated on underrun so the radio's DAC always
 * has a packet every 5.12 ms tick (no on-wire silence gaps that would
 * surface as audible pulses). Protected by tx_pace_last_valid + the
 * pacing thread being the only writer. */
static unsigned char tx_pace_last_packet[SUNSDR_IQ_PKT_SIZE];
static volatile LONG tx_pace_last_valid = 0;
static volatile LONG tx_pace_repeats = 0;

static DWORD WINAPI tx_pace_thread_proc(LPVOID lp)
{
    LARGE_INTEGER due;
    BOOL sp_ok;
    int sp_val;
    (void)lp;

    /* HIGHEST (priority 2 within ABOVE_NORMAL class → base priority 13) —
     * above the RX read thread (ABOVE_NORMAL, base 11) but below the
     * TIME_CRITICAL tier. TIME_CRITICAL preempted the RX read thread at
     * 195 Hz during its UDP recv windows, producing vertical stripes in
     * the RX waterfall (dropped/late RX packets). HIGHEST still wins CPU
     * against UI and gives us the 5.12 ms precision we need (aided by
     * timeBeginPeriod(1) and the high-res waitable timer). */
    sp_ok = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    sp_val = GetThreadPriority(GetCurrentThread());
    sdr_logf("TX_PACE_INIT thread priority rc=%d readback=%d (HIGHEST=2)\n",
        (int)sp_ok, sp_val);

    while (!InterlockedAnd(&tx_pace_stop, 0xffffffff)) {
        /* Block until PTT goes high (auto-reset event). During RX/idle the
         * thread is fully asleep — zero CPU impact on the RX path. When
         * PTT=1 this wakes and we enter the 5.12 ms pacing loop. */
        WaitForSingleObject(tx_pace_ptt_event, INFINITE);
        if (InterlockedAnd(&tx_pace_stop, 0xffffffff)) break;

        /* Pre-buffer: wait only for the FIRST producer packet before we
         * start emitting. Reference EESDR capture shows first FD packet
         * on wire 7 ms after 0x06 PTT-on ack; our previous 4-packet
         * pre-buffer was adding 12-20 ms of delay, and we saw 1/10
         * attempts where the radio accepted 0x06 but never produced
         * RF — suspected cause is the radio's TX watchdog interpreting
         * the gap as abort. Running ring underruns (if any) are now
         * handled by the emit-last-packet path below, which was added
         * after the pre-buffer fix and makes the 4-packet head-start
         * redundant. */
        {
            int wait_ms = 0;
            const int prebuf = 1;
            const int max_wait_ms = 20;  /* cap so TX still starts */
            while (sdr.currentPTT && !InterlockedAnd(&tx_pace_stop, 0xffffffff)) {
                LONG h = InterlockedAnd(&tx_pace_head, 0xffffffff);
                LONG t = InterlockedAnd(&tx_pace_tail, 0xffffffff);
                LONG avail = (h >= t) ? (h - t) : (TX_PACE_RING_SLOTS - t + h);
                if (avail >= prebuf) break;
                if (wait_ms >= max_wait_ms) break;
                Sleep(1);
                wait_ms++;
            }
            sdr_logf("TX_PACE_WARMUP waited_ms=%d target_prebuf=%d\n",
                wait_ms, prebuf);
        }

        /* Inner loop: emit at exactly 5.12 ms cadence while PTT=1.
         *
         * Absolute-schedule pattern: each tick has a target QPC value
         * = first_tick_qpc + N * ticks_per_5.12ms. We sleep until the
         * target, do the work, advance target by one tick, repeat.
         * Any work-time slop does NOT accumulate into the next tick.
         * An earlier one-shot relative-timer design drifted to ~5.5 ms
         * average (181 pps vs 195 pps target) because each cycle was
         * 5.12 ms + work. That drift caused ring fills and producer
         * drops, producing audible pulses on the wire from lost
         * packets. */
        {
            LARGE_INTEGER qpc_freq, qpc_now;
            LONGLONG qpc_per_tick;  /* QPC ticks per 5.12 ms */
            LONGLONG next_tick_qpc;

            QueryPerformanceFrequency(&qpc_freq);
            qpc_per_tick = (qpc_freq.QuadPart * 512) / 100000;  /* 5.12 ms */
            QueryPerformanceCounter(&qpc_now);
            next_tick_qpc = qpc_now.QuadPart + qpc_per_tick;

        while (sdr.currentPTT && !InterlockedAnd(&tx_pace_stop, 0xffffffff)) {
            /* Sleep until absolute target time for this tick. */
            LONGLONG wait_qpc;
            QueryPerformanceCounter(&qpc_now);
            wait_qpc = next_tick_qpc - qpc_now.QuadPart;
            if (wait_qpc > 0) {
                LONGLONG wait_100ns = (wait_qpc * 10000000) / qpc_freq.QuadPart;
                if (wait_100ns > 0) {
                    due.QuadPart = -wait_100ns;
                    if (SetWaitableTimer(tx_pace_timer, &due, 0, NULL, NULL, FALSE)) {
                        WaitForSingleObject(tx_pace_timer, INFINITE);
                    } else {
                        sdr_logf("TX_PACE_ERR SetWaitableTimer failed err=%lu\n", GetLastError());
                        Sleep(5);
                    }
                }
            }
            /* Schedule the NEXT tick at (current_target + 5.12ms), NOT at
             * (now + 5.12ms). That's the whole point of absolute scheduling. */
            next_tick_qpc += qpc_per_tick;

            if (InterlockedAnd(&tx_pace_stop, 0xffffffff)) break;

            if (!sdr.currentPTT) {
                /* PTT just dropped — drain any remaining ring entries so
                 * stale packets don't get emitted on the next PTT-on. */
                InterlockedExchange(&tx_pace_tail, InterlockedAnd(&tx_pace_head, 0xffffffff));
                break;  /* back to outer loop to wait on event */
            }

            {
                struct sockaddr_in dest = sunsdr_stream_dest();
                LONG t = InterlockedAnd(&tx_pace_tail, 0xffffffff);
                LONG h = InterlockedAnd(&tx_pace_head, 0xffffffff);
                int had_packet = (t != h);
                const unsigned char* pkt_to_send = NULL;

                if (sdr.streamSock == INVALID_SOCKET || sdr.streamSock == 0)
                    continue;

                if (had_packet) {
                    pkt_to_send = tx_pace_ring[t];
                    /* Cache this packet so we can repeat it on a future
                     * underrun instead of producing an on-wire gap. */
                    memcpy(tx_pace_last_packet, tx_pace_ring[t], SUNSDR_IQ_PKT_SIZE);
                    InterlockedExchange(&tx_pace_last_valid, 1);
                } else {
                    /* Underrun — ring empty. If we've emitted anything this
                     * session, repeat the last-known packet to keep the
                     * radio's DAC fed on its strict 5.12 ms cadence. This
                     * turns a 5 ms on-wire silence gap (audible "pulse")
                     * into a one-tick content repeat (much less audible;
                     * adjacent packets of a steady tone are nearly
                     * identical anyway). */
                    InterlockedIncrement(&tx_pace_underruns);
                    if (InterlockedAnd(&tx_pace_last_valid, 0xffffffff)) {
                        pkt_to_send = tx_pace_last_packet;
                        InterlockedIncrement(&tx_pace_repeats);
                    } else {
                        /* Nothing to repeat yet — skip this tick. Only
                         * happens at very first tick post-PTT-on before
                         * the producer has delivered anything. */
                        continue;
                    }
                }

                sendto(sdr.streamSock, (const char*)pkt_to_send, SUNSDR_IQ_PKT_SIZE, 0,
                    (struct sockaddr*)&dest, sizeof(dest));

                {
                    /* Reconstruct seq from the buffer for the packet-gap
                     * diagnostic so our existing fdGap* stats remain valid. */
                    unsigned int seq = (unsigned int)pkt_to_send[6] |
                                       ((unsigned int)pkt_to_send[7] << 8);
                    sdr.txAudioPackets++;
                    sunsdr_dbg_note_tx_packet(seq);
                    InterlockedIncrement(&tx_pace_emitted);
                }

                if (had_packet) {
                    InterlockedExchange(&tx_pace_tail, (t + 1) % TX_PACE_RING_SLOTS);
                }
            }
        }   /* end inner while (PTT=1) */
        }   /* end QPC-schedule scope */

        /* PTT just dropped — reset last-packet cache so the next TX
         * session doesn't start by repeating a stale packet from the
         * previous session. */
        InterlockedExchange(&tx_pace_last_valid, 0);
    }       /* end outer while (stop) */

    sdr_logf("TX_PACE_EXIT drops=%ld underruns=%ld repeats=%ld emitted=%ld\n",
        tx_pace_drops, tx_pace_underruns, tx_pace_repeats, tx_pace_emitted);
    return 0;
}

static void tx_pace_start(void)
{
    DWORD tid;
    if (InterlockedAnd(&tx_pace_ready, 0xffffffff)) return;

    tx_pace_head = 0;
    tx_pace_tail = 0;
    InterlockedExchange(&tx_pace_drops, 0);
    InterlockedExchange(&tx_pace_underruns, 0);
    InterlockedExchange(&tx_pace_emitted, 0);
    InterlockedExchange(&tx_pace_stop, 0);

    /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION: Windows 10 1803+. Gives
     * sub-millisecond scheduling precision on SetWaitableTimer.
     * Fallback to default flags if the high-res flag isn't honored —
     * CreateWaitableTimerEx returns NULL in that case, and we retry
     * without the flag. */
    tx_pace_timer = CreateWaitableTimerExW(NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET,
        TIMER_ALL_ACCESS);
    if (!tx_pace_timer) {
        sdr_logf("TX_PACE_INIT high-res timer unavailable (err=%lu), falling back to default\n",
            GetLastError());
        tx_pace_timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    if (!tx_pace_timer) {
        sdr_logf("TX_PACE_INIT FAIL could not create timer err=%lu\n", GetLastError());
        return;
    }

    /* Auto-reset event used to wake the pacing thread only when PTT=1.
     * During RX/idle the thread is blocked on this event with zero CPU
     * impact on the RX read thread. */
    tx_pace_ptt_event = CreateEventW(NULL, FALSE /*auto-reset*/, FALSE /*initial non-signaled*/, NULL);
    if (!tx_pace_ptt_event) {
        sdr_logf("TX_PACE_INIT FAIL could not create PTT event err=%lu\n", GetLastError());
        CloseHandle(tx_pace_timer);
        tx_pace_timer = NULL;
        return;
    }

    tx_pace_thread = CreateThread(NULL, 0, tx_pace_thread_proc, NULL, 0, &tid);
    if (!tx_pace_thread) {
        sdr_logf("TX_PACE_INIT FAIL could not create thread err=%lu\n", GetLastError());
        CloseHandle(tx_pace_timer);
        tx_pace_timer = NULL;
        CloseHandle(tx_pace_ptt_event);
        tx_pace_ptt_event = NULL;
        return;
    }

    InterlockedExchange(&tx_pace_ready, 1);
    sdr_logf("TX_PACE_INIT started tid=%lu ring_slots=%d tick_us=%lld\n",
        (unsigned long)tid, TX_PACE_RING_SLOTS, (long long)(-TX_PACE_TICK_100NS / 10));
}

static void tx_pace_stop_and_join(void)
{
    if (!InterlockedAnd(&tx_pace_ready, 0xffffffff)) return;
    InterlockedExchange(&tx_pace_stop, 1);
    /* Wake the thread if it's blocked on the PTT event, and cancel any
     * in-flight timer wait. */
    if (tx_pace_ptt_event) SetEvent(tx_pace_ptt_event);
    if (tx_pace_timer) CancelWaitableTimer(tx_pace_timer);
    if (tx_pace_thread) {
        WaitForSingleObject(tx_pace_thread, 2000);
        CloseHandle(tx_pace_thread);
        tx_pace_thread = NULL;
    }
    if (tx_pace_timer) {
        CloseHandle(tx_pace_timer);
        tx_pace_timer = NULL;
    }
    if (tx_pace_ptt_event) {
        CloseHandle(tx_pace_ptt_event);
        tx_pace_ptt_event = NULL;
    }
    InterlockedExchange(&tx_pace_ready, 0);
}

/* Producer side — called from WDSP callback (sunsdr_queue_tx_packet_locked)
 * when a complete 200-complex-sample packet has been accumulated in
 * sdr.txAccumBuf. Builds the wire packet into the next ring slot and
 * advances head. If the ring is full, increment drop counter and return
 * without advancing head (silently discard the new packet to preserve
 * the pacing thread's steady tick). */
static void tx_pace_push_packet_locked(const double* iq)
{
    LONG h, next, t;
    unsigned int seq;

    if (!InterlockedAnd(&tx_pace_ready, 0xffffffff)) {
        /* Pacing thread not running — fallback to legacy direct send
         * so TX still works in the unlikely startup race. */
        sunsdr_send_tx_packet(iq);
        return;
    }

    h = InterlockedAnd(&tx_pace_head, 0xffffffff);
    next = (h + 1) % TX_PACE_RING_SLOTS;
    t = InterlockedAnd(&tx_pace_tail, 0xffffffff);
    if (next == t) {
        InterlockedIncrement(&tx_pace_drops);
        return;
    }

    seq = sdr.txSeq++;
    sunsdr_build_tx_packet(tx_pace_ring[h], seq, iq);
    InterlockedExchange(&tx_pace_head, next);
}

/* Debug log: async deferred logger.
 *
 * Callers invoke sdr_logf() as before. Instead of doing synchronous fprintf +
 * fflush on the caller's thread, the formatted message is placed into a ring
 * buffer and a dedicated low-priority writer thread drains the ring to the
 * log file with batched fflush. This removes the unpredictable 2-50 ms disk
 * stalls that synchronous fflush imposes on the IQ read / TX callback / PTT
 * transition paths.
 *
 * Design:
 * - Fixed 4096-slot ring, each slot holds a pre-formatted line up to 512
 *   bytes (including timestamp and caller newline).
 * - Producer path: GetLocalTime + vsnprintf into stack buffer, then memcpy
 *   under a critical section into the ring slot. Signal a manual-reset event.
 * - Writer thread: waits on the event (100 ms timeout as safety), drains the
 *   ring, batches fputs, issues one fflush per drain pass.
 * - Ring-full policy: drop newest, increment dropped counter, emit a summary
 *   line from the writer thread on next pass.
 * - If the writer thread isn't running yet (pre-init), fall back to the
 *   legacy synchronous path so early startup messages still reach the file.
 */
static FILE* sdr_log = NULL;
static int sdr_log_initialized = 0;
static char sdr_log_path[MAX_PATH];

#define SDR_LOG_RING_SIZE 4096
#define SDR_LOG_ENTRY_MAX 512

/* Power request handle for PowerSetRequest(PowerRequestExecutionRequired).
 * Kept active for the whole SunSDR session so the CPU does not enter deep
 * C-states or downclock via DVFS during light UI periods between TX bursts. */
static HANDLE sdr_power_request = NULL;

static char sdr_log_ring[SDR_LOG_RING_SIZE][SDR_LOG_ENTRY_MAX];
static volatile unsigned sdr_log_ring_head = 0;  /* next write slot */
static volatile unsigned sdr_log_ring_tail = 0;  /* next read slot  */
static CRITICAL_SECTION sdr_log_ring_cs;
static int sdr_log_cs_init = 0;
static HANDLE sdr_log_wake = NULL;
static HANDLE sdr_log_thread = NULL;
static volatile LONG sdr_log_dropped = 0;
static volatile int sdr_log_stop = 0;
static volatile int sdr_log_async_ready = 0;

static void sdr_log_build_path(void)
{
    DWORD len;
    char* slash;

    if (sdr_log_path[0] != '\0') {
        return;
    }

    len = GetModuleFileNameA(NULL, sdr_log_path, (DWORD)sizeof(sdr_log_path));
    if (len == 0 || len >= sizeof(sdr_log_path)) {
        strncpy(sdr_log_path, "sunsdr_debug.log", sizeof(sdr_log_path) - 1);
        sdr_log_path[sizeof(sdr_log_path) - 1] = '\0';
        return;
    }

    slash = strrchr(sdr_log_path, '\\');
    if (!slash) {
        slash = strrchr(sdr_log_path, '/');
    }

    if (slash) {
        slash[1] = '\0';
        strncat(sdr_log_path, "sunsdr_debug.log", sizeof(sdr_log_path) - strlen(sdr_log_path) - 1);
    } else {
        strncpy(sdr_log_path, "sunsdr_debug.log", sizeof(sdr_log_path) - 1);
        sdr_log_path[sizeof(sdr_log_path) - 1] = '\0';
    }
}

static void sdr_log_open(void) {
#if !SUNSDR_DEBUG_LOG_ENABLED
    return;
#else
    if (!sdr_log) {
        const char* mode;

        sdr_log_build_path();
        mode = sdr_log_initialized ? "a" : "w";
        sdr_log = fopen(sdr_log_path, mode);
        if (sdr_log) {
            SYSTEMTIME st;
            sdr_log_initialized = 1;
            GetLocalTime(&st);
            fprintf(sdr_log, "===== SunSDR session %04d-%02d-%02d %02d:%02d:%02d.%03d =====\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            fprintf(sdr_log, "Log path: %s\n", sdr_log_path);
            fflush(sdr_log);
        }
    }
#endif
}

static DWORD WINAPI sdr_log_writer_thread(LPVOID param) {
    (void)param;
    /* Writer thread: drains ring buffer and writes to file with batched flush. */
    while (!sdr_log_stop) {
        WaitForSingleObject(sdr_log_wake, 100);

        /* Drain loop: pop one entry at a time from the ring. */
        for (;;) {
            char local[SDR_LOG_ENTRY_MAX];
            int have = 0;

            EnterCriticalSection(&sdr_log_ring_cs);
            if (sdr_log_ring_tail != sdr_log_ring_head) {
                memcpy(local, sdr_log_ring[sdr_log_ring_tail], SDR_LOG_ENTRY_MAX);
                local[SDR_LOG_ENTRY_MAX - 1] = '\0';
                sdr_log_ring_tail = (sdr_log_ring_tail + 1) % SDR_LOG_RING_SIZE;
                have = 1;
            }
            LeaveCriticalSection(&sdr_log_ring_cs);

            if (!have) break;

            if (!sdr_log) sdr_log_open();
            if (sdr_log) fputs(local, sdr_log);
        }

        /* One batched flush per drain pass. */
        if (sdr_log) fflush(sdr_log);

        /* Report drops, if any. */
        {
            LONG dropped = InterlockedExchange(&sdr_log_dropped, 0);
            if (dropped > 0 && sdr_log) {
                fprintf(sdr_log, "sdr_log: dropped %ld messages (ring full)\n", dropped);
                fflush(sdr_log);
            }
        }
    }

    /* Final drain on shutdown. */
    EnterCriticalSection(&sdr_log_ring_cs);
    while (sdr_log_ring_tail != sdr_log_ring_head) {
        if (sdr_log) fputs(sdr_log_ring[sdr_log_ring_tail], sdr_log);
        sdr_log_ring_tail = (sdr_log_ring_tail + 1) % SDR_LOG_RING_SIZE;
    }
    LeaveCriticalSection(&sdr_log_ring_cs);
    if (sdr_log) fflush(sdr_log);

    return 0;
}

static void sdr_log_async_start(void) {
#if !SUNSDR_DEBUG_LOG_ENABLED
    return;
#else
    if (sdr_log_async_ready) return;
    if (!sdr_log_cs_init) {
        InitializeCriticalSection(&sdr_log_ring_cs);
        sdr_log_cs_init = 1;
    }
    if (!sdr_log_wake) {
        sdr_log_wake = CreateEvent(NULL, FALSE, FALSE, NULL);  /* auto-reset */
        if (!sdr_log_wake) return;
    }
    sdr_log_stop = 0;
    if (!sdr_log_thread) {
        sdr_log_thread = CreateThread(NULL, 0, sdr_log_writer_thread, NULL, 0, NULL);
        if (sdr_log_thread) {
            SetThreadPriority(sdr_log_thread, THREAD_PRIORITY_BELOW_NORMAL);
        }
    }
    if (sdr_log_thread) {
        sdr_log_async_ready = 1;
    }
#endif
}

static void sdr_log_async_stop(void) {
#if !SUNSDR_DEBUG_LOG_ENABLED
    return;
#else
    sdr_log_async_ready = 0;
    sdr_log_stop = 1;
    if (sdr_log_wake) SetEvent(sdr_log_wake);
    if (sdr_log_thread) {
        WaitForSingleObject(sdr_log_thread, 2000);
        CloseHandle(sdr_log_thread);
        sdr_log_thread = NULL;
    }
    if (sdr_log_wake) {
        CloseHandle(sdr_log_wake);
        sdr_log_wake = NULL;
    }
#endif
}

static void sdr_logf(const char* fmt, ...) {
#if !SUNSDR_DEBUG_LOG_ENABLED
    (void)fmt;
    return;
#else
    va_list ap;
    SYSTEMTIME st;
    char local[SDR_LOG_ENTRY_MAX];
    int prefix_len;
    int remaining;

    GetLocalTime(&st);
    prefix_len = _snprintf(local, SDR_LOG_ENTRY_MAX,
        "[%02d:%02d:%02d.%03d] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (prefix_len < 0 || prefix_len >= SDR_LOG_ENTRY_MAX) prefix_len = 0;

    remaining = SDR_LOG_ENTRY_MAX - prefix_len - 1;
    if (remaining > 0) {
        va_start(ap, fmt);
        _vsnprintf(local + prefix_len, remaining, fmt, ap);
        va_end(ap);
    }
    local[SDR_LOG_ENTRY_MAX - 1] = '\0';  /* ensure null-terminated on overflow */

    /* Lazy-start the writer thread on first log call so callers don't need to
     * remember to init. Protected by async_ready flag (simple single-writer
     * check; multi-caller race is bounded by the CS InitializeCriticalSection
     * being idempotent via sdr_log_cs_init flag). */
    if (!sdr_log_async_ready) {
        sdr_log_async_start();
    }

    if (sdr_log_async_ready) {
        unsigned next;
        EnterCriticalSection(&sdr_log_ring_cs);
        next = (sdr_log_ring_head + 1) % SDR_LOG_RING_SIZE;
        if (next == sdr_log_ring_tail) {
            /* Ring full — drop this message. */
            LeaveCriticalSection(&sdr_log_ring_cs);
            InterlockedIncrement(&sdr_log_dropped);
        } else {
            memcpy(sdr_log_ring[sdr_log_ring_head], local, SDR_LOG_ENTRY_MAX);
            sdr_log_ring_head = next;
            LeaveCriticalSection(&sdr_log_ring_cs);
            SetEvent(sdr_log_wake);
        }
    } else {
        /* Fallback sync path — only used if async setup failed. */
        if (!sdr_log) sdr_log_open();
        if (!sdr_log) return;
        fputs(local, sdr_log);
        fflush(sdr_log);
    }
#endif
}
#include "cmbuffs.h"

/* ========== Internal state ========== */

/* sdr was moved to the forward-declaration block near the top of the
 * file so the iq_dump and tx_pace helpers can reference it. */
static const char* sdr_ctrl_trace_label = NULL;

static void sunsdr_set_identity_defaults(void)
{
    strncpy(sdr.firmwareVersionText, "Unknown", sizeof(sdr.firmwareVersionText) - 1);
    sdr.firmwareVersionText[sizeof(sdr.firmwareVersionText) - 1] = '\0';
    strncpy(sdr.protocolText, "SunSDR Native", sizeof(sdr.protocolText) - 1);
    sdr.protocolText[sizeof(sdr.protocolText) - 1] = '\0';
    strncpy(sdr.serialText, "Unknown", sizeof(sdr.serialText) - 1);
    sdr.serialText[sizeof(sdr.serialText) - 1] = '\0';
}

static const char* sunsdr_safe_label(const char* label)
{
    return (label && label[0]) ? label : "ctrl";
}

static int sunsdr_copy_text(char* buffer, int maxlen, const char* value)
{
    size_t len;

    if (!buffer || maxlen <= 0 || !value) {
        return 0;
    }

    len = strlen(value);
    if (len >= (size_t)maxlen) {
        len = (size_t)maxlen - 1;
    }

    memcpy(buffer, value, len);
    buffer[len] = '\0';
    return (int)len;
}

static void sunsdr_hexify(const unsigned char* data, int len, char* out, int outlen)
{
    int i;
    int pos = 0;

    if (!out || outlen <= 0) {
        return;
    }

    out[0] = '\0';
    if (!data || len <= 0) {
        return;
    }

    for (i = 0; i < len && pos + 2 < outlen; i++) {
        pos += _snprintf(out + pos, outlen - pos, "%02x", data[i]);
        if (pos < 0 || pos >= outlen) {
            out[outlen - 1] = '\0';
            return;
        }
    }
}

static void sunsdr_asciiify(const unsigned char* data, int len, char* out, int outlen)
{
    int i;
    int pos = 0;

    if (!out || outlen <= 0) {
        return;
    }

    out[0] = '\0';
    if (!data || len <= 0) {
        return;
    }

    for (i = 0; i < len && pos + 1 < outlen; i++) {
        unsigned char ch = data[i];
        out[pos++] = (ch >= 32 && ch <= 126) ? (char)ch : '.';
    }

    out[pos] = '\0';
}

static int sunsdr_is_versionish_ascii(const char* text)
{
    int i;
    int digits = 0;
    int dots = 0;

    if (!text || !text[0]) {
        return 0;
    }

    for (i = 0; text[i] != '\0'; i++) {
        if (text[i] >= '0' && text[i] <= '9') digits++;
        if (text[i] == '.') dots++;
    }

    return digits >= 2 && dots >= 1;
}

static void sunsdr_try_parse_identity_011a(const unsigned char* data, int len, const char* context)
{
    unsigned int device_suffix;
    char year_token[8];

    if (!data || len < 24) {
        return;
    }

    if (data[0] != 0x32 || data[1] != 0xFF || data[2] != 0x01 || data[3] != 0x1A) {
        return;
    }

    device_suffix = (unsigned int)data[4] | ((unsigned int)data[5] << 8);
    _snprintf(year_token, sizeof(year_token), "%02X%02X", data[8], data[9]);
    year_token[sizeof(year_token) - 1] = '\0';

    if (device_suffix > 0 && strcmp(year_token, "0000") != 0) {
        _snprintf(sdr.serialText, sizeof(sdr.serialText), "EED06%s%05u", year_token, device_suffix);
        sdr.serialText[sizeof(sdr.serialText) - 1] = '\0';
        sdr_logf("IDENTITY %s serial_inferred=%s year=%s suffix=%u\n",
            context ? context : "pkt",
            sdr.serialText,
            year_token,
            device_suffix);
    }
}

static void sunsdr_try_parse_serial_from_fwmgr(const unsigned char* data, int len, const char* context)
{
    unsigned int device_suffix;
    char year_token[8];

    if (!data || len < 40) {
        return;
    }

    if (data[0] != 0x32 || data[1] != 0xFF || data[2] != 0x1A || data[4] != 0x20 || data[5] != 0x00) {
        return;
    }

    device_suffix = (unsigned int)data[38] | ((unsigned int)data[39] << 8);
    _snprintf(year_token, sizeof(year_token), "%02X%02X", data[26], data[27]);
    year_token[sizeof(year_token) - 1] = '\0';

    if (device_suffix > 0 && strcmp(year_token, "0000") != 0) {
        _snprintf(sdr.serialText, sizeof(sdr.serialText), "EED06%s%05u", year_token, device_suffix);
        sdr.serialText[sizeof(sdr.serialText) - 1] = '\0';
        sdr_logf("IDENTITY %s serial_inferred_fwmgr=%s year=%s suffix=%u\n",
            context ? context : "pkt",
            sdr.serialText,
            year_token,
            device_suffix);
    }
}

static void sunsdr_cache_identity_candidate(const unsigned char* data, int len, const char* context)
{
    char ascii[256];
    int i;
    int run_start = -1;

    if (!data || len <= 0) {
        return;
    }

    sunsdr_try_parse_identity_011a(data, len, context);
    sunsdr_try_parse_serial_from_fwmgr(data, len, context);

    sunsdr_asciiify(data, len, ascii, (int)sizeof(ascii));
    if (strstr(ascii, "version") || strstr(ascii, "Version") || strstr(ascii, "boot") || strstr(ascii, "serial")) {
        sdr_logf("IDENTITY %s ascii=%s\n", context ? context : "pkt", ascii);
    }

    for (i = 0; i <= len; i++) {
        unsigned char ch = (i < len) ? data[i] : 0;
        int printable = (ch >= 32 && ch <= 126);

        if (printable) {
            if (run_start < 0) run_start = i;
            continue;
        }

        if (run_start >= 0) {
            int run_len = i - run_start;
            if (run_len >= 4) {
                char candidate[64];
                int copy_len = run_len;
                if (copy_len >= (int)sizeof(candidate)) copy_len = (int)sizeof(candidate) - 1;
                memcpy(candidate, data + run_start, copy_len);
                candidate[copy_len] = '\0';

                if (sunsdr_is_versionish_ascii(candidate)) {
                    strncpy(sdr.firmwareVersionText, candidate, sizeof(sdr.firmwareVersionText) - 1);
                    sdr.firmwareVersionText[sizeof(sdr.firmwareVersionText) - 1] = '\0';
                    sdr_logf("IDENTITY %s version_candidate=%s\n", context ? context : "pkt", sdr.firmwareVersionText);
                    return;
                }
            }
            run_start = -1;
        }
    }
}

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

/*
 * TX IQ stream rate. From live ExpertSDR3 voice MOX capture, the host sends
 * packets at 195.3 pkts/sec * 200 complex samples = 39,062.5 samples/sec.
 * The radio upsamples internally; we must NOT send at the RX IQ rate.
 * Our WDSP TX ch_outrate is 192,000 Hz (default sample_rate_tx). The resampler
 * in sunsdr_tx_outbound decimates 192k -> 39.0625k using phase-accumulator
 * fractional downsampling with linear interpolation between consecutive
 * input samples.
 */
#define SUNSDR_TX_INPUT_RATE   192000.0
#define SUNSDR_TX_OUTPUT_RATE  39062.5
#define SUNSDR_TX_RESAMPLE_STEP (SUNSDR_TX_INPUT_RATE / SUNSDR_TX_OUTPUT_RATE)  /* ~4.9152 */

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

static volatile LONG sunsdr_dbg_attempt_seq = 0;
static volatile LONG sunsdr_dbg_active_tx_packets = 0;
static volatile LONG sunsdr_dbg_active_tx_seq_gaps = 0;
static volatile LONG sunsdr_dbg_idle_fe_during_tx = 0;
static volatile LONG sunsdr_dbg_keepalive_tx_races = 0;
static volatile LONG sunsdr_dbg_tx_attempt_id = 0;
static volatile LONG sunsdr_tx_iq_enabled = 0;
static volatile LONG sunsdr_dbg_tx_iq_gate_skips = 0;
static volatile LONG sunsdr_dbg_first_fd_before_cmd = 0;
static ULONGLONG sunsdr_dbg_ptt_request_tick = 0;
static ULONGLONG sunsdr_dbg_mox_cmd_tick = 0;
static ULONGLONG sunsdr_dbg_first_fd_tick = 0;
static ULONGLONG sunsdr_dbg_last_fd_tick = 0;
static unsigned int sunsdr_dbg_last_fd_seq = 0;
static double sunsdr_dbg_fd_gap_min_ms = 0.0;
static double sunsdr_dbg_fd_gap_max_ms = 0.0;
static double sunsdr_dbg_fd_gap_sum_ms = 0.0;
static unsigned int sunsdr_dbg_fd_gap_count = 0;
static volatile LONG sunsdr_dbg_tx_cb_count = 0;
static volatile LONG sunsdr_dbg_tx_cb_silent = 0;
static volatile LONG sunsdr_dbg_tx_cb_nonfinite = 0;
static double sunsdr_dbg_tx_cb_rms_sum = 0.0;
static double sunsdr_dbg_tx_cb_rms_min = 0.0;
static double sunsdr_dbg_tx_cb_rms_max = 0.0;
static double sunsdr_dbg_tx_cb_peak_max = 0.0;
static ULONGLONG sunsdr_dbg_tx_cb_last_tick = 0;
static ULONGLONG sunsdr_dbg_tx_cb_max_gap_ms = 0;

/* TX callback QPC gap tracking + histogram. The TX callback runs on the
 * WDSP TX channel worker thread (spawned by cm_main/SendpOutboundTx), which
 * is NOT the RX read thread we already boosted. Run 10 log showed RX loop
 * never sees an 8+ ms gap during TUNE, but TX_ATTEMPT_END still reports
 * fdGapMax=16.000 ms on every attempt — so the stall is on THIS thread.
 *
 * Buckets (ms): [0] <1, [1] 1-2, [2] 2-4, [3] 4-8, [4] 8-16, [5] 16-32, [6] >=32
 * Normal: most callbacks sub-millisecond apart (nsamples=256 @ 192k = 1.33 ms).
 * Scheduler tick events would show in [4]/[5]. >=32 is pathology. */
#define SDR_TX_CB_HIST_BUCKETS 7
static LARGE_INTEGER sunsdr_dbg_tx_cb_last_qpc = {0};
static volatile LONG sunsdr_dbg_tx_cb_gap_hist[SDR_TX_CB_HIST_BUCKETS] = {0};

static const char* sunsdr_tx_mode_label(int tune)
{
    return tune ? "TUNE" : "MOX";
}

static void sunsdr_dbg_reset_tx_attempt_locked(int attempt_id)
{
    (void)attempt_id;
    InterlockedExchange(&sunsdr_dbg_active_tx_packets, 0);
    InterlockedExchange(&sunsdr_dbg_active_tx_seq_gaps, 0);
    InterlockedExchange(&sunsdr_dbg_idle_fe_during_tx, 0);
    InterlockedExchange(&sunsdr_dbg_keepalive_tx_races, 0);
    InterlockedExchange(&sunsdr_dbg_tx_iq_gate_skips, 0);
    InterlockedExchange(&sunsdr_dbg_first_fd_before_cmd, 0);
    sunsdr_dbg_ptt_request_tick = 0;
    sunsdr_dbg_mox_cmd_tick = 0;
    sunsdr_dbg_first_fd_tick = 0;
    sunsdr_dbg_last_fd_tick = 0;
    sunsdr_dbg_last_fd_seq = 0;
    sunsdr_dbg_fd_gap_min_ms = 0.0;
    sunsdr_dbg_fd_gap_max_ms = 0.0;
    sunsdr_dbg_fd_gap_sum_ms = 0.0;
    sunsdr_dbg_fd_gap_count = 0;
    InterlockedExchange(&sunsdr_dbg_tx_cb_count, 0);
    InterlockedExchange(&sunsdr_dbg_tx_cb_silent, 0);
    InterlockedExchange(&sunsdr_dbg_tx_cb_nonfinite, 0);
    sunsdr_dbg_tx_cb_rms_sum = 0.0;
    sunsdr_dbg_tx_cb_rms_min = 0.0;
    sunsdr_dbg_tx_cb_rms_max = 0.0;
    sunsdr_dbg_tx_cb_peak_max = 0.0;
    sunsdr_dbg_tx_cb_last_tick = 0;
    sunsdr_dbg_tx_cb_max_gap_ms = 0;
}

static void sunsdr_dbg_log_audio_diags(const char* label)
{
    int v;
    long cma_over_in = 0;
    long cma_over_out = 0;
    long cma_under_in = 0;
    long cma_under_out = 0;

    if (!label) {
        label = "unknown";
    }

    if (pcm && pcma) {
        if (pcma->rmatchIN && pcma->rmatchOUT) {
            getCMAevents(&cma_over_in, &cma_over_out, &cma_under_in, &cma_under_out);
        }

        sdr_logf("TX_AUDIO_DIAG %s CMA audioCodecId=%d pcma=1 run=%d block=%d lockMode=%d protocol=%d rmatchIN=%p rmatchOUT=%p directOverIn=%ld directOverOut=%ld directUnderIn=%ld directUnderOut=%ld eventOverIn=%ld eventOverOut=%ld eventUnderIn=%ld eventUnderOut=%ld\n",
            label,
            pcm->audioCodecId,
            pcma->run,
            pcma->blocksize,
            pcma->lockMode,
            pcma->protocol,
            pcma->rmatchIN,
            pcma->rmatchOUT,
            pcma->overFlowsIn,
            pcma->overFlowsOut,
            pcma->underFlowsIn,
            pcma->underFlowsOut,
            cma_over_in,
            cma_over_out,
            cma_under_in,
            cma_under_out);
    } else {
        sdr_logf("TX_AUDIO_DIAG %s CMA audioCodecId=%d pcma=%d\n",
            label,
            pcm ? pcm->audioCodecId : -1,
            pcma ? 1 : 0);
    }

    for (v = 0; v < 2 && v < MAX_EXT_VACS; v++) {
        IVAC vac = pvac[v];
        int out_under = 0;
        int out_over = 0;
        int out_ring = 0;
        int out_nring = 0;
        double out_var = 0.0;
        int in_under = 0;
        int in_over = 0;
        int in_ring = 0;
        int in_nring = 0;
        double in_var = 0.0;

        if (!vac) {
            sdr_logf("TX_AUDIO_DIAG %s VAC%d present=0\n", label, v + 1);
            continue;
        }

        if (vac->rmatchOUT) {
            getIVACdiags(v, 0, &out_under, &out_over, &out_var, &out_ring, &out_nring);
        }
        if (vac->rmatchIN) {
            getIVACdiags(v, 1, &in_under, &in_over, &in_var, &in_ring, &in_nring);
        }

        sdr_logf("TX_AUDIO_DIAG %s VAC%d present=1 run=%d iq=%d stereo=%d mox=%d mon=%d bypass=%d combine=%d rates_iq/mic/audio/txmon/vac=%d/%d/%d/%d/%d sizes_iq/mic/audio/txmon/vac=%d/%d/%d/%d/%d lat_in/out=%.4f/%.4f pa_lat_in/out=%.4f/%.4f rmatchOUT=%p outUnder=%d outOver=%d outVar=%.6f outRing=%d outNring=%d rmatchIN=%p inUnder=%d inOver=%d inVar=%.6f inRing=%d inNring=%d\n",
            label,
            v + 1,
            vac->run,
            vac->iq_type,
            vac->stereo,
            vac->mox,
            vac->mon,
            vac->vac_bypass,
            vac->vac_combine_input,
            vac->iq_rate,
            vac->mic_rate,
            vac->audio_rate,
            vac->txmon_rate,
            vac->vac_rate,
            vac->iq_size,
            vac->mic_size,
            vac->audio_size,
            vac->txmon_size,
            vac->vac_size,
            vac->in_latency,
            vac->out_latency,
            vac->pa_in_latency,
            vac->pa_out_latency,
            vac->rmatchOUT,
            out_under,
            out_over,
            out_var,
            out_ring,
            out_nring,
            vac->rmatchIN,
            in_under,
            in_over,
            in_var,
            in_ring,
            in_nring);
    }
}

static void sunsdr_dbg_note_tx_packet(unsigned int seq)
{
    ULONGLONG now = GetTickCount64();
    LONG count = InterlockedIncrement(&sunsdr_dbg_active_tx_packets);

    if (count == 1) {
        sunsdr_dbg_first_fd_tick = now;
        if (sunsdr_dbg_mox_cmd_tick == 0) {
            InterlockedExchange(&sunsdr_dbg_first_fd_before_cmd, 1);
        }
        sdr_logf("TX_FIRST_FD attempt=%ld seq=%u delay_from_ptt_ms=%llu delay_from_0x06_ms=%llu cmd_sent=%d\n",
            sunsdr_dbg_tx_attempt_id,
            seq,
            sunsdr_dbg_ptt_request_tick ? (unsigned long long)(now - sunsdr_dbg_ptt_request_tick) : 0,
            sunsdr_dbg_mox_cmd_tick ? (unsigned long long)(now - sunsdr_dbg_mox_cmd_tick) : 0,
            sunsdr_dbg_mox_cmd_tick != 0);
    } else {
        double gap_ms = (double)(now - sunsdr_dbg_last_fd_tick);
        if (sunsdr_dbg_fd_gap_count == 0 || gap_ms < sunsdr_dbg_fd_gap_min_ms) sunsdr_dbg_fd_gap_min_ms = gap_ms;
        if (gap_ms > sunsdr_dbg_fd_gap_max_ms) sunsdr_dbg_fd_gap_max_ms = gap_ms;
        sunsdr_dbg_fd_gap_sum_ms += gap_ms;
        sunsdr_dbg_fd_gap_count++;

        if (seq != sunsdr_dbg_last_fd_seq + 1) {
            LONG gaps = InterlockedIncrement(&sunsdr_dbg_active_tx_seq_gaps);
            sdr_logf("TX_FD_SEQ_GAP attempt=%ld prev=%u current=%u gaps=%ld\n",
                sunsdr_dbg_tx_attempt_id, sunsdr_dbg_last_fd_seq, seq, gaps);
        }
    }

    sunsdr_dbg_last_fd_seq = seq;
    sunsdr_dbg_last_fd_tick = now;
}
/*
 * Architecture reset 2026-04-15 (after iter-1..iter-8 LUT cycle
 * failed to produce linear UI->W):
 *
 * We are going back to EESDR's proven control topology:
 *   - Wire IQ peak = always ~1.0 (constant, matches EESDR captures at
 *     every drive level).
 *   - 0x17 drive byte = dynamic, derived from UI watts via an empirical
 *     forward-inverted LUT.
 *
 * The previous Codex architecture (fixed 0x17=0xCE, variable iq_gain)
 * was fighting the radio: the internal PA is calibrated for full-
 * amplitude wire IQ + drive-byte-encoded power. Holding the byte
 * fixed and scaling amplitude ran the PA in a non-linear regime
 * where response was state-dependent (iter-7 hit 107W max, iter-8
 * only 19.5W with the same architecture — unreproducible).
 *
 * iq_gain is now a compile-time constant that normalizes WDSP's
 * ~0.857 peak up to ~1.0 on wire (matching EESDR). If WDSP's output
 * peak ever shifts, adjust SUNSDR_WDSP_TX_PEAK_EST — nothing else.
 *
 * Drive byte LUT anchors come from iter-1 measurements (the only run
 * we have under this topology — full-amplitude wire + varying byte):
 *
 *   UI  10 W -> byte 0x50 (sent)  ->   3 W (measured)
 *   UI  25 W -> byte 0x80          ->  22 W
 *   UI  50 W -> byte 0xB5          ->  85 W
 *   UI  75 W -> byte 0xDD          -> 111 W
 *   UI 100 W -> byte 0xFF          -> 115 W
 *
 * Inverted to target_W -> byte:
 *
 *   3 W   -> 0x50 (80)
 *   22 W  -> 0x80 (128)
 *   85 W  -> 0xB5 (181)
 *   111 W -> 0xDD (221)
 *   115 W -> 0xFF (255)
 *
 * Interpolation between these anchors gives the byte for any target.
 * This is the initial guess; the plan is one dense sweep after
 * rebuild, then I replace the anchors with those measurements and
 * we are done.
 */
#define SUNSDR_WDSP_TX_PEAK_EST 0.857   /* measured from diagnostic log */
#define SUNSDR_TX_IQ_GAIN       (1.0 / SUNSDR_WDSP_TX_PEAK_EST) /* ~1.167 */

static double sunsdr_requested_watts_from_raw(int raw)
{
    if (raw < 0) return 100.0;
    if (raw > 255) raw = 255;
    return (double)raw * 100.0 / 255.0;
}

static double sunsdr_tx_iq_gain_for_watts(double watts)
{
    /* Wire IQ amplitude is no longer the power dial under the new
     * architecture. It is a constant normalization so our wire peak
     * matches EESDR's ~1.0. Returns 0 only when TX is commanded off. */
    if (watts <= 0.0) return 0.0;
    return SUNSDR_TX_IQ_GAIN;
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

static void sunsdr_build_iq_header(unsigned char* buf, int opcode, unsigned int seq, unsigned char byte8, unsigned char byte9)
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
    buf[8] = byte8;
    buf[9] = byte9;
}

static void sunsdr_build_tx_packet(unsigned char* buf, unsigned int seq, const double* iq)
{
    int i;
    unsigned char* payload = buf + SUNSDR_IQ_HDR_SIZE;

    /*
     * Host->radio TX audio packet: opcode 0xFD with byte8=0x02, byte9=0x01.
     * This is the TX-active stream opcode observed in live ExpertSDR3 voice
     * MOX captures. The radio routes 0xFD IQ through the TX audio path; 0xFE
     * is reserved for RX-state / idle keepalives.
     */
    sunsdr_build_iq_header(buf, SUNSDR_OP_IQ_TX_ACTIVE, seq, 0x02, 0x01);

    for (i = 0; i < SUNSDR_IQ_COMPLEX_PER_PKT; i++) {
        int I = sunsdr_quantize24(iq[2 * i + 0]);
        int Q = sunsdr_quantize24(iq[2 * i + 1]);
        int k = i * SUNSDR_IQ_BYTES_PER_IQ;

        /* TX wire order is I first, then Q (24-bit LE each).
         *
         * The radio's RX->host byte order is the opposite (Q-first — see
         * the RX unpack path). We verified the mirror empirically:
         * ground-truth IQ analysis of our Thetis TUNE output showed the
         * tone emitted at +600 Hz on wire when WDSP had generated a
         * -600 Hz tone. Flipping the encoder byte order to I-first
         * restores the correct sideband. Confirmed on-air by a QSO
         * partner reporting the TUNE tone landing at the expected
         * frequency after the swap. */
        payload[k + 0] = (unsigned char)(I & 0xFF);
        payload[k + 1] = (unsigned char)((I >> 8) & 0xFF);
        payload[k + 2] = (unsigned char)((I >> 16) & 0xFF);
        payload[k + 3] = (unsigned char)(Q & 0xFF);
        payload[k + 4] = (unsigned char)((Q >> 8) & 0xFF);
        payload[k + 5] = (unsigned char)((Q >> 16) & 0xFF);
    }
}

static void sunsdr_send_tx_packet(const double* iq)
{
    unsigned char txbuf[SUNSDR_IQ_PKT_SIZE];
    struct sockaddr_in dest;
    unsigned int seq;

    if (sdr.streamSock == INVALID_SOCKET || sdr.streamSock == 0)
        return;

    seq = sdr.txSeq++;
    sunsdr_build_tx_packet(txbuf, seq, iq);
    dest = sunsdr_stream_dest();
    sendto(sdr.streamSock, (const char*)txbuf, SUNSDR_IQ_PKT_SIZE, 0,
        (struct sockaddr*)&dest, sizeof(dest));
    sdr.txAudioPackets++;
    sunsdr_dbg_note_tx_packet(seq);
}

static void sunsdr_queue_tx_packet_locked(double I, double Q)
{
    int idx = sdr.txAccumCount;

    sdr.txAccumBuf[2 * idx + 0] = I;
    sdr.txAccumBuf[2 * idx + 1] = Q;
    sdr.txAccumCount++;

    /* Ground-truth IQ recording: this is the exact post-scaling, post-
     * downsampling value that enters the outgoing UDP packet payload
     * (before 24-bit LE quantization, but within 24-bit precision). */
    iq_dump_append(I, Q);

    if (sdr.txAccumCount >= SUNSDR_IQ_COMPLEX_PER_PKT) {
        /* Tier 3 — push to pacing ring instead of sending directly.
         *
         * EESDR reference captures show stdev 0.098 ms on inter-packet
         * gaps and NO packets over 8 ms apart. Our emission when done
         * from the WDSP callback shows fdGapMax=16.000 ms on every
         * attempt — the scheduler-tick jitter gets baked directly into
         * our wire cadence. That mismatch is the suspected cause of
         * the radio's DAC hiccups that produce the intermittent
         * raspy.
         *
         * tx_pace_push_packet_locked builds the full wire packet into
         * the next ring slot; a dedicated TIME_CRITICAL pacing thread
         * consumes from the ring at exactly 5.12 ms intervals via a
         * HIGH_RESOLUTION waitable timer, matching EESDR's reference
         * cadence.
         *
         * If the pacing thread isn't running (race at init, or if the
         * timer couldn't be created), tx_pace_push_packet_locked falls
         * back to sunsdr_send_tx_packet for backward compatibility. */
        tx_pace_push_packet_locked(sdr.txAccumBuf);
        sdr.txAccumCount = 0;
    }
}

static void sunsdr_tx_outbound(int id, int nsamples, double* buff)
{
    int i;
    static unsigned int dbg_packets = 0;
    int raw_drive = -1;
    double requested_watts = 100.0;
    double iq_gain = 1.0;
    double pre_peak = 0.0;
    double post_peak = 0.0;
    double pre_sum_sq = 0.0;
    double post_sum_sq = 0.0;
    double pre_rms = 0.0;
    double post_rms = 0.0;

    (void)id;

    if (!sdr.txLockInitialized)
        return;

    EnterCriticalSection(&sdr.txLock);

    if (!sdr.powered || !sdr.currentPTT || !sunsdr_tx_iq_enabled || buff == NULL || nsamples <= 0) {
        if (sdr.powered && sdr.currentPTT && !sunsdr_tx_iq_enabled && buff != NULL && nsamples > 0) {
            InterlockedIncrement(&sunsdr_dbg_tx_iq_gate_skips);
        }
        LeaveCriticalSection(&sdr.txLock);
        return;
    }

    raw_drive = sdr.currentDriveRaw;
    requested_watts = sunsdr_requested_watts_from_raw(raw_drive);
    iq_gain = sunsdr_tx_iq_gain_for_watts(requested_watts);

    /* The previous AM-specific "am_boost = 1.48" wire-IQ multiplier
     * was compensating for a misrouted drive command (we were sending
     * mode code as drive via 0x17, giving AM only ~2.5W equivalent
     * drive). Now that SunSDRSetDrive sends 0x17 with the proper
     * sqrt-scaled drive byte, the radio's RF stage does the
     * amplification and the wire IQ does not need any mode-specific boost.
     *
     * Power calibration reset 2026-04-15: stop stacking drive, full_scale,
     * and correction curves. The old product became non-monotonic between
     * measured points. Use one actuator instead: fixed 0x17 PA-region byte
     * plus a monotonic empirical inverse for effective IQ gain. */

    /*
     * Downsampling resampler: Fin=192 kHz -> Fout=39.0625 kHz (ratio ~4.9152).
     * Boxcar-average every ~4.9 input samples into one output, then emit when
     * phase crosses 1.0. The boxcar provides an anti-aliasing low-pass that
     * suppresses WDSP TX spectral skirts above 19.5 kHz (our output Nyquist).
     * Without this, high-frequency content aliases into the voice band as the
     * characteristic "raspy" distortion heard on-air.
     */
    const double step_out_per_in = 1.0 / SUNSDR_TX_RESAMPLE_STEP;  /* ~0.2035 */

    for (i = 0; i < nsamples; i++) {
        double in_I = buff[2 * i + 0];
        double in_Q = buff[2 * i + 1];
        double cur_I, cur_Q;

        cur_I = in_I * iq_gain;
        cur_Q = in_Q * iq_gain;
        double in_mag = sqrt(in_I * in_I + in_Q * in_Q);
        double out_mag = sqrt(cur_I * cur_I + cur_Q * cur_Q);

        if (in_mag > pre_peak) pre_peak = in_mag;
        if (out_mag > post_peak) post_peak = out_mag;

        pre_sum_sq += (in_I * in_I) + (in_Q * in_Q);
        post_sum_sq += (cur_I * cur_I) + (cur_Q * cur_Q);

        /* Accumulate into the current output window. */
        sdr.txAccumBoxI += cur_I;
        sdr.txAccumBoxQ += cur_Q;
        sdr.txAccumBoxN += 1;

        sdr.txPhase += step_out_per_in;
        if (sdr.txPhase >= 1.0) {
            double out_I = sdr.txAccumBoxI / (double)sdr.txAccumBoxN;
            double out_Q = sdr.txAccumBoxQ / (double)sdr.txAccumBoxN;
            sunsdr_queue_tx_packet_locked(out_I, out_Q);

            sdr.txAccumBoxI = 0.0;
            sdr.txAccumBoxQ = 0.0;
            sdr.txAccumBoxN = 0;
            sdr.txPhase -= 1.0;
        }

        sdr.txPrevI = cur_I;
        sdr.txPrevQ = cur_Q;
    }

    if (nsamples > 0) {
        pre_rms = sqrt(pre_sum_sq / (2.0 * (double)nsamples));
        post_rms = sqrt(post_sum_sq / (2.0 * (double)nsamples));
    }

    {
        ULONGLONG now_tick = GetTickCount64();
        LONG cb_count = InterlockedIncrement(&sunsdr_dbg_tx_cb_count);
        static BOOL tx_cb_priority_set = FALSE;
        LARGE_INTEGER qpc_freq_local;
        LARGE_INTEGER now_qpc;

        /* First TX callback: self-elevate this thread to ABOVE_NORMAL so the
         * WDSP TX channel worker stops losing quanta to UI / background.
         * Run 10 evidence: RX loop histogram shows zero stalls (that thread
         * is already boosted), but fdGapMax=16.000 ms on every TX_ATTEMPT_END
         * means this thread is still being preempted. Addressed here. */
        if (!tx_cb_priority_set) {
            BOOL sp_ok = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            int sp_val = GetThreadPriority(GetCurrentThread());
            sdr_logf("TIMING_INIT: TX callback thread priority set rc=%d readback=%d (ABOVE_NORMAL=1)\n",
                (int)sp_ok, sp_val);
            tx_cb_priority_set = TRUE;
        }

        /* QPC-based per-callback gap bucketed into the TX callback histogram.
         * Emitted once per second from the RX loop's per-second log block. */
        QueryPerformanceCounter(&now_qpc);
        QueryPerformanceFrequency(&qpc_freq_local);
        if (sunsdr_dbg_tx_cb_last_qpc.QuadPart > 0 && qpc_freq_local.QuadPart > 0) {
            double gap_ms_qpc = (double)(now_qpc.QuadPart - sunsdr_dbg_tx_cb_last_qpc.QuadPart)
                * 1000.0 / (double)qpc_freq_local.QuadPart;
            int bi;
            if (gap_ms_qpc < 1.0) bi = 0;
            else if (gap_ms_qpc < 2.0) bi = 1;
            else if (gap_ms_qpc < 4.0) bi = 2;
            else if (gap_ms_qpc < 8.0) bi = 3;
            else if (gap_ms_qpc < 16.0) bi = 4;
            else if (gap_ms_qpc < 32.0) bi = 5;
            else bi = 6;
            InterlockedIncrement(&sunsdr_dbg_tx_cb_gap_hist[bi]);
        }
        sunsdr_dbg_tx_cb_last_qpc = now_qpc;

        if (sunsdr_dbg_tx_cb_last_tick > 0 && now_tick >= sunsdr_dbg_tx_cb_last_tick) {
            ULONGLONG gap_ms = now_tick - sunsdr_dbg_tx_cb_last_tick;
            if (gap_ms > sunsdr_dbg_tx_cb_max_gap_ms) {
                sunsdr_dbg_tx_cb_max_gap_ms = gap_ms;
            }
        }
        sunsdr_dbg_tx_cb_last_tick = now_tick;

        if (!isfinite(pre_peak) || !isfinite(pre_rms) || !isfinite(post_peak) || !isfinite(post_rms)) {
            InterlockedIncrement(&sunsdr_dbg_tx_cb_nonfinite);
        }
        if (pre_peak < 1.0e-6 && pre_rms < 1.0e-6) {
            InterlockedIncrement(&sunsdr_dbg_tx_cb_silent);
        }

        /* Sustained-silence detector for voice MOX (not TUNE — TUNE's tone
         * keeps pre_rms high). If the TX callback receives silence from WDSP
         * for >500 ms during an active MOX attempt, log MOX_AUDIO_SILENCE
         * so we can catch "no audio on air" failures the way the user
         * observed during the Run 11 MOX regression. Threshold 1e-4 ~= -80 dBFS
         * which is well below any normal voice content. Logs at most once
         * per 1 s to avoid spam. */
        {
            static ULONGLONG silence_start_tick = 0;
            static ULONGLONG last_silence_log_tick = 0;
            const double silence_threshold = 1.0e-4;
            const ULONGLONG sustained_ms = 500;
            const ULONGLONG log_cooldown_ms = 1000;

            if (sdr.currentPTT && !sdr.currentTune) {
                if (pre_rms < silence_threshold && pre_peak < silence_threshold) {
                    if (silence_start_tick == 0) {
                        silence_start_tick = now_tick;
                    } else if (now_tick - silence_start_tick >= sustained_ms
                               && now_tick - last_silence_log_tick >= log_cooldown_ms) {
                        sdr_logf("MOX_AUDIO_SILENCE_SUSTAINED attempt=%ld duration_ms=%llu pre_rms=%.6e pre_peak=%.6e (WDSP TX producing silence during MOX)\n",
                            sunsdr_dbg_tx_attempt_id,
                            (unsigned long long)(now_tick - silence_start_tick),
                            pre_rms, pre_peak);
                        last_silence_log_tick = now_tick;
                    }
                } else {
                    silence_start_tick = 0;
                }
            } else {
                silence_start_tick = 0;
            }
        }

        sunsdr_dbg_tx_cb_rms_sum += pre_rms;
        if (cb_count == 1 || pre_rms < sunsdr_dbg_tx_cb_rms_min) {
            sunsdr_dbg_tx_cb_rms_min = pre_rms;
        }
        if (cb_count == 1 || pre_rms > sunsdr_dbg_tx_cb_rms_max) {
            sunsdr_dbg_tx_cb_rms_max = pre_rms;
        }
        if (post_peak > sunsdr_dbg_tx_cb_peak_max) {
            sunsdr_dbg_tx_cb_peak_max = post_peak;
        }
    }

    {
        static unsigned int dbg_input_samples_accum = 0;
        dbg_input_samples_accum += (unsigned int)nsamples;

        if (dbg_packets != sdr.txAudioPackets && (sdr.txAudioPackets <= 5 || sdr.txAudioPackets % 250 == 0)) {
            static ULONGLONG dbg_last_tick = 0;
            static unsigned int dbg_last_audio_pkts = 0;
            static unsigned int dbg_last_input_samples = 0;

            ULONGLONG now_tick = GetTickCount64();
            double in_rate_hz = 0.0;
            double out_rate_hz = 0.0;
            if (dbg_last_tick > 0 && now_tick > dbg_last_tick) {
                double dt_s = (double)(now_tick - dbg_last_tick) / 1000.0;
                in_rate_hz  = (double)(dbg_input_samples_accum - dbg_last_input_samples) / dt_s;
                out_rate_hz = (double)(sdr.txAudioPackets - dbg_last_audio_pkts) * (double)SUNSDR_IQ_COMPLEX_PER_PKT / dt_s;
            }

            dbg_packets = sdr.txAudioPackets;
            sdr_logf("TX audio callback: nsamples=%d, pkts=%u, reqW=%.1f rawDrive=%d iqGain=%.3f pre_peak=%.3f pre_rms=%.3f post_peak=%.3f post_rms=%.3f in_rate=%.0f Hz, out_rate=%.0f Hz, step=%.4f\n",
                nsamples, sdr.txAudioPackets, requested_watts, raw_drive, iq_gain, pre_peak, pre_rms, post_peak, post_rms,
                in_rate_hz, out_rate_hz, SUNSDR_TX_RESAMPLE_STEP);

            dbg_last_tick = now_tick;
            dbg_last_audio_pkts = sdr.txAudioPackets;
            dbg_last_input_samples = dbg_input_samples_accum;
        }
    }

    LeaveCriticalSection(&sdr.txLock);
}

/* Send a raw hex packet to the radio control port and optionally wait for reply */
static int sunsdr_send_ctrl(const unsigned char* pkt, int len)
{
    if (pkt && len >= 3) {
        sdr_logf("CTRL send: label=%s len=%d op=0x%02X sub=0x%02X%02X\n",
            sunsdr_safe_label(sdr_ctrl_trace_label),
            len,
            pkt[2],
            len >= 8 ? pkt[7] : 0,
            len >= 7 ? pkt[6] : 0);
    }

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
        if (n > 0) {
            char hexbuf[513];
            char asciibuf[129];
            sunsdr_hexify(reply, n < 64 ? n : 64, hexbuf, (int)sizeof(hexbuf));
            sunsdr_asciiify(reply, n < 64 ? n : 64, asciibuf, (int)sizeof(asciibuf));
            sdr_logf("CTRL recv: label=%s len=%d op=0x%02X hex=%s ascii=%s\n",
                sunsdr_safe_label(sdr_ctrl_trace_label),
                n,
                n >= 3 ? reply[2] : 0,
                hexbuf,
                asciibuf);
            sunsdr_cache_identity_candidate(reply, n, sunsdr_safe_label(sdr_ctrl_trace_label));
        }
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

static void sunsdr_set_ctrl_trace_label(const char* label)
{
    sdr_ctrl_trace_label = label;
}

static void sunsdr_drain_control_socket(const char* label)
{
    unsigned char reply[1024];
    char hexbuf[513];
    char asciibuf[129];
    int drained = 0;

    for (;;) {
        fd_set fds;
        struct timeval tv;
        int n;

        tv.tv_sec = 0;
        tv.tv_usec = 0;
        FD_ZERO(&fds);
        FD_SET(sdr.ctrlSock, &fds);

        if (select(0, &fds, NULL, NULL, &tv) <= 0) {
            break;
        }

        n = recv(sdr.ctrlSock, (char*)reply, (int)sizeof(reply), 0);
        if (n <= 0) {
            break;
        }

        drained++;
        sunsdr_hexify(reply, n < 64 ? n : 64, hexbuf, (int)sizeof(hexbuf));
        sunsdr_asciiify(reply, n < 64 ? n : 64, asciibuf, (int)sizeof(asciibuf));
        sdr_logf("CTRL drain: label=%s len=%d op=0x%02X hex=%s ascii=%s\n",
            sunsdr_safe_label(label),
            n,
            n >= 3 ? reply[2] : 0,
            hexbuf,
            asciibuf);
    }

    sdr_logf("CTRL drain complete: label=%s packets=%d\n", sunsdr_safe_label(label), drained);
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

    case 6:  /* AM */
        return SUNSDR_MODE_AM;

    case 1:  /* USB */
    case 2:  /* DSB */
    case 4:  /* CWU */
    case 5:  /* FM */
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
static void sunsdr_probe_identity_query(void);
static void sunsdr_capture_control_window(const char* label, DWORD duration_ms);
static void sunsdr_drain_control_socket(const char* label);
static void sunsdr_set_ctrl_trace_label(const char* label);
static void sunsdr_query_firmware_manager_version(void);

static void sunsdr_reassert_tx_state(void)
{
    if (!sdr.powered)
        return;

    if (sdr.currentTxFreqHz > 0) {
        sdr_logf("Reassert TX freq: %d Hz\n", sdr.currentTxFreqHz);
        sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_PRIMARY, 0, sdr.currentTxFreqHz);
    }

    /*
     * 0x17 is DRIVE (not mode). EESDR TUNE-entry order is
     * 0x20 -> 0x17 -> 0x06 -> 0x09; voice-MOX-entry is
     * 0x18 -> 0x20 -> 0x18 -> 0x06 (no 0x17 since drive was set earlier
     * by the slider). We assert drive inside SunSDRSetPTT between the
     * config block and 0x06 (see below) rather than here. Mode is set
     * by the 0x20 config block, not by any dedicated opcode.
     */

    if (sdr.currentTxAntenna == 1 || sdr.currentTxAntenna == 2) {
        int selector = sunsdr_map_tx_ant_selector(sdr.currentTxAntenna);
        sdr_logf("Reassert TX antenna: %d selector=0x%02X\n", sdr.currentTxAntenna, selector);
        sunsdr_send_u32_cmd(SUNSDR_OP_ANT_PREAMBLE, 0);
        sunsdr_send_u32_cmd(SUNSDR_OP_RX_ANT, (unsigned int)selector);
        sunsdr_send_u32_cmd(SUNSDR_OP_KEEPALIVE, 0);
    }

    /* NOTE: 0x24 was added here in a previous commit based on the
     * "pa_on_in_tune" capture, but that capture is "Enable PA while
     * already in TUNE", not a TUNE-entry capture. The real TUNE-entry
     * reference (20260408_141031_tun_on) contains NO 0x24. Removed.
     * The TX freq (0x09) is also moved OUT of this function and sent
     * AFTER 0x06 in the PTT-on path to match EESDR ordering. */
}

static void sunsdr_reassert_rx_state(void)
{
    if (!sdr.powered)
        return;

    if (sdr.currentRx1FreqHz > 0) {
        int primary = sdr.currentRx1FreqHz - SUNSDR_DDC0_OFFSET_HZ;
        sdr_logf("Reassert RX1 freq: rx=%d primary=%d\n", sdr.currentRx1FreqHz, primary);
        sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_PRIMARY, 0, primary);
        sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_COMP, 0, sdr.currentRx1FreqHz);
    }

    if (sdr.currentRx2FreqHz > 0) {
        sdr_logf("Reassert RX2 freq: rx2=%d\n", sdr.currentRx2FreqHz);
        sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_COMP, 1, sdr.currentRx2FreqHz);
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

    sunsdr_set_ctrl_trace_label("macro");

    for (i = 0; power_on_macro[i].hex != NULL; i++) {
        int len = power_on_macro[i].len;
        int j;
        char step_label[32];

        _snprintf(step_label, sizeof(step_label), "macro[%02d]", i + 1);
        step_label[sizeof(step_label) - 1] = '\0';
        sunsdr_set_ctrl_trace_label(step_label);

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

    sunsdr_set_ctrl_trace_label(NULL);

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
    MMRESULT tbp_rc;
    LARGE_INTEGER qpc_freq_check;

    /* Explicitly start the async logger writer thread BEFORE any sdr_logf
     * calls, so we deterministically route through the non-blocking path.
     * This is idempotent — safe to call once per session from the single
     * init path, avoiding any lazy-start race in the lock setup. */
    sdr_log_async_start();

    /* Timer-resolution boost: Windows default scheduler tick is ~15.6 ms,
     * which translated directly into the observed fdGapMax=16.000 ms on every
     * TUNE attempt. timeBeginPeriod(1) drops the system-wide tick to ~1 ms
     * for the lifetime of this process, shrinking GetTickCount* resolution,
     * Sleep granularity, recv timeout granularity, and thread-scheduling
     * granularity by ~16x. Paired with timeEndPeriod(1) in SunSDRDestroy. */
    tbp_rc = timeBeginPeriod(1);
    QueryPerformanceFrequency(&qpc_freq_check);

    sdr_logf("SunSDRInit(%s, %d, %d)\n", radioIP, ctrlPort, streamPort);
    sdr_logf("TIMING_INIT: timeBeginPeriod(1) rc=%u (0=OK) qpcFreq=%lld\n",
        (unsigned)tbp_rc, (long long)qpc_freq_check.QuadPart);

    /* Process priority: ABOVE_NORMAL scheduled over ordinary UI apps and
     * background services without the UI-jank risk of HIGH_PRIORITY_CLASS.
     * Reduces the odds of a preemption event landing inside the RX read
     * loop or TX callback and producing the 16 ms scheduler-tick gap that
     * still shows up in Run 9b as the suspected raspy driver for 5/20 of
     * TUNE attempts. */
    {
        BOOL pc_ok = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
        DWORD pc_readback = GetPriorityClass(GetCurrentProcess());
        sdr_logf("TIMING_INIT: SetPriorityClass(ABOVE_NORMAL) rc=%d readback=0x%lX (ABOVE_NORMAL=0x%lX)\n",
            (int)pc_ok, (unsigned long)pc_readback, (unsigned long)ABOVE_NORMAL_PRIORITY_CLASS);
    }

    /* Power request: PowerRequestExecutionRequired keeps CPU out of deep
     * C-states and blocks DVFS from downclocking during light UI periods
     * between TX bursts. Handle held for session lifetime; released in
     * SunSDRDestroy. Kernel32 API (Windows 7+). */
    if (!sdr_power_request) {
        REASON_CONTEXT rc = {0};
        rc.Version = POWER_REQUEST_CONTEXT_VERSION;
        rc.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        rc.Reason.SimpleReasonString = L"Thetis SunSDR low-latency TX/RX";
        sdr_power_request = PowerCreateRequest(&rc);
        if (sdr_power_request) {
            BOOL pr_ok = PowerSetRequest(sdr_power_request, PowerRequestExecutionRequired);
            sdr_logf("TIMING_INIT: PowerCreateRequest OK, PowerSetRequest(ExecutionRequired) rc=%d\n",
                (int)pr_ok);
        } else {
            sdr_logf("TIMING_INIT: PowerCreateRequest FAILED err=%lu\n",
                (unsigned long)GetLastError());
        }
    }
    if (sdr.ctrlSock || sdr.streamSock || sdr.hReadThread || sdr.hKeepaliveThread || sdr.rxBuf) {
        sdr_logf("SunSDRInit() cleaning up previous session state before re-init\n");
        SunSDRDestroy();
    }
    memset(&sdr, 0, sizeof(sdr));
    sdr.ctrlSock = INVALID_SOCKET;
    sdr.streamSock = INVALID_SOCKET;
    sdr.currentMode = -1;
    sdr.ctrlPort = ctrlPort;
    sdr.streamPort = streamPort;
    sdr.currentRxAntenna = 1;
    sdr.currentTxAntenna = 1;
    sunsdr_set_identity_defaults();
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

    /* Stop the TX pacing thread BEFORE closing the stream socket, so
     * the pacing loop's final sendto can't hit an already-closed handle. */
    tx_pace_stop_and_join();

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

    /* Release the 1 ms timer resolution we requested in SunSDRInit. Paired
     * with timeBeginPeriod(1). Do this before stopping the async logger so
     * the release still benefits from the boosted resolution during log
     * drain and thread joins. */
    timeEndPeriod(1);
    sdr_logf("TIMING_DEINIT: timeEndPeriod(1) called\n");

    /* Release the ExecutionRequired power request so the CPU can return to
     * normal power management once Thetis is idle. Process priority class
     * (ABOVE_NORMAL) doesn't need explicit cleanup — it dies with the process. */
    if (sdr_power_request) {
        PowerClearRequest(sdr_power_request, PowerRequestExecutionRequired);
        CloseHandle(sdr_power_request);
        sdr_power_request = NULL;
        sdr_logf("TIMING_DEINIT: PowerClearRequest + handle closed\n");
    }

    /* Stop the async logger writer thread and flush any pending entries
     * BEFORE closing the file handle. */
    sdr_logf("SunSDRDestroy: stopping async log writer\n");
    sdr_log_async_stop();

    if (sdr_log) {
        fclose(sdr_log);
        sdr_log = NULL;
    }

    SendpOutboundTx(OutBound);
    sunsdr_set_identity_defaults();

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
    InterlockedExchange(&sunsdr_tx_iq_enabled, 0);
    sdr.currentRx1FreqHz = 0;
    sdr.currentRx2FreqHz = 0;
    sdr.currentRX2Enabled = 0;
    sdr.currentTune = 0;
    sdr.currentMode = -1;
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

    /* Replay the existing info query and drain follow-on control replies. */
    sunsdr_query_firmware_manager_version();
    sunsdr_probe_identity_query();
    sunsdr_capture_control_window("post_probe", 2000);

    /* Start IQ read thread */
    sdr.hReadThread = CreateThread(NULL, 0, SunSDRReadThread, NULL, 0, NULL);

    /* Bump the RX read thread above NORMAL so it wins CPU against UI and
     * other background work. Not TIME_CRITICAL — that risks UI starvation.
     * ABOVE_NORMAL is the standard for latency-sensitive audio. */
    if (sdr.hReadThread) {
        BOOL sp_ok = SetThreadPriority(sdr.hReadThread, THREAD_PRIORITY_ABOVE_NORMAL);
        int sp_val = GetThreadPriority(sdr.hReadThread);
        sdr_logf("TIMING_INIT: RX thread priority set rc=%d readback=%d (ABOVE_NORMAL=1)\n",
            (int)sp_ok, sp_val);
    }

    /* Clean up leftover IQ dump files from previous sessions so the
     * bin/Debug dir doesn't accumulate .raw files across runs. */
    iq_dump_cleanup_old_files();

    /* Start the Tier 3 TX pacing thread + high-resolution waitable timer.
     * Decouples wire emission cadence from the WDSP TX callback's
     * scheduling jitter so we match EESDR's sub-ms inter-packet stdev.
     * See tx_pace_thread_proc for the loop. Safe to start here because
     * sdr.streamSock is open and sdr.currentPTT is 0 at this point, so
     * the pacing thread will tick but emit nothing until first TX. */
    tx_pace_start();

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
    InterlockedExchange(&sunsdr_tx_iq_enabled, 0);
    sdr.currentTune = 0;
    sdr.currentMode = -1;
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
    sdr_logf("SunSDRSetFreq(receiver=%d, freq=%d, isTx=%d)\n", receiver, freqHz, isTx);
    if (isTx) {
        /*
         * TX frequency: only VFO primary, no DDC companions. While still in RX,
         * cache the TX VFO but do not send primary 0x09; that context is also
         * the RX analog LO and split/VFOB bookkeeping can otherwise retune RX.
         */
        if (sdr.currentPTT || sdr.currentTune) {
            sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_PRIMARY, 0, freqHz);
        } else {
            sdr_logf("Cache TX freq while RX: tx=%d\n", freqHz);
        }
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
            if (sdr.currentPTT || sdr.currentTune) {
                /*
                 * During TX/TUNE the primary 0x09 context is the TX LO. Sending
                 * the RX center offset here retunes the RF output by -92.5 kHz.
                 * Keep COMP[0] current, but leave primary under TX ownership until RX.
                 */
                sdr_logf("Suppress RX primary during TX/TUNE: rx=%d primary=%d ptt=%d tune=%d\n",
                    freqHz, freqHz - SUNSDR_DDC0_OFFSET_HZ, sdr.currentPTT, sdr.currentTune);
            } else {
                sunsdr_send_freq_pkt(SUNSDR_OP_FREQ_PRIMARY, 0, freqHz - SUNSDR_DDC0_OFFSET_HZ);
            }
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
    /* 0x17 is NOT mode — it's drive (see sunsdr.h). Previous code
     * sent 0x17 with the mode code, which the radio interpreted as
     * drive byte. Side effects observed:
     *   - LSB mode 0xBC=188 -> accidental drive "54W" (SSB appeared to work)
     *   - USB mode 0xF5=245 -> accidental drive "92W" (SSB full power)
     *   - AM  mode 0x28=40  -> accidental drive "2.5W" (AM stuck at ~1.5-2W)
     * Mode is set by the radio via the 0x20 config block (verified in
     * EESDR mode-change captures 20260413_125056 USB->AM / 125127 AM->USB:
     * only 0x18/0x20/0x18/0x17/0x18 is sent; the 0x17 byte varies with the
     * CURRENT drive slider, not the mode). We keep the cached currentMode
     * for our own gating (AM-specific wire IQ handling) but do not
     * transmit a wire mode command here. */
    int sunsdr_mode = sunsdr_map_mode_code(mode);
    sdr_logf("SunSDRSetMode(thetis=%d -> sunsdr=0x%02X) [no wire send; 0x17 is drive]\n", mode, sunsdr_mode);
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
    int old_tune = sdr.currentTune;
    sdr.currentTune = tune ? 1 : 0;
    sdr_logf("SunSDRSetTune(%d) old=%d ptt=%d attempt=%ld seq=%u txPackets=%u txPhase=%.4f\n",
        sdr.currentTune,
        old_tune,
        sdr.currentPTT,
        sunsdr_dbg_tx_attempt_id,
        sdr.txSeq,
        sdr.txAudioPackets,
        sdr.txPhase);
}

/* Simple passthrough so managed-side code (console.cs) can write one-liner
 * trace events to the native sunsdr_debug.log. Added so we can tag
 * TX_ATTEMPT triggers with their user-click source vs any programmatic
 * source — user reported 12 recorded attempts when only 10 were
 * consciously initiated, and we need provenance on the mismatch. */
void SunSDRLogTrace(const char* msg)
{
    if (!msg) return;
    sdr_logf("MANAGED_TRACE %s\n", msg);
}

void SunSDRLogTuneState(const char* label, int chk_tun, int chk_mox, int tuning, int mox,
    int tx_dsp_mode, int current_dsp_mode, int postgen_run, int postgen_mode,
    double tone_freq, double tone_mag, int pulse_enabled, int pulse_on,
    int tune_drive_source, int pwr, int new_pwr)
{
    sdr_logf("TUNE_AUDIO_STATE label=%s chkTun=%d chkMox=%d tuning=%d mox=%d txDspMode=%d currentDspMode=%d postGenRun=%d postGenMode=%d toneFreq=%.3f toneMag=%.6f pulseEnabled=%d pulseOn=%d tuneDriveSource=%d pwr=%d newPwr=%d attempt=%ld ptt=%d tune=%d seq=%u txPackets=%u txPhase=%.4f rawDrive=%d\n",
        label ? label : "unknown",
        chk_tun,
        chk_mox,
        tuning,
        mox,
        tx_dsp_mode,
        current_dsp_mode,
        postgen_run,
        postgen_mode,
        tone_freq,
        tone_mag,
        pulse_enabled,
        pulse_on,
        tune_drive_source,
        pwr,
        new_pwr,
        sunsdr_dbg_tx_attempt_id,
        sdr.currentPTT,
        sdr.currentTune,
        sdr.txSeq,
        sdr.txAudioPackets,
        sdr.txPhase,
        sdr.currentDriveRaw);
}

/*
 * Drive byte = raw (pass-through).
 *
 * Thetis already owns power calibration upstream of this function:
 *   console.cs SetTXPwrAndTXFreq computes target_dbm from UI watts,
 *   subtracts per-band PA gain via SetupForm.GetPAGain (which for
 *   SUNSDR2DX layers in GetSunSDRDefaultAdjust offsets), converts
 *   to a voltage fraction and feeds it through NetworkIO.SetOutputPower
 *   -> nativeSunSDRSetDrive(int i) where i = 255 * f * swr_protect.
 *
 * That integer IS the calibrated drive byte the radio should see. Our
 * job is to forward it. Every previous LUT here was DOUBLE-compensating
 * the Thetis curve, which made the end-to-end response non-monotonic
 * and reagent to session/state drift.
 *
 * Calibration from here is an operator workflow, not a code workflow:
 *   Setup -> PA Settings -> PA Gain tab -> Offsets for <band> -> Drive
 *   edit per-drive-level dB offsets until the curve is linear on that
 *   band, then do the next band.
 */
static int sunsdr_drive_raw_to_wire_byte(int raw)
{
    if (raw < 0) raw = 0;
    if (raw > 255) raw = 255;
    return raw;
}

void SunSDRSetDrive(int raw)
{
    int wire_byte;
    if (raw < 0) raw = 0;
    if (raw > 255) raw = 255;
    sdr.currentDriveRaw = raw;
    wire_byte = sunsdr_drive_raw_to_wire_byte(raw);
    sdr_logf("SunSDRSetDrive(raw=%d ui_watts=%.1f wire_byte=0x%02X)\n",
        raw, (double)raw * 100.0 / 255.0, wire_byte);
    if (sdr.powered) {
        sunsdr_send_u32_cmd(SUNSDR_OP_DRIVE, (unsigned int)wire_byte);
    }
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
    int attempt_id = 0;
    int raw_drive = sdr.currentDriveRaw;
    double requested_watts = sunsdr_requested_watts_from_raw(raw_drive);
    double iq_gain = sunsdr_tx_iq_gain_for_watts(requested_watts);

    if (!sdr.powered)
    {
        sdr.currentPTT = new_ptt;
        InterlockedExchange(&sunsdr_tx_iq_enabled, 0);
        return;
    }

    if (sdr.currentPTT == new_ptt)
        return;

    if (sdr.txLockInitialized) {
        EnterCriticalSection(&sdr.txLock);
        if (new_ptt) {
            InterlockedExchange(&sunsdr_tx_iq_enabled, 0);
            sdr.txAudioPackets = 0;
            sdr.txPhase = 0.0;
            sdr.txPrevI = 0.0;
            sdr.txPrevQ = 0.0;
            sdr.txAccumCount = 0;
            sdr.txAccumBoxI = 0.0;
            sdr.txAccumBoxQ = 0.0;
            sdr.txAccumBoxN = 0;
            /* Reset the wire packet sequence counter to 0. EESDR reference
             * captures (voice MOX and TUNE) show the TX stream seq restarts
             * at 0 for every new TX session. Without this we were handing
             * the radio FD packets with carried-over seq (e.g., 18507 on
             * attempt 9) and the radio intermittently discarded them as
             * out-of-order, keying up but producing unmodulated carrier
             * (user-visible as "radio in TX but no tone"). Resetting seq
             * here ensures each session starts seq=0 matching EESDR. */
            sdr.txSeq = 0;
            attempt_id = InterlockedIncrement(&sunsdr_dbg_attempt_seq);
            InterlockedExchange(&sunsdr_dbg_tx_attempt_id, attempt_id);
            sunsdr_dbg_reset_tx_attempt_locked(attempt_id);
            sunsdr_dbg_ptt_request_tick = GetTickCount64();
        }
        LeaveCriticalSection(&sdr.txLock);
    }

    /* Phase C — WDSP TX DSP flush on every PTT-on entry.
     *
     * Run 11 evidence showed TUNE raspy at 5 % (1/20), with the remaining
     * event a cold-start artifact (first-TUNE-after-settle) — TX callback
     * thread was no longer being preempted (TX_CB_GAP_HIST buckets 8-32 all
     * empty), meaning scheduler jitter is no longer the driver. The residual
     * raspy is therefore WDSP-internal state carrying between TX attempts
     * (filter rings, WCPAGC hang_backaverage, rmatch partial state, etc.).
     *
     * FlushChannelNow (added to wdsp/channel.c) synchronously zeroes iobuffs
     * and calls flush_main -> flush_txa for the TX channel, mirroring the
     * same work the flushChannel worker does — but NOT relying on the
     * Sem_Flush semaphore path that the timeout-protected SetChannelState
     * flush-on-state-0 code path uses (which may silently skip the flush on
     * 100 ms timeout).
     *
     * Called only on PTT-on. State/slew/flags are left to existing
     * SetChannelState calls in console.cs. Tx channel id is chid(inid(1,0),0)
     * matching console.cs's WDSP.id(1, 0). */
    if (new_ptt) {
        int tx_chan = chid(inid(1, 0), 0);
        __try {
            FlushChannelNow(tx_chan);
            sdr_logf("TX_FLUSH_DONE attempt=%d channel=%d (Phase C: iobuffs + TXA DSP chain flushed on PTT-on)\n",
                attempt_id, tx_chan);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sdr_logf("TX_FLUSH_FAIL attempt=%d channel=%d exception=0x%08X\n",
                attempt_id, tx_chan, GetExceptionCode());
        }
        /* Arm the ground-truth IQ recorder for this attempt. Append happens
         * inside sunsdr_queue_tx_packet_locked; flush to disk happens on
         * PTT-off below. */
        iq_dump_reset(attempt_id);
        /* NOTE: we do NOT signal tx_pace_ptt_event here — sdr.currentPTT
         * is still 0 at this point in SunSDRSetPTT (it's set near the end
         * of this function after all protocol handshakes). Signalling now
         * would wake the pacing thread, which would see currentPTT=0,
         * exit its inner loop, consume the auto-reset event, and sleep
         * forever. We signal at the end of SunSDRSetPTT instead. */
    } else {
        /* Flush the accumulated IQ buffer to sunsdr_tx_iq_<attempt>.raw
         * (little-endian double I/Q pairs). Post-test analysis: compare
         * clean-attempt vs raspy-attempt files spectrally (FFT on a 100 ms
         * window) to identify whether the distortion signature is harmonic
         * (ALC/compressor), wideband noise (FIR ring), AM (AAMIX slew),
         * or phase jitter (rmatch / sample timing). */
        iq_dump_flush_to_disk();
    }

    sdr_logf("SunSDRSetPTT(%d) currentTune=%d rx1=%d rx2=%d tx=%d rxAnt=%d txAnt=%d pa=%d seq=%u txPhase=%.4f txAccum=%d reqW=%.1f rawDrive=%d iqGain=%.3f attempt=%ld\n",
        new_ptt,
        sdr.currentTune,
        sdr.currentRx1FreqHz,
        sdr.currentRx2FreqHz,
        sdr.currentTxFreqHz,
        sdr.currentRxAntenna,
        sdr.currentTxAntenna,
        sdr.currentPAEnabled,
        sdr.txSeq,
        sdr.txPhase,
        sdr.txAccumCount,
        requested_watts,
        raw_drive,
        iq_gain,
        sunsdr_dbg_tx_attempt_id);

    if (new_ptt) {
        sdr_logf("TX_ATTEMPT_BEGIN #%ld mode=%s ptt=%d tune=%d seq=%u txPhase=%.4f txAccum=%d reqW=%.1f rawDrive=%d iqGain=%.3f\n",
            sunsdr_dbg_tx_attempt_id,
            sunsdr_tx_mode_label(sdr.currentTune),
            new_ptt,
            sdr.currentTune,
            sdr.txSeq,
            sdr.txPhase,
            sdr.txAccumCount,
            requested_watts,
            raw_drive,
            iq_gain);
        sunsdr_dbg_log_audio_diags("BEGIN");
        /*
         * TUNE and MOX follow the same wire path: re-assert current mode
         * (LSB/USB/AM/etc), PTT on, let the WDSP-generated IQ stream carry
         * the tune tone at full drive. Do NOT switch the radio to internal
         * tune mode 0x45 — that path generates a low-power internal carrier
         * and ignores our IQ stream, capping output at ~2.5W.
         *
         * Order is critical: update sdr.currentPTT BEFORE sending 0x06=1 so
         * the keepalive thread sees TX state immediately and stops sending
         * 0xFE silence packets. Keep a separate TX IQ gate closed until after
         * 0x06=1 completes so the audio callback cannot send active 0xFD IQ
         * before the radio has been commanded into TX.
         */
        sdr.lastTxWasTune = sdr.currentTune ? 1 : 0;
        sdr.currentPTT = new_ptt;
        sunsdr_reassert_tx_state();
        sunsdr_send_config_block_state(0);
        /* Assert drive (0x17) after the 0x20 config block and before 0x06,
         * matching EESDR TUNE-entry order. Using the cached slider value;
         * if raw_drive < 0 the user hasn't moved the slider since boot,
         * skip the send and let the prior radio state stand. */
        if (sdr.currentDriveRaw >= 0) {
            int wire_byte = sunsdr_drive_raw_to_wire_byte(sdr.currentDriveRaw);
            sdr_logf("TX_DRIVE_ASSERT raw=%d wire_byte=0x%02X\n",
                sdr.currentDriveRaw, wire_byte);
            sunsdr_send_u32_cmd(SUNSDR_OP_DRIVE, (unsigned int)wire_byte);
        }
        sunsdr_send_u32_cmd(SUNSDR_OP_MOX_PTT, 1);
        sunsdr_dbg_mox_cmd_tick = GetTickCount64();
        sdr_logf("TX_ATTEMPT_0x06_SENT #%ld mode=%s seq=%u txPackets=%u delay_from_begin_ms=%llu\n",
            sunsdr_dbg_tx_attempt_id,
            sunsdr_tx_mode_label(sdr.lastTxWasTune),
            sdr.txSeq,
            sdr.txAudioPackets,
            sunsdr_dbg_ptt_request_tick ? (unsigned long long)(sunsdr_dbg_mox_cmd_tick - sunsdr_dbg_ptt_request_tick) : 0);
        InterlockedExchange(&sunsdr_tx_iq_enabled, 1);
        sdr_logf("TX_IQ_GATE_OPEN #%ld txPackets=%u gateSkips=%ld\n",
            sunsdr_dbg_tx_attempt_id,
            sdr.txAudioPackets,
            sunsdr_dbg_tx_iq_gate_skips);
        /*
         * Only write 0x24 PA_ENABLE on MOX-on when we're actually turning
         * PA ON (user has external PA toggled on). Writing 0x24=0 right
         * after PTT=1 appears to kill AM TX output at the radio for users
         * who don't have the PA enabled. When PA=0 (off), skip the 0x24
         * write entirely — the radio will already be in its default
         * PA-off state from power-on init.
         */
        if (sunsdr_current_pa_wire_state() != 0) {
            sunsdr_send_u32_cmd(SUNSDR_OP_PA_ENABLE, 1);
        }
    } else {
        double avg_fd_gap = sunsdr_dbg_fd_gap_count > 0 ? sunsdr_dbg_fd_gap_sum_ms / (double)sunsdr_dbg_fd_gap_count : 0.0;
        double avg_cb_rms = sunsdr_dbg_tx_cb_count > 0 ? sunsdr_dbg_tx_cb_rms_sum / (double)sunsdr_dbg_tx_cb_count : 0.0;
        unsigned long long first_fd_delay = (sunsdr_dbg_first_fd_tick && sunsdr_dbg_mox_cmd_tick && sunsdr_dbg_first_fd_tick >= sunsdr_dbg_mox_cmd_tick) ?
            (unsigned long long)(sunsdr_dbg_first_fd_tick - sunsdr_dbg_mox_cmd_tick) : 0;
        int first_fd_before_cmd = (int)sunsdr_dbg_first_fd_before_cmd;
        InterlockedExchange(&sunsdr_tx_iq_enabled, 0);
        sdr_logf("TX_ATTEMPT_END #%ld mode=%s result=user_clean_or_raspy ptt=%d tune=%d seq=%u txPackets=%u activeFD=%ld seqGaps=%ld feDuringTx=%ld keepaliveRaces=%ld iqGateSkips=%ld firstFdDelayMs=%llu firstFdBefore0x06=%d fdGapMin=%.3f fdGapAvg=%.3f fdGapMax=%.3f fdGapCount=%u txPhase=%.4f txAccum=%d txCb=%ld txCbSilent=%ld txCbNonfinite=%ld txCbRmsMin=%.6f txCbRmsAvg=%.6f txCbRmsMax=%.6f txCbPostPeakMax=%.6f txCbMaxGapMs=%llu\n",
            sunsdr_dbg_tx_attempt_id,
            sunsdr_tx_mode_label(sdr.lastTxWasTune),
            new_ptt,
            sdr.currentTune,
            sdr.txSeq,
            sdr.txAudioPackets,
            sunsdr_dbg_active_tx_packets,
            sunsdr_dbg_active_tx_seq_gaps,
            sunsdr_dbg_idle_fe_during_tx,
            sunsdr_dbg_keepalive_tx_races,
            sunsdr_dbg_tx_iq_gate_skips,
            first_fd_delay,
            first_fd_before_cmd,
            sunsdr_dbg_fd_gap_min_ms,
            avg_fd_gap,
            sunsdr_dbg_fd_gap_max_ms,
            sunsdr_dbg_fd_gap_count,
            sdr.txPhase,
            sdr.txAccumCount,
            sunsdr_dbg_tx_cb_count,
            sunsdr_dbg_tx_cb_silent,
            sunsdr_dbg_tx_cb_nonfinite,
            sunsdr_dbg_tx_cb_rms_min,
            avg_cb_rms,
            sunsdr_dbg_tx_cb_rms_max,
            sunsdr_dbg_tx_cb_peak_max,
            (unsigned long long)sunsdr_dbg_tx_cb_max_gap_ms);
        sunsdr_dbg_log_audio_diags("END");
        /*
         * On MOX-off, only write 0x24 PA_ENABLE=0 if we had written a 1
         * on MOX-on (i.e. the user has the external PA enabled). Otherwise
         * we never toggled PA on, no reason to toggle it off.
         */
        if (sdr.currentPAEnabled) {
            sunsdr_send_u32_cmd(SUNSDR_OP_PA_ENABLE, 0);
        }
        sunsdr_send_u32_cmd(SUNSDR_OP_MOX_PTT, 0);
        sunsdr_send_config_block_state(1);
        sunsdr_reassert_rx_state();
        sdr.currentPTT = new_ptt;
        sdr.lastTxWasTune = 0;
        sdr.pendingTuneReleaseConfig = 0;
    }

    sdr.currentPTT = new_ptt;

    /* Tier 3 pacing thread wake-up. Must happen AFTER sdr.currentPTT is
     * set to 1 — the thread checks currentPTT in its inner loop and would
     * exit immediately if we signalled while currentPTT was still 0. */
    if (new_ptt && tx_pace_ptt_event) {
        SetEvent(tx_pace_ptt_event);
    }
}

/* ========== IQ Receive Thread ========== */

/*
 * Build a silent IQ packet (1210 bytes, all-zero IQ payload).
 * Uses the opcode/flags appropriate for current TX state:
 *   RX/idle (currentPTT=0): op=0xFE, byte8=0x01, byte9=0x00  (matches ExpertSDR3 idle)
 *   TX-active  (currentPTT=1): op=0xFD, byte8=0x02, byte9=0x01 (matches ExpertSDR3 MOX)
 */
static void sunsdr_build_tx_silence(unsigned char* buf, unsigned int seq)
{
    if (sdr.currentPTT) {
        sunsdr_build_iq_header(buf, SUNSDR_OP_IQ_TX_ACTIVE, seq, 0x02, 0x01);
    } else {
        sunsdr_build_iq_header(buf, SUNSDR_OP_IQ_RX_IDLE, seq, 0x01, 0x00);
    }
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
    /* QPC-based pacing: sub-us resolution, immune to Windows scheduler-tick
     * granularity. Replaces GetTickCount64() which had ~16 ms resolution and
     * directly caused the observed fdGapMax=16.000 ms on every TUNE (Run 8).
     *
     * Clamp rationale: the loop is paced by radio packet arrival via recv().
     * In RX mode packets arrive every ~640 us. In TX/TUNE the radio throttles
     * to ~195 pkts/sec (~5.12 ms/iter) — this is NORMAL, not a stall. Clamp
     * must therefore only fire on PATHOLOGICAL stalls (scheduler preemption,
     * page fault, AV scan). 50 ms catches those while staying comfortably
     * above the 5.12 ms TX cadence and well under a second of catastrophe.
     * Per-iteration burst is separately bounded by `emitted < 16` below.
     *
     * When the clamp fires, elapsed_ms is replaced with the CURRENT
     * iteration's QPC delta (best estimate, NOT a fixed 1 ms) so the
     * silence accumulators keep tracking real cadence. */
    LARGE_INTEGER qpc_freq = {0};
    LARGE_INTEGER last_service_qpc = {0};
    LARGE_INTEGER now_qpc = {0};
    double elapsed_clamp_ms = 50.0;
    double elapsed_replace_ms = 10.0;  /* sentinel value when clamping fires */
    unsigned int dbg_elapsed_clamps = 0;
    unsigned int dbg_elapsed_clamps_prev_sec = 0;
    /* RX-loop iteration gap histogram. Buckets (ms):
     *   [0] <1, [1] 1-2, [2] 2-4, [3] 4-8, [4] 8-16, [5] 16-32, [6] >=32
     * Normal RX: almost all in [0]. Normal TX: mostly in [3] (5.12 ms).
     * Scheduler stalls show up in [4]/[5]. True pathology in [6]. */
    #define SDR_GAP_BUCKETS 7
    unsigned int dbg_gap_hist[SDR_GAP_BUCKETS] = {0};
    unsigned int dbg_gap_hist_prev[SDR_GAP_BUCKETS] = {0};
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
    double rx_silence_accum = 0.0;
    int prev_ptt_state = 0;
    ULONGLONG last_rx_pkt_tick = 0;
    unsigned int dbg_tx_feed_buffers = 0;
    unsigned int dbg_keepalive_fe_sent = 0;
    unsigned int dbg_keepalive_tx_races = 0;
    unsigned int dbg_rx_silence_injected = 0;
    unsigned int dbg_real_fe_packets = 0;
    unsigned int dbg_real_fd_packets = 0;
    unsigned int dbg_xrouter_real_feeds = 0;
    unsigned int dbg_xrouter_total_feeds = 0;

    sdr_logf("IQ read thread started\n");
    sdr_logf("TX pipeline keepalive: stream=%d, buf_size=%d, feed_rate=%d/sec, step=%.4f\n",
        tx_stream_id, tx_buf_size, (int)tx_feed_rate, tx_feed_rate / 1000.0);
    printf("SunSDR: IQ read thread started\n");

    /* Set stream socket timeout so we can check keepRunning */
    int timeout_ms = 500;
    setsockopt(sdr.streamSock, SOL_SOCKET, SO_RCVTIMEO,
        (char*)&timeout_ms, sizeof(timeout_ms));

    DWORD last_log_time = GetTickCount();
    QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&last_service_qpc);
    sdr_logf("TIMING_INIT: RX loop QPC pacing qpcFreq=%lld clampMs=%.1f replaceMs=%.1f\n",
        (long long)qpc_freq.QuadPart, elapsed_clamp_ms, elapsed_replace_ms);
    int timeout_count = 0;

    while (sdr.keepRunning) {
        double elapsed_ms;
        QueryPerformanceCounter(&now_qpc);
        if (qpc_freq.QuadPart > 0) {
            elapsed_ms = (double)(now_qpc.QuadPart - last_service_qpc.QuadPart)
                * 1000.0 / (double)qpc_freq.QuadPart;
        } else {
            elapsed_ms = 0.0;
        }

        /* Bucket the pre-clamp gap so the histogram reflects ground-truth
         * scheduler behavior, not our clamped view of it. */
        {
            unsigned int b;
            if (elapsed_ms < 1.0) b = 0;
            else if (elapsed_ms < 2.0) b = 1;
            else if (elapsed_ms < 4.0) b = 2;
            else if (elapsed_ms < 8.0) b = 3;
            else if (elapsed_ms < 16.0) b = 4;
            else if (elapsed_ms < 32.0) b = 5;
            else b = 6;
            dbg_gap_hist[b]++;
        }

        /* Clamp only PATHOLOGICAL stalls (>50 ms). Normal TX-mode iteration
         * is ~5.12 ms (radio throttled to 195 pps). RX-mode iteration is
         * ~0.64 ms. The per-iteration burst cap inside the silence-injection
         * while-loop (`emitted < 16`) already bounds how many silence buffers
         * one iteration can emit — so an ordinary scheduler hiccup spreads
         * its catch-up across a few iterations rather than hitting VAC in
         * one shot.
         *
         * Earlier 2 ms clamp regressed Run 9 badly: it fired EVERY TX iter,
         * under-fed the accumulators by ~5x, and produced steady-state
         * VAC1 Out underflow + VAC1 In overflow with 100% raspy TUNE. */
        if (elapsed_ms > elapsed_clamp_ms) {
            dbg_elapsed_clamps++;
            elapsed_ms = elapsed_replace_ms;
        }

        if (elapsed_ms > 0.0) {
                tx_feed_accum += elapsed_ms * tx_feed_rate / 1000.0;
                while (tx_feed_accum >= 1.0) {
                    Inbound(tx_stream_id, tx_buf_size, tx_silence_buf);
                    dbg_tx_feed_buffers++;
                    tx_feed_accum -= 1.0;
                }

            tx_keepalive_accum += elapsed_ms * tx_keepalive_rate / 1000.0;
            last_service_qpc = now_qpc;
        }

        /* RX silence padding: if no real RX packet has arrived for >2ms (would
         * normally arrive every ~640us at 1562/sec), inject silence to keep WDSP
         * RX input rate at expected 384k samples/sec. This prevents fexchange0
         * from blocking on Sem_OutReady when SunSDR throttles the IQ stream
         * during TX (radio drops from 1562/sec to ~195/sec during MOX/TUNE). */
        /*
         * RX silence injection during TX: inject silence BUFFERS at the
         * expected steady rate (1562.5/sec), not in bursts.
         *
         * The original burst-mode implementation injected up to 32 buffers at
         * once when the gap exceeded 2ms, producing a sawtooth input pattern
         * into WDSP (long quiet periods followed by rapid-fire buffers). That
         * jitter propagated through WDSP RX -> VAC mixer -> rmatch resampler
         * -> PortAudio, and the cumulative phase drift in the rmatch
         * resampler caused post-TX RX audio to sound raspy until the VAC
         * was manually cycled.
         *
         * Steady-rate injection uses a phase accumulator (same technique as
         * the TX keepalive accumulator above) to emit one silence buffer per
         * 640us tick, smoothly. Reset the accumulator on PTT transitions
         * so we don't carry cross-session drift.
         */
        if (sdr.currentPTT != prev_ptt_state) {
            rx_silence_accum = 0.0;
            prev_ptt_state = sdr.currentPTT;
        }
        if (sdr.currentPTT) {
            if (elapsed_ms > 0.0) {
                rx_silence_accum += elapsed_ms * rx_feed_rate_target / 1000.0;
            }
            /* The read loop is paced by incoming radio packets. During TX the
             * radio drops to the TX IQ rate (~195/sec), so the cap must allow
             * roughly 1562.5 / 195.3 ~= 8 silence buffers per iteration or the
             * RX/VAC path is underfed. Keep a bounded cap to avoid a large burst
             * after a long scheduler delay, but leave headroom for jitter. */
            int emitted = 0;
            while (rx_silence_accum >= 1.0 && emitted < 16) {
                __try {
                    xrouter(NULL, 0, 0, rx_resample_outsize, rx_silence_buf);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
                rx_silence_accum -= 1.0;
                dbg_rx_silence_injected++;
                dbg_xrouter_total_feeds++;
                emitted++;
            }
        }

        while (tx_keepalive_accum >= 1.0) {
            /*
             * Only send idle keepalive silence packets during RX.
             * During active TX the audio callback (sunsdr_tx_outbound) is the
             * sole source of packets and emits at the exact 195.3 pkts/sec
             * expected rate. Any silence injected here would interleave with
             * voice IQ and produce audible raspiness / crackling on-air.
             */
            if (!sdr.currentPTT) {
                struct sockaddr_in streamDest;
                int ptt_before_send;
                memcpy(&streamDest, &sdr.radioAddr, sizeof(streamDest));
                streamDest.sin_port = htons((u_short)sdr.streamPort);
                ptt_before_send = sdr.currentPTT;
                sunsdr_build_tx_silence(txbuf, sdr.txSeq++);
                if (ptt_before_send || sdr.currentPTT || txbuf[2] != SUNSDR_OP_IQ_RX_IDLE) {
                    LONG fe_during_tx = sunsdr_dbg_idle_fe_during_tx;
                    LONG keepalive_races = InterlockedIncrement(&sunsdr_dbg_keepalive_tx_races);
                    dbg_keepalive_tx_races++;
                    if ((ptt_before_send || sdr.currentPTT) && txbuf[2] == SUNSDR_OP_IQ_RX_IDLE) {
                        fe_during_tx = InterlockedIncrement(&sunsdr_dbg_idle_fe_during_tx);
                    }
                    sdr_logf("TX_KEEPALIVE_RACE attempt=%ld ptt_before=%d ptt_after=%d op=0x%02X seq=%u feDuringTx=%ld keepaliveRaces=%ld\n",
                        sunsdr_dbg_tx_attempt_id,
                        ptt_before_send,
                        sdr.currentPTT,
                        txbuf[2],
                        sdr.txSeq - 1,
                        fe_during_tx,
                        keepalive_races);
                }
                sendto(sdr.streamSock, (const char*)txbuf, SUNSDR_IQ_PKT_SIZE, 0,
                    (struct sockaddr*)&streamDest, sizeof(streamDest));
                if (txbuf[2] == SUNSDR_OP_IQ_RX_IDLE) {
                    dbg_keepalive_fe_sent++;
                }
            }
            last_tx_audio_packets = sdr.txAudioPackets;

            tx_keepalive_accum -= 1.0;
        }

        int n = recv(sdr.streamSock, (char*)pktbuf, sizeof(pktbuf), 0);

        /* Log every second regardless */
        DWORD now = GetTickCount();
        if (now - last_log_time >= 1000) {
            unsigned int clamps_this_sec = dbg_elapsed_clamps - dbg_elapsed_clamps_prev_sec;
            dbg_elapsed_clamps_prev_sec = dbg_elapsed_clamps;
            if (clamps_this_sec > 0) {
                sdr_logf("TIMING_CLAMP: elapsed_ms clamped %u times in last 1s (total=%u). Loop stalled > %.1f ms.\n",
                    clamps_this_sec, dbg_elapsed_clamps, elapsed_clamp_ms);
            }
            {
                /* Scheduler-gap histogram delta for the last 1 second.
                 * Buckets (ms): <1 1-2 2-4 4-8 8-16 16-32 >=32.
                 * Expected shape: RX idle -> mostly <1. Active TX/TUNE ->
                 * mostly 4-8 (radio 195 pps = 5.12 ms). Anything in 16-32
                 * is a scheduler tick event. Anything >=32 is pathological. */
                unsigned int d[SDR_GAP_BUCKETS];
                unsigned int total = 0;
                int bi;
                for (bi = 0; bi < SDR_GAP_BUCKETS; bi++) {
                    d[bi] = dbg_gap_hist[bi] - dbg_gap_hist_prev[bi];
                    dbg_gap_hist_prev[bi] = dbg_gap_hist[bi];
                    total += d[bi];
                }
                sdr_logf("RX_GAP_HIST: total=%u <1=%u 1-2=%u 2-4=%u 4-8=%u 8-16=%u 16-32=%u >=32=%u ptt=%d tune=%d\n",
                    total, d[0], d[1], d[2], d[3], d[4], d[5], d[6],
                    sdr.currentPTT, sdr.currentTune);
            }
            {
                /* TX callback gap histogram: scheduler stalls on the WDSP TX
                 * channel worker thread. Expected normal: mostly <1 and 1-2.
                 * Any counts in 8-16 / 16-32 reveal TX-thread preemption
                 * events that correlate with raspy attempts. */
                static unsigned int tx_cb_hist_prev[SDR_TX_CB_HIST_BUCKETS] = {0};
                unsigned int td[SDR_TX_CB_HIST_BUCKETS];
                unsigned int ttotal = 0;
                int tbi;
                for (tbi = 0; tbi < SDR_TX_CB_HIST_BUCKETS; tbi++) {
                    unsigned int cur = (unsigned int)sunsdr_dbg_tx_cb_gap_hist[tbi];
                    td[tbi] = cur - tx_cb_hist_prev[tbi];
                    tx_cb_hist_prev[tbi] = cur;
                    ttotal += td[tbi];
                }
                sdr_logf("TX_CB_GAP_HIST: total=%u <1=%u 1-2=%u 2-4=%u 4-8=%u 8-16=%u 16-32=%u >=32=%u ptt=%d tune=%d\n",
                    ttotal, td[0], td[1], td[2], td[3], td[4], td[5], td[6],
                    sdr.currentPTT, sdr.currentTune);
            }
            {
                /* VAC1 (id=0) counters. type 0 = Out (RX -> speakers), type 1 = In (mic -> TX).
                 * Per user observation 2026-04-14, these climb steadily during TUNE when the
                 * RX silence injection under-feeds WDSP (Out underflow) and the TX feed
                 * Inbound under-drains VAC IN (In overflow). Surfacing them here lets us
                 * correlate clean vs raspy attempts against VAC starvation state. */
                int v1_out_under = 0, v1_out_over = 0, v1_out_ring = 0, v1_out_nring = 0;
                int v1_in_under = 0, v1_in_over = 0, v1_in_ring = 0, v1_in_nring = 0;
                double v1_out_var = 0.0, v1_in_var = 0.0;
                IVAC v1 = pvac[0];
                if (v1) {
                    if (v1->rmatchOUT) {
                        getIVACdiags(0, 0, &v1_out_under, &v1_out_over, &v1_out_var, &v1_out_ring, &v1_out_nring);
                    }
                    if (v1->rmatchIN) {
                        getIVACdiags(0, 1, &v1_in_under, &v1_in_over, &v1_in_var, &v1_in_ring, &v1_in_nring);
                    }
                    sdr_logf("VAC1 status: outUnder=%d outOver=%d outRing=%d/%d inUnder=%d inOver=%d inRing=%d/%d ptt=%d tune=%d\n",
                        v1_out_under, v1_out_over, v1_out_ring, v1_out_nring,
                        v1_in_under, v1_in_over, v1_in_ring, v1_in_nring,
                        sdr.currentPTT, sdr.currentTune);
                }
            }
            sdr_logf("IQ status: pkts=%d, timeouts=%d, keepRunning=%d, HaveSync=%d txAttempt=%ld ptt=%d tune=%d txIqGate=%ld txFeed=%u keepaliveFE=%u keepaliveRace=%u rxSilence=%u realFE=%u realFD=%u xrouterReal=%u xrouterTotal=%u rxAccum=%.3f txPackets=%u activeFD=%ld seqGaps=%ld feDuringTx=%ld keepaliveRaces=%ld iqGateSkips=%ld firstFdBefore0x06=%ld\n",
                pkt_count,
                timeout_count,
                sdr.keepRunning,
                HaveSync,
                sunsdr_dbg_tx_attempt_id,
                sdr.currentPTT,
                sdr.currentTune,
                sunsdr_tx_iq_enabled,
                dbg_tx_feed_buffers,
                dbg_keepalive_fe_sent,
                dbg_keepalive_tx_races,
                dbg_rx_silence_injected,
                dbg_real_fe_packets,
                dbg_real_fd_packets,
                dbg_xrouter_real_feeds,
                dbg_xrouter_total_feeds,
                rx_silence_accum,
                sdr.txAudioPackets,
                sunsdr_dbg_active_tx_packets,
                sunsdr_dbg_active_tx_seq_gaps,
                sunsdr_dbg_idle_fe_during_tx,
                sunsdr_dbg_keepalive_tx_races,
                sunsdr_dbg_tx_iq_gate_skips,
                sunsdr_dbg_first_fd_before_cmd);
            last_log_time = now;
            timeout_count = 0;
            dbg_tx_feed_buffers = 0;
            dbg_keepalive_fe_sent = 0;
            dbg_keepalive_tx_races = 0;
            dbg_rx_silence_injected = 0;
            dbg_real_fe_packets = 0;
            dbg_real_fd_packets = 0;
            dbg_xrouter_real_feeds = 0;
            dbg_xrouter_total_feeds = 0;
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
        if (pktbuf[2] == SUNSDR_OP_IQ_STREAM) {
            dbg_real_fe_packets++;
        } else {
            dbg_real_fd_packets++;
        }

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
                    if (source == 0) {
                        last_rx_pkt_tick = GetTickCount64();
                        dbg_xrouter_real_feeds++;
                        dbg_xrouter_total_feeds++;
                        if (sdr.currentPTT && rx_silence_accum > 0.0) {
                            rx_silence_accum -= 1.0;
                            if (rx_silence_accum < 0.0) {
                                rx_silence_accum = 0.0;
                            }
                        }
                    }
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

static void sunsdr_probe_identity_query(void)
{
    unsigned char pkt[64];
    unsigned char reply[512];
    char hexbuf[1025];
    char asciibuf[257];
    int len;
    DWORD start_tick;
    int packet_count = 0;

    len = sunsdr_parse_hex("32ff07001a000000000001000000000000000000000000000000000000000000000000000000000000000000",
        pkt, (int)sizeof(pkt));
    if (len <= 0) {
        return;
    }

    sunsdr_drain_control_socket("pre_probe");
    sunsdr_set_ctrl_trace_label("identity_probe");
    sdr_logf("IDENTITY probe send: opcode=0x07 len=%d\n", len);
    sunsdr_send_ctrl(pkt, len);
    start_tick = GetTickCount();

    while ((GetTickCount() - start_tick) < 500) {
        fd_set fds;
        struct timeval tv;
        int n;

        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        FD_ZERO(&fds);
        FD_SET(sdr.ctrlSock, &fds);

        if (select(0, &fds, NULL, NULL, &tv) <= 0) {
            continue;
        }

        n = recv(sdr.ctrlSock, (char*)reply, (int)sizeof(reply), 0);
        if (n <= 0) {
            continue;
        }

        packet_count++;
        sunsdr_hexify(reply, n < 128 ? n : 128, hexbuf, (int)sizeof(hexbuf));
        sunsdr_asciiify(reply, n < 128 ? n : 128, asciibuf, (int)sizeof(asciibuf));
        sdr_logf("IDENTITY probe rx #%d: len=%d op=0x%02X hex=%s ascii=%s\n",
            packet_count, n, n >= 3 ? reply[2] : 0, hexbuf, asciibuf);
        sunsdr_cache_identity_candidate(reply, n, "probe");
    }

    sdr_logf("IDENTITY probe complete: packets=%d cached_version=%s\n", packet_count, sdr.firmwareVersionText);
    sunsdr_set_ctrl_trace_label(NULL);
}

static void sunsdr_capture_control_window(const char* label, DWORD duration_ms)
{
    unsigned char reply[1024];
    char hexbuf[1025];
    char asciibuf[257];
    DWORD start_tick;
    int packet_count = 0;

    if (duration_ms == 0) {
        return;
    }

    start_tick = GetTickCount();
    sdr_logf("CONTROL capture start: label=%s duration_ms=%lu\n", label ? label : "capture", (unsigned long)duration_ms);

    while ((GetTickCount() - start_tick) < duration_ms) {
        fd_set fds;
        struct timeval tv;
        int n;

        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        FD_ZERO(&fds);
        FD_SET(sdr.ctrlSock, &fds);

        if (select(0, &fds, NULL, NULL, &tv) <= 0) {
            continue;
        }

        n = recv(sdr.ctrlSock, (char*)reply, (int)sizeof(reply), 0);
        if (n <= 0) {
            continue;
        }

        packet_count++;
        sunsdr_hexify(reply, n < 128 ? n : 128, hexbuf, (int)sizeof(hexbuf));
        sunsdr_asciiify(reply, n < 128 ? n : 128, asciibuf, (int)sizeof(asciibuf));
        sdr_logf("CONTROL capture rx #%d: label=%s len=%d op=0x%02X hex=%s ascii=%s\n",
            packet_count, label ? label : "capture", n, n >= 3 ? reply[2] : 0, hexbuf, asciibuf);
        sunsdr_cache_identity_candidate(reply, n, label ? label : "capture");
    }

    sdr_logf("CONTROL capture complete: label=%s packets=%d cached_version=%s\n",
        label ? label : "capture", packet_count, sdr.firmwareVersionText);
}

static void sunsdr_query_firmware_manager_version(void)
{
    unsigned char pkt[32];
    unsigned char reply[256];
    char hexbuf[513];
    int len;
    int n;

    /*
     * ExpertSDR3 Firmware Manager sends a zero-payload 0x1A query and receives
     * a 0x1A len=0x20 reply whose payload bytes 22/24 carry the displayed
     * firmware version, e.g. 0x58 / 0x08 -> 88.8.
     */
    len = sunsdr_parse_hex("32ff1a000000000000000100000000000000", pkt, (int)sizeof(pkt));
    if (len <= 0) {
        return;
    }

    sunsdr_drain_control_socket("pre_fwmgr");
    sunsdr_set_ctrl_trace_label("fwmgr_version");
    n = sunsdr_send_ctrl_and_recv(pkt, len, reply, (int)sizeof(reply));
    sunsdr_set_ctrl_trace_label(NULL);

    if (n <= 0) {
        sdr_logf("FWMGR query: no reply\n");
        return;
    }

    sunsdr_hexify(reply, n < 64 ? n : 64, hexbuf, (int)sizeof(hexbuf));
    sdr_logf("FWMGR query reply: len=%d hex=%s\n", n, hexbuf);

    if (n >= 26 &&
        reply[0] == 0x32 &&
        reply[1] == 0xFF &&
        reply[2] == 0x1A &&
        reply[4] == 0x20 &&
        reply[5] == 0x00)
    {
        unsigned int version_major = reply[22];
        unsigned int version_minor = reply[24];

        if (version_major > 0 && version_major < 250 && version_minor < 100) {
            _snprintf(sdr.firmwareVersionText, sizeof(sdr.firmwareVersionText), "%u.%u", version_major, version_minor);
            sdr.firmwareVersionText[sizeof(sdr.firmwareVersionText) - 1] = '\0';
            sdr_logf("FWMGR version parsed: %s\n", sdr.firmwareVersionText);
        }
    }
}

int SunSDRGetVersionText(char* buffer, int maxlen)
{
    return sunsdr_copy_text(buffer, maxlen, sdr.firmwareVersionText);
}

int SunSDRGetProtocolText(char* buffer, int maxlen)
{
    return sunsdr_copy_text(buffer, maxlen, sdr.protocolText);
}

int SunSDRGetSerialText(char* buffer, int maxlen)
{
    return sunsdr_copy_text(buffer, maxlen, sdr.serialText);
}

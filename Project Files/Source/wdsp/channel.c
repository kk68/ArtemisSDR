/*  channel.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013 Warren Pratt, NR0V

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

The author can be reached by email at  

warren@wpratt.com

*/

#include "comm.h"

struct _ch ch[MAX_CHANNELS];

void start_thread (int channel)
{
	HANDLE handle = (HANDLE) _beginthread(wdspmain, 0, (void *)(uintptr_t)channel);
	//SetThreadPriority(handle, THREAD_PRIORITY_HIGHEST);
}

/* For non-integer input:dsp rate ratios (SunSDR2 DX native rates), the
 * downstream `rsmpin` polyphase resampler produces a deterministic number
 * of output samples per call ONLY when its input block length is a whole
 * multiple of M (= in_rate / gcd(in_rate, dsp_rate)) AND the matching
 * output length equals k*L (= k * dsp_rate / gcd(...)).
 *
 * `dsp_size` is the block length the DSP chain processes per iteration at
 * dsp_rate. For rsmpin's phase accumulator to return to its initial state
 * at every block boundary, dsp_size MUST be a multiple of L — otherwise
 * rsmpin emits 95 samples on one iteration and 97 on the next, leaving
 * stale or overwriting samples in midbuff and producing a one-sample
 * discontinuity once per DSP iteration (audible as heavy buzzing near the
 * iteration rate).
 *
 * For every integer-ratio case (Anan/HL2) L==1 and every dsp_size trivially
 * qualifies — no behaviour change. Only SunSDR rates (L=96 at 312.5 kHz,
 * L=192 at 156.25 kHz, L=384 at 78.125 kHz, all vs 48 kHz DSP) round up.
 *
 * Round UP (never down): a block smaller than L cannot cover one full
 * polyphase cycle. Caller-requested sizes under L are bumped; multiples of
 * L are kept.
 */
static int adjust_L_for (int in_r, int dsp_r)
{
	int a, b, t;
	if (in_r <= 0 || dsp_r <= 0) return 1;
	a = in_r; b = dsp_r;
	while (b) { t = b; b = a % b; a = t; }
	return dsp_r / a;
}

static void adjust_dsp_size_for_rate_ratio (int channel)
{
	int in_r = ch[channel].in_rate;
	int dsp_r = ch[channel].dsp_rate;
	int L_now, L_48, L_192, L_required, a, b, t, req;
	if (in_r <= 0 || dsp_r <= 0) return;

	L_now = adjust_L_for (in_r, dsp_r);
	if (L_now <= 1) return;  /* integer ratio; Anan/HL2 unchanged */

	/* For SunSDR non-integer ratios, dsp_rate changes on mode switches
	 * (FM uses 192 kHz DSP vs 48 kHz for other modes). If the L factor
	 * differs across dsp_rates, dsp_size would need to change on every
	 * mode switch — which forces WDSP to recompute every FFTW_PATIENT
	 * plan in the chain (bandpass, firmin, cfcomp, etc.), taking
	 * multi-second blocks that Windows flags as "not responding" and
	 * terminates via WER. The observed 3.7 s freeze + crash on 40 m →
	 * 2 m (which switches to NFM, bumping dsp_rate 48 k → 192 k) was
	 * exactly this.
	 *
	 * Fix: round dsp_size up to a value compatible with L at BOTH
	 * common dsp_rates (48 kHz for most modes, 192 kHz for FM). Pick
	 * LCM(L_48, L_192). When the mode later switches, dsp_size is
	 * already aligned for the new L, so the adjust is a no-op and the
	 * FFTW plans don't need to be rebuilt.
	 *
	 * For in_rate=312500: L_48=96, L_192=384, LCM=384.
	 * For in_rate=156250: L_48=192, L_192=768, LCM=768.
	 * For in_rate=78125:  L_48=384, L_192=1536, LCM=1536.
	 *
	 * Anan/HL2 integer-ratio rates: L at every dsp_rate is 1, LCM=1,
	 * no change. */
	L_48 = adjust_L_for (in_r, 48000);
	L_192 = adjust_L_for (in_r, 192000);

	/* LCM(L_48, L_192). */
	a = L_48; b = L_192;
	while (b) { t = b; b = a % b; a = t; }
	if (a <= 0) a = 1;
	L_required = (L_48 / a) * L_192;

	/* Also ensure compatibility with the currently-active rate, even if
	 * it's neither 48 k nor 192 k (defensive). */
	if (L_now > 1)
	{
		a = L_required; b = L_now;
		while (b) { t = b; b = a % b; a = t; }
		if (a > 0)
			L_required = (L_required / a) * L_now;
	}

	req = ch[channel].dsp_size;
	if (req % L_required == 0) return;
	ch[channel].dsp_size = ((req + L_required - 1) / L_required) * L_required;
}

void pre_main_build (int channel)
{
	adjust_dsp_size_for_rate_ratio (channel);

	/* Block-size relationship (each row represents the same block
	 * duration at its own rate):
	 *     dsp_insize  * dsp_rate = dsp_size * in_rate
	 *     dsp_outsize * dsp_rate = dsp_size * out_rate
	 *     out_size    * in_rate  = in_size  * out_rate
	 *
	 * The original implementation split these into two branches
	 * (in_rate >= dsp_rate vs in_rate < dsp_rate) and did the
	 * integer division of the rate ratio BEFORE multiplying. That
	 * truncates to floor for non-integer ratios — fine for Anan/HL2
	 * where every supported rate is an integer multiple of
	 * dsp_rate=48 kHz, wrong for the SunSDR2 DX native rates
	 * (312.5 / 156.25 / 78.125 kHz) where the ratio is fractional.
	 *
	 * Example (in_rate=312500, dsp_rate=48000, dsp_size=64):
	 *     old: dsp_size * (in_rate/dsp_rate) = 64 * 6 = 384
	 *     new: (dsp_size * in_rate) / dsp_rate = 20000000/48000 = 416
	 * The old value made WDSP process each block as if the input
	 * rate were 288 kHz instead of 312.5 kHz — an 8% rate
	 * mis-scaling through every FFT, filter and demod, producing
	 * heavily garbled audio.
	 *
	 * Multiply first, then divide. 64-bit intermediate because at
	 * dsp_size=4096 and in_rate=1.536 MHz the 32-bit product would
	 * overflow.
	 *
	 * For every integer-ratio use case this produces the same result
	 * as before (multiplication and division are associative over
	 * rationals; truncation only bites when the ratio has a
	 * remainder). Only the non-integer SunSDR path changes. */
	long long in_prod  = (long long)ch[channel].dsp_size * ch[channel].in_rate;
	long long out_prod = (long long)ch[channel].dsp_size * ch[channel].out_rate;
	long long xfer     = (long long)ch[channel].in_size  * ch[channel].out_rate;

	ch[channel].dsp_insize  = (int)(in_prod  / ch[channel].dsp_rate);
	ch[channel].dsp_outsize = (int)(out_prod / ch[channel].dsp_rate);
	ch[channel].out_size    = (int)(xfer     / ch[channel].in_rate);

	InitializeCriticalSectionAndSpinCount ( &ch[channel].csDSP, 2500 );
	InitializeCriticalSectionAndSpinCount ( &ch[channel].csEXCH,  2500 );
	InterlockedBitTestAndReset (&ch[channel].flushflag, 0);
	create_iobuffs (channel);
}

void post_main_build (int channel)
{
	InterlockedBitTestAndSet (&ch[channel].run, 0);
	start_thread (channel);
	if (ch[channel].state == 1)
	 	InterlockedBitTestAndSet (&ch[channel].exchange, 0);
}

void build_channel (int channel)
{
	pre_main_build (channel);
	create_main (channel);
	post_main_build (channel);
}

PORT
void OpenChannel (int channel, int in_size, int dsp_size, int input_samplerate, int dsp_rate, int output_samplerate, 
	int type, int state, double tdelayup, double tslewup, double tdelaydown, double tslewdown, int bfo)
{
	ch[channel].in_size = in_size;
	ch[channel].dsp_size = dsp_size;
	ch[channel].in_rate = input_samplerate;
	ch[channel].dsp_rate = dsp_rate;
	ch[channel].out_rate = output_samplerate;
	ch[channel].type = type;
	ch[channel].state = state;
	ch[channel].tdelayup = tdelayup;
	ch[channel].tslewup = tslewup;
	ch[channel].tdelaydown = tdelaydown;
	ch[channel].tslewdown = tslewdown;
	ch[channel].bfo = bfo;
	InterlockedBitTestAndReset (&ch[channel].exchange, 0);
	build_channel (channel);
	if (ch[channel].state)
	{
		InterlockedBitTestAndSet (&ch[channel].iob.pc->slew.upflag, 0);
		InterlockedBitTestAndSet (&ch[channel].iob.ch_upslew, 0);
		InterlockedBitTestAndReset (&ch[channel].iob.pc->exec_bypass, 0);
		InterlockedBitTestAndSet (&ch[channel].exchange, 0);
	}
	_MM_SET_FLUSH_ZERO_MODE (_MM_FLUSH_ZERO_ON);
}

void pre_main_destroy (int channel)
{
	IOB a = ch[channel].iob.pc;
	InterlockedBitTestAndReset (&ch[channel].exchange, 0);
	InterlockedBitTestAndReset (&ch[channel].run, 0);
	InterlockedBitTestAndSet (&ch[channel].iob.pc->exec_bypass, 0);
	ReleaseSemaphore (a->Sem_BuffReady, 1, 0);
	Sleep (25);
}

void post_main_destroy (int channel)
{
	destroy_iobuffs (channel);
	DeleteCriticalSection ( &ch[channel].csEXCH  );
	DeleteCriticalSection ( &ch[channel].csDSP );
}

PORT
void CloseChannel (int channel)
{
	pre_main_destroy (channel);
	destroy_main (channel);
	post_main_destroy (channel);
}

void flushChannel (void* p)
{
	int channel = (int)(uintptr_t)p;
	IOB a = ch[channel].iob.pc;
	while (!InterlockedAnd(&a->flush_bypass, 0xffffffff))
	{
		WaitForSingleObject(a->Sem_Flush, INFINITE);
		if (!InterlockedAnd(&a->flush_bypass, 0xffffffff))
		{
			EnterCriticalSection(&ch[channel].csDSP);
			EnterCriticalSection(&ch[channel].csEXCH);
			flush_iobuffs(channel);
			InterlockedBitTestAndSet(&a->exec_bypass, 0);
			flush_main(channel);
			LeaveCriticalSection(&ch[channel].csEXCH);
			LeaveCriticalSection(&ch[channel].csDSP);
			InterlockedBitTestAndReset(&ch[channel].flushflag, 0);
		}
	}
	InterlockedBitTestAndReset(&a->flush_bypass, 0);
}

/* Synchronous, immediate flush of all channel state (iobuffs + RXA/TXA
 * DSP chain). Mirrors the flushChannel worker's critical work under the
 * same locks. Safe to call while ch[channel].state == 1; buffers are
 * zeroed in place. Intended for callers that need a one-shot flush on
 * transition boundaries (e.g., SUNSDR PTT-on, where carrying TX DSP
 * state from a previous attempt can cause audible transients).
 * Does NOT touch state/slew flags — the caller remains responsible for
 * channel state management via SetChannelState. */
PORT
void FlushChannelNow (int channel)
{
	IOB a = ch[channel].iob.pc;
	EnterCriticalSection(&ch[channel].csDSP);
	EnterCriticalSection(&ch[channel].csEXCH);
	flush_iobuffs(channel);
	InterlockedBitTestAndSet(&a->exec_bypass, 0);
	flush_main(channel);
	InterlockedBitTestAndReset(&a->exec_bypass, 0);
	LeaveCriticalSection(&ch[channel].csEXCH);
	LeaveCriticalSection(&ch[channel].csDSP);
}

/********************************************************************************************************
*																										*
*										Channel Properties												*
*																										*
********************************************************************************************************/

PORT
void SetType (int channel, int type)
{	// no need to rebuild buffers; but we did anyway
	if (type != ch[channel].type)
	{
		CloseChannel (channel);
		ch[channel].type = type;
		build_channel (channel);
	}
}

PORT
void SetInputBuffsize (int channel, int in_size)
{	// we do not rebuild main here since it didn't change
	if (in_size != ch[channel].in_size)
	{
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].in_size = in_size;
		pre_main_build (channel);
		post_main_build (channel);
	}
}

PORT
void SetDSPBuffsize (int channel, int dsp_size)
{
	if (dsp_size != ch[channel].dsp_size)
	{
		int oldstate = SetChannelState (channel, 0, 1);
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].dsp_size = dsp_size;
		pre_main_build (channel);
		setDSPBuffsize_main (channel);
		post_main_build (channel);
		SetChannelState (channel, oldstate, 0);
	}
}

PORT
void SetInputSamplerate (int channel, int in_rate)
{	// no re-build of main required
	if (in_rate != ch[channel].in_rate)
	{
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].in_rate = in_rate;
		pre_main_build (channel);
		setInputSamplerate_main (channel);
		post_main_build (channel);
	}
}

PORT
void SetDSPSamplerate (int channel, int dsp_rate)
{
	if (dsp_rate != ch[channel].dsp_rate)
	{
		int oldstate = SetChannelState (channel, 0, 1);
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].dsp_rate = dsp_rate;
		pre_main_build (channel);
		setDSPSamplerate_main (channel);
		post_main_build (channel);
		SetChannelState (channel, oldstate, 0);
	}
}

PORT
void SetOutputSamplerate (int channel, int out_rate)
{	// no re-build of main required
	if (out_rate != ch[channel].out_rate)
	{
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].out_rate = out_rate;
		pre_main_build (channel);
		setOutputSamplerate_main (channel);
		post_main_build (channel);
	}
}

PORT
void SetAllRates (int channel, int in_rate, int dsp_rate, int out_rate)
{
	if ((in_rate != ch[channel].in_rate) || (dsp_rate != ch[channel].dsp_rate) || (out_rate != ch[channel].out_rate))
	{
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].in_rate  = in_rate;
		ch[channel].dsp_rate = dsp_rate;
		ch[channel].out_rate = out_rate;
		pre_main_build (channel);
		setInputSamplerate_main (channel);
		setDSPSamplerate_main (channel);
		setOutputSamplerate_main (channel);
		post_main_build (channel);
	}
}

PORT
int SetChannelState (int channel, int state, int dmode)
{
	IOB a = ch[channel].iob.pc;
	int prior_state = ch[channel].state;
	int count = 0;
	const int timeout = 100;
	if (ch[channel].state != state)
	{
		ch[channel].state = state;
		switch (ch[channel].state)
		{
		case 0:
			InterlockedBitTestAndSet (&a->slew.downflag, 0);
			InterlockedBitTestAndSet (&ch[channel].flushflag, 0);
			if (dmode)
			{
				while (_InterlockedAnd (&ch[channel].flushflag, 1) && count < timeout) 
				{
					Sleep(1);
					count++;
				}
			}
			if (count >= timeout)
			{
				InterlockedBitTestAndReset (&ch[channel].exchange, 0);
				InterlockedBitTestAndReset (&ch[channel].flushflag, 0);
				InterlockedBitTestAndReset (&a->slew.downflag, 0);
			}
			break;
		case 1:
			InterlockedBitTestAndSet (&a->slew.upflag, 0);
			InterlockedBitTestAndSet (&ch[channel].iob.ch_upslew, 0);
			InterlockedBitTestAndReset (&ch[channel].iob.pc->exec_bypass, 0);
			InterlockedBitTestAndSet (&ch[channel].exchange, 0);
			break;
		}
	}
	return prior_state;
}

PORT
void SetChannelTDelayUp (int channel, double time)
{
	IOB a;
	EnterCriticalSection (&ch[channel].csEXCH);
	a = ch[channel].iob.pc;
	ch[channel].tdelayup = time;
	a->slew.ndelup = (int)(ch[a->channel].tdelayup * ch[a->channel].in_rate);
	flush_slews (a);
	LeaveCriticalSection (&ch[channel].csEXCH);
}

PORT
void SetChannelTSlewUp (int channel, double time)
{
	IOB a;
	EnterCriticalSection (&ch[channel].csEXCH);
	a = ch[channel].iob.pc;
	ch[channel].tslewup = time;
	destroy_slews (a);
	create_slews (a);
	LeaveCriticalSection (&ch[channel].csEXCH);
}

PORT
void SetChannelTDelayDown (int channel, double time)
{
	IOB a;
	EnterCriticalSection (&ch[channel].csEXCH);
	a = ch[channel].iob.pc;
	ch[channel].tdelaydown = time;
	a->slew.ndeldown = (int)(ch[a->channel].tdelaydown * ch[a->channel].out_rate);
	flush_slews (a);
	LeaveCriticalSection (&ch[channel].csEXCH);
}

PORT
void SetChannelTSlewDown (int channel, double time)
{
	IOB a;
	EnterCriticalSection (&ch[channel].csEXCH);
	a = ch[channel].iob.pc;
	ch[channel].tslewdown = time;
	destroy_slews (a);
	create_slews (a);
	LeaveCriticalSection (&ch[channel].csEXCH);
}
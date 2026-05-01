/*  cmasio.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2023 Bryan Rambo W4WMT

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

bryanr@bometals.com

*/
//
//============================================================================================//
// Dual-Licensing Statement (Applies Only to Author's Contributions, Richard Samphire MW0LGE) //
// ------------------------------------------------------------------------------------------ //
// For any code originally written by Richard Samphire MW0LGE, or for any modifications       //
// made by him, the copyright holder for those portions (Richard Samphire) reserves the       //
// right to use, license, and distribute such code under different terms, including           //
// closed-source and proprietary licences, in addition to the GNU General Public License      //
// granted above. Nothing in this statement restricts any rights granted to recipients under  //
// the GNU GPL. Code contributed by others (not Richard Samphire) remains licensed under      //
// its original terms and is not affected by this dual-licensing statement in any way.        //
// Richard Samphire can be reached by email at :  mw0lge@grange-lane.co.uk                    //
//============================================================================================//

#include "cmcomm.h"
#include "obbuffs.h"

/* Defined in sunsdr.c — used here for instrumenting the rate-cascade
 * vs ASIO blocksize relationship. */
extern void sdr_logf(const char* fmt, ...);

cmasio cma = { 0 };
CMASIO pcma = &cma;
int cmaError = 0;

void create_cmasio()
{
	pcma->protocol = 1; // default Protocol 2
	pcma->blocksize = pcm->audio_outsize;
	// TX-side block size: the TX stream's xcm_insize. On integer-ratio rates
	// this matches blocksize; on SunSDR's 312500/48000 non-integer ratio,
	// audio_outsize=96 (RX) but xmtr_insize=64 (TX). Use the TX-stream's
	// actual block size so rmatchIN's pull-side matches what xcmaster's TX
	// loop consumes per iteration.
	{
		int tx_stream = inid(1, 0);
		pcma->tx_size = (tx_stream >= 0 && tx_stream < cmMAXstream)
			? pcm->xcm_insize[tx_stream]
			: pcma->blocksize;
		if (pcma->tx_size <= 0) pcma->tx_size = pcma->blocksize;
	}
	int samplerate = pcm->audio_outrate;
	sdr_logf("[CMASIO ENTRY] create_cmasio() called: blocksize=%d tx_size=%d samplerate=%d audio_outsize=%d audio_outrate=%d\n",
		pcma->blocksize, pcma->tx_size, samplerate, pcm->audio_outsize, pcm->audio_outrate);
	char* asioDriverName = (char*)calloc(32, sizeof(char));
	int regResult = getASIODriverString(asioDriverName);
	if (regResult != 0) {
		sdr_logf("[CMASIO FAIL] getASIODriverString returned %d (no driver configured in registry) — cmASIO will not start\n", regResult);
		free(asioDriverName);
		return;
	}
	sdr_logf("[CMASIO REGISTRY] driver name read from registry: \"%s\" (regResult=%d)\n", asioDriverName, regResult);
	char buf[128];
	sprintf_s(buf, 128, "Initializing cmASIO with: \nblock size = %d\nsample rate = %d\ndriver name = \"%s\"\n\n", pcma->blocksize, samplerate, asioDriverName);
	OutputDebugStringA(buf);

	//[2.10.3.13]MW0LGE get explicit base channel indices for the stereo pair, default to 0 if none in registry
	long input_base_channel = 0;
	if (getASIOBaseInputChannel(&input_base_channel) != 0) input_base_channel = 0;
	long output_base_channel = 0;
	if (getASIOBaseOutputChannel(&output_base_channel) != 0) output_base_channel = 0;

	//[2.10.3.13]MW0LGE the input mode, left = ch1, right = ch2, both = stereo
	long input_mode = 0;
	if (getASIOInputMode(&input_mode) != 0) input_mode = (long)IM_BOTH; //both is default
	pcma->input_mode = (input_mode_t)input_mode;

	//int result = prepareASIO(pcma->blocksize, samplerate, asioDriverName, &CallbackASIO)
	int result = prepareASIO(pcma->blocksize, samplerate, asioDriverName, &CallbackASIO, input_base_channel, output_base_channel);
	sprintf_s(buf, 128, "prepareASIO return = %d", result);
	OutputDebugStringA(buf);
	sdr_logf("[CMASIO PREPARE] prepareASIO(driver=\"%s\", blocksize=%d, rate=%d, in_base=%ld, out_base=%ld) returned %d  (0=success, non-zero=fail)\n",
		asioDriverName, pcma->blocksize, samplerate, input_base_channel, output_base_channel, result);
	if (result != 0) {
		sdr_logf("[CMASIO FAIL] cmASIO will not start — common causes: another app holds the ASIO driver exclusively, driver name mismatch, sample rate not supported by driver, or driver init error. cmaError will be set to %d.\n", result);
	}
	if (result == 0)
	{
		sprintf_s(buf, 128, "ASIO driver \"%s\" is loaded, initialized, and prepared.", asioDriverName);
		OutputDebugStringA(buf);
		pcm->audioCodecId = ASIO;
		pcma->run = 0;

		pcma->output = (double*)malloc0(pcma->blocksize * sizeof(complex));
		pcma->input = (double*)malloc0(pcma->blocksize * sizeof(complex));

		long blockNum = 0;
		if (getASIOBlockNum(&blockNum) != 0) blockNum = 5;
		pcma->lockMode = (0xffff0000 & blockNum) ? 1 : 0;
		blockNum = 0x0000ffff & blockNum;
		sprintf_s(buf, 128, "blockNum = %d\nlockMode = %d", blockNum, pcma->lockMode);
		OutputDebugStringA(buf);

		// rmatchIN: ASIO callback pushes `blocksize` (96) samples per fill;
		// asioIN pulls `tx_size` (64 on SunSDR) samples per call. Same rate
		// (samplerate==samplerate), just different block boundaries —
		// rmatchV handles the buffer-size matching internally.
		pcma->rmatchIN = create_rmatchV(pcma->blocksize, pcma->tx_size, samplerate, samplerate, blockNum * pcma->blocksize, 1);
		// rmatchOUT: asioOUT pushes `blocksize` (96) samples; ASIO callback
		// pulls `blocksize` (96) samples — RX side is symmetric.
		pcma->rmatchOUT = create_rmatchV(pcma->blocksize, pcma->blocksize, samplerate, samplerate, blockNum * pcma->blocksize, 1);
		forceRMatchVar(pcma->rmatchIN, 1, 1);
		forceRMatchVar(pcma->rmatchOUT, 1, 1);
		sdr_logf("[CMASIO INIT] blocksize=%d samplerate=%d blockNum=%ld lockMode=%d  rmatch in/out_size=%d in/out_rate=%d  audio_outsize=%d ch_outsize[0]=%d\n",
			pcma->blocksize, samplerate, blockNum, pcma->lockMode,
			pcma->blocksize, samplerate,
			pcm->audio_outsize, pcm->cmRCVR > 0 ? pcm->rcvr[0].ch_outsize : -1);

		pcma->bufferFull = CreateSemaphore(NULL, 0, 1, NULL);
		pcma->bufferEmpty = CreateSemaphore(NULL, 1, 1, NULL);

		pcma->overFlowsIn = pcma->overFlowsOut = pcma->underFlowsIn = pcma->underFlowsOut = 0;
	}
	cmaError = result;
	free(asioDriverName);
}

void destroy_cmasio()
{
	if (pcm->audioCodecId != ASIO) return;
	CloseHandle(pcma->bufferFull);
	CloseHandle(pcma->bufferEmpty);
	destroy_rmatchV(pcma->rmatchIN);
	destroy_rmatchV(pcma->rmatchOUT);
	_aligned_free(pcma->input);
	_aligned_free(pcma->output);
	unloadASIO();

	char buf[128];
	sprintf_s(buf, 128, "cmASIO has been destroyed");
	OutputDebugStringA(buf);
}

void asioIN(double* in_tx)
{	// used for ASIO MIC data to TX
	if (pcm->audioCodecId != ASIO) return;
	// TX path uses tx_size, NOT blocksize. xcmaster's TX iteration consumes
	// xcm_insize=tx_size complex samples per call. On SunSDR's non-integer
	// rate ratio (audio_outsize=96 vs xmtr_insize=64) using blocksize=96
	// here pulled 96 samples per call against a 750/sec call rate — total
	// drain 72000 samples/sec vs ASIO supply 48000 samples/sec → constant
	// underrun → robotic TX audio. Pulling tx_size=64 matches consumption
	// to supply.
	if (pcma->lockMode)
	{
		if (WaitForSingleObject(pcma->bufferFull, 2) == WAIT_TIMEOUT) { ++pcma->underFlowsIn; return; }
		memcpy(in_tx, pcma->input, pcma->tx_size * sizeof(complex));
		combinebuff(pcma->tx_size, in_tx, in_tx);
		ReleaseSemaphore(pcma->bufferEmpty, 1, NULL);
	}
	else
	{
		xrmatchOUT(pcma->rmatchIN, in_tx);
		combinebuff(pcma->tx_size, in_tx, in_tx);
	}
	/* PSA diag: peak |sample| from cmASIO mic into TX stream, summarised once
	 * per second. Tells us whether PC mic is actually reaching WDSP TXA when
	 * voice doesn't appear in the wire 0xFD packets. */
	{
		static ULONGLONG psa_diag_last_tick = 0;
		static double psa_diag_peak = 0.0;
		static long psa_diag_calls = 0;
		ULONGLONG now = GetTickCount64();
		int k;
		for (k = 0; k < pcma->tx_size; k++) {
			double mi = fabs(in_tx[2*k]);
			double mq = fabs(in_tx[2*k+1]);
			if (mi > psa_diag_peak) psa_diag_peak = mi;
			if (mq > psa_diag_peak) psa_diag_peak = mq;
		}
		psa_diag_calls++;
		if (psa_diag_last_tick == 0) psa_diag_last_tick = now;
		else if (now - psa_diag_last_tick >= 1000) {
			sdr_logf("PSA_DIAG_ASIOIN peak=%.6f calls=%ld lockMode=%d underflow_in=%ld\n",
				psa_diag_peak, psa_diag_calls, pcma->lockMode, pcma->underFlowsIn);
			psa_diag_peak = 0.0;
			psa_diag_calls = 0;
			psa_diag_last_tick = now;
		}
	}
}

void asioOUT(int id, int nsamples, double* buff)
{	// called by the global mixer with a buffer of output data for ASIO

	/* SunSDR audio debug: one-shot logging */
	{
		static volatile long asio_dbg_count = 0;
		long c = InterlockedIncrement(&asio_dbg_count);
		if (c <= 5 || (c <= 1000 && c % 200 == 0))
		{
			char dbg[256];
			sprintf(dbg, "[asioOUT] id=%d nsamples=%d run=%d protocol=%d (call #%ld)\n",
				id, nsamples, pcma->run, pcma->protocol, c);
			OutputDebugStringA(dbg);
		}
	}

	if (!pcma->run) return;
	xrmatchIN(pcma->rmatchOUT, buff);		// audio data from mixer
	if (pcma->protocol == 0) // W4WMT cmASIO via Protocol 1
	{
		memset(buff, 0, nsamples * sizeof(complex));
		OutBound(0, nsamples, buff);
	} // W4WMT cmASIO via Protocol 1
}

//[2.10.3.13]MW0LGE added input mode, so can use ch1(L), ch2(R), or both for input
//clamp and speed refactor
void CallbackASIO(void* inputL, void* inputR, void* outputL, void* outputR)
{
	const double inv_scale = 1.0 / 2147483648.0;
	const double scale = 2147483648.0;
	const double max_i32 = 2147483647.0;
	const double min_i32 = -2147483648.0;

	long* inL = (long*)inputL;
	long* inR = (long*)inputR;
	long* outL = (long*)outputL;
	long* outR = (long*)outputR;

	if (pcma->input_mode == IM_LEFT)
	{
		inR = inL;
	}
	else if (pcma->input_mode == IM_RIGHT)
	{
		inL = inR;
	}

	if (pcma->lockMode)
	{
		DWORD got = WaitForSingleObject(pcma->bufferEmpty, 0);
		if (got == WAIT_OBJECT_0)
		{
			int i = 0;
			int j = 0;
			for (i = 0; i < pcma->blocksize; i++, j += 2)
			{
				pcma->input[j] = (double)inL[i] * inv_scale;
				pcma->input[j + 1] = (double)inR[i] * inv_scale;
			}
			ReleaseSemaphore(pcma->bufferFull, 1, NULL);

			xrmatchOUT(pcma->rmatchOUT, pcma->output);

			j = 0;
			for (i = 0; i < pcma->blocksize; i++, j += 2)
			{
				double dl = pcma->output[j] * scale;
				double dr = pcma->output[j + 1] * scale;

				if (dl > max_i32) dl = max_i32;
				else if (dl < min_i32) dl = min_i32;

				if (dr > max_i32) dr = max_i32;
				else if (dr < min_i32) dr = min_i32;

				outL[i] = (long)dl;
				outR[i] = (long)dr;
			}
			return;
		}

		xrmatchOUT(pcma->rmatchOUT, pcma->output);

		{
			int i = 0;
			int j = 0;
			for (i = 0; i < pcma->blocksize; i++, j += 2)
			{
				double dl = pcma->output[j] * scale;
				double dr = pcma->output[j + 1] * scale;

				if (dl > max_i32) dl = max_i32;
				else if (dl < min_i32) dl = min_i32;

				if (dr > max_i32) dr = max_i32;
				else if (dr < min_i32) dr = min_i32;

				outL[i] = (long)dl;
				outR[i] = (long)dr;
			}
		}

		if (WaitForSingleObject(pcma->bufferEmpty, 2) == WAIT_TIMEOUT)
		{
			++pcma->overFlowsIn;
			return;
		}

		{
			int i = 0;
			int j = 0;
			for (i = 0; i < pcma->blocksize; i++, j += 2)
			{
				pcma->input[j] = (double)inL[i] * inv_scale;
				pcma->input[j + 1] = (double)inR[i] * inv_scale;
			}
			ReleaseSemaphore(pcma->bufferFull, 1, NULL);
		}

		return;
	}

	{
		int i = 0;
		int j = 0;
		for (i = 0; i < pcma->blocksize; i++, j += 2)
		{
			pcma->input[j] = (double)inL[i] * inv_scale;
			pcma->input[j + 1] = (double)inR[i] * inv_scale;
		}
	}

	xrmatchIN(pcma->rmatchIN, pcma->input);
	xrmatchOUT(pcma->rmatchOUT, pcma->output);

	{
		int i = 0;
		int j = 0;
		for (i = 0; i < pcma->blocksize; i++, j += 2)
		{
			double dl = pcma->output[j] * scale;
			double dr = pcma->output[j + 1] * scale;

			if (dl > max_i32) dl = max_i32;
			else if (dl < min_i32) dl = min_i32;

			if (dr > max_i32) dr = max_i32;
			else if (dr < min_i32) dr = min_i32;

			outL[i] = (long)dl;
			outR[i] = (long)dr;
		}
	}
}

//void CallbackASIO(void* inputL, void* inputR, void* outputL, void* outputR)
//{
//	if (pcma->lockMode)
//	{
//		if (WaitForSingleObject(pcma->bufferEmpty, 0) == WAIT_OBJECT_0)
//		{
//			for (int i = 0; i < pcma->blocksize; i++)
//			{
//				pcma->input[2 * i] = ((double)((long*)inputL)[i]) / 2147483648.0;
//				pcma->input[2 * i + 1] = ((double)((long*)inputR)[i]) / 2147483648.0;
//			}
//			ReleaseSemaphore(pcma->bufferFull, 1, NULL);
//
//			xrmatchOUT(pcma->rmatchOUT, pcma->output);
//			for (int i = 0; i < pcma->blocksize; i++)
//			{
//				((long*)outputL)[i] = (long)(pcma->output[2 * i] * 2147483648.0);
//				((long*)outputR)[i] = (long)(pcma->output[2 * i + 1] * 2147483648.0);
//			}
//		}
//		else
//		{
//			xrmatchOUT(pcma->rmatchOUT, pcma->output);
//			for (int i = 0; i < pcma->blocksize; i++)
//			{
//				((long*)outputL)[i] = (long)(pcma->output[2 * i] * 2147483648.0);
//				((long*)outputR)[i] = (long)(pcma->output[2 * i + 1] * 2147483648.0);
//			}
//
//			if (WaitForSingleObject(pcma->bufferEmpty, 2) == WAIT_TIMEOUT) { ++pcma->overFlowsIn; return; }
//			for (int i = 0; i < pcma->blocksize; i++)
//			{
//				pcma->input[2 * i] = ((double)((long*)inputL)[i]) / 2147483648.0;
//				pcma->input[2 * i + 1] = ((double)((long*)inputR)[i]) / 2147483648.0;
//			}
//			ReleaseSemaphore(pcma->bufferFull, 1, NULL);
//		}
//	}
//	else
//	{
//		for (int i = 0; i < pcma->blocksize; i++)
//		{
//			pcma->input[2 * i] = ((double)((long*)inputL)[i]) / 2147483648.0;
//			pcma->input[2 * i + 1] = ((double)((long*)inputR)[i]) / 2147483648.0;
//		}
//		xrmatchIN(pcma->rmatchIN, pcma->input);
//
//		xrmatchOUT(pcma->rmatchOUT, pcma->output);
//		for (int i = 0; i < pcma->blocksize; i++)
//		{
//			((long*)outputL)[i] = (long)(pcma->output[2 * i] * 2147483648.0);
//			((long*)outputR)[i] = (long)(pcma->output[2 * i + 1] * 2147483648.0);
//		}
//	}
//}

//SetAAudioMixOutputPointer(0, 0, asio_out);

long cm_asioStart(int protocol)
{
	if (pcm->audioCodecId != ASIO) return -1;
	pcma->protocol = protocol; // W4WMT cmASIO via Protocol 1
	//if (protocol == 0)
	//{
	//	SendpOutboundRx(OutBound);
	//	pcm->audioCodecId = HERMES;
	//	char buf[128];
	//	sprintf_s(buf, 128, "Protocol1 Aborting!");
	//	OutputDebugStringA(buf);
	//	return -1;
	//}
	char buf[128];
	long result = asioStart();
	sprintf_s(buf, 128, "asioStart = %d", result);
	OutputDebugStringA(buf);
	pcma->run = 1;
	return result;
}

long cm_asioStop()
{
	if (pcm->audioCodecId != ASIO) return -1;
	pcma->run = 0;
	char buf[128];
	long result = asioStop();
	sprintf_s(buf, 128, "asioStop = %d", result);
	OutputDebugStringA(buf);
	return result;
}

PORT
int getCMAstate()
{
	if (cmaError)
	{
		return -1;
	}
	return (pcm->audioCodecId == ASIO) ? 1 : 0;
}

PORT
void getCMAevents(long* overFlowsIn, long* overFlowsOut, long* underFlowsIn, long* underFlowsOut)
{
	if (pcm->audioCodecId != ASIO) return;
	double foo = 0.0;
	double* pfoo = &foo;
	int bar = 0;
	int* pbar = &bar;
	getRMatchDiags(pcma->rmatchIN, underFlowsIn, overFlowsIn, pfoo, pbar, pbar);
	getRMatchDiags(pcma->rmatchOUT, underFlowsOut, overFlowsOut, pfoo, pbar, pbar);
	if (pcma->lockMode)
	{
		*overFlowsIn = pcma->overFlowsIn;
		*underFlowsIn = pcma->underFlowsIn;
	}
}

PORT
void resetCMAevents()
{
	if (pcm->audioCodecId != ASIO) return;
	pcma->overFlowsIn = pcma->overFlowsOut = pcma->underFlowsIn = pcma->underFlowsOut = 0;
	resetRMatchDiags(pcma->rmatchIN);
	resetRMatchDiags(pcma->rmatchOUT);
}
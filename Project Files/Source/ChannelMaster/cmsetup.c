/*  cmsetup.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2014 Warren Pratt, NR0V

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

#include "cmsetup.h"
#include "cmaster.h"

// set radio structure, call this first
// these parameters are used by create_cmaster() to determine units to create & buffer sizes
PORT
void SetRadioStructure (
	int cmSTREAM,			// total number of input streams to set up
	int cmRCVR,				// number of receivers to set up
	int cmXMTR,				// number of transmitters to set up
	int cmSubRCVR,			// number of sub-receivers per receiver to set up
	int cmNspc,				// number of TYPES of special units
	int* cmSPC,				// number of special units of each type
	int* cmMAXInbound,		// maximum number of samples in a call to Inbound(), per stream
	int cmMAXInRate,		// maximum sample rate of an input stream
	int cmMAXAudioRate,		// maximum channel audio output rate (incl. rcvr and tx monitor audio)
	int cmMAXTxOutRate		// maximum transmitter channel output sample rate
	)
{
	pcm->cmSTREAM = cmSTREAM;
	pcm->cmRCVR = cmRCVR;
	pcm->cmXMTR = cmXMTR;
	pcm->cmSubRCVR = cmSubRCVR;
	pcm->cmNspc = cmNspc;
	memcpy (pcm->cmSPC, cmSPC, pcm->cmNspc * sizeof (int));
	memcpy (pcm->cmMAXInbound, cmMAXInbound, pcm->cmSTREAM * sizeof (int));
	pcm->cmMAXInRate = cmMAXInRate;
	pcm->cmMAXAudioRate = cmMAXAudioRate;
	pcm->cmMAXTxOutRate = cmMAXTxOutRate;
}

// set default sample rates, call this before 'create'
PORT
void set_cmdefault_rates (
	int* xcm_inrates,
	int  aud_outrate,
	int* rcvr_ch_outrates,
	int* xmtr_ch_outrates
	)
{
	int i;
	for (i = 0; i < pcm->cmSTREAM; i++)
	{
		pcm->xcm_inrate[i] = xcm_inrates[i];
		pcm->xcm_insize[i] = getbuffsize (pcm->xcm_inrate[i]);
	}
	pcm->audio_outrate = aud_outrate;
	pcm->audioCodecId = HERMES;
	for (i = 0; i < pcm->cmRCVR; i++)
	{
		pcm->rcvr[i].ch_outrate = rcvr_ch_outrates[i];
		/* ch_outsize must match the audio block size the WDSP channel
		 * actually emits per fexchange, which is
		 *     xcm_insize * ch_outrate / xcm_inrate
		 * For Anan/HL2 integer-ratio rates this is algebraically equal
		 * to getbuffsize(ch_outrate), so no behaviour change. For
		 * SunSDR non-integer ratios (xcm_insize rounded to multiple
		 * of M by getbuffsize() above), the channel produces a larger
		 * per-call block (e.g. 96 at 312.5 kHz in with 48 kHz out)
		 * and getbuffsize(ch_outrate) alone (64) would under-sample,
		 * causing 33% VAC underflow and staccato audio. */
		if (pcm->xcm_inrate[i] > 0)
			pcm->rcvr[i].ch_outsize = (int)(((long long)pcm->xcm_insize[i]
			                                 * pcm->rcvr[i].ch_outrate)
			                                / pcm->xcm_inrate[i]);
		else
			pcm->rcvr[i].ch_outsize = getbuffsize (pcm->rcvr[i].ch_outrate);
	}
	/* audio_outsize is the block size the AAMixer emits to downstream
	 * (cmasio, VAC via xvacOUT, etc.). It must equal the size of audio
	 * chunks arriving from the DSP channels — i.e. rcvr[0].ch_outsize
	 * (or max across receivers). Using getbuffsize(audio_outrate) alone
	 * assumes integer rate ratios and undersamples at SunSDR rates
	 * (64 instead of 96), choking the mixer and causing VAC underflow.
	 * Must be set AFTER rcvr[].ch_outsize above. */
	if (pcm->cmRCVR > 0 && pcm->rcvr[0].ch_outsize > 0)
		pcm->audio_outsize = pcm->rcvr[0].ch_outsize;
	else
		pcm->audio_outsize = getbuffsize (pcm->audio_outrate);
	for (i = 0; i < pcm->cmXMTR; i++)
	{
		pcm->xmtr[i].ch_outrate = xmtr_ch_outrates[i];
		/* Symmetric treatment for TX: stream index for xmtr[i] is
		 * pcm->cmRCVR + i (receivers come first in the xcm_ array). */
		{
			int s = pcm->cmRCVR + i;
			if (s < pcm->cmSTREAM && pcm->xcm_inrate[s] > 0)
				pcm->xmtr[i].ch_outsize = (int)(((long long)pcm->xcm_insize[s]
				                                 * pcm->xmtr[i].ch_outrate)
				                                / pcm->xcm_inrate[s]);
			else
				pcm->xmtr[i].ch_outsize = getbuffsize (pcm->xmtr[i].ch_outrate);
		}
	}
}

PORT
void CreateRadio()
{
	create_cmaster();
	create_pipe();
	create_sync();
}

PORT
void DestroyRadio()
{
	destroy_sync();
	destroy_pipe();
	destroy_cmaster();
}

// buffer sizes are a function of sample rate to yield constant latency.
//
// For clean polyphase resampling in the downstream WDSP chain, the buffer
// size passed to OpenChannel() as 'in_size' must be a whole multiple of the
// polyphase M-factor (= rate / gcd(rate, dsp_rate)). If it isn't, the
// resampler's phase accumulator doesn't return to its initial state at the
// end of each call and the number of output samples per call fluctuates by
// ±1, creating a block-boundary discontinuity once per DSP iteration —
// audible as a loud ~750 Hz buzz at a 312.5 kHz input rate.
//
// For integer-ratio rates (every rate on Anan/HL2 is a multiple of the
// 48 kHz base_rate), the natural formula already gives a multiple of M=1
// and nothing changes. For the SunSDR2 DX native rates (312500, 156250,
// 78125, all sharing gcd=500..125 with 48000 and M=625) the natural formula
// yields 416 / 208 / 104 samples, none of which are multiples of 625, and
// we round up to 625 — giving a clean 2 / 4 / 8 ms block period.
PORT
int getbuffsize (int rate)
{
	const int base_rate = 48000;
	const int base_size = 64;
	int natural, a, b, t, m;

	natural = base_size * rate / base_rate;

	/* M = rate / gcd(rate, base_rate) */
	a = rate; b = base_rate;
	while (b) { t = b; b = a % b; a = t; }
	m = rate / a;

	if (m <= 1) return natural;              /* integer ratio: unchanged */
	return ((natural + m - 1) / m) * m;      /* round up to multiple of M */
}

PORT
int getInputRate (int stype, int id)
{
	return pcm->xcm_inrate[inid (stype, id)];
}

PORT
int getChannelOutputRate (int stype, int id)
{
	int rate;
	switch (stype)
	{
	case 0:
		rate = pcm->rcvr[id].ch_outrate;
		break;
	case 1:
		rate = pcm->xmtr[id].ch_outrate;
		break;
	}
	return rate;
}

// Inbound Stream & Channel IDs
//
// Inbound Data Streams are numbered beginning with receiver streams, followed by transmitter streams, and
// the followed by Special Streams.  An example of a special stream might be a receive data stream that is
// not used in a 'standard receiver' but is instead used only to create a panadapter display.  There can be
// multiple classes of Special Streams, each with a range of values.
//
// Receiver Streams are numbered 0 through (cmMAXrcvr - 1).  Transmitter streams are numbered cmMAXrcvr 
// through (cmMAXrcvr + cmMAXxmtr - 1).  Special streams begin at (cmMAXrcvr + cmMAXxmtr).
// ChannelMaster internal 'id's use an 'id' number rather than the stream number.  These 'id's are 
// determined as follows:

int rxid (int stream)					// ChannelMaster id of a receiver
{
	return stream;
}

int txid (int stream)					// ChannelMaster id of a transmitter
{
	return stream - pcm->cmRCVR;
}

int sp0id (int stream)
{
	return stream - pcm->cmRCVR - pcm->cmXMTR;
}

int stype (int stream)					// the stream type, 'stype', can be inferred from stream number
{
	int type;
	if      (stream < pcm->cmRCVR)						type =  0;	// rx
	else if (stream < pcm->cmRCVR + pcm->cmXMTR)		type =  1;	// tx
	else												type =  2;	// special
	return type;
}

// DSP Channels must be allocated for receivers and transmitters.  There are cmSubRcvr channels per
// receiver data stream and there is one channel per transmitter data stream.  Channel numbers for
// receiver and transmitter channels are determined as follows:

PORT
int chid (int stream, int subrx)		 // id of a dsp channel ('channel' number)
{
	int ch_id;
	switch (stype (stream))
	{
	case 0:
		ch_id = pcm->cmSubRCVR * stream + subrx;
		break;
	case 1:
		ch_id = stream + (pcm->cmSubRCVR - 1) * pcm->cmRCVR;
		break;
	default:
		ch_id = -1;
		break;
	}
	return ch_id;
}

PORT
int inid (int stype, int id)	// the id of the input buffer (= id of stream) can be determined
{
	int in_id;
	switch (stype)
	{
	case 0:
		in_id = id;
		break;
	case 1:
		in_id = id + pcm->cmRCVR;
		break;
	case 2:
		in_id = id + pcm->cmRCVR + pcm->cmXMTR;
		break;
	default:
		in_id = -1;
		break;
	}
	return in_id;
}

int mixinid (int stream, int subrx)	// audio mixer input id
{
	int mix_in_id;
	switch (stype (stream))
	{
	case 0:
		mix_in_id = pcm->cmSubRCVR * stream + subrx;
		break;
	case 1:
		mix_in_id = stream + (pcm->cmSubRCVR - 1) * pcm->cmRCVR;
		break;
	default:
		mix_in_id = -1;
		break;
	}
	return mix_in_id;
}

// Hardware-free regression tests using the production mixers, VAC, sidetone
// and built WDSP DLL. No PortAudio stream or radio connection is opened.
#include "cmcomm.h"
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <dbghelp.h>

static LONG WINAPI report_crash(EXCEPTION_POINTERS* exception)
{
    STACKFRAME64 frame = {0};
    CONTEXT context = *exception->ContextRecord;
    HANDLE process = GetCurrentProcess();
    int i;
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    SymInitialize(process, NULL, TRUE);
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    fprintf(stderr, "Audio access violation at %p\n", exception->ExceptionRecord->ExceptionAddress);
    for (i = 0; i < 12; ++i) {
        char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
        SYMBOL_INFO* symbol = (SYMBOL_INFO*)storage;
        DWORD64 displacement = 0;
        memset(storage, 0, sizeof(storage));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
            fprintf(stderr, "  %s + 0x%llx\n", symbol->Name, displacement);
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame,
            &context, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

extern IVAC pvac[MAX_EXT_VACS];
extern void SetIVACmox(int, int);
extern void SetIVACmon(int, int);
extern void SetIVACmonVol(int, double);
extern void SetIVACrxscale(int, double);
extern void SetIVACafgain(int, double);
extern void SetTXAMonitorPre(int, int);
extern void SetTXAPanelGain1(int, double);
extern void SetTXAPostGenRun(int, int);
extern void SetTXAPostGenMode(int, int);
extern void SetTXAPostGenToneFreq(int, double);

static cmaster test_cm;
CMASTER pcm = &test_cm;
// Only used for the unused VAC input bitbucket allocation in this harness.
int getbuffsize(int rate) { (void)rate; return 4096; }
void sdr_logf(const char* fmt, ...) { (void)fmt; }

#define CHECK(condition, message) do { if (!(condition)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); exit(1); } } while (0)

static double captured[2 * 96000];
static volatile LONG received;

static void collect(int id, int count, double* data)
{
    LONG start = InterlockedCompareExchange(&received, 0, 0);
    (void)id;
    CHECK(start + count <= 96000, "capture capacity");
    memcpy(captured + 2 * start, data, count * sizeof(complex));
    InterlockedExchange(&received, start + count);
}

static void wait_samples(int count)
{
    ULONGLONG deadline = GetTickCount64() + 3000;
    while (InterlockedCompareExchange(&received, 0, 0) < count && GetTickCount64() < deadline)
        Sleep(1);
    CHECK(received == count, "mixer must produce exactly the expected sample count");
}

static void tone(double* data, int size, int offset, int rate, double gain)
{
    int i;
    for (i = 0; i < size; ++i)
        data[2*i] = data[2*i+1] = gain * sin(TWOPI * 1000.0 * (offset + i) / rate);
}

static double rms(double* data, int skip, int count)
{
    int i;
    double sum = 0;
    for (i = skip; i < count; ++i) {
        CHECK(_finite(data[2*i]), "finite audio");
        CHECK(fabs(data[2*i] - data[2*i+1]) < 1e-10, "mono audio in both ears");
        sum += data[2*i] * data[2*i];
    }
    return sqrt(sum / (count - skip));
}

static void check_tone(double* data, int skip, int count, int rate)
{
    int i;
    double si = 0, co = 0, energy = 0;
    // Integer numbers of cycles, independent of the resampler's phase delay.
    skip = ((skip + rate/1000 - 1) / (rate/1000)) * (rate/1000);
    count = (count / (rate/1000)) * (rate/1000);
    for (i = skip; i < count; ++i) {
        si += data[2*i] * sin(TWOPI * 1000 * i / rate);
        co += data[2*i] * cos(TWOPI * 1000 * i / rate);
        energy += data[2*i] * data[2*i];
    }
    CHECK(energy > 0.001, "non-silent tone");
    CHECK(2 * (si*si + co*co) / ((count-skip) * energy) > 0.999,
        "continuous 1 kHz tone, without stale samples or timing distortion");
}

static void mixer_blocks(int txrate, int output_size)
{
    int rates[2] = {48000, txrate};
    int txsize = txrate / 750, block, i;
    DWORD previous;
    // End the source buffer at a guard page. The original 384-vs-256
    // overread at 192 kHz faults immediately, rather than reading slack.
    unsigned char* pages = VirtualAlloc(0, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    double* input = (double*)(pages + 4096 - txsize * sizeof(complex));
    AAMIX a;
    CHECK(pages && VirtualProtect(pages + 4096, 4096, PAGE_NOACCESS, &previous), "guard page");
    received = 0;
    a = create_aamix(-1, 0, output_size, output_size, 2, 2, 2, 1.0, 4096,
        rates, 48000, collect, 0, 0, 0, 0);
    for (block = 0; block < 300; block += 3) {
        for (i = 0; i < 3; ++i) {
            tone(input, txsize, (block+i) * txsize, txrate, 1.0);
            xMixAudioSized(a, -1, 1, txsize, input);
        }
        wait_samples((block+3) * 64);
    }
    check_tone(captured, 1024, received, 48000);
    CHECK(fabs(rms(captured, 1200, received) - sqrt(0.5)) < 0.003, "monitor amplitude");
    destroy_aamix(a, 0);
    VirtualFree(pages, 0, MEM_RELEASE);
    printf("PASS mixer: TX %d Hz / %d samples -> 48000 Hz / %d samples\n", txrate, txsize, output_size);
}

static void vac_routes(void)
{
    double rx[192], tx[512];
    IVAC v;
    AAMIX a;
    int i;
    test_cm.cmMAXInRate = 1536000;
    create_ivac(0, 1, 0, 1, 312500, 48000, 48000, 192000, 48000, 64, 625, 96, 256, 96);
    v = pvac[0];
    SetIVACaudioSize(0, 96); // the recreation that used to permanently disable MON
    a = v->mixer;
    SetAAudioMixOutputPointer(a, 0, collect);
    received = 0;
    tone(rx, 96, 0, 48000, 0.25);
    xvacOUT(0, 1, rx);
    wait_samples(96); // RX starts even with no TX keepalive
    CHECK(rms(captured, 0, 96) > 0.1, "receive audio");

    SetIVACmon(0, 1);
    SetIVACmox(0, 1);
    received = 0;
    for (i = 0; i < 3; ++i) {
        tone(tx, 256, i*256, 192000, 0.5);
        xvacOUT(0, 2, tx);
    }
    wait_samples(192); // TX must not wait for SunSDR's throttled RX packets
    CHECK(rms(captured, 96, 192) > 0.1, "VAC TX monitor audible");
    SetIVACmon(0, 0);
    received = 0;
    for (i = 0; i < 3; ++i) xvacOUT(0, 2, tx);
    wait_samples(192);
    CHECK(rms(captured, 0, 192) == 0, "MON off keeps flowing silence during TX");

    SetIVACmon(0, 1);
    SetIVACmonVol(0, 0.3);
    SetIVACrxscale(0, 0.7);
    SetIVACafgain(0, 0.2);
    SetIVACaudioSize(0, 96);
    a = v->mixer;
    CHECK(a->active == 2 && a->what == 2, "recreation preserves TX monitor route");
    CHECK(fabs(a->tvol[1] - 0.21) < 1e-10 && fabs(a->tvol[0] - 0.14) < 1e-10,
        "recreation preserves monitor volume and receive gains");
    SetIVACaudioRate(0, 96000);
    SetIVACaudioRate(0, 48000);
    SetIVACtxmonRate(0, 96000);
    SetIVACtxmonSize(0, 128);
    SetIVACtxmonRate(0, 192000);
    SetIVACtxmonSize(0, 256);
    a = v->mixer;
    CHECK(a->active == 2 && a->what == 2 && fabs(a->tvol[1] - 0.21) < 1e-10,
        "rate changes preserve route and volume");
    SetIVACmox(0, 0);
    SetIVACmon(0, 0);
    SetAAudioMixOutputPointer(a, 0, collect);
    received = 0;
    xvacOUT(0, 1, rx);
    wait_samples(96);
    CHECK(rms(captured, 0, 96) > 0.01, "RX returns after TX and rate changes");
    for (i = 0; i < 20; ++i) {
        SetIVACmon(0, 1);
        SetIVACmox(0, 1);
        SetIVACmon(0, 0);
        SetIVACmox(0, 0);
    }
    received = 0;
    xvacOUT(0, 1, rx);
    wait_samples(96);
    destroy_ivac(0);
    puts("PASS VAC: startup, MON on/off, TX without RX, RX return, reconfiguration and gains");
}

static void dsp_capture(int pre, double gain, double* audio, double* iq)
{
    int block, i, error;
    double input[128], raw[128], output[512], monitor[512];
    SetTXAMonitorPre(0, pre);
    SetTXAPanelGain1(0, gain);
    for (block = 0; block < 600; ++block) {
        tone(input, 64, block*64, 48000, 0.02);
        tone(raw, 64, block*64, 48000, 0.1); // distinct pre-DEXP input
        fexchange0WithMonitor(0, input, output, raw, monitor, &error);
        CHECK(error == 0, "WDSP exchange");
        if (block >= 300) {
            for (i = 0; i < 512; ++i) {
                audio[(block-300)*512+i] = monitor[i];
                iq[(block-300)*512+i] = output[i];
            }
        }
    }
}

static void dsp_taps(void)
{
    double* audio = calloc(2*76800, sizeof(double));
    double* iq = calloc(2*76800, sizeof(double));
    double pre1, pre2, post1, post2, rf1 = 0, rf2 = 0;
    int i, error, mode;
    double input[128] = {0}, out[512], mon[512];
    OpenChannel(0, 64, 4096, 48000, 96000, 192000, 1, 1, 0, 0, 0, 0, 1);
    SetTXAMode(0, TXA_USB);
    SetTXALevelerSt(0, 0);
    dsp_capture(1, 1.0, audio, iq);
    pre1 = rms(audio, 0, 76800);
    check_tone(audio, 0, 76800, 192000);
    for (i = 0; i < 2*76800; ++i) rf1 += iq[i]*iq[i];
    dsp_capture(0, 1.0, audio, iq);
    post1 = rms(audio, 0, 76800);
    check_tone(audio, 0, 76800, 192000);
    for (i = 0; i < 2*76800; ++i) rf2 += iq[i]*iq[i];
    CHECK(fabs(rf1/rf2 - 1.0) < 0.001, "changing monitor tap must not change RF IQ");
    dsp_capture(1, 2.0, audio, iq);
    pre2 = rms(audio, 0, 76800);
    dsp_capture(0, 2.0, audio, iq);
    post2 = rms(audio, 0, 76800);
    CHECK(fabs(pre1 - sqrt(0.5)*0.1) < 0.001, "PRE uses raw input before DEXP");
    CHECK(fabs(pre2/pre1 - 1.0) < 0.001, "PRE ignores MIC gain");
    CHECK(fabs(post2/post1 - 2.0) < 0.03, "POST reflects MIC gain");
    for (mode = TXA_FM; mode <= TXA_AM; ++mode) {
        SetTXAMode(0, mode);
        dsp_capture(0, 1.0, audio, iq);
        check_tone(audio, 0, 76800, 192000);
    }
    SetTXAMode(0, TXA_USB);
    SetTXAPostGenMode(0, 0);
    SetTXAPostGenToneFreq(0, 1000.0);
    SetTXAPostGenRun(0, 1);
    dsp_capture(0, 1.0, audio, iq);
    check_tone(audio, 0, 76800, 192000);
    SetTXAPostGenRun(0, 0);
    puts("  WDSP gain, AM/FM and TUNE checks complete");
    SetTXAMode(0, TXA_CWU);
    dsp_capture(1, 1.0, audio, iq);
    CHECK(rms(audio, 0, 76800) == 0, "CW does not monitor the microphone");
    // No audio worker may retain old samples when the channel stops.
    SetChannelState(0, 0, 0);
    for (i = 0; i < 20; ++i) fexchange0WithMonitor(0, input, out, input, mon, &error);
    CHECK(rms(mon, 0, 256) == 0, "stopped TX monitor is silent");
    puts("  WDSP stopped; checking DSP block resize");
    // Keep DSP blocks within the FIR coefficient count established when
    // opening the channel, as the application does (nc >= dsp_size).
    SetDSPBuffsize(0, 512);
    SetDSPBuffsize(0, 4096);
    puts("  WDSP checking DSP sample rate");
    SetDSPSamplerate(0, 48000);
    puts("  WDSP checking output sample rates");
    SetOutputSamplerate(0, 96000);
    SetOutputSamplerate(0, 192000);
    puts("  WDSP checking input sample rates");
    SetInputSamplerate(0, 96000);
    SetInputSamplerate(0, 48000);
    SetTXAMode(0, TXA_USB);
    SetChannelState(0, 1, 0);
    puts("  WDSP checking audio after reconfiguration");
    dsp_capture(1, 1.0, audio, iq);
    CHECK(fabs(rms(audio, 0, 76800) - pre1) < 0.001, "PRE after DSP/rate reconfiguration");
    dsp_capture(0, 1.0, audio, iq);
    check_tone(audio, 0, 76800, 192000);
    CloseChannel(0);
    free(iq); free(audio);
    printf("PASS WDSP: PRE %.6f -> %.6f, POST %.6f -> %.6f; RF unchanged; AM/FM/TUNE; CW/stop mute; DSP/rate changes\n",
        pre1, pre2, post1, post2);
}

static void cw_sidetone(void)
{
    double iq[512] = {0}, monitor[512], output[512];
    int i;
    create_sidetone(0, 0, 0, 192000, 256, iq, output, iq, 0, 1000, 0.5, 1, 20, 0, 0.005);
    setSidetoneRate(0, 192000); // normal radio initialization builds oscillator/edges
    tone(monitor, 256, 0, 192000, 0.1);
    xsidetone(0, monitor);
    CHECK(memcmp(monitor, output, sizeof(output)) == 0, "voice passes the selected tap");
    SetSidetoneRun(0, 1);
    SetSidetoneSelectKey(0, 0);
    keySidetone(0, 0, 1);
    for (i = 0; i < 20; ++i) xsidetone(0, monitor);
    CHECK(fabs(output[0]) + fabs(output[1]) > 0.01, "keyed software sidetone replaces microphone monitor");
    keySidetone(0, 0, 0);
    for (i = 0; i < 20; ++i) xsidetone(0, monitor);
    CHECK(rms(output, 0, 256) == 0, "key-up sidetone is silent");
    CHECK(rms(iq, 0, 256) == 0, "monitor sidetone does not change RF IQ");
    destroy_sidetone(0);
    puts("PASS software CW sidetone and voice tap routing");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(1, report_crash);
    mixer_blocks(48000, 96);
    mixer_blocks(96000, 96);
    mixer_blocks(192000, 96);
    mixer_blocks(192000, 64); // legacy integer-ratio geometry
    vac_routes();
    cw_sidetone();
    dsp_taps();
    puts("All audio monitor tests passed.");
    return 0;
}

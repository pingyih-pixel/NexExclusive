// player.exe - a deliberately Chromium-shaped WASAPI render loop.
//
// Its only job is to be a stand-in for the NetEase client so nexsink.dll can be
// validated without touching the user's running app. It does what
// wasapi_audio_output_stream does:
//
//   GetMixFormat -> Initialize(SHARED, AUTOCONVERTPCM|SRC_DEFAULT_QUALITY)
//   -> GetBufferSize -> SetEventHandle -> GetService(IAudioRenderClient)
//   -> Start -> wait(event) -> GetCurrentPadding -> GetBuffer -> fill -> ReleaseBuffer
//
// If the sink can drive this loop without stalling, it can drive the real client.
//
//   player.exe --rate 44100 --seconds 5 [--tone 440] [--amp 0.2]

#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const GUID IID_IAudioRenderClient_l =
    {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

int main(int argc, char **argv) {
    int rate = 44100, seconds = 5, tone = 440, delay = 0;
    double amp = 0.2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rate") && i + 1 < argc)         rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tone") && i + 1 < argc)    tone = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--amp") && i + 1 < argc)     amp = atof(argv[++i]);
        else if (!strcmp(argv[i], "--delay") && i + 1 < argc)   delay = atoi(argv[++i]);
    }
    if (delay > 0) {
        printf("pid %lu, waiting %d ms for injection...\n", (unsigned long)GetCurrentProcessId(), delay);
        fflush(stdout);
        Sleep((DWORD)delay);
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("CoInitialize 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDeviceEnumerator *en = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { printf("enumerator 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDevice *dev = NULL;
    hr = en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) { printf("endpoint 0x%08lX\n", (unsigned long)hr); return 1; }

    IAudioClient *ac = NULL;
    hr = dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) { printf("activate 0x%08lX\n", (unsigned long)hr); return 1; }

    WAVEFORMATEX *mix = NULL;
    ac->lpVtbl->GetMixFormat(ac, &mix);
    if (mix)
        printf("device mix format: %lu Hz / %u ch / %u bit (tag 0x%04X)\n",
               (unsigned long)mix->nSamplesPerSec, mix->nChannels,
               mix->wBitsPerSample, mix->wFormatTag);

    // Chromium asks for its own rate in float32 and lets the engine convert.
    WAVEFORMATEXTENSIBLE we;
    memset(&we, 0, sizeof(we));
    we.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    we.Format.nChannels       = 2;
    we.Format.nSamplesPerSec  = (DWORD)rate;
    we.Format.wBitsPerSample  = 32;
    we.Format.nBlockAlign     = 8;
    we.Format.nAvgBytesPerSec = (DWORD)rate * 8;
    we.Format.cbSize          = 22;
    we.Samples.wValidBitsPerSample = 32;
    we.dwChannelMask          = 3;
    we.SubFormat.Data1 = 0x00000003;          // IEEE float
    we.SubFormat.Data2 = 0x0000;
    we.SubFormat.Data3 = 0x0010;
    we.SubFormat.Data4[0] = 0x80; we.SubFormat.Data4[1] = 0x00;
    we.SubFormat.Data4[2] = 0x00; we.SubFormat.Data4[3] = 0xAA;
    we.SubFormat.Data4[4] = 0x00; we.SubFormat.Data4[5] = 0x38;
    we.SubFormat.Data4[6] = 0x9B; we.SubFormat.Data4[7] = 0x71;

    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = ac->lpVtbl->Initialize(ac, AUDCLNT_SHAREMODE_SHARED, flags,
                                300000, 0, (WAVEFORMATEX *)&we, NULL);
    printf("Initialize(shared, %d Hz float32) -> 0x%08lX\n", rate, (unsigned long)hr);
    if (FAILED(hr)) return 1;

    UINT32 buf_frames = 0;
    ac->lpVtbl->GetBufferSize(ac, &buf_frames);
    printf("buffer size: %lu frames (%.1f ms)\n", (unsigned long)buf_frames,
           buf_frames * 1000.0 / rate);

    HANDLE evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    ac->lpVtbl->SetEventHandle(ac, evt);

    IAudioRenderClient *rc = NULL;
    hr = ac->lpVtbl->GetService(ac, &IID_IAudioRenderClient_l, (void **)&rc);
    printf("GetService(IAudioRenderClient) -> 0x%08lX %s\n", (unsigned long)hr,
           rc ? "ok" : "NULL");
    if (!rc) return 1;

    REFERENCE_TIME lat = 0;
    ac->lpVtbl->GetStreamLatency(ac, &lat);
    printf("stream latency: %.2f ms\n", lat / 10000.0);

    hr = ac->lpVtbl->Start(ac);
    printf("Start -> 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    double phase = 0.0, dphase = 2.0 * 3.14159265358979323846 * tone / rate;
    UINT64 total = 0;
    int stalls = 0;

    for (;;) {
        QueryPerformanceCounter(&t1);
        double elapsed = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        if (elapsed >= seconds) break;

        DWORD w = WaitForSingleObject(evt, 500);
        if (w != WAIT_OBJECT_0) { if (++stalls > 8) { printf("no event, giving up\n"); break; } }

        UINT32 pad = 0;
        if (FAILED(ac->lpVtbl->GetCurrentPadding(ac, &pad))) { printf("padding failed\n"); break; }
        UINT32 n = buf_frames > pad ? buf_frames - pad : 0;
        if (!n) continue;

        BYTE *p = NULL;
        if (FAILED(rc->lpVtbl->GetBuffer(rc, n, &p)) || !p) { printf("GetBuffer failed\n"); break; }
        float *s = (float *)p;
        for (UINT32 i = 0; i < n; i++) {
            float v = (float)(amp * sin(phase));
            phase += dphase;
            if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
            s[i * 2 + 0] = v;
            s[i * 2 + 1] = v;
        }
        rc->lpVtbl->ReleaseBuffer(rc, n, 0);
        total += n;
    }

    QueryPerformanceCounter(&t1);
    double elapsed = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    printf("wrote %llu frames in %.3f s  ->  effective %.1f Hz\n",
           (unsigned long long)total, elapsed, total / (elapsed > 0 ? elapsed : 1));

    ac->lpVtbl->Stop(ac);
    ac->lpVtbl->Reset(ac);
    rc->lpVtbl->Release(rc);
    ac->lpVtbl->Release(ac);
    dev->lpVtbl->Release(dev);
    en->lpVtbl->Release(en);
    CoUninitialize();
    printf("done\n");
    return 0;
}

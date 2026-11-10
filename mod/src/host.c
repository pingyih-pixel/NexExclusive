// Controlled WASAPI test host.
//
// Loads an optional hook DLL, then exercises the exact call sequence a player
// uses: enumerate -> activate -> GetMixFormat -> Initialize. Printing what it
// observes is how we prove a vtable patch actually took effect.
//
//   host.exe            baseline, no hook
//   host.exe hook.dll   with hook loaded first

#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>

static void print_fmt(const char *tag, const WAVEFORMATEX *w) {
    if (!w) { printf("  %s: (null)\n", tag); return; }
    printf("  %s: tag=0x%04X rate=%lu container=%u valid=%u ch=%u\n",
           tag, w->wFormatTag, (unsigned long)w->nSamplesPerSec,
           w->wBitsPerSample,
           (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
               ? ((const WAVEFORMATEXTENSIBLE *)w)->Samples.wValidBitsPerSample
               : w->wBitsPerSample,
           w->nChannels);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        HMODULE h = LoadLibraryA(argv[1]);
        printf("hook load: %s -> %p\n", argv[1], (void *)h);
        Sleep(200);
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    printf("CoInitialize 0x%08lX\n", (unsigned long)hr);

    IMMDeviceEnumerator *enumr = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumr);
    if (FAILED(hr)) { printf("enumerator failed 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDevice *dev = NULL;
    hr = enumr->lpVtbl->GetDefaultAudioEndpoint(enumr, eRender, eConsole, &dev);
    if (FAILED(hr)) { printf("endpoint failed 0x%08lX\n", (unsigned long)hr); return 1; }

    IAudioClient *client = NULL;
    hr = dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL,
                               (void **)&client);
    if (FAILED(hr)) { printf("activate failed 0x%08lX\n", (unsigned long)hr); return 1; }
    printf("vtable %p\n", *(void ***)client);

    WAVEFORMATEX *mix = NULL;
    hr = client->lpVtbl->GetMixFormat(client, &mix);
    printf("GetMixFormat 0x%08lX\n", (unsigned long)hr);
    print_fmt("mix", mix);

    if (mix) {
        REFERENCE_TIME dur = 10000000;
        hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0,
                                        dur, 0, mix, NULL);
        printf("Initialize(SHARED) 0x%08lX\n", (unsigned long)hr);
        CoTaskMemFree(mix);
    }

    client->lpVtbl->Release(client);
    dev->lpVtbl->Release(dev);
    enumr->lpVtbl->Release(enumr);
    CoUninitialize();
    return 0;
}

/* NexAudioHost.exe - optional out-of-process exclusive renderer.
   It owns the WASAPI stream and consumes the PCM ring published by nexsink.dll. */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include "nexaudiolink.h"

static const wchar_t *arg_value(int argc, wchar_t **argv, const wchar_t *name) {
    for (int i = 1; i + 1 < argc; i++)
        if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}

static unsigned long long hash_bytes(const unsigned char *p, size_t n,
                                     unsigned long long hash) {
    for (size_t i = 0; i < n; i++) hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
}

static REFERENCE_TIME PeriodForFrames(UINT32 frames, UINT32 sample_rate) {
    if (!frames || !sample_rate) return 200000;
    return (REFERENCE_TIME)(((UINT64)frames * 10000000ull + sample_rate / 2u) /
                            sample_rate);
}

static void fail_link(NexAudioIpc *ipc, HRESULT hr) {
    if (!ipc) return;
    InterlockedExchange((volatile LONG *)&ipc->error, (LONG)hr);
    InterlockedExchange((volatile LONG *)&ipc->state, NEX_AUDIO_FAILED);
}

static IMMDevice *open_device(NexAudioIpc *ipc) {
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr) || !enumerator) return NULL;
    if (ipc->endpoint_id[0])
        hr = IMMDeviceEnumerator_GetDevice(enumerator, ipc->endpoint_id, &device);
    else
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    IMMDeviceEnumerator_Release(enumerator);
    return SUCCEEDED(hr) ? device : NULL;
}

static void fill_format(const NexAudioIpc *ipc, WAVEFORMATEXTENSIBLE *fmt) {
    ZeroMemory(fmt, sizeof(*fmt));
    fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt->Format.nChannels = (WORD)ipc->channels;
    fmt->Format.nSamplesPerSec = ipc->rate;
    fmt->Format.wBitsPerSample = (WORD)ipc->container_bits;
    fmt->Format.nBlockAlign = (WORD)ipc->block;
    fmt->Format.nAvgBytesPerSec = ipc->rate * ipc->block;
    fmt->Format.cbSize = 22;
    fmt->Samples.wValidBitsPerSample = (WORD)ipc->valid_bits;
    fmt->dwChannelMask = ipc->channel_mask;
    fmt->SubFormat = ipc->is_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                   : KSDATAFORMAT_SUBTYPE_PCM;
}

static int consume(NexAudioIpc *ipc, IAudioRenderClient *render,
                   IAudioClient *client, UINT32 device_frames,
                   unsigned long long *endpoint_hash) {
    long long read = InterlockedCompareExchange64((volatile LONG64 *)&ipc->read_pos, 0, 0);
    long long write = InterlockedCompareExchange64((volatile LONG64 *)&ipc->write_pos, 0, 0);
    unsigned long long available = write > read ? (unsigned long long)(write - read) : 0;
    if (!available) return 0;
    UINT32 padding = 0;
    if (FAILED(IAudioClient_GetCurrentPadding(client, &padding)) || padding >= device_frames)
        return 0;
    UINT32 space = device_frames - padding;
    UINT32 frames = available > space ? space : (UINT32)available;
    if (frames > 1024) frames = 1024;
    BYTE *dst = NULL;
    HRESULT hr = IAudioRenderClient_GetBuffer(render, frames, &dst);
    if (FAILED(hr) || !dst) {
        fail_link(ipc, FAILED(hr) ? hr : E_POINTER);
        return -1;
    }
    size_t total = (size_t)frames * ipc->block;
    size_t start = (size_t)(read % (long long)ipc->capacity) * ipc->block;
    size_t ring_bytes = (size_t)ipc->capacity * ipc->block;
    size_t first = ring_bytes - start;
    if (first > total) first = total;
    memcpy(dst, ipc->data + start, first);
    if (first < total) memcpy(dst + first, ipc->data, total - first);
    unsigned long long next_hash = hash_bytes(dst, total, *endpoint_hash);
    hr = IAudioRenderClient_ReleaseBuffer(render, frames, 0);
    if (FAILED(hr)) {
        fail_link(ipc, hr);
        return -1;
    }
    *endpoint_hash = next_hash;
    InterlockedExchange64((volatile LONG64 *)&ipc->read_pos, read + frames);
    InterlockedExchange64((volatile LONG64 *)&ipc->endpoint_frames,
                          (LONG64)(ipc->endpoint_frames + frames));
    InterlockedExchange64((volatile LONG64 *)&ipc->endpoint_hash, (LONG64)*endpoint_hash);
    return 1;
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *map_name = arg_value(argc, argv, L"--map");
    const wchar_t *data_name = arg_value(argc, argv, L"--data");
    const wchar_t *space_name = arg_value(argc, argv, L"--space");
    const wchar_t *ready_name = arg_value(argc, argv, L"--ready");
    if (!map_name || !data_name || !space_name || !ready_name) return 2;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 3;

    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, map_name);
    HANDLE data_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, data_name);
    HANDLE space_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, space_name);
    HANDLE ready_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, ready_name);
    NexAudioIpc *ipc = map ? (NexAudioIpc *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NexAudioIpc)) : NULL;
    if (!ipc || !data_event || !space_event || !ready_event || ipc->magic != NEX_AUDIO_MAGIC) {
        if (ipc) UnmapViewOfFile(ipc);
        if (map) CloseHandle(map);
        if (data_event) CloseHandle(data_event);
        if (space_event) CloseHandle(space_event);
        if (ready_event) CloseHandle(ready_event);
        CoUninitialize();
        return 4;
    }

    IMMDevice *device = open_device(ipc);
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    WAVEFORMATEXTENSIBLE fmt;
    fill_format(ipc, &fmt);
    HANDLE audio_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    HRESULT hr = E_FAIL;
    REFERENCE_TIME period = 1000000;
    for (int attempt = 0; device && attempt < 8; attempt++) {
        if (client) {
            IAudioClient_Release(client);
            client = NULL;
        }
        hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                NULL, (void **)&client);
        if (FAILED(hr) || !client) break;
        hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     period, period, &fmt.Format, NULL);
        if (hr != AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) break;

        UINT32 aligned = 0;
        HRESULT size_hr = IAudioClient_GetBufferSize(client, &aligned);
        IAudioClient_Release(client);
        client = NULL;
        if (FAILED(size_hr) || !aligned) {
            hr = FAILED(size_hr) ? size_hr : AUDCLNT_E_BUFFER_SIZE_ERROR;
            break;
        }
        period = PeriodForFrames(aligned, ipc->rate);
    }
    UINT32 device_frames = 0;
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(client, &device_frames);
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(client, audio_event);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(client, &IID_IAudioRenderClient,
                                                      (void **)&render);
    if (FAILED(hr) || !render || !device_frames) {
        fail_link(ipc, FAILED(hr) ? hr : E_FAIL);
        if (render) IAudioRenderClient_Release(render);
        if (client) IAudioClient_Release(client);
        if (device) IMMDevice_Release(device);
        if (audio_event) CloseHandle(audio_event);
        if (ready_event) SetEvent(ready_event);
        UnmapViewOfFile(ipc); CloseHandle(map); CloseHandle(data_event); CloseHandle(space_event); CloseHandle(ready_event);
        CoUninitialize();
        return 5;
    }

    hr = IAudioClient_Start(client);
    if (FAILED(hr)) {
        fail_link(ipc, hr);
        SetEvent(ready_event);
    } else {
        InterlockedExchange((volatile LONG *)&ipc->state, NEX_AUDIO_READY);
        SetEvent(ready_event);
        unsigned long long endpoint_hash = 1469598103934665603ull;
        HANDLE waits[2] = { data_event, audio_event };
        while (InterlockedCompareExchange((volatile LONG *)&ipc->state, 0, 0) != NEX_AUDIO_CLOSING) {
            DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 100);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) {
                for (;;) {
                    int result = consume(ipc, render, client, device_frames, &endpoint_hash);
                    if (result <= 0) {
                        if (result == 0 && wait == WAIT_OBJECT_0 + 1)
                            InterlockedIncrement64((volatile LONG64 *)&ipc->underrun_events);
                        if (result < 0) {
                            fail_link(ipc, E_FAIL);
                            break;
                        }
                        break;
                    }
                    SetEvent(space_event);
                }
            }
        }
        IAudioClient_Stop(client);
    }
    if (render) IAudioRenderClient_Release(render);
    if (client) IAudioClient_Release(client);
    if (device) IMMDevice_Release(device);
    if (audio_event) CloseHandle(audio_event);
    CloseHandle(ready_event);
    UnmapViewOfFile(ipc); CloseHandle(map); CloseHandle(data_event); CloseHandle(space_event);
    CoUninitialize();
    return 0;
}

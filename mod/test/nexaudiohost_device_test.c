/* Opt-in real-endpoint integration test. It submits digital silence, never a
   test tone, and is intentionally excluded from the device-free test target. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include "../tools/nexaudiolink.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

static WCHAR *find_endpoint_id(const wchar_t *friendly_part) {
    if (!friendly_part || !*friendly_part) return NULL;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDeviceCollection *devices = NULL;
    WCHAR *answer = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr) || !enumerator) return NULL;
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eRender,
                                                DEVICE_STATE_ACTIVE, &devices);
    UINT count = 0;
    if (SUCCEEDED(hr) && devices) IMMDeviceCollection_GetCount(devices, &count);
    for (UINT i = 0; i < count && !answer; i++) {
        IMMDevice *device = NULL;
        IPropertyStore *store = NULL;
        PROPVARIANT name;
        PropVariantInit(&name);
        WCHAR *id = NULL;
        if (FAILED(IMMDeviceCollection_Item(devices, i, &device)) || !device) continue;
        if (SUCCEEDED(IMMDevice_GetId(device, &id)) && id &&
            SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store)) && store &&
            SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR && name.pwszVal && wcsstr(name.pwszVal, friendly_part)) {
            answer = id;
            id = NULL;
        }
        if (id) CoTaskMemFree(id);
        PropVariantClear(&name);
        if (store) IPropertyStore_Release(store);
        IMMDevice_Release(device);
    }
    if (devices) IMMDeviceCollection_Release(devices);
    IMMDeviceEnumerator_Release(enumerator);
    return answer;
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *host = argc > 1 ? argv[1] : L"NexAudioHost.exe";
    HRESULT coinit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(coinit)) return 2;
    WCHAR *endpoint_id = find_endpoint_id(argc > 2 ? argv[2] : NULL);
    if (argc > 2 && !endpoint_id) {
        fprintf(stderr, "active endpoint not found\n");
        CoUninitialize();
        return 2;
    }
    DWORD pid = GetCurrentProcessId();
    WCHAR map_name[NEX_AUDIO_MAPPING_CHARS], pcm_name[NEX_AUDIO_MAPPING_CHARS];
    WCHAR data_name[NEX_AUDIO_MAPPING_CHARS], space_name[NEX_AUDIO_MAPPING_CHARS];
    WCHAR ready_name[NEX_AUDIO_MAPPING_CHARS];
    _snwprintf(map_name, NEX_AUDIO_MAPPING_CHARS, L"Local\\NexAudioDeviceMap_%lu", pid);
    _snwprintf(pcm_name, NEX_AUDIO_MAPPING_CHARS, L"Local\\NexAudioDevicePcm_%lu", pid);
    _snwprintf(data_name, NEX_AUDIO_MAPPING_CHARS, L"Local\\NexAudioDeviceData_%lu", pid);
    _snwprintf(space_name, NEX_AUDIO_MAPPING_CHARS, L"Local\\NexAudioDeviceSpace_%lu", pid);
    _snwprintf(ready_name, NEX_AUDIO_MAPPING_CHARS, L"Local\\NexAudioDeviceReady_%lu", pid);
    const UINT32 rate = 48000, channels = 2, frame_bytes = 6, capacity = rate / 2;
    const UINT64 data_bytes = (UINT64)capacity * frame_bytes;
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, NEX_AUDIO_CONTROL_BYTES, map_name);
    HANDLE pcm = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, (DWORD)data_bytes, pcm_name);
    HANDLE data_event = CreateEventW(NULL, FALSE, FALSE, data_name);
    HANDLE space_event = CreateEventW(NULL, FALSE, FALSE, space_name);
    HANDLE ready_event = CreateEventW(NULL, FALSE, FALSE, ready_name);
    NexAudioControl *c = map ? (NexAudioControl *)MapViewOfFile(map,
        FILE_MAP_ALL_ACCESS, 0, 0, NEX_AUDIO_CONTROL_BYTES) : NULL;
    unsigned char *data = pcm ? (unsigned char *)MapViewOfFile(pcm,
        FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)data_bytes) : NULL;
    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    ZeroMemory(&pi, sizeof(pi));
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    int started_process = 0;
    int result = 1;
    if (!c || !data || !data_event || !space_event || !ready_event) {
        fprintf(stderr, "mapping/event creation failed %lu\n", GetLastError());
        goto done;
    }
    nex_audio_control_init(c, 1);
    c->input_rate = c->output_rate = rate;
    c->input_channels = c->output_channels = channels;
    c->input_container_bits = c->output_container_bits = 24;
    c->input_valid_bits = c->output_valid_bits = 24;
    c->input_encoding = c->output_encoding = NEX_AUDIO_ENCODING_PCM;
    c->input_channel_mask = c->output_channel_mask = 3;
    c->output_frame_bytes = frame_bytes;
    c->capacity_frames = capacity;
    c->data_bytes = data_bytes;
    c->endpoint_hash = 1469598103934665603ull;
    if (endpoint_id) wcscpy(c->target_id, endpoint_id);
    wcscpy(c->data_mapping_name, pcm_name);
    WCHAR command[2048];
    _snwprintf(command, sizeof(command) / sizeof(command[0]),
               L"\"%ls\" --map \"%ls\" --pcm \"%ls\" --data \"%ls\" --space \"%ls\" --ready \"%ls\"",
               host, map_name, pcm_name, data_name, space_name, ready_name);
    if (!CreateProcessW(host, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        fprintf(stderr, "CreateProcess failed %lu\n", GetLastError());
        goto done;
    }
    started_process = 1;
    HANDLE waits[2] = {ready_event, pi.hProcess};
    DWORD ready = WaitForMultipleObjects(2, waits, FALSE, 6000);
    CHECK(ready == WAIT_OBJECT_0 && c->state == NEX_AUDIO_READY);
    if (ready != WAIT_OBJECT_0 || c->state != NEX_AUDIO_READY) {
        fprintf(stderr, "ready failed state=%s reason=%s hr=0x%08lX\n",
                nex_audio_state_name(c->state), nex_audio_fallback_name(c->fallback_reason),
                (unsigned long)c->error);
        goto done;
    }

    unsigned char zeros[512 * 6];
    memset(zeros, 0, sizeof(zeros));
    const UINT32 total = rate * 2; /* two seconds of digital silence */
    UINT32 produced = 0;
    DWORD deadline = GetTickCount() + 6000;
    while (produced < total && (LONG)(GetTickCount() - deadline) < 0) {
        UINT32 chunk = total - produced;
        if (chunk > 512) chunk = 512;
        if (nex_audio_ring_write(c, data, zeros, chunk) == chunk) {
            produced += chunk;
            InterlockedExchangeAdd64((volatile LONG64 *)&c->converted_frames, chunk);
            SetEvent(data_event);
        } else {
            WaitForSingleObject(space_event, 20);
        }
        if (c->state == NEX_AUDIO_FAILED) break;
    }
    CHECK(produced == total);
    CHECK(c->first_buffer_submitted == 1);
    CHECK(c->device_started == 1);
    InterlockedExchange64((volatile LONG64 *)&c->eos_submitted_generation,
                          c->stream_generation);
    SetEvent(data_event);
    deadline = GetTickCount() + 7000;
    while (nex_audio_read64((volatile UINT64 *)&c->eos_complete_generation) !=
           c->stream_generation && (LONG)(GetTickCount() - deadline) < 0 &&
           c->state != NEX_AUDIO_FAILED)
        Sleep(10);
    CHECK(nex_audio_read64((volatile UINT64 *)&c->eos_complete_generation) ==
          c->stream_generation);
    CHECK(c->state == NEX_AUDIO_COMPLETE);
    CHECK(nex_audio_read64(&c->endpoint_frames) == total);
    CHECK(nex_audio_read64((volatile UINT64 *)&c->read_frame) == total);
    CHECK(nex_audio_read64((volatile UINT64 *)&c->write_frame) == total);
    CHECK(nex_audio_read64(&c->getbuffer_failures) == 0);
    CHECK(nex_audio_read64(&c->releasebuffer_failures) == 0);
    CHECK(nex_audio_read64(&c->dropped_frames) == 0);
    if (!failures) {
        printf("PASS NexAudioHost device: ready=%lu first=%u started=%u endpoint=%llu "
               "eos=%llu underrun=%llu backpressure=%llu\n",
               (unsigned long)c->device_frames, c->first_buffer_submitted,
               c->device_started, (unsigned long long)c->endpoint_frames,
               (unsigned long long)c->eos_complete_generation,
               (unsigned long long)c->underrun_events,
               (unsigned long long)c->backpressure_events);
        result = 0;
    }

done:
    if (c) {
        InterlockedExchange((volatile LONG *)&c->state, NEX_AUDIO_CLOSING);
        SetEvent(data_event);
    }
    if (started_process) {
        if (WaitForSingleObject(pi.hProcess, 2000) != WAIT_OBJECT_0)
            TerminateProcess(pi.hProcess, 1);
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        if (result == 0 && exit_code != 0) {
            fprintf(stderr, "host exit code %lu\n", (unsigned long)exit_code);
            result = 1;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (c) UnmapViewOfFile(c);
    if (data) UnmapViewOfFile(data);
    if (map) CloseHandle(map);
    if (pcm) CloseHandle(pcm);
    if (data_event) CloseHandle(data_event);
    if (space_event) CloseHandle(space_event);
    if (ready_event) CloseHandle(ready_event);
    if (endpoint_id) CoTaskMemFree(endpoint_id);
    CoUninitialize();
    return result;
}

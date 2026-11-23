/* NexAudioHost.exe - v3 out-of-process WASAPI exclusive renderer.
   The hook negotiates and converts PCM; this process owns only the negotiated
   output format, endpoint submission and device-clock completion. */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <avrt.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdarg.h>
#include "nexaudiolink.h"

#define NEX_AUDIO_EOS_TIMEOUT_MS 5000u

typedef struct HostRuntime HostRuntime;
typedef struct HostApi {
    HRESULT (*get_padding)(HostRuntime *, UINT32 *);
    HRESULT (*get_buffer)(HostRuntime *, UINT32, BYTE **);
    HRESULT (*release_buffer)(HostRuntime *, UINT32, DWORD);
    HRESULT (*start)(HostRuntime *);
    HRESULT (*stop)(HostRuntime *);
    HRESULT (*get_position)(HostRuntime *, UINT64 *, UINT64 *);
    HRESULT (*reset)(HostRuntime *);
} HostApi;

struct HostRuntime {
    NexAudioControl *control;
    unsigned char *data;
    IAudioClient *client;
    IAudioRenderClient *render;
    IAudioClock *clock;
    uint64_t clock_frequency;
    HANDLE space_event;
    const HostApi *api;
    uint64_t endpoint_hash;
    uint64_t eos_target;
    DWORD eos_since;
    int eos_checking;
};

static wchar_t g_host_log_path[MAX_PATH];

static void host_log(const char *format, ...) {
    if (!g_host_log_path[0]) return;
    FILE *fp = _wfopen(g_host_log_path, L"a");
    if (!fp) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(fp, "%02u:%02u:%02u.%03u [%lu] ", now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds, (unsigned long)GetCurrentProcessId());
    va_list args;
    va_start(args, format);
    vfprintf(fp, format, args);
    va_end(args);
    fputc('\n', fp);
    fclose(fp);
}

#ifndef NEX_AUDIO_HOST_NO_MAIN
static const wchar_t *arg_value(int argc, wchar_t **argv, const wchar_t *name) {
    for (int i = 1; i + 1 < argc; i++)
        if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}
#endif

static uint64_t hash_bytes(const unsigned char *p, size_t n, uint64_t hash) {
    for (size_t i = 0; i < n; i++) hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
}

static void add_counter(volatile uint64_t *counter, uint64_t value) {
    InterlockedExchangeAdd64((volatile LONG64 *)counter, (LONG64)value);
}

static long host_state(HostRuntime *rt) {
    return InterlockedCompareExchange((volatile LONG *)&rt->control->state, 0, 0);
}

static void host_set_state(HostRuntime *rt, long state) {
    InterlockedExchange((volatile LONG *)&rt->control->state, state);
}

static HRESULT host_fail(HostRuntime *rt, HRESULT hr, uint32_t reason) {
    if (SUCCEEDED(hr)) hr = E_FAIL;
    InterlockedExchange((volatile LONG *)&rt->control->error, (LONG)hr);
    rt->control->fallback_reason = reason;
    host_set_state(rt, NEX_AUDIO_FAILED);
    host_log("failed hr=0x%08lX reason=%s state=%s", (unsigned long)hr,
             nex_audio_fallback_name(reason), nex_audio_state_name(host_state(rt)));
    return hr;
}

static int host_eos_requested(HostRuntime *rt) {
    uint64_t generation = rt->control->stream_generation;
    return generation && nex_audio_read64((volatile uint64_t *)
        &rt->control->eos_submitted_generation) == generation;
}

static HRESULT host_complete_eos(HostRuntime *rt) {
    uint64_t generation = rt->control->stream_generation;
    if (!host_eos_requested(rt)) return S_FALSE;
    InterlockedExchange64((volatile LONG64 *)&rt->control->eos_complete_generation,
                          (LONG64)generation);
    host_set_state(rt, NEX_AUDIO_COMPLETE);
    host_log("eos complete generation=%llu endpoint_frames=%llu",
             (unsigned long long)generation,
             (unsigned long long)nex_audio_read64(&rt->control->endpoint_frames));
    return S_OK;
}

static HRESULT host_check_eos(HostRuntime *rt) {
    if (!host_eos_requested(rt)) return S_FALSE;
    long state = host_state(rt);
    if (state != NEX_AUDIO_COMPLETE) host_set_state(rt, NEX_AUDIO_DRAINING);
    if (nex_audio_ring_available(rt->control)) return S_FALSE;
    if (!rt->control->device_started) return host_complete_eos(rt);

    UINT64 position = 0, qpc = 0;
    UINT32 padding = 0;
    HRESULT hr = rt->api->get_position(rt, &position, &qpc);
    if (FAILED(hr)) return host_fail(rt, hr, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    InterlockedExchange64((volatile LONG64 *)&rt->control->device_clock_position,
                          (LONG64)position);
    InterlockedExchange64((volatile LONG64 *)&rt->control->device_clock_qpc,
                          (LONG64)qpc);
    rt->control->device_clock_frequency = rt->clock_frequency;
    hr = rt->api->get_padding(rt, &padding);
    if (FAILED(hr)) return host_fail(rt, hr, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    if (!rt->eos_checking) {
        /* position is an IAudioClock tick count while padding is measured in
           output frames. Convert before adding; the two units only coincide
           when the device clock frequency equals the sample rate. */
        uint64_t frequency = rt->clock_frequency;
        uint64_t rate = rt->control->output_rate;
        uint64_t padding_ticks = (frequency && rate)
            ? ((uint64_t)padding * frequency + rate - 1) / rate
            : padding;
        rt->eos_target = position + padding_ticks;
        rt->eos_since = GetTickCount();
        rt->eos_checking = 1;
        host_log("eos draining generation=%llu target=%llu position=%llu padding=%lu",
                 (unsigned long long)rt->control->stream_generation,
                 (unsigned long long)rt->eos_target,
                 (unsigned long long)position, (unsigned long)padding);
    }
    if (position >= rt->eos_target) return host_complete_eos(rt);
    if (GetTickCount() - rt->eos_since > NEX_AUDIO_EOS_TIMEOUT_MS)
        return host_fail(rt, HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                         NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    return S_FALSE;
}

/* Set NEX_HOST_DUMP=<file> in the client's environment (the Host inherits it)
   to capture exactly the PCM the Host hands to the device, for a byte-level
   comparison with the DLL's NEX_DUMP capture (tools/compare_pcm.py). Start-up
   silence is not written: it is not client audio. Diagnostic only - the
   writes are buffered, but they are still on the render thread. */
static FILE *g_host_dump;
static int   g_host_dump_state;     /* 0 = not tried, 1 = open, -1 = off */

static void host_dump(const BYTE *p, size_t bytes) {
    if (!g_host_dump_state) {
        g_host_dump_state = -1;
        const wchar_t *path = _wgetenv(L"NEX_HOST_DUMP");
        if (path && *path) {
            g_host_dump = _wfopen(path, L"wb");
            if (g_host_dump) {
                setvbuf(g_host_dump, NULL, _IOFBF, 1u << 20);
                g_host_dump_state = 1;
                host_log("dump: device frames -> %ls", path);
            } else {
                host_log("dump: cannot open %ls", path);
            }
        }
    }
    if (g_host_dump_state == 1 && bytes) fwrite(p, 1, bytes, g_host_dump);
}

static HRESULT host_submit(HostRuntime *rt, uint32_t frames) {
    BYTE *destination = NULL;
    HRESULT hr = rt->api->get_buffer(rt, frames, &destination);
    if (FAILED(hr) || !destination) {
        add_counter(&rt->control->getbuffer_failures, 1);
        return host_fail(rt, FAILED(hr) ? hr : E_POINTER,
                         NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    }
    if (nex_audio_ring_peek(rt->control, rt->data, destination, frames) != frames) {
        rt->api->release_buffer(rt, 0, 0);
        return host_fail(rt, E_UNEXPECTED, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    }
    uint64_t next_hash = hash_bytes(destination, (size_t)frames *
                                    rt->control->output_frame_bytes,
                                    rt->endpoint_hash);
    host_dump(destination, (size_t)frames * rt->control->output_frame_bytes);
    hr = rt->api->release_buffer(rt, frames, 0);
    if (FAILED(hr)) {
        add_counter(&rt->control->releasebuffer_failures, 1);
        return host_fail(rt, hr, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    }
    if (nex_audio_ring_commit_read(rt->control, frames) != frames)
        return host_fail(rt, E_UNEXPECTED, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    rt->endpoint_hash = next_hash;
    add_counter(&rt->control->endpoint_frames, frames);
    InterlockedExchange64((volatile LONG64 *)&rt->control->endpoint_hash,
                          (LONG64)next_hash);
    if (rt->space_event) SetEvent(rt->space_event);
    return S_OK;
}

/* Submit silence that is not taken from the ring (start-up prime). */
static HRESULT host_submit_silence(HostRuntime *rt, uint32_t frames) {
    BYTE *destination = NULL;
    HRESULT hr = rt->api->get_buffer(rt, frames, &destination);
    if (FAILED(hr) || !destination) {
        add_counter(&rt->control->getbuffer_failures, 1);
        return host_fail(rt, FAILED(hr) ? hr : E_POINTER, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    }
    hr = rt->api->release_buffer(rt, frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) {
        add_counter(&rt->control->releasebuffer_failures, 1);
        return host_fail(rt, hr, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    }
    uint32_t left = rt->control->prime_request_frames;
    InterlockedExchange((volatile LONG *)&rt->control->prime_request_frames,
                        (LONG)(frames >= left ? 0 : left - frames));
    add_counter(&rt->control->primed_frames_total, frames);
    return S_OK;
}

static HRESULT host_publish_clock(HostRuntime *rt, UINT32 padding) {
    UINT64 position = 0, qpc = 0;
    HRESULT hr = rt->api->get_position(rt, &position, &qpc);
    if (FAILED(hr)) return hr;
    InterlockedExchange64((volatile LONG64 *)&rt->control->device_clock_position,
                          (LONG64)position);
    InterlockedExchange64((volatile LONG64 *)&rt->control->device_clock_qpc,
                          (LONG64)qpc);
    rt->control->device_clock_frequency = rt->clock_frequency;
    InterlockedExchange((volatile LONG *)&rt->control->device_padding_frames,
                        (LONG)padding);
    return S_OK;
}

/* Apply the client's transport. Returns S_FALSE when this step is fully
   handled (a flush, or a pause), S_OK to continue with normal pumping, or a
   failure. Only the Host can stop or reset the device it owns; without this a
   client pause left the device running into underruns and a seek replayed the
   ring's pre-seek audio. */
static HRESULT host_apply_transport(HostRuntime *rt) {
    NexAudioControl *c = rt->control;
    uint64_t request = nex_audio_read64(&c->flush_request_generation);
    HRESULT hr;
    if (request != nex_audio_read64(&c->flush_ack_generation)) {
        if (c->device_started) {
            rt->api->stop(rt);
            rt->api->reset(rt);
        }
        InterlockedExchange64((volatile LONG64 *)&c->read_frame,
                              (LONG64)nex_audio_read64((volatile uint64_t *)&c->write_frame));
        c->device_started = 0;
        c->first_buffer_submitted = 0;
        InterlockedExchange((volatile LONG *)&c->device_paused, 0);
        InterlockedExchange((volatile LONG *)&c->device_padding_frames, 0);
        InterlockedExchange64((volatile LONG64 *)&c->primed_frames_total, 0);
        rt->eos_checking = 0;
        long state = host_state(rt);
        if (state == NEX_AUDIO_RUNNING || state == NEX_AUDIO_PREFILLED || state == NEX_AUDIO_DRAINING)
            host_set_state(rt, NEX_AUDIO_READY);
        InterlockedExchange64((volatile LONG64 *)&c->flush_ack_generation, (LONG64)request);
        if (rt->space_event) SetEvent(rt->space_event);
        host_log("flush generation=%llu: device reset, ring discarded", (unsigned long long)request);
        return S_FALSE;
    }
    if (c->transport == NEX_AUDIO_TRANSPORT_PAUSE) {
        if (c->device_started && !c->device_paused) {
            hr = rt->api->stop(rt);
            if (FAILED(hr)) return host_fail(rt, hr, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
            InterlockedExchange((volatile LONG *)&c->device_paused, 1);
            host_log("paused generation=%llu ring_used=%llu",
                     (unsigned long long)c->stream_generation,
                     (unsigned long long)nex_audio_ring_available(c));
        }
        return S_FALSE;
    }
    if (c->device_paused) {
        hr = rt->api->start(rt);
        if (FAILED(hr)) return host_fail(rt, hr, NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED);
        InterlockedExchange((volatile LONG *)&c->device_paused, 0);
        host_log("resumed generation=%llu", (unsigned long long)c->stream_generation);
    }
    return S_OK;
}

/* One bounded polling step. The mock host test drives this same function. */
static HRESULT host_pump(HostRuntime *rt) {
    NexAudioControl *control = rt->control;
    long state = host_state(rt);
    if (state == NEX_AUDIO_FAILED) return (HRESULT)control->error;
    if (state == NEX_AUDIO_CLOSING) return S_FALSE;
    HRESULT transport = host_apply_transport(rt);
    if (transport != S_OK) return transport;
    if (host_state(rt) == NEX_AUDIO_COMPLETE) return S_OK;

    uint64_t available64 = nex_audio_ring_available(control);
    if (!control->device_started && control->prime_request_frames &&
        available64 >= control->device_frames) {
        /* Fresh start with client audio ready: the first device buffer is
           silence, so a DAC waking up or relocking does not eat the opening. */
        HRESULT hr = host_submit_silence(rt, control->device_frames);
        if (FAILED(hr)) return hr;
        control->first_buffer_submitted = 1;
        host_set_state(rt, NEX_AUDIO_PREFILLED);
        hr = rt->api->start(rt);
        if (FAILED(hr)) return host_fail(rt, hr, NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED);
        control->device_started = 1;
        host_set_state(rt, NEX_AUDIO_RUNNING);
        hr = host_publish_clock(rt, control->device_frames);
        if (FAILED(hr)) return host_fail(rt, hr, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
        host_log("hardware started on silence generation=%llu prime=%lu more=%lu",
                 (unsigned long long)control->stream_generation,
                 (unsigned long)control->device_frames,
                 (unsigned long)control->prime_request_frames);
        return S_OK;
    }
    if (!control->device_started) {
        if (available64 < control->device_frames) {
            if (host_eos_requested(rt) && available64) {
                /* A natural EOS may leave a short final packet. Submit the
                   remaining frames as the first buffer instead of rejecting
                   the stream merely because it is shorter than the normal
                   device period. */
                HRESULT hr = host_submit(rt, (UINT32)available64);
                if (FAILED(hr)) return hr;
                control->first_buffer_submitted = 1;
                host_set_state(rt, NEX_AUDIO_PREFILLED);
                hr = rt->api->start(rt);
                if (FAILED(hr))
                    return host_fail(rt, hr, NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED);
                control->device_started = 1;
                host_set_state(rt, NEX_AUDIO_RUNNING);
                InterlockedExchange((volatile LONG *)&control->device_padding_frames,
                                    (LONG)available64);
                hr = host_publish_clock(rt, (UINT32)available64);
                if (FAILED(hr))
                    return host_fail(rt, hr, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
                return host_check_eos(rt);
            }
            add_counter(&control->source_waits, 1);
            return host_check_eos(rt);
        }
        HRESULT hr = host_submit(rt, control->device_frames);
        if (FAILED(hr)) return hr;
        control->first_buffer_submitted = 1;
        host_set_state(rt, NEX_AUDIO_PREFILLED);
        /* The flag and the PREFILLED state are visible before Start is called. */
        hr = rt->api->start(rt);
        if (FAILED(hr))
            return host_fail(rt, hr, NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED);
        control->device_started = 1;
        host_set_state(rt, NEX_AUDIO_RUNNING);
        hr = host_publish_clock(rt, control->device_frames);
        if (FAILED(hr))
            return host_fail(rt, hr, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
        host_log("hardware started generation=%llu first_buffer=%lu rate=%lu frame_bytes=%lu",
                 (unsigned long long)control->stream_generation,
                 (unsigned long)control->device_frames,
                 (unsigned long)control->output_rate,
                 (unsigned long)control->output_frame_bytes);
        return S_OK;
    }

    UINT32 padding = 0;
    HRESULT hr = rt->api->get_padding(rt, &padding);
    if (FAILED(hr)) return host_fail(rt, hr, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    if (padding > control->device_frames)
        return host_fail(rt, E_UNEXPECTED, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    hr = host_publish_clock(rt, padding);
    if (FAILED(hr)) return host_fail(rt, hr, NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST);
    UINT32 space = control->device_frames - padding;
    if (!space) return host_check_eos(rt);
    if (control->prime_request_frames) {
        /* The rest of a cross-family resync interval. */
        uint32_t left = control->prime_request_frames;
        return host_submit_silence(rt, left < space ? left : space);
    }
    if (!available64) {
        if (host_eos_requested(rt)) return host_check_eos(rt);
        /* Padding still contains audio, so this event is a source wait.  Only
           an empty running device is an underrun. */
        if (!padding) add_counter(&control->underrun_events, 1);
        return S_FALSE;
    }
    UINT32 frames = available64 > space ? space : (UINT32)available64;
    hr = host_submit(rt, frames);
    if (FAILED(hr)) return hr;
    return host_check_eos(rt);
}

static HRESULT real_get_padding(HostRuntime *rt, UINT32 *padding) {
    return IAudioClient_GetCurrentPadding(rt->client, padding);
}
static HRESULT real_get_buffer(HostRuntime *rt, UINT32 frames, BYTE **destination) {
    return IAudioRenderClient_GetBuffer(rt->render, frames, destination);
}
static HRESULT real_release_buffer(HostRuntime *rt, UINT32 frames, DWORD flags) {
    return IAudioRenderClient_ReleaseBuffer(rt->render, frames, flags);
}
static HRESULT real_start(HostRuntime *rt) { return IAudioClient_Start(rt->client); }
static HRESULT real_stop(HostRuntime *rt) { return IAudioClient_Stop(rt->client); }
static HRESULT real_reset(HostRuntime *rt) { return IAudioClient_Reset(rt->client); }
static HRESULT real_get_position(HostRuntime *rt, UINT64 *position, UINT64 *qpc) {
    return IAudioClock_GetPosition(rt->clock, position, qpc);
}

static const HostApi g_real_api = {
    real_get_padding, real_get_buffer, real_release_buffer,
    real_start, real_stop, real_get_position, real_reset
};

static uint32_t host_startup_reason(HRESULT hr) {
    return hr == AUDCLNT_E_UNSUPPORTED_FORMAT
         ? NEX_AUDIO_FALLBACK_UNSUPPORTED_FORMAT
         : NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED;
}

#ifndef NEX_AUDIO_HOST_NO_MAIN
static REFERENCE_TIME period_for_frames(UINT32 frames, UINT32 sample_rate) {
    if (!frames || !sample_rate) return 1000000;
    return (REFERENCE_TIME)(((UINT64)frames * 10000000ull + sample_rate / 2u) /
                            sample_rate);
}

static HRESULT open_device(NexAudioControl *control, IMMDevice **out) {
    *out = NULL;
    IMMDeviceEnumerator *enumerator = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr) || !enumerator) return FAILED(hr) ? hr : E_FAIL;
    if (control->target_id[0])
        hr = IMMDeviceEnumerator_GetDevice(enumerator, control->target_id, out);
    else
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, out);
    IMMDeviceEnumerator_Release(enumerator);
    return hr;
}

static void fill_output_format(const NexAudioControl *control,
                               WAVEFORMATEXTENSIBLE *format) {
    ZeroMemory(format, sizeof(*format));
    format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format->Format.nChannels = (WORD)control->output_channels;
    format->Format.nSamplesPerSec = control->output_rate;
    format->Format.wBitsPerSample = (WORD)control->output_container_bits;
    format->Format.nBlockAlign = (WORD)control->output_frame_bytes;
    format->Format.nAvgBytesPerSec = control->output_rate * control->output_frame_bytes;
    format->Format.cbSize = 22;
    format->Samples.wValidBitsPerSample = control->output_valid_bits;
    format->dwChannelMask = control->output_channel_mask;
    format->SubFormat = control->output_encoding == NEX_AUDIO_ENCODING_FLOAT
                      ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}
#endif

#ifndef NEX_AUDIO_HOST_NO_MAIN
int wmain(int argc, wchar_t **argv) {
    const wchar_t *map_name = arg_value(argc, argv, L"--map");
    const wchar_t *pcm_name = arg_value(argc, argv, L"--pcm");
    const wchar_t *data_name = arg_value(argc, argv, L"--data");
    const wchar_t *space_name = arg_value(argc, argv, L"--space");
    const wchar_t *ready_name = arg_value(argc, argv, L"--ready");
    if (!map_name || !pcm_name || !data_name || !space_name || !ready_name) return 2;

    GetModuleFileNameW(NULL, g_host_log_path, MAX_PATH);
    wchar_t *slash = wcsrchr(g_host_log_path, L'\\');
    if (slash) wcscpy(slash + 1, L"NexAudioHost.log");

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 3;
    int exit_code = 1;
    HANDLE map = NULL, pcm = NULL, data_event = NULL, space_event = NULL, ready_event = NULL;
    NexAudioControl *control = NULL;
    unsigned char *data = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    IAudioClock *clock = NULL;
    HostRuntime rt;
    ZeroMemory(&rt, sizeof(rt));
    rt.api = &g_real_api;
    rt.endpoint_hash = 1469598103934665603ull;
    int ready_signalled = 0;
    int mmss_active = 0;
    DWORD mmss_task = 0;
    HANDLE mmss = NULL;
    DWORD last_status = GetTickCount();

    map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, map_name);
    data_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, data_name);
    space_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, space_name);
    ready_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, ready_name);
    if (map) control = (NexAudioControl *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS,
                                                        0, 0, NEX_AUDIO_CONTROL_BYTES);
    if (!map || !control || !data_event || !space_event || !ready_event) goto done;
    rt.control = control;
    rt.space_event = space_event;

    uint32_t handshake = nex_audio_handshake_error(control);
    if (handshake != NEX_AUDIO_FALLBACK_NONE) {
        host_fail(&rt, E_FAIL, handshake);
        goto done;
    }
    if (wcscmp(control->data_mapping_name, pcm_name) != 0 ||
        !nex_audio_output_format_valid(control)) {
        host_fail(&rt, AUDCLNT_E_UNSUPPORTED_FORMAT,
                  NEX_AUDIO_FALLBACK_UNSUPPORTED_FORMAT);
        goto done;
    }
    pcm = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, pcm_name);
    if (pcm) data = (unsigned char *)MapViewOfFile(pcm, FILE_MAP_ALL_ACCESS, 0, 0,
                                                   (SIZE_T)control->data_bytes);
    if (!pcm || !data) {
        host_fail(&rt, HRESULT_FROM_WIN32(GetLastError()),
                  NEX_AUDIO_FALLBACK_MAPPING_FAILED);
        goto done;
    }
    rt.data = data;
    {
        /* Keep the shared ring and control page resident: a trimmed page here
           is a hard fault on the render thread. Best effort. */
        SIZE_T lock_bytes = (SIZE_T)control->data_bytes + sizeof(NexAudioControl);
        SIZE_T lo = 0, hi = 0;
        HANDLE self = GetCurrentProcess();
        if (GetProcessWorkingSetSize(self, &lo, &hi))
            SetProcessWorkingSetSize(self, lo + lock_bytes + 256 * 1024,
                                     hi > lo + 2 * lock_bytes + 256 * 1024
                                         ? hi : lo + 2 * lock_bytes + 256 * 1024);
        BOOL locked = VirtualLock(data, (SIZE_T)control->data_bytes) &&
                      VirtualLock(control, sizeof(NexAudioControl));
        host_log("memlock: ring %llu bytes %s", (unsigned long long)control->data_bytes,
                 locked ? "locked" : "left pageable");
    }

    hr = open_device(control, &device);
    if (FAILED(hr) || !device) {
        host_fail(&rt, FAILED(hr) ? hr : E_FAIL,
                  NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED);
        goto done;
    }
    WAVEFORMATEXTENSIBLE format;
    fill_output_format(control, &format);
    REFERENCE_TIME period = 1000000;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (client) { IAudioClient_Release(client); client = NULL; }
        hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                NULL, (void **)&client);
        if (FAILED(hr) || !client) break;
        hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE, 0,
                                     period, 0, &format.Format, NULL);
        if (hr != AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) break;
        UINT32 aligned = 0;
        HRESULT size_hr = IAudioClient_GetBufferSize(client, &aligned);
        IAudioClient_Release(client);
        client = NULL;
        if (FAILED(size_hr) || !aligned) {
            hr = FAILED(size_hr) ? size_hr : AUDCLNT_E_BUFFER_SIZE_ERROR;
            break;
        }
        period = period_for_frames(aligned, control->output_rate);
    }
    UINT32 device_frames = 0;
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(client, &device_frames);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(client, &IID_IAudioRenderClient,
                                                      (void **)&render);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(client, &IID_IAudioClock,
                                                      (void **)&clock);
    uint64_t frequency = 0;
    if (SUCCEEDED(hr)) hr = IAudioClock_GetFrequency(clock, &frequency);
    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(hr)) hr = IAudioClient_GetStreamLatency(client, &latency);
    if (FAILED(hr) || !render || !clock || !device_frames || !frequency) {
        host_fail(&rt, FAILED(hr) ? hr : E_FAIL, host_startup_reason(hr));
        goto done;
    }
    control->device_frames = device_frames;
    control->device_latency_100ns = (uint64_t)latency;
    control->active_engine = NEX_AUDIO_ENGINE_WASAPI;
    control->fallback_reason = NEX_AUDIO_FALLBACK_NONE;
    host_set_state(&rt, NEX_AUDIO_READY);
    rt.client = client;
    rt.render = render;
    rt.clock = clock;
    rt.clock_frequency = frequency;
    control->device_clock_frequency = frequency;
    SetEvent(ready_event);
    ready_signalled = 1;
    host_log("ready generation=%llu rate=%lu output=%u.%u encoding=%u device_frames=%lu capacity=%lu",
             (unsigned long long)control->stream_generation,
             (unsigned long)control->output_rate, control->output_container_bits,
             control->output_valid_bits, control->output_encoding,
             (unsigned long)device_frames, (unsigned long)control->capacity_frames);

    mmss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmss_task);
    if (!mmss) mmss = AvSetMmThreadCharacteristicsW(L"Audio", &mmss_task);
    if (mmss) {
        mmss_active = 1;
        AvSetMmThreadPriority(mmss, AVRT_PRIORITY_CRITICAL);
    }
    while (host_state(&rt) != NEX_AUDIO_CLOSING) {
        if (host_state(&rt) == NEX_AUDIO_FAILED) break;
        if (host_state(&rt) == NEX_AUDIO_COMPLETE) {
            WaitForSingleObject(data_event, 10);
            continue;
        }
        WaitForSingleObject(data_event, 3);
        hr = host_pump(&rt);
        if (GetTickCount() - last_status >= 1000) {
            last_status = GetTickCount();
            uint64_t ring_used = nex_audio_ring_available(control);
            host_log("telemetry generation=%llu converted=%llu source_waits=%llu "
                     "backpressure=%llu underrun=%llu dropped=%llu "
                     "eos_submitted=%llu eos_complete=%llu ring_used=%llu ring_capacity=%lu "
                     "padding=%lu clock_position=%llu clock_qpc=%llu state=%s",
                     (unsigned long long)control->stream_generation,
                     (unsigned long long)nex_audio_read64(&control->converted_frames),
                     (unsigned long long)nex_audio_read64(&control->source_waits),
                     (unsigned long long)nex_audio_read64(&control->backpressure_events),
                     (unsigned long long)nex_audio_read64(&control->underrun_events),
                     (unsigned long long)nex_audio_read64(&control->dropped_frames),
                     (unsigned long long)nex_audio_read64(&control->eos_submitted_generation),
                     (unsigned long long)nex_audio_read64(&control->eos_complete_generation),
                     (unsigned long long)ring_used,
                     (unsigned long)control->capacity_frames,
                     (unsigned long)control->device_padding_frames,
                     (unsigned long long)nex_audio_read64(&control->device_clock_position),
                     (unsigned long long)nex_audio_read64(&control->device_clock_qpc),
                     nex_audio_state_name(host_state(&rt)));
        }
        if (FAILED(hr)) break;
    }
    if (control->device_started) real_stop(&rt);
    exit_code = host_state(&rt) == NEX_AUDIO_CLOSING ? 0 : 1;

done:
    if (control && host_state(&rt) != NEX_AUDIO_FAILED &&
        host_state(&rt) != NEX_AUDIO_CLOSING && !ready_signalled)
        host_set_state(&rt, NEX_AUDIO_FAILED);
    if (ready_event && !ready_signalled) SetEvent(ready_event);
    if (mmss_active) AvRevertMmThreadCharacteristics(mmss);
    if (g_host_dump) { fclose(g_host_dump); g_host_dump = NULL; }
    if (clock) IAudioClock_Release(clock);
    if (render) IAudioRenderClient_Release(render);
    if (client) IAudioClient_Release(client);
    if (device) IMMDevice_Release(device);
    if (data) UnmapViewOfFile(data);
    if (pcm) CloseHandle(pcm);
    if (control) UnmapViewOfFile(control);
    if (map) CloseHandle(map);
    if (data_event) CloseHandle(data_event);
    if (space_event) CloseHandle(space_event);
    if (ready_event) CloseHandle(ready_event);
    CoUninitialize();
    return exit_code;
}
#endif

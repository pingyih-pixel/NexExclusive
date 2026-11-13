/* Optional out-of-process renderer. This path is intentionally strict: the
   client format is sent unchanged to NexAudioHost.exe, so conversion remains
   in the existing in-process engine until the remote protocol grows one. */
#include "../tools/nexaudiolink.h"

static HANDLE g_remote_map;
static HANDLE g_remote_data;
static NexAudioIpc *g_remote_ipc;
static HANDLE g_remote_ready;
static PROCESS_INFORMATION g_remote_process;
static HRESULT g_remote_error = E_FAIL;
static WCHAR g_remote_map_name[96];
static WCHAR g_remote_data_name[96];
static WCHAR g_remote_space_name[96];
static WCHAR g_remote_ready_name[96];

static void remote_close(void) {
    if (g_remote_ipc) {
        g_sink.hifi_client_frames = g_remote_ipc->client_frames;
        g_sink.hifi_endpoint_frames = g_remote_ipc->endpoint_frames;
        g_sink.hifi_changed_bytes = g_remote_ipc->changed_bytes;
        g_sink.hifi_dropped_frames = g_remote_ipc->dropped_frames;
        g_sink.hifi_underrun_events = g_remote_ipc->underrun_events;
        g_sink.hifi_client_hash = g_remote_ipc->client_hash;
        g_sink.hifi_endpoint_hash = g_remote_ipc->endpoint_hash;
        InterlockedExchange((volatile LONG *)&g_remote_ipc->state, NEX_AUDIO_CLOSING);
        if (g_remote_data) SetEvent(g_remote_data);
    }
    if (g_remote_process.hProcess) {
        if (WaitForSingleObject(g_remote_process.hProcess, 2000) != WAIT_OBJECT_0)
            TerminateProcess(g_remote_process.hProcess, 1);
        CloseHandle(g_remote_process.hThread);
        CloseHandle(g_remote_process.hProcess);
        memset(&g_remote_process, 0, sizeof(g_remote_process));
    }
    if (g_remote_ipc) UnmapViewOfFile(g_remote_ipc);
    if (g_remote_map) CloseHandle(g_remote_map);
    if (g_remote_data) CloseHandle(g_remote_data);
    if (g_remote_space) CloseHandle(g_remote_space);
    if (g_remote_ready) CloseHandle(g_remote_ready);
    g_remote_ipc = NULL;
    g_remote_map = g_remote_data = g_remote_space = g_remote_ready = NULL;
}

static int remote_start(IMMDevice *dev, const WAVEFORMATEX *fmt) {
    g_remote_error = E_FAIL;
    WCHAR *endpoint_id = NULL;
    if (dev && FAILED(dev->lpVtbl->GetId(dev, &endpoint_id))) endpoint_id = NULL;
    DWORD pid = GetCurrentProcessId();
    _snwprintf(g_remote_map_name, sizeof(g_remote_map_name) / sizeof(WCHAR),
               L"Local\\NexAudioMap_%lu", (unsigned long)pid);
    _snwprintf(g_remote_data_name, sizeof(g_remote_data_name) / sizeof(WCHAR),
               L"Local\\NexAudioData_%lu", (unsigned long)pid);
    _snwprintf(g_remote_space_name, sizeof(g_remote_space_name) / sizeof(WCHAR),
               L"Local\\NexAudioSpace_%lu", (unsigned long)pid);
    _snwprintf(g_remote_ready_name, sizeof(g_remote_ready_name) / sizeof(WCHAR),
               L"Local\\NexAudioReady_%lu", (unsigned long)pid);
    g_remote_map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                      0, (DWORD)sizeof(NexAudioIpc), g_remote_map_name);
    g_remote_data = CreateEventW(NULL, FALSE, FALSE, g_remote_data_name);
    g_remote_space = CreateEventW(NULL, FALSE, FALSE, g_remote_space_name);
    g_remote_ready = CreateEventW(NULL, FALSE, FALSE, g_remote_ready_name);
    if (!g_remote_map || !g_remote_data || !g_remote_space || !g_remote_ready) {
        if (endpoint_id) CoTaskMemFree(endpoint_id);
        remote_close();
        return 0;
    }
    g_remote_ipc = (NexAudioIpc *)MapViewOfFile(g_remote_map, FILE_MAP_ALL_ACCESS, 0, 0,
                                                sizeof(NexAudioIpc));
    if (!g_remote_ipc) {
        if (endpoint_id) CoTaskMemFree(endpoint_id);
        remote_close();
        return 0;
    }
    ZeroMemory(g_remote_ipc, sizeof(*g_remote_ipc));
    g_remote_ipc->magic = NEX_AUDIO_MAGIC;
    g_remote_ipc->version = NEX_AUDIO_VERSION;
    g_remote_ipc->state = NEX_AUDIO_INIT;
    g_remote_ipc->rate = fmt->nSamplesPerSec;
    g_remote_ipc->channels = fmt->nChannels;
    g_remote_ipc->container_bits = fmt->wBitsPerSample;
    g_remote_ipc->valid_bits = fmt->wBitsPerSample;
    g_remote_ipc->is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    g_remote_ipc->block = fmt->nBlockAlign;
    g_remote_ipc->capacity = NEX_AUDIO_RING_FRAMES;
    g_remote_ipc->channel_mask = mask_for(fmt->nChannels);
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *we = (const WAVEFORMATEXTENSIBLE *)fmt;
        g_remote_ipc->is_float = we->SubFormat.Data1 == 3;
        if (we->Samples.wValidBitsPerSample) g_remote_ipc->valid_bits = we->Samples.wValidBitsPerSample;
        if (we->dwChannelMask) g_remote_ipc->channel_mask = we->dwChannelMask;
    }
    if (endpoint_id) {
        wcsncpy(g_remote_ipc->endpoint_id, endpoint_id, NEX_AUDIO_ENDPOINT_CHARS - 1);
        g_remote_ipc->endpoint_id[NEX_AUDIO_ENDPOINT_CHARS - 1] = 0;
        CoTaskMemFree(endpoint_id);
    }

    char base[MAX_PATH];
    if (!module_dir(base, sizeof(base))) {
        g_remote_error = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        log_line("audiohost: cannot resolve module directory");
        remote_close();
        return 0;
    }
    WCHAR wbase[MAX_PATH], host[MAX_PATH], cmd[1024];
    MultiByteToWideChar(CP_ACP, 0, base, -1, wbase, MAX_PATH);
    wcsncpy(host, wbase, MAX_PATH - 1);
    host[MAX_PATH - 1] = 0;
    WCHAR *slash = wcsrchr(host, L'\\');
    if (slash) slash[1] = 0;
    wcsncat(host, L"NexAudioHost.exe", MAX_PATH - wcslen(host) - 1);
    _snwprintf(cmd, sizeof(cmd) / sizeof(WCHAR),
               L"\"%ls\" --map \"%ls\" --data \"%ls\" --space \"%ls\" --ready \"%ls\"",
               host, g_remote_map_name, g_remote_data_name, g_remote_space_name,
               g_remote_ready_name);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&g_remote_process, sizeof(g_remote_process));
    if (!CreateProcessW(host, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                        NULL, &si, &g_remote_process)) {
        g_remote_error = HRESULT_FROM_WIN32(GetLastError());
        log_line("audiohost: start failed %lu (%ls)", GetLastError(), host);
        remote_close();
        return 0;
    }
    HANDLE waits[2] = {g_remote_ready, g_remote_process.hProcess};
    DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 2000);
    LONG state = InterlockedCompareExchange((volatile LONG *)&g_remote_ipc->state, 0, 0);
    if (wait == WAIT_OBJECT_0 && state == NEX_AUDIO_READY) {
        log_line("audiohost: ready pid=%lu rate=%lu block=%lu",
                 (unsigned long)g_remote_process.dwProcessId,
                 (unsigned long)g_remote_ipc->rate,
                 (unsigned long)g_remote_ipc->block);
        return 1;
    }
    g_remote_error = (HRESULT)g_remote_ipc->error;
    log_line("audiohost: failed state=%ld hr=0x%08lX",
             (long)g_remote_ipc->state, (unsigned long)g_remote_ipc->error);
    remote_close();
    return 0;
}

static UINT32 remote_write(const BYTE *src, UINT32 frames) {
    if (!g_remote_ipc || g_remote_ipc->state != NEX_AUDIO_READY) return 0;
    UINT64 read = (UINT64)InterlockedCompareExchange64(
        (volatile LONG64 *)&g_remote_ipc->read_pos, 0, 0);
    UINT64 write = (UINT64)InterlockedCompareExchange64(
        (volatile LONG64 *)&g_remote_ipc->write_pos, 0, 0);
    UINT64 used = write - read;
    UINT32 room = used >= g_remote_ipc->capacity
                    ? 0 : g_remote_ipc->capacity - (UINT32)used;
    if (frames > room) {
        InterlockedExchangeAdd64((volatile LONG64 *)&g_remote_ipc->dropped_frames,
                                 (LONG64)(frames - room));
        frames = room;
    }
    if (!frames) return 0;
    size_t bytes = (size_t)frames * g_remote_ipc->block;
    size_t start = (size_t)(write % g_remote_ipc->capacity) * g_remote_ipc->block;
    size_t ring_bytes = (size_t)g_remote_ipc->capacity * g_remote_ipc->block;
    size_t first = ring_bytes - start;
    if (first > bytes) first = bytes;
    memcpy(g_remote_ipc->data + start, src, first);
    if (first < bytes) memcpy(g_remote_ipc->data, src + first, bytes - first);
    MemoryBarrier();
    InterlockedExchange64((volatile LONG64 *)&g_remote_ipc->write_pos, write + frames);
    InterlockedExchange64((volatile LONG64 *)&g_remote_ipc->client_frames,
                          (LONG64)(g_remote_ipc->client_frames + frames));
    if (!g_sink.hifi_client_hash) g_sink.hifi_client_hash = 1469598103934665603ull;
    g_sink.hifi_client_hash = hifi_hash_bytes(src, bytes, g_sink.hifi_client_hash);
    InterlockedExchange64((volatile LONG64 *)&g_remote_ipc->client_hash,
                          (LONG64)g_sink.hifi_client_hash);
    SetEvent(g_remote_data);
    return frames;
}

static HRESULT remote_drain_once(SinkGen *g) {
    if (!g_remote_ipc || g_remote_ipc->state != NEX_AUDIO_READY) return E_FAIL;
    UINT64 read = (UINT64)InterlockedCompareExchange64(
        (volatile LONG64 *)&g_remote_ipc->read_pos, 0, 0);
    UINT64 write = (UINT64)InterlockedCompareExchange64(
        (volatile LONG64 *)&g_remote_ipc->write_pos, 0, 0);
    UINT32 room = write - read >= g_remote_ipc->capacity
                    ? 0 : g_remote_ipc->capacity - (UINT32)(write - read);
    UINT32 available = g_sink.w - g_sink.r;
    UINT32 end = g_sink.cap - g_sink.r % g_sink.cap;
    if (available > end) available = end;
    UINT32 n = available < room ? available : room;
    if (n > SCRATCH_FRAMES) n = SCRATCH_FRAMES;
    if (!n) {
        if (g_remote_space) WaitForSingleObject(g_remote_space, 5);
        return S_FALSE;
    }
    UINT32 got = ring_take(g_inb, n);
    if (got != n) return E_UNEXPECTED;
    if (remote_write(g_inb, got) != got) return E_FAIL;
    g->submitted += got;
    g->last_submit = (LONG)GetTickCount();
    g->device_started = 1;
    if (g_sink.h_space) SetEvent(g_sink.h_space);
    return S_OK;
}

static HRESULT remote_sink_engage(IAudioClient *self, IMMDevice *dev,
                                  const WAVEFORMATEX *fmt, REFERENCE_TIME dur,
                                  const char *ep_name, int ep_hw) {
    if (!g_hifi || g_compat) {
        log_line("audiohost: requires hifi=1 and compat=0");
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    g_sink.in_rate = (int)fmt->nSamplesPerSec;
    g_sink.in_channels = fmt->nChannels;
    g_sink.in_block = fmt->nBlockAlign;
    g_sink.in_is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    g_sink.in_container_bits = fmt->wBitsPerSample;
    g_sink.in_valid_bits = fmt->wBitsPerSample;
    g_sink.channel_mask = mask_for(fmt->nChannels);
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *we = (const WAVEFORMATEXTENSIBLE *)fmt;
        g_sink.in_is_float = we->SubFormat.Data1 == 3;
        if (we->Samples.wValidBitsPerSample) g_sink.in_valid_bits = we->Samples.wValidBitsPerSample;
        if (we->dwChannelMask) g_sink.channel_mask = we->dwChannelMask;
    }
    g_sink.out_rate = g_sink.in_rate;
    g_sink.out_bits = g_sink.in_valid_bits;
    g_sink.out_container_bits = g_sink.in_container_bits;
    g_sink.out_valid_bits = g_sink.in_valid_bits;
    g_sink.out_is_float = g_sink.in_is_float;
    g_sink.out_block = g_sink.in_block;
    g_sink.resample = 0;
    g_sink.dither = 0;

    scratch_free();
    g_inb = (BYTE *)malloc((size_t)SCRATCH_FRAMES * g_sink.in_block);
    if (!g_inb) return E_OUTOFMEMORY;
    UINT32 want = (UINT32)g_out_frames;
    if (!want) {
        REFERENCE_TIME d = dur > 0 ? dur : 300000;
        want = (UINT32)((double)g_sink.in_rate * (double)d / 10000000.0);
        if (want < 1024) want = 1024;
        if (want > 8192) want = 8192;
    }
    want = (want + 127u) & ~127u;
    g_sink.cap = want * 8;
    if (g_sink.cap < 16384) g_sink.cap = 16384;
    g_sink.ring = (BYTE *)calloc((size_t)g_sink.cap, (size_t)g_sink.in_block);
    if (!g_sink.ring) { sink_teardown_controlled(); return E_OUTOFMEMORY; }
    g_sink.w = g_sink.r = 0;
    g_sink.h_space = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_sink.h_data = CreateEventA(NULL, FALSE, FALSE, NULL);

    SinkGen *gen = gen_new();
    if (!gen) { sink_teardown_controlled(); return E_OUTOFMEMORY; }
    gen->remote = 1;
    gen->real_frames = want;
    g_sink.gen = gen;
    g_sink.generation = gen->id;
    g_sink.proxy_render = (FakeObj *)calloc(1, sizeof(FakeObj));
    g_sink.proxy_clock = (FakeObj *)calloc(1, sizeof(FakeObj));
    if (!g_sink.proxy_render || !g_sink.proxy_clock) { sink_teardown_controlled(); return E_OUTOFMEMORY; }
    g_sink.proxy_render->vtbl = g_fake_render_vtbl;
    g_sink.proxy_render->ref = 1;
    g_sink.proxy_render->generation = g_sink.generation;
    g_sink.proxy_render->capacity = want;
    g_sink.proxy_render->staging = (BYTE *)malloc((size_t)want * g_sink.in_block);
    if (!g_sink.proxy_render->staging) { sink_teardown_controlled(); return E_OUTOFMEMORY; }
    g_sink.proxy_clock->vtbl = g_fake_clock_vtbl;
    g_sink.proxy_clock->ref = 1;
    g_sink.proxy_clock->generation = g_sink.generation;

    g_sink.engaged = 1;
    g_sink.owner = (void *)self;
    ((IUnknown *)self)->lpVtbl->AddRef((IUnknown *)self);
    g_sink.reported_frames = want;
    g_sink.endpoint_key = endpoint_hash(dev);
    g_sink.device_lost = 0;
    g_sink.fault = S_OK;
    g_sink.hifi_map = 1;
    g_sink.hifi_first_mismatch_frame = ~(UINT64)0;
    g_sink.hifi_client_hash = 1469598103934665603ull;
    g_sink.hifi_endpoint_hash = 1469598103934665603ull;
    if (!remote_start(dev, fmt)) {
        log_line("strict native rejected: audiohost could not open the client PCM; "
                 "switch to source-preferred mode to continue playback");
        sink_teardown_controlled();
        return g_remote_error;
    }
    char a[160], b[240];
    describe("in", fmt, a, sizeof(a));
    snprintf(b, sizeof(b), "out=rate:%d/cont:%d.%d/ch:%d hifi=1 map=client-native [native] engine=audiohost",
             g_sink.out_rate, g_sink.out_container_bits, g_sink.out_valid_bits,
             g_sink.in_channels);
    log_line("sink ENGAGED  %s  ->  %s  (client buffer %lu, ring %lu) ep=\"%s\" hw=%d",
             a, b, (unsigned long)want, (unsigned long)g_sink.cap,
             ep_name ? ep_name : "", ep_hw);
    return S_OK;
}

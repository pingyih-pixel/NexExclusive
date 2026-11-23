/* Optional out-of-process renderer. Negotiation, SRC, quantisation and channel
   layout stay in the common sink. Only the negotiated output PCM crosses into
   NexAudioHost.exe, which owns the exclusive endpoint. */
#include "../tools/nexaudiolink.h"

static HANDLE g_remote_map;
static HANDLE g_remote_pcm;
static NexAudioControl *g_remote_ipc;
static unsigned char *g_remote_pcm_data;
static HANDLE g_remote_data;
static HANDLE g_remote_ready;
static PROCESS_INFORMATION g_remote_process;
static HRESULT g_remote_error = E_FAIL;
static UINT32 g_remote_reason = NEX_AUDIO_FALLBACK_NONE;
static int g_remote_bypass;
static BYTE *g_remote_out;
static UINT64 g_remote_generation;
static UINT64 g_remote_format_generation;
static WCHAR g_remote_map_name[NEX_AUDIO_MAPPING_CHARS];
static WCHAR g_remote_pcm_name[NEX_AUDIO_MAPPING_CHARS];
static WCHAR g_remote_data_name[NEX_AUDIO_MAPPING_CHARS];
static WCHAR g_remote_space_name[NEX_AUDIO_MAPPING_CHARS];
static WCHAR g_remote_ready_name[NEX_AUDIO_MAPPING_CHARS];

static void remote_mark_host_exit(void) {
    if (!g_remote_ipc) return;
    LONG state = InterlockedCompareExchange((volatile LONG *)&g_remote_ipc->state, 0, 0);
    if (state == NEX_AUDIO_FAILED || state == NEX_AUDIO_COMPLETE ||
        state == NEX_AUDIO_CLOSING)
        return;
    g_remote_error = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
    g_remote_reason = NEX_AUDIO_FALLBACK_HOST_EXIT;
    InterlockedExchange((volatile LONG *)&g_remote_ipc->error, (LONG)g_remote_error);
    g_remote_ipc->fallback_reason = g_remote_reason;
    InterlockedExchange((volatile LONG *)&g_remote_ipc->state, NEX_AUDIO_FAILED);
    if (g_remote_data) SetEvent(g_remote_data);
    if (g_remote_space) SetEvent(g_remote_space);
    log_line("audiohost: host_exit generation=%llu state=%s",
             (unsigned long long)g_remote_ipc->stream_generation,
             nex_audio_state_name(state));
}

static long remote_state(void) {
    if (g_remote_process.hProcess) {
        DWORD result = WaitForSingleObject(g_remote_process.hProcess, 0);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)
            remote_mark_host_exit();
    }
    return g_remote_ipc ? InterlockedCompareExchange(
        (volatile LONG *)&g_remote_ipc->state, 0, 0) : NEX_AUDIO_CLOSING;
}

static void remote_sync_stats(void) {
    if (!g_remote_ipc) return;
    g_sink.hifi_client_frames = nex_audio_read64(&g_remote_ipc->client_frames);
    g_sink.hifi_converted_frames = nex_audio_read64(&g_remote_ipc->converted_frames);
    g_sink.hifi_endpoint_frames = nex_audio_read64(&g_remote_ipc->endpoint_frames);
    g_sink.hifi_changed_bytes = nex_audio_read64(&g_remote_ipc->changed_bytes);
    g_sink.hifi_dropped_frames = nex_audio_read64(&g_remote_ipc->dropped_frames);
    g_sink.hifi_underrun_events = nex_audio_read64(&g_remote_ipc->underrun_events);
    g_sink.hifi_client_hash = nex_audio_read64(&g_remote_ipc->client_hash);
    g_sink.hifi_endpoint_hash = nex_audio_read64(&g_remote_ipc->endpoint_hash);
    if (g_sink.hifi_map == 1 &&
        g_sink.hifi_client_frames == g_sink.hifi_endpoint_frames &&
        g_sink.hifi_converted_frames == g_sink.hifi_endpoint_frames &&
        !g_sink.hifi_changed_bytes &&
        nex_audio_read64(&g_remote_ipc->converted_hash) == g_sink.hifi_client_hash &&
        g_sink.hifi_endpoint_hash == g_sink.hifi_client_hash)
        g_sink.hifi_verified_frames = g_sink.hifi_endpoint_frames;
    else
        g_sink.hifi_verified_frames = 0;
    static DWORD last_remote_log;
    DWORD now = GetTickCount();
    if (now - last_remote_log >= 1000) {
        last_remote_log = now;
        log_line("audiohost telemetry generation=%llu converted=%llu source_waits=%llu "
                 "backpressure=%llu underrun=%llu dropped=%llu eos_submitted=%llu eos_complete=%llu",
                 (unsigned long long)g_remote_ipc->stream_generation,
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->converted_frames),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->source_waits),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->backpressure_events),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->underrun_events),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->dropped_frames),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->eos_submitted_generation),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->eos_complete_generation));
    }
}

static void remote_close(void) {
    if (g_remote_ipc) {
        remote_sync_stats();
        LONG state = remote_state();
        if (state != NEX_AUDIO_FAILED && state != NEX_AUDIO_COMPLETE)
            InterlockedExchange((volatile LONG *)&g_remote_ipc->state, NEX_AUDIO_CLOSING);
        if (g_remote_data) SetEvent(g_remote_data);
        log_line("audiohost: close generation=%llu state=%s client=%llu converted=%llu "
                 "endpoint=%llu source_waits=%llu backpressure=%llu underrun=%llu dropped=%llu "
                 "eos_submitted=%llu eos_complete=%llu getbuffer=%llu release=%llu",
                 (unsigned long long)g_remote_ipc->stream_generation,
                 nex_audio_state_name(state),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->client_frames),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->converted_frames),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->endpoint_frames),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->source_waits),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->backpressure_events),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->underrun_events),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->dropped_frames),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->eos_submitted_generation),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->eos_complete_generation),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->getbuffer_failures),
                 (unsigned long long)nex_audio_read64(&g_remote_ipc->releasebuffer_failures));
    }
    if (g_remote_process.hProcess) {
        if (WaitForSingleObject(g_remote_process.hProcess, 2000) != WAIT_OBJECT_0)
            TerminateProcess(g_remote_process.hProcess, 1);
        CloseHandle(g_remote_process.hThread);
        CloseHandle(g_remote_process.hProcess);
        memset(&g_remote_process, 0, sizeof(g_remote_process));
    }
    if (g_remote_ipc) UnmapViewOfFile(g_remote_ipc);
    if (g_remote_pcm_data) UnmapViewOfFile(g_remote_pcm_data);
    if (g_remote_map) CloseHandle(g_remote_map);
    if (g_remote_pcm) CloseHandle(g_remote_pcm);
    if (g_remote_data) CloseHandle(g_remote_data);
    if (g_remote_space) CloseHandle(g_remote_space);
    if (g_remote_ready) CloseHandle(g_remote_ready);
    free(g_remote_out);
    g_remote_out = NULL;
    g_remote_ipc = NULL;
    g_remote_pcm_data = NULL;
    g_remote_map = g_remote_pcm = g_remote_data = g_remote_space = g_remote_ready = NULL;
}

static int remote_fallback_is_local(UINT32 reason) {
    return reason == NEX_AUDIO_FALLBACK_PROTOCOL_MISMATCH ||
           reason == NEX_AUDIO_FALLBACK_LAYOUT_MISMATCH ||
           reason == NEX_AUDIO_FALLBACK_BUILD_MISMATCH ||
           reason == NEX_AUDIO_FALLBACK_UNSUPPORTED_ENGINE ||
           reason == NEX_AUDIO_FALLBACK_UNSUPPORTED_DEPLOYMENT ||
           reason == NEX_AUDIO_FALLBACK_UNSUPPORTED_FORMAT ||
           reason == NEX_AUDIO_FALLBACK_MAPPING_FAILED ||
           reason == NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED ||
           reason == NEX_AUDIO_FALLBACK_HOST_EXIT;
}

static HRESULT remote_try_exclusive(SinkGen *g, IMMDevice *dev,
                                    const WAVEFORMATEX *fmt) {
    g_remote_error = E_FAIL;
    g_remote_reason = NEX_AUDIO_FALLBACK_NONE;
    if (g_remote_ipc || g_remote_process.hProcess) remote_close();
    WCHAR *endpoint_id = NULL;
    if (dev && FAILED(dev->lpVtbl->GetId(dev, &endpoint_id))) endpoint_id = NULL;
    UINT64 generation = ++g_remote_generation;
    if (!generation) generation = ++g_remote_generation;
    UINT64 format_generation = ++g_remote_format_generation;
    if (!format_generation) format_generation = ++g_remote_format_generation;
    DWORD pid = GetCurrentProcessId();
    _snwprintf(g_remote_map_name, NEX_AUDIO_MAPPING_CHARS,
               L"Local\\NexAudioV3Map_%lu_%llu", (unsigned long)pid,
               (unsigned long long)generation);
    _snwprintf(g_remote_pcm_name, NEX_AUDIO_MAPPING_CHARS,
               L"Local\\NexAudioV3Pcm_%lu_%llu", (unsigned long)pid,
               (unsigned long long)generation);
    _snwprintf(g_remote_data_name, NEX_AUDIO_MAPPING_CHARS,
               L"Local\\NexAudioV3Data_%lu_%llu", (unsigned long)pid,
               (unsigned long long)generation);
    _snwprintf(g_remote_space_name, NEX_AUDIO_MAPPING_CHARS,
               L"Local\\NexAudioV3Space_%lu_%llu", (unsigned long)pid,
               (unsigned long long)generation);
    _snwprintf(g_remote_ready_name, NEX_AUDIO_MAPPING_CHARS,
               L"Local\\NexAudioV3Ready_%lu_%llu", (unsigned long)pid,
               (unsigned long long)generation);

    UINT32 capacity = fmt->nSamplesPerSec / 2u; /* current short ring: 0.5 s */
    if (capacity < NEX_AUDIO_MIN_RING_FRAMES) capacity = NEX_AUDIO_MIN_RING_FRAMES;
    UINT64 data_bytes = (UINT64)capacity * fmt->nBlockAlign;
    if (!data_bytes || data_bytes > NEX_AUDIO_MAX_DATA_BYTES) {
        if (endpoint_id) CoTaskMemFree(endpoint_id);
        g_remote_error = AUDCLNT_E_UNSUPPORTED_FORMAT;
        g_remote_reason = NEX_AUDIO_FALLBACK_UNSUPPORTED_FORMAT;
        return g_remote_error;
    }
    g_remote_map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                      0, (DWORD)NEX_AUDIO_CONTROL_BYTES,
                                      g_remote_map_name);
    g_remote_pcm = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                      (DWORD)(data_bytes >> 32), (DWORD)data_bytes,
                                      g_remote_pcm_name);
    g_remote_data = CreateEventW(NULL, FALSE, FALSE, g_remote_data_name);
    g_remote_space = CreateEventW(NULL, FALSE, FALSE, g_remote_space_name);
    g_remote_ready = CreateEventW(NULL, FALSE, FALSE, g_remote_ready_name);
    if (g_remote_map)
        g_remote_ipc = (NexAudioControl *)MapViewOfFile(g_remote_map,
            FILE_MAP_ALL_ACCESS, 0, 0, NEX_AUDIO_CONTROL_BYTES);
    if (g_remote_pcm)
        g_remote_pcm_data = (unsigned char *)MapViewOfFile(g_remote_pcm,
            FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)data_bytes);
    if (!g_remote_map || !g_remote_pcm || !g_remote_ipc || !g_remote_pcm_data ||
        !g_remote_data || !g_remote_space || !g_remote_ready) {
        g_remote_error = HRESULT_FROM_WIN32(GetLastError());
        g_remote_reason = NEX_AUDIO_FALLBACK_MAPPING_FAILED;
        g_remote_bypass = 1;
        if (endpoint_id) CoTaskMemFree(endpoint_id);
        remote_close();
        return g_remote_error;
    }

    nex_audio_control_init(g_remote_ipc, generation);
    NexAudioControl *c = g_remote_ipc;
    c->input_rate = (UINT32)g_sink.in_rate;
    c->input_channels = (UINT16)g_sink.in_channels;
    c->input_container_bits = (UINT16)g_sink.in_container_bits;
    c->input_valid_bits = (UINT16)g_sink.in_valid_bits;
    c->input_encoding = g_sink.in_is_float ? NEX_AUDIO_ENCODING_FLOAT : NEX_AUDIO_ENCODING_PCM;
    c->input_channel_mask = g_sink.channel_mask;
    c->output_rate = fmt->nSamplesPerSec;
    c->output_channels = fmt->nChannels;
    c->output_container_bits = fmt->wBitsPerSample;
    c->output_valid_bits = fmt->wBitsPerSample;
    c->output_encoding = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT
                       ? NEX_AUDIO_ENCODING_FLOAT : NEX_AUDIO_ENCODING_PCM;
    c->output_channel_mask = mask_for(fmt->nChannels);
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *we = (const WAVEFORMATEXTENSIBLE *)fmt;
        c->output_encoding = IsEqualGUID(&we->SubFormat, &SUB_FLOAT)
                           ? NEX_AUDIO_ENCODING_FLOAT : NEX_AUDIO_ENCODING_PCM;
        if (we->Samples.wValidBitsPerSample)
            c->output_valid_bits = we->Samples.wValidBitsPerSample;
        if (we->dwChannelMask) c->output_channel_mask = we->dwChannelMask;
    }
    c->output_frame_bytes = fmt->nBlockAlign;
    c->capacity_frames = capacity;
    c->data_bytes = data_bytes;
    c->format_generation = format_generation;
    c->client_hash = c->converted_hash = c->endpoint_hash = 1469598103934665603ull;
    wcscpy(c->data_mapping_name, g_remote_pcm_name);
    if (endpoint_id) {
        wcsncpy(c->target_id, endpoint_id, NEX_AUDIO_ENDPOINT_CHARS - 1);
        c->target_id[NEX_AUDIO_ENDPOINT_CHARS - 1] = 0;
        CoTaskMemFree(endpoint_id);
    }
    if (!nex_audio_output_format_valid(c)) {
        g_remote_error = AUDCLNT_E_UNSUPPORTED_FORMAT;
        g_remote_reason = NEX_AUDIO_FALLBACK_UNSUPPORTED_FORMAT;
        remote_close();
        return g_remote_error;
    }

    char base[MAX_PATH];
    if (!module_dir(base, sizeof(base))) {
        g_remote_error = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        g_remote_reason = NEX_AUDIO_FALLBACK_HOST_EXIT;
        g_remote_bypass = 1;
        remote_close();
        return g_remote_error;
    }
    WCHAR host[MAX_PATH], cmd[2048];
    MultiByteToWideChar(CP_ACP, 0, base, -1, host, MAX_PATH);
    wcsncat(host, L"NexAudioHost.exe", MAX_PATH - wcslen(host) - 1);
    _snwprintf(cmd, sizeof(cmd) / sizeof(WCHAR),
               L"\"%ls\" --map \"%ls\" --pcm \"%ls\" --data \"%ls\" --space \"%ls\" --ready \"%ls\"",
               host, g_remote_map_name, g_remote_pcm_name, g_remote_data_name,
               g_remote_space_name, g_remote_ready_name);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&g_remote_process, sizeof(g_remote_process));
    if (!CreateProcessW(host, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                        NULL, &si, &g_remote_process)) {
        g_remote_error = HRESULT_FROM_WIN32(GetLastError());
        g_remote_reason = NEX_AUDIO_FALLBACK_HOST_EXIT;
        g_remote_bypass = 1;
        log_line("audiohost: start failed %lu (%ls)", GetLastError(), host);
        remote_close();
        return g_remote_error;
    }
    HANDLE waits[2] = {g_remote_ready, g_remote_process.hProcess};
    DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 5000);
    if (wait == WAIT_OBJECT_0 && remote_state() == NEX_AUDIO_READY &&
        c->device_frames && c->device_frames <= c->capacity_frames) {
        g->real_frames = c->device_frames;
        g->real_latency = (REFERENCE_TIME)c->device_latency_100ns;
        g_remote_out = (BYTE *)malloc((size_t)SCRATCH_FRAMES * c->output_frame_bytes);
        if (!g_remote_out) {
            g_remote_error = E_OUTOFMEMORY;
            g_remote_reason = NEX_AUDIO_FALLBACK_MAPPING_FAILED;
            g_remote_bypass = 1;
            remote_close();
            return g_remote_error;
        }
        g->remote = 1;
        g_remote_error = S_OK;
        log_line("audiohost: ready pid=%llu input=%lu/%u.%u output=%lu/%u.%u "
                 "device_frames=%lu capacity=%llu generation=%llu",
                 (unsigned long long)g_remote_process.dwProcessId,
                 (unsigned long)c->input_rate, c->input_container_bits,
                 c->input_valid_bits, (unsigned long)c->output_rate,
                 c->output_container_bits, c->output_valid_bits,
                 (unsigned long)c->device_frames,
                 (unsigned long long)c->data_bytes,
                 (unsigned long long)c->stream_generation);
        return S_OK;
    }
    g_remote_error = (HRESULT)c->error;
    if (SUCCEEDED(g_remote_error)) g_remote_error = E_FAIL;
    g_remote_reason = c->fallback_reason;
    if (!g_remote_reason) g_remote_reason = NEX_AUDIO_FALLBACK_HOST_EXIT;
    if (remote_fallback_is_local(g_remote_reason)) g_remote_bypass = 1;
    log_line("audiohost: open rejected state=%s reason=%s hr=0x%08lX local_fallback=%d",
             nex_audio_state_name(remote_state()),
             nex_audio_fallback_name(g_remote_reason),
             (unsigned long)g_remote_error, g_remote_bypass);
    remote_close();
    return g_remote_error;
}

static void remote_note_client(const BYTE *source, UINT32 frames) {
    if (!g_remote_ipc || !source || !frames) return;
    size_t bytes = (size_t)frames * g_sink.in_block;
    UINT64 hash = hifi_hash_bytes(source, bytes,
        nex_audio_read64(&g_remote_ipc->client_hash));
    InterlockedExchange64((volatile LONG64 *)&g_remote_ipc->client_hash, (LONG64)hash);
    InterlockedExchangeAdd64((volatile LONG64 *)&g_remote_ipc->client_frames, frames);
    g_sink.hifi_client_frames = nex_audio_read64(&g_remote_ipc->client_frames);
    g_sink.hifi_client_hash = hash;
}

static UINT32 remote_write(const BYTE *client, const BYTE *converted, UINT32 frames) {
    if (!g_remote_ipc || !converted || !frames) return 0;
    DWORD deadline = 0;
    for (;;) {
        LONG state = remote_state();
        if (state == NEX_AUDIO_FAILED || state == NEX_AUDIO_CLOSING ||
            state == NEX_AUDIO_COMPLETE) return 0;
        UINT64 write = nex_audio_read64((volatile UINT64 *)&g_remote_ipc->write_frame);
        UINT64 read = nex_audio_read64((volatile UINT64 *)&g_remote_ipc->read_frame);
        UINT64 used = write >= read ? write - read : UINT64_MAX;
        if (used <= g_remote_ipc->capacity_frames &&
            frames <= g_remote_ipc->capacity_frames - (UINT32)used) {
            if (nex_audio_ring_write(g_remote_ipc, g_remote_pcm_data,
                                     converted, frames) != frames) return 0;
            size_t bytes = (size_t)frames * g_remote_ipc->output_frame_bytes;
            UINT64 hash = hifi_hash_bytes(converted, bytes,
                nex_audio_read64(&g_remote_ipc->converted_hash));
            InterlockedExchange64((volatile LONG64 *)&g_remote_ipc->converted_hash, (LONG64)hash);
            InterlockedExchangeAdd64((volatile LONG64 *)&g_remote_ipc->converted_frames, frames);
            if (client && g_sink.hifi_map == 1 && g_sink.in_block == g_sink.out_block) {
                InterlockedExchangeAdd64((volatile LONG64 *)&g_remote_ipc->changed_bytes,
                    (LONG64)hifi_changed_bytes(client, converted, bytes));
            }
            SetEvent(g_remote_data);
            remote_sync_stats();
            return frames;
        }
        DWORD now = GetTickCount();
        if (!deadline) {
            deadline = now + NEX_AUDIO_BACKPRESSURE_TIMEOUT_MS;
            InterlockedExchangeAdd64((volatile LONG64 *)&g_remote_ipc->backpressure_events, 1);
        }
        if ((LONG)(now - deadline) >= 0) {
            g_remote_error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            g_remote_reason = NEX_AUDIO_FALLBACK_RING_BACKPRESSURE;
            InterlockedExchange((volatile LONG *)&g_remote_ipc->error, g_remote_error);
            g_remote_ipc->fallback_reason = g_remote_reason;
            InterlockedExchange((volatile LONG *)&g_remote_ipc->state, NEX_AUDIO_FAILED);
            SetEvent(g_remote_data);
            log_line("audiohost: backpressure timeout after %u ms; write refused without truncation",
                     NEX_AUDIO_BACKPRESSURE_TIMEOUT_MS);
            return 0;
        }
        DWORD remaining = deadline - now;
        WaitForSingleObject(g_remote_space, remaining > 20 ? 20 : remaining);
    }
}

/* v3.1 transport. The Host owns the device, so the client's Stop/Start and
   Reset reach it as requests on the control page. */
static void remote_set_transport(UINT32 transport) {
    if (!g_remote_ipc) return;
    InterlockedExchange((volatile LONG *)&g_remote_ipc->transport, (LONG)transport);
    if (g_remote_data) SetEvent(g_remote_data);
}

/* The device rate of the previous fresh stream, for the clock-family check. */
static long g_last_device_rate;

/* Arm the start-up silence for a fresh device stream (engage, retarget). The
   in-process drain submits it itself; the Host does it for a remote stream. */
static void sink_arm_prime(SinkGen *gen, long rate) {
    long frames = nex_audio_policy_prime_frames(g_last_device_rate, rate,
                                                gen ? (long)gen->real_frames : 0,
                                                g_prime, g_resync_ms);
    g_last_device_rate = rate;
    g_sink.skip_frames = 0;
    g_sink.prime_left = 0;
    if (gen && gen->remote) {
        if (g_remote_ipc)
            InterlockedExchange((volatile LONG *)&g_remote_ipc->prime_request_frames, frames);
    } else {
        g_sink.prime_left = (UINT32)frames;
    }
    if (frames)
        log_line("sink: %ld frames of start-up silence armed (rate %ld, %s)", frames, rate,
                 gen && gen->remote ? "audiohost" : "in-process");
}

/* After a seek the device starts again from an empty buffer: one buffer of
   silence, no resync (the clock family has not changed). */
static void sink_arm_prime_after_reset(SinkGen *gen) {
    g_sink.skip_frames = 0;
    g_sink.prime_left = 0;
    if (!gen || !g_prime) return;
    if (gen->remote) {
        if (g_remote_ipc)
            InterlockedExchange((volatile LONG *)&g_remote_ipc->prime_request_frames,
                                (LONG)gen->real_frames);
    } else {
        g_sink.prime_left = gen->real_frames;
    }
}

/* Ask the Host to drop its ring and device buffer (a seek). Called with the
   drain thread stopped, so nothing writes while the Host discards. Returns 1
   once the Host has acknowledged within timeout_ms. */
static int remote_request_flush(DWORD timeout_ms) {
    if (!g_remote_ipc) return 0;
    UINT64 generation = nex_audio_read64(&g_remote_ipc->flush_request_generation) + 1;
    InterlockedExchange64((volatile LONG64 *)&g_remote_ipc->flush_request_generation,
                          (LONG64)generation);
    if (g_remote_data) SetEvent(g_remote_data);
    DWORD start = GetTickCount();
    for (;;) {
        if (nex_audio_read64(&g_remote_ipc->flush_ack_generation) == generation) return 1;
        LONG state = remote_state();
        if (state == NEX_AUDIO_FAILED || state == NEX_AUDIO_CLOSING) return 0;
        if (GetTickCount() - start >= timeout_ms) return 0;
        if (g_remote_space) WaitForSingleObject(g_remote_space, 5); else Sleep(2);
    }
}

/* EOS asks the Host to play out what it holds. That only makes sense for a
   stream that was still playing; a stopped stream's tail belongs to a pause or
   a skip and must not be played, nor waited for. */
static int sink_teardown_waits_for_eos(int was_remote, int was_playing) {
    return was_remote && was_playing;
}

/* Called only when a real source-end signal becomes available. Stop, Reset and
   a track rebuild are transport controls and must not be misreported as EOS. */
static void remote_submit_eos(void) {
    if (!g_remote_ipc) return;
    UINT64 generation = g_remote_ipc->stream_generation;
    InterlockedExchange64((volatile LONG64 *)&g_remote_ipc->eos_submitted_generation,
                          (LONG64)generation);
    SetEvent(g_remote_data);
}

static int remote_wait_eos_complete(UINT64 generation) {
    if (!g_remote_ipc || !generation) return 0;
    DWORD deadline = GetTickCount() + 5000u;
    for (;;) {
        remote_sync_stats();
        if (nex_audio_read64(&g_remote_ipc->eos_complete_generation) == generation)
            return 1;
        LONG state = remote_state();
        if (state == NEX_AUDIO_FAILED || state == NEX_AUDIO_CLOSING)
            return 0;
        DWORD now = GetTickCount();
        if ((LONG)(now - deadline) >= 0) {
            g_remote_error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            g_remote_reason = NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST;
            InterlockedExchange((volatile LONG *)&g_remote_ipc->error, (LONG)g_remote_error);
            g_remote_ipc->fallback_reason = g_remote_reason;
            InterlockedExchange((volatile LONG *)&g_remote_ipc->state, NEX_AUDIO_FAILED);
            if (g_remote_data) SetEvent(g_remote_data);
            log_line("audiohost: EOS wait timed out generation=%llu state=%s",
                     (unsigned long long)generation, nex_audio_state_name(state));
            return 0;
        }
        DWORD remaining = deadline - now;
        if (g_remote_data) WaitForSingleObject(g_remote_data, remaining > 20 ? 20 : remaining);
        else Sleep(5);
    }
}

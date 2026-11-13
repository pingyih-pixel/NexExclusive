/* ---- sink lifecycle ----------------------------------------------------- */
/* Reference counting. The control path holds one reference and the drain thread
   holds another; the device objects are released by whoever drops the last one.
   That is what makes abandonment safe: a thread parked inside GetBuffer keeps
   its own generation alive, and nothing is freed underneath it. */
static SinkGen *gen_new(void) {
    SinkGen *g = (SinkGen *)calloc(1, sizeof(SinkGen));
    if (!g) return NULL;
    g->ref = 1;
    g->id  = (UINT64)InterlockedIncrement64((volatile LONG64 *)&g_generation);
    g->stage = STAGE_IDLE;
    return g;
}
static void gen_retain(SinkGen *g) {
    if (g) InterlockedIncrement(&g->ref);
}
/* 闸门句柄的关闭必须只发生一次，而且可能从两条线程发起：控制路径在「放弃」
   这一代时提前放掉（见 sink_stop），或者被放弃的线程自己回来释放这一代。
   用一次原子交换把句柄抢过来，谁抢到谁关。 */
static void gate_close(SinkGen *g) {
    HANDLE h = (HANDLE)InterlockedExchangePointer((PVOID volatile *)&g->endpoint_gate, NULL);
    if (h) CloseHandle(h);
}
static void gen_release(SinkGen *g) {
    if (!g) return;
    if (InterlockedDecrement(&g->ref) > 0) return;
    if (g->real_clock)  { g->real_clock->lpVtbl->Release(g->real_clock);   g->real_clock  = NULL; }
    if (g->real_render) { g->real_render->lpVtbl->Release(g->real_render); g->real_render = NULL; }
    if (g->real)        { g->real->lpVtbl->Release(g->real);               g->real        = NULL; }
    gate_close(g);
    if (g->real_evt)      CloseHandle(g->real_evt);
    free(g);
}

/* Stop the drain thread and drop the generation. The join is a ladder, because
   there are three different reasons for a submission to be outstanding and only
   the first one resolves itself:
     1. waiting for device space - the device is still consuming, so leaving it
        running lets the call return within one buffer period;
     2. the client already stopped the stream - nothing consumes, so Stop it and
        let the call fail fast;
     3. the endpoint is being reconfigured - the call does not return at all, and
        the stream has to be released before it will, which cannot happen while
        the thread is inside it. Hand the generation to the thread and walk
        away; it releases the device objects if it ever comes back. */
static HRESULT sink_stop(void) {
    SinkGen *g = g_sink.gen;
    HRESULT hr = S_OK;
    if (g) {
        InterlockedExchange(&g->running, 0);
        if (g_sink.h_data) SetEvent(g_sink.h_data);
        if (g_sink.h_space) SetEvent(g_sink.h_space);
        if (g->real_evt) SetEvent(g->real_evt);

        DWORD t0 = GetTickCount();
        DWORD wait = g->h_thread ? WaitForSingleObject(g->h_thread, 300) : WAIT_OBJECT_0;
        if (wait != WAIT_OBJECT_0) {
            if (g->real && g->device_started) O_STOP(g->real);
            InterlockedExchange(&g->device_started, 0);
            if (g->real) O_RESET(g->real);
            wait = WaitForSingleObject(g->h_thread, 600);
        }
        DWORD took = GetTickCount() - t0;
        if (wait != WAIT_OBJECT_0) {
            /* Not a fault: the stream is not broken, its device is gone. The
               client keeps its ring, its proxies and its timeline, and the next
               engage builds a new device side. */
            InterlockedExchange(&g->retired, 1);
            log_line("sink: drain thread parked at stage %ld (0=idle 1=padding 2=getbuffer "
                     "3=release 4=start); generation %llu abandoned, the thread releases it "
                     "if it returns", (long)g->stage, (unsigned long long)g->id);
            /* 闸门**不**在这里提前放掉——试过，更糟。
               被放弃的这一代确实已经没有「换掉的那条流」的意义，但它那条独占流还
               开着：设备仍被它占着，闸门说的正是实话。提前放掉之后，下一个 engage
               拿到闸门就直接进 negotiate，对着一台还被占着的设备把 19 个候选格式各
               试 8 次（每个 400 ms），实测 09:46:45 整整耗了 3.5 s 才放弃，客户端
               线程被摁在那儿，最后还是拿不到流。攥着闸门则是立刻失败，客户端马上
               重建，反而更快恢复。真正的仲裁留给系统，闸门只负责「别去撞」。
               于是这一代连同闸门一起等它的线程回来（线程退出时 gen_release 收尾）。 */
            g_sink.gen = NULL;
            gen_release(g);
        } else {
            if (took > 300) log_line("sink: drain join took %lu ms", (unsigned long)took);
            /* Joined, so this Stop cannot strand anyone. */
            if (g->real && g->device_started) hr = O_STOP(g->real);
            /* Cleared even when Stop reports failure: the stream is in an
               unknown state, and the only recovery is to let the next drain
               submit frames and Start again. Leaving the flag set would mute
               the next track. */
            g->device_started = 0;
            {
                HANDLE h = (HANDLE)InterlockedExchangePointer((PVOID volatile *)&g->h_thread, NULL);
                if (h) CloseHandle(h);
            }
            /* The generation stays: a pause is a Stop/Start pair on the same
               stream, and rebuilding the device for it would cost an Activate,
               a negotiation and a buffer of silence every time. */
        }
    }
    g_sink.started = 0;
    /* A seek interrupts the client mid-buffer: it stops without ever releasing
       the frames it reserved. Keeping that reservation makes every later
       GetBuffer return AUDCLNT_E_OUT_OF_ORDER, and the client gives up instead
       of writing the new position - the progress bar freezes and audio dies.
       Stop discards pending audio, so the reservation goes with it. */
    if(g_sink.proxy_render) { g_sink.proxy_render->reserved=0; g_sink.proxy_render->writer_tid=0;
                              g_sink.proxy_render->discarded=1; }
    return hr;
}
static void proxy_release_owned(void) {
    if(g_sink.proxy_render)fr_Release(g_sink.proxy_render);
    if(g_sink.proxy_clock)fc_Release(g_sink.proxy_clock);
    g_sink.proxy_render=NULL;g_sink.proxy_clock=NULL;
}
static HRESULT sink_teardown(void) {
    int was_remote = g_sink.gen && g_sink.gen->remote;
    HRESULT stop=sink_stop();
    /* A generation that survived the stop is the normal case: Stop/Start is how
       a client pauses. Teardown is where it actually goes away. */
    if(g_sink.gen) { gen_release(g_sink.gen); g_sink.gen=NULL; }
    g_sink.engaged=0;
    proxy_release_owned();
    if(g_sink.h_space)CloseHandle(g_sink.h_space);
    if(g_sink.h_data)CloseHandle(g_sink.h_data);
    free(g_sink.ring);free(g_sink.conv);
    if(g_sink.owner) {
        EnterCriticalSection(&g_patch_lock);
        for(int i=0;i<128;i++)if(g_bindings[i].client==g_sink.owner) {
            if(g_bindings[i].dev)g_bindings[i].dev->lpVtbl->Release(g_bindings[i].dev);
            memset(&g_bindings[i],0,sizeof(g_bindings[i]));
        }
        LeaveCriticalSection(&g_patch_lock);
        ((IUnknown *)g_sink.owner)->lpVtbl->Release((IUnknown *)g_sink.owner);
    }
    src_free();scratch_free();
    if (was_remote) remote_close();
    if (g_sink.hifi_map) {
        log_line("hifi verifier: map=%s client_frames=%llu endpoint_frames=%llu "
                 "verified_frames=%llu changed_bytes=%llu dropped_frames=%llu "
                 "underrun_events=%llu converted_frames=%llu "
                 "first_mismatch=%s%llu+%lu client_hash=%016llX endpoint_hash=%016llX",
                 g_sink.hifi_map == 1 ? "client-native" : "converted",
                 (unsigned long long)g_sink.hifi_client_frames,
                 (unsigned long long)g_sink.hifi_endpoint_frames,
                 (unsigned long long)g_sink.hifi_verified_frames,
                 (unsigned long long)g_sink.hifi_changed_bytes,
                 (unsigned long long)g_sink.hifi_dropped_frames,
                 (unsigned long long)g_sink.hifi_underrun_events,
                 (unsigned long long)g_sink.hifi_converted_frames,
                 g_sink.hifi_first_mismatch_frame == ~(UINT64)0 ? "none:" : "",
                 g_sink.hifi_first_mismatch_frame == ~(UINT64)0 ? 0 : g_sink.hifi_first_mismatch_frame,
                 (unsigned long)g_sink.hifi_first_mismatch_byte,
                 (unsigned long long)g_sink.hifi_client_hash,
                 (unsigned long long)g_sink.hifi_endpoint_hash);
    }
    memset(&g_sink,0,sizeof(g_sink));
    log_line("sink: torn down (all references and partial allocations released)");
    return stop;
}

/* Lifecycle entry points call this wrapper so proxy calls cannot overlap the
   free/reset phase. Windows critical sections are recursive, allowing the
   serialized Initialize path to hand off an old stream safely. */
static HRESULT sink_teardown_controlled(void) {
    EnterCriticalSection(&g_control);
    HRESULT hr = sink_teardown();
    LeaveCriticalSection(&g_control);
    return hr;
}

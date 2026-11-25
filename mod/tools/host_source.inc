/* ---- 源规格：读网易云 .uc 缓存头 ----------------------------------------
   网易云的 .uc 缓存是逐字节 XOR 0xA3 的原始音频文件，文件名形如
   <歌曲 id>-<品质档>-<hash>.uc。同一首歌可能同时缓存了几个品质档，
   所以要在候选里挑出当前真正在播的那一个：
     1. 面板读到的当前质量档提示优先，用来区分同采样率的不同位深/码率；
     2. 没有质量提示时，再优先匹配 DLL 最近一次协商到的客户端输入采样率；
     3. 仍无法区分时才使用最近写入时间，并把证据写进状态。
   这里只读文件头（FLAC STREAMINFO / MP3 帧头 / MP4 moov），
   不读取完整曲目，也不改变缓存内容。 */
typedef struct {
    char format[16];
    char encoding[24];
    int rate;
    int channels;
    int bits;
    int kbps;
    int lossless;
    int tier;
    FILETIME mtime;
    char path[MAX_PATH];
} CacheProbe;

static unsigned cache_be16(const unsigned char *p) {
    return ((unsigned)p[0] << 8) | p[1];
}

static unsigned long cache_be32(const unsigned char *p) {
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}

static size_t cache_read_at(FILE *fp, long long off, unsigned char *buf, size_t n) {
    size_t got, i;
    if (off < 0 || _fseeki64(fp, off, SEEK_SET) != 0) return 0;
    got = fread(buf, 1, n, fp);
    for (i = 0; i < got; i++) buf[i] ^= 0xA3;
    return got;
}

static int cache_parse_flac(const unsigned char *d, size_t n, CacheProbe *o) {
    unsigned long long packed = 0;
    int block_len, i;
    if (n < 42 || memcmp(d, "fLaC", 4) != 0) return 0;
    block_len = ((int)d[5] << 16) | ((int)d[6] << 8) | d[7];
    if ((d[4] & 0x7F) != 0 || block_len < 34) return 0;
    for (i = 0; i < 8; i++) packed = (packed << 8) | d[18 + i];
    o->rate = (int)((packed >> 44) & 0xFFFFF);
    o->channels = (int)(((packed >> 41) & 0x7) + 1);
    o->bits = (int)(((packed >> 36) & 0x1F) + 1);
    if (o->bits < 4) return 0;
    snprintf(o->format, sizeof(o->format), "flac");
    snprintf(o->encoding, sizeof(o->encoding), "FLAC");
    o->lossless = 1;
    return 1;
}

static int cache_parse_mp3(FILE *fp, const unsigned char *head, size_t head_n, CacheProbe *o) {
    static const int kbps_v1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const int kbps_v2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const int rates[3][3] = {{44100,48000,32000},{22050,24000,16000},{11025,12000,8000}};
    unsigned char d[4096];
    long long off = 0;
    size_t n, i;
    if (head_n >= 10 && memcmp(head, "ID3", 3) == 0) {
        off = 10 + (((long long)(head[6] & 0x7F) << 21) | ((head[7] & 0x7F) << 14) |
                    ((head[8] & 0x7F) << 7) | (head[9] & 0x7F));
        if (head[5] & 0x10) off += 10;
    }
    n = cache_read_at(fp, off, d, sizeof(d));
    for (i = 0; i + 4 <= n; i++) {
        int ver, layer, bi, si, mode, mpeg1, side, spf;
        if (d[i] != 0xFF || (d[i + 1] & 0xE0) != 0xE0) continue;
        ver = (d[i + 1] >> 3) & 3;          /* 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5 */
        layer = (d[i + 1] >> 1) & 3;        /* 1 = Layer III */
        bi = d[i + 2] >> 4;
        si = (d[i + 2] >> 2) & 3;
        mode = d[i + 3] >> 6;
        if (ver == 1 || layer != 1 || bi == 0 || bi == 15 || si == 3) continue;
        mpeg1 = ver == 3;
        o->rate = rates[mpeg1 ? 0 : (ver == 2 ? 1 : 2)][si];
        o->channels = mode == 3 ? 1 : 2;
        o->kbps = mpeg1 ? kbps_v1[bi] : kbps_v2[bi];
        /* VBR 文件第一帧是 Xing 头，帧头码率没有意义；用总字节/总帧数换算平均码率。
           CBR 文件这里是 "Info"，帧头码率就是真实码率。 */
        side = mpeg1 ? (mode == 3 ? 17 : 32) : (mode == 3 ? 9 : 17);
        spf = mpeg1 ? 1152 : 576;
        if (i + 4 + side + 16 <= n && memcmp(d + i + 4 + side, "Xing", 4) == 0) {
            const unsigned char *x = d + i + 4 + side + 4;
            unsigned long flags = cache_be32(x), frames = 0, bytes = 0;
            x += 4;
            if (flags & 1) { frames = cache_be32(x); x += 4; }
            if ((flags & 2) && x + 4 <= d + n) bytes = cache_be32(x);
            o->kbps = frames && bytes
                ? (int)(((double)bytes * 8.0 * o->rate) / ((double)frames * spf) / 1000.0 + 0.5)
                : 0;
        }
        snprintf(o->format, sizeof(o->format), "mp3");
        snprintf(o->encoding, sizeof(o->encoding), "MP3");
        return 1;
    }
    return 0;
}

/* esds：ES_Descriptor 里的 DecoderConfigDescriptor（tag 0x04）带 objectType 与平均码率 */
static void cache_parse_esds(const unsigned char *p, size_t n, CacheProbe *o) {
    size_t i;
    for (i = 4; i + 2 < n; i++) {
        size_t j = i + 1, k;
        unsigned long avg, max;
        unsigned obj;
        if (p[i] != 0x04) continue;
        for (k = 0; k < 3 && j < n && (p[j] & 0x80); k++) j++;
        j++;                                  /* 长度字段最后一个字节 */
        if (j + 13 > n) continue;
        obj = p[j];
        if (obj != 0x40 && obj != 0x66 && obj != 0x67 && obj != 0x68 &&
            obj != 0x69 && obj != 0x6B) continue;
        max = cache_be32(p + j + 5);
        avg = cache_be32(p + j + 9);
        o->kbps = (int)(((avg ? avg : max) + 500) / 1000);
        if (obj == 0x69 || obj == 0x6B) {
            snprintf(o->format, sizeof(o->format), "mp3");
            snprintf(o->encoding, sizeof(o->encoding), "MP3");
        }
        return;
    }
}

static int cache_walk_mp4(const unsigned char *p, size_t n, int depth, CacheProbe *o) {
    size_t off = 0;
    if (depth > 8) return 0;
    while (off + 8 <= n) {
        unsigned long size = cache_be32(p + off);
        const unsigned char *type = p + off + 4;
        if (size < 8 || size > n - off) return 0;
        if (!memcmp(type, "trak", 4) || !memcmp(type, "mdia", 4) ||
            !memcmp(type, "minf", 4) || !memcmp(type, "stbl", 4)) {
            if (cache_walk_mp4(p + off + 8, size - 8, depth + 1, o)) return 1;
        } else if (!memcmp(type, "stsd", 4) && size >= 16) {
            const unsigned char *e = p + off + 16;      /* 跳过 version/flags + entry_count */
            size_t left = size - 16;
            while (left >= 36) {
                unsigned long esize = cache_be32(e);
                if (esize < 36 || esize > left) break;
                if (!memcmp(e + 4, "mp4a", 4)) {
                    size_t c = 36;
                    o->channels = (int)cache_be16(e + 24);
                    o->rate = (int)(cache_be32(e + 32) >> 16);
                    snprintf(o->format, sizeof(o->format), "aac");
                    snprintf(o->encoding, sizeof(o->encoding), "AAC");
                    while (c + 8 <= esize) {
                        unsigned long csize = cache_be32(e + c);
                        if (csize < 8 || csize > esize - c) break;
                        if (!memcmp(e + c + 4, "esds", 4)) {
                            cache_parse_esds(e + c + 8, csize - 8, o);
                            break;
                        }
                        c += csize;
                    }
                    return o->rate >= 8000 && o->channels >= 1;
                }
                e += esize;
                left -= esize;
            }
        }
        off += size;
    }
    return 0;
}

static int cache_parse_mp4(FILE *fp, CacheProbe *o) {
    long long off = 0;
    int i;
    for (i = 0; i < 32; i++) {
        unsigned char h[16];
        unsigned long long size;
        if (cache_read_at(fp, off, h, sizeof(h)) < 8) return 0;
        size = cache_be32(h);
        if (size == 1) size = ((unsigned long long)cache_be32(h + 8) << 32) | cache_be32(h + 12);
        if (size < 8) return 0;
        if (!memcmp(h + 4, "moov", 4)) {
            unsigned char *moov;
            size_t got;
            int ok;
            if (size > 16u * 1024u * 1024u) return 0;
            moov = (unsigned char *)malloc((size_t)size);
            if (!moov) return 0;
            got = cache_read_at(fp, off, moov, (size_t)size);
            ok = got == size && cache_walk_mp4(moov + 8, (size_t)size - 8, 0, o);
            free(moov);
            return ok;
        }
        off += (long long)size;
    }
    return 0;
}

static int cache_parse_file(const char *path, CacheProbe *o) {
    unsigned char head[128];
    size_t got;
    int ok = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    got = cache_read_at(fp, 0, head, sizeof(head));
    if (got >= 4 && memcmp(head, "fLaC", 4) == 0) ok = cache_parse_flac(head, got, o);
    else if (got >= 8 && memcmp(head + 4, "ftyp", 4) == 0) ok = cache_parse_mp4(fp, o);
    else if (got >= 4) ok = cache_parse_mp3(fp, head, got, o);
    fclose(fp);
    return ok && o->rate >= 8000 && o->rate <= 768000 && o->channels >= 1 && o->channels <= 32;
}

/* DLL 最近一次协商到的客户端输入采样率；没有则 0。 */
static int source_client_input_rate(void) {
    char *tail = read_tail(kSinkLog, 12288);
    int rate = 0;
    char *line = tail;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strstr(line, "=== nexsink attached ===")) rate = 0;
        if (strstr(line, "sink ENGAGED")) {
            const char *in = strstr(line, "in=rate:");
            rate = in ? atoi(in + 8) : 0;
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(tail);
    return rate;
}

static int source_probe_cache(const char *play_id_in, int quality_tier_hint) {
    /* 调用方常传 g_source.play_id 本身，而下面会 source_clear()，先拷一份。 */
    char play_id[sizeof(g_source.play_id)];
    char local_appdata[MAX_PATH] = "";
    char prefix[128] = "";
    char dir[MAX_PATH] = "";
    char pattern[MAX_PATH] = "";
    const char *sep;
    size_t prefix_len;
    WIN32_FIND_DATAA fd;
    HANDLE find;
    CacheProbe best, cand;
    int have = 0, best_score = -1, client_rate;
    const char *best_selection = "cache_mtime_fallback";
    ULONGLONG keep_started = g_source.probe_started_ms;
    ULONGLONG now = GetTickCount64();

    g_source.probe_last_ms = now;
    if (!play_id_in || !source_allowed_id(play_id_in, 96)) return 0;
    snprintf(play_id, sizeof(play_id), "%s", play_id_in);
    sep = strchr(play_id, '_');
    prefix_len = sep ? (size_t)(sep - play_id) : strlen(play_id);
    if (!prefix_len || prefix_len >= sizeof(prefix)) return 0;
    for (size_t i = 0; i < prefix_len; i++)
        if (play_id[i] < '0' || play_id[i] > '9') return 0;
    memcpy(prefix, play_id, prefix_len);
    prefix[prefix_len] = 0;
    if (!GetEnvironmentVariableA("LOCALAPPDATA", local_appdata, sizeof(local_appdata))) return 0;
    if (snprintf(dir, sizeof(dir), "%s\\NetEase\\CloudMusic\\Cache\\Cache", local_appdata) <= 0 ||
        snprintf(pattern, sizeof(pattern), "%s\\%s-*.uc", dir, prefix) <= 0) return 0;

    find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    client_rate = source_client_input_rate();
    memset(&best, 0, sizeof(best));
    do {
        int rate_match, quality_match, score;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        memset(&cand, 0, sizeof(cand));
        if (snprintf(cand.path, sizeof(cand.path), "%s\\%s", dir, fd.cFileName) <= 0) continue;
        if (!cache_parse_file(cand.path, &cand)) continue;
        cand.tier = atoi(fd.cFileName + prefix_len + 1);
        cand.mtime = fd.ftLastWriteTime;
        rate_match = client_rate && cand.rate == client_rate;
        quality_match = quality_tier_hint && cand.tier == quality_tier_hint;
        /* 质量提示来源于当前播放器选择，优先级高于客户端 PCM 采样率：
           源文件可能已经被播放器重采样，单看输入采样率会误选另一档。 */
        score = quality_match ? 4 : (rate_match ? 2 : 0);
        if (!have || score > best_score ||
            (score == best_score && CompareFileTime(&cand.mtime, &best.mtime) > 0)) {
            best = cand;
            best_score = score;
            best_selection = quality_match && rate_match ? "cache_quality_and_rate"
                : quality_match ? "cache_quality_hint"
                : rate_match ? "cache_rate_match" : "cache_mtime_fallback";
            have = 1;
        }
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    if (!have) return 0;

    {
        int changed = strcmp(g_source.probe_path, best.path) != 0 ||
                      _stricmp(g_source.status, "observed") != 0 ||
                      strcmp(g_source.play_id, play_id) != 0;
        source_clear();
        snprintf(g_source.status, sizeof(g_source.status), "observed");
        snprintf(g_source.play_id, sizeof(g_source.play_id), "%s", play_id);
        snprintf(g_source.format, sizeof(g_source.format), "%s", best.format);
        snprintf(g_source.encoding, sizeof(g_source.encoding), "%s", best.encoding);
        snprintf(g_source.probe_path, sizeof(g_source.probe_path), "%s", best.path);
        g_source.rate = best.rate;
        g_source.channels = best.channels;
        g_source.valid_bits = best.bits;
        g_source.container_bits = best.bits;
        g_source.sample_bytes = (best.bits + 7) / 8;
        g_source.lossless = best.lossless;
        g_source.bitrate_kbps = best.kbps;
        g_source.quality_tier = best.tier;
        g_source.quality_tier_hint = quality_tier_hint;
        snprintf(g_source.selection, sizeof(g_source.selection), "%s", best_selection);
        g_source.audio_generation = source_current_generation();
        g_source.updated_at_ms = now;
        g_source.probe_started_ms = keep_started;
        g_source.probe_last_ms = now;
        if (changed)
            host_log("source cache probe: observed playId=%s %s rate=%d channels=%d bits=%d kbps=%d tier=%d client_rate=%d selection=%s file=%s",
                     play_id, best.encoding, best.rate, best.channels, best.bits, best.kbps,
                     best.tier, client_rate, best_selection, best.path);
    }
    return 1;
}

/* 换歌后的确认窗口：缓存文件可能晚于 playId 出现，DLL 也可能稍后才协商出
   新的输入采样率；窗口内每 1.5 秒重新挑一次，窗口结束仍没有就判为未取得。 */
#define SOURCE_PROBE_WINDOW_MS 30000
#define SOURCE_PROBE_INTERVAL_MS 1500

static void source_begin_probe(const char *play_id, int quality_tier_hint) {
    source_clear();
    snprintf(g_source.status, sizeof(g_source.status), "pending");
    snprintf(g_source.play_id, sizeof(g_source.play_id), "%s", play_id);
    g_source.audio_generation = source_current_generation();
    g_source.updated_at_ms = GetTickCount64();
    g_source.probe_started_ms = g_source.updated_at_ms;
    g_source.quality_tier_hint = quality_tier_hint;
    source_probe_cache(play_id, quality_tier_hint);
}

static void source_refresh_probe(void) {
    ULONGLONG now = GetTickCount64();
    if (!g_source.probe_started_ms || !g_source.play_id[0]) return;
    if (now - g_source.probe_started_ms > SOURCE_PROBE_WINDOW_MS) {
        if (!_stricmp(g_source.status, "pending")) {
            snprintf(g_source.status, sizeof(g_source.status), "unavailable");
            g_source.updated_at_ms = now;
            host_log("source cache probe: no cache header for playId=%s", g_source.play_id);
        }
        return;
    }
    if (now - g_source.probe_last_ms < SOURCE_PROBE_INTERVAL_MS) return;
    source_probe_cache(g_source.play_id, g_source.quality_tier_hint);
}

static int source_update_from_json(const char *body) {
    char status[16] = "", play_id[128] = "", format[16] = "";
    int sample_bytes = 0, rate = 0, channels = 0, quality_tier_hint = 0;
    if (!json_get_string(body, "source_status", status, sizeof(status))) return 400;
    if (!_stricmp(status, "reset")) {
        source_clear();
        return 200;
    }
    if (!json_get_string(body, "source_play_id", play_id, sizeof(play_id)) ||
        !source_allowed_id(play_id, 96)) return 400;
    if (json_get_int(body, "source_quality_tier_hint", &quality_tier_hint) &&
        !source_quality_tier_allowed(quality_tier_hint)) return 400;
    if (!_stricmp(status, "pending")) {
        source_clear();
        snprintf(g_source.status, sizeof(g_source.status), "pending");
        snprintf(g_source.play_id, sizeof(g_source.play_id), "%s", play_id);
        g_source.quality_tier_hint = quality_tier_hint;
        g_source.audio_generation = source_current_generation();
        g_source.updated_at_ms = GetTickCount64();
        return 200;
    }
    if (!_stricmp(status, "probe") || !_stricmp(status, "unavailable")) {
        /* 面板在换歌时报告 playId 和当前质量提示；源规格由宿主读缓存头取得。
           同一 playId 已经在确认窗口内或已取得结果时不重来，避免清掉 observed。 */
        if (!strcmp(g_source.play_id, play_id) &&
            g_source.quality_tier_hint == quality_tier_hint &&
            (g_source.probe_started_ms || !_stricmp(g_source.status, "observed"))) return 200;
        source_begin_probe(play_id, quality_tier_hint);
        return 200;
    }
    if (_stricmp(status, "observed") != 0 ||
        !json_get_string(body, "source_format", format, sizeof(format)) ||
        !source_format_allowed(format) ||
        !json_get_int(body, "source_sample_bytes", &sample_bytes) ||
        !json_get_int(body, "source_rate", &rate) ||
        !json_get_int(body, "source_channels", &channels) ||
        sample_bytes < 0 || sample_bytes > 4 ||
        (source_format_is_lossless(format) && !source_bits_from_bytes(sample_bytes)) ||
        rate < 8000 || rate > 768000 ||
        channels < 1 || channels > 32 || strcmp(g_source.play_id, play_id) != 0) return 409;
    source_clear();
    snprintf(g_source.status, sizeof(g_source.status), "observed");
    snprintf(g_source.play_id, sizeof(g_source.play_id), "%s", play_id);
    snprintf(g_source.format, sizeof(g_source.format), "%s", format);
    g_source.rate = rate;
    g_source.channels = channels;
    g_source.sample_bytes = sample_bytes;
    g_source.lossless = source_format_is_lossless(format);
    g_source.valid_bits = g_source.lossless ? source_bits_from_bytes(sample_bytes) : 0;
    g_source.container_bits = g_source.valid_bits;
    if (!_stricmp(format, "flac")) snprintf(g_source.encoding, sizeof(g_source.encoding), "FLAC");
    else if (!_stricmp(format, "pcm") || !_stricmp(format, "wav")) snprintf(g_source.encoding, sizeof(g_source.encoding), "整数 PCM");
    else source_uppercase(g_source.encoding, sizeof(g_source.encoding), format);
    g_source.audio_generation = source_current_generation();
    g_source.updated_at_ms = GetTickCount64();
    return 200;
}

/* 源规格缓存头解析的离线测试。
   把宿主源码整体编进来（main 改名），用合成的 XOR 0xA3 缓存文件验证：
     - FLAC STREAMINFO / MP3（CBR Info 与 VBR Xing）/ MP4 AAC esds 码率解析；
     - 同一首歌有多个品质档时，按客户端输入采样率选择，其次按最近写入。
   传入一个目录参数时，额外把该目录里的真实 .uc 缓存逐个解析并打印（不做断言）。 */
#define main nexhost_main_for_test
#include "../tools/nexhost.c"
#undef main

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static void put_be32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static void write_xor(const char *path, const unsigned char *d, size_t n) {
    FILE *fp = fopen(path, "wb");
    for (size_t i = 0; i < n; i++) fputc(d[i] ^ 0xA3, fp);
    fclose(fp);
}

static size_t make_flac(unsigned char *d, int rate, int ch, int bits) {
    unsigned long long packed;
    memset(d, 0, 64);
    memcpy(d, "fLaC", 4);
    d[4] = 0x80; d[7] = 34;
    packed = ((unsigned long long)rate << 44) | ((unsigned long long)(ch - 1) << 41) |
             ((unsigned long long)(bits - 1) << 36);
    for (int i = 0; i < 8; i++) d[18 + i] = (unsigned char)(packed >> (56 - 8 * i));
    return 64;
}

static size_t make_mp3(unsigned char *d, int vbr) {
    size_t off = 10 + 20;
    memset(d, 0, 512);
    memcpy(d, "ID3\x03\x00\x00", 6);
    d[9] = 20;                                   /* ID3v2 大小 20（syncsafe） */
    d[off] = 0xFF; d[off + 1] = 0xFB;            /* MPEG1 Layer III, 无 CRC */
    d[off + 2] = 0xE0;                            /* 320 kbps, 44.1 kHz */
    d[off + 3] = 0x00;                            /* stereo */
    if (vbr) {
        unsigned char *x = d + off + 4 + 32;
        memcpy(x, "Xing", 4);
        put_be32(x + 4, 3);
        put_be32(x + 8, 10000);                   /* 帧数 */
        /* 平均 192 kbps：bytes = 192000/8 * frames*1152/44100 */
        put_be32(x + 12, (unsigned long)(24000.0 * 10000 * 1152 / 44100));
    } else {
        memcpy(d + off + 4 + 32, "Info", 4);
    }
    return 512;
}

static size_t atom(unsigned char *d, const char *type, const unsigned char *payload, size_t n) {
    put_be32(d, (unsigned long)(n + 8));
    memcpy(d + 4, type, 4);
    if (n) memcpy(d + 8, payload, n);
    return n + 8;
}

static size_t make_mp4(unsigned char *d, int rate, unsigned long avg) {
    unsigned char esds[64], mp4a[128], stsd[192], a[256], b[256];
    size_t n, m;
    memset(esds, 0, sizeof(esds));
    /* version/flags, ES_Descriptor(0x03) -> DecoderConfigDescriptor(0x04) */
    n = 4;
    esds[n++] = 0x03; esds[n++] = 0x80; esds[n++] = 0x80; esds[n++] = 0x80; esds[n++] = 0x22;
    esds[n++] = 0x00; esds[n++] = 0x01; esds[n++] = 0x00;
    esds[n++] = 0x04; esds[n++] = 0x80; esds[n++] = 0x80; esds[n++] = 0x80; esds[n++] = 0x14;
    esds[n++] = 0x40; esds[n++] = 0x15; esds[n++] = 0; esds[n++] = 0; esds[n++] = 0;
    put_be32(esds + n, avg + 1000); n += 4;
    put_be32(esds + n, avg); n += 4;
    memset(mp4a, 0, sizeof(mp4a));
    mp4a[7] = 1;                                  /* data_reference_index */
    mp4a[17] = 2;                                 /* channelcount（entry 内偏移 24） */
    mp4a[19] = 16;
    put_be32(mp4a + 24, (unsigned long)rate << 16);
    m = 28 + atom(mp4a + 28, "esds", esds, n);
    memset(stsd, 0, 8);
    stsd[7] = 1;
    n = 8 + atom(stsd + 8, "mp4a", mp4a, m);
    n = atom(a, "stsd", stsd, n);
    n = atom(b, "stbl", a, n);
    n = atom(a, "minf", b, n);
    n = atom(b, "mdia", a, n);
    n = atom(a, "trak", b, n);
    m = atom(d, "ftyp", (const unsigned char *)"isom\0\0\0\0", 8);
    m += atom(d + m, "moov", a, n);
    m += atom(d + m, "mdat", (const unsigned char *)"\0\0\0\0\0\0\0\0", 8);
    return m;
}

static void set_mtime(const char *path, int minutes_ago) {
    HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)minutes_ago * 60ull * 10000000ull;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
}

static void dump_real_cache(const char *dir) {
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    int ok = 0, bad = 0;
    snprintf(pattern, sizeof(pattern), "%s\\*.uc", dir);
    f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) { printf("no .uc in %s\n", dir); return; }
    do {
        CacheProbe p;
        memset(&p, 0, sizeof(p));
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        if (cache_parse_file(path, &p)) {
            ok++;
            printf("  %-28.28s %-4s %6d Hz %2d ch %2d bit %4d kbps\n",
                   fd.cFileName, p.encoding, p.rate, p.channels, p.bits, p.kbps);
        } else {
            bad++;
            printf("  %-28.28s (unparsed)\n", fd.cFileName);
        }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    printf("real cache: %d parsed, %d unparsed\n", ok, bad);
}

int main(int argc, char **argv) {
    char tmp[MAX_PATH], root[MAX_PATH], cache[MAX_PATH], path[MAX_PATH];
    unsigned char buf[4096];
    size_t n;
    CacheProbe p;

    if (argc > 1) { dump_real_cache(argv[1]); return 0; }

    InitializeCriticalSection(&g_log_lock);
    GetTempPathA(sizeof(tmp), tmp);
    snprintf(root, sizeof(root), "%snex-source-probe-%lu", tmp, GetCurrentProcessId());
    CreateDirectoryA(root, NULL);
    snprintf(path, sizeof(path), "%s\\NetEase", root); CreateDirectoryA(path, NULL);
    snprintf(path, sizeof(path), "%s\\NetEase\\CloudMusic", root); CreateDirectoryA(path, NULL);
    snprintf(path, sizeof(path), "%s\\NetEase\\CloudMusic\\Cache", root); CreateDirectoryA(path, NULL);
    snprintf(cache, sizeof(cache), "%s\\NetEase\\CloudMusic\\Cache\\Cache", root); CreateDirectoryA(cache, NULL);
    SetEnvironmentVariableA("LOCALAPPDATA", root);
    snprintf(kSinkLog, sizeof(kSinkLog), "%s\\sink.log", root);
    snprintf(kHostLog, sizeof(kHostLog), "%s\\host.log", root);

    /* 单文件解析 */
    snprintf(path, sizeof(path), "%s\\1-999-a.uc", cache);
    n = make_flac(buf, 96000, 2, 24); write_xor(path, buf, n);
    memset(&p, 0, sizeof(p));
    CHECK(cache_parse_file(path, &p) && p.rate == 96000 && p.bits == 24 && p.channels == 2 &&
          p.lossless && !strcmp(p.encoding, "FLAC"), "flac parse");

    snprintf(path, sizeof(path), "%s\\2-320-a.uc", cache);
    n = make_mp3(buf, 0); write_xor(path, buf, n);
    memset(&p, 0, sizeof(p));
    CHECK(cache_parse_file(path, &p) && p.rate == 44100 && p.kbps == 320 && !p.lossless &&
          !strcmp(p.encoding, "MP3"), "mp3 cbr parse (rate=%d kbps=%d)", p.rate, p.kbps);

    snprintf(path, sizeof(path), "%s\\3-320-a.uc", cache);
    n = make_mp3(buf, 1); write_xor(path, buf, n);
    memset(&p, 0, sizeof(p));
    CHECK(cache_parse_file(path, &p) && p.kbps == 192, "mp3 vbr Xing average (kbps=%d)", p.kbps);

    snprintf(path, sizeof(path), "%s\\4-320-a.uc", cache);
    n = make_mp4(buf, 48000, 256000); write_xor(path, buf, n);
    memset(&p, 0, sizeof(p));
    CHECK(cache_parse_file(path, &p) && p.rate == 48000 && p.channels == 2 && p.kbps == 256 &&
          !strcmp(p.encoding, "AAC"), "mp4 aac parse (rate=%d ch=%d kbps=%d)", p.rate, p.channels, p.kbps);

    snprintf(path, sizeof(path), "%s\\5-999-a.uc", cache);
    memset(buf, 0x11, 256); write_xor(path, buf, 256);
    memset(&p, 0, sizeof(p));
    CHECK(!cache_parse_file(path, &p), "garbage rejected");

    /* 多品质档选择：9-1999 = 96k/24（较旧），9-999 = 48k/16（较新） */
    snprintf(path, sizeof(path), "%s\\9-1999-old.uc", cache);
    n = make_flac(buf, 96000, 2, 24); write_xor(path, buf, n); set_mtime(path, 60);
    snprintf(path, sizeof(path), "%s\\9-999-new.uc", cache);
    n = make_flac(buf, 48000, 2, 16); write_xor(path, buf, n); set_mtime(path, 1);

    DeleteFileA(kSinkLog);
    source_begin_probe("9_ABC", 0);
    CHECK(!strcmp(g_source.status, "observed") && g_source.rate == 48000 && g_source.quality_tier == 999,
          "no client rate -> newest file (rate=%d tier=%d)", g_source.rate, g_source.quality_tier);

    {
        FILE *log = fopen(kSinkLog, "wb");
        fputs("=== nexsink attached ===\r\n"
              "10:00:00.000 [1] sink ENGAGED  generation=3  in=rate:96000/cont:32/valid:24/ch:2 -> out=rate:96000/cont:32.24\r\n", log);
        fclose(log);
    }
    source_begin_probe("9_DEF", 0);
    CHECK(g_source.rate == 96000 && g_source.valid_bits == 24 && g_source.quality_tier == 1999,
          "client rate 96k -> matching tier wins over newer file (rate=%d tier=%d)",
          g_source.rate, g_source.quality_tier);

    /* 同采样率的不同档位必须使用当前质量提示，不能退回文件时间。 */
    snprintf(path, sizeof(path), "%s\\10-999-old.uc", cache);
    n = make_flac(buf, 44100, 2, 16); write_xor(path, buf, n); set_mtime(path, 60);
    snprintf(path, sizeof(path), "%s\\10-1999-new.uc", cache);
    n = make_flac(buf, 44100, 2, 24); write_xor(path, buf, n); set_mtime(path, 1);
    CHECK(source_update_from_json("{\"source_status\":\"probe\",\"source_play_id\":\"10_HINT\",\"source_quality_tier_hint\":999}") == 200 &&
          g_source.rate == 44100 && g_source.valid_bits == 16 && g_source.quality_tier == 999,
          "quality hint 999 must select the 16-bit tier (rate=%d bits=%d tier=%d)",
          g_source.rate, g_source.valid_bits, g_source.quality_tier);
    CHECK(source_update_from_json("{\"source_status\":\"probe\",\"source_play_id\":\"10_HINT2\",\"source_quality_tier_hint\":1999}") == 200 &&
          g_source.rate == 44100 && g_source.valid_bits == 24 && g_source.quality_tier == 1999,
          "quality hint 1999 must select the 24-bit tier (rate=%d bits=%d tier=%d)",
          g_source.rate, g_source.valid_bits, g_source.quality_tier);

    snprintf(path, sizeof(path), "%s\\11-999-old.uc", cache);
    n = make_flac(buf, 44100, 2, 16); write_xor(path, buf, n); set_mtime(path, 60);
    snprintf(path, sizeof(path), "%s\\11-3999-new.uc", cache);
    n = make_flac(buf, 44100, 2, 24); write_xor(path, buf, n); set_mtime(path, 1);
    CHECK(source_update_from_json("{\"source_status\":\"probe\",\"source_play_id\":\"11_HINT\",\"source_quality_tier_hint\":3999}") == 200 &&
          g_source.quality_tier == 3999 && g_source.valid_bits == 24,
          "extended quality hint 3999 must select the matching tier (bits=%d tier=%d)",
          g_source.valid_bits, g_source.quality_tier);

    /* PCM 的样本宽度为零时必须拒绝，不能让状态接口发布 0 bit。 */
    CHECK(source_update_from_json("{\"source_status\":\"probe\",\"source_play_id\":\"pcm_X\"}") == 200 &&
          source_update_from_json("{\"source_status\":\"observed\",\"source_play_id\":\"pcm_X\",\"source_format\":\"pcm\",\"source_sample_bytes\":0,\"source_rate\":44100,\"source_channels\":2}") == 409,
          "PCM zero sample width must be rejected");
    CHECK(source_update_from_json("{\"source_status\":\"probe\",\"source_play_id\":\"pcm_X\"}") == 200 &&
          source_update_from_json("{\"source_status\":\"observed\",\"source_play_id\":\"pcm_X\",\"source_format\":\"pcm\",\"source_sample_bytes\":2,\"source_rate\":44100,\"source_channels\":2}") == 200 &&
          g_source.valid_bits == 16 && !strcmp(g_source.encoding, "整数 PCM"),
          "PCM two-byte samples must map to 16 bits (bits=%d encoding=%s)",
          g_source.valid_bits, g_source.encoding);

    /* 同一 playId 的 probe 请求不重来；换 playId 才重新挑 */
    CHECK(source_update_from_json("{\"source_status\":\"probe\",\"source_play_id\":\"9_DEF\"}") == 200 &&
          !strcmp(g_source.status, "observed"), "same playId probe keeps observed");

    /* 缓存文件还没出现：pending，文件出现后在窗口内补上 */
    source_update_from_json("{\"source_status\":\"probe\",\"source_play_id\":\"77_X\"}");
    CHECK(!strcmp(g_source.status, "pending"), "missing cache -> pending (%s)", g_source.status);
    snprintf(path, sizeof(path), "%s\\77-320-a.uc", cache);
    n = make_mp4(buf, 44100, 160000); write_xor(path, buf, n);
    g_source.probe_last_ms = 0;
    source_refresh_probe();
    CHECK(!strcmp(g_source.status, "observed"), "late cache file picked up");
    CHECK(g_source.bitrate_kbps == 160 && !strcmp(g_source.encoding, "AAC") && !strcmp(g_source.play_id, "77_X"),
          "late cache aac kbps=%d playId=%s", g_source.bitrate_kbps, g_source.play_id);

    /* 窗口过期仍没有缓存 -> unavailable */
    source_update_from_json("{\"source_status\":\"probe\",\"source_play_id\":\"88_X\"}");
    g_source.probe_started_ms -= SOURCE_PROBE_WINDOW_MS + 1;
    source_refresh_probe();
    CHECK(!strcmp(g_source.status, "unavailable"), "window expiry -> unavailable (%s)", g_source.status);

    /* 非数字歌曲 id 不拼进路径 */
    source_begin_probe("..\\x_1", 0);
    CHECK(strcmp(g_source.status, "observed") != 0, "non-numeric id rejected");

    if (!g_fail) printf("PASS source probe: FLAC/MP3/AAC headers, tier selection, probe window\n");
    return g_fail ? 1 : 0;
}

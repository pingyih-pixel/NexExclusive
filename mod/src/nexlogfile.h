/* nexlogfile.h - keep the three logs (sink.log, host.log, NexAudioHost.log)
   bounded and readable across days.

   They used to grow forever (sink.log reached 2.2 MB on the author's machine
   within two weeks) and their HH:MM:SS.mmm stamps could not tell one day from
   the next. Line format stays as it is - the host parses lines by content -
   and two things are added:
     - before a write, a log larger than NEX_LOG_MAX_BYTES is renamed to
       <name>.1 (replacing an older one), so at most two files exist;
     - when the calendar day changes, a "---- YYYY-MM-DD ----" line is
       written first, also as the first line of a fresh file.
   Header-only, used by the module, the host and the separate audio host. */
#ifndef NEX_LOGFILE_H
#define NEX_LOGFILE_H

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#ifndef NEX_HDR_UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define NEX_HDR_UNUSED __attribute__((unused))
#else
#define NEX_HDR_UNUSED
#endif
#endif

#define NEX_LOG_MAX_BYTES (4u * 1024u * 1024u)

typedef struct { WORD year, month, day; } NexLogDay;

/* Returns 1 when the file was rotated (the caller then writes a day marker). */
static NEX_HDR_UNUSED int nex_log_rotate_a(const char *path, unsigned long max_bytes) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!path || !path[0] || !GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
    if (fad.nFileSizeHigh == 0 && fad.nFileSizeLow <= max_bytes) return 0;
    char old[MAX_PATH + 4];
    _snprintf(old, sizeof(old), "%s.1", path);
    old[sizeof(old) - 1] = 0;
    return MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
}

static NEX_HDR_UNUSED int nex_log_rotate_w(const WCHAR *path, unsigned long max_bytes) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!path || !path[0] || !GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    if (fad.nFileSizeHigh == 0 && fad.nFileSizeLow <= max_bytes) return 0;
    WCHAR old[MAX_PATH + 4];
    _snwprintf(old, MAX_PATH + 4, L"%ls.1", path);
    old[MAX_PATH + 3] = 0;
    return MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
}

/* Write the day marker when `now` is a different day from *last (or the file
   was just rotated); updates *last. */
static NEX_HDR_UNUSED void nex_log_day_marker(FILE *f, NexLogDay *last, const SYSTEMTIME *now,
                                              int force) {
    if (!f) return;
    if (!force && last->year == now->wYear && last->month == now->wMonth && last->day == now->wDay)
        return;
    fprintf(f, "---- %04u-%02u-%02u ----\n", now->wYear, now->wMonth, now->wDay);
    last->year = now->wYear;
    last->month = now->wMonth;
    last->day = now->wDay;
}

#endif

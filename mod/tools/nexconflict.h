/* nexconflict.h - what the host does when another application wants to sound
   while the client holds the device exclusively (ROADMAP 5c).

   The signal is the application's media session (nexsmtc.h) turning Playing:
   while the device is held, that application gets no device and the audio
   session layer says nothing, but its media session still reports Playing.

   The listener is asked, with a question that depends on the kind:
     - another music player: hand exclusive over to it? Yes pauses the client
       (through its own media session) and gives the device back, so the
       other player can open it exclusively itself;
     - anything else (a browser, a video, a game): close exclusive and play
       together? Yes keeps the client playing, shared, next to it.
   "Remember" stores the answer per application (apps.cfg beside the host),
   so the host learns a preference list the way RateSync keeps a player
   priority: a remembered yes acts at once, a remembered no never asks again.
   No answer within the timeout counts as no, this time only.
   Exclusive comes back once the other application has stopped playing for
   the quiet window, at the client's next resume (the module decides that
   moment; the host only lifts the "stay shared" command).

   Pure functions only: no Windows calls beyond wide-string helpers, so the
   host test exercises every branch. */
#ifndef NEX_CONFLICT_H
#define NEX_CONFLICT_H

#ifndef NEX_HDR_UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define NEX_HDR_UNUSED __attribute__((unused))
#else
#define NEX_HDR_UNUSED
#endif
#endif

#include <wchar.h>
#include <wctype.h>
#include <string.h>

enum { NEX_APP_SELF = 0, NEX_APP_PLAYER = 1, NEX_APP_OTHER = 2 };
enum { NEX_CONFLICT_NONE = 0, NEX_CONFLICT_YIELD = 1, NEX_CONFLICT_ASK = 2, NEX_CONFLICT_KEEP = 3 };
enum { NEX_REMEMBER_NONE = 0, NEX_REMEMBER_YIELD = 1, NEX_REMEMBER_KEEP = 2 };

#define NEX_CONFLICT_QUIET_MS       10000ul  /* other app stopped this long: exclusive again */
#define NEX_CONFLICT_PROMPT_TIMEOUT 30000u   /* unanswered prompt = keep, this time */

/* Case-insensitive substring search on wide strings. */
static NEX_HDR_UNUSED int nex_wcs_contains_ci(const wchar_t *hay, const wchar_t *needle) {
    if (!hay || !needle || !*needle) return 0;
    size_t n = wcslen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && towlower(hay[i]) == towlower(needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* Music players, matched against the SourceAppUserModelId (packaged apps
   carry their package name, desktop apps usually the executable name). */
static const struct { const wchar_t *match, *name; } kNexPlayers[] = {
    {L"QQMusic", L"QQ 音乐"}, {L"Spotify", L"Spotify"}, {L"AppleMusic", L"Apple Music"},
    {L"foobar2000", L"foobar2000"}, {L"MusicBee", L"MusicBee"}, {L"AIMP", L"AIMP"},
    {L"KuGou", L"酷狗音乐"}, {L"Kuwo", L"酷我音乐"}, {L"TIDAL", L"TIDAL"},
    {L"Qobuz", L"Qobuz"}, {L"AmazonMusic", L"Amazon Music"}, {L"Deezer", L"Deezer"},
    {L"ZuneMusic", L"媒体播放器"}, {L"Roon", L"Roon"}, {L"Dopamine", L"Dopamine"},
    {L"Winamp", L"Winamp"}, {L"MediaMonkey", L"MediaMonkey"}, {L"lx-music", L"洛雪音乐"},
    {L"YesPlayMusic", L"YesPlayMusic"}, {L"Listen1", L"Listen1"},
};
/* The client itself never counts as "another application". */
static const wchar_t *kNexSelf[] = { L"cloudmusic", L"NetEase", L"163music" };

static NEX_HDR_UNUSED int nex_app_kind(const wchar_t *app) {
    for (size_t i = 0; i < sizeof(kNexSelf) / sizeof(kNexSelf[0]); i++)
        if (nex_wcs_contains_ci(app, kNexSelf[i])) return NEX_APP_SELF;
    for (size_t i = 0; i < sizeof(kNexPlayers) / sizeof(kNexPlayers[0]); i++)
        if (nex_wcs_contains_ci(app, kNexPlayers[i].match)) return NEX_APP_PLAYER;
    return NEX_APP_OTHER;
}

/* A short name for the notification: known players and browsers by name,
   anything else by the part of its id before the first '.', '!' or '_'. */
static NEX_HDR_UNUSED void nex_app_display_name(const wchar_t *app, wchar_t *out, size_t cap) {
    static const struct { const wchar_t *match, *name; } browsers[] = {
        {L"Chrome", L"Chrome"}, {L"MicrosoftEdge", L"Edge"}, {L"msedge", L"Edge"},
        {L"Firefox", L"Firefox"}, {L"Opera", L"Opera"}, {L"Brave", L"Brave"},
    };
    if (!cap) return;
    out[0] = 0;
    for (size_t i = 0; i < sizeof(kNexPlayers) / sizeof(kNexPlayers[0]); i++)
        if (nex_wcs_contains_ci(app, kNexPlayers[i].match)) {
            wcsncpy(out, kNexPlayers[i].name, cap - 1); out[cap - 1] = 0; return;
        }
    for (size_t i = 0; i < sizeof(browsers) / sizeof(browsers[0]); i++)
        if (nex_wcs_contains_ci(app, browsers[i].match)) {
            wcsncpy(out, browsers[i].name, cap - 1); out[cap - 1] = 0; return;
        }
    size_t n = 0;
    while (app && app[n] && app[n] != L'.' && app[n] != L'!' && app[n] != L'_' && n + 1 < cap) {
        out[n] = app[n];
        n++;
    }
    out[n] = 0;
    if (!n) { wcsncpy(out, L"另一个应用", cap - 1); out[cap - 1] = 0; }
}

/* What to do about an application whose media session is Playing while the
   client holds the device exclusively, given what the listener said before. */
static NEX_HDR_UNUSED int nex_conflict_action(int kind, int remembered) {
    if (kind == NEX_APP_SELF) return NEX_CONFLICT_NONE;
    if (remembered == NEX_REMEMBER_YIELD) return NEX_CONFLICT_YIELD;
    if (remembered == NEX_REMEMBER_KEEP) return NEX_CONFLICT_KEEP;
    return NEX_CONFLICT_ASK;
}

/* apps.cfg: one remembered answer per line, "yield=<app id>" or
   "keep=<app id>", app ids as the media session reports them (UTF-8). The
   last line for an app wins, so appending is enough to change an answer. */
static NEX_HDR_UNUSED int nex_remembered_choice(const char *text, const char *app) {
    int found = NEX_REMEMBER_NONE;
    if (!text || !app || !*app) return found;
    size_t alen = strlen(app);
    for (const char *line = text; *line; ) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) len--;
        int kind = NEX_REMEMBER_NONE;
        size_t skip = 0;
        if (len > 6 && strncmp(line, "yield=", 6) == 0) { kind = NEX_REMEMBER_YIELD; skip = 6; }
        else if (len > 5 && strncmp(line, "keep=", 5) == 0) { kind = NEX_REMEMBER_KEEP; skip = 5; }
        if (kind && len - skip == alen && strncmp(line + skip, app, alen) == 0) found = kind;
        if (!nl) break;
        line = nl + 1;
    }
    return found;
}

/* After the host closed exclusive for an application: lift the command once
   no other application has been playing for the quiet window. A manual close
   from the tray is the listener's and is never lifted automatically. */
static NEX_HDR_UNUSED int nex_conflict_release_due(int manual, int others_playing, unsigned long others_quiet_ms) {
    return !manual && !others_playing && others_quiet_ms >= NEX_CONFLICT_QUIET_MS;
}

#endif

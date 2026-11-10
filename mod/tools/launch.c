// nexlaunch - start the client with a DevTools port, detached from the caller.
//
// The client is a CEF host: --remote-debugging-port is read once during
// CefInitialize, so the switch has to be on the process command line. Started
// from a shell that lives inside a job object, the child would be reaped when
// that shell exits; explorer.exe is used as the parent instead.
#include <windows.h>

static const wchar_t kExe[] = L"D:\\Installed_Program\\CloudMusic\\cloudmusic.exe";
static const wchar_t kDir[] = L"D:\\Installed_Program\\CloudMusic";
static const wchar_t kArgs[] = L"\"D:\\Installed_Program\\CloudMusic\\cloudmusic.exe\" --remote-debugging-port=9333";

int main(void) {

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    wchar_t buf[512];
    lstrcpynW(buf, kArgs, 512);

    /* Breakaway is tried first: the calling shell may live inside a job object
       that reaps its children. ERROR_ACCESS_DENIED means the job forbids it. */
    DWORD flags = CREATE_BREAKAWAY_FROM_JOB;
    if (!CreateProcessW(kExe, buf, NULL, NULL, FALSE, flags, NULL, kDir, &si, &pi)) {
        DWORD first = GetLastError();
        flags = 0;
        if (!CreateProcessW(kExe, buf, NULL, NULL, FALSE, flags, NULL, kDir, &si, &pi))
            return (int)(first ? first : GetLastError());
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

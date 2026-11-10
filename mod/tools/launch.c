// nexlaunch - start the client with a DevTools port, detached from the caller.
//
// The client is a CEF host: --remote-debugging-port is read once during
// CefInitialize, so the switch has to be on the process command line. Started
// from a shell that lives inside a job object, the child would be reaped when
// that shell exits; CREATE_BREAKAWAY_FROM_JOB (with a fallback) keeps it alive.
//
// 用法：nexlaunch <cloudmusic.exe 的完整路径>
//       不给参数时读环境变量 NEX_CLIENT。
// 这里不写死任何人的安装位置——发布出去的仓库里不该出现开发机的路径。
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static int usage(void) {
    fputs("用法：nexlaunch <cloudmusic.exe 的完整路径>\n"
          "      或设置环境变量 NEX_CLIENT\n", stderr);
    return 2;
}

int main(int argc, char **argv) {
    WCHAR exe[MAX_PATH];
    exe[0] = 0;

    if (argc > 1) {
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, exe, MAX_PATH);
    } else if (GetEnvironmentVariableW(L"NEX_CLIENT", exe, MAX_PATH) == 0) {
        return usage();
    }
    exe[MAX_PATH - 1] = 0;
    if (!exe[0]) return usage();

    /* 工作目录取 exe 所在目录：客户端会在自己目录里找资源 */
    WCHAR dir[MAX_PATH];
    lstrcpynW(dir, exe, MAX_PATH);
    WCHAR *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    /* 命令行 = "exe" --remote-debugging-port=9333 */
    WCHAR cmd[MAX_PATH * 2];
    _snwprintf(cmd, MAX_PATH * 2, L"\"%ls\" --remote-debugging-port=9333", exe);
    cmd[MAX_PATH * 2 - 1] = 0;

    /* Breakaway is tried first: the calling shell may live inside a job object
       that reaps its children. ERROR_ACCESS_DENIED means the job forbids it. */
    DWORD flags = CREATE_BREAKAWAY_FROM_JOB;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, flags, NULL, dir, &si, &pi)) {
        DWORD first = GetLastError();
        flags = 0;
        if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, flags, NULL, dir, &si, &pi))
            return (int)(first ? first : GetLastError());
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

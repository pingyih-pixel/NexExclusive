// jobcheck - 判断子进程能否用 CREATE_BREAKAWAY_FROM_JOB 脱离调用方的作业对象。
// 用法：jobcheck.exe            创建子进程并等待，打印各阶段结果
//       jobcheck.exe --child    子进程模式，报告自身是否在作业里
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void report(const char *tag, int inJob) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0;
    strcat(path, "jobcheck.out");
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%s inJob=%d\n", tag, inJob); fclose(f); }
}

int main(int argc, char **argv) {
    BOOL inJob = FALSE;
    IsProcessInJob(GetCurrentProcess(), NULL, &inJob);

    if (argc > 1 && strcmp(argv[1], "--child") == 0) {
        report("child", inJob);
        return inJob ? 1 : 0;
    }

    report("parent", inJob);
    printf("parent inJob=%d\n", inJob);

    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof(cmd), "\"%s\" --child", self);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                             CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi);
    DWORD err = GetLastError();
    printf("breakaway create -> %d err=%lu\n", ok, (unsigned long)err);
    if (ok) {
        WaitForSingleObject(pi.hProcess, 5000);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        printf("child exit=%lu\n", (unsigned long)code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

// inject2.exe：保留各阶段、完整性级别及缓解策略诊断，仅操作明确给定的 PID。

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <wchar.h>

static const char *errname(DWORD e) {
    switch (e) {
        case 0:    return "ok";
        case 5:    return "ERROR_ACCESS_DENIED";
        case 6:    return "ERROR_INVALID_HANDLE";
        case 8:    return "ERROR_NOT_ENOUGH_MEMORY";
        case 87:   return "ERROR_INVALID_PARAMETER";
        case 998:  return "ERROR_NOACCESS";
        case 1314: return "ERROR_PRIVILEGE_NOT_HELD";
        default:   return "?";
    }
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "用法：inject2.exe <pid> <dll>\n"); return 2; }
    if (!argv[1][0] || !argv[2][0]) {
        fprintf(stderr, "PID 和 DLL 路径不能为空。\n");
        return 2;
    }
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') {
            fprintf(stderr, "PID 必须是 1 到 4294967295 的十进制整数。\n");
            return 2;
        }
    }
    errno = 0;
    unsigned long long value = strtoull(argv[1], NULL, 10);
    if (errno == ERANGE || value == 0 || value > MAXDWORD) {
        fprintf(stderr, "PID 超出有效范围。\n");
        return 2;
    }
    DWORD pid = (DWORD)value;
    HANDLE proc = NULL, th = NULL;
    wchar_t *input = NULL, *dll = NULL;
    void *probe = NULL, *res = NULL, *remote = NULL;
    int thread_finished = 0;
    int rc = 1;
    DWORD error;

    int chars = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, argv[2], -1, NULL, 0);
    if (!chars) {
        fprintf(stderr, "DLL 路径转换失败：err=%lu\n", GetLastError());
        rc = 2;
        goto cleanup;
    }
    input = malloc((SIZE_T)chars * sizeof(*input));
    if (!input) {
        fprintf(stderr, "DLL 路径内存分配失败。\n");
        goto cleanup;
    }
    if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, argv[2], -1, input, chars)) {
        fprintf(stderr, "DLL 路径转换失败：err=%lu\n", GetLastError());
        rc = 2;
        goto cleanup;
    }
    DWORD needed = GetFullPathNameW(input, 0, NULL, NULL);
    if (!needed) {
        fprintf(stderr, "GetFullPathNameW           -> err=%lu\n", GetLastError());
        rc = 2;
        goto cleanup;
    }
    dll = malloc((SIZE_T)needed * sizeof(*dll));
    if (!dll) {
        fprintf(stderr, "绝对 DLL 路径内存分配失败。\n");
        goto cleanup;
    }
    DWORD length = GetFullPathNameW(input, needed, dll, NULL);
    if (!length || length >= needed) {
        error = length ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
        fprintf(stderr, "GetFullPathNameW           -> err=%lu\n", error);
        rc = 2;
        goto cleanup;
    }
    DWORD attributes = GetFileAttributesW(dll);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
        fprintf(stderr, "DLL 不存在或不是普通文件：err=%lu\n", error);
        rc = 2;
        goto cleanup;
    }
    printf("DLL absolute path          -> %ls\n", dll);
    printf("target pid %lu\n", (unsigned long)pid);

    /* 1. 诊断目标进程访问权限。 */
    DWORD rights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    SetLastError(0);
    proc = OpenProcess(rights, FALSE, pid);
    error = GetLastError();
    printf("OpenProcess(0x%lX)          -> %p err=%lu %s\n", (unsigned long)rights,
           proc, error, errname(error));
    if (!proc) goto cleanup;

    /* 2. 保留目标完整性级别诊断。 */
    HANDLE tok = NULL;
    if (OpenProcessToken(proc, TOKEN_QUERY, &tok)) {
        BYTE buf[64];
        DWORD rl = 0;
        if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &rl)) {
            TOKEN_MANDATORY_LABEL *tml = (TOKEN_MANDATORY_LABEL *)buf;
            PSID s = tml->Label.Sid;
            DWORD cnt = *GetSidSubAuthorityCount(s);
            DWORD sub = *GetSidSubAuthority(s, cnt - 1);
            printf("target integrity           -> 0x%04lX\n", (unsigned long)sub);
        } else {
            printf("GetTokenInformation        -> err=%lu\n", GetLastError());
        }
        CloseHandle(tok);
    } else {
        printf("OpenProcessToken           -> err=%lu\n", GetLastError());
    }

    /* 2b. 保留进程缓解策略诊断；ACG / 签名策略可能限制远程内存操作。 */
    {
        static const char *names[] = {
            "DEP", "ASLR", "DynamicCode", "StrictHandle", "SystemCallDisable",
            "MitigationOptions", "ChildProcess", "PayloadRestriction",
            "SEHOP", "HeapTerminate", "ControlFlowGuard", "SignaturePolicy",
            "RedirectionTrust", "UserShadowStack", "XFG",
        };
        for (int i = 0; i < 15; i++) {
            DWORD info[4] = {0, 0, 0, 0};
            if (GetProcessMitigationPolicy(proc, (PROCESS_MITIGATION_POLICY)i,
                                           info, sizeof(info)))
                printf("mitigation[%02d] %-20s -> 0x%08lX 0x%08lX\n", i, names[i],
                       (unsigned long)info[0], (unsigned long)info[1]);
        }
    }

    /* 3. 保留小块提交内存的诊断。 */
    SetLastError(0);
    probe = VirtualAllocEx(proc, NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    error = GetLastError();
    printf("VirtualAllocEx(rw, 16)     -> %p err=%lu %s\n", probe, error, errname(error));
    if (probe) {
        if (!VirtualFreeEx(proc, probe, 0, MEM_RELEASE)) {
            printf("VirtualFreeEx(probe)       -> err=%lu\n", GetLastError());
            goto cleanup;
        }
        probe = NULL;
    }

    /* 4. 保留仅预留内存的诊断。 */
    SetLastError(0);
    res = VirtualAllocEx(proc, NULL, 4096, MEM_RESERVE, PAGE_NOACCESS);
    error = GetLastError();
    printf("VirtualAllocEx(reserve)    -> %p err=%lu %s\n", res, error, errname(error));
    if (res) {
        if (!VirtualFreeEx(proc, res, 0, MEM_RELEASE)) {
            printf("VirtualFreeEx(reserve)     -> err=%lu\n", GetLastError());
            goto cleanup;
        }
        res = NULL;
    }

    /* 5. 写入包含终止符的绝对 DLL 路径。 */
    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    SetLastError(0);
    remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    error = GetLastError();
    printf("VirtualAllocEx(path)       -> %p err=%lu %s\n", remote, error, errname(error));
    if (!remote) goto cleanup;

    SIZE_T wrote = 0;
    BOOL written = WriteProcessMemory(proc, remote, dll, bytes, &wrote);
    error = written ? ERROR_SUCCESS : GetLastError();
    printf("WriteProcessMemory         -> %llu/%llu bytes err=%lu %s\n",
           (unsigned long long)wrote, (unsigned long long)bytes, error, errname(error));
    if (!written || wrote != bytes) {
        fprintf(stderr, "DLL 路径未完整写入。\n");
        goto cleanup;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) {
        printf("GetModuleHandleW           -> err=%lu\n", GetLastError());
        goto cleanup;
    }
    FARPROC loadlib = GetProcAddress(k32, "LoadLibraryW");
    if (!loadlib) {
        printf("GetProcAddress             -> err=%lu\n", GetLastError());
        goto cleanup;
    }
    SetLastError(0);
    th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)loadlib,
                            remote, 0, NULL);
    error = GetLastError();
    printf("CreateRemoteThread         -> %p err=%lu %s\n", th, error, errname(error));
    if (!th) goto cleanup;

    DWORD wait = WaitForSingleObject(th, 5000);
    if (wait != WAIT_OBJECT_0) {
        if (wait == WAIT_TIMEOUT) {
            fprintf(stderr, "WaitForSingleObject        -> WAIT_TIMEOUT (5000 ms)\n");
        } else if (wait == WAIT_FAILED) {
            fprintf(stderr, "WaitForSingleObject        -> WAIT_FAILED err=%lu\n", GetLastError());
        } else {
            fprintf(stderr, "WaitForSingleObject        -> 意外状态 0x%08lX\n", (unsigned long)wait);
        }
        goto cleanup;
    }
    thread_finished = 1;
    DWORD code = 0;
    if (!GetExitCodeThread(th, &code)) {
        printf("GetExitCodeThread          -> err=%lu\n", GetLastError());
        goto cleanup;
    }
    printf("remote LoadLibraryW        -> 0x%08lX\n", (unsigned long)code);
    if (!code) {
        fprintf(stderr, "远程 LoadLibraryW 返回 0，DLL 加载失败。\n");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (remote && (!th || thread_finished)) {
        if (!VirtualFreeEx(proc, remote, 0, MEM_RELEASE)) {
            fprintf(stderr, "VirtualFreeEx(path)        -> err=%lu\n", GetLastError());
            rc = 1;
        }
    } else if (remote) {
        // 未确认线程完成时，远程参数仍可能被读取，不能释放。
        fprintf(stderr, "远程线程未确认完成，保留参数内存：%p\n", remote);
    }
    if (res && !VirtualFreeEx(proc, res, 0, MEM_RELEASE)) {
        fprintf(stderr, "VirtualFreeEx(reserve)     -> err=%lu\n", GetLastError());
        rc = 1;
    }
    if (probe && !VirtualFreeEx(proc, probe, 0, MEM_RELEASE)) {
        fprintf(stderr, "VirtualFreeEx(probe)       -> err=%lu\n", GetLastError());
        rc = 1;
    }
    if (th && !CloseHandle(th)) {
        fprintf(stderr, "CloseHandle(thread)        -> err=%lu\n", GetLastError());
        rc = 1;
    }
    if (proc && !CloseHandle(proc)) {
        fprintf(stderr, "CloseHandle(process)       -> err=%lu\n", GetLastError());
        rc = 1;
    }
    free(dll);
    free(input);
    return rc;
}

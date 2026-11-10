// Minimal same-user DLL injector: OpenProcess -> VirtualAllocEx ->
// WriteProcessMemory -> CreateRemoteThread(LoadLibraryW).
//
//   inject.exe <pid> <absolute-path-to-dll>

#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: inject.exe <pid> <dll>\n");
        return 2;
    }
    DWORD pid = (DWORD)atoi(argv[1]);

    wchar_t dll[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, argv[2], -1, dll, MAX_PATH)) {
        printf("path conversion failed\n");
        return 1;
    }
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        printf("dll not found: %ls\n", dll);
        return 1;
    }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        printf("OpenProcess failed %lu\n", GetLastError());
        return 1;
    }

    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        printf("VirtualAllocEx failed %lu\n", GetLastError());
        return 1;
    }
    if (!WriteProcessMemory(proc, remote, dll, bytes, NULL)) {
        printf("WriteProcessMemory failed %lu\n", GetLastError());
        return 1;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadlib = GetProcAddress(k32, "LoadLibraryW");
    HANDLE th = CreateRemoteThread(proc, NULL, 0,
                                   (LPTHREAD_START_ROUTINE)loadlib, remote, 0, NULL);
    if (!th) {
        printf("CreateRemoteThread failed %lu\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(th, 10000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    printf("injected into pid %lu, LoadLibraryW returned 0x%08lX%s\n",
           (unsigned long)pid, (unsigned long)code,
           code == 0 ? "  (FAILED: check path/architecture)" : "");
    CloseHandle(th);
    CloseHandle(proc);
    return code == 0 ? 1 : 0;
}

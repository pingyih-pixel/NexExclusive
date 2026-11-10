// Smoke test: can this toolchain see the WASAPI headers and emit a x64 DLL?
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        // touch the interfaces so the compiler has to resolve them
        IAudioClient *c = NULL;
        IAudioRenderClient *r = NULL;
        IMMDeviceEnumerator *e = NULL;
        (void)c; (void)r; (void)e;
    }
    return TRUE;
}

__declspec(dllexport) int probe_version(void) {
    return (int)sizeof(WAVEFORMATEXTENSIBLE);
}

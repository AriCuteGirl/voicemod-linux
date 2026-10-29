/*
 * version.dll proxy for Voicemod under Wine.
 *
 * Forwards every version.dll export to the real system32 copy, and on load replaces
 * Voicemod.exe's DeviceIoControl import with a wrapper. Voicemod sends IOCTL 0x1d2007
 * with lpInBuffer set to an event HANDLE value (METHOD_NEITHER, 8 bytes). Windows never
 * reads that buffer, but Wine copies it in user mode and fails with ERROR_NOACCESS, so
 * the wrapper hands the driver a pointer to a copy of the handle instead.
 * Patching in memory keeps Voicemod.exe's Authenticode signature intact.
 *
 * Wine's IAudioSessionManager2::(Un)RegisterSessionNotification are E_NOTIMPL stubs,
 * which aborts Voicemod's VADIPC endpoint setup. Before the first VAD IOCTL the shim
 * patches those vtable slots (shared by every session manager): duck notifications just
 * succeed, session notifications and enumeration go to sessions.c.
 */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include "sessions.h"

#define VAD_REGISTER_EVENT 0x1d2007

#define EXPORTS(X) \
    X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA) \
    X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA) \
    X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW) \
    X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW) \
    X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

/* Each export is a jump through a pointer filled from the real DLL. */
#define DECLARE_PTR(name) void *real_##name;
EXPORTS(DECLARE_PTR)

#define STUB(name) \
    __asm__(".globl " #name "\n" #name ":\n\tjmp *real_" #name "(%rip)\n");
EXPORTS(STUB)

typedef BOOL (WINAPI *DeviceIoControl_fn)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static DeviceIoControl_fn real_DeviceIoControl;

static HRESULT WINAPI duck_notification_ok(IAudioSessionManager2 *iface, LPCWSTR id, void *client)
{
    (void)iface; (void)id; (void)client;
    return S_OK;
}

static HRESULT WINAPI unregister_duck_ok(IAudioSessionManager2 *iface, void *client)
{
    (void)iface; (void)client;
    return S_OK;
}

static void patch_session_manager(void)
{
    static BOOL done;
    IMMDeviceEnumerator *devenum = NULL;
    IMMDevice *device = NULL;
    IAudioSessionManager2 *manager = NULL;
    IAudioSessionManager2Vtbl *vtbl;
    HRESULT init;
    DWORD old;

    if (done) return;
    done = TRUE;

    init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IMMDeviceEnumerator, (void **)&devenum)) &&
        SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(devenum, eRender, eConsole, &device)) &&
        SUCCEEDED(IMMDevice_Activate(device, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&manager)))
    {
        vtbl = manager->lpVtbl;
        VirtualProtect(vtbl, sizeof(*vtbl), PAGE_READWRITE, &old);
        sessions_patch(vtbl);
        vtbl->RegisterDuckNotification = (void *)duck_notification_ok;
        vtbl->UnregisterDuckNotification = (void *)unregister_duck_ok;
        VirtualProtect(vtbl, sizeof(*vtbl), old, &old);
        IAudioSessionManager2_Release(manager);
    }
    if (device) IMMDevice_Release(device);
    if (devenum) IMMDeviceEnumerator_Release(devenum);
    if (SUCCEEDED(init)) CoUninitialize();
}

static BOOL WINAPI hook_DeviceIoControl(HANDLE dev, DWORD code, LPVOID in, DWORD in_size,
                                        LPVOID out, DWORD out_size, LPDWORD returned, LPOVERLAPPED ov)
{
    ULONG_PTR handle_copy;

    char msg[96];

    if (code == VAD_REGISTER_EVENT) patch_session_manager();
    if (code == VAD_REGISTER_EVENT && in_size == sizeof(handle_copy) && (ULONG_PTR)in < 0x10000)
    {
        handle_copy = (ULONG_PTR)in;
        sessions_set_driver_event((HANDLE)handle_copy);
        in = &handle_copy;
    }
    wsprintfA(msg, "vmshim: ioctl %#lx in=%lu out=%lu first=%#lx\n", code, in_size, out_size,
              in && in_size >= 4 ? *(DWORD *)in : 0);
    OutputDebugStringA(msg);
    return real_DeviceIoControl(dev, code, in, in_size, out, out_size, returned, ov);
}

static void hook_main_module_import(void)
{
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    void *target = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "DeviceIoControl");

    if (!dir->VirtualAddress || !target) return;
    real_DeviceIoControl = (DeviceIoControl_fn)target;

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++)
    {
        IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; thunk->u1.Function; thunk++)
        {
            DWORD old;
            if ((void *)thunk->u1.Function != target) continue;
            VirtualProtect(&thunk->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            thunk->u1.Function = (ULONG_PTR)hook_DeviceIoControl;
            VirtualProtect(&thunk->u1.Function, sizeof(void *), old, &old);
        }
    }
}

/* With VOICEMOD_HIDE_QT=1 the Linux UI is used instead, so the (blank) Qt main window is
 * hidden (WM_CLOSE would quit Voicemod). Qt shows it again during startup and whenever a
 * voicemod:// link arrives, so keep hiding it. */
static BOOL CALLBACK hide_main_window(HWND hwnd, LPARAM unused)
{
    WCHAR title[32];
    DWORD pid;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
    if (!GetWindowTextW(hwnd, title, ARRAYSIZE(title)) || lstrcmpW(title, L"VoicemodV3")) return TRUE;
    (void)unused;
    ShowWindow(hwnd, SW_HIDE);
    return TRUE;
}

static DWORD WINAPI hide_qt_window_thread(void *arg)
{
    (void)arg;

    for (;;)
    {
        Sleep(250);
        EnumWindows(hide_main_window, 0);
    }
    return 0;
}

static BOOL hide_qt_requested(void)
{
    WCHAR value[4];
    WCHAR exe[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);

    /* QtWebEngineProcess.exe loads this DLL too; only act in Voicemod.exe. */
    if (len < 12 || lstrcmpiW(exe + len - 12, L"Voicemod.exe")) return FALSE;
    return GetEnvironmentVariableW(L"VOICEMOD_HIDE_QT", value, ARRAYSIZE(value)) && value[0] == '1';
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    WCHAR path[MAX_PATH];
    HMODULE real;
    (void)reserved;

    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    GetSystemDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\version.dll");
    if (!(real = LoadLibraryW(path))) return FALSE;
#define FILL_PTR(name) real_##name = (void *)GetProcAddress(real, #name);
    EXPORTS(FILL_PTR)

    hook_main_module_import();
    if (hide_qt_requested()) CloseHandle(CreateThread(NULL, 0, hide_qt_window_thread, NULL, 0, NULL));
    return TRUE;
}

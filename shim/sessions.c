/*
 * Fake audio sessions on the Voicemod virtual mic.
 *
 * Voicemod only opens the real microphone (topology kCaptureMode) while some app records
 * from its virtual mic. On Windows it learns that from the mic's IAudioSessionManager2:
 * RegisterSessionNotification + GetSessionEnumerator. Under Wine, Linux apps such as
 * Discord never appear there, and Wine's session notifications are stubs.
 *
 * tools/vad-watch.py writes the Linux apps recording from voicemod_mic to
 * %LOCALAPPDATA%\VoicemodV3\vad-clients. While that file lists anyone, this file
 * presents one active IAudioSessionControl2 to Voicemod; when it empties, the session
 * goes inactive and expires. On every change it also signals the event Voicemod gave
 * its driver (IOCTL 0x1d2007), as the real driver does; only then does Voicemod's
 * AudioSessionSubsystem start the global session and switch to kCaptureMode.
 */
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <tlhelp32.h>

#include "sessions.h"

#define MAX_EVENTS 8
/* Visible with WINEDEBUG=+debugstr. */
#define TRACE(msg) OutputDebugStringA("vmshim: " msg "\n")

typedef struct
{
    IAudioSessionControl2 iface;
    LONG ref;
    AudioSessionState state;
    WCHAR name[64];
    IAudioSessionEvents *events[MAX_EVENTS];
    BOOL announced[MAX_EVENTS];
} session;

typedef struct
{
    IAudioSessionEnumerator iface;
    LONG ref;
    session *item;
} session_enum;

/* Built without a CRT or libuuid, so these are spelled out here. */
static const IID iid_unknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID guid_null;

static CRITICAL_SECTION lock;
static IAudioSessionManager2 *vad_manager;
static IAudioSessionNotification *vad_client;
static session *current;
static HANDLE driver_event;
static HRESULT (WINAPI *orig_GetSessionEnumerator)(IAudioSessionManager2 *, IAudioSessionEnumerator **);

static HRESULT copy_string(const WCHAR *src, LPWSTR *out)
{
    int len = lstrlenW(src) + 1;
    if (!out) return E_POINTER;
    if (!(*out = CoTaskMemAlloc(len * sizeof(WCHAR)))) return E_OUTOFMEMORY;
    lstrcpynW(*out, src, len);
    return S_OK;
}

/* IAudioSessionControl2 */

static inline session *impl_session(IAudioSessionControl2 *iface)
{
    return (session *)iface;
}

static HRESULT WINAPI s_QueryInterface(IAudioSessionControl2 *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &iid_unknown) || IsEqualIID(riid, &IID_IAudioSessionControl) ||
        IsEqualIID(riid, &IID_IAudioSessionControl2))
    {
        *out = iface;
        IAudioSessionControl2_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI s_AddRef(IAudioSessionControl2 *iface)
{
    return InterlockedIncrement(&impl_session(iface)->ref);
}

static ULONG WINAPI s_Release(IAudioSessionControl2 *iface)
{
    session *s = impl_session(iface);
    ULONG ref = InterlockedDecrement(&s->ref);
    int i;

    if (!ref)
    {
        for (i = 0; i < MAX_EVENTS; i++)
            if (s->events[i]) IAudioSessionEvents_Release(s->events[i]);
        HeapFree(GetProcessHeap(), 0, s);
    }
    return ref;
}

static HRESULT WINAPI s_GetState(IAudioSessionControl2 *iface, AudioSessionState *state)
{
    TRACE("GetState");
    *state = impl_session(iface)->state;
    return S_OK;
}

static HRESULT WINAPI s_GetDisplayName(IAudioSessionControl2 *iface, LPWSTR *name)
{
    return copy_string(impl_session(iface)->name, name);
}

static HRESULT WINAPI s_SetString(IAudioSessionControl2 *iface, LPCWSTR value, LPCGUID context)
{
    (void)iface; (void)value; (void)context;
    return S_OK;
}

static HRESULT WINAPI s_GetIconPath(IAudioSessionControl2 *iface, LPWSTR *path)
{
    (void)iface;
    return copy_string(L"", path);
}

static HRESULT WINAPI s_GetGroupingParam(IAudioSessionControl2 *iface, GUID *group)
{
    (void)iface;
    *group = guid_null;
    return S_OK;
}

static HRESULT WINAPI s_SetGroupingParam(IAudioSessionControl2 *iface, LPCGUID group, LPCGUID context)
{
    (void)iface; (void)group; (void)context;
    return S_OK;
}

static HRESULT WINAPI s_RegisterAudioSessionNotification(IAudioSessionControl2 *iface, IAudioSessionEvents *events)
{
    TRACE("RegisterAudioSessionNotification");
    session *s = impl_session(iface);
    HRESULT hr = E_OUTOFMEMORY;
    int i;

    EnterCriticalSection(&lock);
    for (i = 0; i < MAX_EVENTS; i++)
    {
        if (s->events[i]) continue;
        IAudioSessionEvents_AddRef(events);
        s->events[i] = events;
        s->announced[i] = FALSE;
        hr = S_OK;
        break;
    }
    LeaveCriticalSection(&lock);
    return hr;
}

static HRESULT WINAPI s_UnregisterAudioSessionNotification(IAudioSessionControl2 *iface, IAudioSessionEvents *events)
{
    session *s = impl_session(iface);
    int i;

    EnterCriticalSection(&lock);
    for (i = 0; i < MAX_EVENTS; i++)
    {
        if (s->events[i] != events) continue;
        IAudioSessionEvents_Release(events);
        s->events[i] = NULL;
    }
    LeaveCriticalSection(&lock);
    return S_OK;
}

static HRESULT WINAPI s_GetSessionIdentifier(IAudioSessionControl2 *iface, LPWSTR *id)
{
    (void)iface;
    return copy_string(L"{0.0.1.00000000}|voicemod-linux|linux-client", id);
}

/* Voicemod drops sessions whose process it cannot open, and the Linux recorder has no
 * Windows process, so report Wine's always-running explorer.exe as the owner. */
static DWORD stand_in_process(void)
{
    PROCESSENTRY32W entry = {sizeof(entry)};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD pid = 0;

    if (snap == INVALID_HANDLE_VALUE) return 0;
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry))
    {
        if (!lstrcmpiW(entry.szExeFile, L"explorer.exe"))
        {
            pid = entry.th32ProcessID;
            break;
        }
    }
    CloseHandle(snap);
    return pid;
}

static HRESULT WINAPI s_GetProcessId(IAudioSessionControl2 *iface, DWORD *pid)
{
    TRACE("GetProcessId");
    (void)iface;
    *pid = stand_in_process();
    return *pid ? S_OK : E_FAIL;
}

static HRESULT WINAPI s_IsSystemSoundsSession(IAudioSessionControl2 *iface)
{
    (void)iface;
    return S_FALSE;
}

static HRESULT WINAPI s_SetDuckingPreference(IAudioSessionControl2 *iface, BOOL opt_out)
{
    (void)iface; (void)opt_out;
    return S_OK;
}

static IAudioSessionControl2Vtbl session_vtbl =
{
    s_QueryInterface, s_AddRef, s_Release,
    s_GetState, s_GetDisplayName, s_SetString, s_GetIconPath, s_SetString,
    s_GetGroupingParam, s_SetGroupingParam,
    s_RegisterAudioSessionNotification, s_UnregisterAudioSessionNotification,
    s_GetSessionIdentifier, s_GetSessionIdentifier, s_GetProcessId,
    s_IsSystemSoundsSession, s_SetDuckingPreference,
};

/* IAudioSessionEnumerator */

static inline session_enum *impl_enum(IAudioSessionEnumerator *iface)
{
    return (session_enum *)iface;
}

static HRESULT WINAPI e_QueryInterface(IAudioSessionEnumerator *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &iid_unknown) || IsEqualIID(riid, &IID_IAudioSessionEnumerator))
    {
        *out = iface;
        IAudioSessionEnumerator_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI e_AddRef(IAudioSessionEnumerator *iface)
{
    return InterlockedIncrement(&impl_enum(iface)->ref);
}

static ULONG WINAPI e_Release(IAudioSessionEnumerator *iface)
{
    session_enum *e = impl_enum(iface);
    ULONG ref = InterlockedDecrement(&e->ref);

    if (!ref)
    {
        if (e->item) IAudioSessionControl2_Release(&e->item->iface);
        HeapFree(GetProcessHeap(), 0, e);
    }
    return ref;
}

static HRESULT WINAPI e_GetCount(IAudioSessionEnumerator *iface, int *count)
{
    *count = impl_enum(iface)->item ? 1 : 0;
    return S_OK;
}

static HRESULT WINAPI e_GetSession(IAudioSessionEnumerator *iface, int index, IAudioSessionControl **out)
{
    session_enum *e = impl_enum(iface);

    if (index != 0 || !e->item) return E_INVALIDARG;
    IAudioSessionControl2_AddRef(&e->item->iface);
    *out = (IAudioSessionControl *)&e->item->iface;
    return S_OK;
}

static IAudioSessionEnumeratorVtbl enum_vtbl =
{
    e_QueryInterface, e_AddRef, e_Release, e_GetCount, e_GetSession,
};

/* IAudioSessionManager2 replacements */

static HRESULT WINAPI m_RegisterSessionNotification(IAudioSessionManager2 *iface, IAudioSessionNotification *client)
{
    TRACE("RegisterSessionNotification");
    EnterCriticalSection(&lock);
    /* Voicemod registers exactly once, on the virtual mic (IOCTLComm::initializeEndPoints). */
    if (!vad_client)
    {
        IAudioSessionNotification_AddRef(client);
        vad_client = client;
        vad_manager = iface;
    }
    LeaveCriticalSection(&lock);
    return S_OK;
}

static HRESULT WINAPI m_UnregisterSessionNotification(IAudioSessionManager2 *iface, IAudioSessionNotification *client)
{
    (void)iface;
    EnterCriticalSection(&lock);
    if (client == vad_client)
    {
        IAudioSessionNotification_Release(vad_client);
        vad_client = NULL;
        vad_manager = NULL;
    }
    LeaveCriticalSection(&lock);
    return S_OK;
}

static HRESULT WINAPI m_GetSessionEnumerator(IAudioSessionManager2 *iface, IAudioSessionEnumerator **out)
{
    session_enum *e;

    if (iface != vad_manager) return orig_GetSessionEnumerator(iface, out);
    TRACE("GetSessionEnumerator (virtual mic)");
    if (!(e = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*e)))) return E_OUTOFMEMORY;
    e->iface.lpVtbl = &enum_vtbl;
    e->ref = 1;
    EnterCriticalSection(&lock);
    if (current && current->state == AudioSessionStateActive)
    {
        e->item = current;
        IAudioSessionControl2_AddRef(&current->iface);
    }
    LeaveCriticalSection(&lock);
    *out = &e->iface;
    return S_OK;
}

/* State file polling */

/* Returns TRUE when the file lists at least one recorder; name gets the first app's name. */
static BOOL read_clients(const WCHAR *path, WCHAR *name, int name_len)
{
    char buf[512], *tab, *end;
    DWORD got = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, 0, NULL);

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile(file, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(file);
    buf[got] = 0;
    if (!got) return FALSE;

    tab = buf;
    while (*tab && *tab != '\t') tab++;
    if (*tab) tab++;
    for (end = tab; *end && *end != '\n'; end++) ;
    *end = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, *tab ? tab : "Linux app", -1, name, name_len))
        lstrcpyW(name, L"Linux app");
    return TRUE;
}

static void notify_state(session *s, AudioSessionState state)
{
    IAudioSessionEvents *events[MAX_EVENTS];
    int i, n = 0;

    EnterCriticalSection(&lock);
    s->state = state;
    for (i = 0; i < MAX_EVENTS; i++)
    {
        if (!s->events[i]) continue;
        events[n] = s->events[i];
        IAudioSessionEvents_AddRef(events[n]);
        n++;
    }
    LeaveCriticalSection(&lock);

    for (i = 0; i < n; i++)
    {
        IAudioSessionEvents_OnStateChanged(events[i], state);
        IAudioSessionEvents_Release(events[i]);
    }
}

static void start_session(const WCHAR *name)
{
    IAudioSessionNotification *client;
    session *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));

    if (!s) return;
    s->iface.lpVtbl = &session_vtbl;
    s->ref = 1;
    s->state = AudioSessionStateActive;
    lstrcpynW(s->name, name, ARRAYSIZE(s->name));

    EnterCriticalSection(&lock);
    current = s;
    if ((client = vad_client)) IAudioSessionNotification_AddRef(client);
    LeaveCriticalSection(&lock);

    TRACE("session started");
    if (client)
    {
        IAudioSessionNotification_OnSessionCreated(client, (IAudioSessionControl *)&s->iface);
        IAudioSessionNotification_Release(client);
    }
}

static void stop_session(void)
{
    session *s;

    EnterCriticalSection(&lock);
    s = current;
    current = NULL;
    LeaveCriticalSection(&lock);
    if (!s) return;

    TRACE("session stopped");
    notify_state(s, AudioSessionStateInactive);
    notify_state(s, AudioSessionStateExpired);
    IAudioSessionControl2_Release(&s->iface);
}

/* Voicemod registers its IAudioSessionEvents from a worker thread some time after
 * OnSessionCreated and acts on OnStateChanged, so tell each new listener the session
 * has become active. Called from the poll thread, outside Voicemod's callbacks. */
static void announce_to_new_listeners(void)
{
    IAudioSessionEvents *events[MAX_EVENTS];
    int i, n = 0;

    EnterCriticalSection(&lock);
    if (current && current->state == AudioSessionStateActive)
    {
        for (i = 0; i < MAX_EVENTS; i++)
        {
            if (!current->events[i] || current->announced[i]) continue;
            current->announced[i] = TRUE;
            events[n] = current->events[i];
            IAudioSessionEvents_AddRef(events[n]);
            n++;
        }
    }
    LeaveCriticalSection(&lock);

    for (i = 0; i < n; i++)
    {
        TRACE("announce active");
        IAudioSessionEvents_OnStateChanged(events[i], AudioSessionStateActive);
        IAudioSessionEvents_Release(events[i]);
    }
}

static DWORD WINAPI poll_thread(void *arg)
{
    WCHAR path[MAX_PATH], name[64];
    BOOL active = FALSE, now;
    (void)arg;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH - 32)) return 0;
    lstrcatW(path, L"\\VoicemodV3\\vad-clients");

    for (;;)
    {
        Sleep(500);
        if (!vad_client) continue;
        announce_to_new_listeners();
        now = read_clients(path, name, ARRAYSIZE(name));
        if (now == active) continue;
        active = now;
        if (active) start_session(name);
        else stop_session();
        if (driver_event)
        {
            TRACE("signal driver event");
            SetEvent(driver_event);
        }
    }
}

void sessions_set_driver_event(HANDLE event)
{
    driver_event = event;
}

void sessions_patch(IAudioSessionManager2Vtbl *vtbl)
{
    TRACE("patched session manager");
    InitializeCriticalSection(&lock);
    orig_GetSessionEnumerator = vtbl->GetSessionEnumerator;
    vtbl->GetSessionEnumerator = m_GetSessionEnumerator;
    vtbl->RegisterSessionNotification = m_RegisterSessionNotification;
    vtbl->UnregisterSessionNotification = m_UnregisterSessionNotification;
    CloseHandle(CreateThread(NULL, 0, poll_thread, NULL, 0, NULL));
}

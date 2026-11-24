/*
 * pchelper.exe — single native helper for the Ulanzi "PC Tools" plugin.
 * Uses Windows APIs directly: no emulated key presses, no polling.
 *
 * Commands (stdin, one per line; or one command via argv for testing):
 *   media playpause|next|previous|stop|mute|volup|voldown  -> OK media <cmd> via session|appcommand
 *   list                    -> DEFAULT <id> / DEV <id>\t<name> ... / END   (active output devices)
 *   status                  -> STATUS mic=<0|1|-1> out=<default output id>
 *   mic toggle|mute|unmute  -> OK mic=<0|1>
 *   setout <id>             -> OK out=<id>
 *   phones                  -> PHONE <id>\t<name> ... / END   (paired phones that can stream to this PC)
 *   phone connect|disconnect|toggle|status [id] -> OK phone=<0|1>
 * Unsolicited events (pushed by Windows, only when something changes):
 *   EVENT mic=<0|1|-1>      default microphone muted/unmuted or default mic changed
 *   EVENT out=<id>          default output device changed
 *   EVENT phone=<0|1>       phone audio connection opened/closed (also when the phone drops it)
 *
 * Build: x86_64-w64-mingw32-gcc -Os -s -static -o pchelper.exe pchelper.c -lole32 -luuid
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <string.h>

static CRITICAL_SECTION g_out; /* stdout is shared by replies and event threads */
#define OUT_BEGIN() EnterCriticalSection(&g_out)
#define OUT_END()   do { fflush(stdout); LeaveCriticalSection(&g_out); } while (0)

/* ======================================================================= media session (SMTC) */
/* ---- WinRT bits, loaded at runtime from combase.dll (no SDK headers needed) ---- */
typedef struct HSTRING__ *HSTRING;
typedef HRESULT (WINAPI *RoInitialize_t)(int);
typedef HRESULT (WINAPI *RoGetActivationFactory_t)(HSTRING, REFIID, void **);
typedef HRESULT (WINAPI *WindowsCreateString_t)(LPCWSTR, UINT32, HSTRING *);
typedef HRESULT (WINAPI *WindowsDeleteString_t)(HSTRING);

static RoGetActivationFactory_t pRoGetActivationFactory;
static WindowsCreateString_t pWindowsCreateString;
static WindowsDeleteString_t pWindowsDeleteString;

/* IIDs (from Windows SDK metadata) */
static const GUID IID_ISessionManagerStatics = /* IGlobalSystemMediaTransportControlsSessionManagerStatics */
    {0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};
static const GUID IID_IAsyncInfo =
    {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

/* vtable slots (IInspectable occupies 0..5) */
enum {
    SLOT_QI = 0, SLOT_RELEASE = 2,
    STATICS_RequestAsync = 6,
    MANAGER_GetCurrentSession = 6,
    SESSION_TryStopAsync = 12,
    SESSION_TrySkipNextAsync = 16,
    SESSION_TrySkipPreviousAsync = 17,
    SESSION_TryTogglePlayPauseAsync = 20,
    ASYNCINFO_Status = 7,
    ASYNCOP_GetResults = 8,
};

#define VT(o) (*(void ***)(o))
typedef HRESULT (STDMETHODCALLTYPE *QI_t)(void *, REFIID, void **);
typedef ULONG (STDMETHODCALLTYPE *Release_t)(void *);
typedef HRESULT (STDMETHODCALLTYPE *OutPtr_t)(void *, void **);
typedef HRESULT (STDMETHODCALLTYPE *OutInt_t)(void *, int *);
typedef HRESULT (STDMETHODCALLTYPE *OutBool_t)(void *, unsigned char *);

static void release(void *o) { if (o) ((Release_t)VT(o)[SLOT_RELEASE])(o); }
static HRESULT call_out(void *o, int slot, void **out) { *out = NULL; return ((OutPtr_t)VT(o)[slot])(o, out); }

/* Wait for an IAsyncOperation<T> to finish (polls IAsyncInfo.Status). */
static HRESULT wait_async(void *op, DWORD timeout_ms) {
    void *info = NULL;
    HRESULT hr = ((QI_t)VT(op)[SLOT_QI])(op, &IID_IAsyncInfo, &info);
    if (FAILED(hr)) return hr;
    DWORD start = GetTickCount();
    int status = 0;
    for (;;) {
        hr = ((OutInt_t)VT(info)[ASYNCINFO_Status])(info, &status);
        if (FAILED(hr) || status != 0 /* Started */) break;
        if (GetTickCount() - start > timeout_ms) { hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); break; }
        Sleep(1);
    }
    release(info);
    if (SUCCEEDED(hr) && status != 1 /* Completed */) hr = E_FAIL;
    return hr;
}

static void *g_manager = NULL; /* IGlobalSystemMediaTransportControlsSessionManager */

static void init_winrt(void) {
    HMODULE combase = LoadLibraryW(L"combase.dll");
    if (!combase) return;
    RoInitialize_t pRoInitialize = (RoInitialize_t)GetProcAddress(combase, "RoInitialize");
    pRoGetActivationFactory = (RoGetActivationFactory_t)GetProcAddress(combase, "RoGetActivationFactory");
    pWindowsCreateString = (WindowsCreateString_t)GetProcAddress(combase, "WindowsCreateString");
    pWindowsDeleteString = (WindowsDeleteString_t)GetProcAddress(combase, "WindowsDeleteString");
    if (!pRoInitialize || !pRoGetActivationFactory || !pWindowsCreateString || !pWindowsDeleteString) {
        pRoGetActivationFactory = NULL;
        return;
    }
    (void)pRoInitialize; /* COM is already initialised as MTA in main() */
}

static int ensure_manager(void) {
    if (g_manager) return 1;
    if (!pRoGetActivationFactory) return 0;
    static const WCHAR cls[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
    HSTRING hs = NULL;
    if (FAILED(pWindowsCreateString(cls, (UINT32)(sizeof(cls) / sizeof(WCHAR) - 1), &hs))) return 0;
    void *statics = NULL, *op = NULL;
    HRESULT hr = pRoGetActivationFactory(hs, &IID_ISessionManagerStatics, &statics);
    pWindowsDeleteString(hs);
    if (FAILED(hr)) return 0;
    hr = call_out(statics, STATICS_RequestAsync, &op);
    release(statics);
    if (FAILED(hr) || !op) return 0;
    if (SUCCEEDED(wait_async(op, 3000)))
        call_out(op, ASYNCOP_GetResults, &g_manager);
    release(op);
    return g_manager != NULL;
}

/* Returns 1 if the active media session accepted the command. */
static int try_session(int slot) {
    if (!ensure_manager()) return 0;
    void *session = NULL, *op = NULL;
    if (FAILED(call_out(g_manager, MANAGER_GetCurrentSession, &session)) || !session) return 0;
    int ok = 0;
    if (SUCCEEDED(call_out(session, slot, &op)) && op) {
        if (SUCCEEDED(wait_async(op, 3000))) {
            unsigned char result = 0;
            if (SUCCEEDED(((OutBool_t)VT(op)[ASYNCOP_GetResults])(op, &result))) ok = result != 0;
        }
        release(op);
    }
    release(session);
    return ok;
}

static int send_appcommand(int cmd) {
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!tray) return 0;
    return PostMessageW(tray, WM_APPCOMMAND, (WPARAM)tray, MAKELPARAM(0, cmd)) != 0;
}



/* ======================================================================= audio */

/* IPolicyConfig: undocumented but stable since Windows 7 (used by SoundSwitch, AudioDeviceCmdlets, ...) */
typedef struct IPolicyConfig IPolicyConfig;
typedef struct IPolicyConfigVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IPolicyConfig *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IPolicyConfig *);
    ULONG (STDMETHODCALLTYPE *Release)(IPolicyConfig *);
    void *GetMixFormat, *GetDeviceFormat, *ResetDeviceFormat, *SetDeviceFormat;
    void *GetProcessingPeriod, *SetProcessingPeriod, *GetShareMode, *SetShareMode;
    void *GetPropertyValue, *SetPropertyValue;
    HRESULT (STDMETHODCALLTYPE *SetDefaultEndpoint)(IPolicyConfig *, LPCWSTR, ERole);
    void *SetEndpointVisibility;
} IPolicyConfigVtbl;
struct IPolicyConfig { const IPolicyConfigVtbl *lpVtbl; };
static const GUID CLSID_PolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const GUID IID_IPolicyConfig        = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

static IMMDeviceEnumerator *g_enum = NULL;

static void put_utf8(LPCWSTR w) {
    char buf[1024];
    if (WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, buf, sizeof buf, NULL, NULL) <= 0) buf[0] = 0;
    fputs(buf, stdout);
}

static HRESULT default_id(EDataFlow flow, LPWSTR *id) {
    IMMDevice *dev = NULL;
    *id = NULL;
    HRESULT hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_enum, flow, eConsole, &dev);
    if (FAILED(hr)) return hr;
    hr = IMMDevice_GetId(dev, id);
    IMMDevice_Release(dev);
    return hr;
}

/* returns 1 muted, 0 live, -1 no mic. set: -1 read, 0/1 set, 2 toggle */
static int mic_mute(int set) {
    IMMDevice *dev = NULL;
    IAudioEndpointVolume *vol = NULL;
    int result = -1;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_enum, eCapture, eConsole, &dev))) return -1;
    if (SUCCEEDED(IMMDevice_Activate(dev, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&vol))) {
        BOOL muted = FALSE;
        if (SUCCEEDED(IAudioEndpointVolume_GetMute(vol, &muted))) {
            if (set == 2) set = !muted;
            if ((set == 0 || set == 1) && SUCCEEDED(IAudioEndpointVolume_SetMute(vol, set, NULL))) muted = set;
            result = muted ? 1 : 0;
        }
        IAudioEndpointVolume_Release(vol);
    }
    IMMDevice_Release(dev);
    return result;
}

/* ---- change notifications (no polling) ---- */
static HANDLE g_micChanged;                 /* signalled when the default mic changes */
static IAudioEndpointVolume *g_micVol = NULL; /* volume interface we're subscribed to */

static HRESULT STDMETHODCALLTYPE cb_QI(void *This, REFIID riid, void **ppv) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IAudioEndpointVolumeCallback) ||
        IsEqualIID(riid, &IID_IMMNotificationClient)) { *ppv = This; return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cb_AddRef(void *This) { (void)This; return 1; } /* static objects */
static ULONG STDMETHODCALLTYPE cb_Release(void *This) { (void)This; return 1; }

static HRESULT STDMETHODCALLTYPE vol_OnNotify(IAudioEndpointVolumeCallback *This, AUDIO_VOLUME_NOTIFICATION_DATA *d) {
    (void)This;
    static int last = -2;
    int m = d->bMuted ? 1 : 0;
    if (m != last) { last = m; OUT_BEGIN(); printf("EVENT mic=%d\n", m); OUT_END(); }
    return S_OK;
}
static IAudioEndpointVolumeCallbackVtbl g_volVtbl = {
    (void *)cb_QI, (void *)cb_AddRef, (void *)cb_Release, vol_OnNotify};
static IAudioEndpointVolumeCallback g_volCb = {&g_volVtbl};

static HRESULT STDMETHODCALLTYPE nc_OnDeviceStateChanged(IMMNotificationClient *t, LPCWSTR id, DWORD s) { (void)t; (void)id; (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_OnDeviceAdded(IMMNotificationClient *t, LPCWSTR id) { (void)t; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_OnDeviceRemoved(IMMNotificationClient *t, LPCWSTR id) { (void)t; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_OnPropertyValueChanged(IMMNotificationClient *t, LPCWSTR id, const PROPERTYKEY k) { (void)t; (void)id; (void)k; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_OnDefaultDeviceChanged(IMMNotificationClient *t, EDataFlow flow, ERole role, LPCWSTR id) {
    (void)t;
    if (role != eConsole) return S_OK;
    if (flow == eRender) { OUT_BEGIN(); fputs("EVENT out=", stdout); put_utf8(id); fputs("\n", stdout); OUT_END(); }
    if (flow == eCapture) SetEvent(g_micChanged); /* re-subscribe on a worker thread, not inside the callback */
    return S_OK;
}
static IMMNotificationClientVtbl g_ncVtbl = {
    (void *)cb_QI, (void *)cb_AddRef, (void *)cb_Release,
    nc_OnDeviceStateChanged, nc_OnDeviceAdded, nc_OnDeviceRemoved, nc_OnDefaultDeviceChanged, nc_OnPropertyValueChanged};
static IMMNotificationClient g_nc = {&g_ncVtbl};

static void subscribe_mic(void) {
    if (g_micVol) {
        IAudioEndpointVolume_UnregisterControlChangeNotify(g_micVol, &g_volCb);
        IAudioEndpointVolume_Release(g_micVol);
        g_micVol = NULL;
    }
    IMMDevice *dev = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_enum, eCapture, eConsole, &dev))) {
        if (SUCCEEDED(IMMDevice_Activate(dev, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&g_micVol)))
            IAudioEndpointVolume_RegisterControlChangeNotify(g_micVol, &g_volCb);
        IMMDevice_Release(dev);
    }
}

static DWORD WINAPI mic_watch_thread(LPVOID p) {
    (void)p;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        WaitForSingleObject(g_micChanged, INFINITE);
        subscribe_mic();
        int m = mic_mute(-1);
        OUT_BEGIN(); printf("EVENT mic=%d\n", m); OUT_END();
    }
    return 0;
}

static void cmd_list(void) {
    LPWSTR def = NULL;
    default_id(eRender, &def);
    fputs("DEFAULT ", stdout); put_utf8(def); fputs("\n", stdout);
    if (def) CoTaskMemFree(def);

    IMMDeviceCollection *col = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(g_enum, eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT count = 0;
        IMMDeviceCollection_GetCount(col, &count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice *dev = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &dev))) continue;
            LPWSTR id = NULL;
            IPropertyStore *props = NULL;
            PROPVARIANT name;
            PropVariantInit(&name);
            IMMDevice_GetId(dev, &id);
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props))) {
                IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &name);
                IPropertyStore_Release(props);
            }
            fputs("DEV ", stdout); put_utf8(id); fputs("\t", stdout);
            put_utf8(name.vt == VT_LPWSTR ? name.pwszVal : L"(unknown)"); fputs("\n", stdout);
            PropVariantClear(&name);
            if (id) CoTaskMemFree(id);
            IMMDevice_Release(dev);
        }
        IMMDeviceCollection_Release(col);
    }
    fputs("END\n", stdout);
}

static void cmd_status(void) {
    LPWSTR def = NULL;
    default_id(eRender, &def);
    printf("STATUS mic=%d out=", mic_mute(-1));
    put_utf8(def); fputs("\n", stdout);
    if (def) CoTaskMemFree(def);
}

static void cmd_setout(const char *id_utf8) {
    WCHAR id[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, id_utf8, -1, id, 512)) { printf("ERR setout bad id\n"); return; }
    IPolicyConfig *pc = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_PolicyConfigClient, NULL, CLSCTX_ALL, &IID_IPolicyConfig, (void **)&pc);
    if (FAILED(hr)) { printf("ERR setout policyconfig 0x%08lx\n", (unsigned long)hr); return; }
    HRESULT h[3];
    h[0] = pc->lpVtbl->SetDefaultEndpoint(pc, id, eConsole);
    h[1] = pc->lpVtbl->SetDefaultEndpoint(pc, id, eMultimedia);
    h[2] = pc->lpVtbl->SetDefaultEndpoint(pc, id, eCommunications);
    pc->lpVtbl->Release(pc);
    for (int i = 0; i < 3; i++)
        if (FAILED(h[i])) { printf("ERR setout 0x%08lx\n", (unsigned long)h[i]); return; }
    printf("OK out=%s\n", id_utf8);
}

/* ======================================================================= phone audio (AudioPlaybackConnection)
 * Same Windows API that AudioPlaybackConnector uses: the PC becomes a Bluetooth speaker for the phone.
 * The connection lives exactly as long as this helper holds it.
 */
typedef PCWSTR (WINAPI *WindowsGetStringRawBuffer_t)(HSTRING, UINT32 *);
static WindowsGetStringRawBuffer_t pWindowsGetStringRawBuffer;

static const GUID IID_IAudioPlaybackConnectionStatics = {0xe60963a2, 0x69e6, 0x5ffc, {0x9e, 0x13, 0x82, 0x4a, 0x85, 0x21, 0x3d, 0xaf}};
static const GUID IID_IDeviceInformationStatics       = {0xc17f100e, 0x3a46, 0x4a78, {0x80, 0x13, 0x76, 0x9d, 0xc9, 0xb9, 0x73, 0x90}};
static const GUID IID_IClosable                       = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
/* TypedEventHandler<AudioPlaybackConnection, IInspectable> (computed from the WinRT signature) */
static const GUID IID_APCStateHandler                 = {0x0e389b05, 0x31a6, 0x58f1, {0x9e, 0xa4, 0x0c, 0x1e, 0x4d, 0x70, 0xa7, 0xb8}};
static const GUID IID_IAgileObject_                   = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

enum {
    APCS_GetDeviceSelector = 6, APCS_TryCreateFromId = 7,
    APC_Start = 6, APC_State = 9, APC_Open = 10, APC_AddStateChanged = 12, APC_RemoveStateChanged = 13,
    APCR_Status = 6,
    DIS_FindAllAsyncAqsFilter = 10,
    DI_Id = 6, DI_Name = 7,
    VV_GetAt = 6, VV_Size = 7,
    CLOSABLE_Close = 6,
};

typedef HRESULT (STDMETHODCALLTYPE *OutHstr_t)(void *, HSTRING *);
typedef HRESULT (STDMETHODCALLTYPE *HstrOutPtr_t)(void *, HSTRING, void **);
typedef HRESULT (STDMETHODCALLTYPE *NoArg_t)(void *);
typedef HRESULT (STDMETHODCALLTYPE *OutU32_t)(void *, UINT32 *);
typedef HRESULT (STDMETHODCALLTYPE *GetAt_t)(void *, UINT32, void **);
typedef HRESULT (STDMETHODCALLTYPE *AddHandler_t)(void *, void *, INT64 *);
typedef HRESULT (STDMETHODCALLTYPE *RemoveHandler_t)(void *, INT64);

static void *g_conn = NULL;   /* IAudioPlaybackConnection we own */
static INT64 g_connToken = 0;

static void *activation_factory(const WCHAR *cls, const GUID *iid) {
    if (!pRoGetActivationFactory) return NULL;
    HSTRING hs = NULL;
    void *f = NULL;
    if (FAILED(pWindowsCreateString(cls, (UINT32)wcslen(cls), &hs))) return NULL;
    if (FAILED(pRoGetActivationFactory(hs, iid, &f))) f = NULL;
    pWindowsDeleteString(hs);
    return f;
}

static void put_hstring(HSTRING h) {
    UINT32 n = 0;
    const WCHAR *w = (h && pWindowsGetStringRawBuffer) ? (const WCHAR *)pWindowsGetStringRawBuffer(h, &n) : L"";
    put_utf8(w);
}

static int conn_state(void *conn) { /* 1 = opened, 0 = closed */
    int s = 0;
    if (conn && FAILED(((OutInt_t)VT(conn)[APC_State])(conn, &s))) s = 0;
    return s == 1;
}

/* StateChanged delegate: a static COM object */
static HRESULT STDMETHODCALLTYPE sh_QI(void *This, REFIID riid, void **ppv) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_APCStateHandler) || IsEqualIID(riid, &IID_IAgileObject_)) {
        *ppv = This; return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE sh_Invoke(void *This, void *sender, void *args) {
    (void)This; (void)args;
    OUT_BEGIN(); printf("EVENT phone=%d\n", conn_state(sender)); OUT_END();
    return S_OK;
}
static void *g_shVtbl[] = {(void *)sh_QI, (void *)cb_AddRef, (void *)cb_Release, (void *)sh_Invoke};
static struct { void **lpVtbl; } g_stateHandler = {g_shVtbl};

static void drop_conn(int close) {
    if (!g_conn) return;
    if (g_connToken) ((RemoveHandler_t)VT(g_conn)[APC_RemoveStateChanged])(g_conn, g_connToken);
    g_connToken = 0;
    if (close) {
        void *closable = NULL;
        if (SUCCEEDED(((QI_t)VT(g_conn)[SLOT_QI])(g_conn, &IID_IClosable, &closable))) {
            ((NoArg_t)VT(closable)[CLOSABLE_Close])(closable);
            release(closable);
        }
    }
    release(g_conn);
    g_conn = NULL;
}

/* phones: list paired devices that can stream audio to this PC */
static void cmd_phones(void) {
    void *apcs = activation_factory(L"Windows.Media.Audio.AudioPlaybackConnection", &IID_IAudioPlaybackConnectionStatics);
    void *dis = activation_factory(L"Windows.Devices.Enumeration.DeviceInformation", &IID_IDeviceInformationStatics);
    HSTRING selector = NULL;
    void *op = NULL, *list = NULL;
    if (apcs && dis && SUCCEEDED(((OutHstr_t)VT(apcs)[APCS_GetDeviceSelector])(apcs, &selector)) &&
        SUCCEEDED(((HstrOutPtr_t)VT(dis)[DIS_FindAllAsyncAqsFilter])(dis, selector, &op)) && op &&
        SUCCEEDED(wait_async(op, 5000))) {
        call_out(op, ASYNCOP_GetResults, &list);
    }
    if (list) {
        UINT32 n = 0;
        ((OutU32_t)VT(list)[VV_Size])(list, &n);
        for (UINT32 i = 0; i < n; i++) {
            void *di = NULL;
            HSTRING id = NULL, name = NULL;
            if (FAILED(((GetAt_t)VT(list)[VV_GetAt])(list, i, &di)) || !di) continue;
            ((OutHstr_t)VT(di)[DI_Id])(di, &id);
            ((OutHstr_t)VT(di)[DI_Name])(di, &name);
            fputs("PHONE ", stdout); put_hstring(id); fputs("\t", stdout); put_hstring(name); fputs("\n", stdout);
            if (id) pWindowsDeleteString(id);
            if (name) pWindowsDeleteString(name);
            release(di);
        }
    }
    if (selector) pWindowsDeleteString(selector);
    release(list); release(op); release(dis); release(apcs);
    fputs("END\n", stdout);
}

/* first phone id (when none is configured) */
static int first_phone(WCHAR *out, size_t cap) {
    void *apcs = activation_factory(L"Windows.Media.Audio.AudioPlaybackConnection", &IID_IAudioPlaybackConnectionStatics);
    void *dis = activation_factory(L"Windows.Devices.Enumeration.DeviceInformation", &IID_IDeviceInformationStatics);
    HSTRING selector = NULL, id = NULL;
    void *op = NULL, *list = NULL, *di = NULL;
    int ok = 0;
    UINT32 n = 0;
    if (apcs && dis && SUCCEEDED(((OutHstr_t)VT(apcs)[APCS_GetDeviceSelector])(apcs, &selector)) &&
        SUCCEEDED(((HstrOutPtr_t)VT(dis)[DIS_FindAllAsyncAqsFilter])(dis, selector, &op)) && op &&
        SUCCEEDED(wait_async(op, 5000)) && SUCCEEDED(call_out(op, ASYNCOP_GetResults, &list)) && list &&
        SUCCEEDED(((OutU32_t)VT(list)[VV_Size])(list, &n)) && n > 0 &&
        SUCCEEDED(((GetAt_t)VT(list)[VV_GetAt])(list, 0, &di)) && di &&
        SUCCEEDED(((OutHstr_t)VT(di)[DI_Id])(di, &id)) && id) {
        UINT32 len = 0;
        const WCHAR *w = (const WCHAR *)pWindowsGetStringRawBuffer(id, &len);
        if (len < cap) { wcsncpy(out, w, cap - 1); out[cap - 1] = 0; ok = 1; }
    }
    if (id) pWindowsDeleteString(id);
    if (selector) pWindowsDeleteString(selector);
    release(di); release(list); release(op); release(dis); release(apcs);
    return ok;
}

/* phone connect|disconnect|toggle [device id] */
static void cmd_phone(const char *args) {
    if (!pRoGetActivationFactory || !pWindowsGetStringRawBuffer) { printf("ERR phone winrt unavailable\n"); return; }
    char verb[16] = "";
    const char *id_utf8 = "";
    const char *sp = strchr(args, ' ');
    size_t vlen = sp ? (size_t)(sp - args) : strlen(args);
    if (vlen >= sizeof verb) vlen = sizeof verb - 1;
    memcpy(verb, args, vlen); verb[vlen] = 0;
    if (sp) id_utf8 = sp + 1;

    int open = conn_state(g_conn);
    if (strcmp(verb, "status") == 0) { printf("OK phone=%d\n", open); return; }
    if (strcmp(verb, "toggle") == 0) strcpy(verb, open ? "disconnect" : "connect");

    if (strcmp(verb, "disconnect") == 0) {
        drop_conn(1);
        printf("OK phone=0\n");
        return;
    }
    if (strcmp(verb, "connect") != 0) { printf("ERR phone unknown verb '%s'\n", verb); return; }
    if (open) { printf("OK phone=1\n"); return; }
    drop_conn(1); /* stale, closed connection */

    WCHAR id[512] = L"";
    if (*id_utf8) MultiByteToWideChar(CP_UTF8, 0, id_utf8, -1, id, 512);
    else if (!first_phone(id, 512)) { printf("ERR phone no paired phone found\n"); return; }

    void *apcs = activation_factory(L"Windows.Media.Audio.AudioPlaybackConnection", &IID_IAudioPlaybackConnectionStatics);
    if (!apcs) { printf("ERR phone api unavailable (needs Windows 10 2004+)\n"); return; }
    HSTRING hid = NULL;
    void *conn = NULL;
    pWindowsCreateString(id, (UINT32)wcslen(id), &hid);
    HRESULT hr = ((HstrOutPtr_t)VT(apcs)[APCS_TryCreateFromId])(apcs, hid, &conn);
    pWindowsDeleteString(hid);
    release(apcs);
    if (FAILED(hr) || !conn) { printf("ERR phone device not available (paired and in range?)\n"); return; }

    ((AddHandler_t)VT(conn)[APC_AddStateChanged])(conn, &g_stateHandler, &g_connToken);
    g_conn = conn;

    void *result = NULL;
    int status = 3;
    if (SUCCEEDED(((NoArg_t)VT(conn)[APC_Start])(conn)) &&
        SUCCEEDED(call_out(conn, APC_Open, &result)) && result) {
        ((OutInt_t)VT(result)[APCR_Status])(result, &status);
        release(result);
    }
    if (status != 0) {
        static const char *why[] = {"ok", "phone did not answer (timeout)", "denied by system", "unknown failure"};
        drop_conn(1);
        printf("ERR phone %s\n", status >= 0 && status <= 3 ? why[status] : "failed");
        return;
    }
    printf("OK phone=1\n");
}


/* ======================================================================= media */
static const struct { const char *name; int appcmd; int slot; } MEDIA[] = {
    {"playpause", 14, SESSION_TryTogglePlayPauseAsync},
    {"next",      11, SESSION_TrySkipNextAsync},
    {"previous",  12, SESSION_TrySkipPreviousAsync},
    {"stop",      13, SESSION_TryStopAsync},
    {"mute",       8, 0},
    {"voldown",    9, 0},
    {"volup",     10, 0},
};

static void cmd_media(const char *cmd) {
    for (size_t i = 0; i < sizeof(MEDIA) / sizeof(MEDIA[0]); i++) {
        if (_stricmp(cmd, MEDIA[i].name) != 0) continue;
        if (MEDIA[i].slot && try_session(MEDIA[i].slot)) printf("OK media %s via session\n", cmd);
        else if (send_appcommand(MEDIA[i].appcmd)) printf("OK media %s via appcommand\n", cmd);
        else printf("ERR media %s taskbar window not found\n", cmd);
        return;
    }
    printf("ERR unknown media command '%s'\n", cmd);
}

/* ======================================================================= main */
static void run(char *line) {
    OUT_BEGIN();
    if (strncmp(line, "media ", 6) == 0) cmd_media(line + 6);
    else if (strcmp(line, "list") == 0) cmd_list();
    else if (strcmp(line, "status") == 0) cmd_status();
    else if (strcmp(line, "mic toggle") == 0) printf("OK mic=%d\n", mic_mute(2));
    else if (strcmp(line, "mic mute") == 0) printf("OK mic=%d\n", mic_mute(1));
    else if (strcmp(line, "mic unmute") == 0) printf("OK mic=%d\n", mic_mute(0));
    else if (strncmp(line, "setout ", 7) == 0) cmd_setout(line + 7);
    else if (strcmp(line, "phones") == 0) cmd_phones();
    else if (strncmp(line, "phone ", 6) == 0) cmd_phone(line + 6);
    else printf("ERR unknown command '%s'\n", line);
    OUT_END();
}

int main(int argc, char **argv) {
    InitializeCriticalSection(&g_out);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    init_winrt();
    pWindowsGetStringRawBuffer = (WindowsGetStringRawBuffer_t)GetProcAddress(GetModuleHandleW(L"combase.dll"), "WindowsGetStringRawBuffer");
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&g_enum))) {
        printf("ERR no audio device enumerator\n");
        return 1;
    }

    if (argc > 1) { /* one-shot mode for testing */
        char line[1024] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) strncat(line, " ", sizeof line - strlen(line) - 1);
            strncat(line, argv[i], sizeof line - strlen(line) - 1);
        }
        run(line);
        return 0;
    }

    /* event subscriptions: Windows calls us only when something changes */
    g_micChanged = CreateEventW(NULL, FALSE, FALSE, NULL);
    subscribe_mic();
    IMMDeviceEnumerator_RegisterEndpointNotificationCallback(g_enum, &g_nc);
    CreateThread(NULL, 64 * 1024, mic_watch_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);

    ensure_manager(); /* warm up the media session API so the first press is instant */

    OUT_BEGIN(); printf("READY\n"); OUT_END();
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) run(line);
    }
    return 0;
}

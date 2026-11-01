/* GameCube controllers straight from USB, on Windows (padusb.h): Nintendo's GameCube adapter
 * (057e:0337; other brands' adapters in their Wii U mode are the same device to a PC), and the
 * Switch 2's GameCube controller (057e:2073, Nintendo Switch Online) on a USB cable.
 *
 * SDL3 reads both only through libusb, and SDL3.dll as the installer downloads it has none: an
 * adapter whose driver Zadig made WinUSB (as Dolphin's setup asks) was invisible to it, and the
 * Switch 2 controller, which sends nothing until it is started with commands on its second (bulk)
 * interface, too. So these two devices are read here, through Windows' own libraries, loaded at
 * run time, and SDL is told to leave them alone:
 *
 * - The adapter: through WinUSB when that is its driver (its interrupt pipes), else through
 *   Windows' HID driver (its reports). 0x13 starts it; report 0x21 then holds its four ports, 9
 *   bytes each; 0x11 and a byte per port (0 stop, 1 rumble, 2 stop hard) drive the motors.
 * - The Switch 2 controller: interface 1 (bulk) takes the start-up commands and gives the sticks'
 *   calibration, through WinUSB, which needs Zadig unless Windows gave it that driver itself;
 *   interface 0 (HID, Windows' driver) then sends 64-byte input reports and takes rumble reports.
 *   Not over Bluetooth (SDL can't either).
 *
 * The protocol is SDL's (its HIDAPI GameCube and Switch 2 drivers, zlib licence). A thread looks
 * for the devices every second; each one found is read on a thread of its own, and padusb_poll
 * copies what they saw. SSBU_GC_USB=0 leaves both devices to SDL, as before. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "padusb.h"

#ifndef _WIN32
const char *padusb_start(void) { return NULL; }
void padusb_poll(PadUsbState out[PADUSB_PORTS]) { memset(out, 0, sizeof(PadUsbState) * PADUSB_PORTS); }
void padusb_rumble(int port, int on) { (void)port; (void)on; }
#else
#include <setupapi.h>

/* ---- Windows' libraries, loaded at run time (declared here: no DDK headers needed) ----------- */
typedef struct HidCaps {
    USHORT usage, usage_page, input_len, output_len, feature_len, reserved[17];
    USHORT counts[10];
} HidCaps;
typedef struct UsbInterfaceDescriptor {
    UCHAR length, type, number, alternate, endpoints, class_, subclass, protocol, string;
} UsbInterfaceDescriptor;
typedef struct WinUsbPipe { int type; UCHAR id; USHORT max_packet; UCHAR interval; } WinUsbPipe;   /* type 2 bulk, 3 interrupt */
#pragma pack(push, 1)
typedef struct WinUsbSetup { UCHAR request_type, request; USHORT value, index, length; } WinUsbSetup;
#pragma pack(pop)
enum { PIPE_TIMEOUT_POLICY = 3 };

static struct {
    HDEVINFO (WINAPI *GetClassDevs)(const GUID *, PCWSTR, HWND, DWORD);
    BOOL (WINAPI *EnumInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
    BOOL (WINAPI *InterfaceDetail)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD, PSP_DEVINFO_DATA);
    BOOL (WINAPI *EnumInfo)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
    BOOL (WINAPI *InstanceId)(HDEVINFO, PSP_DEVINFO_DATA, PWSTR, DWORD, PDWORD);
    HKEY (WINAPI *OpenDevKey)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, DWORD, DWORD, REGSAM);
    BOOL (WINAPI *DestroyList)(HDEVINFO);
    LSTATUS (WINAPI *RegQuery)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
    LSTATUS (WINAPI *RegClose)(HKEY);
    BOOLEAN (WINAPI *HidPreparsed)(HANDLE, void **);
    BOOLEAN (WINAPI *HidFreePreparsed)(void *);
    LONG (WINAPI *HidCapsOf)(void *, HidCaps *);
    BOOL (WINAPI *UsbInit)(HANDLE, void **);
    BOOL (WINAPI *UsbFree)(void *);
    BOOL (WINAPI *UsbInterface)(void *, UCHAR, UsbInterfaceDescriptor *);
    BOOL (WINAPI *UsbPipe)(void *, UCHAR, UCHAR, WinUsbPipe *);
    BOOL (WINAPI *UsbPolicy)(void *, UCHAR, ULONG, ULONG, void *);
    BOOL (WINAPI *UsbRead)(void *, UCHAR, UCHAR *, ULONG, ULONG *, OVERLAPPED *);
    BOOL (WINAPI *UsbWrite)(void *, UCHAR, UCHAR *, ULONG, ULONG *, OVERLAPPED *);
    BOOL (WINAPI *UsbControl)(void *, WinUsbSetup, UCHAR *, ULONG, ULONG *, OVERLAPPED *);
} api;

static int load_api(void) {
    HMODULE setup = LoadLibraryW(L"setupapi.dll"), adv = LoadLibraryW(L"advapi32.dll"), hid = LoadLibraryW(L"hid.dll");
    HMODULE usb = LoadLibraryW(L"winusb.dll");
    if (!setup || !adv || !hid) return 0;
#define GET(field, m, name) (*(FARPROC *)&api.field = GetProcAddress(m, name))
    int ok = GET(GetClassDevs, setup, "SetupDiGetClassDevsW") && GET(EnumInterfaces, setup, "SetupDiEnumDeviceInterfaces") &&
             GET(InterfaceDetail, setup, "SetupDiGetDeviceInterfaceDetailW") && GET(EnumInfo, setup, "SetupDiEnumDeviceInfo") &&
             GET(InstanceId, setup, "SetupDiGetDeviceInstanceIdW") && GET(OpenDevKey, setup, "SetupDiOpenDevRegKey") &&
             GET(DestroyList, setup, "SetupDiDestroyDeviceInfoList") && GET(RegQuery, adv, "RegQueryValueExW") &&
             GET(RegClose, adv, "RegCloseKey") && GET(HidPreparsed, hid, "HidD_GetPreparsedData") &&
             GET(HidFreePreparsed, hid, "HidD_FreePreparsedData") && GET(HidCapsOf, hid, "HidP_GetCaps");
    if (usb && !(GET(UsbInit, usb, "WinUsb_Initialize") && GET(UsbFree, usb, "WinUsb_Free") &&
                 GET(UsbInterface, usb, "WinUsb_QueryInterfaceSettings") && GET(UsbPipe, usb, "WinUsb_QueryPipe") &&
                 GET(UsbPolicy, usb, "WinUsb_SetPipePolicy") && GET(UsbRead, usb, "WinUsb_ReadPipe") &&
                 GET(UsbWrite, usb, "WinUsb_WritePipe") && GET(UsbControl, usb, "WinUsb_ControlTransfer")))
        api.UsbInit = NULL;
#undef GET
    return ok;
}

/* ---- finding the devices ------------------------------------------------------------------ */
enum { KIND_ADAPTER, KIND_SWITCH2 };
static const GUID HID_INTERFACE = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

typedef struct Found { wchar_t path[260]; int kind, winusb; } Found;

static int contains(const wchar_t *s, const wchar_t *what) {   /* ignoring case */
    size_t n = wcslen(what);
    for (; *s; s++)
        if (!_wcsnicmp(s, what, n)) return 1;
    return 0;
}

/* Which device a path or instance id names: the adapter, the Switch 2 controller's interface
 * (0 for HID, 1 for its bulk one), or -1. */
static int kind_of(const wchar_t *s, int bulk) {
    if (contains(s, L"vid_057e&pid_0337")) return KIND_ADAPTER;
    if (contains(s, L"vid_057e&pid_2073") && (bulk ? contains(s, L"&mi_01") : !contains(s, L"&mi_01"))) return KIND_SWITCH2;
    return -1;
}

static void add_interfaces(const GUID *g, int bulk, Found *out, int *n, int max) {
    HDEVINFO set = api.GetClassDevs(g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA itf = {.cbSize = sizeof itf};
    union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; char space[sizeof(DWORD) + 260 * sizeof(wchar_t)]; } detail;
    for (DWORD i = 0; *n < max && api.EnumInterfaces(set, NULL, g, i, &itf); i++) {
        detail.d.cbSize = sizeof detail.d;
        if (!api.InterfaceDetail(set, &itf, &detail.d, sizeof detail, NULL, NULL)) continue;
        int kind = kind_of(detail.d.DevicePath, bulk);
        if (kind < 0) continue;
        int dup = 0;
        for (int k = 0; k < *n && !dup; k++) dup = !_wcsicmp(out[k].path, detail.d.DevicePath);
        if (dup) continue;
        wcsncpy(out[*n].path, detail.d.DevicePath, 259);
        out[*n].path[259] = 0;
        out[*n].kind = kind;
        out[*n].winusb = bulk;
        (*n)++;
    }
    api.DestroyList(set);
}

/* The interfaces with the WinUSB driver: a USB device node of ours whose registry entry has the
 * interface GUID WinUSB registered it under (Zadig, or Windows from the device's own descriptors). */
static void add_winusb(Found *out, int *n, int max) {
    HDEVINFO set = api.GetClassDevs(NULL, L"USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA info = {.cbSize = sizeof info};
    wchar_t id[256];
    for (DWORD i = 0; api.EnumInfo(set, i, &info); i++) {
        if (!api.InstanceId(set, &info, id, 256, NULL) || kind_of(id, 1) < 0) continue;
        HKEY key = api.OpenDevKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        wchar_t guids[512] = {0};
        DWORD size = sizeof guids - sizeof(wchar_t), type;
        if (api.RegQuery(key, L"DeviceInterfaceGUIDs", NULL, &type, (BYTE *)guids, &size) &&
            (size = sizeof guids - sizeof(wchar_t), api.RegQuery(key, L"DeviceInterfaceGUID", NULL, &type, (BYTE *)guids, &size)))
            guids[0] = 0;
        api.RegClose(key);
        unsigned a, b, c, d[8];
        if (swscanf(guids, L"{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5],
                    &d[6], &d[7]) != 11)
            continue;
        GUID g = {a, (USHORT)b, (USHORT)c, {(UCHAR)d[0], (UCHAR)d[1], (UCHAR)d[2], (UCHAR)d[3], (UCHAR)d[4], (UCHAR)d[5], (UCHAR)d[6], (UCHAR)d[7]}};
        add_interfaces(&g, 1, out, n, max);
    }
    api.DestroyList(set);
}

/* ---- the ports, shared with the input thread ---------------------------------------------- */
static SRWLOCK usb_lock = SRWLOCK_INIT;
static PadUsbState usb_ports[PADUSB_PORTS];
static int port_used[PADUSB_PORTS];        /* (by a device; under the lock) */
static int rumble_want[PADUSB_PORTS];

typedef struct Axis { int seen; int centre, reach; } Axis;   /* a stick's axis as it has moved */

static i32 stick_value(Axis *a, int v, int centre, int reach) {
    if (!a->seen) { a->seen = 1; a->centre = centre; a->reach = reach; }
    int dv = v - a->centre, mag = dv < 0 ? -dv : dv;
    if (mag > a->reach) a->reach = mag;   /* a stick that reaches further than thought */
    long r = (long)dv * 32767 / a->reach;
    return (i32)(r > 32767 ? 32767 : r < -32767 ? -32767 : r);
}

/* ---- the adapter ---------------------------------------------------------------------------- */
typedef struct Device {
    int kind;
    wchar_t path[260], bulk_path[260];
    HANDLE file, bulk_file;
    void *usb, *bulk;            /* WinUSB handles: the adapter's, the Switch 2 controller's interface 1 */
    UCHAR in, out, bulk_in, bulk_out;
    USHORT in_len, out_len;      /* through HID: its reports' sizes (with their id) */
    int first;                   /* its ports: first .. first + 3 (the adapter) or first */
    volatile LONG done;          /* the reading thread has stopped */
    Axis axes[4][6];
    int trigger_rest[4][2], trigger_top[4][2];
    struct { int neutral[2], min[2], max[2]; } calib[2];   /* (Switch 2) the sticks, from its memory */
    int trigger_zero[2];
    int started;                 /* (Switch 2) interface 1 took the start-up commands */
} Device;

static Device *devices[8];

static int hid_write(Device *d, const UCHAR *data, int n) {
    UCHAR buf[128] = {0};
    int len = d->out_len < sizeof buf ? d->out_len : (int)sizeof buf;
    if (len <= 0) return 0;
    memcpy(buf, data, n < len ? n : len);
    OVERLAPPED ov = {.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL)};
    DWORD put = 0;
    BOOL ok = WriteFile(d->file, buf, (DWORD)len, NULL, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 200) == WAIT_OBJECT_0) ok = GetOverlappedResult(d->file, &ov, &put, FALSE);
        else { CancelIoEx(d->file, &ov); GetOverlappedResult(d->file, &ov, &put, TRUE); }
    }
    CloseHandle(ov.hEvent);
    return ok;
}

static int dev_write(Device *d, const UCHAR *data, int n) {
    if (!d->usb) return hid_write(d, data, n);
    ULONG put = 0;
    return api.UsbWrite(d->usb, d->out, (UCHAR *)data, (ULONG)n, &put, NULL) && put == (ULONG)n;
}

static void adapter_report(Device *d, const UCHAR *p, int n) {
    if (n < 37 || p[0] != 0x21) return;
    AcquireSRWLockExclusive(&usb_lock);
    for (int i = 0; i < 4; i++) {
        const UCHAR *s = p + 1 + 9 * i;
        PadUsbState *o = &usb_ports[d->first + i];
        int was = o->connected;
        o->connected = (s[0] & 0x30) != 0;   /* 0x10 a cable, 0x20 a WaveBird */
        if (!o->connected) continue;
        if (!was) {   /* a controller plugged in: its sticks' centres and triggers' rests as they are now */
            memset(d->axes[i], 0, sizeof d->axes[i]);
            for (int t = 0; t < 2; t++) {
                d->trigger_rest[i][t] = s[7 + t] < 70 ? s[7 + t] : 30;
                d->trigger_top[i][t] = d->trigger_rest[i][t] + 140;
            }
        }
        static const struct { int off; UCHAR bit; u32 pu; } BUTTONS[] = {
            {1, 0x01, PU_A}, {1, 0x02, PU_B}, {1, 0x04, PU_X}, {1, 0x08, PU_Y}, {1, 0x10, PU_LEFT}, {1, 0x20, PU_RIGHT},
            {1, 0x40, PU_DOWN}, {1, 0x80, PU_UP}, {2, 0x01, PU_START}, {2, 0x02, PU_Z}, {2, 0x04, PU_R}, {2, 0x08, PU_L},
        };
        o->buttons = 0;
        for (int b = 0; b < (int)(sizeof BUTTONS / sizeof *BUTTONS); b++)
            if (s[BUTTONS[b].off] & BUTTONS[b].bit) o->buttons |= BUTTONS[b].pu;
        int centre = 128;
        o->lx = stick_value(&d->axes[i][0], s[3], s[3] > 80 && s[3] < 176 ? s[3] : centre, 80);
        o->ly = -stick_value(&d->axes[i][1], s[4], s[4] > 80 && s[4] < 176 ? s[4] : centre, 80);
        o->rx = stick_value(&d->axes[i][2], s[5], s[5] > 80 && s[5] < 176 ? s[5] : centre, 80);
        o->ry = -stick_value(&d->axes[i][3], s[6], s[6] > 80 && s[6] < 176 ? s[6] : centre, 80);
        for (int t = 0; t < 2; t++) {
            int v = s[7 + t], rest = d->trigger_rest[i][t];
            if (v > d->trigger_top[i][t]) d->trigger_top[i][t] = v;
            int val = v <= rest ? 0 : (v - rest) * 32767 / (d->trigger_top[i][t] - rest);
            *(t ? &o->rt : &o->lt) = val > 32767 ? 32767 : val;
        }
    }
    ReleaseSRWLockExclusive(&usb_lock);
}

/* ---- the Switch 2 controller -------------------------------------------------------------- */
static int bulk_send(Device *d, const UCHAR *data, int n) {
    ULONG put = 0;
    return api.UsbWrite(d->bulk, d->bulk_out, (UCHAR *)data, (ULONG)n, &put, NULL) ? (int)put : -1;
}

static int bulk_recv(Device *d, UCHAR *data, int size) {
    int total = 0;
    while (size > 0) {
        ULONG got = 0, want = size > 64 ? 64 : (ULONG)size;
        if (!api.UsbRead(d->bulk, d->bulk_in, data, want, &got, NULL)) return total ? total : -1;
        total += (int)got; data += got; size -= (int)got;
        if (got < want) break;
    }
    return total;
}

/* 0x40 bytes of the controller's memory from `address`. */
static int read_flash(Device *d, u32 address, UCHAR out[0x40]) {
    UCHAR cmd[16] = {0x02, 0x91, 0x00, 0x01, 0x00, 0x08, 0x00, 0x00, 0, 0, 0, 0,
                     (UCHAR)address, (UCHAR)(address >> 8), (UCHAR)(address >> 16), (UCHAR)(address >> 24)};
    UCHAR reply[0x50] = {0};
    if (bulk_send(d, cmd, sizeof cmd) < 0 || bulk_recv(d, reply, sizeof reply) < 0x50) return 0;
    memcpy(out, reply + 0x10, 0x40);
    return 1;
}

static void parse_calibration(Device *d, int stick, const UCHAR *c) {
    d->calib[stick].neutral[0] = c[0] | (c[1] & 0x0F) << 8;
    d->calib[stick].neutral[1] = c[1] >> 4 | c[2] << 4;
    d->calib[stick].max[0] = c[3] | (c[4] & 0x0F) << 8;
    d->calib[stick].max[1] = c[4] >> 4 | c[5] << 4;
    d->calib[stick].min[0] = c[6] | (c[7] & 0x0F) << 8;
    d->calib[stick].min[1] = c[7] >> 4 | c[8] << 4;
}

/* Interface 1: the sticks' calibration, then the commands that make it send input reports. */
static int switch2_start(Device *d) {
    UsbInterfaceDescriptor desc;
    if (!api.UsbInterface(d->bulk, 0, &desc)) return 0;
    for (UCHAR e = 0; e < desc.endpoints; e++) {
        WinUsbPipe p;
        if (!api.UsbPipe(d->bulk, 0, e, &p) || p.type != 2) continue;
        if (p.id & 0x80) d->bulk_in = p.id;
        else d->bulk_out = p.id;
    }
    if (!d->bulk_in || !d->bulk_out) return 0;
    ULONG timeout = 200;
    api.UsbPolicy(d->bulk, d->bulk_in, PIPE_TIMEOUT_POLICY, sizeof timeout, &timeout);
    api.UsbPolicy(d->bulk, d->bulk_out, PIPE_TIMEOUT_POLICY, sizeof timeout, &timeout);
    UCHAR block[0x40];
    if (read_flash(d, 0x13080, block)) parse_calibration(d, 0, block + 0x28);
    if (read_flash(d, 0x130C0, block)) parse_calibration(d, 1, block + 0x28);
    if (read_flash(d, 0x13140, block)) { d->trigger_zero[0] = block[0]; d->trigger_zero[1] = block[1]; }
    if (read_flash(d, 0x1FC040, block) && block[0] == 0xB2 && block[1] == 0xA1) parse_calibration(d, 0, block + 2);   /* the user's own */
    if (read_flash(d, 0x1FC080, block) && block[0] == 0xB2 && block[1] == 0xA1) parse_calibration(d, 1, block + 2);
    static const UCHAR INIT[][28] = {
        {0x07, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},
        {0x0c, 0x91, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00},   /* feature output bits */
        {0x11, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},
        {0x0a, 0x91, 0x00, 0x08, 0x00, 0x14, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0x35, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x0c, 0x91, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00},   /* enable them */
        {0x01, 0x91, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00},
        {0x01, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},                           /* rumble */
        {0x08, 0x91, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00},
        {0x03, 0x91, 0x00, 0x0a, 0x00, 0x04, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00},   /* report format */
        {0x03, 0x91, 0x00, 0x0d, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},   /* start */
    };
    for (int i = 0; i < (int)(sizeof INIT / sizeof *INIT); i++) {
        if (bulk_send(d, INIT[i], INIT[i][5] + 8) < 0) return 0;
        bulk_recv(d, block, sizeof block);
    }
    return 1;
}

static i32 switch2_axis(Device *d, int stick, int axis, int v) {
    int n = d->calib[stick].neutral[axis], lo = d->calib[stick].min[axis], hi = d->calib[stick].max[axis];
    if (!n || !lo || !hi) return stick_value(&d->axes[0][stick * 2 + axis], v, 2048, 1400);   /* no calibration */
    long r = v >= n ? (long)(v - n) * 32767 / hi : (long)(v - n) * 32767 / lo;
    return (i32)(r > 32767 ? 32767 : r < -32767 ? -32767 : r);
}

static void switch2_report(Device *d, const UCHAR *p, int n) {
    if (n < 63) return;
    static const struct { int off; UCHAR bit; u32 pu; } BUTTONS[] = {
        {5, 0x01, PU_Y}, {5, 0x02, PU_X}, {5, 0x04, PU_B}, {5, 0x08, PU_A}, {5, 0x40, PU_R}, {5, 0x80, PU_Z},
        {6, 0x02, PU_START}, {6, 0x10, PU_HOME}, {6, 0x20, PU_CAPTURE}, {6, 0x40, PU_C},
        {7, 0x01, PU_DOWN}, {7, 0x02, PU_UP}, {7, 0x04, PU_RIGHT}, {7, 0x08, PU_LEFT}, {7, 0x40, PU_L}, {7, 0x80, PU_ZL},
    };
    AcquireSRWLockExclusive(&usb_lock);
    PadUsbState *o = &usb_ports[d->first];
    o->connected = 1;
    o->buttons = 0;
    for (int b = 0; b < (int)(sizeof BUTTONS / sizeof *BUTTONS); b++)
        if (p[BUTTONS[b].off] & BUTTONS[b].bit) o->buttons |= BUTTONS[b].pu;
    o->lx = switch2_axis(d, 0, 0, p[11] | (p[12] & 0x0F) << 8);
    o->ly = -switch2_axis(d, 0, 1, p[12] >> 4 | p[13] << 4);
    o->rx = switch2_axis(d, 1, 0, p[14] | (p[15] & 0x0F) << 8);
    o->ry = -switch2_axis(d, 1, 1, p[15] >> 4 | p[16] << 4);
    for (int t = 0; t < 2; t++) {
        int zero = d->trigger_zero[t], v = p[61 + t];
        int val = v <= zero || zero >= 232 ? 0 : (v - zero) * 32767 / (232 - zero);
        *(t ? &o->rt : &o->lt) = val > 32767 ? 32767 : val;
    }
    ReleaseSRWLockExclusive(&usb_lock);
}

/* ---- reading a device ------------------------------------------------------------------------ */
static void release_ports(Device *d) {
    int count = d->kind == KIND_ADAPTER ? 4 : 1;
    AcquireSRWLockExclusive(&usb_lock);
    for (int i = 0; i < count; i++) {
        memset(&usb_ports[d->first + i], 0, sizeof usb_ports[0]);
        port_used[d->first + i] = 0;
        rumble_want[d->first + i] = 0;
    }
    ReleaseSRWLockExclusive(&usb_lock);
}

static void close_device(Device *d) {
    if (d->usb) api.UsbFree(d->usb);
    if (d->bulk) api.UsbFree(d->bulk);
    if (d->file && d->file != INVALID_HANDLE_VALUE) CloseHandle(d->file);
    if (d->bulk_file && d->bulk_file != INVALID_HANDLE_VALUE) CloseHandle(d->bulk_file);
    d->usb = d->bulk = NULL;
    d->file = d->bulk_file = NULL;
}

/* Rumble as the game last asked: the adapter's motors when one changes; the Switch 2
 * controller's every 30 ms while it runs (it stops by itself), and once when it stops. */
static void send_rumble(Device *d, int sent[4], ULONGLONG *sent_at, unsigned *seq) {
    int want[4] = {0}, count = d->kind == KIND_ADAPTER ? 4 : 1, changed = 0;
    AcquireSRWLockShared(&usb_lock);
    for (int i = 0; i < count; i++) {
        want[i] = rumble_want[d->first + i] && usb_ports[d->first + i].connected;
        changed |= want[i] != sent[i];
    }
    ReleaseSRWLockShared(&usb_lock);
    ULONGLONG now = GetTickCount64();
    if (d->kind == KIND_ADAPTER) {
        if (!changed) return;
        UCHAR packet[5] = {0x11};
        for (int i = 0; i < 4; i++) packet[1 + i] = want[i] ? 1 : sent[i] ? 2 : 0;
        dev_write(d, packet, sizeof packet);   /* (without the second cable it rumbles nothing: no retry) */
        memcpy(sent, want, sizeof want);
        return;
    }
    if (!changed && !(want[0] && now - *sent_at >= 30)) return;
    UCHAR packet[64] = {0x03, (UCHAR)(0x50 | ((*seq)++ & 0x0F)), (UCHAR)(want[0] ? 1 : 2)};
    hid_write(d, packet, sizeof packet);
    sent[0] = want[0];
    *sent_at = now;
}

static DWORD WINAPI device_thread(LPVOID param) {
    Device *d = param;
    SetThreadDescription(GetCurrentThread(), L"input usb");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    UCHAR buf[128];
    int sent[4] = {0}, reports = 0;
    ULONGLONG sent_at = 0;
    unsigned seq = 0;
    OVERLAPPED ov = {.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL)};
    int pending = 0;
    for (;;) {
        int n = -1;
        if (d->usb) {   /* the adapter through WinUSB */
            ULONG got = 0;
            if (api.UsbRead(d->usb, d->in, buf, 37, &got, NULL)) n = (int)got;
            else if (GetLastError() == ERROR_SEM_TIMEOUT) n = 0;
        } else {        /* a HID interface */
            DWORD got = 0;
            if (!pending) {
                ResetEvent(ov.hEvent);
                if (ReadFile(d->file, buf, d->in_len, NULL, &ov) || GetLastError() == ERROR_IO_PENDING) pending = 1;
                else break;
            }
            DWORD w = WaitForSingleObject(ov.hEvent, 50);
            if (w == WAIT_TIMEOUT) n = 0;
            else if (GetOverlappedResult(d->file, &ov, &got, FALSE)) { n = (int)got; pending = 0; }
            else break;
        }
        if (n < 0) break;
        if (n > 0) {
            const UCHAR *p = buf;
            if (d->kind == KIND_SWITCH2 && !d->usb && d->in_len > 64 && buf[0] == 0) { p++; n--; }   /* (no report ids) */
            if (!reports++)
                fprintf(stderr, "[input] %s: first report, %d bytes, id 0x%02x\n",
                        d->kind == KIND_ADAPTER ? "GameCube adapter" : "Switch 2 GameCube controller", n, p[0]);
            if (d->kind == KIND_ADAPTER) adapter_report(d, p, n);
            else if (d->started || p[0] == 0x05) switch2_report(d, p, n);   /* (else: not in the format it knows) */
        }
        send_rumble(d, sent, &sent_at, &seq);
    }
    if (pending) { CancelIoEx(d->file, &ov); DWORD got; GetOverlappedResult(d->file, &ov, &got, TRUE); }
    CloseHandle(ov.hEvent);
    fprintf(stderr, "[input] %s disconnected (error %lu)\n",
            d->kind == KIND_ADAPTER ? "GameCube adapter" : "Switch 2 GameCube controller", GetLastError());
    close_device(d);
    release_ports(d);
    InterlockedExchange(&d->done, 1);
    return 0;
}

static HANDLE open_path(const wchar_t *path) {
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
}

/* Ports for a new device: `count` free in a row, or -1. */
static int take_ports(int count) {
    int first = -1;
    AcquireSRWLockExclusive(&usb_lock);
    for (int i = 0; i + count <= PADUSB_PORTS && first < 0; i++) {
        int free = 1;
        for (int k = 0; k < count; k++) free &= !port_used[i + k];
        if (free) first = i;
    }
    for (int k = 0; first >= 0 && k < count; k++) port_used[first + k] = 1;
    ReleaseSRWLockExclusive(&usb_lock);
    return first;
}

static void name_ports(Device *d, int count) {
    char path[160];
    WideCharToMultiByte(CP_UTF8, 0, d->path, -1, path, sizeof path, NULL, NULL);
    path[sizeof path - 1] = 0;
    AcquireSRWLockExclusive(&usb_lock);
    for (int i = 0; i < count; i++) {
        PadUsbState *o = &usb_ports[d->first + i];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "Nintendo GameCube Controller");
        if (d->kind == KIND_ADAPTER) snprintf(o->id, sizeof o->id, "gcadapter-%d", i + 1);
        else snprintf(o->id, sizeof o->id, "gcswitch2");
        snprintf(o->path, sizeof o->path, "%s#%d", path, i + 1);
    }
    ReleaseSRWLockExclusive(&usb_lock);
}

/* Opens what was found and starts its thread; 0 when it can't be read. */
static int open_device(Device *d, const Found *f, const wchar_t *bulk_path) {
    d->kind = f->kind;
    wcscpy(d->path, f->path);
    d->file = open_path(f->path);
    if (d->file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[input] %s: can't open it (error %lu)\n",
                f->kind == KIND_ADAPTER ? "GameCube adapter" : "Switch 2 GameCube controller", GetLastError());
        d->file = NULL;
        return 0;
    }
    if (f->winusb) {   /* the adapter, WinUSB: its interrupt pipes; 0x13 starts it */
        if (!api.UsbInit || !api.UsbInit(d->file, &d->usb)) {
            fprintf(stderr, "[input] GameCube adapter: its driver isn't WinUSB's (error %lu): pick WinUSB in Zadig\n", GetLastError());
            d->usb = NULL;
            close_device(d);
            return 0;
        }
        UsbInterfaceDescriptor desc;
        if (api.UsbInterface(d->usb, 0, &desc))
            for (UCHAR e = 0; e < desc.endpoints; e++) {
                WinUsbPipe p;
                if (!api.UsbPipe(d->usb, 0, e, &p) || p.type != 3) continue;
                if (p.id & 0x80) d->in = p.id;
                else d->out = p.id;
            }
        if (!d->in || !d->out) { fprintf(stderr, "[input] GameCube adapter: no interrupt pipes\n"); close_device(d); return 0; }
        ULONG timeout = 100;
        api.UsbPolicy(d->usb, d->in, PIPE_TIMEOUT_POLICY, sizeof timeout, &timeout);
        api.UsbPolicy(d->usb, d->out, PIPE_TIMEOUT_POLICY, sizeof timeout, &timeout);
        WinUsbSetup protocol = {0x21, 11, 1, 0, 0};   /* (HID SET_PROTOCOL: other brands' adapters need it) */
        ULONG got = 0;
        api.UsbControl(d->usb, protocol, NULL, 0, &got, NULL);
    } else {   /* HID: its reports' sizes */
        void *pre = NULL;
        HidCaps caps = {0};
        if (api.HidPreparsed(d->file, &pre)) { api.HidCapsOf(pre, &caps); api.HidFreePreparsed(pre); }
        d->in_len = caps.input_len ? caps.input_len : 64;
        d->out_len = caps.output_len;
        if (d->in_len > 128) d->in_len = 128;
    }
    if (d->kind == KIND_SWITCH2) {   /* interface 1 starts it */
        int started = 0;
        if (bulk_path && *bulk_path && api.UsbInit) {
            wcscpy(d->bulk_path, bulk_path);
            d->bulk_file = open_path(bulk_path);
            if (d->bulk_file != INVALID_HANDLE_VALUE && api.UsbInit(d->bulk_file, &d->bulk)) started = switch2_start(d);
        }
        if (!started)
            fprintf(stderr, "[input] Switch 2 GameCube controller: can't start it: its interface 1 %s (Zadig: Options > List All "
                            "Devices, \"Nintendo GameCube Controller (Interface 1)\", WinUSB, Replace Driver)\n",
                    bulk_path && *bulk_path ? "didn't answer" : "has no WinUSB driver");
        else fprintf(stderr, "[input] Switch 2 GameCube controller started\n");
        d->started = started;
    } else {
        UCHAR start = 0x13;
        if (!dev_write(d, &start, 1))
            fprintf(stderr, "[input] GameCube adapter (%s): it didn't take the start command (error %lu)\n",
                    d->usb ? "WinUSB" : "Windows' HID driver", GetLastError());
        else fprintf(stderr, "[input] GameCube adapter found (%s)\n", d->usb ? "WinUSB" : "Windows' HID driver");
        Sleep(10);
    }
    int count = d->kind == KIND_ADAPTER ? 4 : 1;
    if ((d->first = take_ports(count)) < 0) { close_device(d); return 0; }
    name_ports(d, count);
    HANDLE t = CreateThread(NULL, 0, device_thread, d, 0, NULL);
    if (!t) { close_device(d); release_ports(d); return 0; }
    CloseHandle(t);
    return 1;
}

/* Every second: devices that came. A path that couldn't be opened is tried again after 10 s. */
static DWORD WINAPI finder_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"input usb finder");
    static struct { wchar_t path[260]; ULONGLONG at; } failed[16];
    for (;;) {
        Found found[24];
        int n = 0;
        add_winusb(found, &n, 24);
        int bulk_end = n;   /* found[0 .. bulk_end): WinUSB interfaces */
        add_interfaces(&HID_INTERFACE, 0, found, &n, 24);
        ULONGLONG now = GetTickCount64();
        for (int i = 0; i < (int)(sizeof devices / sizeof *devices); i++)   /* threads that ended */
            if (devices[i] && devices[i]->done) { free(devices[i]); devices[i] = NULL; }
        int bulk_taken[24] = {0};
        for (int i = 0; i < (int)(sizeof devices / sizeof *devices); i++)
            for (int k = 0; devices[i] && k < bulk_end; k++)
                if (!_wcsicmp(devices[i]->bulk_path, found[k].path)) bulk_taken[k] = 1;
        for (int f = 0; f < n; f++) {
            if (f < bulk_end && found[f].kind == KIND_SWITCH2) continue;   /* (a Switch 2 controller is opened by its HID side) */
            int open = 0, slot = -1, recent = 0;
            for (int i = 0; i < (int)(sizeof devices / sizeof *devices); i++) {
                if (devices[i] && !_wcsicmp(devices[i]->path, found[f].path)) open = 1;
                if (!devices[i] && slot < 0) slot = i;
            }
            for (int k = 0; k < 16; k++) recent |= !_wcsicmp(failed[k].path, found[f].path) && now - failed[k].at < 10000;
            if (open || recent || slot < 0) continue;
            const wchar_t *bulk = L"";
            for (int k = 0; found[f].kind == KIND_SWITCH2 && k < bulk_end && !*bulk; k++)
                if (found[k].kind == KIND_SWITCH2 && !bulk_taken[k]) { bulk = found[k].path; bulk_taken[k] = 1; }
            Device *d = calloc(1, sizeof *d);
            if (!d) continue;
            if (open_device(d, &found[f], bulk)) devices[slot] = d;
            else {
                free(d);
                int k = 0;
                for (int j = 1; j < 16; j++) if (failed[j].at < failed[k].at) k = j;
                wcscpy(failed[k].path, found[f].path);
                failed[k].at = now;
            }
        }
        Sleep(1000);
    }
}

const char *padusb_start(void) {
    const char *env = getenv("SSBU_GC_USB");
    if (env && !strcmp(env, "0")) return NULL;
    if (!load_api()) { fprintf(stderr, "[input] no setupapi/hid: GameCube adapters through SDL only\n"); return NULL; }
    HANDLE t = CreateThread(NULL, 0, finder_thread, NULL, 0, NULL);
    if (!t) return NULL;
    CloseHandle(t);
    fprintf(stderr, "[input] GameCube adapters and Switch 2 GameCube controllers: read over USB here (WinUSB %s)\n",
            api.UsbInit ? "available" : "missing");
    return "0x057e/0x0337,0x057e/0x2073";
}

void padusb_poll(PadUsbState out[PADUSB_PORTS]) {
    AcquireSRWLockShared(&usb_lock);
    memcpy(out, usb_ports, sizeof usb_ports);
    ReleaseSRWLockShared(&usb_lock);
}

void padusb_rumble(int port, int on) {
    if (port < 0 || port >= PADUSB_PORTS) return;
    AcquireSRWLockExclusive(&usb_lock);
    rumble_want[port] = on;
    ReleaseSRWLockExclusive(&usb_lock);
}
#endif

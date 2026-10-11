/* ST-LINK 虛擬 COM 埠：自動找、115200 8N1、斷線後重開。 */
#include "bridge.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

static HANDLE s_port = INVALID_HANDLE_VALUE;
static char   s_name[32];

/* 登錄檔 Enum\USB\VID_0483&PID_xxxx\<序號>\Device Parameters\PortName：
 * VID 0483 是 ST，ST-LINK 的虛擬 COM 埠都掛在這底下（V2-1、V3 的 PID 不一樣）。 */
static bool find_stlink_port(char *out, size_t cap)
{
    HKEY usb;
    char vidpid[128], serial[128], path[512], port[32];
    bool found = false;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB", 0,
                      KEY_READ, &usb) != ERROR_SUCCESS) {
        return false;
    }
    for (DWORD i = 0; !found; i++) {
        DWORD n = sizeof(vidpid);
        if (RegEnumKeyExA(usb, i, vidpid, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) { break; }
        if (_strnicmp(vidpid, "VID_0483&PID_", 13) != 0) { continue; }
        HKEY dev;
        if (RegOpenKeyExA(usb, vidpid, 0, KEY_READ, &dev) != ERROR_SUCCESS) { continue; }
        for (DWORD j = 0; !found; j++) {
            DWORD m = sizeof(serial);
            if (RegEnumKeyExA(dev, j, serial, &m, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) { break; }
            snprintf(path, sizeof(path), "%s\\Device Parameters", serial);
            DWORD len = sizeof(port), type;
            HKEY params;
            if (RegOpenKeyExA(dev, path, 0, KEY_READ, &params) != ERROR_SUCCESS) { continue; }
            if (RegQueryValueExA(params, "PortName", NULL, &type, (BYTE *)port, &len) == ERROR_SUCCESS &&
                type == REG_SZ && _strnicmp(port, "COM", 3) == 0) {
                /* 登錄檔會留著拔掉的裝置；開得起來才算數 */
                char dev_path[48];
                snprintf(dev_path, sizeof(dev_path), "\\\\.\\%s", port);
                HANDLE h = CreateFileA(dev_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                    snprintf(out, cap, "%s", port);
                    found = true;
                }
            }
            RegCloseKey(params);
        }
        RegCloseKey(dev);
    }
    RegCloseKey(usb);
    return found;
}

bool serial_open_auto(const char *force_port)
{
    char port[32], dev_path[48];
    DCB  dcb;
    COMMTIMEOUTS to;

    if (s_port != INVALID_HANDLE_VALUE) { return true; }
    if (force_port && *force_port) { snprintf(port, sizeof(port), "%s", force_port); }
    else if (!find_stlink_port(port, sizeof(port))) { return false; }

    snprintf(dev_path, sizeof(dev_path), "\\\\.\\%s", port);
    s_port = CreateFileA(dev_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (s_port == INVALID_HANDLE_VALUE) { return false; }

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    GetCommState(s_port, &dcb);
    dcb.BaudRate = 115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = dcb.fInX = FALSE;
    if (!SetCommState(s_port, &dcb)) { serial_close(); return false; }

    memset(&to, 0, sizeof(to));
    to.WriteTotalTimeoutConstant = 50;     /* 板子拔掉時不要卡住 */
    to.ReadIntervalTimeout = MAXDWORD;
    SetCommTimeouts(s_port, &to);
    snprintf(s_name, sizeof(s_name), "%s", port);
    logf_("序列埠 %s 已開啟", s_name);
    return true;
}

bool serial_write(const char *s, int n)
{
    DWORD done = 0;

    if (s_port == INVALID_HANDLE_VALUE) { return false; }
    if (!WriteFile(s_port, s, (DWORD)n, &done, NULL) || done != (DWORD)n) {
        logf_("序列埠 %s 寫入失敗，重新連線", s_name);
        serial_close();
        return false;
    }
    return true;
}

const char *serial_port_name(void)
{
    return s_port != INVALID_HANDLE_VALUE ? s_name : NULL;
}

void serial_close(void)
{
    if (s_port != INVALID_HANDLE_VALUE) { CloseHandle(s_port); }
    s_port = INVALID_HANDLE_VALUE;
}

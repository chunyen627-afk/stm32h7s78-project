/* 2.8 吋 ESP32 儀表（ESP32-2432S028「CYD」）跑原作 esp32-gt7-dashboard 韌體，不改韌體。
 *
 * 原作的 USB 模式用的是 SimHub 的 Arduino 協定，所以這裡照那個協定講話
 * （協定出自原作的 src/ArqSerial.h、FlowSerialRead.h、SHCommands.h、main.cpp）：
 *   - 主機送出的資料一律包成 ARQ 封包：01 01 編號 長度 資料(1~32) CRC8（CRC 算編號、長度、資料）
 *     板子回 03 編號 = 收到；04 上一個好的編號 原因 = 要重送。編號 0~127 循環
 *   - 資料流裡每個指令是 03 + 指令字元：'1' 打招呼（後面帶 1 位元組，板子回版本：08 'j'）、
 *     '8' 換速度（後面帶速度代號，板子 200ms 後換）、'P' 儀表資料（後面是整行 DSH1 + 換行）
 *   - 板子回的資料：06 長度 字串 20（字串）、08 位元組（單一位元組）
 *   - 'P' 那一行板子收完、解析完會回 08 15。不等它：封包確認就是流量控制（板子讀完手上的
 *     資料才收下一包），永遠送最新的一行，板子跑多快就送多快，不會越積越多
 *   - 開機時是 19200 baud，打完招呼換到 1Mbaud（不行就 230400）
 *
 * 獨立一條執行緒：等確認的時間不會拖慢 H7 那條。 */
#include "bridge.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

static const uint8_t k_crc8[256] = {
    0,213,127,170,254,43,129,84,41,252,86,131,215,2,168,125,82,135,45,248,172,121,211,6,123,174,4,209,133,80,250,47,
    164,113,219,14,90,143,37,240,141,88,242,39,115,166,12,217,246,35,137,92,8,221,119,162,223,10,160,117,33,244,94,139,
    157,72,226,55,99,182,28,201,180,97,203,30,74,159,53,224,207,26,176,101,49,228,78,155,230,51,153,76,24,205,103,178,
    57,236,70,147,199,18,184,109,16,197,111,186,238,59,145,68,107,190,20,193,149,64,234,63,66,151,61,232,188,105,195,22,
    239,58,144,69,17,196,110,187,198,19,185,108,56,237,71,146,189,104,194,23,67,150,60,233,148,65,235,62,106,191,21,192,
    75,158,52,225,181,96,202,31,98,183,29,200,156,73,227,54,25,204,102,179,231,50,152,77,48,229,79,154,206,27,177,100,
    114,167,13,216,140,89,243,38,91,142,36,241,165,112,218,15,32,245,95,138,222,11,161,116,9,220,118,163,247,34,136,93,
    214,3,169,124,40,253,87,130,255,42,128,85,1,212,126,171,132,81,251,46,122,175,5,208,173,120,210,7,83,134,44,249,
};

/* ---- 主執行緒丟進來的最新一行 ---- */
static CRITICAL_SECTION s_lock;
static char   s_line[256];
static int    s_line_len;
static bool   s_line_new;
static HANDLE s_event;

/* ---- 狀態（給狀態列看） ---- */
static volatile LONG s_sent;          /* 送成功的行數 */
static char          s_port_name[32];
static volatile bool s_connected;

/* ---- 序列埠 ---- */
static HANDLE  s_h = INVALID_HANDLE_VALUE;
static uint8_t s_next_id;               /* 下一個封包編號 */
static int     s_last_good = 255;       /* 板子照順序收下的最後一個編號（它下一個只收這個 +1） */

static bool find_cyd_port(char *out, size_t cap)
{
    static const char *const k_ids[] = { "VID_1A86&PID_7523", "VID_1A86&PID_55D4", "VID_10C4&PID_EA60" };
    HKEY usb;
    bool found = false;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ, &usb) != ERROR_SUCCESS) {
        return false;
    }
    for (int k = 0; k < 3 && !found; k++) {
        HKEY dev;
        if (RegOpenKeyExA(usb, k_ids[k], 0, KEY_READ, &dev) != ERROR_SUCCESS) { continue; }
        char serial[128];
        for (DWORD j = 0; !found; j++) {
            DWORD m = sizeof(serial);
            if (RegEnumKeyExA(dev, j, serial, &m, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) { break; }
            char path[256], port[32];
            DWORD len = sizeof(port), type;
            HKEY params;
            snprintf(path, sizeof(path), "%s\\Device Parameters", serial);
            if (RegOpenKeyExA(dev, path, 0, KEY_READ, &params) != ERROR_SUCCESS) { continue; }
            if (RegQueryValueExA(params, "PortName", NULL, &type, (BYTE *)port, &len) == ERROR_SUCCESS &&
                type == REG_SZ && _strnicmp(port, "COM", 3) == 0) {
                char dp[48];
                snprintf(dp, sizeof(dp), "\\\\.\\%s", port);
                HANDLE h = CreateFileA(dp, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
                if (h != INVALID_HANDLE_VALUE) {      /* 登錄檔會留著拔掉的裝置：開得起來才算 */
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

static bool port_baud(DWORD baud)
{
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    GetCommState(s_h, &dcb);
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    /* DTR / RTS 接到 ESP32 的 IO0 / EN：不要動，不然會讓它重開機或進燒錄模式 */
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = dcb.fInX = FALSE;
    return SetCommState(s_h, &dcb) != 0;
}

static void port_close(void)
{
    if (s_h != INVALID_HANDLE_VALUE) { CloseHandle(s_h); }
    s_h = INVALID_HANDLE_VALUE;
    s_connected = false;
}

static bool port_open(const char *port)
{
    char dp[48];
    COMMTIMEOUTS to;

    snprintf(dp, sizeof(dp), "\\\\.\\%s", port);
    s_h = CreateFileA(dp, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (s_h == INVALID_HANDLE_VALUE) { return false; }
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout = MAXDWORD;      /* 讀：有多少拿多少，不等 */
    to.WriteTotalTimeoutConstant = 100;
    SetCommTimeouts(s_h, &to);
    SetupComm(s_h, 4096, 4096);
    return port_baud(19200);
}

/* 讓 ESP32 重開機（EN 經 RTS 拉低再放開） */
static void esp_reset(void)
{
    EscapeCommFunction(s_h, CLRDTR);
    EscapeCommFunction(s_h, SETRTS);
    Sleep(120);
    EscapeCommFunction(s_h, CLRRTS);
}

static int port_read(uint8_t *buf, int cap)
{
    DWORD got = 0;
    if (!ReadFile(s_h, buf, (DWORD)cap, &got, NULL)) { return -1; }
    return (int)got;
}

static bool port_write(const uint8_t *buf, int n)
{
    DWORD done = 0;
    return WriteFile(s_h, buf, (DWORD)n, &done, NULL) && done == (DWORD)n;
}

/* ---- ARQ ---- */
/* 回應解析：03 編號（收到）、04 編號 原因（重送）、06 長度 字串 20、08 位元組 */
static struct {
    int     stage, need;
    uint8_t kind, buf[300];
    int     len;
} s_rx;
static uint8_t s_ackq[64];          /* 收到的確認編號（依到達順序） */
static int     s_ack_head, s_ack_tail;
static bool s_nack;
static int  s_last_byte = -1;       /* 最近一個 08 回應的位元組 */
static char s_last_str[64];

static void rx_feed(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t c = p[i];
        switch (s_rx.stage) {
        case 0:
            if (c == 0x03 || c == 0x04 || c == 0x06 || c == 0x08) { s_rx.kind = c; s_rx.stage = 1; s_rx.len = 0; }
            break;
        case 1:
            if (s_rx.kind == 0x03) { s_ackq[s_ack_tail++ & 63] = c; s_rx.stage = 0; }
            else if (s_rx.kind == 0x04) { s_nack = true; s_rx.stage = 2; }      /* 再吃一個原因 */
            else if (s_rx.kind == 0x08) { s_last_byte = c; s_rx.stage = 0; }
            else { s_rx.need = c; s_rx.stage = s_rx.need ? 3 : 4; }            /* 06：長度 */
            break;
        case 2:
            s_rx.stage = 0;
            break;
        case 3:
            if (s_rx.len < (int)sizeof(s_rx.buf)) { s_rx.buf[s_rx.len++] = c; }
            if (s_rx.len >= s_rx.need) { s_rx.stage = 4; }
            break;
        default:                                                                  /* 06 結尾的 0x20 */
            s_rx.buf[s_rx.len < (int)sizeof(s_rx.buf) ? s_rx.len : (int)sizeof(s_rx.buf) - 1] = 0;
            snprintf(s_last_str, sizeof(s_last_str), "%s", (const char *)s_rx.buf);
            s_rx.stage = 0;
            break;
        }
    }
}

static void rx_pump(void)
{
    uint8_t b[512];
    int n;
    while ((n = port_read(b, sizeof(b))) > 0) { rx_feed(b, n); }
}

static int build_packet(uint8_t *out, uint8_t id, const uint8_t *data, int len)
{
    uint8_t crc = k_crc8[0 ^ id];
    crc = k_crc8[crc ^ (uint8_t)len];
    out[0] = 1; out[1] = 1; out[2] = id; out[3] = (uint8_t)len;
    for (int i = 0; i < len; i++) { out[4 + i] = data[i]; crc = k_crc8[crc ^ data[i]]; }
    out[4 + len] = crc;
    return 5 + len;
}

/* 把一串資料切成封包送出，全部被確認才回 true（中途要重送就從那一包重來） */
static bool arq_send(const uint8_t *data, int len, DWORD timeout_ms)
{
    int     npk = (len + 31) / 32;
    uint8_t ids[16];
    DWORD   t0 = GetTickCount();
    int     done = 0;                       /* 已確認的封包數 */

    if (npk > 16) { return false; }
    while (done < npk) {
        /* 從 done 開始全部送出（一次送多包：板子的接收緩衝放得下） */
        uint8_t frame[64 * 16];
        int     fl = 0;
        for (int k = done; k < npk; k++) {
            int off = k * 32, n = len - off < 32 ? len - off : 32;
            ids[k] = s_next_id;
            s_next_id = s_next_id >= 127 ? 0 : s_next_id + 1;
            fl += build_packet(frame + fl, ids[k], data + off, n);
        }
        s_nack = false;
        if (!port_write(frame, fl)) { return false; }
        /* 等確認：只照順序往前推。板子對順序不對的封包也會回確認、但會丟掉，
         * 所以看到後面的編號被確認不能跳過去 */
        for (;;) {
            rx_pump();
            while (s_ack_head != s_ack_tail) {
                uint8_t a = s_ackq[s_ack_head++ & 63];
                if (done < npk && a == ids[done]) { s_last_good = ids[done]; done++; }
            }
            if (done >= npk) { return true; }
            if (s_nack) { break; }                                  /* 從 done 重送 */
            if (GetTickCount() - t0 > timeout_ms) { return false; }
            Sleep(0);
        }
        /* 重送：編號接著板子照順序收下的最後一個 */
        s_next_id = (uint8_t)(s_last_good > 127 ? 0 : s_last_good + 1);
        if (GetTickCount() - t0 > timeout_ms) { return false; }
    }
    return true;
}

/* 打招呼：03 '1' 00，板子回版本（一個字元，08 'j'） */
static bool hello(DWORD timeout_ms)
{
    static const uint8_t cmd[3] = { 0x03, '1', 0x00 };
    s_last_byte = -1;
    if (!arq_send(cmd, 3, timeout_ms)) { return false; }
    DWORD t0 = GetTickCount();
    while (s_last_byte < 0 && GetTickCount() - t0 < timeout_ms) { rx_pump(); Sleep(2); }
    return s_last_byte >= 0;
}

static bool connect_cyd(const char *port)
{
    /* 板子可能還停在上次換好的速度（Bridge 重開、ESP32 沒重開），三種都試；
     * 都沒回應就讓它重開機回到 19200 */
    static const DWORD k_try[3] = { 19200, 1000000, 230400 };
    DWORD at = 0;

    if (!port_open(port)) { return false; }
    memset(&s_rx, 0, sizeof(s_rx));
    s_ack_head = s_ack_tail = 0;
    for (int round = 0; round < 2 && !at; round++) {
        for (int k = 0; k < 3 && !at; k++) {
            port_baud(k_try[k]);
            Sleep(30);
            PurgeComm(s_h, PURGE_RXCLEAR);
            s_next_id = 255;                    /* 255 板子什麼時候都收，之後從 0 開始 */
            s_last_good = 255;
            if (hello(300) || hello(300)) { at = k_try[k]; }
        }
        if (!at && round == 0) {
            esp_reset();
            Sleep(3000);                        /* 開機 */
        }
    }
    if (!at) { port_close(); return false; }
    char ver[8];
    snprintf(ver, sizeof(ver), "%c", s_last_byte);

    if (at == 19200) {
        /* 換速度：先 1Mbaud（代號 14），確認不了就讓它重開機、改 230400（代號 12） */
        static const struct { uint8_t code; DWORD baud; } k_speed[2] = { { 14, 1000000 }, { 12, 230400 } };
        at = 0;
        for (int k = 0; k < 2 && !at; k++) {
            uint8_t cmd[3] = { 0x03, '8', k_speed[k].code };
            if (!arq_send(cmd, 3, 500)) { break; }
            Sleep(300);                         /* 板子收到後等 200ms 才換 */
            port_baud(k_speed[k].baud);
            PurgeComm(s_h, PURGE_RXCLEAR);
            if (hello(500) || hello(500)) { at = k_speed[k].baud; break; }
            /* 這個速度不通：重開機回到 19200 再試下一個 */
            esp_reset();
            port_baud(19200);
            Sleep(3000);
            s_next_id = 255;
            s_last_good = 255;
            if (!(hello(500) || hello(500))) { break; }
        }
        if (!at) { port_close(); return false; }
    }
    snprintf(s_port_name, sizeof(s_port_name), "%s", port);
    logf_("2.8 吋 ESP32 已連上 %s（原作韌體 %s，%lu baud）", port, ver, (unsigned long)at);
    s_connected = true;
    return true;
}

static DWORD WINAPI cyd_thread(LPVOID arg)
{
    (void)arg;
    char port[32];
    for (;;) {
        if (!s_connected) {
            if (!find_cyd_port(port, sizeof(port)) || !connect_cyd(port)) { Sleep(3000); continue; }
        }
        WaitForSingleObject(s_event, 200);
        uint8_t data[260];
        int     n = 0;
        EnterCriticalSection(&s_lock);
        if (s_line_new) {
            data[0] = 0x03; data[1] = 'P';
            memcpy(data + 2, s_line, s_line_len);       /* 已含結尾換行 */
            n = 2 + s_line_len;
            s_line_new = false;
        }
        LeaveCriticalSection(&s_lock);
        if (!n) { rx_pump(); continue; }

        s_last_byte = -1;
        if (!arq_send(data, n, 500)) {
            logf_("2.8 吋 ESP32（%s）沒有回應，重新連線", s_port_name);
            port_close();
            continue;
        }
        /* 不等板子回「這行解析完」（08 15）：每個封包的確認本身就是流量控制 ——
         * 板子要把手上的資料讀完才收下一包。原作收一行要跑兩圈主迴圈（先讀 'P'、
         * 下一圈才讀內容），所以接著送，讓它讀完上一行的同一圈就能接著讀下一行 */
        InterlockedIncrement(&s_sent);
    }
    return 0;
}

void cyd_start(void)
{
    InitializeCriticalSection(&s_lock);
    s_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, cyd_thread, NULL, 0, NULL);
}

/* 主執行緒每送一行給 H7 就丟一份過來（只留最新的） */
void cyd_offer(const char *line, int n)
{
    if (!s_event || n <= 0 || n >= (int)sizeof(s_line)) { return; }
    EnterCriticalSection(&s_lock);
    memcpy(s_line, line, n);
    s_line_len = n;
    s_line_new = true;
    LeaveCriticalSection(&s_lock);
    SetEvent(s_event);
}

/* 狀態：沒連上回 NULL；有的話回 COM 名稱，並把上次查詢後送出的行數放進 *sent */
const char *cyd_status(long *sent)
{
    *sent = InterlockedExchange(&s_sent, 0);
    return s_connected ? s_port_name : NULL;
}

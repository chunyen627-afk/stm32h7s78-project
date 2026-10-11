/* Gran Turismo 7（PS5）：官方遙測（UDP，Salsa20 加密）。電腦當中繼：PS5 → Bridge → 板子。
 *
 * 協定（跟原作 orig/GT7UDPParser.cpp 一樣）：
 *   - 電腦從 UDP 33740 埠對 PS5 的 33739 埠送一個字元的心跳（'C' / '~' / 'B' / 'A'，
 *     決定封包版本），每 0.5 秒一次；PS5 就每秒 60 次送封包回電腦的 33740 埠
 *   - 封包用 Salsa20 加密：金鑰是 "Simulator Interface Packet GT7 ver 0.0" 的前 32 位元組；
 *     偏移 0x40 的 4 位元組是 iv1，iv2 = iv1 ^ 常數（看版本），nonce = iv2 ‖ iv1
 *   - 解出來偏移 0 是 0x47375330（"0S7G"）才算數
 *   - 長度：A 296、B 316、~ 344、C 368；C 版多了「本圈時間」
 *
 * PS5 的 IP：設定檔 racedash-bridge.ini 的 ps5=（找到後自動寫入）；沒有的話每 5 秒對本機
 * 區網（對外連線走的那張網卡）的 /24 網段逐一送心跳，只有在跑 GT7 的 PS5 會回應，收到就記住它。
 * 心跳版本先試 C，3 秒沒回應依序退到 ~、B、A。
 *
 * 沒有的資料：ABS；名次只有開賽前有（之後是 -1）。
 * Windows 防火牆要允許 racedash-bridge.exe 接收區網的 UDP（第一次執行時 Windows 會問）。 */
#include "bridge.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define GT7_LOCAL_PORT  33740
#define GT7_REMOTE_PORT 33739
#define GT7_MAGIC       0x47375330u

/* ---------------- Salsa20（8 位元組 nonce、64 位元計數器） ---------------- */
#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void salsa20_block(uint32_t out[16], const uint32_t in[16])
{
    uint32_t x[16];
    memcpy(x, in, sizeof(x));
    for (int i = 0; i < 10; i++) {
        x[4]  ^= ROTL(x[0]  + x[12], 7);  x[8]  ^= ROTL(x[4]  + x[0],  9);
        x[12] ^= ROTL(x[8]  + x[4], 13);  x[0]  ^= ROTL(x[12] + x[8], 18);
        x[9]  ^= ROTL(x[5]  + x[1], 7);   x[13] ^= ROTL(x[9]  + x[5],  9);
        x[1]  ^= ROTL(x[13] + x[9], 13);  x[5]  ^= ROTL(x[1]  + x[13], 18);
        x[14] ^= ROTL(x[10] + x[6], 7);   x[2]  ^= ROTL(x[14] + x[10], 9);
        x[6]  ^= ROTL(x[2]  + x[14], 13); x[10] ^= ROTL(x[6]  + x[2], 18);
        x[3]  ^= ROTL(x[15] + x[11], 7);  x[7]  ^= ROTL(x[3]  + x[15], 9);
        x[11] ^= ROTL(x[7]  + x[3], 13);  x[15] ^= ROTL(x[11] + x[7], 18);
        x[1]  ^= ROTL(x[0]  + x[3], 7);   x[2]  ^= ROTL(x[1]  + x[0],  9);
        x[3]  ^= ROTL(x[2]  + x[1], 13);  x[0]  ^= ROTL(x[3]  + x[2], 18);
        x[6]  ^= ROTL(x[5]  + x[4], 7);   x[7]  ^= ROTL(x[6]  + x[5],  9);
        x[4]  ^= ROTL(x[7]  + x[6], 13);  x[5]  ^= ROTL(x[4]  + x[7], 18);
        x[11] ^= ROTL(x[10] + x[9], 7);   x[8]  ^= ROTL(x[11] + x[10], 9);
        x[9]  ^= ROTL(x[8]  + x[11], 13); x[10] ^= ROTL(x[9]  + x[8], 18);
        x[12] ^= ROTL(x[15] + x[14], 7);  x[13] ^= ROTL(x[12] + x[15], 9);
        x[14] ^= ROTL(x[13] + x[12], 13); x[15] ^= ROTL(x[14] + x[13], 18);
    }
    for (int i = 0; i < 16; i++) { out[i] = x[i] + in[i]; }
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* 32 位元組金鑰、8 位元組 nonce，從計數器 0 開始，把 buf 就地加／解密 */
void gt7_salsa20(const uint8_t key[32], const uint8_t nonce[8], uint8_t *buf, int n)
{
    static const uint8_t sigma[17] = "expand 32-byte k";
    uint32_t in[16], ks[16];
    in[0] = le32(sigma);      in[5] = le32(sigma + 4);   in[10] = le32(sigma + 8);  in[15] = le32(sigma + 12);
    for (int i = 0; i < 4; i++) { in[1 + i] = le32(key + i * 4); in[11 + i] = le32(key + 16 + i * 4); }
    in[6] = le32(nonce);
    in[7] = le32(nonce + 4);
    in[8] = in[9] = 0;
    for (int off = 0; off < n; off += 64) {
        salsa20_block(ks, in);
        for (int i = 0; i < 64 && off + i < n; i++) {
            buf[off + i] ^= (uint8_t)(ks[i / 4] >> (8 * (i % 4)));
        }
        if (++in[8] == 0) { in[9]++; }
    }
}

/* 解密並檢查；回 1 = 有效的 GT7 封包 */
int gt7_decrypt(uint8_t *p, int n)
{
    static const char key_text[] = "Simulator Interface Packet GT7 ver 0.0";
    uint8_t  key[32], nonce[8];
    uint32_t iv1, iv2, x;

    if (n == 296) { x = 0xDEADBEAFu; }
    else if (n == 316 || n == 368) { x = 0xDEADBEEFu; }
    else if (n == 344) { x = 0x55FABB4Fu; }
    else { return 0; }
    memcpy(key, key_text, 32);
    iv1 = le32(p + 0x40);
    iv2 = iv1 ^ x;
    memcpy(nonce, &iv2, 4);
    memcpy(nonce + 4, &iv1, 4);
    gt7_salsa20(key, nonce, p, n);
    return le32(p) == GT7_MAGIC;
}

/* ---------------- 網路 ---------------- */
static SOCKET   s_sock = INVALID_SOCKET;
static uint32_t s_ps5;              /* 鎖定的 PS5 位址（網路位元組序），0 = 還沒找到 */
static int      s_ver;              /* 心跳版本索引 */
static const char k_ver[4] = { 'C', '~', 'B', 'A' };
static DWORD    s_last_hb, s_last_sweep, s_last_rx, s_ver_since;
static char     s_ini[MAX_PATH];

static void ini_path(void)
{
    GetModuleFileNameA(NULL, s_ini, sizeof(s_ini));
    char *slash = strrchr(s_ini, '\\');
    if (slash) { strcpy(slash + 1, "racedash-bridge.ini"); }
}

static void load_ps5(void)
{
    char ip[64] = "";
    ini_path();
    GetPrivateProfileStringA("gt7", "ps5", "", ip, sizeof(ip), s_ini);
    if (ip[0]) {
        struct in_addr a;
        if (inet_pton(AF_INET, ip, &a) == 1) { s_ps5 = a.s_addr; logf_("GT7：設定檔的 PS5 位址 %s", ip); }
    }
}

static void save_ps5(void)
{
    char ip[32];
    inet_ntop(AF_INET, &s_ps5, ip, sizeof(ip));
    WritePrivateProfileStringA("gt7", "ps5", ip, s_ini);
}

static void send_hb(uint32_t addr)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(GT7_REMOTE_PORT);
    a.sin_addr.s_addr = addr;
    sendto(s_sock, &k_ver[s_ver], 1, 0, (struct sockaddr *)&a, sizeof(a));
}

/* 對家裡區網的 /24 網段逐一送心跳（找 PS5）。只掃 Windows 對外連線實際走的那張網卡
 * （GetBestInterface 到公網位址），ZeroTier、VPN、Hyper-V 這類虛擬網卡就算有閘道也不掃，
 * 免得對別人的裝置送封包。 */
static void sweep(void)
{
    ULONG size = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, NULL, NULL, &size);
    IP_ADAPTER_ADDRESSES *list = (IP_ADAPTER_ADDRESSES *)malloc(size ? size : 1);
    DWORD best = 0;
    struct in_addr pub;
    inet_pton(AF_INET, "8.8.8.8", &pub);
    if (!list || GetBestInterface(pub.s_addr, &best) != NO_ERROR) { free(list); return; }
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, NULL, list, &size) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *ad = list; ad; ad = ad->Next) {
            if (ad->OperStatus != IfOperStatusUp || ad->IfIndex != best) { continue; }
            for (IP_ADAPTER_UNICAST_ADDRESS *u = ad->FirstUnicastAddress; u; u = u->Next) {
                uint32_t me = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
                if ((me >> 16) == 0xA9FE) { continue; }            /* 169.254 自動位址 */
                uint32_t net = me & 0xFFFFFF00u;
                static uint32_t logged;
                if (logged != net) {
                    logged = net;
                    logf_("GT7：在 %u.%u.%u.0/24 找 PS5（每 5 秒，找到就記住）",
                          net >> 24, (net >> 16) & 255, (net >> 8) & 255);
                }
                for (uint32_t h = 1; h < 255; h++) {
                    if ((net | h) != me) { send_hb(htonl(net | h)); }
                }
            }
        }
    }
    free(list);
}

static bool gt7_open(void)
{
    struct sockaddr_in a;
    u_long nb = 1;

    if (s_sock != INVALID_SOCKET) { return true; }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) { return false; }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(GT7_LOCAL_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&a, sizeof(a)) != 0) {
        logf_("GT7：UDP %d 埠開不起來", GT7_LOCAL_PORT);
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        return false;
    }
    ioctlsocket(s_sock, FIONBIO, &nb);
    load_ps5();
    s_ver_since = GetTickCount();
    logf_("GT7：監聽 UDP %d 埠", GT7_LOCAL_PORT);
    return true;
}

static void gt7_close(void) {}      /* 一直開著：PS5 隨時可能開 GT7 */

/* ---------------- 資料 ---------------- */
static struct {
    bool     updated;
    int32_t  last_pkt_id, lap_start_pkt, prev_lap;
    tele_t   t;
} s;

static float   f32(const uint8_t *p, int o) { float v; memcpy(&v, p + o, 4); return v; }
static int32_t s32(const uint8_t *p, int o) { int32_t v; memcpy(&v, p + o, 4); return v; }
static int16_t s16(const uint8_t *p, int o) { int16_t v; memcpy(&v, p + o, 2); return v; }

static void parse(const uint8_t *p, int n)
{
    tele_t *t = &s.t;
    int16_t flags = s16(p, 0x8E);
    int32_t pkt = s32(p, 0x70);

    if (pkt == s.last_pkt_id) { return; }
    s.last_pkt_id = pkt;

    tele_clear(t);
    t->running   = (flags & 1) && !(flags & 4);            /* 在賽道上、不在讀取中（暫停也算在跑） */
    t->speed_kmh = f32(p, 0x4C) * 3.6f;
    t->gear      = p[0x90] & 0x0F;                          /* 0 = N（原作也這樣顯示） */
    t->rpm       = f32(p, 0x3C);
    int16_t lo = s16(p, 0x88), hi = s16(p, 0x8A);           /* 轉速警示區間：紅線 ~ 最大 */
    if (hi >= 1000 && hi <= 25000) {
        t->max_rpm = hi;
        if (lo > 0 && lo < hi) { t->redline_pct = lo * 100.0f / hi; }
    }
    t->throttle  = p[0x91] / 255.0f;
    t->brake     = p[0x92] / 255.0f;
    t->tc        = (flags & (1 << 11)) != 0;                /* TCS 介入中 */
    for (int i = 0; i < 4; i++) { t->tyre_c[i] = f32(p, 0x60 + i * 4); }   /* FL FR RL RR */
    float fuel = f32(p, 0x44), cap = f32(p, 0x48);
    if (cap > 0) { t->fuel_l = fuel; t->fuel_max_l = cap; }
    int32_t best = s32(p, 0x78), last = s32(p, 0x7C);
    t->best_ms = best > 0 ? best : 0;
    t->last_ms = last > 0 ? last : 0;
    int16_t lap = s16(p, 0x74), total = s16(p, 0x76);
    if (lap > 0) { t->lap = lap; }
    if (total > 0) { t->total_laps = total; }
    int16_t pos = s16(p, 0x84);                             /* 只有開賽前有，之後是 -1 */
    if (pos > 0) { t->position = pos; }
    /* 本圈時間：C 版封包直接有（0x15C：~ 版 344 位元組之後是 4 位元組路面類型，再來就是它）；
     * 其他版本用封包編號算（每秒 60 包） */
    if (n == 368) {
        t->cur_ms = s32(p, 0x15C);
    } else {
        if (lap != s.prev_lap) { s.lap_start_pkt = pkt; s.prev_lap = lap; }
        t->cur_ms = lap > 0 ? (int32_t)((int64_t)(pkt - s.lap_start_pkt) * 1000 / 60) : 0;
    }
    s.updated = true;
}

static int gt7_poll(tele_t *t)
{
    static uint8_t buf[512];
    struct sockaddr_in from;
    DWORD now = GetTickCount();

    if (s_sock == INVALID_SOCKET && !gt7_open()) { return 0; }

    for (;;) {
        int len = sizeof(from);
        int n = recvfrom(s_sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &len);
        if (n <= 0) { break; }
        if (!gt7_decrypt(buf, n)) { continue; }
        if (s_ps5 != from.sin_addr.s_addr) {
            char ip[32];
            s_ps5 = from.sin_addr.s_addr;
            inet_ntop(AF_INET, &s_ps5, ip, sizeof(ip));
            logf_("GT7：找到 PS5 %s（封包 %d 位元組）", ip, n);
            save_ps5();
        }
        s_last_rx = now;
        parse(buf, n);
    }

    /* 心跳：鎖定的 PS5 每 0.5 秒；還沒收到就每 5 秒掃一次網段 */
    if (s_ps5 && now - s_last_hb >= 500) { s_last_hb = now; send_hb(s_ps5); }
    if (now - s_last_rx > 3000) {
        if (now - s_ver_since > 3000 && s_ps5) {                /* 這個版本 3 秒沒回應：換下一個 */
            s_ver = (s_ver + 1) % 4;
            s_ver_since = now;
        }
        if (now - s_last_sweep >= 5000) { s_last_sweep = now; sweep(); }
    } else {
        s_ver_since = now;
    }

    if (!s.updated) { return 0; }
    s.updated = false;
    *t = s.t;
    return 1;
}

const source_t src_gt7 = { "Gran Turismo 7（PS5）", { NULL }, gt7_open, gt7_poll, gt7_close };

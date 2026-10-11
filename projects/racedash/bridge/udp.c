/* UDP 接收：F1 與 Forza 都用 UDP 送遙測，共用接收埠（20777 與 20888，Forza 的 Data Out
 * 設成 20777）。依封包內容分給各自的解析：
 *   F1 24 / 25：開頭兩個位元組是 2024、2025 或 2026（m_packetFormat，看遊戲的 UDP Format 設定）
 *   Forza：固定長度（311 / 323 / 324 / 331 位元組；232 的 Sled 格式沒有儀表資料） */
#include "bridge.h"

#include <winsock2.h>
#include <stdio.h>

/* 20777 是 F1 的出廠設定；20888 是這台電腦上 F1 25 設定檔裡的埠。
 * 兩個都聽，遊戲設定不用改。 */
static const uint16_t k_ports[] = { 20777, 20888 };
#define N_PORTS (int)(sizeof(k_ports) / sizeof(k_ports[0]))

static SOCKET s_sock[N_PORTS];
static bool   s_started;

bool udp_open(void)
{
    WSADATA wsa;
    bool    any = false;

    if (s_started) { return true; }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { return false; }
    s_started = true;
    for (int i = 0; i < N_PORTS; i++) {
        struct sockaddr_in a;
        u_long nb = 1;
        int    rcvbuf = 1 << 20;

        s_sock[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_sock[i] == INVALID_SOCKET) { continue; }
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(k_ports[i]);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s_sock[i], (struct sockaddr *)&a, sizeof(a)) != 0) {
            logf_("UDP %d 埠開不起來（被別的程式佔用？）", k_ports[i]);
            closesocket(s_sock[i]);
            s_sock[i] = INVALID_SOCKET;
            continue;
        }
        ioctlsocket(s_sock[i], FIONBIO, &nb);
        setsockopt(s_sock[i], SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
        logf_("UDP 監聽 %d 埠（F1、Forza）", k_ports[i]);
        any = true;
    }
    return any;
}

int udp_recv(uint8_t *buf, int cap)
{
    for (int i = 0; i < N_PORTS; i++) {
        if (s_sock[i] == INVALID_SOCKET || s_sock[i] == 0) { continue; }
        int n = recv(s_sock[i], (char *)buf, cap, 0);
        if (n > 0) { return n; }
    }
    return 0;
}

/* 把積著的封包全部讀出來分給 F1 / Forza 的解析。兩個來源的 poll 都會呼叫這個。 */
void udp_pump(void)
{
    static uint8_t buf[2048];
    int n;

    while ((n = udp_recv(buf, sizeof(buf))) > 0) {
        int fmt = n >= 2 ? buf[0] | buf[1] << 8 : 0;
        if (n >= 29 && fmt >= 2024 && fmt <= 2026) { f1_feed(buf, n); }
        else if (n == 311 || n == 323 || n == 324 || n == 331) { forza_feed(buf, n); }
    }
}

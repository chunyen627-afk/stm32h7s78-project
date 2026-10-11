/* UDP 接收：F1 與 Forza 都用 UDP 送遙測，共用一個埠（預設 20777，F1 的出廠設定；
 * Forza 的 Data Out 也設成 20777）。依封包內容分給各自的解析：
 *   F1 25：開頭兩個位元組是 2025 或 2026（m_packetFormat，看遊戲的 UDP Format 設定）
 *   Forza：固定長度（311 / 323 / 324 / 331 位元組；232 的 Sled 格式沒有儀表資料） */
#include "bridge.h"

#include <winsock2.h>
#include <stdio.h>

#define UDP_PORT 20777

static SOCKET s_sock = INVALID_SOCKET;

bool udp_open(void)
{
    WSADATA wsa;
    struct sockaddr_in a;
    u_long nb = 1;
    int    rcvbuf = 1 << 20;

    if (s_sock != INVALID_SOCKET) { return true; }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { return false; }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) { return false; }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(UDP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&a, sizeof(a)) != 0) {
        logf_("UDP %d 埠開不起來（被別的程式佔用？例如 SimHub）", UDP_PORT);
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        return false;
    }
    ioctlsocket(s_sock, FIONBIO, &nb);
    setsockopt(s_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
    logf_("UDP 監聽 %d 埠（F1、Forza）", UDP_PORT);
    return true;
}

int udp_recv(uint8_t *buf, int cap)
{
    if (s_sock == INVALID_SOCKET) { return 0; }
    int n = recv(s_sock, (char *)buf, cap, 0);
    return n > 0 ? n : 0;
}

/* 把積著的封包全部讀出來分給 F1 / Forza 的解析。兩個來源的 poll 都會呼叫這個。 */
void udp_pump(void)
{
    static uint8_t buf[2048];
    int n;

    while ((n = udp_recv(buf, sizeof(buf))) > 0) {
        int fmt = n >= 2 ? buf[0] | buf[1] << 8 : 0;
        if (n >= 29 && (fmt == 2025 || fmt == 2026)) { f1_feed(buf, n); }
        else if (n == 311 || n == 323 || n == 324 || n == 331) { forza_feed(buf, n); }
    }
}

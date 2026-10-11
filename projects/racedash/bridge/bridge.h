/* RaceDash Bridge：讀賽車遊戲的官方遙測，換成板子吃的 DSH1 格式，每秒 60 次從
 * ST-LINK 的虛擬 COM 埠送出。取代 SimHub（免費版只有每秒 10 次）。
 *
 * 每套遊戲一個「來源」（src_*.c），把自己的資料填進共用的 tele_t；
 * 這邊負責偵測現在開的是哪套、算油耗、組字串、控制送出的節奏。 */
#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stddef.h>

#define TELE_UNKNOWN_I  (-999999)   /* 整數欄位：遊戲沒提供 */

typedef struct {
    bool    running;        /* 正在開車（不是選單、暫停、重播） */
    float   speed_kmh;
    int     gear;           /* -1 倒檔、0 空檔、1~9；TELE_UNKNOWN_I = 不知道 */
    float   rpm, max_rpm;
    float   throttle, brake;            /* 0..1 */
    int32_t cur_ms, last_ms, best_ms;   /* 圈速毫秒；0 = 還沒有；TELE_UNKNOWN_I = 不知道 */
    int     lap;            /* 現在第幾圈（從 1 開始） */
    int     total_laps;     /* 這場共幾圈；0 / TELE_UNKNOWN_I = 不限 */
    int     position;
    float   fuel_l, fuel_max_l;         /* 公升；NAN = 不知道 */
    float   fuel_laps;      /* 遊戲自己算的「還能跑幾圈」；NAN = 由 bridge 依油耗算 */
    float   fuel_pct;       /* 只有百分比的遊戲（Forza）；NAN = 由公升算 */
    int     tc, abs;        /* 正在介入 1 / 沒有 0 / TELE_UNKNOWN_I */
    int     invalid;        /* 這圈被判無效 */
    float   tyre_c[4];      /* 胎溫 °C：左前、右前、左後、右後；NAN = 不知道 */
} tele_t;

void tele_clear(tele_t *t);

/* 一套遊戲。行程名稱用來偵測「現在開的是哪套」；UDP 型的另外看有沒有封包進來。 */
typedef struct source {
    const char *name;               /* 狀態列顯示的名稱 */
    const char *exe[6];             /* 行程名稱（小寫比對），NULL 結尾 */
    bool (*open)(void);             /* 開始讀（共享記憶體要等遊戲建好才開得到） */
    /* 讀最新一份資料。回 1 = 有新資料、0 = 沒變、-1 = 斷了（遊戲關了） */
    int  (*poll)(tele_t *t);
    void (*close)(void);
} source_t;

extern const source_t src_acevo, src_acc, src_acrally, src_f1, src_forza;

/* UDP（F1、Forza 共用一個接收埠，依封包內容分給各自的解析） */
bool udp_open(void);
/* 取一個封包；沒有回 0 */
int  udp_recv(uint8_t *buf, int cap);
/* 把積著的封包全部讀出來，分給 f1_feed / forza_feed */
void udp_pump(void);
void f1_feed(const uint8_t *p, int n);
void forza_feed(const uint8_t *p, int n);

/* 序列埠 */
bool serial_open_auto(const char *force_port);
bool serial_write(const char *s, int n);
const char *serial_port_name(void);
void serial_close(void);

/* 組一行 DSH1（含換行），回長度 */
int  dsh1_format(char *out, int cap, uint32_t seq, bool game_running, const tele_t *t,
                 float fuel_laps_est);

/* 簡單的紀錄（主控台／檔案） */
void logf_(const char *fmt, ...);

#endif

/* RaceDash Bridge 主程式：偵測遊戲、讀遙測、每秒 60 次送 DSH1 給板子。
 *
 *   racedash-bridge.exe                 自動找 ST-LINK 的 COM 埠、自動偵測遊戲
 *   racedash-bridge.exe --com COM3      指定 COM 埠
 *   racedash-bridge.exe --test          不讀遊戲，送模擬資料（驗證每秒 60 筆用）
 *   racedash-bridge.exe --demo          展示模式：送模擬跑圈（也可以從系統匣右鍵開關）
 *   racedash-bridge.exe --console       開主控台顯示紀錄（平常在背景跑，只有系統匣圖示）
 *
 * 送出的節奏：遊戲有新資料時每秒 60 次；遊戲開著但沒在跑（選單、暫停）每秒 10 次；
 * 沒有遊戲每秒 2 次（「遊戲在跑」欄位 = 0，板子顯示等待畫面）。 */
#include "bridge.h"

#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const source_t *const k_sources[] = {
    &src_acevo, &src_acc, &src_acrally, &src_f1, &src_forza,
};
#define N_SOURCES (int)(sizeof(k_sources) / sizeof(k_sources[0]))

static FILE *s_log;
static bool  s_console;

void tray_init(const char *log_path_utf8);
void tray_status(const char *utf8);
bool tray_pump(void);
bool tray_demo(int on);
int  demo_line(char *out, int cap, uint32_t now);
void tray_close(void);
void cyd_start(void);
void cyd_offer(const char *line, int n);
const char *cyd_status(long *sent);

void logf_(const char *fmt, ...)
{
    char       msg[512];
    SYSTEMTIME st;
    va_list    ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    GetLocalTime(&st);
    if (s_console) {
        printf("[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
        fflush(stdout);
    }
    if (s_log) {
        fprintf(s_log, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n", st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, msg);
        fflush(s_log);
    }
}

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* 目前在跑的遊戲（看行程名稱）；沒有回 NULL */
static const source_t *detect_by_process(void)
{
    PROCESSENTRY32W pe;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    const source_t *hit = NULL;

    if (snap == INVALID_HANDLE_VALUE) { return NULL; }
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !hit; ok = Process32NextW(snap, &pe)) {
        char exe[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, exe, sizeof(exe), NULL, NULL);
        for (char *p = exe; *p; p++) { if (*p >= 'A' && *p <= 'Z') { *p += 32; } }
        for (int i = 0; i < N_SOURCES && !hit; i++) {
            for (int k = 0; k < 6 && k_sources[i]->exe[k]; k++) {
                if (strcmp(exe, k_sources[i]->exe[k]) == 0) { hit = k_sources[i]; break; }
            }
        }
    }
    CloseHandle(snap);
    return hit;
}

/* ---- 油耗：每圈開始記下油量，換圈時算這圈用了多少，取最近三圈平均 ---- */
static struct { int lap; float start_l, per_lap[3]; int n; } s_fuel;

static float fuel_laps_estimate(const tele_t *t)
{
    if (isnan(t->fuel_l) || t->lap == TELE_UNKNOWN_I) { return NAN; }
    if (t->lap != s_fuel.lap) {
        float used = s_fuel.start_l - t->fuel_l;
        if (s_fuel.lap > 0 && t->lap == s_fuel.lap + 1 && used > 0.05f && used < 50.0f) {
            s_fuel.per_lap[s_fuel.n % 3] = used;
            s_fuel.n++;
        }
        s_fuel.lap = t->lap;
        s_fuel.start_l = t->fuel_l;
    }
    if (t->fuel_l > s_fuel.start_l + 1.0f) { s_fuel.start_l = t->fuel_l; }   /* 加過油 */
    if (s_fuel.n == 0) { return NAN; }
    float sum = 0;
    int   k = s_fuel.n < 3 ? s_fuel.n : 3;
    for (int i = 0; i < k; i++) { sum += s_fuel.per_lap[i]; }
    return t->fuel_l / (sum / k);
}

/* ---- --test：模擬資料，只用來驗證序列埠每秒 60 筆 ---- */
static void test_fill(tele_t *t, double ms)
{
    double ph = fmod(ms / 1000.0, 10.0);
    tele_clear(t);
    t->running = true;
    t->rpm = (float)(3000 + 5000 * (ph / 10.0));
    t->max_rpm = 8500;
    t->gear = 1 + (int)(ph / 2.0);
    t->speed_kmh = (float)(40 + ph * 20);
    t->throttle = 1.0f;
    t->brake = 0.0f;
    t->cur_ms = (int32_t)fmod(ms, 90000.0);
    t->lap = 1;
    t->position = 1;
}

int main(int argc, char **argv)
{
    const char     *force_port = NULL;
    bool            test = false;
    const source_t *src = NULL;
    bool            src_open = false;
    tele_t          t;
    double          last_detect = -1e9, last_send = 0, last_data = -1e9, last_status = 0, next_open = 0;
    uint32_t        seq = 0, sent = 0;

    char log_path[MAX_PATH];

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--com") && i + 1 < argc) { force_port = argv[++i]; }
        else if (!strcmp(argv[i], "--test")) { test = true; }
        else if (!strcmp(argv[i], "--console")) { s_console = true; }
        else if (!strcmp(argv[i], "--demo")) { tray_demo(1); }
    }
    if (s_console) {
        /* 從命令列啟動就印在那個視窗，不然開一個新的 */
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) { AllocConsole(); }
        freopen("CONOUT$", "w", stdout);
        SetConsoleOutputCP(CP_UTF8);
    }
    /* 同時只跑一個（開機自動啟動之後，手動再開一次也不會搶 COM 埠） */
    CreateMutexA(NULL, TRUE, "Local\\RaceDashBridge");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { return 0; }

    GetModuleFileNameA(NULL, log_path, sizeof(log_path));
    {
        char *slash = strrchr(log_path, '\\');
        if (slash) { strcpy(slash + 1, "racedash-bridge.log"); }
        s_log = fopen(log_path, "a");
    }
    tray_init(log_path);
    timeBeginPeriod(1);
    udp_open();
    cyd_start();                            /* 2.8 吋 ESP32（原作韌體）：自己的執行緒 */
    tele_clear(&t);
    logf_("RaceDash Bridge 啟動%s", test ? "（模擬資料）" : "");

    for (;;) {
        double now = now_ms();

        if (!serial_port_name() && now >= next_open) {
            if (!serial_open_auto(force_port)) { next_open = now + 2000; }
        }

        /* 每秒看一次在跑的遊戲；UDP 型（F1、Forza）沒有行程也可能有封包（例如 PS5） */
        if (!test && now - last_detect >= 1000) {
            const source_t *s = detect_by_process();
            last_detect = now;
            /* 靠 UDP 封包認出來的（例如 PS5 上的 F1，電腦上沒有遊戲行程）：封包還在來就維持 */
            bool udp_alive = (src == &src_f1 || src == &src_forza || src == &src_gt7) && now - last_data < 3000;
            if (s != src && !(s == NULL && udp_alive)) {
                if (src && src_open) { src->close(); }
                src = s;
                src_open = false;
                memset(&s_fuel, 0, sizeof(s_fuel));
                tele_clear(&t);
                logf_(src ? "偵測到遊戲：%s" : "沒有偵測到遊戲%s", src ? src->name : "");
            }
        }
        if (!test && src && !src_open) { src_open = src->open(); }

        bool fresh = false;
        if (test) {
            test_fill(&t, now);
            fresh = true;
        } else if (src && src_open) {
            int r = src->poll(&t);
            if (r > 0) { last_data = now; }
            if (r < 0) { src->close(); src_open = false; }
        } else {
            /* 沒偵測到行程：看 UDP 有沒有 F1、Forza 的封包，或 PS5 上的 GT7（會自己找 PS5） */
            if (src_f1.poll(&t) > 0) { last_data = now; src = &src_f1; src_open = true; logf_("收到 F1 封包"); }
            else if (src_forza.poll(&t) > 0) { last_data = now; src = &src_forza; src_open = true; logf_("收到 Forza 封包"); }
            else if (src_gt7.poll(&t) > 0) { last_data = now; src = &src_gt7; src_open = true; logf_("收到 GT7 封包"); }
        }
        if (!test) { fresh = now - last_data < 1000; }

        /* 展示模式：不管遊戲，送模擬跑圈，每秒 60 */
        bool demo = tray_demo(-1);
        static bool was_demo;
        if (demo != was_demo) { logf_(demo ? "展示模式開始" : "展示模式結束"); was_demo = demo; }
        if (demo) {
            if (now - last_send >= 1000.0 / 60.0 - 0.5) {
                char line[256];
                int  n = demo_line(line, sizeof(line), (uint32_t)now);
                if (serial_write(line, n)) { sent++; }
                cyd_offer(line, n);
                last_send = (now - last_send < 1000.0 / 30.0) ? last_send + 1000.0 / 60.0 : now;
            }
        }
        /* 送出：在跑每秒 60、開著沒在跑每秒 10、沒遊戲每秒 2 */
        double period = (fresh && t.running) ? 1000.0 / 60.0 : fresh ? 100.0 : 500.0;
        if (!demo && now - last_send >= period - 0.5) {
            char   line[256];
            tele_t none;
            const tele_t *use = &t;
            if (!fresh) { tele_clear(&none); use = &none; }
            int n = dsh1_format(line, sizeof(line), ++seq, fresh && t.running, use, fuel_laps_estimate(use));
            if (serial_write(line, n)) { sent++; }
            cyd_offer(line, n);
            /* 固定節拍：跟著理想時間走，不要因為一次晚了就整體變慢 */
            last_send = (now - last_send < period * 2) ? last_send + period : now;
        }

        if (now - last_status >= 5000) {
            char status[256];
            long        cyd_sent;
            const char *cyd = cyd_status(&cyd_sent);
            char        cyd_txt[48] = "";
            if (cyd) { snprintf(cyd_txt, sizeof(cyd_txt), "｜2.8吋 %s 每秒 %.1f 筆", cyd, cyd_sent * 1000.0 / (now - last_status)); }
            snprintf(status, sizeof(status), "%s｜%s｜每秒 %.1f 筆%s",
                     demo ? "展示模式" : test ? "模擬資料" : src ? src->name : "沒有遊戲",
                     serial_port_name() ? serial_port_name() : "找不到 ST-LINK",
                     sent * 1000.0 / (now - last_status), cyd_txt);
            tray_status(status);
            if (s_console) { logf_("%s", status); }
            sent = 0;
            last_status = now;
        }
        if (tray_pump()) { break; }
        Sleep(1);
    }
    logf_("結束");
    tray_close();
    serial_close();
    return 0;
}

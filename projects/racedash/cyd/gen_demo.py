"""從 bridge/demo.c 產生 2.8 吋韌體用的 RaceDashDemo.h（同一套模擬跑圈）"""
import sys

src_path, out_path = sys.argv[1], sys.argv[2]
s = open(src_path, encoding='utf-8').read()
body = s[s.index('typedef struct {'):]
body = body.replace('int demo_line(char *out, int cap, uint32_t now)',
                    'static int racedash_demo_line(char *out, int cap, uint32_t now)')
body = body.replace('%.1f;%.1f;%.1f;%.1f\\n"', '%.1f;%.1f;%.1f;%.1f"')
assert '\\n"' not in body
hdr = '''#pragma once
// RaceDash 展示模式：設定頁的 DEMO 用，板子自己模擬跑圈產生一行 DSH1（不含換行），
// 跟 H7 板子、RaceDash Bridge 的展示模式是同一套模擬。由 bridge/demo.c 產生，不要手改。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

'''
tail = '''
static void racedash_demo_reset(void) { g_sim.started = false; }
'''
open(out_path, 'w', encoding='utf-8', newline='\r\n').write(hdr + body + tail)

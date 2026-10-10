/* 從 TTF 取出 ASCII 字元的輪廓，產生 hires_fonts.inc（板子上不必帶整個 TTF）。
 *
 * 用法（電腦端）：gen_hires_fonts FreeSans.ttf DejaVuSans.ttf > ../hires_fonts.inc
 * 字型出處見 ../../NOTICE.md。輪廓直接寫成 stb_truetype 的 stbtt_vertex，
 * 板子上用 stbtt_Rasterize 依需要的大小畫成抗鋸齒字形。 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "../stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned char *load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) exit(1);
    fclose(f);
    return b;
}

static void emit(const char *name, const char *path)
{
    stbtt_fontinfo fi;
    unsigned char *ttf = load(path);
    if (!stbtt_InitFont(&fi, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) exit(1);
    int upem = ttf[fi.head + 18] << 8 | ttf[fi.head + 19];

    printf("static const stbtt_vertex hf_%s_v[] = {\n", name);
    int off[95], cnt[95];
    int total = 0;
    for (int c = 0x20; c <= 0x7E; ++c) {
        stbtt_vertex *v;
        int n = stbtt_GetCodepointShape(&fi, c, &v);
        off[c - 0x20] = total; cnt[c - 0x20] = n;
        for (int i = 0; i < n; ++i)
            printf("{%d,%d,%d,%d,%d,%d,%d,0},%s", v[i].x, v[i].y, v[i].cx, v[i].cy,
                   v[i].cx1, v[i].cy1, v[i].type, (i % 4 == 3) ? "\n" : "");
        total += n;
        stbtt_FreeShape(&fi, v);
    }
    printf("{0,0,0,0,0,0,0,0}};\n");
    printf("static const HiGlyph hf_%s_g[95] = {\n", name);
    for (int c = 0x20; c <= 0x7E; ++c) {
        int adv, lsb, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointHMetrics(&fi, c, &adv, &lsb);
        stbtt_GetCodepointBox(&fi, c, &x0, &y0, &x1, &y1);
        printf("{%d,%d,%d,%d,%d,%d,%d}, /* '%c' */\n", off[c - 0x20], cnt[c - 0x20],
               adv, x0, y0, x1, y1, c == 92 ? '/' : c);   /* 反斜線放進註解會接到下一行 */
    }
    printf("};\n");
    printf("static const HiFace hf_%s = { %d, hf_%s_v, hf_%s_g };\n\n", name, upem, name, name);
    fprintf(stderr, "%s: upem %d, %d vertices\n", name, upem, total);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s FreeSans.ttf DejaVuSans.ttf\n", argv[0]); return 1; }
    printf("/* 由 tools/gen_hires_fonts.c 產生，不要手改。\n"
           " * FreeSans：GNU FreeFont 20120503（GPLv3＋字型例外）；DejaVuSans：DejaVu 2.37（Bitstream Vera 授權）。\n"
           " * 只取 0x20–0x7E 的輪廓。 */\n\n");
    emit("freesans", argv[1]);
    emit("dejavu", argv[2]);
    return 0;
}

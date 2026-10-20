"""原作的各主題用函式內 static 的離屏畫布（LGFX_Sprite），建立後就一直佔著記憶體，
換主題也不放。ESP32 沒有 PSRAM，換過幾個主題後就配不到新的畫布：
  Pocket 的檔位／時速框整塊空白（配不到就不畫）、Classic 左邊三個圈速格與
  Endurance 右上的本圈時間改成直接畫在螢幕上（先塗黑再寫字 → 閃）。
改成：畫布畫完時登記起來，換主題（invalidateDashboardRenderer）時全部釋放，新主題要用再配。
用法：python patch_sprites.py <mod_src/src>"""
import sys, os

root = sys.argv[1]


def patch(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, encoding='utf-8-sig').read()
    for old, new in pairs:
        n = s.count(old)
        if n != 1:
            sys.exit(f'{rel}：找到 {n} 處：{old[:60]!r}')
        s = s.replace(old, new)
    open(p, 'w', encoding='utf-8-sig', newline='\r\n').write(s)


patch('SHCustomProtocol.h', [
    ('''	void releaseSpriteAfterThemePreview(LGFX_Sprite &sprite, bool &created)
	{
''', '''	// 畫過的離屏畫布登記在這裡，換主題時一起釋放（ESP32 記憶體放不下全部主題的畫布）
	struct TrackedSprite { LGFX_Sprite *sprite; bool *created; };
	TrackedSprite trackedSprites[32];
	uint8_t trackedSpriteCount = 0;

	void trackThemeSprite(LGFX_Sprite &sprite, bool *created)
	{
		for (uint8_t i = 0; i < trackedSpriteCount; ++i)
			if (trackedSprites[i].sprite == &sprite) return;
		if (trackedSpriteCount < 32) trackedSprites[trackedSpriteCount++] = {&sprite, created};
	}

	void releaseThemeSprites()
	{
		for (uint8_t i = 0; i < trackedSpriteCount; ++i)
		{
			trackedSprites[i].sprite->deleteSprite();
			if (trackedSprites[i].created) *trackedSprites[i].created = false;
		}
	}

	void releaseSpriteAfterThemePreview(LGFX_Sprite &sprite, bool &created)
	{
		trackThemeSprite(sprite, &created);
'''),
    ('''	void invalidateDashboardRenderer()
	{
		tft.fillScreen(TFT_BLACK);''', '''	void invalidateDashboardRenderer()
	{
		releaseThemeSprites();
		tft.fillScreen(TFT_BLACK);'''),
])

patch('dashboard/themes/GT3Theme.inc', [
    ('''				rpmTextSprite.pushSprite(DashboardLayout::CENTER_X - 55, 40);
''', '''				rpmTextSprite.pushSprite(DashboardLayout::CENTER_X - 55, 40);
				trackThemeSprite(rpmTextSprite, &rpmTextSpriteCreated);
'''),
])

patch('dashboard/themes/ClassicTheme.inc', [
    ('''			if (spriteWidth != totalWidth || spriteHeight != cellHeight)
			{
				classicPedalSprite.deleteSprite();''', '''			if (spriteWidth != totalWidth || spriteHeight != cellHeight ||
				classicPedalSprite.getBuffer() == nullptr)
			{
				classicPedalSprite.deleteSprite();'''),
    ('''				classicPedalSprite.pushSprite(x, cellY);
''', '''				classicPedalSprite.pushSprite(x, cellY);
				trackThemeSprite(classicPedalSprite, nullptr);
'''),
])
print('OK')

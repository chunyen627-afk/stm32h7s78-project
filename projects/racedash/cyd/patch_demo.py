"""在原作 SHCustomProtocol.h 加上 RaceDash 展示模式：
  設定主選單多一顆 DEMO；DEMO 中點螢幕叫出 DEMO 選單（上一個／下一個主題、停止 DEMO、返回）。"""
import sys

path = sys.argv[1]
s = open(path, encoding='utf-8-sig').read()   # 換行統一成 \n，寫回時再換成 CRLF


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f'找到 {n} 處（預期 {count}）：{old[:70]!r}')
    s = s.replace(old, new)


# 1. 引入模擬
rep('#include "SimHubProtocol.h"\n', '#include "SimHubProtocol.h"\n#include "RaceDashDemo.h"\n')

# 2. 新的設定畫面與狀態
rep('''		TouchCalibration,
	};
''', '''		TouchCalibration,
		DemoMenu,
	};

	// RaceDash 展示模式：板子自己模擬跑圈，USB 送來的資料先不理
	bool demoActive = false;
	uint32_t demoLastFrame = 0;
''')

# 3. 收到的一行拆成：檢查 + 套用（展示模式也用套用那一半）
rep('''        if (firstRun || telemetry.mode == TelemetryMode::GT7) return true;
        DashboardState next;''', '''        if (demoActive || firstRun || telemetry.mode == TelemetryMode::GT7) return true;
        applySimHubFrame(frame);
        return true;
    }
    void applySimHubFrame(SimHubProtocol::Frame &frame) {
        DashboardState next;''')
rep('''        telemetry.simhub.received = true; telemetry.simhub.time = millis(); telemetry.simhub.running = simHubRunning;
        return true;
    }''', '''        telemetry.simhub.received = true; telemetry.simhub.time = millis(); telemetry.simhub.running = simHubRunning;
    }
    void startDemo() {
        racedash_demo_reset();
        demoActive = true;
        demoLastFrame = millis() - 1000;
    }
    void stopDemo() {
        demoActive = false;
        telemetry.simhub.received = false;   // 等 USB 真的有資料再顯示
        simHubGearFilter.reset();
    }
    // 每秒 60 筆模擬資料，跟 USB 收到的一行走同一條路
    void pollDemo(uint32_t now) {
        if (!demoActive || uint32_t(now - demoLastFrame) < 16) return;
        demoLastFrame = now;
        char line[200];
        racedash_demo_line(line, sizeof(line), now);
        SimHubProtocol::Frame frame;
        if (SimHubProtocol::parse(line, frame)) { applySimHubFrame(frame); simhubDirty = true; }
    }''')

# 4. 更新遙測：展示模式固定用 SimHub 那一路
rep('''        const TelemetrySource selected = firstRun ? TelemetrySource::None : telemetry.update(now);''',
    '''        pollDemo(now);
        const TelemetrySource selected = firstRun ? TelemetrySource::None
            : demoActive ? (telemetry.active = TelemetrySource::SimHub) : telemetry.update(now);''')

# 5. 主選單：四顆按鈕（多一顆 DEMO）
rep('''			drawSettingsButton(30, 55, 260, 48, "SELECT THEME", pressedButton == 0);
			drawSettingsButton(30, 113, 260, 48, "DEVICE SETTINGS", pressedButton == 1);
			drawSettingsButton(30, 171, 260, 48, "BACK", pressedButton == 2);
		}''', '''			drawSettingsButton(30, 50, 260, 40, "SELECT THEME", pressedButton == 0);
			drawSettingsButton(30, 96, 260, 40, "DEVICE SETTINGS", pressedButton == 1);
			drawSettingsButton(30, 142, 260, 40, "DEMO", pressedButton == 2);
			drawSettingsButton(30, 188, 260, 40, "BACK", pressedButton == 3);
		}
		else if (settingsScreen == SettingsScreen::DemoMenu)
		{
			tft.setTextColor(TFT_WHITE, TFT_BLACK);
			tft.drawString("DEMO", X_CENTER, 22, 4);
			tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
			tft.drawString(String("THEME: ") + dashboardThemeName(activeDashboardTheme), X_CENTER, 50, 2);
			drawSettingsButton(20, 66, 135, 54, "< PREV", pressedButton == 0);
			drawSettingsButton(165, 66, 135, 54, "NEXT >", pressedButton == 1);
			drawSettingsButton(20, 130, 280, 46, "STOP DEMO", pressedButton == 2, false, true);
			drawSettingsButton(20, 186, 280, 42, "BACK", pressedButton == 3);
		}''')
rep('''			if (button == 0)
				drawSettingsButton(30, 55, 260, 48, "SELECT THEME", pressed);
			else if (button == 1)
				drawSettingsButton(30, 113, 260, 48, "DEVICE SETTINGS", pressed);
			else if (button == 2)
				drawSettingsButton(30, 171, 260, 48, "BACK", pressed);
		}''', '''			if (button == 0)
				drawSettingsButton(30, 50, 260, 40, "SELECT THEME", pressed);
			else if (button == 1)
				drawSettingsButton(30, 96, 260, 40, "DEVICE SETTINGS", pressed);
			else if (button == 2)
				drawSettingsButton(30, 142, 260, 40, "DEMO", pressed);
			else if (button == 3)
				drawSettingsButton(30, 188, 260, 40, "BACK", pressed);
		}
		else if (settingsScreen == SettingsScreen::DemoMenu)
		{
			if (button == 0)
				drawSettingsButton(20, 66, 135, 54, "< PREV", pressed);
			else if (button == 1)
				drawSettingsButton(165, 66, 135, 54, "NEXT >", pressed);
			else if (button == 2)
				drawSettingsButton(20, 130, 280, 46, "STOP DEMO", pressed, false, true);
			else if (button == 3)
				drawSettingsButton(20, 186, 280, 42, "BACK", pressed);
		}''')
rep('''			if (touchInside(30, 55, 260, 48)) return 0;
			if (touchInside(30, 113, 260, 48)) return 1;
			if (touchInside(30, 171, 260, 48)) return 2;
		}''', '''			if (touchInside(30, 50, 260, 40)) return 0;
			if (touchInside(30, 96, 260, 40)) return 1;
			if (touchInside(30, 142, 260, 40)) return 2;
			if (touchInside(30, 188, 260, 40)) return 3;
		}
		else if (settingsScreen == SettingsScreen::DemoMenu)
		{
			if (touchInside(20, 66, 135, 54)) return 0;
			if (touchInside(165, 66, 135, 54)) return 1;
			if (touchInside(20, 130, 280, 46)) return 2;
			if (touchInside(20, 186, 280, 42)) return 3;
		}''')
rep('''			else if (button == 2)
				closeSettings();
		}
		else if (settingsScreen == SettingsScreen::ThemeSelection)''', '''			else if (button == 2)
			{
				startDemo();
				closeSettings();
			}
			else if (button == 3)
				closeSettings();
		}
		else if (settingsScreen == SettingsScreen::DemoMenu)
		{
			// 換主題後直接回到儀表，看得到新主題；要再換就再點一下螢幕
			if (button == 0 || button == 1)
			{
				const size_t currentIndex = dashboardThemeIndex(activeDashboardTheme);
				const size_t nextIndex = button == 0
					? (currentIndex + DASHBOARD_THEME_COUNT - 1) % DASHBOARD_THEME_COUNT
					: (currentIndex + 1) % DASHBOARD_THEME_COUNT;
				closeSettings();
				selectDashboardTheme(DASHBOARD_THEMES[nextIndex].id, true);
			}
			else if (button == 2)
			{
				stopDemo();
				closeSettings();
			}
			else if (button == 3)
				closeSettings();
		}
		else if (settingsScreen == SettingsScreen::ThemeSelection)''')

# 6. 儀表畫面上點一下：展示中叫出 DEMO 選單，不然是原本的設定
rep('''				showSettingsScreen(SettingsScreen::Main);
				waitForReleaseAfterScreenChange = true;''', '''				showSettingsScreen(demoActive ? SettingsScreen::DemoMenu : SettingsScreen::Main);
				waitForReleaseAfterScreenChange = true;''')

open(path, 'w', encoding='utf-8-sig', newline='\r\n').write(s)
print('OK')

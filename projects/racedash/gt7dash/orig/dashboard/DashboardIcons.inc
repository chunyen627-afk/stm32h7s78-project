	void drawDashboardFuelPumpIcon(int x, int y, int fuelPercent,
		uint16_t fullColor, uint16_t emptyColor, uint16_t background)
	{
		constexpr int height = 24;
		const int darkRows = (constrain(fuelPercent, 0, 100) * height + 50) / 100;
		const int liquidTop = height - darkRows;
		for (int row = 0; row < height; ++row)
		{
			const uint16_t color = row >= liquidTop ? fullColor : emptyColor;
			if (row <= 20) tft.drawFastHLine(x, y + row, 13, color);
			if (row >= 21) tft.drawFastHLine(x - 3, y + row, 19, color);
			if (row >= 1 && row <= 5)
			{
				const int nozzleX = x + 13 + (row - 1);
				tft.drawFastHLine(nozzleX + (row == 1 ? 1 : 0), y + row,
					row == 1 ? 2 : 3, color);
			}
			if (row >= 6 && row <= 20)
				tft.drawFastHLine(x + 16, y + row, 2, color);
			if (row >= 6 && row <= 13)
				tft.drawFastHLine(x + 17, y + row, 3, color);
		}
		// The pump window remains transparent at every fuel level.
		tft.fillRect(x + 3, y + 3, 7, 6, background);
	}

	void drawDashboardAbsIcon(int centerX, int centerY, bool active,
		uint16_t activeColor, uint16_t inactiveColor, uint16_t background)
	{
		const uint16_t color = active ? activeColor : inactiveColor;
		if (active)
			tft.fillCircle(centerX, centerY, 11, color);
		else
		{
			tft.drawCircle(centerX, centerY, 11, color);
			tft.drawCircle(centerX, centerY, 10, color);
		}
		for (int angle = 132; angle <= 228; angle += 3)
		{
			const float radians = angle * DEG_TO_RAD;
			const int arcX = centerX + lroundf(cosf(radians) * 14.0f);
			const int arcY = centerY + lroundf(sinf(radians) * 14.0f);
			tft.drawPixel(arcX, arcY, color);
			tft.drawPixel(arcX - 1, arcY, color);
		}
		for (int angle = -48; angle <= 48; angle += 3)
		{
			const float radians = angle * DEG_TO_RAD;
			const int arcX = centerX + lroundf(cosf(radians) * 14.0f);
			const int arcY = centerY + lroundf(sinf(radians) * 14.0f);
			tft.drawPixel(arcX, arcY, color);
			tft.drawPixel(arcX + 1, arcY, color);
		}
		tft.setTextColor(active ? background : color,
			active ? color : background);
		tft.setTextDatum(TL_DATUM);
		constexpr int textWidth = 18;
		constexpr int textHeight = 8;
		tft.drawString("ABS", centerX + 1 - textWidth / 2,
			centerY + 1 - textHeight / 2, 1);
	}

	void drawDashboardTcsIcon(int x, int y, uint16_t color,
		uint16_t background)
	{
		// Rear-view car: symmetric trapezoid roof with an inset rear window.
		tft.fillTriangle(x + 5, y, x + 15, y, x, y + 8, color);
		tft.fillTriangle(x + 16, y, x + 21, y + 8, x, y + 8, color);
		tft.fillTriangle(x + 6, y + 2, x + 14, y + 2, x + 3, y + 7,
			background);
		tft.fillTriangle(x + 15, y + 2, x + 18, y + 7, x + 3, y + 7,
			background);

		tft.fillRect(x, y + 8, 22, 10, color);
		tft.fillRect(x + 3, y + 10, 4, 3, background);
		tft.fillRect(x + 15, y + 10, 4, 3, background);
		tft.fillRect(x + 2, y + 18, 5, 3, color);
		tft.fillRect(x + 15, y + 18, 5, 3, color);

		static constexpr int8_t skid[][2] = {
			{3, 0}, {4, 1}, {3, 2}, {1, 3}, {-1, 4},
			{-2, 5}, {-1, 6}, {1, 7}, {4, 7}};
		for (int trail = 0; trail < 2; ++trail)
		{
			const int originX = x + 1 + trail * 13;
			for (size_t i = 0; i < sizeof(skid) / sizeof(skid[0]); ++i)
			{
				const int pointX = originX + skid[i][0];
				const int pointY = y + 21 + skid[i][1];
				tft.fillCircle(pointX, pointY, 1, color);
				if (i > 0)
					tft.drawLine(originX + skid[i - 1][0],
						y + 21 + skid[i - 1][1], pointX, pointY, color);
			}
		}
	}

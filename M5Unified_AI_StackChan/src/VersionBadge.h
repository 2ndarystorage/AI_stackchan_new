#pragma once
#include "FirmwareVersion.h"

// Caller owns GfxLockGuard. Render last so faces/balloons cannot cover the badge.
template<class Display>
void drawFirmwareVersionBadge(Display& display) {
  const auto style = display.getTextStyle();
  const auto font = display.getFont();
  display.fillRoundRect(2, 2, 52, 16, 3, 0x000000);
  display.setTextDatum(lgfx::textdatum_t::top_left);
  display.setTextSize(1);
  display.setTextColor(0xFFFFFF, 0x000000);
  display.drawString(STACKCHAN_FIRMWARE_LABEL, 6, 4, &fonts::Font0);
  display.setFont(font);
  display.setTextStyle(style);
}

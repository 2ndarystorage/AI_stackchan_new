// Copyright (c) Shinya Ishikawa. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full
// license information.

#ifndef BALLOON_H_
#define BALLOON_H_
#define LGFX_USE_V1
#include <M5Unified.h>
#include "DrawContext.h"
#include "Drawable.h"

#ifndef ARDUINO
#include <string>
typedef std::string String;
#endif  // ARDUINO

namespace m5avatar {
class Balloon final : public Drawable {
 public:
  // constructor
  Balloon() = default;
  ~Balloon() = default;
  Balloon(const Balloon &other) = default;
  Balloon &operator=(const Balloon &other) = default;
  void draw(M5Canvas *spi, BoundingRect rect,
            DrawContext *drawContext) override {
    String text = drawContext->getspeechText();
    const lgfx::IFont *font = drawContext->getSpeechFont();
    if (text.length() == 0) {
      return;
    }
    ColorPalette* cp = drawContext->getColorPalette();
    uint16_t primaryColor = cp->get(COLOR_BALLOON_FOREGROUND);
    uint16_t backgroundColor = cp->get(COLOR_BALLOON_BACKGROUND);
    // Keep the anchor above the captions; shrink the text, body and tail to 80%.
    constexpr float textSize = 1.2f;  // 1.5 * 0.8
    constexpr int cx = 220;
    constexpr int cy = 140;
    constexpr int ry = static_cast<int>(24 * 0.8f + 0.5f);
    const auto style = spi->getTextStyle();
    spi->setTextSize(textSize);
    spi->setTextColor(primaryColor, backgroundColor);
    spi->setTextDatum(MC_DATUM);
    const int textWidth = spi->textWidth(text.c_str(), font);
    spi->fillEllipse(cx, cy, textWidth + 2, ry + 2, primaryColor);
    spi->fillTriangle(cx - 34, cy - 34, cx + 10, cy - 8, cx - 17, cy - 6,
                      primaryColor);
    spi->fillEllipse(cx, cy, textWidth, ry, backgroundColor);
    spi->fillTriangle(cx - 32, cy - 32, cx + 8, cy - 8, cx - 16, cy - 8,
                      backgroundColor);
    spi->drawString(text.c_str(), cx - textWidth / 6 + 4, cy, font);
    spi->setTextStyle(style);
  }
};

}  // namespace m5avatar

#endif  // BALLOON_H_

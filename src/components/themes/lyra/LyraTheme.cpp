#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/icons/generated_icons.h"  // Lucide icons (freeink::Icon) generated via the SDK Icons lib
#include "fontIds.h"

// Internal constants
namespace {
// This theme's metrics scaled by the board uiScale (see BaseTheme.cpp M()).
const ThemeMetrics& M() {
  static const ThemeMetrics m = scaleThemeMetrics(LyraMetrics::values, UITheme::uiScale());
  return m;
}
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 32;
constexpr int listIconSize = 24;
constexpr int mainMenuColumns = 2;
int coverWidth = 0;

// The generated icon sizes (px). The generator (gen_icons.py) emits these.
constexpr int kIconSizes[] = {24, 32, 40, 48};

// Scale a base icon size by the board uiScale.
int scaledIcon(int base) { return static_cast<int>(base * UITheme::uiScale() + 0.5f); }

// Index of the generated size nearest targetPx, and that size in px.
int iconVariantIndex(int targetPx) {
  int best = 0, bestDist = 1 << 30;
  for (int i = 0; i < 4; ++i) {
    const int d = kIconSizes[i] > targetPx ? kIconSizes[i] - targetPx : targetPx - kIconSizes[i];
    if (d < bestDist) {
      bestDist = d;
      best = i;
    }
  }
  return best;
}
int nearestIconSize(int targetPx) { return kIconSizes[iconVariantIndex(targetPx)]; }

// Generated Lucide icon variant nearest targetPx for a UIIcon. Each icon ships at
// every size with its optical center baked in, so a scaled UI gets a crisp larger
// asset (not an upscale) with exact alignment.
const freeink::Icon* pickIcon(UIIcon icon, int targetPx) {
  const freeink::Icon* const* v = nullptr;
#define VARIANTS(n)                                                                                     \
  {                                                                                                     \
    static const freeink::Icon* a[] = {&icon_##n##_24, &icon_##n##_32, &icon_##n##_40, &icon_##n##_48}; \
    v = a;                                                                                              \
  }
  switch (icon) {
    case UIIcon::Folder:
      VARIANTS(folder) break;
    case UIIcon::Text:
      VARIANTS(text) break;
    case UIIcon::Image:
      VARIANTS(image) break;
    case UIIcon::Book:
      VARIANTS(book) break;
    case UIIcon::File:
      VARIANTS(file) break;
    case UIIcon::Settings:
      VARIANTS(settings) break;
    case UIIcon::Transfer:
      VARIANTS(transfer) break;
    case UIIcon::Library:
      VARIANTS(library) break;
    case UIIcon::Wifi:
      VARIANTS(wifi) break;
    case UIIcon::Hotspot:
      VARIANTS(hotspot) break;
    case UIIcon::Bookmark:
      VARIANTS(bookmark) break;
    default:
      return nullptr;
  }
#undef VARIANTS
  return v[iconVariantIndex(targetPx)];
}
}  // namespace

void LyraTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  if (charging) {
    // Solid fill when charging so lightning bolt is visible
    renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4);
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  } else {
    if (percentage > 10) {
      renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 40) {
      renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 70) {
      renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4);
    }
    const int extraBarX = rect.x + 14;
    const int cavityRight = rect.x + 2 + (rect.width - 5);
    if (percentage >= 95 && extraBarX < cavityRight) {
      renderer.fillRect(extraBarX, rect.y + 2, std::min(3, cavityRight - extraBarX), rect.height - 4);
    }
  }
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  TouchRegistry::getInstance().add(Rect{rect.x, rect.y, 64, rect.height + 8}, -1, TouchRegistry::Back);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - M().batteryWidth;
  drawBatteryRight(renderer, Rect{batteryX, rect.y + 5, M().batteryWidth, M().batteryHeight}, showBatteryPercentage);

  int maxTitleWidth = title != nullptr ? renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD) : 0;
  int maxSubtitleWidth =
      subtitle != nullptr ? renderer.getTextWidth(SMALL_FONT_ID, subtitle, EpdFontFamily::REGULAR) : 0;

  // Available space is the distance between the side paddings, and a with side padding between title and subtitle.
  const int availableSpace = rect.width - M().contentSidePadding * 3;

  if (maxTitleWidth + maxSubtitleWidth > availableSpace) {
    if ((maxTitleWidth > availableSpace / 2) && (maxSubtitleWidth > availableSpace / 2)) {
      // Both are wider then half the space, truncate both.
      maxTitleWidth = availableSpace / 2;
      maxSubtitleWidth = availableSpace / 2;
    } else {
      // Truncate the the longest one
      if (maxTitleWidth > maxSubtitleWidth) {
        maxTitleWidth = availableSpace - maxSubtitleWidth;
      } else {
        maxSubtitleWidth = availableSpace - maxTitleWidth;
      }
    }
  }

  if (title) {
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + M().contentSidePadding, rect.y + M().batteryBarHeight + 3,
                      truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(SMALL_FONT_ID, subtitle, maxSubtitleWidth, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - M().contentSidePadding - truncatedSubtitleWidth, rect.y + 50,
                      truncatedSubtitle.c_str(), true);
  }
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  int currentX = rect.x + M().contentSidePadding;
  int rightSpace = M().contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - M().contentSidePadding - rightLabelWidth, rect.y + 7,
                      truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + hPaddingInSelection;
  }

  auto truncatedLabel = renderer.truncatedText(UI_10_FONT_ID, label, rect.width - M().contentSidePadding - rightSpace,
                                               EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, currentX, rect.y + 6, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  const int pad = M().contentSidePadding;
  const int spacing = M().tabSpacing;

  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  }

  // Measure pass: slide the row to keep the selected tab visible when scaled tabs
  // overflow the width (slot = label + horizontal padding).
  int contentWidth = 0, selStart = 0, selWidth = 0;
  for (const auto& tab : tabs) {
    const int slotW = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR) + 2 * hPaddingInSelection;
    if (tab.selected) {
      selStart = contentWidth;
      selWidth = slotW;
    }
    contentWidth += slotW + spacing;
  }
  const int scroll = tabBarScrollOffset(contentWidth, selStart, selWidth, rect.width - pad * 2);

  int currentX = rect.x + pad - scroll;
  int tabIndex = 0;
  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);
    const int slotW = textWidth + 2 * hPaddingInSelection;
    const int idx = tabIndex++;

    if (currentX + slotW > rect.x && currentX < rect.x + rect.width) {
      TouchRegistry::getInstance().add(Rect{currentX, rect.y, slotW + spacing, rect.height}, idx, TouchRegistry::Tab);

      if (tab.selected) {
        if (selected) {
          renderer.fillRoundedRect(currentX, rect.y + 1, slotW, rect.height - 4, cornerRadius, Color::Black);
        } else {
          renderer.fillRectDither(currentX, rect.y, slotW, rect.height - 3, Color::LightGray);
          renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + slotW, rect.y + rect.height - 3, 2, true);
        }
      }

      renderer.drawText(UI_10_FONT_ID, currentX + hPaddingInSelection, rect.y + 6, tab.label,
                        !(tab.selected && selected), EpdFontFamily::REGULAR);
    }

    currentX += slotW + spacing;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

int LyraTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  int rowHeight = (hasSubtitle) ? M().listWithSubtitleRowHeight : M().listRowHeight;
  return contentHeight / rowHeight;
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed) const {
  int rowHeight = (rowSubtitle != nullptr) ? M().listWithSubtitleRowHeight : M().listRowHeight;
  int pageItems = rect.height / rowHeight;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - M().scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - M().scrollBarWidth, scrollBarY, M().scrollBarWidth, scrollBarHeight, true);
  }

  // Draw selection
  int contentWidth = rect.width - (totalPages > 1 ? (M().scrollBarWidth + M().scrollBarRightOffset) : 1);
  if (selectedIndex >= 0) {
    renderer.fillRoundedRect(rect.x + M().contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
                             contentWidth - M().contentSidePadding * 2, rowHeight, cornerRadius, Color::LightGray);
  }

  int textX = rect.x + M().contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - M().contentSidePadding * 2 - hPaddingInSelection * 2;
  int drawnIconPx = 0;  // chosen generated icon size (scaled by uiScale)
  if (rowIcon != nullptr) {
    const int base = (rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize;
    drawnIconPx = nearestIconSize(scaledIcon(base));
    textX += drawnIconPx + hPaddingInSelection;
    textWidth -= drawnIconPx + hPaddingInSelection;
  }

  // Title baseline-top offset within the row. Single-line rows center the title
  // (and its icon) vertically in the scaled row via the real font metrics, so the
  // row never looks top-heavy as it scales. Rows with a subtitle keep the stacked
  // layout (title near the top, subtitle below).
  const int titleOffset =
      (rowSubtitle != nullptr) ? 7 : rowHeight / 2 - renderer.getTextVisualCenterOffset(UI_10_FONT_ID);

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    TouchRegistry::getInstance().add(Rect{rect.x, itemY, rect.width, rowHeight}, i, TouchRegistry::Item);
    int rowTextWidth = textWidth;

    // Draw name
    int valueWidth = 0;
    std::string valueText = "";
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxListValueWidth);
      valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + hPaddingInSelection;
      rowTextWidth -= valueWidth;
    }

    const int titleTop = itemY + titleOffset;
    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    renderer.drawText(UI_10_FONT_ID, textX, titleTop, item.c_str(), true);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, item.c_str());
      const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
      for (int py = titleTop; py < titleTop + lineH; py++)
        for (int px = textX; px < textX + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowIcon != nullptr) {
      const freeink::Icon* ic = pickIcon(rowIcon(i), drawnIconPx);
      if (ic != nullptr) {
        // Place the icon's baked-in optical center on the row's text center. For rows
        // with a subtitle, center across both lines (title near the top, subtitle at
        // itemY + 30) so the icon sits in the middle of the stacked block rather than
        // riding up with the title alone.
        int textCenter = titleTop + renderer.getTextVisualCenterOffset(UI_10_FONT_ID);
        if (rowSubtitle != nullptr) {
          const int subtitleCenter = itemY + 30 + renderer.getTextVisualCenterOffset(SMALL_FONT_ID);
          textCenter = (textCenter + subtitleCenter) / 2;
        }
        renderer.drawIcon(*ic, rect.x + M().contentSidePadding + hPaddingInSelection, textCenter - ic->opticalCenterY);
      }
    }

    if (rowSubtitle != nullptr) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      renderer.drawText(SMALL_FONT_ID, textX, itemY + 30, subtitle.c_str(), true);
    }

    // Draw value
    if (!valueText.empty()) {
      if (i == selectedIndex && highlightValue) {
        renderer.fillRoundedRect(rect.x + contentWidth - M().contentSidePadding - hPaddingInSelection - valueWidth,
                                 itemY, valueWidth + hPaddingInSelection, rowHeight, cornerRadius, Color::Black);
      }

      int valueY = itemY + 6;
      if (rowSubtitle != nullptr) {
        valueY = itemY + 16;
      }
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - M().contentSidePadding - valueWidth, valueY,
                        valueText.c_str(), !(i == selectedIndex && highlightValue));
    }
  }
}

void LyraTheme::drawButtonHintsImpl(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                    const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = LyraMetrics::values.buttonHintsHeight;
  constexpr int buttonY = LyraMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  // X3 has wider screen in portrait (528 vs 480), use more spacing
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // Draw the filled background and border for a FULL-sized button
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, cornerRadius, true, true, false,
                               false, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
    } else {
      // Draw the filled background and border for a SMALL-sized button
      renderer.fillRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, cornerRadius,
                               Color::White);
      renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1, cornerRadius, true,
                               true, false, false, true);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHintsImpl(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 0;

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(buttonMargin, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, false, true, false,
                               true, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, buttonMargin, x3ButtonY + (buttonHeight + textWidth) / 2, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonWidth;
      renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, rightX, x3ButtonY + (buttonHeight + textWidth) / 2, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                               false, true, false, true);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topHintButtonY + (i * buttonHeight) + 5;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
      }
    }
  }
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = rect.width - 2 * M().contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = M().homeCoverHeight * 0.6;
  }

  // Tapping anywhere on the continue-reading card (cover + title/gray area) opens
  // recentBooks[0] (home selector 0) — full card width, not just the cover photo.
  if (hasContinueReading) {
    TouchRegistry::getInstance().add(
        Rect{M().contentSidePadding, tileY, tileWidth, M().homeCoverHeight + 2 * hPaddingInSelection}, 0,
        TouchRegistry::Cover);
  }

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      int tileX = M().contentSidePadding;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, M().homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                                M().homeCoverHeight);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth, M().homeCoverHeight,
                        true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection + (M().homeCoverHeight / 3),
                          coverWidth, 2 * M().homeCoverHeight / 3, true);
        renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    bool bookSelected = (selectorIndex == 0);

    int tileX = M().contentSidePadding;
    int textWidth = tileWidth - 2 * hPaddingInSelection - M().verticalSpacing - coverWidth;

    if (bookSelected) {
      // Draw selection box
      renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                               Color::LightGray);
      renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection, M().homeCoverHeight,
                              Color::LightGray);
      renderer.fillRectDither(tileX + hPaddingInSelection + coverWidth, tileY + hPaddingInSelection,
                              tileWidth - hPaddingInSelection - coverWidth, M().homeCoverHeight, Color::LightGray);
      renderer.fillRoundedRect(tileX, tileY + M().homeCoverHeight + hPaddingInSelection, tileWidth, hPaddingInSelection,
                               cornerRadius, false, false, true, true, Color::LightGray);
    }

    auto titleLines = renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textWidth, 3, EpdFontFamily::BOLD);

    auto author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textWidth);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
    const int authorHeight = book.author.empty() ? 0 : (renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2);
    const int totalBlockHeight = titleBlockHeight + authorHeight;
    int titleY = tileY + tileHeight / 2 - totalBlockHeight / 2;
    const int textX = tileX + hPaddingInSelection + coverWidth + M().verticalSpacing;
    for (const auto& line : titleLines) {
      renderer.drawText(UI_12_FONT_ID, textX, titleY, line.c_str(), true, EpdFontFamily::BOLD);
      titleY += titleLineHeight;
    }
    if (!book.author.empty()) {
      titleY += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, titleY, author.c_str(), true);
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

void LyraTheme::drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const {
  constexpr int padding = 48;
  renderer.drawText(UI_12_FONT_ID, rect.x + padding,
                    rect.y + rect.height / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 2, tr(STR_NO_OPEN_BOOK), true,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rect.x + padding, rect.y + rect.height / 2 + 2, tr(STR_START_READING), true);
}

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  // Fit all buttons into rect.height: clamp the stride (and row height) when the
  // scaled rows would overflow, so a scaled-up menu never runs off the bottom.
  int stride = M().menuRowHeight + M().menuSpacing;
  if (buttonCount > 0 && stride * buttonCount > rect.height) {
    stride = rect.height / buttonCount;
  }
  const int rowHeight = std::max(1, stride - M().menuSpacing);

  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = rect.width - M().contentSidePadding * 2;
    Rect tileRect = Rect{rect.x + M().contentSidePadding, rect.y + i * stride, tileWidth, rowHeight};
    TouchRegistry::getInstance().add(tileRect, i, TouchRegistry::Item);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, Color::LightGray);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (rowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      const freeink::Icon* ic = pickIcon(rowIcon(i), scaledIcon(mainMenuIconSize));
      if (ic != nullptr) {
        // Place the icon's baked-in optical center on the label's optical center.
        const int textCenter = textY + renderer.getTextVisualCenterOffset(UI_12_FONT_ID);
        renderer.drawIcon(*ic, textX, textCenter - ic->opticalCenterY);
        textX += ic->w + hPaddingInSelection + 2;
      }
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
  }
}

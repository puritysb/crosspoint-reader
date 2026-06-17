#include "UITheme.h"

#include <BoardConfig.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cmath>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

UITheme UITheme::instance;

float UITheme::uiScale() { return BoardConfig::ACTIVE.uiScale; }

namespace {
// Round a pixel dimension by the UI scale.
int sp(int v, float s) { return static_cast<int>(std::lround(v * s)); }
}  // namespace

ThemeMetrics scaleThemeMetrics(const ThemeMetrics& b, float s) {
  // Every pixel field is scaled explicitly below. Counts, percents, ratios, bools,
  // and the intentionally-unscaled chrome (homeCover*, statusBar*, progressBar*) are
  // NOT touched. This guard fails when a ThemeMetrics field is added or removed —
  // classify the new field (scale it, or leave it and note it here) and bump the
  // expected size, so scaling can never silently miss a field.
  static_assert(sizeof(ThemeMetrics) == THEME_METRICS_SIZEOF,
                "ThemeMetrics changed: review scaleThemeMetrics() and update THEME_METRICS_SIZEOF");
  ThemeMetrics m = b;
  if (s == 1.0f) return m;
  m.batteryWidth = sp(b.batteryWidth, s);
  m.batteryHeight = sp(b.batteryHeight, s);
  m.topPadding = sp(b.topPadding, s);
  m.batteryBarHeight = sp(b.batteryBarHeight, s);
  m.headerHeight = sp(b.headerHeight, s);
  m.verticalSpacing = sp(b.verticalSpacing, s);
  m.contentSidePadding = sp(b.contentSidePadding, s);
  m.listRowHeight = sp(b.listRowHeight, s);
  m.listWithSubtitleRowHeight = sp(b.listWithSubtitleRowHeight, s);
  m.menuRowHeight = sp(b.menuRowHeight, s);
  m.menuSpacing = sp(b.menuSpacing, s);
  m.tabSpacing = sp(b.tabSpacing, s);
  m.tabBarHeight = sp(b.tabBarHeight, s);
  m.scrollBarWidth = sp(b.scrollBarWidth, s);
  m.scrollBarRightOffset = sp(b.scrollBarRightOffset, s);
  m.homeTopPadding = sp(b.homeTopPadding, s);
  // homeCoverHeight / homeCoverTileHeight are intentionally NOT scaled: the home
  // screen fills the fixed panel height exactly, so scaling the decorative cover
  // would push the menu off the bottom. The cover stays native; the menu (a touch
  // target) scales and is fit into the remaining space by drawButtonMenu.
  m.homeMenuTopOffset = sp(b.homeMenuTopOffset, s);
  m.buttonHintsHeight = sp(b.buttonHintsHeight, s);
  m.sideButtonHintsWidth = sp(b.sideButtonHintsWidth, s);
  // Status-bar metrics are intentionally NOT scaled: it's compact reader chrome
  // (with the un-remapped SMALL font), and scaling it would eat reading area and
  // bloat the customise-status-bar preview. progressBarHeight / progressBarMarginTop
  // / statusBar*Margin keep their base values.
  m.keyboardKeyWidth = sp(b.keyboardKeyWidth, s);
  m.keyboardKeyHeight = sp(b.keyboardKeyHeight, s);
  m.keyboardKeySpacing = sp(b.keyboardKeySpacing, s);
  m.keyboardBottomKeyHeight = sp(b.keyboardBottomKeyHeight, s);
  m.keyboardBottomKeySpacing = sp(b.keyboardBottomKeySpacing, s);
  m.keyboardVerticalOffset = sp(b.keyboardVerticalOffset, s);
  m.keyboardKeyCornerRadius = sp(b.keyboardKeyCornerRadius, s);
  m.keyboardSecondaryLabelRightPadding = sp(b.keyboardSecondaryLabelRightPadding, s);
  m.keyboardSecondaryLabelTopPadding = sp(b.keyboardSecondaryLabelTopPadding, s);
  m.keyboardMinArrowHeadSize = sp(b.keyboardMinArrowHeadSize, s);
  m.popupMarginX = sp(b.popupMarginX, s);
  m.popupMarginY = sp(b.popupMarginY, s);
  m.popupFrameThickness = sp(b.popupFrameThickness, s);
  m.popupCornerRadius = sp(b.popupCornerRadius, s);
  m.popupTextBaselineOffsetY = sp(b.popupTextBaselineOffsetY, s);
  m.popupProgressBarHeight = sp(b.popupProgressBarHeight, s);
  m.textFieldHorizontalPadding = sp(b.textFieldHorizontalPadding, s);
  m.textFieldNormalThickness = sp(b.textFieldNormalThickness, s);
  m.textFieldCursorThickness = sp(b.textFieldCursorThickness, s);
  m.textFieldLineEndOffset = sp(b.textFieldLineEndOffset, s);
  return m;
}

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  const ThemeMetrics* base = &BaseMetrics::values;
  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      base = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = std::make_unique<LyraTheme>();
      base = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = std::make_unique<RoundedRaffTheme>();
      base = &RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      base = &Lyra3CoversMetrics::values;
      break;
  }
  scaledMetrics = scaleThemeMetrics(*base, uiScale());
  currentMetrics = &scaledMetrics;
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  auto orientation = renderer.getOrientation();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints && orientation != GfxRenderer::Orientation::LandscapeClockwise &&
      orientation != GfxRenderer::Orientation::LandscapeCounterClockwise) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

// Screen area excluding the button hints
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += currentMetrics->buttonHintsHeight;
        safeArea.width -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += currentMetrics->buttonHintsHeight;
        safeArea.height -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= currentMetrics->buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename)) {
    return Image;
  }
  if (FsHelpers::checkFileExtension(filename, ".bin")) {
    return Firmware;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  const bool showStatusBar = SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
                             SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                             SETTINGS.statusBarBattery;
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showStatusBar ? (metrics.statusBarVerticalMargin) : 0) +
         (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}

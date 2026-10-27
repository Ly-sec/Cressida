#pragma once

#include "ui/signal.h"

namespace Style {

  inline constexpr int animFast = 100;
  inline constexpr int animNormal = 200;
  inline constexpr int animSlow = 400;

  inline constexpr float radiusSm = 3.0F;
  inline constexpr float radiusXs = 2.0F;
  inline constexpr float radiusMd = 6.0F;
  inline constexpr float radiusLg = 9.0F;
  inline constexpr float radiusXl = 12.0F;

  inline constexpr float borderWidth = 1.0F;
  inline constexpr float emphasizedBorderWidth = 3.0F;
  inline constexpr float focusRingWidth = 2.0F;
  inline constexpr float disabledOutlineAlpha = 0.5F;
  inline constexpr float textHighlightAlpha = 0.3F;
  inline constexpr float mutedChannelOpacity = 0.55F;
  inline constexpr float modalBackdropOpacity = 0.65F;
  inline constexpr float destructiveHoverBlend = 0.12F;
  inline constexpr float avatarDecorationScale = 1.2F;
  inline constexpr float nameplateAccountStartOpacity = 0.10F;
  inline constexpr float nameplateAccountDarkEndOpacity = 0.40F;
  inline constexpr float nameplateAccountLightEndOpacity = 0.30F;
  inline constexpr float nameplateRowMiddleOpacity = 0.08F;
  inline constexpr float nameplateRowEndOpacity = 0.20F;
  inline constexpr float nameplateRowHoverEndOpacity = 0.30F;
  inline constexpr float nameplateRowSelectedEndOpacity = 0.50F;
  inline constexpr float nameplateGradientStart = 0.20F;
  inline constexpr float nameplateGradientMiddle = 0.50F;
  inline constexpr float profileThemeBlend = 0.15F;
  inline constexpr float profileCardWidth = 300.0F;
  inline constexpr float profileFrameResponsiveAspect = 300.0F / 480.0F;

  inline constexpr float spaceXs = 4.0F;
  inline constexpr float spaceSm = 8.0F;
  inline constexpr float spaceMd = 12.0F;
  inline constexpr float spaceLg = 16.0F;
  inline constexpr float spaceXxs = 2.0F;
  inline constexpr float spaceCompact = 6.0F;
  inline constexpr float spaceDense = 10.0F;
  inline constexpr float spaceRelaxed = 20.0F;
  inline constexpr float spaceXl = 24.0F;
  inline constexpr float spaceWide = 28.0F;
  inline constexpr float space2Xl = 32.0F;
  inline constexpr float spaceSection = 40.0F;
  inline constexpr float spaceGroup = 48.0F;
  inline constexpr float embedMaximumWidth = 480.0F;
  inline constexpr float embedThumbnailSize = 72.0F;
  inline constexpr float embedImageMaximumHeight = 280.0F;
  inline constexpr float mediaGalleryMaximumWidth = 550.0F;
  inline constexpr float mediaGalleryMaximumHeight = 400.0F;
  inline constexpr float mediaGalleryPreferredCellWidth = 100.0F;
  inline constexpr float mediaGalleryAspectLimit = 2.0F;
  inline constexpr unsigned mediaGalleryMaximumColumns = 3;

  inline constexpr float cardPadding = 14.0F;
  inline constexpr float panelPadding = 14.0F;
  inline constexpr float serverFolderTintMix = 0.12F;

  inline constexpr float baseGlyphSize = 16.0F;
  inline constexpr float windowTitlebarHeight = 36.0F;
  inline constexpr float windowButtonWidth = spaceGroup;
  inline constexpr float windowButtonMinimumWidth = spaceXl;
  inline constexpr float windowButtonMaximumWidth = windowButtonWidth + spaceXl;
  inline constexpr int windowMinimumWidth = 760;
  inline constexpr int windowMinimumHeight = 520;

  inline constexpr float fontSizeMini = 11.0F;
  inline constexpr float fontSizeFine = 9.0F;
  inline constexpr float fontSizeDetail = 10.0F;
  inline constexpr float fontSizeDescription = 12.0F;
  inline constexpr float fontSizeCaption = 13.0F;
  inline constexpr float fontSizeBody = 14.0F;
  inline constexpr float fontSizeTitle = 16.0F;
  inline constexpr float fontSizeHeader = 20.0F;
  inline constexpr float fontSizeSubtitle = 15.0F;
  inline constexpr float fontSizeSection = 17.0F;
  inline constexpr float fontSizeHeading = 18.0F;
  inline constexpr float fontSizeDialog = 19.0F;
  inline constexpr float fontSizeProfile = 21.0F;
  inline constexpr float fontSizePage = 22.0F;
  inline constexpr float fontSizeModal = 23.0F;
  inline constexpr float fontSizeHero = 24.0F;
  inline constexpr float fontSizeWelcome = 26.0F;
  inline constexpr float fontSizeFeature = 25.0F;
  inline constexpr float fontSizeDisplay = 28.0F;

  inline constexpr float glyphSizeTiny = 12.0F;
  inline constexpr float glyphSizeSmall = 14.0F;
  inline constexpr float glyphSizeMedium = 18.0F;
  inline constexpr float glyphSizeLarge = 20.0F;
  inline constexpr float glyphSizeFeature = 24.0F;
  inline constexpr float glyphSizeHero = 28.0F;
  inline constexpr float glyphSizeDisplay = 32.0F;
  inline constexpr float controlRadius = 5.0F;
  inline constexpr float hoverRadius = 7.0F;
  inline constexpr float surfaceRadius = 8.0F;
  inline constexpr float logoHighlightMix = 0.25F;
  inline constexpr float logoShadowMix = 0.65F;
  inline constexpr int cornerRadiusDefaultPercent = 100;
  inline constexpr int cornerRadiusMaximumPercent = cornerRadiusDefaultPercent * 2;

  inline constexpr float controlHeightSm = 32.0F;
  inline constexpr float controlHeight = 38.0F;
  inline constexpr float controlHeightLg = 44.0F;
  inline constexpr float scrollWheelStep = 56.0F;
  // Pointer distance in logical px before an armed drag becomes active.
  inline constexpr float dragStartThreshold = 6.0F;

  // Base scrollbar thickness at rest, and while the pointer is over it. The bar overlays the
  // content as it expands, so hovering never reflows the scroll view.
  inline constexpr float scrollbarWidth = 6.0F;
  inline constexpr float scrollbarHoverWidth = 12.0F;
  inline constexpr float scrollbarGap = spaceSm;
  // Shortest the thumb gets on a long document; it must stay a usable drag target and read as a
  // bar rather than a lozenge next to the hovered thickness.
  inline constexpr float scrollbarMinThumbHeight = 32.0F;
  // Pointer margin on the content side of the bar, so a thin bar stays an easy grab target.
  inline constexpr float scrollbarHitSlop = 6.0F;

  // Growth cap (logical px, before content scale) for menus/dropdowns that size to their content.
  inline constexpr float menuAutoMaxWidth = 420.0F;

  // Toggle preset geometry. Track height = thumb + 2 * inset; track width = thumb + 2 * inset + travel.
  inline constexpr float toggleThumbSizeSm = 14.0F;
  inline constexpr float toggleInsetSm = 2.0F;
  inline constexpr float toggleTravelSm = 12.0F;
  inline constexpr float toggleThumbSizeMd = 18.0F;
  inline constexpr float toggleInsetMd = 3.0F;
  inline constexpr float toggleTravelMd = 16.0F;
  inline constexpr float toggleThumbSizeLg = 22.0F;
  inline constexpr float toggleInsetLg = 4.0F;
  inline constexpr float toggleTravelLg = 20.0F;

  // Slider geometry.
  inline constexpr float sliderDefaultWidth = 180.0F;
  inline constexpr float sliderTrackHeight = 8.0F;
  inline constexpr float sliderThumbSize = 18.0F;
  inline constexpr float sliderHorizontalPadding = 2.0F;

  [[nodiscard]] float cornerRadiusScale() noexcept;
  void setCornerRadiusScale(float scale) noexcept;

  [[nodiscard]] bool buttonBordersEnabled() noexcept;
  void setButtonBordersEnabled(bool enabled);
  Signal<>& buttonBordersChanged();

  [[nodiscard]] bool inputBordersEnabled() noexcept;
  void setInputBordersEnabled(bool enabled);
  Signal<>& inputBordersChanged();

  [[nodiscard]] bool popupBordersEnabled() noexcept;
  void setPopupBordersEnabled(bool enabled);

  [[nodiscard]] bool rtl() noexcept;
  void setRtl(bool rtl) noexcept;

  [[nodiscard]] bool cardBordersEnabled() noexcept;
  void setCardBordersEnabled(bool enabled);

  [[nodiscard]] bool popupShadowsEnabled() noexcept;
  void setPopupShadowsEnabled(bool enabled);

  [[nodiscard]] float scaledRadius(float radius, float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledShapeRadius(float radius, float width, float height) noexcept;
  [[nodiscard]] float scaledRadiusSm(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusMd(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusLg(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusXl(float localScale = 1.0F) noexcept;

} // namespace Style

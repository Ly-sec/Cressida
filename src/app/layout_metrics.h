#pragma once

#include "ui/style.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

// Application geometry extends the shared control style. Measurements and hit
// regions must use these same values; changing density must not move them apart.
namespace app::layout {
  struct Frame {
    float x, y, width, height;
  };
  struct MediaGallery {
    static constexpr float maximumWidth = Style::mediaGalleryMaximumWidth;
    static constexpr float maximumHeight = Style::mediaGalleryMaximumHeight;
    static constexpr float preferredCellWidth = Style::mediaGalleryPreferredCellWidth;
    static constexpr float gap = Style::spaceXs;
    static constexpr float contentGap = Style::spaceSm;
    static float aspect(float value) {
      return std::isfinite(value) && value > 0
          ? std::clamp(value, 1 / Style::mediaGalleryAspectLimit, Style::mediaGalleryAspectLimit)
          : 1;
    }
  };
  struct MediaGalleryFrame {
    std::vector<Frame> cells;
    float width = 0, height = 0;
  };
  inline MediaGalleryFrame mediaGalleryFrame(float available, std::span<const float> aspects) {
    MediaGalleryFrame result;
    if (aspects.empty() || !std::isfinite(available) || available <= 0)
      return result;
    const float width = std::min(available, MediaGallery::maximumWidth), gap = MediaGallery::gap;
    result.width = width;
    result.cells.resize(aspects.size());
    const auto row = [&](std::size_t start, std::size_t count, float y, float height) {
      const float usable = width - gap * (count - 1);
      const float minimum = std::min(MediaGallery::preferredCellWidth, usable / count);
      float sum = 0;
      for (auto i = start; i < start + count; ++i)
        sum += MediaGallery::aspect(aspects[i]);
      float x = 0;
      for (std::size_t i = 0; i < count; ++i) {
        const float cellWidth = i + 1 == count
            ? width - x
            : minimum + (usable - minimum * count) * MediaGallery::aspect(aspects[start + i]) / sum;
        result.cells[start + i] = {x, y, cellWidth, height};
        x += cellWidth + gap;
      }
    };
    auto columns = std::max<std::size_t>(
        1,
        std::min<std::size_t>(
            Style::mediaGalleryMaximumColumns,
            static_cast<std::size_t>((width + gap) / (MediaGallery::preferredCellWidth + gap))
        )
    );
    if (aspects.size() == 3 && columns >= 2) {
      const float first = MediaGallery::aspect(aspects[0]), second = MediaGallery::aspect(aspects[1]),
                  third = MediaGallery::aspect(aspects[2]);
      if (first <= 1) {
        const float stackAspect = 1 / (1 / second + 1 / third);
        const float height =
            std::min(MediaGallery::maximumHeight, (width - gap + gap * stackAspect) / (first + stackAspect));
        const float minimum = std::min(MediaGallery::preferredCellWidth, (width - gap) / 2);
        const float left = std::clamp(height * first, minimum, width - gap - minimum);
        const float minimumHeight = std::min(MediaGallery::preferredCellWidth / 2, (height - gap) / 2);
        const float upper = std::clamp(
            (height - gap) * (1 / second) / (1 / second + 1 / third), minimumHeight, height - gap - minimumHeight
        );
        result.cells = {
            {0, 0, left, height},
            {left + gap, 0, width - left - gap, upper},
            {left + gap, upper + gap, width - left - gap, height - upper - gap}
        };
        result.height = height;
      } else {
        const float lead = width / first, lower = (width - gap) / (second + third);
        const float height = std::min(MediaGallery::maximumHeight, lead + gap + lower);
        const float leadHeight = (height - gap) * lead / (lead + lower);
        result.cells[0] = {0, 0, width, leadHeight};
        row(1, 2, leadHeight + gap, height - leadHeight - gap);
        result.height = height;
      }
      return result;
    }
    if (aspects.size() == 4)
      columns = std::min<std::size_t>(columns, 2);
    const auto rows = (aspects.size() + columns - 1) / columns;
    const auto base = aspects.size() / rows, extra = aspects.size() % rows;
    std::vector<float> heights;
    std::vector<std::size_t> counts;
    float total = 0;
    std::size_t start = 0;
    for (std::size_t r = 0; r < rows; ++r) {
      const auto count = base + (r >= rows - extra ? 1 : 0);
      float sum = 0;
      for (auto i = start; i < start + count; ++i)
        sum += MediaGallery::aspect(aspects[i]);
      const float height = (width - gap * (count - 1)) / sum;
      heights.push_back(height);
      counts.push_back(count);
      total += height;
      start += count;
    }
    const float heightBudget = std::max(0.0F, MediaGallery::maximumHeight - gap * (rows - 1));
    const float scale = total > heightBudget ? heightBudget / total : 1;
    float y = 0;
    start = 0;
    for (std::size_t r = 0; r < rows; ++r) {
      const float height = heights[r] * scale;
      row(start, counts[r], y, height);
      start += counts[r];
      y += height + gap;
    }
    result.height = y - gap;
    return result;
  }
  struct Floating {
    static constexpr float inset = Style::spaceXs;
    static constexpr float gap = inset * 2;
  };
  inline Frame floatingFrame(float x, float y, float width, float height) {
    return {
        x + Floating::inset, y + Floating::inset, std::max(0.0F, width - Floating::gap),
        std::max(0.0F, height - Floating::gap)
    };
  }
  struct Composer {
    static constexpr float bottomInset = Floating::inset;
    static constexpr float minimumTextHeight = Style::fontSizeBody + Style::spaceSm;
    static constexpr float verticalPadding = Style::spaceXl;
    static constexpr float minimumHeight = minimumTextHeight + verticalPadding;
  };
  struct ComposerReply {
    static constexpr float controlSize = Style::spaceXl;
    static constexpr float padding = Style::spaceSm;
    static constexpr float inset = Style::spaceLg;
    static constexpr float gap = Style::spaceSm;
    static constexpr float height = controlSize + padding * 2;
    static constexpr float captionSize = Style::fontSizeDescription;
  };
  struct ComposerReplyFrame {
    Frame label, mention, close;
  };
  inline ComposerReplyFrame
  composerReplyFrame(float x, float y, float width, float mentionTextWidth, float labelHeight) {
    const float controlY = y + (ComposerReply::height - ComposerReply::controlSize) / 2;
    const Frame close{
        x + width - ComposerReply::inset - ComposerReply::controlSize, controlY, ComposerReply::controlSize,
        ComposerReply::controlSize
    };
    const float mentionWidth = mentionTextWidth + Style::spaceMd * 2;
    const Frame mention{
        close.x - ComposerReply::gap - mentionWidth, controlY, mentionWidth, ComposerReply::controlSize
    };
    const float labelX = x + ComposerReply::inset;
    return {
        {labelX, y + (ComposerReply::height - labelHeight) / 2, std::max(0.0F, mention.x - ComposerReply::gap - labelX),
         labelHeight},
        mention,
        close
    };
  }
  struct Sidebar {
    static constexpr float railWidth = 78;
    static constexpr float preferredWidth = 248;
    static constexpr float minimumWidth = 200;
    static constexpr float maximumWidth = 480;
    static constexpr float minimumConversation = 360;
    static constexpr float resizeSlop = Style::spaceXs;
    static constexpr float headerTop = Style::windowTitlebarHeight + Style::spaceMd;
    static constexpr float headerHeight = Style::controlHeightLg;
    static constexpr float channelsTop = headerTop + headerHeight + Style::spaceLg;
    static constexpr float categoryHeight = 24;
    static constexpr float rowHeight = Style::spaceWide;
    static constexpr float rowGap = Style::spaceXs;
    static constexpr float rowStep = rowHeight + rowGap;
    static constexpr float rowInset = Style::spaceMd;
    static constexpr float unreadWidth = Style::spaceXs;
    static constexpr float unreadHeight = Style::spaceCompact;
    static constexpr float unreadX = railWidth + Floating::inset + Style::spaceXs;
    static constexpr float accountHeight = Composer::minimumHeight;
    static constexpr float accountReserve = accountHeight + Composer::bottomInset;
    static constexpr float accountStatusSize = Style::spaceDense + Style::borderWidth;
    static constexpr float accountSettingsSize = Style::controlHeightSm + Style::spaceXs;
    static constexpr float accountOffset(float size) { return (accountHeight - size) / 2; }
    static constexpr float iconSize = 20;
    static constexpr float threadIconSize = Style::baseGlyphSize;
    static constexpr float threadBranchWidth = threadIconSize - Style::spaceXs;
    static constexpr float threadBranchHeight = rowHeight / 2;
    static constexpr float threadBranchStroke = Style::borderWidth;
    static constexpr float lockSize = 8;
    static constexpr float lockOffset = iconSize - lockSize;
    static constexpr float panelPreferredWidth = 380;
    static constexpr float panelMinimumWidth = 250;
  };
  struct Window {
    static constexpr float titlebarHeight = Style::windowTitlebarHeight;
    static constexpr int minimumWidth = Style::windowMinimumWidth;
    static constexpr int minimumHeight = Style::windowMinimumHeight;
  };
  struct DmSidebar {
    static constexpr float contentInset = Style::spaceXs;
    static constexpr float identityHeight = Style::spaceRelaxed;
    static constexpr float statusHeight = Style::spaceMd + Style::spaceXxs;
    static constexpr float conversationHeight = contentInset * 2 + identityHeight + statusHeight;
    static constexpr float conversationStep = conversationHeight + Style::spaceXs;
    static constexpr float pinSize = Style::spaceRelaxed + Style::spaceCompact;
    static constexpr float inset = Style::spaceLg;
    static constexpr float searchTop = Window::titlebarHeight + Style::spaceLg;
    static constexpr float searchHeight = Style::controlHeight;
    static constexpr float rowHeight = Style::controlHeight;
    static constexpr float friendsTop = searchTop + searchHeight + Style::spaceMd;
    static constexpr float requestsTop = friendsTop + rowHeight + Style::spaceXs;
    static constexpr float headingTop = requestsTop + rowHeight + Style::spaceMd;
    static constexpr float headingHeight = Style::spaceRelaxed + Style::spaceCompact;
    static constexpr float channelsTop = headingTop + headingHeight + Style::spaceMd;
  };
  inline float sidebarWidth(float preferred, float windowWidth) {
    if (!std::isfinite(preferred))
      preferred = Sidebar::preferredWidth;
    const float maximum = std::max(
        Sidebar::minimumWidth,
        std::min(Sidebar::maximumWidth, windowWidth - Sidebar::railWidth - Sidebar::minimumConversation)
    );
    return std::clamp(preferred, Sidebar::minimumWidth, maximum);
  }
  struct Chat {
    static constexpr auto jumpHighlightDuration = std::chrono::seconds{2};
    static constexpr float jumpHighlightOpacity = .18F;
    static constexpr float pendingStatusHeight = Style::controlHeightSm + Style::spaceXs;
    static constexpr float inset = 24;
    static constexpr float avatarSize = 40;
    static constexpr float authorGutter = 50;
    static constexpr float topPadding = Style::spaceMd;
    static constexpr float bottomPadding = Style::spaceXs;
    static constexpr float composerGap = Style::spaceSm;
    static constexpr float headerHeight = 18;
    static constexpr float footerGap = 6;
    static constexpr float reactionTailGap = Style::spaceXs;
    static constexpr float maximumCompactGroupGap = Style::spaceSm;
    static constexpr float defaultGroupGap = Style::spaceLg;
    static constexpr float minimumBodyHeight = Style::fontSizeBody + Style::spaceSm;
    static constexpr float continuationTimestampWidth = Style::spaceSection + Style::spaceXxs;
    static constexpr float centeredTextTop(float center, float logicalTop, float inkTop, float inkBottom) {
      return center + logicalTop - (inkTop + inkBottom) / 2;
    }
    static constexpr float timelineTop = 122;
    static constexpr float unreadTimelineTop = 166;
    static constexpr float minimumHistoryFetchDistance = 240;
  };
  struct Typing {
    static constexpr float height = Style::spaceXl;
    static constexpr std::size_t dotCount = 3;
    static constexpr float dotSize = Style::spaceXs;
    static constexpr float dotStep = dotSize + Style::spaceXxs;
    static constexpr float dotsWidth = dotSize + (dotCount - 1) * dotStep;
    static constexpr float textInset = dotsWidth + Style::spaceSm;
    static constexpr auto cycle = std::chrono::milliseconds{Style::animSlow * 3};
  };
  struct ComposerFrame {
    float top, panelBottom, typingTop, viewportBottom;
  };
  inline ComposerFrame composerFrame(float windowHeight, float composerHeight) {
    const float top = windowHeight - Composer::bottomInset - composerHeight;
    const float panelBottom = top - Floating::gap;
    const float typingTop = panelBottom - Style::spaceSm - Typing::height;
    return {top, panelBottom, typingTop, typingTop - Chat::composerGap};
  }
  struct TypingBadge {
    static constexpr float height = Style::spaceMd;
    static constexpr float width = height * 2;
    static constexpr float inset = Style::spaceXxs;
    static constexpr float dotSize = Style::spaceXxs;
    static constexpr float dotStep = dotSize + Style::spaceXxs;
    static constexpr float dotsWidth = dotSize + (Typing::dotCount - 1) * dotStep;
  };
  struct Reply {
    static constexpr float height = Style::spaceRelaxed;
    static constexpr float avatarSize = Style::spaceLg;
    static constexpr float avatarGap = Style::spaceXs;
    static constexpr float step = Style::spaceXl;
  };
  struct ChannelHeader {
    static constexpr float height = 66;
    static constexpr float centerY = Window::titlebarHeight + height / 2;
    static constexpr float textTop(float textHeight) { return centerY - textHeight / 2; }
    static constexpr float iconTop = 58;
    static constexpr float searchBreakpoint = 690;
    static constexpr float searchMinimumWidth = 180;
    static constexpr float searchMaximumWidth = 260;
    static constexpr float searchWidthFraction = .28F;
    static constexpr float controlTop = 52;
    static constexpr float controlHeight = 32;
    static constexpr float actionWidth = 30;
    static constexpr float actionStep = 35;
    static constexpr float actionsReserve = 146;
  };

  struct Inbox {
    static constexpr float width = Style::spaceSection * 16;
    static constexpr float maximumHeight = Style::spaceSection * 18;
    static constexpr float header = Style::spaceGroup, tabs = Style::controlHeightLg;
    static constexpr float cardHeader = Style::controlHeightLg + Style::spaceMd;
    static constexpr float padding = Style::spaceLg, gap = Style::spaceLg;
    static constexpr float avatar = Style::spaceSection;
    static constexpr float bodyLeft = padding + avatar + Style::spaceMd;
    static constexpr float previewHeight = Style::spaceSection * 5;
    static constexpr float footer = Style::controlHeightLg;
    static constexpr float emptyArtWidth = Style::spaceSection * 4;
    static constexpr float emptyArtHeight = Style::spaceSection * 3;
    static constexpr float emptyTextWidth = Style::embedMaximumWidth;
    static constexpr float emptyGuideWidth = Style::spaceSection * 9;
    static constexpr float emptyGuideHeight = Style::spaceSection * 2;
    static constexpr float emptyGap = Style::spaceXl;
    static constexpr float top = ChannelHeader::controlTop + ChannelHeader::controlHeight + Style::spaceSm;
    static constexpr std::size_t unreadPageSize = 25, maximumPreviews = 12, previewConcurrency = 3;
    static constexpr std::chrono::hours reminderWakeLimit{24};
  };
  struct SearchMenu {
    static constexpr float width = Style::spaceSection * 9;
    static constexpr float rowHeight = Style::controlHeightLg + Style::spaceSm;
    static constexpr float headingHeight = Style::space2Xl;
    static constexpr float inset = Style::spaceSm;
    static constexpr float textLeft = Style::spaceGroup;
    static constexpr float iconSize = Style::glyphSizeLarge;
    static constexpr float maximumHeight = rowHeight * 9 + headingHeight * 2 + inset;
    static constexpr std::size_t maximumSuggestions = 50, maximumRecent = 5, maximumHistory = 20;
  };
  struct Titlebar {
    static constexpr float height = Window::titlebarHeight;
    static constexpr float defaultButtonWidth = Style::windowButtonWidth;
    static float buttonWidth(float preferred) {
      return std::isfinite(preferred)
          ? std::clamp(preferred, Style::windowButtonMinimumWidth, Style::windowButtonMaximumWidth)
          : defaultButtonWidth;
    }
    static constexpr float iconSize = Style::glyphSizeMedium;
    static constexpr float dotSize = Style::glyphSizeTiny;
    static constexpr float labelInset = Style::spaceLg + Style::spaceXxs;
    static constexpr float pathFieldHeight = Style::controlHeightSm;
    static Frame buttons(float width, std::size_t count, bool left, float preferred = defaultButtonWidth) {
      const float size = static_cast<float>(count) * buttonWidth(preferred);
      return {left ? 0 : width - size, 0, size, height};
    }
    static Frame
    button(float width, std::size_t count, bool left, std::size_t index, float preferred = defaultButtonWidth) {
      const auto area = buttons(width, count, left, preferred);
      const auto cell = buttonWidth(preferred);
      return {area.x + static_cast<float>(index) * cell, 0, cell, height};
    }
    static bool
    dragContains(float x, float y, float width, std::size_t count, bool left, float preferred = defaultButtonWidth) {
      const auto area = buttons(width, count, left, preferred);
      return y >= 0 && y < height && x >= 0 && x < width && (x < area.x || x >= area.x + area.width);
    }
  };
  struct Members {
    static constexpr float width = Style::spaceSection * 6;
    static constexpr float minimumWindowWidth = Style::windowMinimumWidth + width;
    static constexpr float minimumAvailableWidth = Sidebar::minimumConversation + width;
    static constexpr float rowHeight = Style::controlHeightLg;
    static constexpr float groupHeight = Style::spaceXl;
    static constexpr float top = Window::titlebarHeight + ChannelHeader::height + Floating::inset + Style::spaceSm;
    static constexpr float bottomInset = Composer::bottomInset + Style::spaceXs;
    static constexpr float groupTextTop = (groupHeight - Style::fontSizeMini) / 2;
    static constexpr float groupIconTop = (groupHeight - Style::baseGlyphSize) / 2;
    static constexpr float rowHitHeight = rowHeight - Style::spaceXxs;
    static float viewportHeight(float windowHeight) { return std::max(0.0F, windowHeight - bottomInset - top); }
  };
  struct DraftAttachment {
    static constexpr float size = 82;
    static constexpr float topInset = Style::spaceXxs;
    static constexpr float step = 96;
    static constexpr float removeSize = 28;
    static constexpr float removeLeft = size - removeSize + Style::spaceXs;
    static constexpr float height = size + topInset * 2;
    static constexpr float composerReserve = height + Style::spaceLg + Style::spaceXs;
  };
  struct ChannelIntro {
    static constexpr float badgeSize = 58;
    static constexpr float badgeTop = Style::spaceXs;
    static constexpr float iconInset = Style::spaceMd;
    static constexpr float iconSize = badgeSize - iconInset * 2;
    static constexpr float descriptionTop = Style::spaceGroup * 3 - Style::spaceXxs;
  };
  struct Markdown {
    static constexpr float subtextScale = Style::fontSizeDescription / Style::fontSizeBody;
    static constexpr float lineGap = Style::spaceSm;
    static constexpr float strikeThickness(float size) { return Style::borderWidth * size / Style::fontSizeBody; }
  };
  struct Embed {
    static constexpr float removeSize = Style::spaceXl;
    static constexpr float removeGap = Style::spaceXs;
    static constexpr float maximumWidth = Style::embedMaximumWidth;
    static constexpr float padding = Style::spaceLg;
    static constexpr float verticalPadding = Style::spaceMd;
    static constexpr float gap = Style::spaceXs;
    static constexpr float bodySize = Style::fontSizeDescription;
    static constexpr float titleSize = Style::fontSizeBody;
    static constexpr float detailSize = Style::fontSizeMini;
    static constexpr float thumbnailSize = Style::embedThumbnailSize;
    static constexpr float thumbnailReserve = thumbnailSize + padding;
    static constexpr float thumbnailMinimumWidth = thumbnailReserve * 2 + Style::spaceXl;
  };
  struct SignIn {
    static constexpr float maximumWidth = Style::signInMaximumWidth;
    static constexpr float padding = Style::spaceXl;
    static constexpr float gap = Style::spaceLg;
    static constexpr float detailGap = Style::spaceSm;
    static constexpr float brandHeight = Style::space2Xl;
    static constexpr float qrSize = Style::signInQrSize;
    static constexpr float rowHeight = Style::spaceGroup;
    static constexpr float actionHeight = Style::controlHeightLg;
    static constexpr float footerHeight = Style::controlHeightSm;
    static constexpr float sectionHeight = Style::spaceXl;
    static constexpr float tokenHeight = Style::controlHeightLg;
    static constexpr float tokenBodyHeight = Style::spaceGroup * 2 + tokenHeight;
  };
  struct SignInFrame {
    Frame viewport, card, brand, qr, details, separator, browser, status, footer, token;
    float contentHeight = 0, maximumScroll = 0;
    bool stacked = false;
  };
  struct StartupFrame {
    Frame logo;
  };
  inline StartupFrame startupFrame(float width, float height) {
    const float side = Style::spaceGroup * 3;
    return {{(width - side) / 2, (height - side) / 2, side, side}};
  }
  inline SignInFrame signInFrame(
      float width, float height, bool token, bool loading, float statusHeight, float detailHeight = SignIn::rowHeight
  ) {
    SignInFrame result{};
    const float outer = Style::spaceLg;
    const float viewportTop = Window::titlebarHeight + outer;
    result.viewport = {
        outer, viewportTop, std::max(0.0F, width - outer * 2), std::max(0.0F, height - viewportTop - outer)
    };
    const float cardWidth = std::min(SignIn::maximumWidth, result.viewport.width);
    const float inner = std::max(0.0F, cardWidth - SignIn::padding * 2);
    result.stacked = true;
    float y = SignIn::padding;
    result.brand = {SignIn::padding, y, inner, SignIn::brandHeight};
    y += SignIn::brandHeight + SignIn::gap;
    if (token) {
      result.token = {SignIn::padding, y, inner, SignIn::tokenBodyHeight};
      y += SignIn::tokenBodyHeight;
    } else if (loading) {
      result.details = {SignIn::padding, y, inner, SignIn::rowHeight};
      y += SignIn::rowHeight;
    } else {
      const float qrSide = std::min(SignIn::qrSize, inner);
      result.qr = {(cardWidth - qrSide) / 2, y, qrSide, qrSide};
      result.details = {SignIn::padding, y + qrSide + SignIn::gap, inner, std::max(SignIn::rowHeight, detailHeight)};
      y = result.details.y + result.details.height;
      result.separator = {SignIn::padding, y + SignIn::detailGap, inner, SignIn::sectionHeight};
      y = result.separator.y + result.separator.height;
      result.browser = {SignIn::padding, y, inner, SignIn::actionHeight};
      y += SignIn::actionHeight;
    }
    result.status = {SignIn::padding, y + SignIn::detailGap, inner, statusHeight};
    y = result.status.y + statusHeight;
    result.footer = {SignIn::padding, y + SignIn::detailGap, inner, SignIn::footerHeight};
    y = result.footer.y + result.footer.height + SignIn::padding;
    result.contentHeight = y;
    result.maximumScroll = std::max(0.0F, y - result.viewport.height);
    result.card = {
        (result.viewport.width - cardWidth) / 2, std::max(0.0F, (result.viewport.height - y) / 2), cardWidth, y
    };
    return result;
  }
  inline Frame embedRemoveFrame(const Frame& embed) {
    return {embed.x + embed.width + Embed::removeGap, embed.y, Embed::removeSize, Embed::removeSize};
  }
  inline float embedContentWidth(float width) { return std::max(0.0F, width - Embed::removeSize - Embed::removeGap); }
  struct EmbedRemoval {
    static constexpr float maximumWidth = Embed::maximumWidth + Style::spaceXl * 2;
    static constexpr float padding = Style::spaceXl;
    static constexpr float gap = Style::spaceMd;
    static constexpr float footerGap = Style::spaceGroup;
    static constexpr float buttonHeight = Style::spaceSection;
    float title, description, hint, error;
    Frame dialog, cancel, remove;
  };
  inline EmbedRemoval embedRemovalLayout(
      float width, float height, float titleHeight, float descriptionHeight, float hintHeight, float errorHeight,
      float cancelWidth, float removeWidth
  ) {
    EmbedRemoval result{};
    const float dialogWidth = std::min(EmbedRemoval::maximumWidth, width - Style::space2Xl);
    const float contentWidth = dialogWidth - EmbedRemoval::padding * 2;
    result.title = EmbedRemoval::padding;
    result.description = result.title + titleHeight + EmbedRemoval::gap;
    result.hint = result.description + descriptionHeight + EmbedRemoval::gap;
    result.error = result.hint + hintHeight + EmbedRemoval::gap;
    const float footer = result.error + errorHeight + EmbedRemoval::footerGap;
    const bool stacked = cancelWidth + removeWidth + EmbedRemoval::gap > contentWidth;
    const float dialogHeight = footer
        + EmbedRemoval::buttonHeight * (stacked ? 2 : 1)
        + (stacked ? EmbedRemoval::gap : 0)
        + EmbedRemoval::padding;
    result.dialog = {
        (width - dialogWidth) / 2, std::max(Window::titlebarHeight + Style::spaceSm, (height - dialogHeight) / 2),
        dialogWidth, dialogHeight
    };
    result.remove = {
        dialogWidth - EmbedRemoval::padding - (stacked ? contentWidth : removeWidth),
        footer + (stacked ? EmbedRemoval::buttonHeight + EmbedRemoval::gap : 0), stacked ? contentWidth : removeWidth,
        EmbedRemoval::buttonHeight
    };
    result.cancel = {
        stacked ? EmbedRemoval::padding : result.remove.x - EmbedRemoval::gap - cancelWidth, footer,
        stacked ? contentWidth : cancelWidth, EmbedRemoval::buttonHeight
    };
    return result;
  }
  struct AutoModeration {
    static constexpr float avatarSize = Style::space2Xl;
    static constexpr float indent = avatarSize + Style::spaceSm;
    static constexpr float gap = Style::spaceSm;
    static constexpr float bodySize = Style::fontSizeBody;
    static constexpr float detailSize = Style::fontSizeDescription;
    static constexpr float actionSize = Style::fontSizeDetail;
    static constexpr float actionHeight = Style::spaceXl;
    static constexpr float actionPadding = Style::spaceXs;
    static constexpr float actionGap = Style::spaceMd;
    static constexpr float actionGlyph = Style::baseGlyphSize;
  };
  struct AutoModerationActions {
    std::array<Frame, 3> controls;
    float height;
  };
  inline AutoModerationActions autoModerationActions(float width, const std::array<float, 3>& captions) {
    AutoModerationActions result{};
    float x = 0, y = 0;
    for (std::size_t i = 0; i < captions.size(); ++i) {
      const float controlWidth = std::min(
          width,
          captions[i] + AutoModeration::actionPadding * 2 + (i == 0 ? AutoModeration::actionGlyph + Style::spaceXs : 0)
      );
      if (x > 0 && x + controlWidth > width) {
        x = 0;
        y += AutoModeration::actionHeight;
      }
      result.controls[i] = {x, y, controlWidth, AutoModeration::actionHeight};
      x += controlWidth + AutoModeration::actionGap;
    }
    result.height = y + AutoModeration::actionHeight;
    return result;
  }
  struct Tooltip {
    static constexpr float maximumWidth = 480;
    static constexpr float horizontalPadding = Style::spaceDense;
    static constexpr float verticalPadding = Style::spaceSm;
    static constexpr float top = Window::titlebarHeight + Style::spaceXs;
    static constexpr float belowAnchor = 38;
  };
  struct Cosmetics {
    static Frame profileFrameBody(const Frame& bounds, float overflowHorizontal) {
      return {bounds.x - overflowHorizontal, bounds.y, bounds.width + overflowHorizontal * 2, bounds.height};
    }
    static Frame profileFrameOuter(const Frame& body, float overflowTop, float overflowBottom) {
      return {body.x, body.y - overflowTop, body.width, body.height + overflowTop + overflowBottom};
    }
    static bool responsiveFrameRail(const Frame& body) {
      return body.height > 0 && body.width / body.height <= Style::profileFrameResponsiveAspect;
    }
    static Frame avatarDecoration(float x, float y, float size) {
      const float extent = size * Style::avatarDecorationScale;
      return {x + (size - extent) / 2, y + (size - extent) / 2, extent, extent};
    }
  };
  struct Profile {
    static constexpr float radius = Style::radiusXl + Style::radiusSm;
    static constexpr float width = Style::profileCardWidth;
    static constexpr float inset = 18;
    static constexpr float maximumBioHeight = 63;
    static constexpr float compactHeight = 510;
    static constexpr float compactBannerHeight = 76;
    static constexpr float bannerHeight = 108;
    static constexpr float identityHeight = 112;
    static constexpr float pronounIdentityHeight = 136;
    static constexpr float footerHeight = 70;
    static constexpr float viewportReserve = 64;
    static constexpr float anchorOffset = 96;
    static constexpr float roleChipHeight = 26;
    static constexpr float roleChipStep = 28;
    static constexpr float collapsedRoles = 6;
  };
  struct Settings {
    static constexpr float frameWidth = 1100;
    static constexpr float narrowWidth = 760;
    static constexpr float navigationWidth = 216;
    static constexpr float narrowNavigationWidth = 180;
    static constexpr float navigationTop = 124;
    static constexpr float navigationBottom = 78;
    static constexpr float navigationRowHeight = 30;
    static constexpr float navigationHeadingHeight = 26;
    // Native bars expand inward and add hit slop. Keep all of that outside
    // the clipped navigation/content panes, with a stationary visible gap.
    static constexpr float scrollbarWidth = Style::scrollbarWidth;
    static constexpr float scrollbarInwardExtent =
        Style::scrollbarHoverWidth - Style::scrollbarWidth + Style::scrollbarHitSlop;
    static constexpr float scrollbarLane = Style::scrollbarHoverWidth + Style::scrollbarHitSlop + Style::scrollbarGap;
    static constexpr float contentGutter = 28;
    static constexpr float contentInset = Style::cardPadding;
    static constexpr float contentMaximumWidth = 730;
    static constexpr float rightReserve = 68;
    static constexpr float viewportTop = 108;
    static constexpr float actionHeight = 26;
    static constexpr float actionRadius = Style::controlRadius;
    static constexpr float stackThreshold = 420;
    static constexpr float toggleWidth = Style::toggleThumbSizeSm + Style::toggleInsetSm * 2 + Style::toggleTravelSm;
    static constexpr float toggleHeight = Style::toggleThumbSizeSm + Style::toggleInsetSm * 2;
    static constexpr float saveHeight = 48;
    static constexpr float stackedSaveHeight = 82;
  };
  struct SettingsFrame {
    float x, navigationWidth, contentLeft, contentWidth, contentX, innerWidth;
    float navigationContentWidth, navigationScrollbarX, contentScrollbarX;
  };
  inline SettingsFrame settingsFrame(float windowWidth) {
    const float x = std::max(Style::spaceLg, (windowWidth - Settings::frameWidth) / 2);
    const float navigation =
        windowWidth < Settings::narrowWidth ? Settings::narrowNavigationWidth : Settings::navigationWidth;
    const float left = x + navigation + Settings::contentGutter;
    const float width = std::max(
        Settings::narrowNavigationWidth,
        std::min(windowWidth - left - Settings::rightReserve, Settings::contentMaximumWidth)
    );
    const auto viewportWidth = width - Settings::scrollbarLane;
    return {
        x,
        navigation,
        left,
        viewportWidth,
        left + Settings::contentInset,
        viewportWidth - Settings::contentInset * 2,
        navigation - Settings::scrollbarLane,
        x + navigation - Settings::scrollbarWidth,
        left + width - Settings::scrollbarWidth
    };
  }
  struct Picker {
    static constexpr float emojiWidth = 552;
    static constexpr float gifWidth = 440;
    static constexpr float maximumHeight = 520;
    static constexpr float minimumHeight = 240;
    static constexpr float emojiMinimumHeight = 250;
    static constexpr float stickerInfoHeight = 430;
    static constexpr float composerReserve = 64;
    static constexpr float memberBreakpoint = 1000;
    static constexpr float memberWidth = 240;
    static constexpr float searchTop = 52;
    static constexpr float contentTop = 104;
    static constexpr float railWidth = 64;
    static constexpr float railItemSize = 36;
    static constexpr float railStep = 44;
    static constexpr float emojiRowStep = 48;
    static constexpr float emojiCellHeight = 44;
    static constexpr float emojiImageSize = 36;
    static constexpr float favoriteSize = 17;
    static constexpr float gifMinimumTileHeight = 80;
    static constexpr float gifMaximumTileHeight = 220;
    static constexpr float gifFallbackTileHeight = 140;
  };
  inline Frame pickerFrame(
      float windowWidth, float composerTop, bool members, float desiredWidth,
      float minimumHeight = Picker::minimumHeight, float maximumHeight = Picker::maximumHeight
  ) {
    const float width = std::min(desiredWidth, windowWidth - Style::spaceLg * 2);
    const float height = std::min(maximumHeight, std::max(minimumHeight, composerTop - Picker::composerReserve));
    const float memberReserve = members && windowWidth >= Picker::memberBreakpoint ? Picker::memberWidth : 0;
    const float x = std::clamp(
        windowWidth - memberReserve - width - Style::spaceXl, Style::spaceLg,
        std::max(Style::spaceLg, windowWidth - width - Style::spaceLg)
    );
    const float y = std::max(Window::titlebarHeight + Style::spaceSm, composerTop - height - Style::spaceSm);
    return {x, y, width, height};
  }
  struct Avatar {
    static constexpr float tiny = 20, compact = 24, dm = 28, member = 30, small = 32, account = 34;
    static constexpr float chat = Chat::avatarSize, rail = Style::spaceSection, preview = 56, large = 64;
    static constexpr float request = 72, profile = 84, hero = 96;
  };
  struct DmProfile {
    static constexpr float width = Style::spaceSection * 8;
    static constexpr float inset = Style::spaceLg;
    static constexpr float bannerHeight = Style::spaceGroup * 2;
    static constexpr float avatarSize = Avatar::profile;
    static constexpr float avatarTop = bannerHeight - avatarSize / 2;
    static constexpr float identityTop = avatarTop + avatarSize + Style::spaceLg;
    static constexpr float nameHeight = Style::spaceWide;
    static constexpr float metaHeight = Style::spaceRelaxed;
    static constexpr float statusHeight = Style::spaceXl;
    static constexpr float gap = Style::spaceSm;
    static constexpr float sectionGap = Style::spaceLg;
    static constexpr float footerHeight = Style::controlHeight + inset * 2;
    static constexpr float statusScale = avatarSize / Avatar::rail;
    static constexpr float headerAvatar = Avatar::compact;
    static constexpr float headerGap = Style::spaceSm;
  };
  struct AccountFrame {
    float x, y, width, height, avatarX, avatarY, nameX, nameWidth, settingsX, settingsY, presenceWidth;
  };
  inline AccountFrame accountFrame(float windowHeight, float sidebarRight) {
    const float x = Floating::inset, y = windowHeight - Sidebar::accountReserve;
    const float width = sidebarRight - x * 2;
    const float avatarX = x + Style::spaceSm;
    const float nameX = avatarX + Avatar::account + Style::spaceSm;
    const float settingsX = x + width - Style::spaceXs - Sidebar::accountSettingsSize;
    return {
        x,
        y,
        width,
        Sidebar::accountHeight,
        avatarX,
        y + Sidebar::accountOffset(Avatar::account),
        nameX,
        std::max(0.0F, settingsX - Style::spaceSm - nameX),
        settingsX,
        y + Sidebar::accountOffset(Sidebar::accountSettingsSize),
        settingsX - x
    };
  }
  struct Voice {
    static constexpr float height = Style::controlHeight + Style::spaceSection + Style::spaceMd;
    static constexpr float reserve = height + Floating::gap;
    static constexpr float padding = Style::spaceSm;
    static constexpr float memberStep = Style::controlHeightSm + Style::spaceXxs;
    static constexpr float memberHeight = Style::controlHeightSm;
    static constexpr float memberAvatar = Avatar::tiny;
    static constexpr float memberSpeakingStroke = Style::borderWidth * 2;
    static constexpr float memberAvatarX = memberSpeakingStroke;
    static constexpr float memberIndent = Sidebar::rowInset * 2 + Sidebar::iconSize;
    static constexpr float memberStatus = Style::baseGlyphSize;
    static constexpr float memberTail = Style::spaceXs;
    static constexpr float membersHeight(std::size_t count) { return count ? count * memberStep + memberTail : 0; }
    static Frame frame(float windowHeight, float sidebarRight) {
      const auto account = accountFrame(windowHeight, sidebarRight);
      return {
          Sidebar::railWidth + Floating::inset, account.y - reserve,
          sidebarRight - Sidebar::railWidth - Floating::inset * 2, height
      };
    }
    static std::array<Frame, 4> controls(const Frame& bar) {
      std::array<Frame, 4> frames;
      const float width =
          std::max(0.0F, (bar.width - padding * 2 - Style::spaceXs * (frames.size() - 2)) / (frames.size() - 1));
      const float y = bar.y + bar.height - padding - Style::controlHeightSm;
      for (std::size_t i = 0; i + 1 < frames.size(); ++i)
        frames[i] = {bar.x + padding + i * (width + Style::spaceXs), y, width, Style::controlHeightSm};
      frames.back() = {
          bar.x + bar.width - padding - Style::controlHeightSm, bar.y + padding, Style::controlHeightSm,
          Style::controlHeightSm
      };
      return frames;
    }
    static Frame connectionIcon(const Frame& bar) {
      return {bar.x + padding, bar.y + padding, Style::controlHeightSm, Style::controlHeightSm};
    }
    static Frame connectionText(const Frame& bar) {
      const auto icon = connectionIcon(bar), disconnect = controls(bar).back();
      const float x = icon.x + icon.width + Style::spaceSm;
      return {x, icon.y, std::max(0.0F, disconnect.x - Style::spaceXs - x), icon.height};
    }
  };
  struct CountBadge {
    static constexpr float height = Style::glyphSizeMedium;
    static constexpr float fontSize = Style::fontSizeDescription;
    static constexpr float padding = Style::spaceDense;
    static constexpr float railHeight = Style::spaceMd + Style::spaceXxs;
    static constexpr float railFontSize = Style::fontSizeDetail;
    static constexpr float railPadding = Style::spaceCompact;
    static constexpr float ring = Style::spaceXxs;
  };
  struct ServerRail {
    static constexpr float iconX = (Sidebar::railWidth - Avatar::rail) / 2;
    static constexpr float homeTop = Window::titlebarHeight + Style::spaceLg;
    static constexpr float logoSize = Avatar::rail - Style::spaceXs * 2;
    static constexpr float logoInset = (Avatar::rail - logoSize) / 2;
    static constexpr float folderSize = Avatar::rail + Style::spaceXs;
    static constexpr float folderX = (Sidebar::railWidth - folderSize) / 2;
    static constexpr float folderAvatarSize = Style::baseGlyphSize;
    static constexpr float folderAvatarStep = folderAvatarSize + Style::spaceXxs;
    static constexpr float folderInset = (folderSize - folderAvatarSize - folderAvatarStep) / 2;
    static constexpr float folderHeaderHeight = Style::space2Xl;
    static constexpr float folderGlyphSize = Style::glyphSizeMedium;
    static constexpr float folderChildStep = Avatar::rail + Style::spaceCompact;
    static constexpr float folderGap = Style::spaceSm;
    static constexpr float dropEdgeFraction = 0.25F;
    static constexpr float dropStroke = Style::borderWidth * 2;
    static constexpr float autoScrollEdge = Avatar::rail / 2;
    static constexpr float autoScrollSpeed = (Avatar::rail + Style::spaceMd) * 1000 / Style::animNormal;
    struct FolderFrame {
      float headerHeight, childrenHeight, height;
    };
    static FolderFrame folderFrame(std::size_t children, float progress) {
      const float p = std::clamp(progress, 0.0F, 1.0F);
      const float header = std::lerp(folderSize, folderHeaderHeight, p);
      const float reveal = children * folderChildStep * p;
      return {header, reveal, header + reveal};
    }
    static constexpr float markerX = Style::spaceSm + Style::spaceXxs;
    static constexpr float badgeRight = iconX + Avatar::rail + CountBadge::ring;
    static constexpr float badgeTop(float height) { return height - CountBadge::railHeight - CountBadge::ring; }
    static constexpr float visibleTop = Window::titlebarHeight + ChannelHeader::height + Style::spaceSm;
    static constexpr float top =
        Window::titlebarHeight + ChannelHeader::height + Style::spaceDense + Style::borderWidth;
    static constexpr float step = Avatar::rail + Style::spaceMd;
    static constexpr float rowHeight = folderSize;
  };
  struct Emoji {
    static constexpr float badge = Style::baseGlyphSize, inlineSize = 22, picker = 36, preview = 32, jumbo = 48;
    static constexpr float pickerFont = 30, previewFont = 27;
  };
  struct Completion {
    static constexpr float rowHeight = 36;
    static constexpr float headingHeight = 32;
    static constexpr float inset = 6;
    static constexpr float footerInset = 6;
    static constexpr float chromeHeight = headingHeight + footerInset;
    static constexpr float maximumHeight = 326;
    static constexpr float minimumHeight = chromeHeight + rowHeight;
    static constexpr float preferredRoom = 180;
    static constexpr float topReserve = 110;
    static constexpr float composerLeadingInset = 46;
    static constexpr float composerExtraWidth = 142;
    static constexpr std::size_t maximumResults = 50;
  };
  struct SlashCommands {
    static constexpr float railWidth = Style::spaceGroup + Style::spaceSm;
    static constexpr float appSize = Style::space2Xl;
    static constexpr float appStep = appSize + Style::spaceMd;
    static constexpr float rowHeight = Style::spaceGroup + Style::spaceMd;
    static constexpr float headerHeight = Style::space2Xl + Style::spaceSm;
    static constexpr float footerHeight = Style::spaceWide;
    static constexpr float maximumHeight = Style::spaceGroup * 9;
    static constexpr float rowInset = Style::spaceMd;
    static constexpr float scrollbarGutter = Style::spaceLg;
    static constexpr float composerHeader = Style::space2Xl + Style::borderWidth;
    static constexpr float composerPadding = Style::spaceMd;
    static constexpr float chipGap = Style::spaceSm;
    static constexpr float chipMaximumWidth = Style::spaceGroup * 5;
    static constexpr float inputMinimumWidth = Style::spaceGroup * 3;
    static constexpr float inputMaximumWidth = Style::spaceGroup * 8;
    static constexpr float inputMaximumHeight = Style::controlHeight * 3;
    static constexpr float maximumComposerBody = Style::spaceGroup * 4;
    static constexpr float feedbackHeight = Style::space2Xl;
    struct ComposerLayout {
      std::vector<Frame> items;
      float height = 0, contentHeight = 0, viewportHeight = 0;
    };
    static ComposerLayout compose(float width, float windowHeight, std::span<const Frame> sizes, bool feedback) {
      ComposerLayout result;
      const float available = std::max(Style::controlHeight, width - composerPadding * 2);
      float x = 0, y = 0, rowHeight = Style::controlHeight;
      std::size_t rowBegin = 0;
      const auto finishRow = [&] {
        for (std::size_t i = rowBegin; i < result.items.size(); ++i)
          result.items[i].y += (rowHeight - result.items[i].height) / 2;
        rowBegin = result.items.size();
      };
      for (const auto& size : sizes) {
        const float itemWidth = std::min(available, size.width);
        if (x && x + itemWidth > available) {
          finishRow();
          x = 0;
          y += rowHeight + chipGap;
          rowHeight = Style::controlHeight;
        }
        result.items.push_back({x, y, itemWidth, size.height});
        x += itemWidth + chipGap;
        rowHeight = std::max(rowHeight, size.height);
      }
      finishRow();
      result.contentHeight = y + rowHeight;
      result.viewportHeight = std::min(result.contentHeight, std::min(maximumComposerBody, windowHeight / 3));
      result.height = composerHeader + composerPadding * 2 + result.viewportHeight + (feedback ? feedbackHeight : 0);
      return result;
    }
    static Frame picker(float x, float composerY, float width, float count) {
      const float room = std::max(0.0F, composerY - Window::titlebarHeight - Style::spaceSm * 2);
      const float height =
          std::min({maximumHeight, room, headerHeight + footerHeight + std::max(1.0F, count) * rowHeight});
      return {x, composerY - height - Style::spaceSm, width, height};
    }
  };
  struct MessageAppBadge {
    static constexpr float height = Style::spaceLg + Style::spaceXxs;
    static constexpr float padding = Style::spaceXs;
    static constexpr float gap = Style::spaceXs;
    static constexpr float fontSize = Style::fontSizeMini;
    static constexpr float radius = Style::radiusSm;
    static constexpr float width(float textWidth) { return textWidth + padding * 2; }
  };
  struct FontPicker {
    static constexpr float preferredWidth = 420;
    static constexpr float maximumViewportHeight = 600;
    static constexpr float chromeHeight = 88;
    static constexpr float rowStep = 30;
    static constexpr float rowHeight = 26;
    static constexpr float listTop = 80;
    static constexpr std::size_t maximumPreparedRows = 25;
    static constexpr std::size_t replayRows = maximumPreparedRows * 2;
  };
  struct ChannelLink {
    static constexpr float padding = Style::spaceXs;
    static constexpr float gap = Style::spaceXs;
    static constexpr float iconExtra = Style::spaceXxs;
    static constexpr float lineExtra = Style::spaceSm;
    static constexpr float breadcrumbMinimumWidth = 180;
    static constexpr float tint = .10F;
    static constexpr float hoverTint = .20F;
    static constexpr float selectionTint = .30F;
  };
} // namespace app::layout

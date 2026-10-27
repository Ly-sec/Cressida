#include "app/automod_message.h"
#include "app/discord_timestamp.h"
#include "app/emoji_catalog.h"
#include "app/layout_metrics.h"
#include "app/message_layout.h"
#include "app/message_log_layout.h"
#include "app/reply_preview.h"
#include "app/text_edit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <stdexcept>

int main(int argc, char** argv) {
  auto require = [](bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  };
  try {
    const app::MessageMeasure editMeasure = [](std::string_view text, const app::MessageStyle& style) {
      return static_cast<float>(text.size()) * style.size * .5F;
    };
    const auto alerts = api::parseMessages(nlohmann::json::parse(R"JSON([{
      "id":"9201","channel_id":"101","type":24,"author":{"id":"11","username":"Maya"},
      "embeds":[{"type":"auto_moderation_message","description":"A blocked phrase in context",
        "fields":[{"name":"rule_name","value":"Flagged words"},
          {"name":"channel_id","value":"102"},{"name":"decision_id","value":"opaque-id"},
          {"name":"keyword","value":"blocked"},{"name":"keyword_matched_content","value":"blocked"},
          {"name":"decision_outcome","value":"blocked"}]}]
    }])JSON"));
    const auto alert = app::layoutRichMessage(alerts.front(), 500, editMeasure);
    require(
        alert.autoModeration.has_value() && alert.boxes.empty(),
        "AutoMod has dedicated system presentation instead of a generic embed"
    );
    require(
        alert.plainText.starts_with("AutoMod has blocked a message")
            && alert.plainText.contains("Maya")
            && alert.plainText.contains("Rule: Flagged words")
            && !alert.plainText.contains("decision_id")
            && !alert.plainText.contains("opaque-id")
            && !alert.plainText.contains("keyword_matched_content"),
        "AutoMod presents human-readable details without exposing transport fields"
    );
    require(
        alert.autoModeration->matched == "blocked" && alerts.front().content.empty(),
        "AutoMod retains matching metadata without rewriting protocol content"
    );
    require(
        app::autoModerationJump(alerts.front()) == std::pair{std::string("102"), std::string("9201")},
        "Surrounding navigation anchors the source timeline to the alert snowflake, including blocked messages"
    );
    const auto footer = app::layout::autoModerationActions(200, {40, 120, 80});
    require(
        footer.height > app::layout::AutoModeration::actionHeight,
        "AutoMod footer controls wrap as complete actions at narrow widths"
    );
    for (const auto& frame : footer.controls)
      require(
          frame.x >= 0 && frame.x + frame.width <= 200 && frame.y + frame.height <= footer.height,
          "All three AutoMod footer actions remain inside the measured bounds"
      );
    require(
        std::ranges::any_of(
            alert.runs, [](const auto& run) { return run.text == "Maya" && run.style.userMention == "11"; }
        ),
        "AutoMod keeps the affected author as an independent profile target"
    );
    auto unavailableAlert = alerts.front();
    unavailableAlert.embeds.clear();
    require(
        app::layoutRichMessage(unavailableAlert, 500, editMeasure).plainText.contains("Flagged content unavailable"),
        "Missing AutoMod snapshots have an explicit readable fallback"
    );
    api::Message previewMessage;
    previewMessage.embeds.push_back(
        {.title = "One long linked title wraps over several words",
         .description = "Small embed description",
         .url = "https://example.com/article",
         .provider = "Example"}
    );
    const auto preview = app::layoutRichMessage(previewMessage, 230, editMeasure, Style::fontSizePage);
    require(preview.embeds.size() == 1, "Rich previews expose their measured removal frame");
    for (const float width : {160.0F, 230.0F, 600.0F}) {
      const auto measured = app::layoutRichMessage(previewMessage, width, editMeasure);
      const auto control = app::layout::embedRemoveFrame(measured.embeds.front());
      require(
          control.x >= 0 && control.x + control.width <= width && control.y >= 0,
          "Embed close controls fit alongside the card without covering its title"
      );
    }
    const auto removalFrames = app::layout::embedRemovalLayout(320, 520, 22, 40, 40, 30, 100, 200);
    require(
        removalFrames.remove.y > removalFrames.cancel.y
            && removalFrames.remove.x + removalFrames.remove.width <= removalFrames.dialog.width
            && removalFrames.remove.y + removalFrames.remove.height <= removalFrames.dialog.height,
        "Narrow confirmations stack complete actions within the measured dialog"
    );
    auto suppressedPreview = previewMessage;
    suppressedPreview.flags = api::messageSuppressEmbeds;
    suppressedPreview.content = "Keep the original link";
    const auto suppressed = app::layoutRichMessage(suppressedPreview, 230, editMeasure);
    require(
        suppressed.embeds.empty()
            && suppressed.boxes.empty()
            && suppressed.plainText.starts_with("Keep the original link"),
        "Flags-only updates hide previews while keeping message text"
    );
    for (const std::string type : {"image", "gifv", "video"}) {
      api::Message mediaMessage;
      api::Embed embed;
      embed.type = type;
      embed.url = "https://example.com/media";
      embed.image = {"https://example.com/image.png", 320, 200};
      if (type == "video")
        embed.video = {"https://example.com/video.mp4", 320, 200};
      mediaMessage.content = embed.url;
      mediaMessage.embeds = {embed};
      const auto mediaPreview = app::layoutRichMessage(mediaMessage, 230, editMeasure);
      require(
          mediaPreview.embeds.size() == 1 && !mediaPreview.pictures.empty(),
          "Image, animated and video embeds retain a distinct removal frame"
      );
      mediaMessage.flags = api::messageSuppressEmbeds;
      const auto removed = app::layoutRichMessage(mediaMessage, 230, editMeasure);
      require(
          removed.embeds.empty() && removed.pictures.empty() && removed.plainText.contains("https://example.com/media"),
          "Suppressing media embeds restores their original standalone URL"
      );
    }
    const auto linkAreas = app::messageLinkAreas(preview);
    require(
        !linkAreas.empty()
            && std::ssize(linkAreas)
                < std::ranges::count_if(preview.runs, [](const auto& run) { return !run.style.link.empty(); }),
        "Multiword embed links use one continuous hit region per line, including spaces"
    );
    for (const auto& run : preview.runs) {
      require(
          run.style.size <= app::layout::Embed::titleSize, "Embed typography is bounded independently of chat text size"
      );
      if (!run.style.link.empty()) {
        require(!app::messageMarkup(run.text, run.style).contains("<u>"), "Embed links have no resting underline");
        auto hovered = run.style;
        hovered.linkHovered = true;
        require(
            app::messageMarkup(run.text, hovered).contains("<u>"),
            "Hover adds the link underline without changing wire text"
        );
      }
    }
    for (const auto& area : linkAreas) {
      require(area.group == linkAreas.front().group, "Wrapped lines retain one logical link identity");
      const auto& first = preview.runs[area.first];
      const auto& last = preview.runs[area.last];
      require(
          area.x == first.x && area.x + area.width == last.x + last.width,
          "Link regions span from the first to last glyph, including inter-word gaps"
      );
    }
    const auto separatedLinks =
        app::layoutMessage("[first](https://example.com) plain [second](https://example.com)", 500, editMeasure);
    const auto separatedAreas = app::messageLinkAreas(separatedLinks);
    require(
        separatedAreas.size() == 2 && separatedAreas[0].group != separatedAreas[1].group,
        "Separate links sharing a URL do not share a hover group across unrelated text"
    );
    const auto adjacentLinks = app::messageLinkAreas(
        app::layoutMessage("[first](https://example.com) [second](https://example.com)", 500, editMeasure)
    );
    require(
        adjacentLinks.size() == 2 && adjacentLinks[0].group != adjacentLinks[1].group,
        "Distinct adjacent Markdown links retain separate identities even when their URL matches"
    );
    for (const auto content :
         {"https://example.com", "[a link with spaces](https://example.com)",
          "__[underlined link](https://example.com)__"}) {
      const auto link = app::layoutMessage(content, 100, editMeasure);
      const auto areas = app::messageLinkAreas(link);
      require(!areas.empty(), "Ordinary message links expose hover regions");
      for (std::size_t i = 0; i < link.runs.size(); ++i) {
        const auto& run = link.runs[i];
        if (run.style.link.empty())
          continue;
        require(!app::messageMarkup(run.text, run.style).contains("<u>"), "Links never underline at rest");
        auto style = run.style;
        style.linkHovered = app::messageLinkHovered(areas, i, areas.front().x, areas.front().y);
        require(app::messageMarkup(run.text, style).contains("<u>"), "Hover underlines every line of the same link");
        require(!app::messageLinkHovered(areas, i, -1, -1), "Leaving a link removes its hover state");
      }
    }
    require(
        app::messageMarkup("underlined", {.underline = true}).contains("<u>"),
        "Explicit plain-text underline remains visible"
    );
    const auto maskedText = app::layoutMessage("||first **bold** last|| plain ||another secret||", 500, editMeasure);
    const auto hiddenAreas = app::messageSpoilerAreas(maskedText);
    require(hiddenAreas.size() == 2, "Each spoiler has one mask across words, spaces and nested emphasis");
    for (const auto& area : hiddenAreas) {
      require(area.last > area.first, "A multiword spoiler mask spans its individual text runs");
      const auto& first = maskedText.runs[area.first];
      const auto& last = maskedText.runs[area.last];
      require(area.x == first.x && area.x + area.width == last.x + last.width, "Spoiler masks cover inter-word spaces");
    }
    const auto adjacentSpoilers = app::layoutMessage("||one||||two||", 500, editMeasure);
    require(
        app::messageSpoilerAreas(adjacentSpoilers).size() == 2, "Adjacent spoilers retain distinct span identities"
    );
    const auto wrappedSpoiler = app::layoutMessage("||first second third fourth||", 85, editMeasure);
    const auto wrappedAreas = app::messageSpoilerAreas(wrappedSpoiler);
    require(wrappedAreas.size() > 1, "A wrapping spoiler has one coherent mask per line");
    for (const auto& area : wrappedAreas)
      require(area.x >= 0 && area.x + area.width <= 85, "Wrapped masks stay inside the measured column");
    const auto crossed = app::layoutMessage("~~ text abc ~~", 500, editMeasure);
    require(crossed.plainText == " text abc \n", "Spaced strikethrough delimiters preserve enclosed text");
    require(
        std::ranges::all_of(crossed.runs, [](const auto& run) { return run.style.strike; }),
        "Strikethrough includes spaces"
    );
    require(
        std::ranges::any_of(
            crossed.runs,
            [](const auto& run) { return run.text == " " && app::messageMarkup(run.text, run.style).contains("<s>"); }
        ),
        "Strikethrough draws through its inter-word gaps"
    );
    const auto strikeAreas = app::messageStrikeAreas(crossed);
    require(
        strikeAreas.size() == 1
            && strikeAreas.front().first == 0
            && strikeAreas.front().last == crossed.runs.size() - 1,
        "Strikethrough paints one continuous line across all words and spaces"
    );
    const auto nestedStrike = app::layoutMessage("~~strike **through** several words~~", 500, editMeasure);
    require(app::messageStrikeAreas(nestedStrike).size() == 1, "Nested emphasis does not split the strike line");
    const auto wrappedStrike = app::layoutMessage("~~strike through several words~~", 85, editMeasure);
    require(app::messageStrikeAreas(wrappedStrike).size() > 1, "Wrapping creates one strike decoration per line");
    for (const auto size : {Style::fontSizeBody, Style::fontSizePage}) {
      const auto small =
          app::layoutMessage("-# **small text** with `code` and more words\nnormal", 100, editMeasure, size);
      require(small.plainText.starts_with("small text with code"), "Subtext removes only its leading syntax");
      require(!small.plainText.contains("-#"), "Subtext markers never enter copied text");
      require(
          small.runs.back().style.size == size && !small.runs.back().style.muted,
          "The next source line restores body typography"
      );
      for (const auto& run : small.runs) {
        require(
            small.plainText.substr(run.offset, run.text.size()) == run.text,
            "Styled whitespace and subtext retain byte selection offsets"
        );
        if (run.text != "normal")
          require(run.style.size < size && run.style.muted, "All wrapped subtext runs stay smaller and muted");
      }
      const auto one = app::layoutMessage("-# small", 500, editMeasure, size);
      const auto ordinary = app::layoutMessage("small", 500, editMeasure, size);
      require(one.height < ordinary.height, "Subtext uses a smaller measured line height");
    }
    for (const auto content :
         {"`-# literal ~~strike~~ ||secret||`", "```\n-# literal ~~strike~~ ||secret||\n```",
          "escaped \\~\\~literal\\~\\~", "inline -# literal"}) {
      const auto literal = app::layoutMessage(content, 500, editMeasure);
      require(
          std::ranges::none_of(
              literal.runs, [](const auto& run) { return run.style.strike || run.style.spoiler || run.style.muted; }
          ),
          "Code, escaped delimiters and inline prefixes retain literal text"
      );
    }
    const auto singleBreak = app::layoutMessage("first\nsecond", 500, editMeasure);
    const float lineStep = singleBreak.runs.back().y - singleBreak.runs.front().y;
    for (const auto& [input, expected] :
         {std::pair{"first\n\nsecond", "first\n\nsecond\n"}, std::pair{"first\n\n\n\nsecond", "first\n\n\n\nsecond\n"},
          std::pair{"first\r\n\r\nsecond", "first\n\nsecond\n"}, std::pair{"first\r\rsecond", "first\n\nsecond\n"},
          std::pair{"first\n \t\nsecond", "first\n\nsecond\n"}}) {
      const auto paragraphs = app::layoutMessage(input, 500, editMeasure);
      const auto breaks = std::ranges::count(std::string_view(expected), '\n') - 1;
      require(paragraphs.plainText == expected, "Copy and selection retain explicit blank paragraph lines");
      require(
          paragraphs.runs.back().y - paragraphs.runs.front().y == lineStep * breaks,
          "Blank paragraph lines occupy their actual text line height"
      );
      require(
          paragraphs.runs.back().offset == paragraphs.plainText.find("second"),
          "Run selection offsets include preserved blank lines"
      );
    }
    for (const auto input : {"first\n\n**second**", "first\n\n<@1>", "first\n\nhttps://example.com"}) {
      const auto paragraphs = app::layoutMessage(input, 500, editMeasure);
      require(
          paragraphs.plainText.starts_with("first\n\n") && paragraphs.runs.back().y == lineStep * 2,
          "Formatting, mentions and links keep their preceding blank line"
      );
    }
    const auto looseList = app::layoutMessage("- first\n\n- second", 500, editMeasure);
    require(
        looseList.plainText == "• first\n\n• second\n", "Loose list copy retains the blank line before the next marker"
    );
    const auto secondItem = std::ranges::find_if(looseList.runs, [](const auto& run) { return run.text == "second"; });
    require(
        secondItem != looseList.runs.end()
            && secondItem != looseList.runs.begin()
            && (secondItem - 1)->text == "• "
            && (secondItem - 1)->y == secondItem->y,
        "A blank list line moves the marker with its text"
    );
    api::Message forwardedMessage;
    forwardedMessage.id = "900";
    forwardedMessage.forwarded = api::ForwardedMessage{"800", "20", "1", "2026-10-18T18:50:00Z", "", {}, {}, {}, true};
    for (int i = 0; i < 4; ++i)
      forwardedMessage.forwarded->attachments.push_back(
          {std::to_string(700 + i),
           "image.png",
           "https://cdn.discordapp.com/attachments/20/700/image.png",
           "image/png",
           {"demo-image", 800, 600}}
      );
    auto gallery = app::layoutRichMessage(forwardedMessage, 700, editMeasure);
    api::Message ordinaryImages;
    ordinaryImages.attachments = forwardedMessage.forwarded->attachments;
    const auto ordinaryGallery = app::layoutRichMessage(ordinaryImages, 700, editMeasure);
    require(
        ordinaryGallery.pictures.size() == 4
            && ordinaryGallery.pictures[0].y == ordinaryGallery.pictures[1].y
            && ordinaryGallery.pictures[2].y == ordinaryGallery.pictures[3].y,
        "Ordinary multi-image messages use a mosaic instead of stacking each image vertically"
    );
    const std::array<std::pair<int, int>, 5> mediaSizes{{{240, 400}, {400, 240}, {320, 320}, {600, 200}, {200, 600}}};
    for (std::size_t count = 1; count <= 10; ++count) {
      api::Message images;
      for (std::size_t i = 0; i < count; ++i) {
        const auto [width, height] = mediaSizes[i % mediaSizes.size()];
        const auto url =
            "https://cdn.discordapp.com/attachments/10/" + std::to_string(100 + i) + "/image.png?ex=signature";
        images.attachments.push_back({std::to_string(i + 1), "image.png", url, "image/png", {url, width, height}});
      }
      for (const auto width : {120.0F, 220.0F, 400.0F, 900.0F}) {
        const auto mosaic = app::layoutRichMessage(images, width, editMeasure);
        require(mosaic.pictures.size() == count, "Mosaics retain every attachment from one through ten images");
        require(
            mosaic.height <= app::layout::MediaGallery::maximumHeight
                    + app::layout::MediaGallery::contentGap
                    + Style::borderWidth,
            "A multi-image message stays bounded instead of growing into a full-size vertical stack"
        );
        for (std::size_t i = 0; i < count; ++i) {
          const auto& cell = mosaic.pictures[i];
          require(
              std::isfinite(cell.x)
                  && std::isfinite(cell.y)
                  && cell.width > 0
                  && cell.height > 0
                  && cell.x >= 0
                  && cell.x + cell.width <= width + Style::borderWidth
                  && cell.y + cell.height <= mosaic.height + Style::borderWidth,
              "Every gallery cell remains usable and inside the measured message at narrow widths"
          );
          require(
              cell.media == images.attachments[i].image && cell.link == images.attachments[i].url,
              "Mosaics preserve source order, original dimensions and signed attachment URLs for the viewer"
          );
          for (std::size_t j = 0; j < i; ++j) {
            const auto& other = mosaic.pictures[j];
            require(
                cell.x + cell.width <= other.x + Style::borderWidth
                    || other.x + other.width <= cell.x + Style::borderWidth
                    || cell.y + cell.height <= other.y + Style::borderWidth
                    || other.y + other.height <= cell.y + Style::borderWidth,
                "Mosaic tiles never overlap each other"
            );
          }
        }
        if (count == 1)
          require(
              std::abs(
                  mosaic.pictures[0].width / mosaic.pictures[0].height
                  - static_cast<float>(images.attachments[0].image.width) / images.attachments[0].image.height
              ) < .001F,
              "Single images retain their original aspect ratio"
          );
      }
    }
    api::Message threeImages;
    threeImages.attachments = {
        ordinaryImages.attachments[0], ordinaryImages.attachments[1], ordinaryImages.attachments[2]
    };
    threeImages.attachments[0].image.width = 200;
    threeImages.attachments[0].image.height = 400;
    const auto portraitGallery = app::layoutRichMessage(threeImages, 550, editMeasure);
    require(
        portraitGallery.pictures[0].height > portraitGallery.pictures[1].height
            && portraitGallery.pictures[0].y == portraitGallery.pictures[1].y
            && portraitGallery.pictures[1].x == portraitGallery.pictures[2].x,
        "Three-image galleries give a portrait lead a tall tile alongside two stacked images"
    );
    threeImages.attachments[0].image.width = 400;
    threeImages.attachments[0].image.height = 200;
    const auto landscapeGallery = app::layoutRichMessage(threeImages, 550, editMeasure);
    require(
        landscapeGallery.pictures[0].width > landscapeGallery.pictures[1].width
            && landscapeGallery.pictures[1].y == landscapeGallery.pictures[2].y
            && landscapeGallery.pictures[1].y > landscapeGallery.pictures[0].y,
        "Three-image galleries give a landscape lead a wide tile above two images"
    );
    auto mixedMedia = threeImages;
    mixedMedia.attachments[1].filename = "clip.mp4";
    mixedMedia.attachments[1].contentType = "video/mp4";
    mixedMedia.attachments[1].url = "https://cdn.discordapp.com/attachments/10/101/clip.mp4";
    mixedMedia.attachments[2].filename = "SPOILER_motion.gif";
    mixedMedia.attachments[2].contentType = "image/gif";
    const auto mixedGallery = app::layoutRichMessage(mixedMedia, 550, editMeasure);
    require(
        mixedGallery.pictures.size() == 3
            && !mixedGallery.pictures[1].videoSource.empty()
            && mixedGallery.pictures[2].animated
            && mixedGallery.pictures[2].spoiler,
        "Mixed image/video/GIF mosaics retain native playback and spoiler metadata"
    );
    auto mixedFiles = ordinaryImages;
    mixedFiles.attachments.insert(
        mixedFiles.attachments.begin() + 2, {"77", "notes.txt", "https://example.com/notes.txt", "text/plain"}
    );
    const auto fileGallery = app::layoutRichMessage(mixedFiles, 550, editMeasure);
    require(
        fileGallery.pictures.size() == 4
            && fileGallery.boxes.size() == 1
            && fileGallery.pictures[1].y + fileGallery.pictures[1].height <= fileGallery.boxes[0].y
            && fileGallery.boxes[0].y + fileGallery.boxes[0].height <= fileGallery.pictures[2].y,
        "Ordinary files remain in attachment order between consecutive visual galleries"
    );
    auto unknownSizes = ordinaryImages;
    for (auto& item : unknownSizes.attachments)
      item.image.width = item.image.height = 0;
    const auto unknownGallery = app::layoutRichMessage(unknownSizes, 400, editMeasure);
    require(
        unknownGallery.pictures.size() == 4 && std::isfinite(unknownGallery.height),
        "Unknown image dimensions use a stable gallery fallback"
    );
    require(
        gallery.forwards.size() == 1
            && gallery.pictures.size() == 4
            && gallery.pictures[0].y == gallery.pictures[1].y
            && gallery.pictures[2].y == gallery.pictures[3].y
            && gallery.pictures[2].y > gallery.pictures[0].y
            && gallery.pictures[1].x >= gallery.pictures[0].x + gallery.pictures[0].width,
        "Four forwarded images use a measured two-column gallery rather than a system message or vertical stack"
    );
    for (const auto width : {160.0F, 320.0F}) {
      const auto narrow = app::layoutRichMessage(forwardedMessage, width, editMeasure);
      for (const auto& image : narrow.pictures)
        require(
            image.x >= 0 && image.x + image.width <= width && image.y + image.height <= narrow.height,
            "Forwarded galleries remain inside narrow message bounds"
        );
    }
    forwardedMessage.forwarded->attachments.resize(1);
    require(
        app::layoutRichMessage(forwardedMessage, 500, editMeasure).pictures.size() == 1,
        "A single forwarded image retains its actual media preview"
    );
    forwardedMessage.forwarded->attachments.clear();
    forwardedMessage.forwarded->content = "Saved **formatted** text <@43>";
    forwardedMessage.forwarded->mentions["43"] = "Friend";
    const auto forwardedText = app::layoutRichMessage(forwardedMessage, 500, editMeasure);
    require(
        forwardedText.plainText.contains("formatted")
            && forwardedText.plainText.contains("@Friend")
            && !forwardedText.plainText.contains("Forwarded")
            && forwardedText.forwards.size() == 1,
        "Forwarded text preserves Markdown and mention resolution without adding UI labels to copied content"
    );
    api::Message editedMessage{"900", "10", "Maya", "Hello", "2026-10-17T09:00:00Z"};
    const auto originalLayout = app::layoutRichMessage(editedMessage, 400, editMeasure);
    require(!originalLayout.edited, "Unedited messages have no marker");
    editedMessage.editedTimestamp = "2026-10-17T09:01:00Z";
    auto editedLayout = app::layoutRichMessage(editedMessage, 400, editMeasure);
    require(
        editedLayout.edited
            && editedLayout.edited->x > 0
            && editedLayout.edited->y < 22
            && editedLayout.plainText == originalLayout.plainText,
        "Edited markers follow the last text line without becoming copied message text"
    );
    editedLayout = app::layoutRichMessage(editedMessage, 42, editMeasure);
    require(
        editedLayout.edited
            && editedLayout.edited->x == 0
            && editedLayout.height >= editedLayout.edited->y + editedLayout.edited->height,
        "Narrow messages reserve a new line for the marker without overflowing"
    );
    api::Message oldVersion = editedMessage;
    oldVersion.content = "Earlier **text** with <@1>";
    oldVersion.mentions["1"] = "Alex";
    oldVersion.editedTimestamp.clear();
    const auto liveLayout = app::layoutRichMessage(editedMessage, 400, editMeasure);
    const auto loggedLayout = app::withEditHistory(liveLayout, std::vector{oldVersion}, 400, editMeasure, 14);
    require(
        loggedLayout.historyHeight > 0
            && loggedLayout.edited
            && loggedLayout.edited->y == liveLayout.edited->y + loggedLayout.historyHeight
            && loggedLayout.height == liveLayout.height + loggedLayout.historyHeight,
        "Edit history reserves height and shifts the current edited marker together"
    );
    require(
        loggedLayout.plainText.contains("Earlier text with @Alex")
            && loggedLayout.plainText.ends_with(liveLayout.plainText),
        "Historical text remains selectable and current content stays last"
    );
    for (const auto& run : loggedLayout.runs) {
      require(
          run.style.historical == (run.y < loggedLayout.historyHeight),
          "Only earlier versions use muted history styling"
      );
      require(
          run.offset + run.text.size() <= loggedLayout.plainText.size(), "History text selection offsets stay valid"
      );
      if (run.image.empty())
        require(
            loggedLayout.plainText.substr(run.offset, run.text.size()) == run.text,
            "Selection offsets identify the exact historical/current text"
        );
    }
    require(
        app::withEditHistory(liveLayout, {}, 400, editMeasure, 14).height == liveLayout.height,
        "Without retained edits the normal message layout is unchanged"
    );
    editedMessage.content = "```cpp\nint x = 1;\n```";
    editedLayout = app::layoutRichMessage(editedMessage, 400, editMeasure);
    require(
        editedLayout.edited
            && editedLayout.edited->x == 0
            && !editedLayout.boxes.empty()
            && editedLayout.edited->y >= editedLayout.boxes.back().y + editedLayout.boxes.back().height,
        "Edited labels sit outside fenced code blocks"
    );
    editedMessage.content.clear();
    editedMessage.attachments.push_back(
        {"1",
         "image.png",
         "https://cdn.discordapp.com/attachments/1/2/image.png",
         "image/png",
         {"https://cdn.discordapp.com/attachments/1/2/image.png", 200, 100}}
    );
    editedLayout = app::layoutRichMessage(editedMessage, 400, editMeasure);
    require(
        editedLayout.edited
            && !editedLayout.pictures.empty()
            && editedLayout.edited->y >= editedLayout.pictures.back().y + editedLayout.pictures.back().height,
        "Media-only edited messages keep their marker clear of the image"
    );
    api::MessageReference reference{
        "1", "2", "3", "Maya",
        "First line\r\nSecond\tline\xE2\x80\xA8"
        "Third\xE2\x80\xA9"
        "Fourth"
    };
    require(
        app::replyPreview(reference, "Maya\nname") == "↳ Maya name: First line Second line Third Fourth",
        "Reply previews flatten body and author paragraph breaks into a single line"
    );
    reference.content = std::string(4095, 'a') + "🙂";
    require(
        app::replyPreview(reference, "Maya").ends_with(std::string(4095, 'a')),
        "Bounding long reply previews never splits a UTF-8 character"
    );
    reference.deleted = true;
    require(
        app::replyPreview(reference, "Maya") == "↳ Maya: Original message deleted",
        "Deleted reply previews retain a readable fallback"
    );
    api::Message original;
    original.id = "1";
    original.channelId = "2";
    original.authorId = "3";
    original.author = "Maya";
    original.avatar = "/avatars/3/avatar.png";
    original.content = "Already loaded original";
    api::MessageReference unresolved{"1", "2"};
    auto resolved = app::resolvedReply(unresolved, &original);
    require(
        resolved.content == original.content
            && resolved.author == "Maya"
            && resolved.avatar == original.avatar
            && resolved.available,
        "Reference-only replies use already loaded content and author identity"
    );
    original.content = "Edited original";
    require(
        app::resolvedReply(resolved, &original).content == "Edited original",
        "Reply previews follow loaded edits instead of retaining stale text"
    );
    original.channelId = "4";
    require(
        app::resolvedReply(unresolved, &original).content.empty(),
        "Loaded content from another channel cannot fill a reply preview"
    );
    original.channelId = "2";
    require(
        app::resolvedReply(resolved, &original, true).content.empty()
            && app::resolvedReply(resolved, &original, true).deleted,
        "A deletion journal overrides loaded original content"
    );
    unresolved.deleted = true;
    require(
        app::resolvedReply(unresolved, &original).deleted && app::resolvedReply(unresolved, &original).content.empty(),
        "An explicit unavailable original is never resurrected from loaded content"
    );
    require(argc == 2, "Emoji assets directory required");
    app::EmojiCatalog catalog;
    catalog.load(std::filesystem::path(argv[1]) / "emoji.json", std::filesystem::path(argv[1]) / "emoji-aliases.json");
    const app::MessageOptions emojiOptions{.unicodeEmoji = [&](std::string_view text) {
      return catalog.isUnicode(text);
    }};
    const app::MessageMeasure emojiMeasure = [](std::string_view text, const app::MessageStyle& style) {
      return style.emoji ? style.size : static_cast<float>(text.size() * 8);
    };
    auto profileOptions = emojiOptions;
    profileOptions.compact = true;
    profileOptions.discordTimestamps = true;
    auto profileBio = app::layoutMessage(
        "**My midnight** is <t:1754172000:t> <:wave:123>", 900, emojiMeasure, 13, {}, profileOptions
    );
    require(
        profileBio.plainText.find(*app::discordTimestamp("t:1754172000:t")) != std::string::npos
            && profileBio.plainText.find("<t:") == std::string::npos
            && std::ranges::any_of(profileBio.runs, [](const auto& run) { return run.style.bold; })
            && std::ranges::any_of(profileBio.runs, [](const auto& run) { return !run.image.empty(); }),
        "Profile bios format Discord timestamps alongside escaped Markdown and custom emoji"
    );
    profileBio = app::layoutMessage(
        "`<t:1754172000:t>` <t:invalid:t> <span color='red'>bio</span>", 900, emojiMeasure, 13, {}, profileOptions
    );
    require(
        std::ranges::any_of(
            profileBio.runs, [](const auto& run) { return run.style.code && run.text == "<t:1754172000:t>"; }
        )
            && profileBio.plainText.find("<t:invalid:t>") != std::string::npos
            && profileBio.plainText.find("<span color='red'>bio</span>") != std::string::npos,
        "Timestamps in code, malformed tokens and raw HTML stay literal in profiles"
    );
    reference.deleted = false;
    reference.avatar = "/avatars/3/avatar.png";
    reference.content = "Reply with an avatar";
    const auto withAvatar = app::layoutReplyPreview(reference, "Maya", 500, emojiMeasure, emojiOptions);
    require(
        withAvatar.pictures.size() == 1
            && withAvatar.pictures.front().media.url == reference.avatar
            && withAvatar.pictures.front().y == (withAvatar.height - withAvatar.pictures.front().height) / 2
            && withAvatar.runs[1].x >= withAvatar.pictures.front().x + withAvatar.pictures.front().width
            && withAvatar.plainText == "↳ Maya: Reply with an avatar",
        "A centered reply avatar reserves width without entering copied preview text"
    );
    const auto tinyAvatar = app::layoutReplyPreview(reference, "Maya", 20, emojiMeasure, emojiOptions);
    require(
        tinyAvatar.pictures.empty()
            && std::ranges::all_of(tinyAvatar.runs, [](const auto& run) { return run.x + run.width <= 20; }),
        "Narrow reply previews omit avatars before clipping author text"
    );
    reference.avatar.clear();
    reference.content = "my beautiful pink role <:woe:1407464763293831269> <a:dance:123> 🙂";
    auto reply = app::layoutReplyPreview(reference, "Yocraft", 1000, emojiMeasure, emojiOptions);
    auto replyImages = std::views::filter(reply.runs, [](const auto& run) { return !run.image.empty(); });
    require(
        std::ranges::distance(replyImages) == 2, "Reply previews render static and animated custom emoji as images"
    );
    require(replyImages.front().image == api::emojiPath("1407464763293831269", false), "Static reply emoji uses PNG");
    require(std::next(replyImages.begin())->image == api::emojiPath("123", true), "Animated reply emoji uses GIF");
    require(
        std::ranges::all_of(
            reply.runs,
            [](const auto& run) { return run.y == 0 && run.height == 20 && (!run.style.emoji || run.style.size == 16); }
        ) && reply.height == 20,
        "Reply emoji stay inline inside one reserved row; never jumbo"
    );
    reference.content = "<:wave:123><a:dance:456>";
    reply = app::layoutReplyPreview(reference, "Maya", 500, emojiMeasure, emojiOptions);
    require(
        std::ranges::count_if(reply.runs, [](const auto& run) { return !run.image.empty(); }) == 2,
        "Adjacent reply emoji are separate native images"
    );
    reference.content = "`<:wave:123>` <:bad:not-a-snowflake> <:wave:123>";
    reply = app::layoutReplyPreview(reference, "**Maya** <:name:789>", 2000, emojiMeasure, emojiOptions);
    require(
        std::ranges::count_if(reply.runs, [](const auto& run) { return !run.image.empty(); }) == 1
            && reply.runs.front().text == "↳ **Maya** <:name:789>: "
            && std::ranges::any_of(
                reply.runs, [](const auto& run) { return run.style.code && run.text == "<:wave:123>"; }
            ),
        "Reply author names, code and malformed custom tags are never interpreted as emoji images"
    );
    reference.content = "first\nsecond\n\nthird <:wave:123>";
    reply = app::layoutReplyPreview(reference, "Maya", 1000, emojiMeasure, emojiOptions);
    require(
        reply.plainText.find('\n') == std::string::npos
            && std::ranges::all_of(reply.runs, [](const auto& run) { return run.y == 0; }),
        "Rich reply previews flatten every paragraph onto one line"
    );
    reference.content = "lots of words <:wave:123> and more words";
    reply = app::layoutReplyPreview(reference, "Maya", 140, emojiMeasure, emojiOptions);
    require(
        reply.plainText.ends_with("…")
            && std::ranges::all_of(reply.runs, [](const auto& run) { return run.x >= 0 && run.x + run.width <= 140; }),
        "Truncated reply runs and ellipsis fit the viewport"
    );
    reference.content = "<:wave:123>";
    const auto prefix = emojiMeasure("↳ Maya: ", {.size = 12});
    reply = app::layoutReplyPreview(reference, "Maya", prefix + 15, emojiMeasure, emojiOptions);
    require(
        std::ranges::none_of(reply.runs, [](const auto& run) { return !run.image.empty(); }),
        "A clipped emoji is omitted whole, without requesting its asset"
    );
    reference.content = "https://cdn.discordapp.com/emojis/123.png?size=48&name=wave";
    reply = app::layoutReplyPreview(reference, "Maya", 1000, emojiMeasure, {.emojiImageLinks = true});
    require(
        std::ranges::count_if(reply.runs, [](const auto& run) { return !run.image.empty(); }) == 1,
        "Opt-in emoji image links render consistently in reply previews"
    );
    reference.content = "||<:hidden:123>||";
    reply = app::layoutReplyPreview(reference, "Maya", 500, emojiMeasure, emojiOptions);
    require(
        std::ranges::any_of(reply.runs, [](const auto& run) { return run.style.spoiler; }),
        "Reply previews preserve spoiler masking instead of revealing hidden emoji"
    );
    reference.deleted = true;
    reply = app::layoutReplyPreview(reference, "Maya", 1000, emojiMeasure, emojiOptions);
    require(
        reply.plainText == "↳ Maya: Original message deleted"
            && std::ranges::none_of(reply.runs, [](const auto& run) { return !run.image.empty(); }),
        "Deleted reply previews cannot load media from old content"
    );
    api::Message emojiMessage;
    emojiMessage.content = "🙂 ❤️ 👩🏽‍💻 🇩🇪 1️⃣ <:wave:123>";
    auto sized = app::layoutRichMessage(emojiMessage, 600, emojiMeasure, 14, emojiOptions);
    require(
        sized.runs.size() == 6
            && std::ranges::all_of(
                sized.runs,
                [](const auto& run) {
                  return run.style.emoji && (run.image.empty() ? run.style.size == 48 : run.width == 48);
                }
            ),
        "Unicode, skin tones, ZWJ, flags, keycaps and custom emoji share jumbo sizing"
    );
    for (const auto& run : sized.runs)
      require(run.y + run.height <= sized.height, "Jumbo rows reserve height for every emoji");
    auto compact = emojiOptions;
    compact.compact = true;
    sized = app::layoutRichMessage(emojiMessage, 600, emojiMeasure, 14, compact);
    require(
        std::ranges::all_of(
            sized.runs,
            [](const auto& run) {
              return run.style.emoji && (run.image.empty() ? run.style.size == 22 : run.width == 22);
            }
        ),
        "Compact messages retain consistent inline sizing"
    );
    emojiMessage.content = "Hello 🙂 <:wave:123>!";
    sized = app::layoutRichMessage(emojiMessage, 600, emojiMeasure, 14, emojiOptions);
    require(
        std::ranges::count_if(sized.runs, [](const auto& run) { return run.style.emoji; }) == 2
            && std::ranges::all_of(
                sized.runs,
                [](const auto& run) {
                  return !run.style.emoji || (run.image.empty() ? run.style.size == 22 : run.width == 22);
                }
            ),
        "Mixed text uses larger inline Unicode emoji without enlarging the message"
    );
    emojiMessage.content.clear();
    for (int i = 0; i < 27; ++i)
      emojiMessage.content += "🙂";
    sized = app::layoutRichMessage(emojiMessage, 120, emojiMeasure, 14, emojiOptions);
    require(
        sized.runs.size() == 27 && sized.runs.front().style.size == 48 && sized.height > 100,
        "Up to 27 jumbo emoji wrap across lines"
    );
    emojiMessage.content += "🙂";
    sized = app::layoutRichMessage(emojiMessage, 120, emojiMeasure, 14, emojiOptions);
    require(sized.runs.size() == 28 && sized.runs.front().style.size == 22, "28 emoji return to inline size");
    emojiMessage.content = "`🙂 <:wave:123>`";
    sized = app::layoutRichMessage(emojiMessage, 600, emojiMeasure, 14, emojiOptions);
    require(
        std::ranges::none_of(sized.runs, [](const auto& run) { return run.style.emoji; }),
        "Code keeps literal text sizing"
    );
    emojiMessage.content = "<:wave:123>";
    sized = app::layoutRichMessage(emojiMessage, 32, emojiMeasure, 14, emojiOptions);
    require(sized.runs.front().width <= 32, "Jumbo custom emoji fit narrow columns");
    auto measure = [](std::string_view value) { return static_cast<float>(value.size() * 8); };
    auto layout = app::layoutMessage("hi <:wave:123> there\n<a:dance:456>!", 200, measure);
    require(
        layout.runs.size() == 5
            && layout.runs[1].image == "/emojis/123.png?size=128"
            && layout.runs[3].image == "/emojis/456.gif?size=128",
        "Render static and animated custom emoji inline"
    );
    require(
        layout.runs[1].x > layout.runs[0].x && layout.runs[3].y > layout.runs[1].y,
        "Preserve emoji positions and explicit line breaks"
    );
    auto noticeOptions = emojiOptions;
    noticeOptions.mentionPrefix = false;
    const auto notice = app::layoutMessage(
        "Welcome <@123>!", 1800, emojiMeasure, 14, {{"123", "Maya **literal** <b>name</b>"}}, noticeOptions
    );
    require(
        notice.plainText.starts_with("Welcome Maya **literal** <b>name</b>!")
            && !notice.plainText.contains("@")
            && std::ranges::any_of(notice.runs, [](const auto& run) { return run.style.mention && !run.style.bold; }),
        "Inline system names omit the mention prefix while remaining literal and styled"
    );
    api::Message mentionMessage;
    mentionMessage.content = "Hi <@123> and <@!123>, see <#456>. `literal <@123>`";
    mentionMessage.mentions = {{"123", "Maya **literal**"}, {"456", "general"}};
    const auto mentions =
        app::layoutRichMessage(mentionMessage, 1800, [&](std::string_view text, const app::MessageStyle&) {
          return measure(text);
        });
    require(
        mentions.plainText.contains("@Maya **literal**")
            && mentions.plainText.contains("#general")
            && mentions.plainText.contains("literal <@123>"),
        "Resolve mentions outside code, preserving literal display names and code"
    );
    require(
        std::ranges::any_of(mentions.runs, [](const auto& run) { return run.style.mention && !run.style.bold; }),
        "Mention names cannot inject Markdown and receive mention styling"
    );
    app::MessageOptions roleOptions{.roleMention = [](std::string_view id) {
      return id == "50" ? "Nightwatch **literal**" : "";
    }};
    api::Message roleMessage;
    roleMessage.content = "Hello <@&50>, <@50> and <@&51>. `<@&50>`";
    roleMessage.mentions = {{"50", "Maya"}, {"51", "Wrong user alias"}};
    const auto resolvedRoles = app::layoutRichMessage(roleMessage, 2400, editMeasure, 14, roleOptions);
    require(
        resolvedRoles.plainText.contains("@Nightwatch **literal**")
            && resolvedRoles.plainText.contains("@Maya")
            && resolvedRoles.plainText.contains("@unknown-role")
            && resolvedRoles.plainText.contains("<@&50>"),
        "Role mentions use their own resolver, preserve code and never borrow user aliases"
    );
    require(
        std::ranges::any_of(
            resolvedRoles.runs,
            [](const auto& run) {
              return run.text.contains("Nightwatch")
                  && run.style.mention
                  && !run.style.bold
                  && run.style.userMention.empty();
            }
        ),
        "Role names are literal highlighted text without user profile targets"
    );
    require(
        app::layoutRichMessage(roleMessage, 2400, editMeasure).plainText.contains("@unknown-role"),
        "Unavailable role directories keep an explicit fallback"
    );
    const auto roleReply = app::layoutReplyPreview(
        {.author = "Maya", .content = "<@&50>", .available = true}, "Maya", 2400, editMeasure, roleOptions
    );
    require(
        roleReply.plainText.contains("@Nightwatch **literal**"), "Reply previews resolve roles with the same server"
    );
    const std::array roleVersions{roleMessage};
    const auto roleHistory = app::withEditHistory(resolvedRoles, roleVersions, 2400, editMeasure, 14, roleOptions);
    require(
        std::ranges::any_of(
            roleHistory.runs,
            [](const auto& run) {
              return run.style.historical && run.text.contains("Nightwatch") && run.style.userMention.empty();
            }
        ),
        "Retained edit history resolves role mentions without turning them into users"
    );
    auto malformed = app::layoutMessage("<:broken:abc>", 400, measure);
    require(
        malformed.runs.size() == 1 && malformed.runs[0].image.empty() && malformed.runs[0].text == "<:broken:abc>",
        "Malformed emoji must remain readable text"
    );
    auto wrapped = app::layoutMessage("abcdefghij <:ok:1>", 32, measure);
    require(wrapped.height > 24, "Wrap long words and emoji rather than overflowing");
    for (const auto& run : wrapped.runs)
      require(run.x + run.width <= 32, "Every fragment fits within the message column");
    auto unicode = app::layoutMessage("hello 👩‍💻 🌿", 500, measure);
    std::string text;
    for (const auto& run : unicode.runs)
      text += run.text;
    require(
        text.contains("👩‍💻") && text.contains("🌿"),
        "Keep Unicode emoji and ZWJ sequences for Pango color-font rendering"
    );
    auto markdown =
        app::layoutMessage("**bold** *italic* _also italic_ __underlined__ ~~gone~~ `a < b` ||secret||", 1200, measure);
    auto styled = [&](std::string_view text, auto test) {
      return std::ranges::any_of(markdown.runs, [&](const auto& run) { return run.text == text && test(run.style); });
    };
    require(styled("bold", [](const auto& s) { return s.bold; }), "Render strong emphasis");
    require(styled("italic", [](const auto& s) { return s.italic; }), "Render emphasis");
    require(styled("underlined", [](const auto& s) { return s.underline; }), "Discord double-underscore underline");
    require(styled("gone", [](const auto& s) { return s.strike; }), "Render strikethrough");
    require(styled("secret", [](const auto& s) { return s.spoiler; }), "Spoilers remain hidden until interaction");
    const auto inlineCode = app::layoutMessage("Before `a < b` after `two words`.", 600, editMeasure);
    require(
        inlineCode.boxes.size() == 2 && inlineCode.boxes[0].inlineCode && inlineCode.boxes[1].inlineCode,
        "Each inline code span has one continuous background, including its spaces"
    );
    require(inlineCode.plainText == "Before a < b after two words.\n", "Inline code padding never changes copied text");
    for (const auto& run : inlineCode.runs) {
      require(
          inlineCode.plainText.substr(run.offset, run.text.size()) == run.text, "Inline code retains selection offsets"
      );
      if (run.style.code) {
        require(run.style.size < 14, "Inline monospace text is slightly smaller than surrounding text");
        require(
            std::ranges::any_of(
                inlineCode.boxes,
                [&](const auto& box) { return run.x > box.x && run.x + run.width < box.x + box.width; }
            ),
            "Inline code backgrounds reserve padding on both sides"
        );
      }
    }
    const auto wrappedCode = app::layoutMessage("`~/.cache/noctalia/noctalia.log with spaces`", 95, editMeasure);
    require(wrappedCode.boxes.size() > 1, "Inline code wraps long paths without overflowing");
    for (const auto& box : wrappedCode.boxes)
      require(box.x >= 0 && box.x + box.width <= 95.01F, "Wrapped code padding stays within the column");
    const auto emojiCode = app::layoutMessage("🙂 `a b`", 600, editMeasure, 14, {}, emojiOptions);
    const auto codeRun = std::ranges::find_if(emojiCode.runs, [](const auto& run) { return run.style.code; });
    require(
        codeRun != emojiCode.runs.end() && std::abs(codeRun->y - emojiCode.boxes.front().y - 1) < .01F,
        "Inline code decoration follows emoji baseline alignment"
    );
    auto unmatched = app::layoutMessage("literal __marker and || unfinished", 600, measure);
    require(
        unmatched.plainText.contains("__marker") && unmatched.plainText.contains("||"),
        "Unpaired delimiters remain visible"
    );
    auto escaped = app::messageMarkup("<span>& \"bad\"", {.bold = true});
    require(
        escaped.find("&lt;span&gt;") != std::string::npos && escaped.find("&amp;") != std::string::npos,
        "Untrusted message text cannot inject Pango markup"
    );
    auto code = app::layoutMessage("```cpp\n  if (a < b) {\n    __literal__;\n  }\n```", 240, measure);
    require(
        !code.boxes.empty() && code.boxes.front().code == "  if (a < b) {\n    __literal__;\n  }\n",
        "Code preserves indentation, newlines, and literal delimiters for copying"
    );
    require(
        std::ranges::all_of(code.runs, [](const auto& run) { return run.style.code && !run.style.underline; }),
        "Fenced code uses monospace without interpreting formatting"
    );
    require(
        code.boxes.size() == 1 && !code.boxes.front().inlineCode && code.runs.back().style.size == 14,
        "Fenced code retains its full-sized font and separate block decoration"
    );
    const auto checkFencedBody = [&](std::string_view input, std::string_view body, float width = 240) {
      auto result = app::layoutMessage(input, width, editMeasure);
      if (result.boxes.size() != 1 || result.boxes.front().inlineCode || result.boxes.front().code != body)
        std::cerr
            << "Fence input: "
            << nlohmann::json(input)
            << "\nExpected code: "
            << nlohmann::json(body)
            << "\nActual code: "
            << (result.boxes.empty() ? nlohmann::json(nullptr) : nlohmann::json(result.boxes.front().code))
            << '\n';
      require(
          result.boxes.size() == 1 && !result.boxes.front().inlineCode && result.boxes.front().code == body,
          "Chat fences preserve every original code byte for the code-copy action"
      );
      require(result.plainText.contains(body), "The full fenced body remains available to text selection");
      for (const auto& run : result.runs)
        require(
            result.plainText.substr(run.offset, run.text.size()) == run.text,
            "Fenced text fragments retain UTF-8 byte selection offsets after wrapping"
        );
      return result;
    };
    constexpr std::string_view terminalBody =
        "❯ sudo dnf in libdave-devel                                                     2.4s\n"
        "Updating and loading repositories:\n"
        "Repositories loaded.\n"
        "Failed to resolve the transaction:\n"
        "No match for argument: libdave-devel\n"
        "You can try to add to command line:\n"
        "  --skip-unavailable to skip unavailable packages\n"
        "aditya@adi-fedora ~ [1]\n"
        "❯                                                                               1.2s";
    for (const float width : {240.F, 1200.F}) {
      const auto terminal = checkFencedBody("```" + std::string(terminalBody) + "```", terminalBody, width);
      require(
          terminal.plainText.starts_with("code❯ sudo dnf in libdave-devel")
              && terminal.plainText.ends_with("1.2s\n")
              && !terminal.plainText.contains("```"),
          "Same-line terminal fences retain the command and timings instead of treating them as metadata"
      );
    }
    checkFencedBody("```echo hello```", "echo hello");
    checkFencedBody("__before__ <@1> ```<@1> __literal__ ||secret||``` **after**", "<@1> __literal__ ||secret||");
    checkFencedBody("```sudo dnf in libdave-devel\noutput```", "sudo dnf in libdave-devel\noutput");
    checkFencedBody("```❯ sudo dnf in libdave-devel\noutput", "❯ sudo dnf in libdave-devel\noutput");
    checkFencedBody("```future_lang\noutput", "output\n");
    checkFencedBody("```text output\nfirst\n```", "text output\nfirst\n");
    const auto slashLanguage = checkFencedBody("```text/plain\nfirst\n```", "first\n");
    require(slashLanguage.plainText.starts_with("text/plain"), "Single-word ASCII language labels retain punctuation");
    checkFencedBody("```sh\nprintf '%s' `literal`\nlast```", "printf '%s' `literal`\nlast");
    const auto crlfFence = app::layoutMessage("```\r\nfirst\r\nlast```", 240, editMeasure);
    require(
        crlfFence.boxes.size() == 1
            && crlfFence.boxes.front().code == "first\r\nlast"
            && crlfFence.plainText.contains("first\nlast"),
        "Code copy preserves original CRLF bytes while rendered line endings use plain-text newlines"
    );
    const auto unknownLanguage = checkFencedBody("```future_lang\n**literal**\n```", "**literal**\n");
    require(
        unknownLanguage.plainText.starts_with("future_lang")
            && std::ranges::none_of(unknownLanguage.runs, [](const auto& run) { return run.style.bold; }),
        "Canonical unknown language headers remain headers and code contents remain literal"
    );
    checkFencedBody(
        "````\nconst char* value = \"```\";\n`one` and ``two``\n````",
        "const char* value = \"```\";\n`one` and ``two``\n"
    );
    checkFencedBody("```cpp\nconst char* value = \"```\";\n```", "const char* value = \"```\";\n");
    const auto tildeLiteral = app::layoutMessage("~~~text\n```echo hello```\n~~~", 400, editMeasure);
    require(
        std::ranges::none_of(tildeLiteral.boxes, [](const auto& box) { return !box.inlineCode; })
            && tildeLiteral.plainText.contains("echo hello"),
        "Existing tilde and strike syntax does not create new nested backtick blocks"
    );
    const auto nestedFence =
        app::layoutMessage("> ```future_lang\n> one\n> ```\n\n- ```text\n  two\n  ```", 400, editMeasure);
    require(
        std::ranges::any_of(nestedFence.boxes, [](const auto& box) { return box.code == "one\n"; })
            && std::ranges::any_of(nestedFence.boxes, [](const auto& box) { return box.code == "two\n"; }),
        "Canonical quote and list fences keep parser-owned indentation handling"
    );
    const auto inlineQuoteFence = app::layoutMessage("> ```echo hello``` ", 400, editMeasure);
    require(
        inlineQuoteFence.plainText == "echo hello\n"
            && std::ranges::any_of(inlineQuoteFence.boxes, [](const auto& box) { return box.quote; })
            && std::ranges::any_of(inlineQuoteFence.boxes, [](const auto& box) { return box.inlineCode; })
            && std::ranges::none_of(inlineQuoteFence.boxes, [](const auto& box) { return !box.code.empty(); }),
        "Touching fences inside quotes retain the existing quote container and inline code treatment"
    );
    const auto inlineListFence = app::layoutMessage("- ```echo hello``` ", 400, editMeasure);
    require(
        inlineListFence.plainText == "• echo hello\n"
            && std::ranges::any_of(inlineListFence.boxes, [](const auto& box) { return box.inlineCode; })
            && std::ranges::none_of(inlineListFence.boxes, [](const auto& box) { return !box.code.empty(); }),
        "Touching fences inside lists retain the existing list marker and inline code treatment"
    );
    for (const auto input :
         {"``literal ``` marker``", "\\`\\`\\`echo hello\\`\\`\\`", "    ```echo hello```", "\t```echo hello```"}) {
      const auto literal = app::layoutMessage(input, 600, editMeasure);
      require(
          std::ranges::none_of(literal.boxes, [](const auto& box) { return !box.inlineCode; }),
          "Inline, escaped and indented literal backticks never become a new fenced block"
      );
      require(
          literal.plainText.contains("echo hello") || literal.plainText.contains("literal ``` marker"),
          "Literal backtick content remains readable"
      );
    }
    const auto numericPrefix = app::layoutMessage("123```cpp\ncode\n```", 400, editMeasure);
    require(
        numericPrefix.boxes.size() == 1
            && numericPrefix.boxes.front().code == "code\n"
            && numericPrefix.plainText.starts_with("123\ncppcode"),
        "A numeric prose prefix does not masquerade as a numbered-list fence marker"
    );
    const auto openInline = app::layoutMessage("before ```unfinished", 400, editMeasure);
    require(
        openInline.boxes.empty() && openInline.plainText == "before ```unfinished\n",
        "Unmatched mid-prose backticks remain literal text"
    );
    const auto surrounding = app::layoutMessage(
        "before ```echo hello``` **after** [same](https://example.com/x) [same](https://example.com/x)", 180,
        editMeasure
    );
    if (surrounding.plainText != "before\ncodeecho hello\nafter same same\n")
      std::cerr << "Surrounding text: " << nlohmann::json(surrounding.plainText) << '\n';
    require(
        surrounding.plainText == "before\ncodeecho hello\nafter same same\n"
            && std::ranges::any_of(
                surrounding.runs, [](const auto& run) { return run.text == "after" && run.style.bold; }
            ),
        "Moving chat fences onto parser block boundaries preserves surrounding text and formatting"
    );
    std::vector<std::size_t> followingLinks;
    for (const auto& run : surrounding.runs) {
      require(
          surrounding.plainText.substr(run.offset, run.text.size()) == run.text,
          "Text after a normalized fence retains correct selection offsets"
      );
      if (!run.style.link.empty())
        followingLinks.push_back(run.style.linkId);
    }
    require(
        followingLinks.size() == 2 && followingLinks.front() != followingLinks.back(),
        "Separate links after a chat fence retain independent hover identities"
    );
    const auto multiple = app::layoutMessage("```one``` between ```two words``` after", 300, editMeasure);
    require(
        multiple.boxes.size() == 2
            && multiple.boxes[0].code == "one"
            && multiple.boxes[1].code == "two words"
            && multiple.plainText.contains("between")
            && multiple.plainText.ends_with("after\n"),
        "Multiple same-line fences keep independent bodies and following prose"
    );
    const auto placeholderLanguage =
        app::layoutMessage("```cressida-code\noriginal\n```\n```echo hello```", 300, editMeasure);
    require(
        placeholderLanguage.boxes.size() == 2
            && placeholderLanguage.boxes[0].code == "original\n"
            && placeholderLanguage.boxes[1].code == "echo hello",
        "A user language label cannot collide with private fence-body metadata"
    );
    const auto blankFence = app::layoutMessage("before\n\n```echo hello```\n\nafter", 400, editMeasure);
    const auto canonicalBlankFence = app::layoutMessage("before\n\n```\necho hello\n```\n\nafter", 400, editMeasure);
    require(
        blankFence.plainText == canonicalBlankFence.plainText && blankFence.height == canonicalBlankFence.height,
        "Synthetic fence boundaries do not introduce copied or visible blank lines"
    );
    const auto checkTokens = [&](std::string_view language, std::string_view source,
                                 std::initializer_list<std::pair<std::string_view, app::CodeToken>> expected) {
      const auto spans = app::highlightCode(source, language);
      std::size_t offset = 0;
      for (const auto& span : spans) {
        require(span.offset == offset && span.length > 0, "Syntax ranges cover source bytes without gaps");
        offset += span.length;
      }
      require(offset == source.size(), "Highlighting preserves all original code bytes");
      for (const auto& [text, token] : expected) {
        const auto at = source.find(text);
        require(
            at != std::string_view::npos
                && std::ranges::any_of(
                    spans,
                    [&](const auto& span) {
                      return span.offset <= at && span.offset + span.length >= at + text.size() && span.token == token;
                    }
                ),
            "Language-aware syntax classification"
        );
      }
    };
    using Token = app::CodeToken;
    checkTokens(
        "C++", "#include <vector>\nconstexpr int count = 42;\n/* if \"x\"\ncomment */\nprint(\"<@1> __literal__\");",
        {{"#include", Token::Preprocessor},
         {"constexpr", Token::Keyword},
         {"int", Token::Type},
         {"42", Token::Number},
         {"/* if \"x\"\ncomment */", Token::Comment},
         {"print", Token::Function},
         {"\"<@1> __literal__\"", Token::String}}
    );
    checkTokens(
        "python", "def greet(name):\n  text = \"\"\"first\n# still a string\"\"\"\n  return True # hi\n",
        {{"def", Token::Keyword},
         {"greet", Token::Function},
         {"\"\"\"first\n# still a string\"\"\"", Token::String},
         {"return", Token::Keyword},
         {"True", Token::Number},
         {"# hi", Token::Comment}}
    );
    checkTokens(
        "js", "const value = `one\ntwo`; // ignored return\nrun(3.5e-2);",
        {{"const", Token::Keyword},
         {"`one\ntwo`", Token::String},
         {"// ignored return", Token::Comment},
         {"3.5e-2", Token::Number}}
    );
    checkTokens(
        "json", "{\"key\": \"value\", \"enabled\": true}",
        {{"\"key\"", Token::Property}, {"\"value\"", Token::String}, {"true", Token::Number}}
    );
    checkTokens(
        "bash", "# comment\nif test $value; then echo \"yes\"; fi",
        {{"# comment", Token::Comment}, {"if", Token::Keyword}, {"$value", Token::Property}, {"echo", Token::Keyword}}
    );
    checkTokens(
        "rust", "fn test<'a>(x: &'a str) { /* outer /* inner */ done */ }",
        {{"fn", Token::Keyword}, {"'a", Token::Property}, {"/* outer /* inner */ done */", Token::Comment}}
    );
    checkTokens("rust", "let value = 'ß'; return value;", {{"'ß'", Token::String}, {"return", Token::Keyword}});
    checkTokens(
        "cpp", "R\"tag(a \"quote\"\n// still string)tag\"",
        {{"R\"tag(a \"quote\"\n// still string)tag\"", Token::String}}
    );
    checkTokens("diff", "+added\n-removed\n@@ context\n", {{"+added", Token::Added}, {"-removed", Token::Removed}});
    const std::string nixSource = R"NIX({
  description = "My NixOS config";
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    home-manager = {
      url = "github:nix-community/home-manager";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };
  enabled = true;
  # Keep this comment out of the string.
  value' = let count = 42; in count;
  text = ''
    echo ''${PATH}
    echo ${if enabled then "yes" else "no"}
  '';
  path = ./${name}/config.nix;
})NIX";
    for (const auto language : {"nix", " NIX ", "nixos"})
      checkTokens(
          language, nixSource,
          {{"description", Token::Property},
           {"\"My NixOS config\"", Token::String},
           {"home-manager", Token::Property},
           {"follows", Token::Property},
           {"true", Token::Number},
           {"# Keep this comment out of the string.", Token::Comment},
           {"value'", Token::Property},
           {"let", Token::Keyword},
           {"42", Token::Number},
           {"''${PATH}", Token::String},
           {"if", Token::Keyword},
           {"then", Token::Keyword},
           {"\"yes\"", Token::String},
           {"config.nix", Token::String}}
      );
    checkTokens(
        "nix", R"NIX("before ${if true then "${({ a = "ok"; }).a}" else "no"} after"; rec {})NIX",
        {{"if", Token::Keyword},
         {"then", Token::Keyword},
         {"\"ok\"", Token::String},
         {"else", Token::Keyword},
         {" after\"", Token::String},
         {"rec", Token::Keyword}}
    );
    checkTokens(
        "nix", R"NIX("\${literal} $${make}"; ''
      ''' quote ''\n newline ''${literal} $${make}
    ''; /* multiline
    comment */ with builtins; import <nixpkgs> {})NIX",
        {{"\\${literal}", Token::String},
         {"$${make}", Token::String},
         {"''' quote", Token::String},
         {"''\\n newline", Token::String},
         {"/* multiline\n    comment */", Token::Comment},
         {"with", Token::Keyword},
         {"builtins", Token::Function},
         {"<nixpkgs>", Token::String}}
    );
    for (const auto malformed : {"\"unterminated ${if true then", "'' unfinished ''${escaped}", "/* unfinished"})
      checkTokens("nix", malformed, {});
    for (const auto language : {"mjs", "cjs", "mts", "cts", "javascript", "TS"})
      checkTokens(language, "const value = 1;", {{"const", Token::Keyword}});
    checkTokens(
        "json5", "// comment\n{key: 'value'}",
        {{"// comment", Token::Comment}, {"key", Token::Property}, {"'value'", Token::String}}
    );
    checkTokens("shellscript", "if true; then echo yes; fi", {{"then", Token::Keyword}});
    const auto nixLayout = app::layoutMessage("```nix\n" + nixSource + "\n```", 200, editMeasure);
    require(nixLayout.boxes.front().code == nixSource + "\n", "Nix Copy retains source and multiline escapes exactly");
    for (const auto& run : nixLayout.runs)
      require(
          nixLayout.plainText.substr(run.offset, run.text.size()) == run.text,
          "Nix interpolation and wrapping retain selection offsets"
      );
    for (const auto language : {"", "text", "unsupported-language"})
      checkTokens(language, "if True: print(123)", {{"if True: print(123)", Token::Plain}});
    const auto highlighted =
        app::layoutMessage("```cpp\n  const int count = 42; // hello\n```\n`const int`", 150, editMeasure);
    require(
        highlighted.boxes.front().code == "  const int count = 42; // hello\n",
        "Highlighted code Copy retains exact indentation and newlines"
    );
    require(
        std::ranges::any_of(highlighted.runs, [](const auto& run) { return run.style.syntax == Token::Keyword; }),
        "Fenced language reaches rich text styles"
    );
    require(
        highlighted.runs.back().style.syntax == Token::Plain, "Inline code remains uncolored after a highlighted fence"
    );
    for (const auto& run : highlighted.runs)
      require(
          highlighted.plainText.substr(run.offset, run.text.size()) == run.text,
          "Highlighting and wrapping retain selection byte offsets"
      );
    const auto plainAgain = app::layoutMessage("```cpp\nreturn 1;\n```\n```text\nreturn 2;\n```", 400, editMeasure);
    require(plainAgain.runs.back().style.syntax == Token::Plain, "Syntax state never leaks between code fences");
    auto link = app::layoutMessage("[docs](https://example.com)\n> quote\n- list item", 300, measure);
    require(link.runs.front().style.link == "https://example.com", "Masked links retain their actual URL");
    require(
        std::ranges::any_of(link.boxes, [](const auto& box) { return box.quote; }), "Blockquotes have a native rule"
    );
    api::Message rich;
    rich.content = "**An image**";
    rich.attachments.push_back(
        {"1",
         "photo.png",
         "https://cdn.discordapp.com/attachments/1/2/photo.png",
         "image/png",
         {"https://cdn.discordapp.com/attachments/1/2/photo.png", 1920, 1080}}
    );
    api::Embed embed;
    embed.title = "Preview";
    embed.description = "**Formatted** description";
    embed.fields.push_back({"Field", "value"});
    rich.embeds.push_back(embed);
    auto richLayout = app::layoutRichMessage(rich, 400, [&](std::string_view text, const app::MessageStyle&) {
      return measure(text);
    });
    require(
        richLayout.pictures.size() == 1
            && richLayout.pictures.front().width <= 400
            && richLayout.pictures.front().height <= 300,
        "Image previews preserve aspect ratio within the message column"
    );
    require(
        std::ranges::any_of(richLayout.boxes, [](const auto& box) { return box.embed; })
            && richLayout.plainText.contains("Preview")
            && richLayout.plainText.contains("value"),
        "Embeds lay out titles, descriptions, and fields"
    );
    for (const auto& run : richLayout.runs)
      require(
          richLayout.plainText.substr(run.offset, run.text.size()) == run.text,
          "Selection offsets refer to rendered plain text"
      );
    api::Message video;
    api::Message attachedVideo;
    const std::string videoUrl = "https://cdn.discordapp.com/attachments/1/2/clip.mp4?ex=a&hm=b";
    attachedVideo.attachments.push_back({"2", "clip.mp4", videoUrl, "video/mp4", {videoUrl, 1920, 1080}});
    const auto attachedLayout =
        app::layoutRichMessage(attachedVideo, 400, [&](std::string_view text, const app::MessageStyle&) {
          return measure(text);
        });
    require(
        attachedLayout.pictures.size() == 1
            && attachedLayout.pictures[0].videoSource == videoUrl
            && !attachedLayout.pictures[0].animated
            && attachedLayout.pictures[0].media.width == 1920,
        "Video attachments retain the original signed source and use the native player, never GIF playback"
    );
    attachedVideo.attachments[0].contentType.clear();
    attachedVideo.attachments[0].image = {};
    const auto missingMetadata =
        app::layoutRichMessage(attachedVideo, 400, [&](std::string_view text, const app::MessageStyle&) {
          return measure(text);
        });
    require(
        missingMetadata.pictures.size() == 1 && missingMetadata.pictures[0].videoSource == videoUrl,
        "Known video extensions remain playable without optional MIME/thumbnail dimensions"
    );
    api::Message animated;
    api::Embed gif;
    gif.type = "gifv";
    gif.url = "https://klipy.com/gifs/example";
    gif.video = {"https://static.klipy.com/ii/example.mp4", 320, 180};
    gif.thumbnail = {"https://static.klipy.com/ii/still.png", 320, 180};
    animated.embeds.push_back(gif);
    animated.content = gif.url;
    const auto animationLayout =
        app::layoutRichMessage(animated, 400, [&](std::string_view text, const app::MessageStyle&) {
          return measure(text);
        });
    require(
        animationLayout.pictures.size() == 1
            && !animationLayout.pictures[0].video
            && animationLayout.pictures[0].videoSource.empty()
            && animationLayout.pictures[0].animated
            && animationLayout.pictures[0].media.url == gif.video.url
            && animationLayout.boxes.empty(),
        "GIF videos use their looping media, without a video link card or play overlay"
    );
    require(
        animationLayout.runs.empty() && animationLayout.plainText.empty(),
        "A standalone GIF link disappears when the media preview is rendered"
    );
    auto mixed = animated;
    auto masked = animated;
    masked.content = "[woe](" + gif.url + ")";
    require(
        app::layoutRichMessage(
            masked, 400, [&](std::string_view text, const app::MessageStyle&) { return measure(text); }
        ).runs.empty(),
        "Hide a masked media-only link without a duplicate label"
    );
    mixed.content = "Look at this " + gif.url;
    auto mixedLayout = app::layoutRichMessage(mixed, 400, [&](std::string_view text, const app::MessageStyle&) {
      return measure(text);
    });
    require(
        mixedLayout.plainText.contains("Look at this") && mixedLayout.plainText.contains(gif.url),
        "Keep captions and mixed message text"
    );
    auto hidden = animated;
    hidden.embeds.clear();
    require(
        !app::layoutRichMessage(
             hidden, 400, [&](std::string_view text, const app::MessageStyle&) { return measure(text); }
        ).runs.empty(),
        "Hidden or missing GIF previews retain their source link"
    );
    auto image = animated;
    image.embeds.front().type = "image";
    image.embeds.front().video = {};
    image.embeds.front().image = {};
    const auto imageLayout = app::layoutRichMessage(image, 400, [&](std::string_view text, const app::MessageStyle&) {
      return measure(text);
    });
    require(
        imageLayout.pictures.size() == 1
            && imageLayout.boxes.empty()
            && imageLayout.runs.empty()
            && imageLayout.pictures[0].media.url == gif.thumbnail.url,
        "Image embeds with thumbnail media display as an unframed image, not a link card"
    );
    api::Message fake;
    const std::string fakeUrl = "https://cdn.discordapp.com/emojis/123456789.gif?size=48&name=wave&lossless=true";
    fake.content = "[wave](" + fakeUrl + ")";
    auto fakeLayout = app::layoutRichMessage(
        fake, 400, [&](std::string_view text, const app::MessageStyle&) { return measure(text); }, 14,
        {.emojiImageLinks = true}
    );
    require(
        fakeLayout.runs.size() == 1 && !fakeLayout.runs[0].image.empty() && fakeLayout.pictures.empty(),
        "Standalone emoji image links render inline without requiring a server embed"
    );
    api::Embed duplicate;
    duplicate.type = "image";
    duplicate.url = fakeUrl;
    duplicate.thumbnail = {fakeUrl, 48, 48};
    fake.embeds.push_back(duplicate);
    fakeLayout = app::layoutRichMessage(
        fake, 400, [&](std::string_view text, const app::MessageStyle&) { return measure(text); }, 14,
        {.emojiImageLinks = true}
    );
    require(fakeLayout.runs.size() == 1 && fakeLayout.pictures.empty(), "Suppress duplicate emoji image previews");
    fake.content = "`" + fake.content + "`";
    fake.embeds.clear();
    fakeLayout = app::layoutRichMessage(
        fake, 400, [&](std::string_view text, const app::MessageStyle&) { return measure(text); }, 14,
        {.emojiImageLinks = true}
    );
    require(
        std::ranges::none_of(fakeLayout.runs, [](const auto& run) { return !run.image.empty(); }),
        "Code containing emoji CDN links remains literal"
    );
    api::Embed youtube;
    youtube.type = "video";
    youtube.provider = "YouTube";
    youtube.title = "A video";
    youtube.description = "Long descriptions should not replace the video thumbnail.";
    youtube.thumbnail = {
        "https://images-ext-1.discordapp.net/external/hash/https/i.ytimg.com/vi/test/hqdefault.jpg", 1280, 720
    };
    video.embeds.push_back(youtube);
    auto videoLayout = app::layoutRichMessage(video, 400, [&](std::string_view text, const app::MessageStyle&) {
      return measure(text);
    });
    require(
        videoLayout.pictures.size() == 1
            && videoLayout.pictures[0].video
            && videoLayout.pictures[0].width > 300
            && !videoLayout.plainText.contains("Long descriptions"),
        "YouTube embeds use a full-width video thumbnail and play action"
    );
    auto larger = app::layoutRichMessage(
        rich, 400,
        [&](std::string_view text, const app::MessageStyle& style) { return measure(text) * style.size / 14; }, 20
    );
    require(
        larger.height > richLayout.height && larger.runs.front().style.size == 20,
        "Message font settings affect both measured wrapping and drawn glyph sizes"
    );
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

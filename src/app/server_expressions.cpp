#include "app/client_application.h"
#include "app/theme.h"
#include "render/core/texture_manager.h"
#include "ui/controls/input.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace app {
  using namespace theme;
  using Json = nlohmann::json;
  using Section = api::GuildSettingsSection;
  using Verb = api::GuildSettingsVerb;
  namespace {
    std::string text(const Json& object, const char* key, std::string fallback = {}) {
      const auto at = object.find(key);
      return at != object.end() && at->is_string() ? at->get<std::string>() : fallback;
    }
    std::string identity(const Json& record) { return text(record, "id", text(record, "sound_id")); }
    std::string folded(std::string value) {
      std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return value;
    }
    std::string creatorName(const Json& record) {
      if (record.contains("user") && record["user"].is_object())
        return text(record["user"], "global_name", text(record["user"], "username", "Unknown member"));
      return "Unknown member";
    }
    bool feature(const Json& guild, std::string_view name) {
      const auto at = guild.find("features");
      return at != guild.end() && at->is_array() && std::ranges::any_of(*at, [&](const auto& value) {
               return value.is_string() && value.template get<std::string>() == name;
             });
    }
    unsigned slots(const Json& guild, Section section) {
      // Current Discord client modules 307731/202541/473145 define the base,
      // legacy tier, extra-slot and feature-floor values independently.
      const auto tier = std::clamp(guild.value("premium_tier", 0), 0, 3);
      const auto features = guild.value("premium_features", Json::object());
      const char* key = section == Section::Emoji ? "additional_emoji_slots"
          : section == Section::Stickers          ? "additional_sticker_slots"
                                                  : "additional_sound_slots";
      const unsigned base = section == Section::Emoji ? 50 : section == Section::Stickers ? 5 : 8;
      constexpr std::array<unsigned, 4> emoji{50, 100, 150, 250}, stickers{5, 15, 30, 60}, sounds{8, 24, 36, 48};
      unsigned limit = section == Section::Emoji ? emoji[tier]
          : section == Section::Stickers         ? stickers[tier]
                                                 : sounds[tier];
      if (features.is_object() && features.contains(key) && features[key].is_number_unsigned())
        limit = base + std::min(10000u, features[key].get<unsigned>());
      else if (
          features.is_object()
          && features.contains(key)
          && features[key].is_number_integer()
          && features[key].get<std::int64_t>() >= 0
      )
        limit = base + std::min<std::int64_t>(10000, features[key].get<std::int64_t>());
      if (section == Section::Emoji && feature(guild, "MORE_EMOJI"))
        limit = std::max(limit, 200u);
      if (section == Section::Soundboard && feature(guild, "MORE_SOUNDBOARD"))
        limit = std::max(limit, 200u);
      if (section == Section::Stickers && tier == 3 && feature(guild, "MORE_STICKERS"))
        limit = std::max(limit, 120u);
      return limit;
    }
  } // namespace
  void ClientApplication::stageServerEmojiUploads(std::vector<PickedFile> files) {
    auto& state = m_serverSettings;
    if (!state.open || state.page != "emoji" || state.saving || state.mfaRequest)
      return;
    std::string failures;
    for (auto& file : files) {
      if (!file.error.empty() || file.bytes.empty() || file.bytes.size() > 256 * 1024) {
        if (!failures.empty())
          failures += " ";
        failures += file.name + ": " + (file.error.empty() ? "Choose an image up to 256 KiB." : file.error);
        continue;
      }
      if (state.emojiUploads.size() >= 50)
        break;
      const auto id = std::to_string(++state.serial);
      auto name = file.name;
      if (const auto dot = name.rfind('.'); dot != std::string::npos)
        name.erase(dot);
      std::ranges::transform(name, name.begin(), [](unsigned char c) {
        return static_cast<char>(std::isalnum(c) && c < 128 ? c : '_');
      });
      if (name.size() > 32)
        name.resize(32);
      if (name.size() < 2)
        name = "emoji_" + id;
      constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string encoded;
      encoded.reserve((file.bytes.size() + 2) / 3 * 4);
      for (std::size_t i = 0; i < file.bytes.size(); i += 3) {
        const unsigned a = file.bytes[i], b = i + 1 < file.bytes.size() ? file.bytes[i + 1] : 0,
                       c = i + 2 < file.bytes.size() ? file.bytes[i + 2] : 0;
        encoded += alphabet[a >> 2];
        encoded += alphabet[((a & 3) << 4) | (b >> 4)];
        encoded += i + 1 < file.bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        encoded += i + 2 < file.bytes.size() ? alphabet[c & 63] : '=';
      }
      state.emojiUploads.push_back(
          {{"id", id},
           {"name", name},
           {"filename", file.name},
           {"image", "data:" + file.mime + ";base64," + encoded},
           {"animated", file.mime == "image/gif"}}
      );
      if (file.raster) {
        const auto& raster = *file.raster;
        replaceCachedImageTexture(
            m_images["server-emoji-upload:" + id], m_window.context().textureManager(),
            m_window.context().textureManager().loadFromRgba(raster.pixels.data(), raster.width, raster.height)
        );
      }
    }
    state.creating = false;
    state.resource.clear();
    state.record = Json::object();
    state.draft.clear();
    state.error = std::move(failures);
    state.status.clear();
    ++state.generation;
    m_scrolling.stop(state.scroll);
    state.scroll = 0;
    m_window.invalidate();
  }
  bool ClientApplication::drawServerExpressions(ServerSettingsForm& form) {
    auto& state = m_serverSettings;
    if ((state.page != "emoji" && state.page != "stickers" && state.page != "soundboard")
        || state.creating
        || !state.resource.empty())
      return false;
    auto& p = form.node;
    auto& cursor = form.y;
    const auto width = form.width;
    const auto section = state.page == "emoji" ? Section::Emoji
        : state.page == "stickers"             ? Section::Stickers
                                               : Section::Soundboard;
    const auto limit = slots(state.overview, section);
    const auto list = state.lists.contains(state.page) && state.lists[state.page].is_array() ? state.lists[state.page]
                                                                                             : Json::array();
    const auto staticCount = std::ranges::count_if(list, [](const auto& record) {
      return !record.value("animated", false) && !record.value("managed", false);
    });
    const auto animatedCount = std::ranges::count_if(list, [](const auto& record) {
      return record.value("animated", false) && !record.value("managed", false);
    });
    const auto remaining = [&](std::size_t count) { return count < limit ? limit - count : 0; };
    const bool full = section == Section::Emoji ? staticCount >= limit && animatedCount >= limit : list.size() >= limit;
    const auto allowed = [&](Verb verb, const std::string& resource = {}) {
      api::GuildSettingsRequest request;
      request.guildId = state.guild;
      request.section = section;
      request.verb = verb;
      request.resourceId = resource;
      return api::canManageGuildSettings(request, serverSettingsAccess(resource));
    };
    const auto button = [&](float x, float y, float w, const std::string& caption, const std::string& action,
                            bool enabled = true, ButtonVariant variant = ButtonVariant::Default) {
      form.buttonControl(p, x, y, w, caption, action, variant, enabled);
    };
    form.section(state.page == "emoji" ? "Server emoji" : state.page == "stickers" ? "Server stickers" : "Soundboard");
    form.detail(
        state.page == "emoji"          ? "Emoji for your server"
            : state.page == "stickers" ? "Stickers for your server"
                                       : "Sounds for your voice channels",
        state.page == "emoji" ? "Upload PNG, JPEG, WebP or GIF images up to 256 KiB. Members can use these emoji in "
                                "messages and reactions."
            : state.page == "stickers"
            ? "Upload 320×320 PNG, APNG or GIF stickers up to 512 KiB. Verified and partnered servers can also use "
              "Lottie stickers."
            : "Upload an Ogg or MP3 sound up to 5.2 seconds long and 512 KiB for members to play in voice channels."
    );
    form.row(
        "Available slots",
        section == Section::Emoji ? std::to_string(remaining(staticCount))
                + " emoji · "
                + std::to_string(remaining(animatedCount))
                + " animated emoji"
                                  : std::to_string(remaining(list.size()))
                + " of "
                + std::to_string(limit)
                + " "
                + (section == Section::Stickers ? "sticker" : "sound")
                + " slots available"
    );
    button(
        0, cursor, 136,
        state.page == "emoji"          ? "Upload emoji"
            : state.page == "stickers" ? "Upload sticker"
                                       : "Upload sound",
        "new", allowed(Verb::Create) && !full, ButtonVariant::Primary
    );
    cursor += Style::spaceSection;
    if (state.page == "emoji" && !state.emojiUploads.empty()) {
      form.section("Ready to upload — " + std::to_string(state.emojiUploads.size()));
      for (const auto& file : state.emojiUploads) {
        const auto id = text(file, "id");
        picture(p, 0, cursor + (Style::spaceXxs + Style::borderWidth), 32, 32, "server-emoji-upload:" + id, 4);
        auto& alias = inputControl(
            p, 44, cursor, std::max(50.0F, width - 84), 30, "server-settings:emoji-upload:" + id, text(file, "name"),
            "Emoji name"
        );
        alias.setFontSize(Style::fontSizeDescription);
        alias.setEnabled(!state.saving && !state.mfaRequest);
        const auto name = text(file, "name");
        alias.setInvalid(name.size() < 2 || name.size() > 32 || !std::ranges::all_of(name, [](unsigned char c) {
                           return c < 128 && (std::isalnum(c) || c == '_');
                         }));
        alias.setOnChange([this, id](const std::string& name) {
          for (auto& file : m_serverSettings.emojiUploads)
            if (text(file, "id") == id)
              file["name"] = name;
          m_window.invalidate();
        });
        alias.setOnSubmit([](const std::string&) {});
        if (!state.saving && !state.mfaRequest)
          iconButton(
              p,
              {width - 28, cursor + Style::spaceXxs, 26, 26, Action::ServerSettingsAction, "emoji-upload-remove:" + id},
              "x", "Remove upload"
          );
        cursor += (Style::space2Xl + Style::spaceXs);
        cursor +=
            label(p, 44, cursor, width - 44, text(file, "filename"), Style::fontSizeMini, muted, FontWeight::Normal, 0)
            + 12;
      }
      form.section("Current emoji");
    }
    auto& search = inputControl(
        p, 0, cursor, width, 32, "server-settings:expressions-search", state.search,
        state.page == "emoji" ? "Search emoji" : "Search by name"
    );
    search.setFontSize(Style::fontSizeDescription);
    search.setOnChange([this](const std::string& value) {
      m_serverSettings.search = value;
      m_window.invalidate();
    });
    search.setOnSubmit([](const std::string&) {});
    cursor += (Style::spaceSection + Style::spaceXs);
    const auto found = state.lists.find(state.page);
    std::vector<Json> records;
    if (found != state.lists.end() && found->second.is_array())
      for (const auto& record : found->second)
        if (record.is_object() && (state.search.empty() || folded(text(record, "name")).contains(folded(state.search))))
          records.push_back(record);
    if (state.page == "emoji") {
      for (const bool animated : {false, true}) {
        std::vector<Json> group;
        for (const auto& record : records)
          if (record.value("animated", false) == animated)
            group.push_back(record);
        form.section(
            std::string(animated ? "Animated emoji" : "Emoji")
            + " — "
            + std::to_string(animated ? animatedCount : staticCount)
            + " / "
            + std::to_string(limit)
        );
        if (group.empty()) {
          form.row(
              state.search.empty() ? (animated ? "No animated emoji yet" : "No emoji yet") : "No matching emoji",
              state.search.empty() ? "Upload an emoji to add it to this server." : "Try another emoji name."
          );
          continue;
        }
        if (width >= 420) {
          label(p, 50, cursor, width - 344, "Name", Style::fontSizeMini, muted, FontWeight::Medium);
          label(p, width - 282, cursor, 150, "Uploaded by", Style::fontSizeMini, muted, FontWeight::Medium);
          cursor += Style::spaceXl;
        }
        for (const auto& record : group) {
          const auto id = identity(record), original = text(record, "name");
          const auto top = cursor;
          const bool compact = width < 420;
          const auto rowHeight = compact ? 94.0F : 58.0F;
          const auto name =
              state.expressionDrafts.contains(id) ? state.expressionDrafts[id].value("name", original) : original;
          const auto image = api::guildExpressionAsset(section, record);
          if (!picture(p, 0, top + (Style::spaceCompact + Style::borderWidth), 36, 36, image, 4))
            label(p, 3, top + Style::spaceCompact, 34, text(record, "demo_emoji", "🙂"), 27, foreground);
          const auto nameWidth = std::max(50.0F, compact ? width - 90 : width - 344);
          auto& input = inputControl(
              p, 50, top + Style::spaceDense, nameWidth, 30, "server-settings:emoji-alias:" + id, name, "Emoji name"
          );
          input.setFontSize(Style::fontSizeDescription);
          input.setEnabled(allowed(Verb::Update, id) && !state.saving && !state.mfaRequest);
          input.setFrameRadius(5);
          input.setInvalid(name.size() < 2 || name.size() > 32 || !std::ranges::all_of(name, [](unsigned char c) {
                             return std::isalnum(c) || c == '_';
                           }));
          input.setOnChange([this, id, original](const std::string& value) {
            auto& state = m_serverSettings;
            if (state.saving || state.mfaRequest)
              return;
            if (value == original)
              state.expressionDrafts.erase(id);
            else
              state.expressionDrafts[id] = {{"name", value}};
            state.error.clear();
            state.status.clear();
            m_window.invalidate();
          });
          input.setOnSubmit([](const std::string&) {});
          label(
              p, compact ? 50 : width - 282,
              compact ? top + (Style::spaceGroup - Style::spaceXxs) : top + (Style::spaceLg + Style::spaceXxs),
              compact ? width - 150 : 150, creatorName(record), Style::fontSizeMini, muted
          );
          const auto roleCount = record.contains("roles") && record["roles"].is_array() ? record["roles"].size() : 0;
          button(
              compact ? 50 : width - 118, compact ? top + 66 : top + Style::spaceMd, 80,
              roleCount ? "Roles (" + std::to_string(roleCount) + ")" : "Roles", "record:" + id,
              allowed(Verb::Update, id)
          );
          if (allowed(Verb::Delete, id))
            iconButton(
                p, {width - 28, top + Style::spaceMd, 26, 26, Action::ServerSettingsAction, "expression-delete:" + id},
                "trash", "Delete emoji"
            );
          cursor += rowHeight;
        }
      }
    } else if (state.page == "stickers") {
      form.section("Stickers — " + std::to_string(list.size()) + " / " + std::to_string(limit));
      const int columns = std::max(1, static_cast<int>(width / 214));
      const float gap = 14, tileWidth = (width - gap * (columns - 1)) / columns, tileHeight = 252, start = cursor;
      for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& record = records[i];
        const auto id = identity(record);
        const float x = (i % columns) * (tileWidth + gap), y = start + (i / columns) * (tileHeight + gap);
        rectangle(p, x, y, tileWidth, tileHeight, panel, Style::hoverRadius);
        const auto imageSize = std::min(138.0F, tileWidth - Style::spaceWide);
        const auto image = api::guildExpressionAsset(section, record);
        if (!picture(
                p, x + (tileWidth - imageSize) / 2, y + Style::spaceMd, imageSize, imageSize, image,
                Style::controlRadius, true, false, ImageFitMode::Contain
            ))
          label(
              p, x + (tileWidth - 64) / 2, y + ((Style::spaceSection + Style::spaceXs) + Style::borderWidth), 64,
              text(record, "demo_emoji", "👋"), 52, foreground
          );
        label(
            p, x + Style::spaceMd, y + 158, tileWidth - Style::spaceXl, text(record, "name"), Style::fontSizeCaption,
            foreground, FontWeight::SemiBold
        );
        label(
            p, x + Style::spaceMd, y + 181, tileWidth - Style::spaceXl, creatorName(record), Style::fontSizeMini, muted
        );
        button(
            x + Style::spaceMd, y + 215, std::max(60.0F, tileWidth - 58), "Edit", "record:" + id,
            allowed(Verb::Update, id)
        );
        if (allowed(Verb::Delete, id))
          iconButton(
              p,
              {x + tileWidth - (Style::spaceSection - Style::spaceXxs), y + 215, 26, 26, Action::ServerSettingsAction,
               "expression-delete:" + id},
              "trash", "Delete sticker"
          );
      }
      cursor = start + std::ceil(static_cast<float>(records.size()) / columns) * (tileHeight + gap);
      if (records.empty())
        form.row(
            state.search.empty() ? "No stickers yet" : "No matching stickers",
            state.search.empty() ? "Upload a sticker to add it to this server." : "Try another name."
        );
    } else {
      form.section("Sounds — " + std::to_string(list.size()) + " / " + std::to_string(limit));
      for (const auto& record : records) {
        const auto id = identity(record), emoji = text(record, "emoji_name");
        const auto top = cursor, height = width < 420 ? 122.0F : 88.0F;
        rectangle(p, -Style::spaceXs, top, width + Style::spaceSm, height - Style::spaceSm, panel, Style::hoverRadius);
        if (!emoji.empty())
          label(p, 8, top + Style::cardPadding, 40, emoji, Style::fontSizeDisplay, foreground);
        else if (!text(record, "emoji_id").empty())
          picture(p, 8, top + Style::cardPadding, 32, 32, api::emojiPath(text(record, "emoji_id")), 4);
        else
          glyph(p, 10, top + (Style::spaceLg + Style::spaceXxs), 26, "volume", muted);
        label(
            p, 56, top + Style::spaceMd, width - 68, text(record, "name"), Style::fontSizeCaption, foreground,
            FontWeight::SemiBold
        );
        label(
            p, 56, top + (Style::space2Xl + Style::spaceXxs), width - 68,
            creatorName(record) + " · " + std::to_string(static_cast<int>(record.value("volume", 1.0) * 100)) + "%",
            Style::fontSizeMini, muted
        );
        const auto by = top + (width < 420 ? 82 : 52);
        button(
            56, by, 78, m_soundPreviewSource == id ? "Stop" : "Preview",
            m_soundPreviewSource == id ? "sound-stop" : "sound-preview:" + id
        );
        button(146, by, 70, "Edit", "record:" + id, allowed(Verb::Update, id));
        if (allowed(Verb::Delete, id))
          iconButton(
              p, {width - 28, by, 26, 26, Action::ServerSettingsAction, "expression-delete:" + id}, "trash",
              "Delete sound"
          );
        cursor += height;
      }
      if (records.empty())
        form.row(
            state.search.empty() ? "No sounds yet" : "No matching sounds",
            state.search.empty() ? "Upload a sound to add it to the soundboard." : "Try another name."
        );
    }
    return true;
  }
} // namespace app

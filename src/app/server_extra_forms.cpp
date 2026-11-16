#include "app/client_application.h"
#include "app/server_recurrence.h"
#include "app/theme.h"
#include "ui/controls/input.h"
#include "ui/controls/slider.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace app {
  using Json = nlohmann::json;
  using Section = api::GuildSettingsSection;
  using Verb = api::GuildSettingsVerb;
  using namespace theme;
  namespace {
    std::string text(const Json& value, const char* key, std::string fallback = {}) {
      const auto at = value.find(key);
      return at != value.end() && at->is_string() ? at->get<std::string>() : fallback;
    }
    std::size_t index(const std::string& value) {
      std::size_t result = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
      return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? result : SIZE_MAX;
    }
    std::string date(const Json& value) {
      if (!value.is_string())
        return "Not available";
      if (const auto instant = api::muteDeadline(value.get<std::string>())) {
        auto epoch = std::chrono::system_clock::to_time_t(*instant);
        std::tm local{};
        localtime_r(&epoch, &local);
        char out[64];
        std::strftime(out, sizeof(out), "%d %b %Y, %H:%M", &local);
        return out;
      }
      return "Not available";
    }
    Json cleanRecurrence(const Json& rule) {
      Json result = Json::object();
      if (rule.is_object())
        for (const auto key :
             {"start", "frequency", "interval", "by_weekday", "by_n_weekday", "by_month", "by_month_day"})
          if (rule.contains(key))
            result[key] = rule[key];
      return result;
    }
  } // namespace

  bool ClientApplication::serverExtraAction(const std::string& action) {
    auto& state = m_serverSettings;
    if (action == "save" && state.page == "guide" && state.draft.empty() && !state.guideIconDrafts.empty()) {
      if (state.saving || state.mfaRequest)
        return true;
      const auto first = state.guideIconDrafts.begin();
      const auto key = first.key();
      const bool task = key.starts_with("guide-action:");
      const auto channel = key.substr(task ? 13 : 15);
      const auto records = state.record.value(task ? "new_member_actions" : "resource_channels", Json::array());
      if (std::ranges::none_of(records, [&](const auto& record) { return text(record, "channel_id") == channel; })) {
        state.guideIconDrafts.erase(key);
        state.error = "This guide item was removed. Save the guide before choosing its icon again.";
        m_window.invalidate();
        return true;
      }
      api::GuildSettingsRequest request;
      request.section = task ? Section::GuideAction : Section::GuideResource;
      request.verb = Verb::Update;
      request.resourceId = channel;
      request.values = first.value();
      requestServerSettings(request);
      return true;
    }
    if (action.starts_with("guide-icon-clear:")) {
      const auto field = action.substr(17);
      if (field.starts_with("guide-action:") || field.starts_with("guide-resource:")) {
        state.guideIconDrafts[field] = {{"icon", nullptr}};
        m_window.invalidate();
      }
      return true;
    }
    if (action.starts_with("widget-copy:")) {
      const auto kind = action.substr(12);
      const auto style = state.lists.contains("widget-style") && state.lists["widget-style"].is_string()
          ? state.lists["widget-style"].get<std::string>()
          : "shield";
      if (kind == "html")
        m_window.copy(
            "<iframe src=\"https://discord.com/widget?id="
            + state.guild
            + "&theme="
            + (lightTheme ? "light" : "dark")
            + "\" width=\"350\" height=\"500\" allowtransparency=\"true\" frameborder=\"0\" sandbox=\"allow-popups "
              "allow-popups-to-escape-sandbox allow-same-origin allow-scripts\"></iframe>"
        );
      else if (kind == "image")
        m_window.copy("https://discord.com/api/guilds/" + state.guild + "/widget.png?style=" + style);
      else
        m_window.copy("https://discord.com/api/guilds/" + state.guild + "/widget.json");
      state.status = "Widget code copied.";
      m_window.invalidate();
      return true;
    }
    if (action.starts_with("discovery-category:")) {
      const auto id = action.substr(19);
      const auto selected = state.record.value("secondary_category_ids", Json::array());
      api::GuildSettingsRequest request;
      request.section = Section::DiscoveryCategory;
      request.resourceId = id;
      request.verb = std::ranges::find_if(selected, [&](const auto& item) { return item.dump() == id || item == id; })
              == selected.end()
          ? Verb::Create
          : Verb::Delete;
      requestServerSettings(request);
      return true;
    }
    if (action == "discovery-reason-add" || action.starts_with("discovery-reason-remove:")) {
      auto reasons = state.draft.value("reasons_to_join", state.record.value("reasons_to_join", Json::array()));
      if (action == "discovery-reason-add") {
        if (reasons.size() < 4)
          reasons.push_back({{"reason", ""}, {"emoji_id", nullptr}, {"emoji_name", "✨"}});
      } else {
        const auto at = index(action.substr(24));
        if (at < reasons.size())
          reasons.erase(reasons.begin() + at);
      }
      setServerSettingsValue("reasons_to_join", reasons);
      return true;
    }
    if (action.starts_with("prompt-move:")
        || action.starts_with("option-move:")
        || action.starts_with("welcome-move:")) {
      const auto first = action.find(':');
      const auto second = action.find(':', first + 1);
      const auto direction = action.substr(first + 1, second - first - 1);
      const auto third = action.find(':', second + 1);
      const auto at =
          index(action.substr(second + 1, third == std::string::npos ? std::string::npos : third - second - 1));
      const auto key = action.starts_with("welcome") ? "welcome_channels" : "prompts";
      auto records = state.draft.value(key, state.record.value(key, Json::array()));
      Json* list = &records;
      std::size_t item = at;
      if (action.starts_with("option")) {
        if (at >= records.size() || third == std::string::npos)
          return true;
        list = &records[at]["options"];
        item = index(action.substr(third + 1));
      }
      if (!list->is_array() || item >= list->size())
        return true;
      const auto other = direction == "up" ? (item ? item - 1 : SIZE_MAX) : item + 1;
      if (other < list->size()) {
        std::swap((*list)[item], (*list)[other]);
        setServerSettingsValue(key, records);
      }
      return true;
    }
    return false;
  }

  bool ClientApplication::serverExtraResult(const api::Event& event) {
    if (!event.guildSettingsRequest)
      return false;
    const auto& request = *event.guildSettingsRequest;
    const bool category = request.section == Section::DiscoveryCategory;
    const bool action = request.section == Section::GuideAction;
    const bool resource = request.section == Section::GuideResource;
    if (!category && !action && !resource)
      return false;
    auto& state = m_serverSettings;
    const auto at = state.pending.find(request.request);
    if (at == state.pending.end() || at->second != request || request.guildId != state.guild)
      return true;
    // Central handling retains the exact mutation and presents server MFA errors.
    if (event.guildSettingsMfaChallenge || !event.error.empty())
      return false;
    state.pending.erase(at);
    state.loading =
        std::ranges::any_of(state.pending, [](const auto& entry) { return entry.second.verb == Verb::Get; });
    state.saving = m_serverCommands.preparing.has_value()
        || std::ranges::any_of(state.pending, [](const auto& entry) { return entry.second.verb != Verb::Get; });
    state.mfaRequest.reset();
    state.mfaChallenge = 0;
    state.mfaCode.clear();
    if (auto input = m_inputs.find("server-settings:mfa-code"); input != m_inputs.end())
      input->second->setValue("");
    if (category) {
      api::GuildSettingsRequest refresh;
      refresh.section = Section::Discovery;
      requestServerSettings(refresh);
      state.status = "Discovery categories saved.";
    } else {
      state.guideIconDrafts.erase(std::string(action ? "guide-action:" : "guide-resource:") + request.resourceId);
      if (!state.guideIconDrafts.empty())
        serverExtraAction("save");
      else {
        state.status = "Server Guide icons saved.";
        state.loaded.erase("guide");
        loadServerSettings();
      }
    }
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::drawServerExtraForms(ServerSettingsForm& f, std::string_view part) {
    auto& state = m_serverSettings;
    auto& node = f.node;
    auto& y = f.y;
    const auto width = f.width;
    const auto editable = !state.saving && !state.mfaRequest;
    const auto value = [&](const std::string& key, Json fallback = Json()) {
      return f.value(key, std::move(fallback));
    };
    const auto input = [&](const std::string& id, const std::string& title, const std::string& description,
                           const std::string& initial, std::function<void(const std::string&)> change,
                           float height = 32) {
      f.detail(title, description);
      auto& field = inputControl(node, 0, y, width, height, "server-settings:extra:" + id, initial, {}, height > 32);
      field.setFontSize(Style::fontSizeDescription);
      field.setEnabled(editable);
      field.setOnChange(std::move(change));
      field.setOnSubmit([](const std::string&) {});
      y += height + 12;
    };
    const auto emoji = [&](const std::string& id, const Json& entry, std::function<void(Json)> change) {
      std::vector<std::string> names{"Unicode emoji"};
      std::vector<Json> choices{Json()};
      std::size_t selected = 0;
      const auto current = text(entry, "emoji_id");
      if (const auto at = state.lists.find("emoji"); at != state.lists.end() && at->second.is_array())
        for (const auto& record : at->second) {
          if (text(record, "id") == current)
            selected = choices.size();
          names.push_back(":" + text(record, "name") + ":");
          choices.push_back(record);
        }
      f.chooseCallback(
          id + "-custom", "Emoji", "Choose a server emoji or enter a Unicode emoji.", names, selected,
          [choices, change](std::size_t selected) {
            if (selected >= choices.size())
              return;
            const auto& chosen = choices[selected];
            change(
                chosen.is_object() ? Json{{"emoji_id", chosen["id"]}, {"emoji_name", text(chosen, "name")}}
                                   : Json{{"emoji_id", nullptr}, {"emoji_name", ""}}
            );
          }
      );
      if (current.empty())
        input(
            id + "-unicode", "Unicode emoji", "Display one emoji beside this item.", text(entry, "emoji_name"),
            [change](const std::string& text) {
              change({{"emoji_id", nullptr}, {"emoji_name", text.empty() ? Json() : Json(text)}});
            }
        );
    };

    if (part.starts_with("guide-action:") || part.starts_with("guide-resource:")) {
      const std::string field(part);
      const bool task = part.starts_with("guide-action:");
      const auto channel = field.substr(task ? 13 : 15);
      const auto rows = state.record.value(task ? "new_member_actions" : "resource_channels", Json::array());
      const auto persisted =
          std::ranges::find_if(rows, [&](const auto& row) { return text(row, "channel_id") == channel; });
      if (persisted == rows.end()) {
        f.row("Channel icon", "Save this new guide item before selecting its image.");
        return true;
      }
      if (!state.guideIconDrafts.contains(field)) {
        const auto hash = text(*persisted, "icon");
        if (!hash.empty()) {
          picture(
              node, 0, y, 48, 48,
              "https://cdn.discordapp.com/"
                  + std::string(task ? "new-member-actions" : "resource-channels")
                  + '/'
                  + channel
                  + '/'
                  + hash
                  + (hash.starts_with("a_") ? ".gif?size=128" : ".png?size=128"),
              Style::controlRadius
          );
          y += 60;
        }
      }
      f.detail("Channel icon", "Choose a PNG, JPEG, GIF or WebP image up to 10 MiB to display beside this guide item.");
      if (state.guideIconDrafts.contains(field)) {
        if (state.guideIconDrafts[field].value("icon", Json()).is_null())
          f.row("Icon removed", "Save changes to remove this guide image.");
        else {
          picture(node, 0, y, 48, 48, "server-guide-upload:" + state.guild + ':' + field, Style::controlRadius);
          y += 60;
        }
      }
      f.button("Choose image", "upload:" + field);
      f.button("Remove image", "guide-icon-clear:" + field);
      if (task) {
        auto current = state.guideIconDrafts.contains(field) && state.guideIconDrafts[field].contains("emoji")
            ? state.guideIconDrafts[field]["emoji"]
            : persisted->value("emoji", Json::object());
        if (!current.is_object())
          current = Json::object();
        Json projected = {{"emoji_id", current.value("id", Json())}, {"emoji_name", current.value("name", Json())}};
        emoji(field + "-emoji", projected, [this, field](Json chosen) {
          m_serverSettings.guideIconDrafts[field] = {
              {"emoji",
               {{"id", chosen.value("emoji_id", Json())},
                {"name", chosen.value("emoji_name", Json())},
                {"animated", false}}}
          };
          m_window.invalidate();
        });
      }
      return true;
    }

    if (part == "profile-assets") {
      f.section("Profile preview");
      const auto banner = value("custom_banner", text(state.record, "custom_banner_hash"));
      const auto icon = value("icon", text(state.record, "icon_hash", text(state.overview, "icon")));
      const auto asset = [&](const Json& value, const std::string& kind) {
        if (!value.is_string() || value.get<std::string>().empty())
          return std::string{};
        const auto hash = value.get<std::string>();
        if (hash.starts_with("data:"))
          return std::string("server-upload:") + state.guild + ':' + kind;
        return "https://cdn.discordapp.com/"
            + kind
            + '/'
            + state.guild
            + '/'
            + hash
            + (hash.starts_with("a_") ? ".gif?size=512" : ".png?size=512");
      };
      const auto headerHeight = std::min(156.0F, width * 0.35F);
      rectangle(node, 0, y, width, headerHeight + 74, panel, Style::surfaceRadius);
      if (!picture(node, 0, y, width, headerHeight, asset(banner, "banners"), Style::surfaceRadius))
        rectangle(node, 0, y, width, headerHeight, raised, Style::surfaceRadius);
      rectangle(node, 16, y + headerHeight - (Style::space2Xl + Style::spaceXxs), 72, 72, panel, 36);
      if (!picture(node, 20, y + headerHeight - (Style::space2Xl - Style::spaceXxs), 64, 64, asset(icon, "icons"), 32))
        glyph(node, 36, y + headerHeight - Style::cardPadding, Style::glyphSizeDisplay, "building-community", muted);
      // Use the actual draft string; JSON scalars intentionally do not use object lookup.
      const auto name = value("name", "");
      if (name.is_string())
        label(
            node, 102, y + headerHeight + Style::spaceDense, width - 118, name.get<std::string>(), Style::fontSizeTitle,
            foreground, FontWeight::SemiBold
        );
      const auto description = value("description", "");
      if (description.is_string())
        label(
            node, 102, y + headerHeight + (Style::space2Xl + Style::spaceXs), width - 118,
            description.get<std::string>(), Style::fontSizeMini, muted
        );
      y += headerHeight + 90;
      return true;
    }
    if (part == "sound-volume") {
      const auto volume = value("volume", 1.0);
      const auto current = volume.is_number() ? volume.get<double>() : 1.0;
      f.detail("Volume", "Set the default playback volume for this sound from 0 to 100 percent.");
      const std::string id = "server-sound-volume";
      std::unique_ptr<Node> saved;
      if (auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
        saved = std::move(at->second);
        m_detachedInputs.erase(at);
      }
      auto slider = saved ? std::unique_ptr<Slider>(static_cast<Slider*>(saved.release())) : std::make_unique<Slider>();
      m_widgets[id] = slider.get();
      slider->setPosition(0, y);
      slider->setSize(std::max(60.0F, width - 64), 26);
      slider->setControlHeight(26);
      slider->setRange(0, 100);
      slider->setStep(1);
      slider->setValue(std::round(current * 100));
      slider->setTrackHeight(3);
      slider->setThumbSize(12);
      slider->setWheelAdjustEnabled(false);
      api::GuildSettingsRequest request;
      request.guildId = state.guild;
      request.section = Section::Soundboard;
      request.verb = state.creating ? Verb::Create : Verb::Update;
      request.resourceId = state.resource;
      slider->setEnabled(editable && api::canManageGuildSettings(request, serverSettingsAccess(state.resource)));
      slider->setOnValueChanged([this](double value) { setServerSettingsValue("volume", value / 100); });
      node.addChild(std::move(slider));
      label(
          node, width - 54, y + (Style::spaceXs + Style::borderWidth), 54,
          std::to_string(static_cast<int>(std::round(current * 100))) + "%", Style::fontSizeDescription, foreground
      );
      y += (Style::spaceSection - Style::spaceXxs);
      return true;
    }
    if (part == "template-summary") {
      if (state.creating)
        return true;
      f.section("Template status");
      const auto flag = state.record.find("is_dirty");
      const std::optional<bool> dirty =
          flag != state.record.end() && flag->is_boolean() ? std::optional(flag->get<bool>()) : std::nullopt;
      f.row(
          !dirty       ? "Sync status unavailable"
              : *dirty ? "Changes have not been synced"
                       : "Template is up to date",
          !dirty       ? "Sync this template to include the server's current roles, channels and settings."
              : *dirty ? "Sync this template to include the server's current roles, channels and settings. Existing "
                         "servers made from the template stay unchanged."
                       : "New servers created from this template use its latest synced roles, channels and settings."
      );
      if (state.record.contains("updated_at"))
        f.row("Last synced", date(state.record["updated_at"]));
      if (state.record.contains("usage_count"))
        f.row("Servers created", state.record["usage_count"].dump());
      const auto snapshot = state.record.value("serialized_source_guild", Json::object());
      if (snapshot.is_object()) {
        const auto channels = snapshot.value("channels", Json::array());
        const auto roles = snapshot.value("roles", Json::array());
        f.row(
            "Saved structure",
            std::to_string(channels.size()) + " channels · " + std::to_string(roles.size()) + " roles"
        );
      }
      f.section("Template details");
      return true;
    }
    if (part == "event-recurrence") {
      f.section("Repeating event");
      const auto rule = value("recurrence_rule");
      const auto start = value("scheduled_start_time", "");
      std::tm utc{}, local{};
      if (start.is_string())
        if (const auto instant = api::muteDeadline(start.get<std::string>())) {
          const auto epoch = std::chrono::system_clock::to_time_t(*instant);
          gmtime_r(&epoch, &utc);
          localtime_r(&epoch, &local);
        }
      const int weekday = (utc.tm_wday + 6) % 7;
      const int nth = std::min(5, (std::max(1, utc.tm_mday) - 1) / 7 + 1);
      const auto dayOffset = recurrenceDayOffset(local.tm_wday, utc.tm_wday);
      const auto weekdaysLocal = recurrenceUtcWeekdays(std::array{0, 1, 2, 3, 4}, dayOffset);
      const auto weekendLocal = recurrenceUtcWeekdays(std::array{5, 6}, dayOffset);
      std::vector<std::pair<std::string, Json>> schedules = {
          {"Does not repeat", Json()},
          {"Every day", {{"frequency", 3}, {"interval", 1}}},
          {"Monday to Friday", {{"frequency", 3}, {"interval", 1}, {"by_weekday", weekdaysLocal}}},
          {"Saturday and Sunday", {{"frequency", 3}, {"interval", 1}, {"by_weekday", weekendLocal}}},
          {"Every week", {{"frequency", 2}, {"interval", 1}, {"by_weekday", Json::array({weekday})}}},
          {"Every other week", {{"frequency", 2}, {"interval", 2}, {"by_weekday", Json::array({weekday})}}},
          {"Every month",
           {{"frequency", 1}, {"interval", 1}, {"by_n_weekday", Json::array({{{"n", nth}, {"day", weekday}}})}}},
          {"Every year",
           {{"frequency", 0},
            {"interval", 1},
            {"by_month", Json::array({utc.tm_mon + 1})},
            {"by_month_day", Json::array({std::max(1, utc.tm_mday)})}}}
      };
      std::size_t selected = 0, customIndex = SIZE_MAX;
      if (rule.is_object()) {
        const auto frequency = rule.value("frequency", -1);
        selected = frequency == 0 ? 7
            : frequency == 1      ? 6
            : frequency == 2      ? (rule.value("interval", 1) == 2 ? 5 : 4)
                                  : 1;
        if (frequency == 3 && !rule.value("by_weekday", Json()).is_null() && !rule["by_weekday"].empty()) {
          auto current = rule["by_weekday"].get<std::vector<int>>();
          std::ranges::sort(current);
          bool matched = false;
          for (std::size_t i = 2; i <= 3; ++i) {
            auto preset = schedules[i].second["by_weekday"].get<std::vector<int>>();
            std::ranges::sort(preset);
            if (current == preset) {
              selected = i;
              matched = true;
              break;
            }
          }
          if (!matched) {
            customIndex = schedules.size();
            selected = customIndex;
            schedules.emplace_back("Current custom repeat", rule);
          }
        }
      }
      std::vector<std::string> names;
      for (const auto& item : schedules)
        names.push_back(item.first);
      f.chooseCallback(
          "event-repeat", "Repeats",
          "Choose how often this event repeats. Repeating events keep the scheduled start time.", names, selected,
          [this, schedules, start, customIndex](std::size_t at) {
            if (at >= schedules.size() || at == customIndex)
              return;
            auto rule = schedules[at].second;
            if (rule.is_object())
              rule["start"] = start;
            setServerSettingsValue("recurrence_rule", rule);
          }
      );
      if (rule.is_object()) {
        const auto frequency = rule.value("frequency", -1);
        const std::vector<std::string> weekdays{"Monday", "Tuesday",  "Wednesday", "Thursday",
                                                "Friday", "Saturday", "Sunday"};
        if (frequency == 2 || frequency == 1) {
          auto existing = frequency == 2 ? rule.value("by_weekday", Json::array({weekday})) : Json::array();
          const auto byNth = rule.value("by_n_weekday", Json::array());
          const auto chosenDay = frequency == 2 ? (!existing.empty() ? existing[0].get<int>() : weekday)
                                                : (!byNth.empty() ? byNth[0].value("day", weekday) : weekday);
          f.chooseCallback(
              "event-repeat-weekday", frequency == 2 ? "Day of the week" : "Day of the week (UTC)",
              frequency == 2 ? "Choose the weekday in your time zone for each occurrence."
                             : "Monthly recurrence uses the UTC calendar date of the event's start time.",
              weekdays, frequency == 2 ? recurrenceLocalWeekday(chosenDay, dayOffset) : chosenDay,
              [this, rule, frequency, nth, dayOffset](std::size_t at) mutable {
                auto next = cleanRecurrence(rule);
                if (frequency == 2)
                  next["by_weekday"] = Json::array({recurrenceUtcWeekday(static_cast<int>(at), dayOffset)});
                else {
                  auto n = rule.value("by_n_weekday", Json::array());
                  next["by_n_weekday"] = Json::array({{{"n", n.empty() ? nth : n[0].value("n", nth)}, {"day", at}}});
                }
                setServerSettingsValue("recurrence_rule", next);
              }
          );
          if (frequency == 1) {
            const auto number = byNth.empty() ? nth : byNth[0].value("n", nth);
            f.chooseCallback(
                "event-repeat-week", "Week of the month (UTC)",
                "Choose which occurrence of this weekday in the UTC month hosts the event.",
                {"First", "Second", "Third", "Fourth", "Fifth"}, std::clamp(number, 1, 5) - 1,
                [this, rule, chosenDay](std::size_t at) {
                  auto next = cleanRecurrence(rule);
                  next["by_n_weekday"] = Json::array({{{"n", at + 1}, {"day", chosenDay}}});
                  setServerSettingsValue("recurrence_rule", next);
                }
            );
          }
        }
        if (frequency == 0) {
          const auto months = rule.value("by_month", Json::array({utc.tm_mon + 1}));
          const auto days = rule.value("by_month_day", Json::array({std::max(1, utc.tm_mday)}));
          f.chooseCallback(
              "event-repeat-month", "Month (UTC)", "Choose the UTC calendar month for the annual event.",
              {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
               "November", "December"},
              months.empty() ? 0 : std::clamp(months[0].get<int>(), 1, 12) - 1, [this, rule](std::size_t at) {
                auto next = cleanRecurrence(rule);
                next["by_month"] = Json::array({at + 1});
                setServerSettingsValue("recurrence_rule", next);
              }
          );
          std::vector<std::string> dates;
          for (int d = 1; d <= 31; ++d)
            dates.push_back(std::to_string(d));
          f.chooseCallback(
              "event-repeat-date", "Day of the month (UTC)", "Choose the UTC calendar date for the annual event.",
              dates, days.empty() ? 0 : std::clamp(days[0].get<int>(), 1, 31) - 1, [this, rule](std::size_t at) {
                auto next = cleanRecurrence(rule);
                next["by_month_day"] = Json::array({at + 1});
                setServerSettingsValue("recurrence_rule", next);
              }
          );
        }
      }
      return true;
    }
    if (!part.empty())
      return false;

    if (state.page == "widget") {
      f.section("Server widget");
      f.toggle(
          "Enable server widget", "enabled", "Display the server's online members and an invite link on a website."
      );
      f.channel("Invite channel", "channel_id", "Choose where members arrive when they join through this widget.", 0);
      if (value("enabled", false) != true)
        return true;
      f.section("Embed widget");
      f.row("Website widget", "Copy an interactive 350×500 widget in your current color mode for your website.");
      f.button("Copy HTML", "widget-copy:html");
      f.row("JSON feed", "Use the public widget endpoint to display the server's public online-member data.");
      f.button("Copy JSON URL", "widget-copy:json");
      const std::vector<std::string> styles{"shield", "banner1", "banner2", "banner3", "banner4"};
      const auto style = state.lists.contains("widget-style") && state.lists["widget-style"].is_string()
          ? state.lists["widget-style"].get<std::string>()
          : "shield";
      const auto chosen = std::ranges::find(styles, style);
      f.chooseCallback(
          "widget-style", "Image style", "Choose the public widget image style to embed in a page or forum post.",
          {"Shield", "Banner 1", "Banner 2", "Banner 3", "Banner 4"},
          chosen == styles.end() ? 0 : std::distance(styles.begin(), chosen), [this, styles](std::size_t at) {
            if (at < styles.size()) {
              m_serverSettings.lists["widget-style"] = styles[at];
              m_window.invalidate();
            }
          }
      );
      f.button("Copy image URL", "widget-copy:image");
      return true;
    }
    if (state.page == "welcome") {
      f.section("Welcome screen");
      f.toggle(
          "Enable welcome screen", "enabled",
          "Introduce the server and recommend up to five channels when someone joins."
      );
      f.field("Description", "description", "Introduce your community in up to 140 characters.", 64, 2);
      auto channels = value("welcome_channels", Json::array());
      const auto cache = m_guildChannels.find(state.guild);
      const std::vector<api::Channel> empty;
      const auto& available = cache == m_guildChannels.end() ? empty : cache->second;
      for (std::size_t i = 0; i < channels.size(); ++i) {
        const auto entry = channels[i];
        f.section("Recommended channel " + std::to_string(i + 1));
        std::vector<std::string> names, ids;
        std::size_t selected = 0;
        for (const auto& channel : available)
          if (channel.type == 0 || channel.type == 5) {
            if (text(entry, "channel_id") == channel.id)
              selected = ids.size();
            ids.push_back(channel.id);
            names.push_back("# " + channel.name);
          }
        f.chooseCallback(
            "welcome-channel-" + std::to_string(i), "Channel", "Choose a channel that helps new members get started.",
            names, selected, [this, channels, i, ids](std::size_t at) mutable {
              if (at < ids.size()) {
                channels[i]["channel_id"] = ids[at];
                setServerSettingsValue("welcome_channels", channels);
              }
            }
        );
        input(
            "welcome-description-" + std::to_string(i), "Description", "Explain what members can do in this channel.",
            text(entry, "description"), [this, channels, i](const std::string& text) mutable {
              channels[i]["description"] = text;
              setServerSettingsValue("welcome_channels", channels);
            }
        );
        emoji("welcome-emoji-" + std::to_string(i), entry, [this, channels, i](Json next) mutable {
          for (const auto& [key, value] : next.items())
            channels[i][key] = value;
          setServerSettingsValue("welcome_channels", channels);
        });
        f.buttonControl(
            node, 0, y, 82, "Move up", "welcome-move:up:" + std::to_string(i), ButtonVariant::Default, editable && i > 0
        );
        f.buttonControl(
            node, 92, y, 92, "Move down", "welcome-move:down:" + std::to_string(i), ButtonVariant::Default,
            editable && i + 1 < channels.size()
        );
        f.buttonControl(
            node, 194, y, 82, "Remove", "remove-welcome:" + std::to_string(i), ButtonVariant::Default, editable
        );
        y += (Style::spaceSection - Style::spaceXxs);
      }
      if (channels.size() < 5)
        f.button("Add channel", "add-welcome");
      return true;
    }
    if (state.page == "discovery") {
      f.section("Discovery listing");
      f.toggle(
          "Publish in discovery", "is_published",
          "Let people find this eligible community in Discord's server discovery."
      );
      f.toggle(
          "Discover server emoji", "emoji_discoverability_enabled",
          "Allow this server's emoji to appear in public discovery surfaces."
      );
      f.field("About this server", "about", "Describe your community for people browsing discovery.", 96, 2);
      f.lineList(
          "Search keywords", "keywords", "Enter up to 10 keywords, one per line, to help people find this server."
      );
      f.lineList("Social links", "social_links", "Add up to nine community links, one per line.");
      f.section("Reasons to join");
      auto reasons = value("reasons_to_join", Json::array());
      for (std::size_t i = 0; i < reasons.size(); ++i) {
        input(
            "reason-" + std::to_string(i), "Reason " + std::to_string(i + 1),
            "Explain a benefit of joining in up to 300 characters.", text(reasons[i], "reason"),
            [this, reasons, i](const std::string& text) mutable {
              reasons[i]["reason"] = text;
              setServerSettingsValue("reasons_to_join", reasons);
            },
            64
        );
        emoji("reason-emoji-" + std::to_string(i), reasons[i], [this, reasons, i](Json next) mutable {
          for (const auto& [key, value] : next.items())
            reasons[i][key] = value;
          setServerSettingsValue("reasons_to_join", reasons);
        });
        f.button("Remove reason", "discovery-reason-remove:" + std::to_string(i));
      }
      if (reasons.size() < 4)
        f.button("Add reason", "discovery-reason-add");
      f.section("Categories");
      std::vector<std::pair<std::string, Json>> categories;
      if (const auto at = state.lists.find("discovery-categories"); at != state.lists.end() && at->second.is_array())
        for (const auto& item : at->second)
          if (item.contains("id"))
            categories.emplace_back(text(item, "name", "Category " + item["id"].dump()), item["id"]);
      if (!categories.empty())
        f.choose("Primary category", "primary_category_id", "Choose the main topic of this server.", categories);
      const auto secondary = state.record.value("secondary_category_ids", Json::array());
      for (const auto& [name, id] : categories) {
        if (id == value("primary_category_id"))
          continue;
        const bool selected = std::ranges::find(secondary, id) != secondary.end();
        const auto row = f.row(name, selected ? "Secondary category" : "Add this topic as a secondary category.", 88);
        const auto x = width - 78;
        f.buttonControl(
            node, x, row.first + (row.second - 26) / 2, 78, selected ? "Remove" : "Add",
            "discovery-category:" + (id.is_string() ? id.get<std::string>() : id.dump()), ButtonVariant::Default,
            editable && !state.dirty()
        );
      }
      if (const auto at = state.lists.find("discovery-requirements");
          at != state.lists.end() && at->second.is_object()) {
        f.section("Eligibility");
        for (const auto& [key, entry] : at->second.items())
          if (entry.is_boolean()) {
            auto title = key;
            std::ranges::replace(title, '_', ' ');
            if (!title.empty())
              title[0] = std::toupper(static_cast<unsigned char>(title[0]));
            f.row(title, entry.get<bool>() ? "Met" : "Not met");
          }
      }
      return true;
    }
    return false;
  }
} // namespace app

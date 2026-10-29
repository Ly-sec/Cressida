#include "app/client_application.h"
#include "app/composer_tokens.h"
#include "app/theme.h"
#include "ui/controls/scrollbar.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  std::vector<TextReplacement> ClientApplication::composerReplacements(std::string_view value) {
    std::vector<TextReplacement> result;
    const auto tokens = composerTokens(value);
    std::map<std::string, std::string> names;
    if (std::ranges::any_of(tokens, [](const auto& token) { return token.kind == ComposerToken::User; })) {
      if (const auto draft = m_draftMentionNames.find(m_channelId); draft != m_draftMentionNames.end())
        names = draft->second;
      for (const auto& user : m_userDirectory.relatedUsers())
        names[user.id] = user.name;
      for (const auto& message : m_messages) {
        names[message.authorId] = message.author;
        for (const auto& [id, name] : message.mentions)
          names[id] = name;
      }
      for (const auto& member : m_members)
        names[member.user.id] = member.user.name;
      names[m_account.id] = m_account.name;
    }
    for (const auto& token : tokens) {
      const std::string id(token.id);
      std::string label;
      TextureHandle texture;
      if (token.kind == ComposerToken::Emoji) {
        label = ":" + std::string(token.name) + ":";
        const auto path = api::emojiPath(id);
        auto& cached = m_images[path];
        cached.uses |= imageUseMask(ImageUse::Content);
        cached.touched = ++m_imageSerial;
        cached.visibleDraw = m_animationDraw;
        texture = cached.texture;
        if (!texture.valid())
          if (const auto animated = m_images.find(api::emojiPath(id, true)); animated != m_images.end()) {
            texture = animated->second.texture;
            animated->second.uses |= imageUseMask(ImageUse::Content);
            animated->second.touched = ++m_imageSerial;
            animated->second.visibleDraw = m_animationDraw;
          }
        if (!texture.valid()
            && !m_demo
            && !m_pendingImages.contains(path)
            && std::chrono::steady_clock::now() >= cached.retry
            && m_assets.request(path))
          m_pendingImages[path] |= imageUseMask(ImageUse::Content);
        if (texture.valid())
          label = "\u2060\u2003\u2060";
      } else if (token.kind == ComposerToken::Channel) {
        label = "#" + channelMention(m_guildId, id).name;
      } else if (token.kind == ComposerToken::Role) {
        label = "@unknown-role";
        for (const auto& role : m_roleDirectory.allRoles(m_guildId))
          if (role.id == id) {
            label = "@" + role.name;
            break;
          }
      } else {
        const auto known = names.find(id);
        label = "@" + userName(id, known == names.end() ? "unknown-user" : known->second);
      }
      result.push_back({token.begin, token.end, std::move(label), texture.id, texture.width, texture.height});
    }
    return result;
  }
  namespace {
    std::string folded(std::string_view value) {
      auto* text = g_utf8_casefold(value.data(), value.size());
      std::string result = text ? text : "";
      g_free(text);
      return result;
    }
    int matchScore(const std::string& value, const std::string& query) {
      const auto name = folded(value);
      if (query.empty())
        return 3;
      if (name == query)
        return 0;
      if (name.starts_with(query))
        return 1;
      return name.contains(query) ? 2 : 100;
    }
  } // namespace
  void ClientApplication::closeCompletion(bool dismiss) {
    m_completionDirty = true;
    if (dismiss && m_slashCommand)
      m_slashOptionsOpen = false;
    if (dismiss && m_completionQuery)
      m_completionDismissed = m_channelId
          + ":"
          + m_completionInput
          + ":"
          + std::to_string(m_completionQuery->end)
          + ":"
          + m_completionSnapshot;
    m_completionQuery.reset();
    m_completionItems.clear();
    m_completionBounds = {};
    m_completionInput.clear();
    m_completionSnapshot.clear();
    m_completionNonce.clear();
    m_completionGuild.clear();
    m_completionMembers.clear();
    m_completionSearchError.clear();
    m_completionSearchTimer.stop();
    m_completionTimeout.stop();
    m_scrolling.stop(m_completionScroll);
    m_completionScroll = 0;
  }

  void ClientApplication::updateCompletion() {
    if (m_slashCommand) {
      updateSlashArgumentCompletion();
      return;
    }
    const auto field = m_inputs.find(m_focusedInput);
    if (!m_connected
        || settingsVisible()
        || !m_dialogKind.empty()
        || m_contextOpen
        || m_presenceMenu
        || m_emojiMenu
        || m_gifMenu
        || !m_viewImage.empty()
        || m_videoExpanded
        || (m_focusedInput != "composer" && m_focusedInput != "message-edit")
        || field == m_inputs.end()
        || !field->second->parent()
        || !field->second->enabled()) {
      closeCompletion();
      return;
    }
    const auto& value = field->second->value();
    const auto cursor = field->second->textInputState();
    if (cursor.surroundingText != value) { // Never complete an IME preedit.
      closeCompletion();
      return;
    }
    auto query = completionQuery(value, cursor.cursor, cursor.anchor);
    if (query && query->trigger == '/' && m_focusedInput != "composer")
      query.reset();
    const auto identity = m_channelId + ":" + m_focusedInput + ":" + std::to_string(cursor.cursor) + ":" + value;
    if (identity != m_completionDismissed)
      m_completionDismissed.clear();
    if (!query
        || (query->trigger == ':' && g_utf8_strlen(query->text.data(), query->text.size()) < 2)
        || identity == m_completionDismissed) {
      closeCompletion();
      return;
    }
    const bool changed =
        m_completionQuery != query || m_completionInput != m_focusedInput || m_completionSnapshot != value;
    if (changed) {
      closeCompletion();
      m_completionQuery = query;
      m_completionInput = m_focusedInput;
      m_completionSnapshot = value;
      m_completionSelected = 0;
      m_completionGuild = m_dmMode ? "" : m_guildId;
      if (query->trigger == '@' && !m_completionGuild.empty() && !query->text.empty() && !m_demo) {
        const auto guild = m_completionGuild, text = query->text;
        m_completionNonce = "complete-" + std::to_string(++m_completionSerial);
        const auto nonce = m_completionNonce;
        m_completionSearchTimer.start(std::chrono::milliseconds(250), [this, guild, text, nonce] {
          if (!m_completionQuery || m_completionNonce != nonce || m_completionGuild != guild)
            return;
          if (!m_client.searchMembers(guild, text, nonce)) {
            m_completionSearchError = "Member search unavailable";
            m_window.invalidate();
            return;
          }
          m_completionTimeout.start(std::chrono::seconds(5), [this, nonce] {
            if (m_completionNonce == nonce) {
              m_completionSearchError = "Member search unavailable";
              m_window.invalidate();
            }
          });
        });
      }
    }
    if (query->trigger == '/') {
      if (changed || m_completionDirty)
        updateSlashCompletion();
      m_completionDirty = false;
      return;
    }
    if (!changed && !m_completionDirty)
      return;
    m_completionDirty = false;
    const auto selected =
        m_completionSelected < m_completionItems.size() ? m_completionItems[m_completionSelected].insert : "";
    m_completionItems.clear();
    const auto search = folded(query->text);
    if (query->trigger == ':' && !m_demo)
      prepareEmojiPicker();
    std::map<std::string, std::size_t> usageRanks;
    const auto frequent = m_emojiCatalog.frequentKeys(m_recentEmojis, m_emojiPreferences.frequent);
    for (std::size_t i = 0; i < frequent.size(); ++i)
      usageRanks[frequent[i]] = i;
    if (query->trigger == '@') {
      std::set<std::string> seen;
      const auto member = [&](api::Account user) {
        if (!api::validSnowflake(user.id) || m_userDirectory.relationship(user.id) == 2 || !seen.insert(user.id).second)
          return;
        user.name = userName(user.id, user.name);
        const auto score = std::min(matchScore(user.name, search), matchScore(user.username, search));
        if (score >= 100)
          return;
        m_completionItems.push_back(
            {CompletionItem::Member, user.id, user.name, user.username, "<@" + user.id + ">", user.avatar, score}
        );
      };
      for (const auto& user : m_completionMembers)
        member(user);
      for (const auto& row : m_members)
        member(row.user);
      for (auto message = m_messages.rbegin(); message != m_messages.rend(); ++message)
        if (m_completionItems.size() < 200)
          member({message->authorId, message->author, message->avatar});
      if (m_dmMode)
        for (const auto& channel : m_privateChannels)
          if (channel.id == m_channelId)
            for (const auto& user : channel.recipients)
              member(user);
      member(m_account);
      if (!m_dmMode) {
        for (const auto& role : m_roleDirectory.allRoles(m_guildId)) {
          const auto score = matchScore(role.name, search);
          if (role.id == m_guildId || score >= 100)
            continue;
          m_completionItems.push_back(
              {CompletionItem::Role,
               role.id,
               "@" + role.name,
               "Role",
               "<@&" + role.id + ">",
               {},
               score,
               role.colors.primary}
          );
        }
        for (const auto name : {"everyone", "here"}) {
          const auto score = matchScore(name, search);
          if (score < 100)
            m_completionItems.push_back(
                {CompletionItem::Broadcast,
                 name,
                 "@" + std::string(name),
                 "Notify members",
                 "@" + std::string(name),
                 {},
                 score + 4}
            );
        }
      }
    } else if (query->trigger == '#') {
      for (const auto& guild : m_guilds) {
        if (!m_dmMode && guild.id != m_guildId)
          continue;
        std::set<std::string> seen;
        const auto addChannel = [&](const api::Channel& channel) {
          if (channel.kind() == api::ChannelType::GuildCategory || !seen.insert(channel.id).second)
            return;
          const auto resolved = channelMention(guild.id, channel.id);
          if (resolved.link.empty())
            return;
          const auto score = matchScore(resolved.name, search);
          if (score < 100)
            m_completionItems.push_back(
                {CompletionItem::Channel,
                 channel.id,
                 "#" + resolved.name,
                 guild.name,
                 "<#" + channel.id + ">",
                 {},
                 score,
                 0,
                 channelGlyph(guild.id, channel)}
            );
        };
        for (const auto& channel : serverChannels(guild.id))
          addChannel(channel);
        for (const auto& thread : m_threads.list(guild.id))
          addChannel(thread);
      }
    } else {
      const auto emoji = [&](const std::string& key, const std::string& origin) {
        const auto* value = m_emojiCatalog.find(key);
        if (!value || !value->available)
          return;
        auto score = matchScore(value->name, search);
        for (const auto& alias : value->aliases)
          score = std::min(score, matchScore(alias, search));
        if (score >= 100)
          return;
        if (!value->id.empty()
            && api::emojiNeedsImage(*value, m_dmMode ? "" : m_guildId, m_account.premiumType.value_or(0))
            && !m_preferences.emojiImageLinks)
          return;
        m_completionItems.push_back(
            {CompletionItem::Emoji, key, ":" + value->name + ":", origin, value->token(),
             value->id.empty() ? "" : api::emojiPath(value->id, value->animated), score}
        );
      };
      for (const auto& key : m_emojiCatalog.unicode())
        emoji(key, "Unicode");
      for (const auto& guild : m_guilds)
        for (const auto& key : m_emojiCatalog.guild(guild.id))
          emoji(key, guild.name);
      // READY supplies most catalogs. Fill missing snapshots one guild at a time.
      if (!m_demo && m_loadingEmojiGuilds.empty())
        for (const auto& guild : m_guilds)
          if (!m_loadedEmojiGuilds.contains(guild.id) && !m_emojiGuildErrors.contains(guild.id)) {
            if (m_client.guildEmojis(guild.id))
              m_loadingEmojiGuilds.insert(guild.id);
            break;
          }
    }
    std::stable_sort(m_completionItems.begin(), m_completionItems.end(), [&](const auto& a, const auto& b) {
      if (a.score != b.score)
        return a.score < b.score;
      if (a.kind != b.kind)
        return a.kind < b.kind;
      if (a.kind == CompletionItem::Emoji) {
        const auto rank = [&](const auto& item) {
          const auto at = usageRanks.find(item.id);
          return at == usageRanks.end() ? std::size_t{64} : at->second;
        };
        if (rank(a) != rank(b))
          return rank(a) < rank(b);
        const auto priority = [&](const auto& item) {
          const auto* emoji = m_emojiCatalog.find(item.id);
          return !emoji || emoji->id.empty() ? 2 : emoji->guild == m_guildId ? 0 : 1;
        };
        if (priority(a) != priority(b))
          return priority(a) < priority(b);
      }
      return a.name != b.name ? a.name < b.name : a.id < b.id;
    });
    if (m_completionItems.size() > layout::Completion::maximumResults)
      m_completionItems.resize(layout::Completion::maximumResults);
    if (!changed && !selected.empty()) {
      const auto at = std::ranges::find(m_completionItems, selected, &CompletionItem::insert);
      m_completionSelected = at == m_completionItems.end() ? 0 : at - m_completionItems.begin();
    }
    if (m_completionSelected >= m_completionItems.size())
      m_completionSelected = 0;
  }

  void ClientApplication::completionEvent(const api::Event& event) {
    if (!m_completionQuery
        || m_completionQuery->trigger != '@'
        || event.resource != m_completionGuild
        || event.nonce != m_completionNonce
        || m_completionInput != m_focusedInput)
      return;
    const auto input = m_inputs.find(m_completionInput);
    if (input == m_inputs.end() || input->second->value() != m_completionSnapshot)
      return;
    m_completionMembers = event.memberMatches;
    m_completionDirty = true;
    m_completionTimeout.stop();
    m_completionSearchError.clear();
    m_window.invalidate();
  }

  void ClientApplication::chooseCompletion(std::size_t index) {
    if (m_slashCommand) {
      if (!m_completionQuery || index >= m_completionItems.size() || m_slashPending)
        return;
      const auto field = m_slashActiveOption ? "slash-field:" + std::to_string(*m_slashActiveOption) : "slash-options";
      const auto value = m_slashActiveOption ? m_slashValues[*m_slashActiveOption] : m_slashOptionQuery;
      if (field != m_completionInput || value != m_completionSnapshot)
        return;
      const auto picked = m_completionItems[index];
      if (picked.kind == CompletionItem::SlashOption)
        selectSlashOption(std::stoul(picked.id));
      else if (picked.kind == CompletionItem::SlashValue)
        chooseSlashChoice(std::stoul(picked.id));
      return;
    }
    if (!m_completionQuery || index >= m_completionItems.size())
      return;
    const auto field = m_inputs.find(m_completionInput);
    if (field == m_inputs.end()
        || !field->second->parent()
        || !field->second->enabled()
        || m_focusedInput != m_completionInput
        || field->second->value() != m_completionSnapshot)
      return;
    const auto cursor = field->second->textInputState();
    if (completionQuery(field->second->value(), cursor.cursor, cursor.anchor) != m_completionQuery)
      return;
    const auto query = *m_completionQuery;
    const auto picked = m_completionItems[index];
    if (picked.kind == CompletionItem::Command || picked.kind == CompletionItem::CommandRetry) {
      chooseSlashCommand(picked.id);
      return;
    }
    if (picked.kind == CompletionItem::Member)
      m_draftMentionNames[m_channelId][picked.id] = picked.name;
    auto text = m_completionItems[index].insert;
    if (query.end == m_completionSnapshot.size() || !g_ascii_isspace(m_completionSnapshot[query.end]))
      text += ' ';
    if (m_completionSnapshot.size() - (query.end - query.begin) + text.size() > 8000)
      return;
    const auto id = m_completionInput;
    TextInputEdit edit;
    edit.hasDelete = edit.hasCommitText = true;
    edit.deleteBeforeLength = query.end - query.begin;
    edit.commitText = std::move(text);
    closeCompletion();
    field->second->textInputApplyEdit(edit);
    if (picked.kind == CompletionItem::Emoji)
      recordEmojiUse(picked.id);
    focusControl(id);
    m_window.invalidate();
  }

  bool ClientApplication::completionKey(const Key& key) {
    if (m_slashCommand)
      return false;
    updateCompletion();
    if (!m_completionQuery)
      return false;
    if (key.symbol == XKB_KEY_Escape) {
      closeCompletion(true);
      m_window.invalidate();
      return true;
    }
    if (key.control || key.shift)
      return false;
    if (m_completionQuery->trigger == '/' && (key.symbol == XKB_KEY_Left || key.symbol == XKB_KEY_Right)) {
      const auto current = std::ranges::find(m_slashApps, m_slashFilter);
      const auto at = current == m_slashApps.end() ? 0 : current - m_slashApps.begin();
      filterSlashCommands(
          m_slashApps[(at + (key.symbol == XKB_KEY_Left ? m_slashApps.size() - 1 : 1)) % m_slashApps.size()]
      );
      return true;
    }
    if (m_completionItems.empty())
      return false;
    if (key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter || key.symbol == XKB_KEY_Tab) {
      chooseCompletion(m_completionSelected);
      return true;
    }
    if (key.symbol != XKB_KEY_Up && key.symbol != XKB_KEY_Down)
      return false;
    const auto count = m_completionItems.size();
    m_completionSelected = (m_completionSelected + (key.symbol == XKB_KEY_Up ? count - 1 : 1)) % count;
    m_scrolling.stop(m_completionScroll);
    const float rowHeight =
        m_completionQuery->trigger == '/' ? layout::SlashCommands::rowHeight : layout::Completion::rowHeight;
    const float top = m_completionSelected * rowHeight;
    m_completionScroll = std::clamp(m_completionScroll, std::max(0.0F, top + rowHeight - m_completionViewport), top);
    m_window.invalidate();
    return true;
  }

  bool ClientApplication::completionClick(float x, float y) {
    if (m_settingsOpen || !m_dialogKind.empty() || m_contextOpen || m_emojiMenu || m_gifMenu || !m_viewImage.empty())
      return false;
    if (!m_completionQuery || !m_completionBounds.contains(x, y))
      return false;
    if (m_completionQuery->trigger == '/') {
      for (auto hit = m_hits.rbegin(); hit != m_hits.rend(); ++hit)
        if (hit->contains(x, y)) {
          if (hit->action == Action::SlashFilter) {
            filterSlashCommands(hit->id);
            return true;
          }
          if (hit->action == Action::CompleteText) {
            chooseCompletion(std::stoul(hit->id));
            return true;
          }
        }
      m_controlInput.pointerButton(
          x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
      );
      return true;
    }
    if (x >= m_completionBounds.x + m_completionBounds.width - 12) {
      m_controlInput.pointerButton(
          x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
      );
      return true;
    }
    for (const auto& hit : m_hits)
      if (hit.action == Action::CompleteText && hit.contains(x, y)) {
        chooseCompletion(std::stoul(hit.id));
        break;
      }
    return true;
  }

  void ClientApplication::demoCompletionStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto setDraft = [&](const std::string& value) {
      m_drafts[m_channelId] = value;
      m_inputs.at("composer")->setValue(value);
      focusControl("composer");
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      require(
          m_completionQuery && !m_completionItems.empty() && m_completionBounds.height > 0,
          "Typing @ opens member matches without losing field focus"
      );
      onKey({XKB_KEY_Return});
      require(
          m_drafts[m_channelId] == "Hello <@11> " && m_messages.size() == 2 && !m_sending,
          "Enter inserts a real mention without submitting the composer"
      );
      require(
          m_inputs.at("composer")->displayValue() == "Hello @Maya ", "Selected mentions display names instead of IDs"
      );
    } else if (step == 1) {
      setDraft("prefix :umb tail");
      for (int i = 0; i < 5; ++i)
        m_inputs.at("composer")->moveCaretLeft();
      onKey({XKB_KEY_Tab});
      require(
          m_drafts[m_channelId].starts_with("prefix <a:umbriel:") && m_drafts[m_channelId].ends_with(" tail"),
          "Tab replaces only the active emoji token and preserves the suffix"
      );
      require(
          m_inputs.at("composer")->displayValue().find("<a:") == std::string::npos,
          "Selected custom emoji display their preview instead of protocol syntax"
      );
      onKey({.symbol = XKB_KEY_z, .control = true});
      require(m_drafts[m_channelId] == "prefix :umb tail", "Autocomplete insertion is a single undoable native edit");
    } else if (step == 2) {
      setDraft("@moon");
      onKey({XKB_KEY_Tab});
      require(m_drafts[m_channelId] == "<@&50> ", "Role autocomplete inserts the actual role mention syntax");
      setDraft("@");
      updateCompletion();
      require(m_completionItems.size() >= 5, "An empty @ query offers cached members and roles");
      const auto expected = m_completionItems[1].insert + " ";
      onKey({XKB_KEY_Down});
      onKey({XKB_KEY_Return});
      require(m_drafts[m_channelId] == expected, "Arrow navigation chooses the highlighted match");
    } else if (step == 3) {
      setDraft("@ma");
      onKey({XKB_KEY_Escape});
      updateCompletion();
      require(
          !m_completionQuery && m_drafts[m_channelId] == "@ma",
          "Escape dismisses autocomplete while leaving the text intact"
      );
      setDraft("@no");
      updateCompletion();
      require(m_completionQuery.has_value(), "Changing the query reopens a dismissed popup");
      m_completionNonce = "complete-current";
      api::Event result;
      result.resource = m_guildId;
      result.nonce = "complete-stale";
      result.memberMatches = {{"19", "Nobody", {}, "nobody"}};
      completionEvent(result);
      require(m_completionMembers.empty(), "Stale search nonces cannot replace current member results");
      result.nonce = m_completionNonce;
      completionEvent(result);
      updateCompletion();
      require(
          std::ranges::any_of(m_completionItems, [](const auto& item) { return item.id == "19"; }),
          "Matching server member results join the cached suggestions"
      );
    } else if (step == 4) {
      setDraft("draft stays");
      beginMessageEdit("9901");
    } else if (step == 5) {
      openContext(m_completionBounds.x + 100, m_completionBounds.y + Style::spaceGroup);
      require(!m_contextOpen, "Autocomplete blocks message context targets behind the popup");
      require(
          m_completionInput == "message-edit" && m_completionQuery, "Inline editing offers the same completion list"
      );
      onKey({XKB_KEY_Return});
      require(
          m_messageEdit
              && !m_messageEdit->request
              && m_messageEdit->text == "Fix <@11> "
              && m_drafts[m_channelId] == "draft stays",
          "Choosing inline autocomplete cannot save or alter the composer draft"
      );
      onKey({XKB_KEY_Escape});
      require(!m_messageEdit && !m_completionQuery, "Escape returns from inline editing after completing a token");
    } else if (step == 6) {
      for (const auto* draft : {":", ":u", ":é"}) {
        setDraft(draft);
        updateCompletion();
        require(
            !m_completionQuery && m_completionItems.empty() && m_completionBounds.height == 0,
            "Emoji autocomplete waits for two characters after the colon"
        );
      }
      setDraft(":um");
      updateCompletion();
      require(m_completionQuery && !m_completionItems.empty(), "Two emoji-name characters enable suggestions");
      setDraft(":u");
      updateCompletion();
      require(!m_completionQuery && m_completionItems.empty(), "Backspacing below two characters clears suggestions");
      setDraft(":umb");
    } else if (step == 7) {
      const auto row = std::ranges::find(m_hits, Action::CompleteText, &Hit::action);
      require(row != m_hits.end(), "Autocomplete exposes clipped pointer targets");
      const auto hit = *row;
      m_window.click(
          hit.x + Style::spaceDense,
          hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272
      );
      m_window.released();
      require(
          m_drafts[m_channelId].starts_with("<a:umbriel:") && m_focusedInput == "composer",
          "Clicking a suggestion retains native input focus and inserts its token"
      );
      setDraft(":smile");
      updateCompletion();
      require(!m_completionItems.empty(), "Unicode emoji names and aliases are searchable");
      onKey({XKB_KEY_Tab});
      require(!m_drafts[m_channelId].starts_with(":"), "Unicode emoji are inserted as native characters");
    } else {
      setDraft("@ma");
      updateCompletion();
      selectChannel(m_channels.back().id);
      require(
          !m_completionQuery && m_completionBounds.height == 0 && m_completionNonce.empty(),
          "Navigation clears suggestions and pending search identities"
      );
      api::Event result;
      result.resource = m_guildId;
      result.nonce = "complete-current";
      completionEvent(result);
      require(!m_completionQuery, "A delayed member reply cannot reopen autocomplete after navigation");
    }
    m_window.invalidate();
  }

  void ClientApplication::drawCompletion(Node& root, float w, float h) {
    using namespace theme;
    if (m_slashCommand) {
      drawSlashOptions(root, w, h);
      return;
    }
    updateCompletion();
    if (!m_completionQuery)
      return;
    if (m_completionQuery->trigger == '/') {
      drawSlashPicker(root, w, h);
      return;
    }
    const auto* input = m_inputs.at(m_completionInput);
    float x = 0, y = 0;
    for (const auto* node = static_cast<const Node*>(input); node && node != m_scene.get(); node = node->parent()) {
      x += node->x();
      y += node->y();
    }
    float width = input->width();
    if (m_completionInput == "composer") {
      x -= layout::Completion::composerLeadingInset;
      width += layout::Completion::composerExtraWidth;
    }
    x = std::clamp(x, 8.0F, std::max(8.0F, w - width - 8));
    width = std::min(width, w - x - Style::spaceSm);
    const float anchorHeight =
        m_completionInput == "message-edit" ? messageEditHeight(input->width()) : input->height();
    const float above = y - layout::Completion::topReserve, below = h - y - anchorHeight - Style::spaceLg;
    const bool upward = above >= std::min(layout::Completion::preferredRoom, below);
    const float room = upward ? above : below;
    if (room < layout::Completion::minimumHeight) {
      m_completionBounds = {};
      return;
    }
    const float full = std::max<std::size_t>(1, m_completionItems.size()) * layout::Completion::rowHeight;
    const float height = std::min({room, layout::Completion::maximumHeight, layout::Completion::chromeHeight + full});
    y = upward ? y - height - 6 : y + anchorHeight + Style::spaceCompact;
    m_completionViewport = height - layout::Completion::chromeHeight;
    m_completionBounds = {x, y, width, height, Action::None, {}};
    rectangle(root, x, y, width, height, raised, Style::surfaceRadius);
    rectangle(
        root, x + Style::borderWidth, y + Style::borderWidth, width - Style::spaceXxs, height - Style::spaceXxs,
        sidebar, Style::hoverRadius
    );
    label(
        root, x + Style::spaceMd, y + (Style::spaceDense + Style::borderWidth), width - Style::spaceXl,
        m_completionQuery->trigger == ':'       ? "Emoji matching :" + m_completionQuery->text
            : m_completionQuery->trigger == '#' ? "Channels"
            : !m_completionSearchError.empty() && m_completionItems.empty() ? m_completionSearchError
                                                                            : "Members & roles",
        Style::fontSizeMini, muted
    );
    m_hits.push_back(m_completionBounds);
    auto list = std::make_unique<Node>();
    list->setPosition(x + Style::spaceCompact, y + Style::space2Xl);
    list->setSize(width - 18, m_completionViewport);
    list->setClipChildren(true);
    m_scrolling.limit(m_completionScroll, std::max(0.0F, full - m_completionViewport));
    list->setTextScrollOffset(-m_completionScroll);
    for (std::size_t i = 0; i < m_completionItems.size(); ++i) {
      const float rowY = i * layout::Completion::rowHeight - m_completionScroll;
      if (rowY + layout::Completion::rowHeight <= 0 || rowY >= m_completionViewport)
        continue;
      const auto& item = m_completionItems[i];
      const bool over =
          hovered(x + Style::spaceCompact, y + Style::space2Xl + rowY, width - 18, layout::Completion::rowHeight)
          && m_completionBounds.contains(m_pointerX, m_pointerY);
      if (i == m_completionSelected || over)
        rectangle(*list, 0, rowY, width - 18, 34, raised, Style::controlRadius);
      if (item.kind == CompletionItem::Member)
        avatar(
            *list, 8, rowY + (Style::spaceXs + Style::borderWidth), layout::Avatar::compact, item.image, item.name,
            item.kind == CompletionItem::Member ? item.id : ""
        );
      else if (item.kind == CompletionItem::Emoji) {
        if (!item.image.empty())
          emojiImage(*list, 8, rowY + (Style::spaceXs + Style::borderWidth), 24, item.image);
        else if (const auto* emoji = m_emojiCatalog.find(item.id))
          label(*list, 8, rowY + Style::spaceXs, 30, emoji->unicode, Style::fontSizePage, foreground);
      } else
        glyph(
            *list, 10, rowY + Style::spaceSm, Style::glyphSizeLarge,
            item.kind == CompletionItem::Role          ? "users"
                : item.kind == CompletionItem::Channel ? item.glyph
                                                       : "at",
            item.color ? rgbHex(item.color) : muted
        );
      const float detailWidth = std::min(200.0F, width * .35F);
      if (item.kind == CompletionItem::Member || item.kind == CompletionItem::Role)
        roleLabel(
            *list, 42, rowY + Style::spaceDense, width - detailWidth - 68, item.name, Style::fontSizeCaption,
            foreground, FontWeight::Normal, item.id, "complete:" + item.id, over,
            item.kind == CompletionItem::Role ? std::optional<std::vector<std::string>>{std::vector{item.id}}
                                              : std::nullopt
        );
      else
        label(
            *list, 42, rowY + Style::spaceDense, width - detailWidth - 68, item.name, Style::fontSizeCaption,
            item.color ? rgbHex(item.color) : foreground
        );
      label(
          *list, width - detailWidth - Style::spaceXl, rowY + Style::spaceMd, detailWidth, item.detail,
          Style::fontSizeDetail, muted
      );
      const float top = std::max(0.0F, rowY),
                  bottom = std::min(m_completionViewport, rowY + layout::Completion::rowHeight);
      m_hits.push_back(
          {x + Style::spaceCompact, y + Style::space2Xl + top, width - 18, bottom - top, Action::CompleteText,
           std::to_string(i)}
      );
    }
    if (m_completionItems.empty())
      label(*list, 12, 10, width - 42, "No matches", Style::fontSizeDescription, muted);
    root.addChild(std::move(list));
    const std::string barId = "completion-scrollbar";
    std::unique_ptr<Node> owned;
    if (const auto stored = m_detachedInputs.find(barId); stored != m_detachedInputs.end()) {
      owned = std::move(stored->second);
      m_detachedInputs.erase(stored);
    } else {
      auto bar = std::make_unique<Scrollbar>();
      bar->setOnScrollChanged([this](float offset) {
        m_scrolling.stop(m_completionScroll);
        m_completionScroll = offset;
        m_window.invalidate();
      });
      owned = std::move(bar);
    }
    auto* bar = static_cast<Scrollbar*>(owned.get());
    m_widgets[barId] = bar;
    bar->setPosition(x + width - Style::spaceDense, y + Style::space2Xl);
    bar->setSize(8, m_completionViewport);
    bar->update(m_completionViewport, full, m_completionScroll);
    root.addChild(std::move(owned));
  }
} // namespace app

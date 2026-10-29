#include "api/uploads.h"
#include "app/client_application.h"
#include "app/theme.h"
#include "ui/controls/scrollbar.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    using Type = api::CommandOptionType;
    using Metrics = layout::SlashCommands;
    constexpr auto suggestionDelay = std::chrono::milliseconds(250);
    constexpr auto suggestionDeadline = std::chrono::seconds(5);
    constexpr auto submissionDeadline = std::chrono::seconds(35);
    constexpr std::size_t frequentCommandLimit = 50;
    constexpr std::uint64_t commandFileRequestTag = 1ULL << 63;
    constexpr std::int64_t interactionEpochMs = 1420070400000LL;
    constexpr unsigned interactionTimestampShift = 22;
    std::string fold(std::string_view value) {
      auto* folded = g_utf8_casefold(value.data(), value.size());
      std::string result = folded ? folded : "";
      g_free(folded);
      return result;
    }
    bool entity(Type type) {
      return type == Type::User || type == Type::Channel || type == Type::Role || type == Type::Mentionable;
    }
    std::string nonce() {
      static std::uint64_t last = 0;
      const auto now =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();
      const auto timestamp = std::uint64_t(std::max<std::int64_t>(0, now - interactionEpochMs))
          << interactionTimestampShift;
      last = std::max(last + 1, timestamp);
      return std::to_string(last);
    }
    std::string appIcon(const api::CommandApplication& app) {
      return app.icon.empty() ? std::string{} : "/app-icons/" + app.id + "/" + app.icon + ".png?size=64";
    }
  } // namespace

  api::CommandContext ClientApplication::slashContext() const {
    api::CommandContext context;
    context.channel = m_channelId;
    context.guild = m_dmMode ? "" : m_guildId;
    context.user = m_account.id;
    context.roles = m_roleDirectory.memberships(context.guild, context.user).value_or(std::vector<std::string>{});
    context.permissions = selectedChannelPermissions();
    if (const auto* channel = selectedChannel()) {
      context.parent = channel->parentId;
      context.thread = channel->isThread();
      context.nsfw = channel->nsfw.value_or(false);
      if (channel->isThread())
        if (const auto parent = std::ranges::find(m_channels, channel->parentId, &api::Channel::id);
            parent != m_channels.end())
          context.nsfw = parent->nsfw.value_or(false);
      if (context.thread)
        context.permissions = serverChannelPermissions(context.guild, context.parent);
    }
    context.administrator = m_roleDirectory.owner(context.guild) == context.user
        || (context.permissions && (*context.permissions & api::commandAdministratorPermission));
    if (m_dmMode)
      if (const auto channel = std::ranges::find(m_privateChannels, m_channelId, &api::Channel::id);
          channel != m_privateChannels.end() && channel->recipients.size() == 1)
        context.recipient = channel->recipients.front().id;
    return context;
  }
  void ClientApplication::clearSlashCommands() {
    m_slashCommands.clear();
    m_slashScope.clear();
    m_slashFilter = "*";
    m_slashIndexError.clear();
    m_slashIndexPending.clear();
    m_slashIndexLoaded = false;
    m_slashUsage.clear();
    ++m_slashRevision;
    m_slashApps.clear();
    m_slashAppScroll = 0;
    cancelSlashCommand(false);
  }
  void ClientApplication::updateSlashCompletion() {
    const auto scope = m_dmMode ? "channel:" + m_channelId : "guild:" + m_guildId;
    if (m_slashScope != scope) {
      m_slashScope = scope;
      ++m_slashRevision;
      m_slashCommands.clear();
      m_slashIndexPending.clear();
      m_slashIndexError.clear();
      m_slashIndexLoaded = false;
      m_slashFilter = "*";
      m_slashAppScroll = 0;
    }
    if (!m_slashIndexLoaded && m_slashIndexPending.empty()) {
      m_slashIndexLoaded = true;
      for (const bool user : {false, true}) {
        api::CommandIndexQuery query{m_channelId, m_dmMode ? "" : m_guildId, user, m_slashRevision};
        if (!m_demo && m_client.applicationCommands(query))
          m_slashIndexPending.insert(user);
        else if (!m_demo)
          m_slashIndexError = "Could not load commands. Choose Retry.";
      }
    }
    m_completionItems.clear();
    m_slashApps = {"*", "~"};
    const auto context = slashContext();
    std::map<std::string, std::string> apps;
    for (const auto& command : m_slashCommands)
      if (api::commandAvailable(command, context))
        apps[command.application] = command.app.name;
    std::vector<std::pair<std::string, std::string>> ordered(apps.begin(), apps.end());
    std::ranges::sort(ordered, [](const auto& a, const auto& b) { return fold(a.second) < fold(b.second); });
    for (const auto& [id, name] : ordered)
      m_slashApps.push_back(id);
    if (std::ranges::find(m_slashApps, m_slashFilter) == m_slashApps.end())
      m_slashFilter = "*";
    auto search = fold(m_completionQuery->text);
    while (!search.empty() && search.back() == ' ')
      search.pop_back();
    for (const auto& command : m_slashCommands) {
      if (!api::commandAvailable(command, context)
          || (m_slashFilter != "*" && m_slashFilter != "~" && m_slashFilter != command.application)
          || (m_slashFilter == "~" && !m_slashUsage.contains(command.key())))
        continue;
      const auto label = fold(command.label);
      auto canonicalName = command.name;
      for (const auto& [type, name] : command.path)
        canonicalName += " " + name;
      const auto canonical = fold(canonicalName);
      if (!search.empty()
          && !label.contains(search)
          && !canonical.contains(search)
          && !fold(command.description).contains(search)
          && !fold(command.app.name).contains(search))
        continue;
      const int score = label == search ? 0 : label.starts_with(search) ? 1 : 2;
      m_completionItems.push_back(
          {CompletionItem::Command, command.key(), command.displayName(), command.description, command.key(),
           appIcon(command.app), score}
      );
    }
    std::stable_sort(m_completionItems.begin(), m_completionItems.end(), [&](const auto& a, const auto& b) {
      if (a.score != b.score)
        return a.score < b.score;
      const auto usage = [&](const auto& item) {
        const auto at = m_slashUsage.find(item.id);
        return at == m_slashUsage.end() ? std::uint64_t(0) : at->second;
      };
      if (usage(a) != usage(b))
        return usage(a) > usage(b);
      return fold(a.name) < fold(b.name);
    });
    if (!m_slashIndexError.empty())
      m_completionItems.push_back(
          {CompletionItem::CommandRetry, "retry", "Retry loading commands", m_slashIndexError, "retry", {}}
      );
    if (m_completionSelected >= m_completionItems.size())
      m_completionSelected = 0;
  }
  void ClientApplication::filterSlashCommands(const std::string& application) {
    if (!m_completionQuery || m_completionQuery->trigger != '/')
      return;
    m_slashFilter = application;
    m_completionSelected = 0;
    m_scrolling.stop(m_completionScroll);
    m_completionScroll = 0;
    updateSlashCompletion();
    m_window.invalidate();
  }
  void ClientApplication::chooseSlashCommand(const std::string& key) {
    if (key == "retry") {
      m_slashIndexPending.clear();
      m_slashIndexLoaded = false;
      m_slashIndexError.clear();
      m_slashCommands.clear();
      ++m_slashRevision;
      updateSlashCompletion();
      m_window.invalidate();
      return;
    }
    const auto at = std::ranges::find_if(m_slashCommands, [&](const auto& command) { return command.key() == key; });
    if (at == m_slashCommands.end() || !api::commandAvailable(*at, slashContext()))
      return;
    auto command = *at;
    const auto snapshot = m_drafts[m_channelId];
    closeCompletion();
    closeDialog();
    closeContext();
    m_slashCommand = std::move(command);
    m_slashChannel = m_channelId;
    m_slashDraft = snapshot;
    m_slashValues.resize(m_slashCommand->options.size());
    m_slashValueLabels.clear();
    m_slashChoices.clear();
    m_slashMembers.clear();
    m_slashUploads.clear();
    m_slashPending = false;
    m_slashError.clear();
    m_slashOptionQuery.clear();
    m_slashSelectedOptions.clear();
    m_slashActiveOption.reset();
    m_slashOptionsOpen = true;
    m_slashComposerScroll = 0;
    ++m_slashGeneration;
    for (std::size_t i = 0; i < m_slashValues.size(); ++i)
      if (m_slashCommand->options[i].required) {
        m_slashSelectedOptions.insert(i);
        m_slashActiveOption = i;
        break;
      }
    for (auto& [id, input] : m_inputs)
      if (id.starts_with("slash-field:") || id == "slash-options")
        input->setValue("");
    selectSlashOption(m_slashActiveOption);
    m_window.invalidate();
  }
  std::vector<std::string> ClientApplication::slashValues() const {
    auto values = m_slashValues;
    if (!m_slashCommand)
      return values;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (!entity(m_slashCommand->options[i].type) || values[i].empty())
        continue;
      if (values[i].starts_with("<") && values[i].ends_with('>')) {
        const auto type = m_slashCommand->options[i].type;
        const bool channel = type == Type::Channel && values[i].starts_with("<#");
        const bool role = (type == Type::Role || type == Type::Mentionable) && values[i].starts_with("<@&");
        const bool user = (type == Type::User || type == Type::Mentionable)
            && (values[i].starts_with("<@!") || (values[i].starts_with("<@") && !values[i].starts_with("<@&")));
        if (channel || role || user) {
          const auto begin = values[i].find_first_of("0123456789");
          if (begin != std::string::npos)
            values[i] = values[i].substr(begin, values[i].size() - begin - 1);
        }
      }
      if (!api::validSnowflake(values[i]))
        throw std::runtime_error("Choose a suggestion or enter an ID for " + m_slashCommand->options[i].label + ".");
      if (m_slashCommand->options[i].type == Type::Channel) {
        const auto channel = std::ranges::find(m_channels, values[i], &api::Channel::id);
        const auto& types = m_slashCommand->options[i].channelTypes;
        if (channel == m_channels.end()
            || (!types.empty() && std::ranges::find(types, channel->type) == types.end())
            || !serverChannelVisible(*channel))
          throw std::runtime_error("Choose an available channel of the required type.");
      }
      if (m_slashCommand->options[i].type == Type::Role) {
        const auto roles = m_roleDirectory.allRoles(m_guildId);
        if (std::ranges::none_of(roles, [&](const auto& role) { return role.id == values[i]; }))
          throw std::runtime_error("Choose an available role in this server.");
      }
    }
    return values;
  }
  void ClientApplication::slashValueChanged(std::size_t index) {
    if (!m_slashCommand || index >= m_slashValues.size() || m_slashPending)
      return;
    m_slashAutocompleteTimer.stop();
    m_slashAutocompleteTimeout.stop();
    m_slashAutocompleteNonce.clear();
    m_slashChoiceOption = index;
    m_slashChoiceSelected = 0;
    m_slashChoices.clear();
    m_slashOptionsOpen = true;
    m_completionDirty = true;
    if (m_slashError == "Could not load argument suggestions. You can keep editing.")
      m_slashError.clear();
    const auto& option = m_slashCommand->options[index];
    auto query = fold(m_slashValues[index]);
    if (query.starts_with('@') || query.starts_with('#'))
      query.erase(0, 1);
    std::set<std::string> seen;
    for (const auto& choice : option.choices)
      if (query.empty()
          || fold(choice.name).contains(query)
          || fold(api::commandChoiceText(choice.value)).contains(query))
        m_slashChoices.push_back(choice);
    if (option.type == Type::Boolean)
      for (const auto& choice : std::vector<api::CommandChoice>{{"True", true}, {"False", false}})
        if (query.empty() || fold(choice.name).contains(query))
          m_slashChoices.push_back(choice);
    const auto add = [&](const std::string& id, const std::string& name, const std::string& prefix) {
      if (api::validSnowflake(id)
          && seen.insert(id).second
          && m_slashChoices.size() < api::commandChoiceLimit
          && (query.empty() || fold(name).contains(query) || id == query))
        m_slashChoices.push_back({prefix + name, id});
    };
    if (option.type == Type::User || option.type == Type::Mentionable) {
      const auto addUser = [&](const api::Account& user) {
        if (m_userDirectory.relationship(user.id) != 2)
          add(user.id, userName(user.id, user.name), "@");
      };
      for (const auto& user : m_slashMembers)
        addUser(user);
      for (const auto& member : m_members)
        addUser(member.user);
      for (const auto& user : m_userDirectory.relatedUsers())
        addUser(user);
      for (const auto& message : m_messages)
        addUser({message.authorId, message.author});
      for (const auto& channel : m_privateChannels)
        if (channel.id == m_channelId)
          for (const auto& user : channel.recipients)
            addUser(user);
      addUser(m_account);
    }
    if (option.type == Type::Role || option.type == Type::Mentionable)
      for (const auto& role : m_roleDirectory.allRoles(m_guildId))
        add(role.id, role.name, "@");
    if (option.type == Type::Channel)
      for (const auto& channel : m_channels)
        if (serverChannelVisible(channel)
            && (option.channelTypes.empty()
                || std::ranges::find(option.channelTypes, channel.type) != option.channelTypes.end()))
          add(channel.id, channel.name, "#");
    const bool memberSearch =
        !m_dmMode && !query.empty() && (option.type == Type::User || option.type == Type::Mentionable);
    if ((option.autocomplete || memberSearch) && !m_demo) {
      const auto generation = m_slashGeneration;
      const auto snapshot = nlohmann::json(m_slashValues).dump();
      m_slashAutocompleteSnapshot = snapshot;
      m_slashAutocompleteTimer.start(suggestionDelay, [this, index, generation, snapshot, memberSearch, query] {
        if (!m_slashCommand
            || generation != m_slashGeneration
            || m_slashPending
            || nlohmann::json(m_slashValues).dump() != snapshot)
          return;
        if (const auto field = m_inputs.find("slash-field:" + std::to_string(index));
            field == m_inputs.end() || field->second->hasPreedit())
          return;
        m_slashAutocompleteNonce = nonce();
        bool queued = false;
        if (memberSearch)
          queued = m_client.searchMembers(m_guildId, query, m_slashAutocompleteNonce);
        else {
          auto values = m_slashValues;
          api::Interaction action{
              .kind = api::InteractionKind::CommandAutocomplete,
              .channel = m_channelId,
              .guild = m_dmMode ? "" : m_guildId,
              .application = m_slashCommand->application,
              .nonce = m_slashAutocompleteNonce,
              .values = std::move(values),
              .command = m_slashCommand,
              .focusedOption = index
          };
          queued = m_client.interact(std::move(action));
        }
        if (!queued)
          m_slashAutocompleteNonce.clear();
        else
          m_slashAutocompleteTimeout.start(suggestionDeadline, [this, generation] {
            if (generation == m_slashGeneration) {
              m_slashAutocompleteNonce.clear();
              m_window.invalidate();
            }
          });
      });
    }
    m_window.invalidate();
  }
  void ClientApplication::chooseSlashChoice(std::size_t index) {
    if (!m_slashCommand
        || m_slashPending
        || index >= m_slashChoices.size()
        || m_slashChoiceOption >= m_slashValues.size())
      return;
    const auto choice = m_slashChoices[index];
    const auto option = m_slashChoiceOption;
    auto text = api::commandChoiceText(choice.value);
    auto& labels = m_slashValueLabels[option];
    labels[text] = choice.name;
    while (labels.size() > api::commandChoiceLimit) {
      auto oldest = labels.begin();
      if (oldest->first == text)
        ++oldest;
      labels.erase(oldest);
    }
    const auto id = "slash-field:" + std::to_string(option);
    if (const auto input = m_inputs.find(id); input != m_inputs.end()) {
      input->second->selectAll();
      TextInputEdit edit;
      edit.hasCommitText = true;
      edit.commitText = text;
      input->second->textInputApplyEdit(edit);
      focusControl(id);
    }
    m_slashValues[option] = text;
    m_slashChoices.clear();
    m_slashAutocompleteNonce.clear();
    m_slashAutocompleteTimer.stop();
    m_slashAutocompleteTimeout.stop();
    m_slashOptionsOpen = false;
    closeCompletion();
    m_window.invalidate();
  }
  void ClientApplication::submitSlashCommand() {
    if (!slashComposerActive()
        || !m_dialogKind.empty()
        || settingsVisible()
        || m_contextOpen
        || m_inboxOpen
        || m_slashPending)
      return;
    if (!api::commandAvailable(*m_slashCommand, slashContext())) {
      m_slashError = "This command is no longer available in this channel.";
      m_window.invalidate();
      return;
    }
    for (const auto& [id, input] : m_inputs)
      if (id.starts_with("slash-field:") && input->hasPreedit()) {
        m_slashError = "Finish composing the current argument before running this command.";
        m_window.invalidate();
        return;
      }
    const auto indexed = std::ranges::find_if(m_slashCommands, [&](const auto& command) {
      return command.key() == m_slashCommand->key();
    });
    if (indexed == m_slashCommands.end() || indexed->version != m_slashCommand->version) {
      m_slashError = "The command list changed. Cancel this command and choose it again.";
      m_window.invalidate();
      return;
    }
    try {
      auto values = slashValues();
      std::vector<api::Upload> uploads;
      for (const auto& [option, upload] : m_slashUploads) {
        values[option] = std::to_string(uploads.size());
        uploads.push_back(upload);
      }
      if (!api::validCommandUploads(uploads))
        throw std::runtime_error("Attachments must total no more than 20 MiB.");
      for (std::size_t i = 0; i < values.size(); ++i)
        if (m_slashCommand->options[i].required && values[i].empty()) {
          selectSlashOption(i);
          throw std::runtime_error("Enter a value for " + m_slashCommand->options[i].label + ".");
        }
      (void)api::applicationCommandData(*m_slashCommand, values);
      if (!api::randomizeMediaFilenames(uploads, m_preferences.randomizeMediaFilenames))
        throw std::runtime_error("Could not prepare the attachment filenames. Try again.");
      const auto requestNonce = nonce();
      const auto uploadCount = uploads.size();
      api::Interaction action{
          .kind = api::InteractionKind::ApplicationCommand,
          .channel = m_channelId,
          .guild = m_dmMode ? "" : m_guildId,
          .application = m_slashCommand->application,
          .nonce = requestNonce,
          .values = std::move(values),
          .command = m_slashCommand,
          .commandUploads = std::move(uploads)
      };
      if ((!m_demo && !m_client.interact(std::move(action)))
          || (m_demo && !m_demoOverlay.starts_with("slash-commands")))
        throw std::runtime_error("Could not queue this command. Check the connection and try again.");
      m_slashNonce = requestNonce;
      m_slashPending = true;
      m_slashError.clear();
      m_slashAutocompleteTimer.stop();
      m_slashAutocompleteTimeout.stop();
      m_slashAutocompleteNonce.clear();
      m_slashChoices.clear();
      m_slashOptionsOpen = false;
      closeCompletion();
      const auto generation = m_slashGeneration;
      const auto deadline = submissionDeadline * (uploadCount ? uploadCount + 2 : 1);
      m_slashSubmissionTimer.start(deadline, [this, requestNonce, generation] {
        if (generation == m_slashGeneration && m_slashNonce == requestNonce && m_slashPending) {
          m_slashPending = false;
          m_slashError =
              "The application has not confirmed this command. It may already have run. Submit again only if needed.";
          m_window.invalidate();
        }
      });
    } catch (const std::exception& error) {
      m_slashError = error.what();
    }
    m_window.invalidate();
  }
  void ClientApplication::pickSlashAttachment(std::size_t index) {
    if (!m_slashCommand
        || m_slashPending
        || index >= m_slashValues.size()
        || m_slashCommand->options[index].type != Type::Attachment)
      return;
    if (m_slashUploads.contains(index)) {
      m_slashUploads.erase(index);
      m_slashValues[index].clear();
      m_slashSelectedOptions.erase(index);
      m_slashRevealActive = true;
      m_completionDirty = true;
      m_window.invalidate();
      return;
    }
    if (m_demo) {
      m_slashError = "The file chooser is unavailable in this offline preview.";
      m_window.invalidate();
      return;
    }
    m_slashFileOption = index;
    m_slashFileRequest = commandFileRequestTag | ++m_serverUploadSerial;
    m_filePicker.open(m_slashFileRequest, "Choose a command attachment", {"*/*"}, api::maxUploadBytes);
  }
  bool ClientApplication::slashAttachmentResult(const PickedFile& result) {
    if (!m_slashFileRequest || result.saving || result.request != m_slashFileRequest)
      return false;
    m_slashFileRequest = 0;
    if (!m_slashCommand || !m_dialogKind.empty() || m_slashPending || m_slashChannel != m_channelId || result.cancelled)
      return true;
    if (!result.error.empty())
      m_slashError = result.error;
    else {
      auto uploads = m_slashUploads;
      uploads[m_slashFileOption] = {result.name, result.mime, result.bytes};
      std::vector<api::Upload> checked;
      for (const auto& [index, upload] : uploads)
        checked.push_back(upload);
      if (!api::validCommandUploads(checked))
        m_slashError = "Choose up to ten attachments totaling no more than 20 MiB.";
      else {
        m_slashUploads = std::move(uploads);
        m_slashValues[m_slashFileOption] = "0";
        m_slashSelectedOptions.insert(m_slashFileOption);
        m_slashRevealActive = true;
        m_slashError.clear();
      }
    }
    m_window.invalidate();
    return true;
  }
  void ClientApplication::slashCommandEvent(const api::Event& event) {
    if (event.operation == api::Operation::CommandIndex) {
      if (event.live && (event.resource.empty() || m_slashScope == "guild:" + event.resource || m_dmMode)) {
        m_slashIndexLoaded = false;
        m_slashIndexPending.clear();
        m_slashCommands.clear();
        m_slashIndexError.clear();
        ++m_slashRevision;
        m_completionDirty = true;
        m_window.invalidate();
        return;
      }
      if (!event.commandIndex
          || event.commandIndex->revision != m_slashRevision
          || (event.commandIndex->guild.empty() ? "channel:" + event.commandIndex->channel
                                                : "guild:" + event.commandIndex->guild)
              != m_slashScope
          || !m_slashIndexPending.erase(event.commandIndex->user))
        return;
      if (!event.error.empty())
        m_slashIndexError = event.error;
      else
        for (const auto& command : event.commands) {
          const auto existing =
              std::ranges::find_if(m_slashCommands, [&](const auto& entry) { return entry.key() == command.key(); });
          if (existing != m_slashCommands.end()) {
            if (!event.commandIndex->user)
              *existing = command;
          } else if (m_slashCommands.size() < api::commandIndexLimit)
            m_slashCommands.push_back(command);
        }
      m_completionDirty = true;
      m_window.invalidate();
      return;
    }
    if (!m_slashCommand || m_slashChannel != m_channelId || !m_dialogKind.empty())
      return;
    if (!event.nonce.empty()
        && event.nonce == m_slashAutocompleteNonce
        && nlohmann::json(m_slashValues).dump() == m_slashAutocompleteSnapshot) {
      if (event.operation == api::Operation::MemberSearch) {
        m_slashMembers = event.memberMatches;
        const auto index = m_slashChoiceOption;
        slashValueChanged(index);
        m_slashAutocompleteTimer.stop();
      } else if (event.operation == api::Operation::CommandChoices) {
        m_slashChoices = event.commandChoices;
        m_slashChoiceSelected = 0;
        m_slashAutocompleteNonce.clear();
        m_slashAutocompleteTimeout.stop();
        m_completionDirty = true;
      }
      m_window.invalidate();
      return;
    }
    if (event.operation == api::Operation::Interaction
        && event.interaction
        && event.interaction->kind == api::InteractionKind::CommandAutocomplete
        && event.interaction->nonce == m_slashAutocompleteNonce
        && !event.error.empty()
        && !m_slashPending) {
      m_slashAutocompleteNonce.clear();
      m_slashAutocompleteTimeout.stop();
      m_slashError = "Could not load argument suggestions. You can keep editing.";
    } else if (
        event.operation == api::Operation::Interaction
        && event.interaction
        && event.interaction->nonce == m_slashNonce
        && event.interaction->kind == api::InteractionKind::ApplicationCommand
    ) {
      if (!event.error.empty()) {
        m_slashPending = false;
        m_slashError = event.error;
        m_slashSubmissionTimer.stop();
      }
    } else if (
        event.operation == api::Operation::ComponentResult && !event.nonce.empty() && event.nonce == m_slashNonce
    ) {
      if (event.modal
          && (event.modal->channel != m_slashChannel || event.modal->application != m_slashCommand->application))
        return;
      m_slashSubmissionTimer.stop();
      if (!event.error.empty()) {
        m_slashPending = false;
        m_slashError = event.error;
      } else {
        if (m_drafts[m_slashChannel] == m_slashDraft) {
          m_drafts[m_slashChannel].clear();
          if (m_inputs.contains("composer"))
            m_inputs.at("composer")->setValue("");
        }
        ++m_slashUsage[m_slashCommand->key()];
        while (m_slashUsage.size() > frequentCommandLimit) {
          const auto lowest =
              std::ranges::min_element(m_slashUsage, {}, [](const auto& entry) { return entry.second; });
          m_slashUsage.erase(lowest);
        }
        closeDialog();
        if (event.modal) {
          m_applicationModal = event.modal;
          for (const auto& field : event.modal->fields)
            m_applicationValues.push_back(field.value);
          m_dialogKind = "application-modal";
          m_focusedInput = "application-field:0";
        } else
          focusChatComposer();
      }
    }
    m_window.invalidate();
  }

  void ClientApplication::drawSlashPicker(Node& root, float w, float h) {
    using namespace theme;
    const auto* input = m_inputs.at("composer");
    float x = 0, y = 0;
    for (const auto* node = static_cast<const Node*>(input); node && node != m_scene.get(); node = node->parent()) {
      x += node->x();
      y += node->y();
    }
    x = std::max(Style::spaceSm, x - layout::Completion::composerLeadingInset);
    const float width = std::min(input->width() + layout::Completion::composerExtraWidth, w - x - Style::spaceSm);
    const auto frame = Metrics::picker(x, y, width, m_completionItems.size());
    if (frame.height < Metrics::headerHeight + Metrics::footerHeight + Style::controlHeight) {
      m_completionBounds = {};
      return;
    }
    m_completionBounds = {frame.x, frame.y, frame.width, frame.height, Action::None, {}};
    m_hits.push_back(m_completionBounds);
    rectangle(root, frame.x, frame.y, frame.width, frame.height, sidebar, Style::surfaceRadius);
    const float listX = frame.x + Metrics::railWidth;
    const float listWidth = frame.width - Metrics::railWidth;
    const float viewport = frame.height - Metrics::headerHeight - Metrics::footerHeight;
    m_completionViewport = viewport;
    std::string title = m_slashFilter == "~" ? "Frequently used" : "All commands";
    for (const auto& command : m_slashCommands)
      if (command.application == m_slashFilter) {
        title = command.app.name;
        break;
      }
    if (!m_slashIndexPending.empty())
      title += " · Loading…";
    label(
        root, listX + Style::spaceMd, frame.y + Style::spaceMd, listWidth - Style::spaceXl, title,
        Style::fontSizeDescription, muted, FontWeight::SemiBold
    );
    const auto scrollbar = [&](const std::string& id, float bx, float by, float view, float full, float& offset) {
      std::unique_ptr<Node> owned;
      if (auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
        owned = std::move(at->second);
        m_detachedInputs.erase(at);
      } else
        owned = std::make_unique<Scrollbar>();
      auto* bar = static_cast<Scrollbar*>(owned.get());
      m_widgets[id] = bar;
      bar->setOnScrollChanged([this, position = &offset](float value) {
        m_scrolling.stop(*position);
        *position = value;
        m_window.invalidate();
      });
      bar->setPosition(bx, by);
      bar->setSize(Style::spaceSm, view);
      bar->update(view, full, offset);
      root.addChild(std::move(owned));
    };
    auto rail = std::make_unique<Node>();
    rail->setPosition(frame.x, frame.y + Style::spaceSm);
    rail->setSize(Metrics::railWidth, frame.height - Style::spaceSm * 2);
    rail->setClipChildren(true);
    const float railHeight = m_slashApps.size() * Metrics::appStep;
    m_scrolling.limit(m_slashAppScroll, std::max(0.0F, railHeight - rail->height()));
    for (std::size_t i = 0; i < m_slashApps.size(); ++i) {
      const auto& app = m_slashApps[i];
      const float top = i * Metrics::appStep - m_slashAppScroll;
      if (top + Metrics::appSize <= 0 || top >= rail->height())
        continue;
      const float rx = (Metrics::railWidth - Metrics::appSize) / 2;
      const bool over = hovered(frame.x + rx, frame.y + Style::spaceSm + top, Metrics::appSize, Metrics::appSize)
          && m_completionBounds.contains(m_pointerX, m_pointerY);
      rectangle(
          *rail, rx, top, Metrics::appSize, Metrics::appSize, app == m_slashFilter || over ? raised : panel,
          Style::controlRadius
      );
      std::string caption = app == "*" ? "All commands" : "Frequently used";
      if (i < 2)
        glyph(
            *rail, rx + Style::spaceSm, top + Style::spaceSm, Style::glyphSizeMedium, i ? "clock" : "command",
            app == m_slashFilter ? accent : muted
        );
      else {
        const auto command = std::ranges::find(m_slashCommands, app, &api::ApplicationCommand::application);
        if (command != m_slashCommands.end()) {
          caption = command->app.name;
          if (!image(*rail, rx, top, Metrics::appSize, appIcon(command->app), Style::controlRadius))
            label(
                *rail, rx + Style::spaceSm, top + Style::spaceCompact, Metrics::appSize - Style::spaceSm,
                caption.substr(0, g_utf8_next_char(caption.c_str()) - caption.c_str()), Style::fontSizeTitle,
                foreground, FontWeight::SemiBold
            );
        }
      }
      const float hitTop = std::max(0.0F, top), hitBottom = std::min(rail->height(), top + Metrics::appSize);
      m_hits.push_back(
          {frame.x + rx, frame.y + Style::spaceSm + hitTop, Metrics::appSize, hitBottom - hitTop, Action::SlashFilter,
           app}
      );
      if (over) {
        m_tooltip = caption;
        m_tooltipX = frame.x + Metrics::railWidth / 2;
        m_tooltipY = frame.y + Style::spaceSm + top;
      }
    }
    root.addChild(std::move(rail));
    if (railHeight > frame.height - Style::spaceSm * 2)
      scrollbar(
          "slash-app-scrollbar", frame.x + Metrics::railWidth - Style::spaceSm, frame.y + Style::spaceSm,
          frame.height - Style::spaceSm * 2, railHeight, m_slashAppScroll
      );
    auto list = std::make_unique<Node>();
    list->setPosition(listX, frame.y + Metrics::headerHeight);
    list->setSize(listWidth - Metrics::scrollbarGutter, viewport);
    list->setClipChildren(true);
    const float full = m_completionItems.size() * Metrics::rowHeight;
    m_scrolling.limit(m_completionScroll, std::max(0.0F, full - viewport));
    list->setTextScrollOffset(-m_completionScroll);
    for (std::size_t i = 0; i < m_completionItems.size(); ++i) {
      const float top = i * Metrics::rowHeight - m_completionScroll;
      if (top + Metrics::rowHeight <= 0 || top >= viewport)
        continue;
      const auto& item = m_completionItems[i];
      const bool over = hovered(listX, frame.y + Metrics::headerHeight + top, list->width(), Metrics::rowHeight)
          && m_completionBounds.contains(m_pointerX, m_pointerY);
      if (over || i == m_completionSelected)
        rectangle(
            *list, Style::spaceXs, top, list->width() - Style::spaceSm, Metrics::rowHeight - Style::spaceXs, raised,
            Style::controlRadius
        );
      const auto command =
          std::ranges::find_if(m_slashCommands, [&](const auto& command) { return command.key() == item.id; });
      const float iconX = Style::spaceMd;
      if (!image(*list, iconX, top + Style::spaceMd, Metrics::appSize, item.image, Style::controlRadius))
        glyph(*list, iconX + Style::spaceXs, top + Style::spaceLg, Style::glyphSizeLarge, "command", muted);
      const float textX = iconX + Metrics::appSize + Style::spaceMd;
      const float originWidth = std::min(list->width() / 4, Style::spaceGroup * 3);
      label(
          *list, textX, top + Style::spaceSm, list->width() - textX - originWidth - Style::spaceMd, item.name,
          Style::fontSizeBody, foreground, FontWeight::SemiBold
      );
      label(
          *list, textX, top + Style::space2Xl, list->width() - textX - Style::spaceMd, item.detail,
          Style::fontSizeDescription, muted
      );
      if (command != m_slashCommands.end())
        label(
            *list, list->width() - originWidth - Style::spaceSm, top + Style::spaceMd, originWidth, command->app.name,
            Style::fontSizeMini, muted
        );
      const float hitTop = std::max(0.0F, top), hitBottom = std::min(viewport, top + Metrics::rowHeight);
      m_hits.push_back(
          {listX, frame.y + Metrics::headerHeight + hitTop, list->width(), hitBottom - hitTop, Action::CompleteText,
           std::to_string(i)}
      );
    }
    if (m_completionItems.empty())
      label(
          *list, Style::spaceMd, Style::spaceMd, list->width() - Style::spaceXl,
          !m_slashIndexPending.empty() ? "Loading commands…"
              : m_slashFilter == "~"   ? "Commands you use appear here for this session."
                                       : "No commands available here.",
          Style::fontSizeDescription, muted, FontWeight::Normal, 2
      );
    root.addChild(std::move(list));
    scrollbar(
        "completion-scrollbar", frame.x + frame.width - Style::spaceMd, frame.y + Metrics::headerHeight, viewport, full,
        m_completionScroll
    );
    label(
        root, listX + Style::spaceMd, frame.y + frame.height - Metrics::footerHeight + Style::spaceXs,
        listWidth - Style::spaceXl, "↑ ↓ Choose command   ← → Choose app   Enter or Tab Select   Esc Close",
        Style::fontSizeMini, muted
    );
  }

} // namespace app

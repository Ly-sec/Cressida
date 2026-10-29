#include "app/client_application.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::prepareSlashCommandDemo() {
    using Json = nlohmann::json;
    m_showMembers = false;
    m_dmMode = false;
    m_slashScope = "guild:" + m_guildId;
    m_slashIndexLoaded = true;
    m_slashIndexPending.clear();
    m_slashFilter = "*";
    auto index = Json::parse(
        R"JSON({"applications":[
      {"id":"30","name":"Orbit","bot_id":"31"},
      {"id":"32","name":"Lunar","bot_id":"33"}],"application_commands":[
      {"id":"40","version":"50","application_id":"30","name":"echo","description":"Send a message through Orbit","options":[
        {"type":3,"name":"text","description":"The message to send","required":true,"max_length":100},
        {"type":5,"name":"private","description":"Only you can see the response"},
        {"type":3,"name":"style","description":"Response style","choices":[{"name":"Normal","value":"normal"},{"name":"Quiet","value":"quiet"}]}]},
      {"id":"41","version":"51","application_id":"30","name":"logging","description":"Configure event logs","options":[
        {"type":1,"name":"moderation","description":"Choose a person and channel","options":[
          {"type":6,"name":"person","description":"Choose a member","required":true},
          {"type":7,"name":"channel","description":"Text destination","required":true,"channel_types":[0]}]}]},
      {"id":"42","version":"52","application_id":"32","name":"lookup","description":"Search Lunar's catalogue","options":[
        {"type":3,"name":"query","description":"Search the catalogue","required":true,"autocomplete":true}]},
      {"id":"43","version":"53","application_id":"32","name":"upload","description":"Share an attachment with Lunar","options":[
        {"type":11,"name":"file","description":"A local file","required":true}]},
      {"id":"44","version":"54","application_id":"32","name":"docs","description":"Get documentation links","options":[{"type":3,"name":"page","description":"Search for a documentation page","autocomplete":true}]}]})JSON"
    );
    for (unsigned i = 0; i < 18; ++i) {
      const auto app = std::to_string(100 + i);
      index["applications"].push_back({{"id", app}, {"name", "Utility " + std::to_string(i)}});
      index["application_commands"].push_back(
          {{"id", std::to_string(200 + i)},
           {"version", "300"},
           {"application_id", app},
           {"name", "utility" + std::to_string(i)},
           {"description", "A command for the app rail scrolling fixture"}}
      );
    }
    Json manyOptions = Json::array();
    for (std::size_t i = 0; i < api::commandOptionLimit; ++i)
      manyOptions.push_back(
          {{"type", 3}, {"name", "option" + std::to_string(i)}, {"description", "A wrapped composer argument"}}
      );
    index["application_commands"].push_back(
        {{"id", "45"},
         {"version", "55"},
         {"application_id", "32"},
         {"name", "configure"},
         {"description", "Configure many options"},
         {"options", manyOptions}}
    );
    m_slashCommands = api::parseCommandIndex(index, {m_channelId, m_guildId});
    m_drafts[m_channelId] = "/";
    m_focusedInput = "composer";
    if (const auto input = m_inputs.find("composer"); input != m_inputs.end())
      input->second->setValue("/");
    if (m_demoOverlay.starts_with("slash-commands-inline"))
      chooseSlashCommand("32:44");
  }
  void ClientApplication::demoSlashCommandStep() {
    const auto require = [](bool value, const char* reason) {
      if (!value)
        throw std::runtime_error(reason);
    };
    const auto setDraft = [&](const std::string& value) {
      m_drafts[m_channelId] = value;
      m_inputs.at("composer")->setValue(value);
      m_inputs.at("composer")->setEnabled(true);
      m_focusedInput = "composer";
      focusControl("composer");
      m_completionDismissed.clear();
      updateCompletion();
    };
    const auto choose = [&](const std::string& name) {
      const auto command = std::ranges::find(m_slashCommands, name, &api::ApplicationCommand::name);
      require(command != m_slashCommands.end(), "The synthetic command exists");
      chooseSlashCommand(command->key());
    };
    const auto click = [&](Action action, const std::string& id) {
      const auto hit =
          std::ranges::find_if(m_hits, [&](const auto& hit) { return hit.action == action && hit.id == id; });
      require(hit != m_hits.end(), "The command replay clicks a visible native target");
      const float x = hit->x + hit->width / 2;
      const float y = hit->y + hit->height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, BTN_LEFT);
      m_window.released();
    };
    const auto popup = [&](CompletionItem::Kind kind, const std::string& id) {
      const auto item =
          std::ranges::find_if(m_completionItems, [&](const auto& item) { return item.kind == kind && item.id == id; });
      require(item != m_completionItems.end(), "The inline option is available in the popup");
      click(Action::CompleteText, std::to_string(item - m_completionItems.begin()));
    };
    const auto complete = [&](const std::string& error = std::string{}) {
      api::Event event{.operation = api::Operation::ComponentResult, .session = m_session, .error = error};
      event.nonce = m_slashNonce;
      slashCommandEvent(event);
    };
    const auto field = [&](std::size_t index) -> Input& {
      auto* input = m_inputs.at("slash-field:" + std::to_string(index));
      require(input->parent(), "The active inline argument uses a mounted native input");
      return *input;
    };
    const auto write = [&](Input& input, const std::string& value) {
      TextInputEdit edit;
      edit.hasCommitText = true;
      edit.commitText = value;
      input.textInputApplyEdit(edit);
    };
    const auto step = m_demoInteractionStep++;
    switch (step) {
    case 0:
      require(
          m_completionQuery && m_completionQuery->trigger == '/' && m_completionItems.size() > 20,
          "A slash opens the app command picker"
      );
      click(Action::SlashFilter, "30");
      require(m_slashFilter == "30" && m_completionItems.size() == 2, "The app rail filters its commands");
      break;
    case 1:
      onKey({XKB_KEY_Left});
      require(m_slashFilter == "32", "Keyboard arrows switch apps");
      filterSlashCommands("*");
      setDraft("/echo");
      onKey({XKB_KEY_Return});
      require(
          slashComposerActive() && m_dialogKind.empty() && !m_slashPending,
          "Selecting a command starts inline editing without opening a dialog or submitting"
      );
      break;
    case 2:
      require(
          field(0).parent() && m_dialogBounds.height == 0,
          "Arguments stay in the chat composer without a modal backdrop"
      );
      require(
          m_completionBounds.y + m_completionBounds.height <= m_slashComposerFrame.y,
          "Argument suggestions are anchored above the composer"
      );
      submitSlashCommand();
      require(!m_slashPending && !m_slashError.empty(), "Missing required values are rejected locally");
      click(Action::CancelSlashCommand, "");
      require(
          !m_slashCommand && m_drafts[m_channelId] == "/echo",
          "The compact close control preserves the original slash draft"
      );
      setDraft("/echo");
      choose("echo");
      break;
    case 3:
      focusControl("slash-field:0");
      write(field(0), "Hello from the inline composer");
      require(
          m_slashValues[0] == "Hello from the inline composer", "Typing edits the argument in the native composer input"
      );
      onKey({.symbol = XKB_KEY_Return, .shift = true});
      require(m_slashValues[0].ends_with('\n') && !m_slashPending, "Shift+Enter adds a line without sending");
      onKey({.symbol = XKB_KEY_z, .control = true});
      require(m_slashValues[0] == "Hello from the inline composer", "Native undo retains argument edits");
      onKey({XKB_KEY_Tab});
      require(!m_slashActiveOption && m_focusedInput == "slash-options", "Tab reaches the remaining option selector");
      break;
    case 4:
      require(
          m_inputs.at("slash-options")->parent() && m_completionItems.size() == 2,
          "Unset optional arguments appear in the popup above the composer"
      );
      popup(CompletionItem::SlashOption, "1");
      require(m_slashActiveOption == 1 && !m_slashPending, "Choosing an optional argument adds an inline field");
      break;
    case 5:
      require(field(1).parent() && m_completionItems.size() == 2, "Boolean arguments offer native popup choices");
      onKey({XKB_KEY_Down});
      onKey({XKB_KEY_Return});
      require(
          m_slashValues[1] == "false" && field(1).displayValue() == "False" && !m_slashPending,
          "Enter chooses the readable value while retaining its canonical source"
      );
      onKey({.symbol = XKB_KEY_z, .control = true});
      require(m_slashValues[1].empty(), "Choosing a value is one native undoable edit");
      onKey({.symbol = XKB_KEY_z, .control = true, .shift = true});
      require(m_slashValues[1] == "false", "Redo restores the typed wire value");
      onKey({XKB_KEY_Escape});
      onKey({XKB_KEY_Return});
      require(m_slashPending && !m_slashNonce.empty(), "Enter sends the completed command directly from the composer");
      {
        const auto nonce = m_slashNonce;
        onKey({XKB_KEY_Return});
        require(m_slashNonce == nonce, "Repeated Enter does not submit a pending command twice");
      }
      break;
    case 6: {
      complete("Synthetic application failure");
      require(
          !m_slashPending && !m_slashError.empty() && !m_slashValues[0].empty() && m_slashValues[1] == "false",
          "Failure preserves inline arguments and does not open a dialog"
      );
      submitSlashCommand();
      const auto previous = m_slashNonce;
      complete();
      require(
          !m_slashCommand && m_drafts[m_channelId].empty() && !m_slashUsage.empty() && m_focusedInput == "composer",
          "Completion consumes only the submitted slash draft"
      );
      api::Event late{.operation = api::Operation::Interaction, .error = "Late HTTP failure"};
      late.interaction = api::Interaction{.kind = api::InteractionKind::ApplicationCommand, .nonce = previous};
      slashCommandEvent(late);
      require(!m_slashCommand, "Late HTTP failures cannot restore a completed command");
      break;
    }
    case 7:
      setDraft("/");
      filterSlashCommands("~");
      require(
          m_completionItems.size() == 1 && m_completionItems.front().name == "/echo",
          "Frequent commands retain session usage"
      );
      choose("docs");
      break;
    case 8:
      require(
          m_dialogKind.empty()
              && !m_slashActiveOption
              && m_completionItems.size() == 1
              && m_completionItems.front().name == "page"
              && m_inputs.at("slash-options")->value().empty(),
          "An optional-only command shows its name, option count and the Options popup without opening a field"
      );
      onKey({XKB_KEY_Escape});
      require(!m_completionQuery && m_slashCommand, "Escape dismisses the popup before cancelling the command");
      {
        float left = 0, top = 0, right = 0, bottom = 0;
        Node::transformedBounds(m_inputs.at("slash-options"), left, top, right, bottom);
        const float x = (left + right) / 2;
        const float y = (top + bottom) / 2;
        m_window.click(x, y, BTN_LEFT);
        m_window.released();
        updateCompletion();
        require(m_completionQuery.has_value(), "Clicking the focused option selector reopens dismissed options");
      }
      onKey({XKB_KEY_Tab});
      require(m_slashActiveOption == 0 && !m_slashPending, "Tab selects the optional page argument without sending");
      break;
    case 9: {
      write(field(0), "sta");
      m_slashChoiceOption = 0;
      m_slashAutocompleteNonce = "999";
      m_slashAutocompleteSnapshot = nlohmann::json(m_slashValues).dump();
      api::Event choices{.operation = api::Operation::CommandChoices, .nonce = "998"};
      choices.commandChoices = {{"Starlight", "star-42"}};
      slashCommandEvent(choices);
      require(m_slashChoices.empty(), "Stale suggestion nonces cannot populate an argument");
      choices.nonce = "999";
      slashCommandEvent(choices);
      require(m_slashChoices.size() == 1, "App suggestions populate the active inline argument");
      break;
    }
    case 10: {
      const auto& argument = m_slashComposerLayout.items.at(1);
      const float center = m_slashComposerFrame.y
          + layout::SlashCommands::composerHeader
          + layout::SlashCommands::composerPadding
          + argument.y
          - m_slashComposerScroll
          + argument.height / 2;
      unsigned centered = 0;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text) {
          const auto& text = static_cast<const TextNode&>(node);
          if (text.text() == "page" || text.text() == "sta") {
            float baseline = 0;
            for (auto* at = &node; at; at = at->parent())
              baseline += at->y();
            const auto bounds = m_window.target().renderer().measureFont(text.fontSize(), text.fontWeight());
            require(
                std::abs(baseline - bounds.capHeight / 2 - center) <= Style::borderWidth,
                "Inline argument names and typed values share the vertical center of their chip"
            );
            ++centered;
          }
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *field(0).parent());
      require(centered == 2, "The centered inline argument includes its name and native typed value");
      TextInputEdit preedit;
      preedit.hasPreedit = true;
      preedit.preeditText = "temporary";
      preedit.preeditCursorBegin = preedit.preeditCursorEnd = preedit.preeditText.size();
      field(0).textInputApplyEdit(preedit);
      onKey({XKB_KEY_Return});
      require(
          !m_slashPending && m_slashValues[0] == "sta",
          "Input-method composition takes priority over selection and sending"
      );
      preedit.preeditText.clear();
      preedit.preeditCursorBegin = preedit.preeditCursorEnd = 0;
      field(0).textInputApplyEdit(preedit);
      m_completionDirty = true;
      updateCompletion();
      onKey({XKB_KEY_Return});
      require(
          m_slashValues[0] == "star-42" && field(0).displayValue() == "Starlight" && !m_slashPending,
          "Suggestion labels remain readable without replacing their native wire source"
      );
      onKey({XKB_KEY_Return});
      require(m_slashPending, "A second Enter sends after the suggestion was selected");
      break;
    }
    case 11: {
      api::Event event{.operation = api::Operation::ComponentResult, .nonce = m_slashNonce};
      event.modal = api::ApplicationModal{
          .id = "601", .application = "99", .channel = m_channelId, .customId = "details", .title = "Details"
      };
      slashCommandEvent(event);
      require(m_slashCommand && m_dialogKind.empty(), "Unrelated apps cannot open a follow-up form");
      event.modal->application = "32";
      event.modal->fields = {{.customId = "note", .label = "Note"}};
      slashCommandEvent(event);
      require(
          m_dialogKind == "application-modal" && !m_slashCommand,
          "Actual app follow-up forms retain their native modal path"
      );
      break;
    }
    case 12:
      require(m_inputs.at("application-field:0")->parent(), "A genuine app follow-up text field still renders");
      closeDialog();
      setDraft("/logging moderation");
      choose("logging");
      break;
    case 13:
      require(!m_slashChoices.empty(), "Member arguments offer cached people immediately");
      onKey({XKB_KEY_Return});
      require(
          api::validSnowflake(m_slashValues[0]) && field(0).displayValue() != m_slashValues[0],
          "People are readable while their IDs stay in the native source"
      );
      onKey({XKB_KEY_Tab});
      require(m_slashActiveOption == 1 && !m_slashPending, "Tab advances to the next required argument");
      break;
    case 14: {
      require(!m_slashChoices.empty(), "The next inline argument offers type-filtered channels");
      onKey({XKB_KEY_Return});
      const auto target = slashValues()[1];
      const auto channel = std::ranges::find(m_channels, target, &api::Channel::id);
      require(channel != m_channels.end(), "The selected channel belongs to the current directory");
      const auto type = channel->type;
      channel->type = int(api::ChannelType::GuildVoice);
      bool rejected = false;
      try {
        (void)slashValues();
      } catch (const std::exception&) {
        rejected = true;
      }
      channel->type = type;
      require(rejected, "Previously selected channels still receive fresh type validation");
      selectSlashOption({});
      break;
    }
    case 15:
      click(Action::SlashOption, "0");
      require(m_slashActiveOption == 0, "Completed argument chips can be reopened for editing");
      cancelSlashCommand();
      setDraft("/upload");
      choose("upload");
      break;
    case 16: {
      m_slashFileRequest = ++m_serverUploadSerial;
      m_slashFileOption = 0;
      PickedFile file{.request = m_slashFileRequest, .mime = "text/plain", .name = "notes.txt", .bytes = {'h', 'i'}};
      require(
          slashAttachmentResult(file) && m_slashUploads.size() == 1,
          "Attachment arguments retain bounded local chooser results"
      );
      submitSlashCommand();
      require(m_slashPending, "Inline attachments use the existing command submission path");
      complete("Synthetic upload failure");
      require(m_slashUploads.size() == 1 && !m_slashPending, "Upload failures retain the attachment chip");
      break;
    }
    case 17:
      click(Action::SlashAttachment, "0");
      require(
          m_slashUploads.empty() && !m_slashSelectedOptions.contains(0),
          "The attachment chip has a native remove target"
      );
      cancelSlashCommand();
      setDraft("/docs");
      choose("docs");
      break;
    case 18:
      m_slashCommand->permissions = {.user = false, .specified = true};
      submitSlashCommand();
      require(!m_slashPending && !m_slashError.empty(), "Enter submission rechecks current command permissions");
      cancelSlashCommand();
      setDraft("/docs");
      choose("docs");
      submitSlashCommand();
      m_drafts[m_channelId] = "A newer draft";
      m_inputs.at("composer")->setValue("A newer draft");
      complete();
      require(m_drafts[m_channelId] == "A newer draft", "Delayed completion preserves newer ordinary composer text");
      break;
    case 19:
      setDraft("/");
      filterSlashCommands("*");
      m_slashAppScroll = layout::SlashCommands::appStep * 10;
      break;
    case 20:
      require(
          m_slashAppScroll > 0 && m_completionBounds.x + m_completionBounds.width <= m_window.width(),
          "The app rail still scrolls inside the bounded picker"
      );
      openContext(
          m_completionBounds.x + layout::SlashCommands::railWidth + Style::spaceMd,
          m_completionBounds.y + layout::SlashCommands::headerHeight + Style::spaceMd
      );
      require(!m_contextOpen, "Command popups isolate chat context targets beneath them");
      setDraft("/no-such-command");
      send();
      require(m_drafts[m_channelId] == "/no-such-command" && !m_sending, "Unknown slash text is never sent as chat");
      break;
    case 21: {
      api::Event stale{.operation = api::Operation::CommandIndex};
      stale.commandIndex = api::CommandIndexQuery{m_channelId, m_guildId, false, m_slashRevision - 1};
      stale.commands = m_slashCommands;
      const auto commands = m_slashCommands;
      slashCommandEvent(stale);
      require(m_slashCommands.size() == commands.size(), "Stale discovery generations cannot replace the index");
      api::Event invalidation{.operation = api::Operation::CommandIndex, .resource = m_guildId, .live = true};
      slashCommandEvent(invalidation);
      require(m_slashCommands.empty(), "Live permission and command changes invalidate discovery");
      m_slashIndexPending = {false, true};
      api::Event userIndex{.operation = api::Operation::CommandIndex};
      userIndex.commandIndex = api::CommandIndexQuery{m_channelId, m_guildId, true, m_slashRevision};
      userIndex.commands = commands;
      slashCommandEvent(userIndex);
      auto serverIndex = userIndex;
      serverIndex.commandIndex->user = false;
      serverIndex.commands.front().permissions = {.user = false, .specified = true};
      slashCommandEvent(serverIndex);
      m_slashIndexPending.insert(true);
      slashCommandEvent(userIndex);
      require(
          m_slashCommands.front().permissions.user == false,
          "Scoped permissions survive overlapping account-index completions"
      );
      m_slashCommands = commands;
      m_slashIndexLoaded = true;
      setDraft("/configure");
      choose("configure");
      for (std::size_t i = 0; i < m_slashValues.size(); ++i) {
        m_slashSelectedOptions.insert(i);
        m_slashValues[i] = "A longer selected argument";
      }
      selectSlashOption(m_slashValues.size() - 1);
      break;
    }
    case 22: {
      require(
          m_slashComposerScroll > 0 && m_slashComposerLayout.contentHeight > m_slashComposerLayout.viewportHeight,
          "Many arguments wrap and scroll within the bounded composer"
      );
      const auto& frame = m_slashComposerLayout.items[m_slashValues.size()];
      require(
          frame.y >= m_slashComposerScroll
              && frame.y + frame.height
                  <= m_slashComposerScroll + m_slashComposerLayout.viewportHeight + Style::borderWidth,
          "The active wrapped argument remains completely visible"
      );
      onKey({.symbol = XKB_KEY_Tab, .shift = true});
      require(m_slashActiveOption == m_slashValues.size() - 2, "Shift+Tab moves backward across selected arguments");
      break;
    }
    case 23:
      selectSlashOption({});
      break;
    case 24:
      require(
          m_inputs.at("slash-options")->parent() && m_slashComposerScroll > 0,
          "The trailing selector is revealed after the last wrapped argument"
      );
      click(Action::CancelSlashCommand, "");
      require(
          !m_slashCommand && m_drafts[m_channelId] == "/configure",
          "Closing the inline command preserves the slash draft"
      );
      setDraft("/docs");
      choose("docs");
      submitSlashCommand();
      {
        const auto nonce = m_slashNonce;
        selectChannel(m_channels.back().id);
        require(
            !m_slashCommand && m_slashNonce.empty(), "Navigation retires command inputs, timers and pending identities"
        );
        api::Event stale{.operation = api::Operation::ComponentResult, .nonce = nonce};
        slashCommandEvent(stale);
        require(!m_slashCommand && m_dialogKind.empty(), "A delayed response cannot reopen a departed inline editor");
      }
      break;
    default:
      clearSlashCommands();
      require(
          m_slashCommands.empty() && m_slashUsage.empty() && m_slashUploads.empty() && m_slashNonce.empty(),
          "Account reset clears command data and inline state"
      );
      break;
    }
    std::clog << "Offline inline slash command step " << step + 1 << " passed\n";
    m_window.invalidate();
  }
} // namespace app

#include "api/actions.h"
#include "api/application_commands.h"
#include "api/command_uploads.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
  using Json = nlohmann::json;
  void require(bool value, const char* reason) {
    if (!value)
      throw std::runtime_error(reason);
  }
  template <class F> void rejects(F operation, const char* reason) {
    bool rejected = false;
    try {
      operation();
    } catch (const std::exception&) {
      rejected = true;
    }
    require(rejected, reason);
  }
  Json index() {
    return Json::parse(R"JSON({
      "applications":[{"id":"30","name":"Orbit","bot_id":"31","icon":null}],
      "application_commands":[{"id":"40","version":"50","application_id":"30","guild_id":"10",
        "name":"logging","description":"Configure logging","type":1,
        "options":[{"type":2,"name":"events","description":"Event logging","options":[
          {"type":1,"name":"set","description":"Choose a log channel","options":[
            {"type":7,"name":"channel","description":"Destination","required":true,"channel_types":[0]},
            {"type":5,"name":"enabled","description":"Enable logging"},
            {"type":4,"name":"count","description":"Number of events","min_value":1,"max_value":20},
            {"type":10,"name":"ratio","description":"Ratio","min_value":0,"max_value":1},
            {"type":3,"name":"label","description":"Label","min_length":2,"max_length":8},
            {"type":6,"name":"user","description":"Person"},
            {"type":8,"name":"role","description":"Role"},
            {"type":9,"name":"target","description":"Person or role"},
            {"type":11,"name":"file","description":"Attachment"}
          ]}]}]}]
    })JSON");
  }
} // namespace
int main() {
  try {
    using namespace api;
    const CommandIndexQuery query{"20", "10", false, 1};
    require(
        commandIndexPath(query) == "/guilds/10/application-command-index", "Server discovery uses the scoped GET route"
    );
    require(
        commandIndexPath({"20", "", false}) == "/channels/20/application-command-index",
        "DM discovery is channel scoped"
    );
    require(
        commandIndexPath({"20", "10", true}) == "/users/@me/application-command-index",
        "User-installed commands use their own index"
    );
    require(commandIndexPath({"../@me", "10"}).empty(), "Route identities cannot contain paths");
    require(
        commandIndexPath({"0", "10"}).empty() && commandIndexPath({"18446744073709551616", "10"}).empty(),
        "Command identifiers reject zero and overflowing snowflakes"
    );
    const auto commands = parseCommandIndex(index(), query);
    require(
        commands.size() == 1 && commands[0].displayName() == "/logging events set",
        "Subcommand groups are flattened without losing their path"
    );
    const auto& command = commands.front();
    const auto data = applicationCommandData(command, {"21", "false", "12", "0.5", "Résumé", "60", "70", "80", "0"});
    require(
        data["id"] == "40" && data["version"] == "50" && data["name"] == "logging" && data["guild_id"] == "10",
        "Invocations carry the root identity and version"
    );
    const auto& options = data["options"][0]["options"][0]["options"];
    require(
        data["options"][0]["type"] == 2
            && data["options"][0]["options"][0]["type"] == 1
            && options[0]["value"] == "21"
            && options[1]["value"] == false
            && options[2]["value"] == 12
            && options[3]["value"] == 0.5
            && options[4]["value"] == "Résumé"
            && options[8]["value"] == "0",
        "Nested payloads retain numeric, boolean, text, entity and attachment wire types"
    );
    Interaction action{
        .kind = InteractionKind::ApplicationCommand,
        .channel = "20",
        .guild = "10",
        .application = "30",
        .nonce = "12345",
        .values = {"21", "true", "", "", "", "", "", "", ""},
        .command = command
    };
    const auto request = interactionRequest(action);
    require(
        request
            && request->path == "/interactions"
            && request->method == "POST"
            && request->body["type"] == 2
            && request->body["application_id"] == "30"
            && !request->body.contains("session_id")
            && !request->body.contains("content")
            && !request->body.contains("message_id"),
        "Commands use the interaction envelope and keep private session identity in the worker"
    );
    action.application = "32";
    require(!interactionRequest(action), "Application identity mismatches are rejected before enqueue");
    action.application = "30";
    action.guild = "11";
    require(!interactionRequest(action), "A server command cannot run in another server");
    rejects([&] { applicationCommandData(command, {""}); }, "Option count mismatch is rejected");
    const std::vector<std::string> empty(command.options.size());
    rejects([&] { applicationCommandData(command, empty); }, "Required options cannot be omitted");
    for (const auto& [position, value] : std::vector<std::pair<std::size_t, std::string>>{
             {0, "21/route"},
             {1, "yes"},
             {2, "2.5"},
             {2, "0"},
             {2, "21"},
             {2, "9007199254740993"},
             {3, "nan"},
             {3, "inf"},
             {3, "1.1"},
             {4, "a"},
             {4, "123456789"},
             {8, "-1"}
         }) {
      auto values = empty;
      values[0] = "21";
      values[position] = value;
      rejects(
          [&] { applicationCommandData(command, values); }, "Typed values and advertised constraints are validated"
      );
    }
    for (const auto& mutate : std::vector<std::function<void(Json&)>>{
             [](Json& v) { v["application_commands"][0]["guild_id"] = "11"; },
             [](Json& v) { v["application_commands"][0]["application_id"] = "32"; },
             [](Json& v) { v["application_commands"][0]["version"] = "../50"; },
             [](Json& v) { v["application_commands"].push_back(v["application_commands"][0]); },
             [](Json& v) { v["applications"].push_back(v["applications"][0]); },
             [](Json& v) { v["application_commands"][0]["options"][0]["options"][0]["options"][0]["type"] = 99; },
             [](Json& v) { v["application_commands"][0]["options"][0]["options"][0]["options"][4]["min_length"] = -1; },
             [](Json& v) {
               v["application_commands"][0]["options"][0]["options"][0]["options"][4]["max_length"] =
                   commandStringLimit + 1;
             },
             [](Json& v) { v["applications"][0]["permissions"] = {{"channels", {{"../20", true}}}}; }
         }) {
      auto invalid = index();
      mutate(invalid);
      rejects([&] { parseCommandIndex(invalid, query); }, "Malformed or unscoped indexes fail explicitly");
    }
    auto localized = index();
    localized["application_commands"][0]["name"] = "journal";
    localized["application_commands"][0]["name_default"] = "logging";
    const auto translated = parseCommandIndex(localized, query).front();
    require(
        translated.displayName() == "/journal events set"
            && applicationCommandData(translated, {"21", "", "", "", "", "", "", "", ""})["name"] == "logging",
        "Localized display names do not replace canonical wire names"
    );
    auto choices = command;
    choices.path.clear();
    choices.options = {
        {.type = CommandOptionType::String,
         .name = "query",
         .label = "query",
         .required = true,
         .choices = {{"Friendly label", "wire-value"}}}
    };
    require(
        applicationCommandData(choices, {"wire-value"})["options"][0]["value"] == "wire-value",
        "Static choices submit their typed value"
    );
    rejects(
        [&] { applicationCommandData(choices, {"Friendly label"}); },
        "Choice labels are not accepted as arbitrary values"
    );
    choices.options[0].choices.clear();
    choices.options[0].autocomplete = true;
    action = {
        .kind = InteractionKind::CommandAutocomplete,
        .channel = "20",
        .guild = "10",
        .application = "30",
        .nonce = "12346",
        .values = {"partial"},
        .command = choices,
        .focusedOption = 0
    };
    const auto autocomplete = interactionRequest(action);
    require(
        autocomplete && autocomplete->body["type"] == 4 && autocomplete->body["data"]["options"][0]["focused"] == true,
        "Autocomplete is a separate focused interaction, without running the command"
    );
    require(
        parseCommandChoices(
            Json::array({{{"name", "Shown"}, {"value", "wire"}}, {{"name", "Number"}, {"value", 3}}})
        ).size()
            == 2,
        "Gateway suggestions retain typed values"
    );
    rejects(
        [&] { parseCommandChoices(Json::array({{{"name", "Bad"}, {"value", Json::object()}}})); },
        "Suggestions cannot contain arbitrary objects"
    );
    CommandContext context{
        .channel = "20", .guild = "10", .user = "60", .roles = {"70"}, .permissions = useApplicationCommands
    };
    require(commandAvailable(command, context), "Ordinary commands are available in the matching server");
    auto allChannels = command;
    allChannels.app.permissions.channels["9"] = false;
    require(
        !commandAvailable(allChannels, context),
        "The all-channels sentinel subtracts the server ID without retaining leading zeroes"
    );
    auto restricted = command;
    restricted.defaultPermissions = 0;
    require(!commandAvailable(restricted, context), "Commands disabled by default require an explicit grant");
    restricted.permissions = {.roles = {{"70", true}}, .specified = true};
    require(commandAvailable(restricted, context), "A role grant can enable a command");
    restricted.permissions.user = false;
    require(!commandAvailable(restricted, context), "A user denial overrides a role grant");
    restricted.permissions.user = true;
    restricted.app.permissions.channels["20"] = false;
    require(
        !commandAvailable(restricted, context), "A command member override retains application channel restrictions"
    );
    restricted.permissions.channels["20"] = true;
    require(
        commandAvailable(restricted, context), "A command channel grant overrides the application-wide channel denial"
    );
    context.thread = true;
    context.parent = "21";
    restricted.app.permissions.channels["21"] = false;
    require(!commandAvailable(restricted, context), "Threads inherit command channel permissions from their parent");
    restricted.permissions.channels["21"] = true;
    require(commandAvailable(restricted, context), "A command grant on the parent also applies inside its threads");
    context.thread = false;
    context.parent.clear();
    restricted.permissions.channels["20"] = false;
    require(!commandAvailable(restricted, context), "Channel denials remain effective after a user grant");
    context.administrator = true;
    require(commandAvailable(restricted, context), "Administrators bypass command permission overrides");
    context.administrator = false;
    context.permissions = 0;
    require(!commandAvailable(command, context), "The channel application-command permission is required when known");
    context.guild.clear();
    context.permissions.reset();
    require(!commandAvailable(command, context), "Server-only commands do not appear in private conversations");
    auto global = command;
    global.guild.clear();
    global.contexts = {1};
    context.recipient = "31";
    require(commandAvailable(global, context), "Bot-DM context is resolved using the application bot identity");
    context.recipient = "32";
    require(!commandAvailable(global, context), "Bot-DM-only commands stay out of other private conversations");
    context.recipient.clear();
    global.app.bot.clear();
    global.contexts = {2};
    require(
        commandAvailable(global, context), "A group DM without a recipient or bot ID uses the private-channel context"
    );
    global.dmPermission = false;
    require(
        commandAvailable(global, context), "Explicit interaction contexts supersede the deprecated DM permission flag"
    );
    const std::vector<Upload> files{{"notes.txt", "text/plain", {'h', 'i'}}};
    require(validCommandUploads(files), "Command attachments can use content-detected file types");
    require(!validUploads(files), "Ordinary message attachment behavior remains separate");
    auto attachmentCommand = command;
    attachmentCommand.options = {
        {.type = CommandOptionType::Attachment, .name = "file", .label = "file", .required = true}
    };
    require(
        validCommandAttachments(attachmentCommand, {"0"}, files),
        "Attachment option indexes bind to the selected file array"
    );
    require(
        !validCommandAttachments(attachmentCommand, {"1"}, files)
            && !validCommandAttachments(attachmentCommand, {""}, files),
        "Unbound or unreferenced command uploads are rejected"
    );
    require(
        commandUploadRequest(files)
            == Json{{"files", Json::array({{{"id", "0"}, {"filename", "notes.txt"}, {"file_size", 2}}})}},
        "Attachment reservation sends bounded metadata before the interaction"
    );
    const Json slotResponse{
        {"attachments",
         Json::array(
             {{{"id", "0"},
               {"upload_url",
                "https://discord-attachments-uploads-prd.storage.googleapis.com/slot/notes.txt?signature=synthetic"},
               {"upload_filename", "slot/notes.txt"}}}
         )}
    };
    const auto slots = commandUploadSlots(slotResponse, files);
    require(
        commandUploadedAttachments(slots, files)
            == Json::array({{{"id", "0"}, {"filename", "notes.txt"}, {"uploaded_filename", "slot/notes.txt"}}}),
        "Interaction attachments refer to staged file identities without exposing upload destinations"
    );
    for (const auto& url :
         {"http://discord-attachments-uploads-prd.storage.googleapis.com/file", "https://private.invalid/file",
          "https://discord-attachments-uploads-prd.storage.googleapis.com.evil.invalid/file",
          "https://discord-attachments-uploads-prd.storage.googleapis.com@evil.invalid/file",
          "https://discord-attachments-uploads-prd.storage.googleapis.com/file#fragment"}) {
      auto invalid = slotResponse;
      invalid["attachments"][0]["upload_url"] = url;
      rejects(
          [&] { commandUploadSlots(invalid, files); }, "Staging rejects untrusted hosts, schemes and URL confusion"
      );
    }
    auto mismatched = slotResponse;
    mismatched["attachments"][0]["id"] = "1";
    rejects([&] { commandUploadSlots(mismatched, files); }, "Staging responses must match the reserved file indexes");
    auto invalidFiles = files;
    invalidFiles[0].mime = "text/plain\r\nInjected: yes";
    require(!validCommandUploads(invalidFiles), "Attachment MIME metadata cannot inject headers");
    invalidFiles = files;
    invalidFiles[0].filename = "../notes.txt";
    require(!validCommandUploads(invalidFiles), "Command filenames cannot carry paths");
    std::cout << "Application command protocol checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

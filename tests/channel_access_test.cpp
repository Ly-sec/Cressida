#include "app/channel_access.h"

#include <iostream>
#include <stdexcept>

int main() {
  const auto require = [](bool value, const char* reason) {
    if (!value)
      throw std::runtime_error(reason);
  };
  try {
    using app::ChannelAccess;
    const auto channel = [](api::ChannelType type, std::optional<bool> joined = {}) {
      api::Channel item;
      item.id = "1";
      item.type = static_cast<int>(type);
      item.joined = joined;
      return item;
    };
    const auto text = channel(api::ChannelType::GuildText);
    const std::uint64_t view = api::viewChannel;
    const std::uint64_t send = api::sendMessages;
    const std::uint64_t history = api::permission::readMessageHistory;

    const auto unknown = app::resolveChannelAccess(text, std::nullopt);
    require(unknown.access == ChannelAccess::Unknown, "Missing permission data stays Unknown");
    require(!unknown.allows(0) && !unknown.allows(view), "Unknown access never grants a permission");
    require(!app::channelActivityVisible(text, unknown), "Unknown access hides activity");

    const auto denied = app::resolveChannelAccess(text, send | history);
    require(denied.denied(), "Missing View Channel denies access");
    require(!denied.allows(send), "Other bits never apply without View Channel");
    require(!app::channelActivityVisible(text, denied), "Denied access hides activity");

    const auto granted = app::resolveChannelAccess(text, view | send);
    require(granted.granted() && granted.allows(send) && granted.allows(view | send), "Granted bits are allowed");
    require(!granted.allows(history) && !granted.allows(send | history), "Every requested bit must be present");
    require(app::channelActivityVisible(text, granted), "Granted channels show activity");

    const auto category = channel(api::ChannelType::GuildCategory);
    require(
        !app::channelActivityVisible(category, app::resolveChannelAccess(category, view)),
        "Categories never carry activity of their own"
    );

    const auto privateThread = channel(api::ChannelType::PrivateThread, false);
    require(app::resolveChannelAccess(privateThread, view).denied(), "Unjoined private threads require Manage Threads");
    require(
        app::resolveChannelAccess(privateThread, view | api::manageThreads).granted(),
        "Manage Threads opens unjoined private threads"
    );
    const auto joinedPrivate = channel(api::ChannelType::PrivateThread, true);
    require(app::resolveChannelAccess(joinedPrivate, view).granted(), "Joined private threads are readable");
    require(
        app::resolveChannelAccess(channel(api::ChannelType::PublicThread), view).granted(),
        "Public threads follow their parent's View Channel"
    );

    require(app::channelListed(text, unknown, std::nullopt), "Unknown channels stay listed without guild data");
    require(app::channelListed(text, unknown, view), "Unknown channels follow a server-wide View Channel");
    require(!app::channelListed(text, unknown, send), "A server-wide denial hides Unknown channels");
    require(!app::channelListed(text, denied, view), "Known denials override the server-wide fallback");
    require(app::channelListed(text, granted, std::nullopt), "Granted channels are listed");
    require(app::channelListed(joinedPrivate, unknown, send), "Joined threads stay listed while permissions load");

    namespace p = api::permission;
    require(!p::allows(std::nullopt, send) && !p::allowsAny(std::nullopt, send), "Missing data allows nothing");
    require(p::allows(p::administrator, send | history), "Administrator implies every bit");
    require(p::allowsAny(send, send | history) && !p::allows(send, send | history), "Any and all differ");

    for (const auto check :
         {app::can::addReaction, app::can::joinReaction, app::can::attachFiles, app::can::createPublicThread,
          app::can::manageMessages, app::can::editChannel, app::can::createInvite, app::can::connect})
      require(!check(unknown) && !check(denied), "Unknown or denied access hides every action");

    const auto reader = app::resolveChannelAccess(text, view | history);
    require(app::can::joinReaction(reader) && !app::can::addReaction(reader), "Joining needs only history");
    require(
        app::can::addReaction(app::resolveChannelAccess(text, view | history | p::addReactions)),
        "New reactions need Add Reactions and history"
    );
    require(!app::can::addReaction(app::resolveChannelAccess(text, view | p::addReactions)), "Reactions need history");
    require(!app::can::attachFiles(granted) && !app::can::createPublicThread(granted), "Absent bits stay hidden");
    require(app::can::attachFiles(app::resolveChannelAccess(text, view | p::attachFiles)), "Attach Files allows files");
    require(
        app::can::createPublicThread(app::resolveChannelAccess(text, view | p::createPublicThreads)),
        "Create Public Threads allows thread creation"
    );
    require(
        app::can::editChannel(app::resolveChannelAccess(text, view | p::managePermissions))
            && !app::can::manageChannel(app::resolveChannelAccess(text, view | p::managePermissions)),
        "Either management bit opens channel settings"
    );
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}

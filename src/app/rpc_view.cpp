#include "app/client_application.h"
#include "app/theme.h"
#include "core/input/key_symbols.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/text_node.h"
#include "render/text/glyph_registry.h"
#include "ui/controls/button.h"
#include "ui/controls/collapsible.h"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <limits>
#include <linux/input-event-codes.h>
#include <nlohmann/json.hpp>

namespace app {
  namespace {
    constexpr std::size_t maximumRpcApplications = 32, maximumRpcMetadata = 128;
    void activityPosition(const Node* scene, const Node* node, float localX, float localY, float& x, float& y) {
      float sceneX = 0, sceneY = 0;
      Node::mapToScene(node, localX, localY, sceneX, sceneY);
      (void)Node::mapFromScene(scene, sceneX, sceneY, x, y);
    }
    std::string activityGlyph(int type) {
      return type == 2             ? "activity-listening"
          : type == 1 || type == 3 ? "activity-watching"
          : type == 5              ? "activity-competing"
                                   : "activity-playing";
    }
    std::string activityButtonAction(const std::string& user, const rpc::Activity& activity, std::size_t index) {
      return "rpc:button:"
          + user
          + ":"
          + activity.application
          + ":"
          + std::to_string(activity.fields.value("type", 0))
          + ":"
          + std::to_string(index);
    }
    std::optional<std::int64_t> activityNumber(const std::string& text) {
      if (text.empty())
        return {};
      std::int64_t value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? std::optional(value) : std::nullopt;
    }
  } // namespace
  void ClientApplication::rpcClear() {
    m_activityTimer.stop();
    if (m_activityButtonRequest)
      m_activityButtonRequest->revoked->store(true);
    m_activityButtonRequest.reset();
    for (auto& [connection, application] : m_rpcApplications)
      application.revoked->store(true);
    m_rpcApplications.clear();
    m_rpcMetadata.clear();
    m_rpcLookups.clear();
    if (!m_demo && !m_rpcPublished.empty())
      m_client.setActivities({});
    m_rpcPublished.clear();
    if (!m_account.id.empty()) {
      api::UserUpdates update;
      update.users[m_account.id].activities = std::vector<rpc::Activity>{};
      m_userDirectory.apply(update);
    }
    m_rpcConfiguration = {};
    m_rpc.configure({});
    m_rpcStatus.clear();
  }
  void ClientApplication::rpcProtectPublishedActivity(api::UserUpdates& updates) const {
    if (!m_preferences.rpcEnabled || m_rpcPublished.empty())
      return;
    if (auto self = updates.users.find(m_account.id); self != updates.users.end() && self->second.activities)
      self->second.activities = m_rpcPublished;
  }
  void ClientApplication::rpcTick() {
    if (m_demo)
      return;
    const bool enabled = m_connected && !m_account.id.empty() && m_preferences.rpcEnabled;
    rpc::Configuration configuration{
        enabled ? m_session : 0,
        enabled ? nlohmann::json{{"id", m_account.id}, {"username", m_account.name}, {"discriminator", "0"}}
                : nlohmann::json(nullptr),
        enabled, m_preferences.rpcBlocked
    };
    if (configuration != m_rpcConfiguration) {
      if (configuration.session != m_rpcConfiguration.session || !enabled)
        rpcClear();
      m_rpcConfiguration = configuration;
      m_rpc.configure(std::move(configuration));
    }
    for (auto& update : m_rpc.drain()) {
      if (!enabled || update.session != m_session)
        continue;
      const auto previous = m_rpcApplications.find(update.connection);
      if (update.disconnected || (update.command == "SET_ACTIVITY" && !update.activity)) {
        if (previous != m_rpcApplications.end()) {
          if (previous->second.replyPending)
            m_rpc.complete(previous->second.request, {}, "Activity was cleared.");
          previous->second.revoked->store(true);
          m_rpcApplications.erase(previous);
        }
        if (!update.disconnected)
          m_rpc.complete(update, {});
        rpcPublish();
        continue;
      }
      if (update.command != "SET_ACTIVITY") {
        m_rpc.complete(update, {}, "No matching activity join request is pending.");
        continue;
      }
      if (m_rpcApplications.size() >= maximumRpcApplications && previous == m_rpcApplications.end()) {
        m_rpc.complete(update, {}, "Too many local applications are connected.");
        continue;
      }
      if (previous != m_rpcApplications.end())
        if (previous->second.replyPending) {
          m_rpc.complete(previous->second.request, {}, "Replaced by a newer activity update.");
          previous->second.replyPending = false;
        }
      if (previous != m_rpcApplications.end())
        previous->second.revoked->store(true);
      RpcApplication application;
      application.raw = *update.activity;
      application.request = std::move(update);
      application.revision = ++m_rpcRevision;
      m_rpcApplications[application.request.connection] = std::move(application);
      rpcResolve();
    }
    const auto listener = m_rpc.endpoint() + m_rpc.error();
    if (listener != m_rpcListenerState) {
      m_rpcListenerState = listener;
      m_window.invalidate();
    }
    rpcPublish();
  }
  std::string ClientApplication::activitySummary(const std::string& user, bool detailed) const {
    if (m_userDirectory.relationship(user) == 2 || m_userDirectory.ignored(user))
      return {};
    const auto activities = m_userDirectory.activities(user);
    if (activities.empty())
      return {};
    const auto& fields = activities.front().fields;
    const auto name = fields.value("name", std::string{}), details = fields.value("details", std::string{}),
               state = fields.value("state", std::string{});
    if (detailed && fields.value("type", 0) == 2 && !details.empty())
      return state.empty() ? details : state + " - " + details;
    const auto display = fields.value("status_display_type", 0);
    if (display == 1 && !state.empty())
      return state;
    if (display == 2 && !details.empty())
      return details;
    return !name.empty() ? name : !details.empty() ? details : state;
  }
  bool ClientApplication::drawActivitySummary(
      Node& root, const layout::Frame& frame, const std::string& user, float fontSize, bool detailed
  ) {
    const auto text = activitySummary(user, detailed);
    if (text.empty() || frame.width <= 0 || frame.height <= 0)
      return false;
    const auto activities = m_userDirectory.activities(user);
    const auto type = activities.front().fields.value("type", 0);
    const auto icon = activityGlyph(type);
    const float iconSize = std::min(Style::glyphSizeTiny, frame.height);
    glyph(
        root, frame.x, frame.y + (frame.height - iconSize) / 2, iconSize, icon,
        type == 2 ? theme::success : theme::muted
    );
    const auto metrics = m_window.target().renderer().measureText(text, fontSize);
    const float textX = frame.x + iconSize + Style::spaceXs;
    label(
        root, textX, frame.y + (frame.height - (metrics.bottom - metrics.top)) / 2,
        std::max(0.0F, frame.width - iconSize - Style::spaceXs), text, fontSize, theme::muted
    );
    float x = 0, y = 0;
    activityPosition(m_scene.get(), &root, frame.x, frame.y, x, y);
    bool visible = hovered(x, y, frame.width, frame.height);
    for (auto* parent = &root; visible && parent; parent = parent->parent())
      if (parent->clipChildren()) {
        float left = 0, top = 0, right = 0, bottom = 0;
        Node::transformedBounds(parent, left, top, right, bottom);
        (void)Node::mapFromScene(m_scene.get(), left, top, left, top);
        (void)Node::mapFromScene(m_scene.get(), right, bottom, right, bottom);
        visible = hovered(left, top, right - left, bottom - top);
      }
    if (visible) {
      m_tooltip = activitySummary(user, true);
      m_tooltipX = x + frame.width / 2;
      m_tooltipY = y;
    }
    return true;
  }
  void ClientApplication::rpcResolve() {
    for (auto& [connection, application] : m_rpcApplications) {
      if (application.resolved || !application.error.empty() || application.externalPending)
        continue;
      const auto& id = application.raw.application;
      const auto metadata = m_rpcMetadata.find(id);
      if (metadata == m_rpcMetadata.end()) {
        if (m_rpcLookups.contains(id))
          continue;
        const auto revision = ++m_rpcRevision;
        if (!m_demo && m_client.activityApplication(id, revision))
          m_rpcLookups[id] = std::to_string(revision);
        else
          application.error = "Could not load the activity application. Try again.";
        continue;
      }
      if (metadata->second.contains("error")) {
        application.error = metadata->second["error"].get<std::string>();
        continue;
      }
      auto resolved = application.raw;
      if (resolved.fields.value("name", std::string{}).empty())
        resolved.fields["name"] = metadata->second.value("name", id);
      std::string error;
      if (resolved.fields.contains("assets")) {
        for (const auto key : {"large_image", "small_image", "invite_cover_image"}) {
          const auto image = resolved.fields["assets"].value(key, std::string{});
          if (image.empty()
              || image.starts_with("https://")
              || image.starts_with("mp:")
              || image.starts_with("spotify:")
              || image.starts_with("twitch:")
              || image.starts_with("youtube:")
              || api::validSnowflake(image))
            continue;
          const auto& assets = metadata->second["assets"];
          const auto asset = std::ranges::find_if(assets, [&](const auto& entry) {
            return entry.is_object()
                && entry.value("name", std::string{}) == image
                && entry.contains("id")
                && entry["id"].is_string()
                && api::validSnowflake(entry["id"].template get<std::string>());
          });
          if (asset == assets.end()) {
            error = "An artwork key was not found in this application.";
            break;
          }
          resolved.fields["assets"][key] = (*asset)["id"];
        }
      }
      if (!error.empty()) {
        application.error = error;
        if (connection && application.replyPending) {
          m_rpc.complete(application.request, {}, error);
          application.replyPending = false;
        }
        continue;
      }
      const auto urls = rpc::externalImages(resolved);
      if (!urls.empty()) {
        if (!m_demo && m_client.activityExternalAssets(id, urls, application.revision, application.revoked)) {
          application.resolved = std::move(resolved);
          application.externalPending = true;
        } else
          application.error = "Could not resolve activity artwork. Try again.";
        continue;
      }
      application.resolved = std::move(resolved);
      if (connection && application.replyPending) {
        m_rpc.complete(application.request, application.resolved);
        application.replyPending = false;
      }
    }
    for (auto& [connection, application] : m_rpcApplications)
      if (connection && application.replyPending && !application.error.empty()) {
        m_rpc.complete(application.request, {}, application.error);
        application.replyPending = false;
      }
    rpcPublish();
    m_window.invalidate();
  }
  bool ClientApplication::rpcResult(const api::Event& event) {
    if (event.operation == api::Operation::ActivityMetadata) {
      if (event.session != m_session || !m_activityButtonRequest)
        return true;
      auto& request = *m_activityButtonRequest;
      if (!request.pending
          || request.revoked->load()
          || event.nonce != std::to_string(request.revision)
          || event.resource != request.user)
        return true;
      request.pending = false;
      const auto current = m_userDirectory.activities(request.user);
      if (m_userDirectory.relationship(request.user) == 2
          || m_userDirectory.ignored(request.user)
          || std::ranges::find(current, request.source) == current.end())
        return true;
      if (!event.error.empty()
          || !event.activityData
          || !event.activityData->is_array()
          || request.index >= event.activityData->size()
          || !(*event.activityData)[request.index].is_string()) {
        m_status = "Could not open the activity link. Click the button to try again.";
      } else {
        const auto url = (*event.activityData)[request.index].get<std::string>();
        const auto verified = rpc::activityButtonUrls({{"button_urls", nlohmann::json::array({url})}});
        if (verified) {
          request.openedUrl = url;
          if (!m_demo)
            activate({0, 0, 0, 0, Action::OpenUrl, url});
        } else
          m_status = "The activity link was invalid.";
      }
      m_window.invalidate();
      return true;
    }
    if (event.operation != api::Operation::ActivityApplication
        && event.operation != api::Operation::ActivityExternalAssets)
      return false;
    if (event.session != m_session)
      return true;
    if (event.operation == api::Operation::ActivityApplication) {
      const auto lookup = m_rpcLookups.find(event.resource);
      if (lookup == m_rpcLookups.end() || lookup->second != event.nonce)
        return true;
      m_rpcLookups.erase(lookup);
      if (event.error.empty() && event.activityData && m_rpcMetadata.size() < maximumRpcMetadata)
        m_rpcMetadata[event.resource] = *event.activityData;
      else {
        if (m_rpcMetadata.size() < maximumRpcMetadata)
          m_rpcMetadata[event.resource] = {
              {"error", event.error.empty() ? "Could not load activity details. Try again." : event.error}
          };
        for (auto& [connection, application] : m_rpcApplications)
          if (application.raw.application == event.resource) {
            application.error = event.error.empty() ? "Could not load activity details. Try again." : event.error;
            if (connection && application.replyPending) {
              m_rpc.complete(application.request, {}, application.error);
              application.replyPending = false;
            }
          }
      }
    } else {
      for (auto& [connection, application] : m_rpcApplications) {
        if (application.raw.application != event.resource
            || std::to_string(application.revision) != event.nonce
            || !application.externalPending
            || application.revoked->load())
          continue;
        application.externalPending = false;
        auto error = event.error;
        if (error.empty() && event.activityData && event.activityData->is_array()) {
          auto& assets = application.resolved->fields["assets"];
          for (const auto key : {"large_image", "small_image", "invite_cover_image"}) {
            const auto image = assets.value(key, std::string{});
            if (!image.starts_with("https://"))
              continue;
            const auto resolved = std::ranges::find_if(*event.activityData, [&](const auto& item) {
              return item.is_object()
                  && item.contains("url")
                  && item["url"].is_string()
                  && item["url"] == image
                  && item.contains("external_asset_path")
                  && item["external_asset_path"].is_string();
            });
            if (resolved == event.activityData->end()) {
              error = "Activity artwork could not be resolved.";
              break;
            }
            const auto path = (*resolved)["external_asset_path"].get<std::string>();
            if (!path.starts_with("external/") || path.find("..") != std::string::npos || path.size() > 2048) {
              error = "The activity artwork response was invalid.";
              break;
            }
            assets[key] = "mp:" + path;
          }
        } else if (error.empty())
          error = "Activity artwork could not be resolved.";
        application.error = std::move(error);
        if (connection && application.replyPending) {
          m_rpc.complete(
              application.request, application.error.empty() ? application.resolved : std::nullopt, application.error
          );
          application.replyPending = false;
        }
      }
    }
    rpcResolve();
    return true;
  }
  void ClientApplication::activityButton(const std::string& user, const rpc::Activity& activity, std::size_t index) {
    const auto current = m_userDirectory.activities(user);
    if (m_userDirectory.relationship(user) == 2
        || m_userDirectory.ignored(user)
        || std::ranges::find(current, activity) == current.end()
        || !activity.fields.contains("buttons")
        || index >= activity.fields["buttons"].size())
      return;
    if (m_status.starts_with("Could not open the activity link.")
        || m_status == "The activity link was invalid."
        || m_status.starts_with("The activity link is unavailable."))
      m_status.clear();
    if (m_activityButtonRequest) {
      if (m_activityButtonRequest->pending
          && m_activityButtonRequest->user == user
          && m_activityButtonRequest->source == activity
          && m_activityButtonRequest->index == index)
        return;
      m_activityButtonRequest->revoked->store(true);
    }
    ActivityButtonRequest request;
    request.user = user;
    request.source = activity;
    request.index = index;
    request.revision = ++m_rpcRevision;
    const auto url = activity.fields["buttons"][index].value("url", std::string{});
    if (!url.empty()) {
      request.openedUrl = url;
      if (!m_demo)
        activate({0, 0, 0, 0, Action::OpenUrl, url});
    } else if (
        rpc::activityMetadataPath(user, activity)
        && (m_demo || m_client.activityMetadata(user, activity, request.revision, request.revoked))
    )
      request.pending = true;
    else
      m_status = "The activity link is unavailable. Wait for an updated activity and try again.";
    m_activityButtonRequest = std::move(request);
    m_window.invalidate();
  }
  void ClientApplication::rpcPublish() {
    std::vector<const RpcApplication*> candidates;
    if (m_preferences.rpcEnabled
        && (!m_accountSettings.contains("show_activity") || m_accountSettings.at("show_activity")))
      for (const auto& [connection, application] : m_rpcApplications)
        if (application.resolved
            && !application.externalPending
            && application.error.empty()
            && !application.revoked->load())
          candidates.push_back(&application);
    std::ranges::sort(candidates, [](const auto* left, const auto* right) {
      if (!left->request.connection || !right->request.connection)
        return !left->request.connection && right->request.connection;
      return left->revision > right->revision;
    });
    std::set<int> types;
    std::vector<rpc::Activity> activities;
    for (const auto* application : candidates)
      if (types.insert(application->resolved->fields.value("type", 0)).second
          && activities.size() < rpc::maximumActivities)
        activities.push_back(*application->resolved);
    if (activities != m_rpcPublished && (m_demo || m_client.setActivities(activities))) {
      m_rpcPublished = std::move(activities);
      api::UserUpdates update;
      update.users[m_account.id].activities = m_rpcPublished;
      m_userDirectory.apply(update);
      m_window.invalidate();
    }
  }
  float ClientApplication::drawActivities(
      Node* root, const layout::Frame& area, const std::string& user, const layout::Frame& clip, std::string_view scope
  ) {
    if (m_userDirectory.relationship(user) == 2 || m_userDirectory.ignored(user))
      return 0;
    using C = layout::ActivityCard;
    const auto activities = m_userDirectory.activities(user);
    float cursor = area.y;
    for (const auto& activity : activities) {
      const auto& fields = activity.fields;
      const auto type = fields.value("type", 0);
      const auto name = fields.value("name", activity.application);
      const auto heading = type == 0 ? "Playing"
          : type == 5                ? "Competing in"
                                     : (type == 2       ? "Listening to "
                                            : type == 3 ? "Watching "
                                            : type == 1 ? "Streaming "
                                            : type == 5 ? "Competing in "
                                                        : "Playing ")
              + name;
      const auto assets = fields.value("assets", nlohmann::json::object());
      const auto artwork = [&](const char* key) {
        const auto image = assets.value(key, std::string{});
        if (image.starts_with("mp:"))
          return api::mediaUrl("https://media.discordapp.net/" + image.substr(3));
        if (api::validSnowflake(image) && !activity.application.empty())
          return api::mediaUrl("/app-assets/" + activity.application + "/" + image + ".png?size=256");
        return std::string{};
      };
      auto largeArtwork = artwork("large_image");
      const auto smallArtwork = artwork("small_image");
      const auto metadata = m_rpcMetadata.find(activity.application);
      if (largeArtwork.empty() && metadata != m_rpcMetadata.end()) {
        const auto icon = metadata->second.value("icon", std::string{});
        if (api::validAssetHash(icon))
          largeArtwork = api::mediaUrl("/app-icons/" + activity.application + "/" + icon + ".png?size=256");
      }
      const bool showArtwork = m_preferences.showImages && !largeArtwork.empty();
      std::vector<std::pair<std::string, std::string>> lines;
      if (type == 0 || type == 5)
        lines.emplace_back(name, std::string{});
      for (const auto key : {"details", "state"})
        if (const auto text = fields.value(key, std::string{}); !text.empty())
          lines.emplace_back(text, fields.value(std::string(key) + "_url", std::string{}));
      if (type == 2)
        if (const auto album = assets.value("large_text", std::string{}); !album.empty()
            && album != name
            && std::ranges::none_of(lines, [&](const auto& line) { return line.first == album; }))
          lines.emplace_back(album, std::string{});
      if (lines.empty())
        lines.emplace_back(name, std::string{});
      if (fields.contains("party") && fields["party"].contains("size"))
        lines.emplace_back(
            fields["party"]["size"][0].dump() + " of " + fields["party"]["size"][1].dump() + " in party", std::string{}
        );
      const auto now =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();
      const auto clock = rpc::activityClock(activity, now);
      const float imageX = area.x + C::inset, imageY = cursor + C::inset + C::header;
      const float textX = imageX + (showArtwork ? C::image + C::gap : 0),
                  textWidth = std::max(0.0F, area.width - C::inset - (textX - area.x));
      const auto elapsed = clock ? rpc::activityTime(clock->elapsed) : std::string{},
                 total = clock ? rpc::activityTime(clock->total) : std::string{};
      const auto textWidthOf = [&](const std::string& text) {
        return m_window.target().renderer().measureText(text, Style::fontSizeCaption).width;
      };
      const auto timeline = C::timeline(
          {textX, imageY + lines.size() * C::row, textWidth, C::row}, textWidthOf(elapsed), textWidthOf(total)
      );
      const float contentHeight = lines.size() * C::row + (clock ? clock->progress ? timeline.height : C::row : 0);
      const auto buttons = fields.value("buttons", nlohmann::json::array());
      const float height = C::height(contentHeight, buttons.size());
      if (root) {
        float cardX = 0, cardY = 0;
        activityPosition(m_scene.get(), root, area.x, cursor, cardX, cardY);
        const bool visible = cardX < clip.x + clip.width
            && cardX + area.width > clip.x
            && cardY < clip.y + clip.height
            && cardY + height > clip.y;
        if (visible
            && m_preferences.showImages
            && largeArtwork.empty()
            && !activity.application.empty()
            && metadata == m_rpcMetadata.end()
            && !m_rpcLookups.contains(activity.application)
            && m_rpcMetadata.size() < maximumRpcMetadata
            && m_rpcLookups.size() < maximumRpcApplications
            && !m_demo) {
          const auto revision = ++m_rpcRevision;
          if (m_client.activityApplication(activity.application, revision))
            m_rpcLookups[activity.application] = std::to_string(revision);
        }
        rectangle(*root, area.x, cursor, area.width, height, theme::raised, Style::surfaceRadius);
        glyph(*root, imageX, cursor + C::inset, Style::glyphSizeTiny, activityGlyph(type), theme::muted);
        label(
            *root, imageX + Style::glyphSizeTiny + Style::spaceXs, cursor + C::inset,
            area.width - C::inset * 2 - Style::glyphSizeTiny - Style::spaceXs, heading, Style::fontSizeDetail,
            theme::muted, FontWeight::Medium
        );
        if (showArtwork) {
          picture(*root, imageX, imageY, C::image, C::image, largeArtwork, Style::radiusMd, true);
          if (!smallArtwork.empty())
            picture(
                *root, imageX + C::image - C::smallImage, imageY + C::image - C::smallImage, C::smallImage,
                C::smallImage, smallArtwork, C::smallImage / 2, true
            );
        }
        for (std::size_t i = 0; i < lines.size(); ++i)
          label(
              *root, textX, imageY + i * C::row, textWidth, lines[i].first, Style::fontSizeCaption,
              i ? theme::muted : theme::foreground, i ? FontWeight::Normal : FontWeight::Medium
          );
        if (clock) {
          if (clock->progress) {
            label(
                *root, timeline.elapsed.x, timeline.elapsed.y, timeline.elapsed.width, elapsed, Style::fontSizeCaption,
                theme::foreground
            );
            const float totalX = timeline.total.x + timeline.total.width - textWidthOf(total);
            label(
                *root, std::max(timeline.total.x, totalX), timeline.total.y, timeline.total.width, total,
                Style::fontSizeCaption, theme::foreground
            );
            const auto& track = timeline.track;
            rectangle(*root, track.x, track.y, track.width, track.height, theme::background, track.height / 2);
            if (clock->fraction > 0)
              rectangle(
                  *root, track.x, track.y, track.width * clock->fraction, track.height, theme::foreground,
                  track.height / 2
              );
          } else {
            const auto time =
                clock->countdown ? rpc::activityTime(clock->remaining) + " remaining" : elapsed + " elapsed";
            label(*root, textX, timeline.elapsed.y, textWidth, time, Style::fontSizeCaption, theme::muted);
          }
          if (visible && clock->ticking && !m_activityTimer.active()) {
            constexpr auto activityClockTick = std::chrono::seconds(1);
            m_activityTimer.start(activityClockTick, [this] { m_window.invalidate(); });
          }
        }
        const auto hit = [&](layout::Frame frame, const std::string& url) {
          if (url.empty())
            return;
          float x = 0, y = 0;
          activityPosition(m_scene.get(), root, frame.x, frame.y, x, y);
          const float left = std::max(x, clip.x), top = std::max(y, clip.y);
          const float right = std::min(x + frame.width, clip.x + clip.width),
                      bottom = std::min(y + frame.height, clip.y + clip.height);
          if (right > left && bottom > top)
            m_hits.push_back({left, top, right - left, bottom - top, Action::OpenUrl, url});
        };
        hit({imageX, imageY, C::image, C::image}, assets.value("large_url", std::string{}));
        for (std::size_t i = 0; i < lines.size(); ++i)
          hit({textX, imageY + i * C::row, textWidth, C::row}, lines[i].second);
        for (std::size_t i = 0; i < buttons.size(); ++i) {
          const auto frame = layout::Frame{
              imageX, imageY + std::max(C::image, contentHeight) + C::gap + i * (Style::controlHeightSm + C::gap),
              area.width - C::inset * 2, Style::controlHeightSm
          };
          const auto id =
              "activity:" + std::string(scope) + ":" + user + ":" + activity.application + ":" + std::to_string(i);
          std::unique_ptr<Node> saved;
          if (auto at = m_detachedInputs.find(id); at != m_detachedInputs.end()) {
            saved = std::move(at->second);
            m_detachedInputs.erase(at);
          }
          auto button =
              saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
          m_widgets[id] = button.get();
          button->setPosition(frame.x, frame.y);
          button->setFrameSize(frame.width, frame.height);
          button->setControlHeight(frame.height);
          button->setText(buttons[i].value("label", std::string{}));
          button->setVariant(ButtonVariant::Secondary);
          const bool pending = m_activityButtonRequest
              && m_activityButtonRequest->pending
              && m_activityButtonRequest->user == user
              && m_activityButtonRequest->source == activity
              && m_activityButtonRequest->index == i;
          button->setEnabled(!pending);
          button->setOnClick([this, user, activity, i] { activityButton(user, activity, i); });
          root->addChild(std::move(button));
          if (!pending) {
            float x = 0, y = 0;
            activityPosition(m_scene.get(), root, frame.x, frame.y, x, y);
            const float left = std::max(x, clip.x), top = std::max(y, clip.y),
                        right = std::min(x + frame.width, clip.x + clip.width),
                        bottom = std::min(y + frame.height, clip.y + clip.height);
            if (right > left && bottom > top)
              m_hits.push_back(
                  {left, top, right - left, bottom - top, Action::Preference, activityButtonAction(user, activity, i)}
              );
          }
        }
      }
      cursor += height + C::gap;
    }
    return cursor - area.y;
  }
  void ClientApplication::rpcEditor() {
    const auto value = m_preferences.customActivity ? m_preferences.customActivity->fields : nlohmann::json::object();
    m_settingsFields["rpc:application"] = m_preferences.customActivity ? m_preferences.customActivity->application : "";
    for (const auto key : {"name", "details", "state", "details_url", "state_url"})
      m_settingsFields[std::string("rpc:") + key] = value.value(key, std::string{});
    m_settingsFields["rpc:type"] = std::to_string(value.value("type", 0));
    m_settingsFields["rpc:status_display_type"] = std::to_string(value.value("status_display_type", 0));
    m_settingsFields["rpc:instance"] = value.value("instance", false) ? "1" : "0";
    const auto platforms = value.value("supported_platforms", nlohmann::json::array({"desktop"}));
    for (const auto platform : {"desktop", "android", "ios"})
      m_settingsFields[std::string("rpc:platform:") + platform] =
          std::ranges::find(platforms, platform) != platforms.end() ? "1" : "0";
    m_settingsFields["rpc:party:privacy"] =
        value.contains("party") ? std::to_string(value["party"].value("privacy", 0)) : "0";
    for (const auto section : {"assets", "timestamps", "party", "secrets"})
      for (const auto key :
           {"large_image", "large_text", "large_url", "small_image", "small_text", "small_url", "invite_cover_image",
            "start", "end", "id", "current", "maximum", "join", "spectate", "match"}) {
        const auto item = value.contains(section) ? value[section].value(key, nlohmann::json{}) : nlohmann::json{};
        m_settingsFields[std::string("rpc:") + section + ":" + key] = item.is_string() ? item.get<std::string>()
            : item.is_number_integer()                                                 ? item.dump()
                                                                                       : "";
      }
    if (value.contains("party") && value["party"].contains("size")) {
      m_settingsFields["rpc:party:current"] = value["party"]["size"][0].dump();
      m_settingsFields["rpc:party:maximum"] = value["party"]["size"][1].dump();
    }
    for (std::size_t i = 0; i < rpc::maximumButtons; ++i)
      for (const auto key : {"label", "url"})
        m_settingsFields["rpc:button:" + std::to_string(i) + ":" + key] =
            value.contains("buttons") && i < value["buttons"].size() ? value["buttons"][i].value(key, std::string{})
                                                                     : "";
  }
  bool ClientApplication::rpcAction(const std::string& action) {
    if (!action.starts_with("rpc:"))
      return false;
    if (action.starts_with("rpc:button:")) {
      auto value = std::string_view(action).substr(std::string_view("rpc:button:").size());
      std::array<std::string, 4> parts;
      for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto colon = value.find(':');
        if ((i + 1 < parts.size()) != (colon != std::string_view::npos))
          return true;
        parts[i] = value.substr(0, colon);
        if (colon != std::string_view::npos)
          value.remove_prefix(colon + 1);
      }
      const auto type = activityNumber(parts[2]), index = activityNumber(parts[3]);
      if (!type || !index || *index < 0 || *index >= static_cast<std::int64_t>(rpc::maximumButtons))
        return true;
      for (const auto& activity : m_userDirectory.activities(parts[0]))
        if (activity.application == parts[1] && activity.fields.value("type", 0) == *type) {
          activityButton(parts[0], activity, static_cast<std::size_t>(*index));
          break;
        }
      return true;
    }
    if (action == "rpc:enabled") {
      m_preferences.rpcEnabled = !m_preferences.rpcEnabled;
      if (!m_preferences.rpcEnabled)
        rpcClear();
      savePreferences();
    } else if (action == "rpc:stop") {
      if (const auto at = m_rpcApplications.find(0); at != m_rpcApplications.end()) {
        at->second.revoked->store(true);
        m_rpcApplications.erase(at);
      }
      rpcPublish();
    } else if (action == "rpc:start" || action == "rpc:save") {
      nlohmann::json fields = nlohmann::json::object();
      const auto get = [&](const std::string& key) { return m_settingsFields["rpc:" + key]; };
      for (const auto key : {"name", "details", "state", "details_url", "state_url"})
        if (!get(key).empty())
          fields[key] = get(key);
      for (const auto key : {"type", "status_display_type"}) {
        const auto number = activityNumber(get(key));
        fields[key] = number.value_or(0);
      }
      for (const auto key :
           {"large_image", "large_text", "large_url", "small_image", "small_text", "small_url", "invite_cover_image"})
        if (!get(std::string("assets:") + key).empty())
          fields["assets"][key] = get(std::string("assets:") + key);
      for (const auto key : {"start", "end"})
        if (!get(std::string("timestamps:") + key).empty()) {
          const auto number = activityNumber(get(std::string("timestamps:") + key));
          if (!number) {
            m_rpcStatus = "Enter activity times as Unix seconds.";
            return true;
          }
          fields["timestamps"][key] = *number;
        }
      if (!get("party:id").empty())
        fields["party"]["id"] = get("party:id");
      fields["instance"] = get("instance") == "1";
      fields["supported_platforms"] = nlohmann::json::array();
      for (const auto platform : {"desktop", "android", "ios"})
        if (get(std::string("platform:") + platform) == "1")
          fields["supported_platforms"].push_back(platform);
      if (!get("party:current").empty() || !get("party:maximum").empty()) {
        const auto current = activityNumber(get("party:current")), maximum = activityNumber(get("party:maximum"));
        if (!current || !maximum) {
          m_rpcStatus = "Enter both current and maximum party sizes.";
          return true;
        }
        fields["party"]["size"] = {*current, *maximum};
      }
      if (fields.contains("party"))
        fields["party"]["privacy"] = activityNumber(get("party:privacy")).value_or(0);
      for (const auto key : {"join", "spectate", "match"})
        if (!get(std::string("secrets:") + key).empty())
          fields["secrets"][key] = get(std::string("secrets:") + key);
      for (std::size_t i = 0; i < rpc::maximumButtons; ++i) {
        const auto prefix = "button:" + std::to_string(i) + ":";
        if (!get(prefix + "label").empty() || !get(prefix + "url").empty())
          fields["buttons"].push_back({{"label", get(prefix + "label")}, {"url", get(prefix + "url")}});
      }
      auto parsed = rpc::parseActivity(get("application"), fields);
      if (!parsed) {
        m_rpcStatus = parsed.error();
        return true;
      }
      auto saved = *parsed;
      saved.fields.erase("secrets");
      m_preferences.customActivity = std::move(saved);
      savePreferences();
      m_rpcStatus = "Custom activity saved.";
      if (action == "rpc:start") {
        if (!m_preferences.rpcEnabled) {
          m_rpcStatus = "Enable RPC before starting an activity.";
          return true;
        }
        if (const auto previous = m_rpcApplications.find(0); previous != m_rpcApplications.end())
          previous->second.revoked->store(true);
        RpcApplication application;
        application.request.session = m_session;
        application.raw = std::move(*parsed);
        if (!application.raw.fields.contains("timestamps"))
          application.raw.fields["timestamps"] = {
              {"start",
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
               )
                   .count()}
          };
        application.revision = ++m_rpcRevision;
        m_rpcApplications[0] = std::move(application);
        rpcResolve();
      }
    } else if (action.starts_with("rpc:retry:")) {
      const auto connection = activityNumber(action.substr(std::string_view("rpc:retry:").size()));
      if (connection && *connection >= 0)
        if (auto at = m_rpcApplications.find(static_cast<std::uint64_t>(*connection)); at != m_rpcApplications.end()) {
          if (const auto metadata = m_rpcMetadata.find(at->second.raw.application);
              metadata != m_rpcMetadata.end() && metadata->second.contains("error"))
            m_rpcMetadata.erase(metadata);
          at->second.revoked->store(true);
          at->second.revoked = std::make_shared<std::atomic_bool>(false);
          at->second.resolved.reset();
          at->second.externalPending = false;
          at->second.error.clear();
          at->second.revision = ++m_rpcRevision;
          rpcResolve();
        }
    } else if (action.starts_with("rpc:block:")) {
      const auto id = action.substr(std::string_view("rpc:block:").size());
      if (api::validSnowflake(id)
          && m_preferences.rpcBlocked.size() < maximumRpcMetadata
          && std::ranges::find(m_preferences.rpcBlocked, id) == m_preferences.rpcBlocked.end())
        m_preferences.rpcBlocked.push_back(id);
      for (auto at = m_rpcApplications.begin(); at != m_rpcApplications.end();)
        if (at->second.raw.application == id) {
          at->second.revoked->store(true);
          at = m_rpcApplications.erase(at);
        } else
          ++at;
      savePreferences();
      rpcPublish();
    } else if (action.starts_with("rpc:unblock:")) {
      std::erase(m_preferences.rpcBlocked, action.substr(std::string_view("rpc:unblock:").size()));
      savePreferences();
    }
    m_window.invalidate();
    return true;
  }
  void ClientApplication::prepareRpcDemo(const std::string& view) {
    if (view.ends_with("light"))
      m_preferences.theme = "light";
    if (view.ends_with("narrow"))
      m_window.setUiScale(1.6F);
    m_demoOverlay = view;
    m_demoInteractionStep = 0;
    m_preferences.rpcEnabled = true;
    auto activity = rpc::parseActivity(
        "44",
        {{"name", "Synthetic music"},
         {"type", 2},
         {"details", "An offline track"},
         {"state", "A synthetic artist"},
         {"buttons",
          nlohmann::json::array(
              {{{"label", "Music site"}, {"url", "https://example.com/music"}},
               {{"label", "Artist"}, {"url", "https://example.com/artist"}}}
          )}}
    );
    m_preferences.customActivity = *activity;
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    m_preferences.customActivity->fields["timestamps"] = {{"start", now - 60}, {"end", now + 180}};
    m_preferences.customActivity->fields["assets"] = {{"large_image", "123"}, {"large_text", "Synthetic album"}};
    const auto imageSize = static_cast<int>(layout::ActivityCard::image);
    constexpr std::size_t rgbaChannels = 4;
    std::vector<std::uint8_t> artwork(imageSize * imageSize * rgbaChannels);
    for (std::size_t pixel = 0; pixel < artwork.size(); pixel += rgbaChannels) {
      artwork[pixel] = static_cast<std::uint8_t>(pixel / rgbaChannels % imageSize * 255 / imageSize);
      artwork[pixel + 1] = static_cast<std::uint8_t>(pixel / rgbaChannels / imageSize * 255 / imageSize);
      artwork[pixel + 2] = 128;
      artwork[pixel + 3] = 255;
    }
    m_images[api::mediaUrl("/app-assets/44/123.png?size=256")].texture =
        m_window.context().textureManager().loadFromRgba(artwork.data(), imageSize, imageSize);
    m_images[api::mediaUrl("/app-icons/44/11111111111111111111111111111111.png?size=256")].texture =
        m_window.context().textureManager().loadFromRgba(artwork.data(), imageSize, imageSize);
    m_rpcMetadata["44"] = {
        {"name", "Synthetic music"}, {"icon", "11111111111111111111111111111111"}, {"assets", nlohmann::json::array()}
    };
    m_presence.status = "dnd";
    m_presence.text = "Keep this status";
    openSettings();
    selectSettingsPage("activity");
    if (view.starts_with("rpc-settings-active"))
      rpcAction("rpc:start");
    if (view.starts_with("rpc-profile")) {
      activate({0, 0, 0, 0, Action::CloseSettings, {}});
      prepareDmProfileDemo("dm-profile");
      auto received = rpc::gatewayActivity(*m_preferences.customActivity);
      received.erase("metadata");
      received["buttons"] = nlohmann::json::array({"Music site", "Artist"});
      received["session_id"] = "synthetic-activity-session";
      m_userDirectory.consume(
          "PRESENCE_UPDATE",
          {{"user_id", "11"}, {"status", "online"}, {"activities", nlohmann::json::array({received})}}
      );
      api::UserUpdates own;
      own.users[m_account.id].activities = m_userDirectory.activities("11");
      m_userDirectory.apply(own);
    }
  }
  void ClientApplication::demoRpcStep() {
    const auto require = [](bool condition, const char* message) {
      if (!condition)
        throw std::runtime_error(message);
    };
    const auto toggle = [&] {
      const auto at = m_widgets.find("toggle:rpc:enabled");
      require(at != m_widgets.end() && at->second->parent(), "RPC settings mounts a native enable toggle");
      m_controlInput.setFocus(m_controlInput.firstTabFocusUnder(at->second));
      m_window.key({.symbol = XKB_KEY_space});
    };
    const auto customKey = [&](std::uint32_t symbol) {
      const auto at = m_widgets.find("rpc-custom-activity");
      require(at != m_widgets.end() && at->second->parent(), "Custom Activity uses the shared native collapsible");
      m_controlInput.setFocus(m_controlInput.firstTabFocusUnder(at->second));
      m_window.key({.symbol = symbol});
    };
    const auto hasText = [&](const std::string& expected, float left, float right) {
      bool found = false;
      const auto inspect = [&](auto&& self, const Node& node, float x) -> void {
        x += node.x();
        if (node.type() == NodeType::Text
            && x >= left
            && x < right
            && static_cast<const TextNode&>(node).text() == expected)
          found = true;
        for (const auto& child : node.children())
          self(self, *child, x);
      };
      inspect(inspect, *m_scene, 0);
      return found;
    };
    switch (m_demoInteractionStep++) {
    case 0:
      require(
          GlyphRegistry::contains("activity-watching")
              && GlyphRegistry::contains("activity-listening")
              && GlyphRegistry::lookup("activity-watching") == GlyphRegistry::lookup("player-play-filled")
              && GlyphRegistry::lookup("activity-listening") == GlyphRegistry::lookup("headphones-filled"),
          "Activity summaries and cards share bundled filled glyphs"
      );
      require(
          m_preferences.rpcEnabled && m_rpc.endpoint().empty(),
          "RPC defaults enabled, while ordinary offline demos never bind local sockets"
      );
      require(m_settingsPage == "activity", "Activity Privacy owns both account visibility and local RPC controls");
      {
        const auto frame = layout::settingsFrame(m_window.width());
        const auto* custom = m_widgets.at("rpc-custom-activity");
        float x = 0, y = 0;
        Node::mapToScene(custom, 0, 0, x, y);
        require(
            std::abs(custom->width() - frame.contentWidth) < Style::borderWidth
                && std::abs(x - frame.contentLeft) < Style::borderWidth,
            "The custom activity collapsible spans the same outer width as settings cards"
        );
      }
      require(!m_rpcCustomExpanded, "The custom activity editor starts collapsed");
      customKey(XKB_KEY_Right);
      require(m_rpcCustomExpanded, "The collapsible opens through native keyboard input");
      m_settingsFields["rpc:button:0:url"] = "file:///private";
      rpcAction("rpc:start");
      require(m_rpcApplications.empty() && !m_rpcStatus.empty(), "Invalid button links cannot start a custom activity");
      break;
    case 1:
      m_settingsFields["rpc:button:0:url"] = "https://example.com/music";
      m_settingsFields["rpc:secrets:join"] = "synthetic-session-secret";
      rpcAction("rpc:start");
      require(
          m_rpcApplications.contains(0) && m_rpcPublished.size() == 1,
          "Custom activity resolves independently and reaches the shared presence intention"
      );
      break;
    case 2: {
      auto stale = rpc::parseActivity("44", {{"type", 2}, {"name", "Synthetic music"}, {"details", "Older track"}});
      require(stale.has_value(), "An older activity title is valid synthetic input");
      api::UserUpdates stalePresence;
      stalePresence.users[m_account.id].activities = {*stale};
      rpcProtectPublishedActivity(stalePresence);
      m_userDirectory.apply(stalePresence);
      require(
          !m_preferences.customActivity->fields.contains("secrets")
              && m_rpcPublished.front().fields["buttons"].size() == rpc::maximumButtons,
          "The custom editor keeps two buttons and never saves session secrets"
      );
      require(
          m_userDirectory.activities(m_account.id) == m_rpcPublished,
          "A delayed self-presence update cannot replace the locally published activity title"
      );
      require(
          m_presence.status == "dnd" && m_presence.text == "Keep this status",
          "Rich activity never overwrites availability or custom status text"
      );
      const auto button = m_widgets.find("activity:rpc-settings:" + m_account.id + ":44:0");
      require(
          button != m_widgets.end() && button->second->parent(),
          "The native activity preview paints a retained button with its actual destination"
      );
      customKey(XKB_KEY_Left);
      require(
          !m_rpcCustomExpanded && m_rpcPublished.size() == 1,
          "Collapsing Custom Activity hides its controls without stopping publication"
      );
      rpcAction("rpc:stop");
      require(
          m_rpcPublished.empty() && m_userDirectory.activities(m_account.id).empty(),
          "Explicit stop clears publication and the self-profile activity together"
      );
      break;
    }
    case 3:
      toggle();
      break;
    case 4:
      require(
          !m_preferences.rpcEnabled && m_rpcApplications.empty(),
          "The native RPC toggle revokes all active publications"
      );
      toggle();
      break;
    case 5: {
      require(
          m_preferences.rpcEnabled && m_rpcApplications.empty(),
          "Re-enabling RPC does not restart a saved custom activity"
      );
      m_rpcMetadata["44"] = {
          {"name", "Synthetic music"}, {"icon", "11111111111111111111111111111111"}, {"assets", nlohmann::json::array()}
      };
      const auto saved = m_preferences.customActivity;
      m_settingsFields["rpc:timestamps:start"].clear();
      m_settingsFields["rpc:timestamps:end"].clear();
      m_settingsFields["rpc:assets:large_image"].clear();
      rpcAction("rpc:start");
      m_settingsScroll = std::max(0.0F, m_widgets.at("rpc-custom-activity")->y() - Style::spaceLg);
      const auto clock = rpc::activityClock(
          m_rpcPublished.front(),
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count()
      );
      require(
          clock
              && clock->ticking
              && clock->elapsed == 0
              && !clock->countdown
              && !m_preferences.customActivity->fields.contains("timestamps"),
          "A custom activity starts an elapsed timer without saving its generated start time"
      );
      m_preferences.customActivity = saved;
      rpcEditor();
      break;
    }
    case 6: {
      bool iconDrawn = false;
      const auto icon =
          m_images.at(api::mediaUrl("/app-icons/44/11111111111111111111111111111111.png?size=256")).texture.id;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Image && static_cast<const ImageNode&>(node).textureId() == icon)
          iconDrawn = true;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(iconDrawn, "An activity without custom artwork renders the registered application icon");
      for (std::size_t index = 0; index < rpc::maximumButtons; ++index) {
        const auto id = "activity:rpc-settings:" + m_account.id + ":44:" + std::to_string(index);
        const auto* button = m_widgets.at(id);
        float x = 0, y = 0;
        Node::mapToScene(button, button->width() / 2, button->height() / 2, x, y);
        m_window.motion(x, y);
        m_window.click(x, y, BTN_LEFT);
        m_window.released();
        require(
            m_activityButtonRequest
                && m_activityButtonRequest->index == index
                && m_activityButtonRequest->openedUrl
                    == (index ? "https://example.com/artist" : "https://example.com/music"),
            "Pointer clicks open the matching first and second activity destinations"
        );
      }
      const auto revoked = m_rpcApplications.at(0).revoked;
      const auto revision = m_rpcApplications.at(0).revision;
      rpcAction("rpc:stop");
      api::Event stale;
      stale.session = m_session;
      stale.operation = api::Operation::ActivityExternalAssets;
      stale.resource = "44";
      stale.nonce = std::to_string(revision);
      stale.activityData = nlohmann::json::array();
      rpcResult(stale);
      require(
          revoked->load() && m_rpcApplications.empty() && m_rpcPublished.empty(),
          "Stopped activity generations reject delayed artwork completions"
      );
      RpcApplication missing;
      missing.raw = *m_preferences.customActivity;
      missing.raw.application = "999";
      m_rpcApplications[0] = std::move(missing);
      m_rpcLookups["999"] = "synthetic-lookup";
      api::Event failure;
      failure.session = m_session;
      failure.operation = api::Operation::ActivityApplication;
      failure.resource = "999";
      failure.nonce = "synthetic-lookup";
      failure.error = "Application not found. Use a valid application ID from the developer portal.";
      rpcResult(failure);
      require(
          m_rpcApplications.at(0).error == failure.error && m_rpcPublished.empty(),
          "Unknown application IDs retain a specific setup error without publishing an activity"
      );
      break;
    }
    case 7:
      rpcClear();
      require(
          m_rpcMetadata.empty() && m_rpcLookups.empty(), "Account reset drops RPC metadata and pending activity state"
      );
      break;
    case 8: {
      activate({0, 0, 0, 0, Action::CloseSettings, {}});
      prepareDmProfileDemo("dm-profile");
      auto fields = rpc::gatewayActivity(*m_preferences.customActivity);
      fields.erase("metadata");
      fields["buttons"] = nlohmann::json::array({"Music site", "Artist"});
      fields["session_id"] = "synthetic-activity-session";
      fields["status_display_type"] = nullptr;
      fields["instance"] = nullptr;
      const auto update = m_userDirectory.consume(
          "PRESENCE_UPDATE", {{"user_id", "11"}, {"status", "online"}, {"activities", nlohmann::json::array({fields})}}
      );
      require(!m_userDirectory.activities("11").empty(), "Received activities tolerate optional null fields");
      api::UserUpdates own;
      own.users[m_account.id].activities = m_userDirectory.activities("11");
      m_userDirectory.apply(own);
      showProfile("11");
      m_profileScroll = std::numeric_limits<float>::max();
      break;
    }
    case 9: {
      require(
          hasText("Synthetic music", layout::Sidebar::railWidth, chatLeft()),
          "The DM sidebar paints the actual received activity below the person's name"
      );
      require(
          hasText("A synthetic artist - An offline track", 0, chatLeft()),
          "The account plate displays the current listening artist and track"
      );
      if (memberPaneWidth() > 0)
        require(
            hasText("An offline track", m_window.width() - memberPaneWidth(), m_window.width()),
            "The right-side DM profile displays a complete activity card"
        );
      if (memberPaneWidth() > 0)
        require(
            hasText("4:00", m_window.width() - memberPaneWidth(), m_window.width()),
            "The profile card paints total duration beside its elapsed-time progress bar"
        );
      const auto received = m_userDirectory.activities("11").front();
      require(
          received.fields["buttons"].size() == rpc::maximumButtons && !received.fields["buttons"][0].contains("url"),
          "Gateway button labels survive without inline URLs"
      );
      {
        const auto firstHit = m_hits.size();
        const auto sceneY = m_scene->y();
        m_scene->setPosition(m_scene->x(), -layout::Window::titlebarHeight);
        auto scratch = std::make_unique<Node>();
        scratch->setPosition(Style::spaceLg, layout::Window::titlebarHeight);
        auto* card = m_scene->addChild(std::move(scratch));
        drawActivities(
            card, {0, 0, Style::profileCardWidth, 0}, "11",
            {0, 0, static_cast<float>(m_window.width()), static_cast<float>(m_window.height())}, "rpc-geometry"
        );
        for (std::size_t index = 0; index < rpc::maximumButtons; ++index) {
          const auto id = "activity:rpc-geometry:11:44:" + std::to_string(index);
          const auto* button = m_widgets.at(id);
          const auto hit = std::find_if(
              m_hits.begin() + static_cast<std::ptrdiff_t>(firstHit), m_hits.end(),
              [&](const auto& candidate) { return candidate.id == activityButtonAction("11", received, index); }
          );
          require(hit != m_hits.end(), "Activity buttons retain an overlay-safe click region");
          float x = 0, y = 0;
          Node::mapToScene(button, 0, 0, x, y);
          require(
              std::abs(hit->y - (y - m_scene->y())) < Style::borderWidth,
              "Activity click regions compensate for the absent client title bar"
          );
          m_widgets.erase(id);
        }
        m_scene->removeChild(card);
        m_scene->setPosition(m_scene->x(), sceneY);
        m_hits.resize(firstHit);
      }
      const auto clickProfileButton = [&](std::size_t index) {
        const auto at = m_widgets.find("activity:profile:11:44:" + std::to_string(index));
        require(at != m_widgets.end() && at->second->parent(), "The profile modal mounts its activity buttons");
        float x = 0, y = 0;
        Node::mapToScene(at->second, at->second->width() / 2, at->second->height() / 2, x, y);
        m_window.motion(x, y);
        m_window.click(x, y, BTN_LEFT);
        m_window.released();
      };
      clickProfileButton(0);
      require(
          m_activityButtonRequest && m_activityButtonRequest->pending,
          "Activating a received button requests its separate link metadata"
      );
      api::Event links;
      links.operation = api::Operation::ActivityMetadata;
      links.session = m_session;
      links.resource = "11";
      links.nonce = std::to_string(m_activityButtonRequest->revision);
      links.activityData = nlohmann::json::array({"https://example.com/music", "https://example.com/artist"});
      rpcResult(links);
      require(
          m_activityButtonRequest->openedUrl == "https://example.com/music",
          "The selected profile button opens only its matching resolved link"
      );
      clickProfileButton(1);
      require(
          m_activityButtonRequest->index == 1 && m_activityButtonRequest->pending,
          "The second profile button requests its own destination instead of the first button's"
      );
      links.nonce = std::to_string(m_activityButtonRequest->revision);
      links.error = "Synthetic metadata failure";
      rpcResult(links);
      require(
          !m_activityButtonRequest->pending && m_activityButtonRequest->openedUrl.empty(),
          "Failed link reads remain stopped for explicit retry"
      );
      rpcAction(activityButtonAction("11", received, 1));
      links.nonce = std::to_string(m_activityButtonRequest->revision);
      links.error.clear();
      auto changed = received;
      changed.fields["details"] = "A changed track";
      api::UserUpdates newer;
      newer.users["11"].activities = std::vector{changed};
      m_userDirectory.apply(newer);
      rpcResult(links);
      require(m_activityButtonRequest->openedUrl.empty(), "An activity change retires late button destinations");
      closeDialog();
      selectChannel("9003");
      break;
    }
    case 10: {
      if (memberPaneWidth() > 0)
        require(
            hasText("Synthetic music", m_window.width() - memberPaneWidth(), m_window.width()),
            "Group DM participants show the activity subtitle"
        );
      selectGuild("1");
      selectChannel("101");
      m_showMembers = true;
      m_members = {{.index = 0, .user = {"33", "Uma"}, .status = "online"}};
      m_membersLoaded = true;
      m_memberCount = 1;
      auto fields = rpc::gatewayActivity(*m_preferences.customActivity);
      m_userDirectory.consume(
          "GUILD_MEMBER_LIST_UPDATE",
          {{"guild_id", "1"},
           {"ops",
            nlohmann::json::array(
                {{{"op", "SYNC"},
                  {"items",
                   nlohmann::json::array(
                       {{{"member",
                          {{"user", {{"id", "33"}}},
                           {"presence", {{"status", "online"}, {"activities", nlohmann::json::array({fields})}}}}}}}
                   )}}}
            )}}
      );
      break;
    }
    case 11:
      require(
          !m_userDirectory.activities("33").empty(), "Member-list presence imports rich activities for non-friends"
      );
      if (memberPaneWidth() > 0)
        require(
            hasText("Synthetic music", m_window.width() - memberPaneWidth(), m_window.width()),
            "Server member rows paint the activity subtitle without changing row geometry"
        );
      rpcClear();
      std::clog << "Offline RPC collapsible, editor, buttons, account plate, DM/profile and member activities passed\n";
      break;
    default:
      require(false, "RPC replay has a fixed bounded frame count");
    }
    m_window.invalidate();
  }
} // namespace app

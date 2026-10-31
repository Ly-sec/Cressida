#include "api/streams.h"

#include "voice/protocol.h"

#include <algorithm>
#include <charconv>
#include <set>

namespace api {
  namespace {
    constexpr std::size_t maximumStreamRegionBytes = 64, maximumStreamTokenBytes = 4096;
    std::string text(const nlohmann::json& data, const char* key, std::size_t maximum) {
      const auto at = data.find(key);
      if (at == data.end() || !at->is_string())
        return {};
      const auto& value = at->get_ref<const std::string&>();
      if (value.empty() || value.size() > maximum || std::ranges::any_of(value, [](unsigned char ch) {
            return ch < ' ' || ch == 127;
          }))
        return {};
      return value;
    }
    bool valid(const StreamKey& key) {
      return (key.guild.empty() || voice::protocol::validId(key.guild))
          && voice::protocol::validId(key.channel)
          && voice::protocol::validId(key.owner);
    }
  } // namespace
  std::string StreamKey::value() const {
    if (!valid(*this))
      return {};
    return (guild.empty() ? "call:" : "guild:" + guild + ':') + channel + ':' + owner;
  }
  bool
  shareQualityAvailable(voice::ShareSettings settings, std::optional<int> premiumType, std::optional<int> serverTier) {
    if (voice::sanitizeShareSettings(settings) != settings)
      return false;
    constexpr int classicPlan = 1, fullPlan = 2, fastServerTier = 1, highServerTier = 2;
    const bool full = premiumType == fullPlan, medium = full || premiumType == classicPlan;
    if (settings.resolution == 0 || settings.resolution >= 2160)
      return full;
    if (settings.resolution > 720)
      return medium || serverTier.value_or(0) >= highServerTier;
    return settings.frameRate <= 30 || medium || serverTier.value_or(0) >= fastServerTier;
  }
  std::optional<StreamKey> parseStreamKey(std::string_view value) {
    if (value.empty() || value.size() > maximumStreamKeyBytes)
      return {};
    const auto original = std::string(value);
    const auto field = [&value]() {
      const auto end = value.find(':');
      const auto part = value.substr(0, end);
      value = end == std::string_view::npos ? std::string_view{} : value.substr(end + 1);
      return part;
    };
    StreamKey key;
    const auto kind = field();
    if (kind == "guild")
      key.guild = field();
    else if (kind != "call")
      return {};
    key.channel = field();
    key.owner = field();
    if (!value.empty() || !valid(key) || (kind == "guild" && key.guild.empty()) || key.value() != original)
      return {};
    return key;
  }
  std::optional<nlohmann::json> streamCommand(const StreamCommand& command) {
    if (!valid(command.target))
      return {};
    using Json = nlohmann::json;
    constexpr int createOpcode = 18, deleteOpcode = 19, watchOpcode = 20, pingOpcode = 21, pauseOpcode = 22;
    int opcode = createOpcode;
    Json data;
    if (command.kind == StreamCommandKind::Create)
      data = {
          {"type", command.target.guild.empty() ? "call" : "guild"},
          {"guild_id", command.target.guild.empty() ? Json(nullptr) : Json(command.target.guild)},
          {"channel_id", command.target.channel},
          {"preferred_region", nullptr}
      };
    else {
      data = {{"stream_key", command.target.value()}};
      switch (command.kind) {
      case StreamCommandKind::Delete:
        opcode = deleteOpcode;
        break;
      case StreamCommandKind::Watch:
        opcode = watchOpcode;
        break;
      case StreamCommandKind::Ping:
        opcode = pingOpcode;
        break;
      case StreamCommandKind::Pause:
        opcode = pauseOpcode;
        data["paused"] = command.paused;
        break;
      default:
        return {};
      }
    }
    return Json{{"op", opcode}, {"d", std::move(data)}};
  }
  std::optional<StreamUpdate> parseStreamUpdate(std::string_view type, const nlohmann::json& data) {
    if (!data.is_object() || (type != "STREAM_CREATE" && type != "STREAM_UPDATE" && type != "STREAM_DELETE"))
      return {};
    const auto key = parseStreamKey(text(data, "stream_key", maximumStreamKeyBytes));
    if (!key)
      return {};
    StreamUpdate update{
        .kind = type == "STREAM_CREATE" ? StreamUpdate::Kind::Create
            : type == "STREAM_DELETE"   ? StreamUpdate::Kind::Delete
                                        : StreamUpdate::Kind::Update,
        .stream = {.key = *key}
    };
    if (update.kind == StreamUpdate::Kind::Delete)
      return update;
    if (const auto at = data.find("viewer_ids"); at != data.end()) {
      if (!at->is_array() || at->size() > maximumStreamViewers)
        return {};
      std::set<std::string> seen;
      for (const auto& item : *at) {
        if (!item.is_string()
            || !voice::protocol::validId(item.get_ref<const std::string&>())
            || !seen.insert(item.get<std::string>()).second)
          return {};
        update.stream.viewers.push_back(item.get<std::string>());
      }
      update.viewersPresent = true;
    }
    if (const auto at = data.find("paused"); at != data.end()) {
      if (!at->is_boolean())
        return {};
      update.stream.paused = at->get<bool>();
      update.pausedPresent = true;
    }
    if (data.contains("region")) {
      update.stream.region = text(data, "region", maximumStreamRegionBytes);
      if (update.stream.region.empty() && !data["region"].is_null())
        return {};
      update.regionPresent = true;
    }
    return update;
  }
  void StreamDirectory::clear() { m_records.clear(); }
  void StreamDirectory::apply(const StreamUpdate& update) {
    const auto key = update.stream.key.value();
    if (key.empty())
      return;
    if (update.kind == StreamUpdate::Kind::Delete) {
      m_records.erase(key);
      return;
    }
    auto at = m_records.find(key);
    if (at == m_records.end()) {
      if (update.kind != StreamUpdate::Kind::Create || m_records.size() >= maximumStreams)
        return;
      at = m_records.emplace(key, StreamRecord{.key = update.stream.key}).first;
    }
    if (update.viewersPresent)
      at->second.viewers = update.stream.viewers;
    if (update.pausedPresent)
      at->second.paused = update.stream.paused;
    if (update.regionPresent)
      at->second.region = update.stream.region;
  }
  std::vector<StreamRecord> StreamDirectory::records() const {
    std::vector<StreamRecord> result;
    for (const auto& [key, record] : m_records)
      result.push_back(record);
    return result;
  }
  std::optional<StreamRecord> StreamDirectory::find(const std::string& key) const {
    const auto at = m_records.find(key);
    return at == m_records.end() ? std::nullopt : std::optional{at->second};
  }
  bool StreamSession::begin(
      std::uint64_t generation, std::uint64_t revision, StreamKey key, std::string self, std::string parentSession,
      bool publishing, bool reused
  ) {
    clear();
    if (!generation
        || !revision
        || !valid(key)
        || !voice::protocol::validId(self)
        || parentSession.empty()
        || parentSession.size() > maximumStreamTokenBytes
        || std::ranges::any_of(parentSession, [](unsigned char ch) { return ch < ' ' || ch == 127; })
        || publishing != (self == key.owner))
      return false;
    m_key = std::move(key);
    m_info.user = std::move(self);
    m_info.session = std::move(parentSession);
    m_info.generation = generation;
    m_info.revision = revision;
    m_publishing = publishing;
    m_reused = reused;
    m_deadline = std::chrono::steady_clock::now() + streamSetupDeadline;
    return true;
  }
  StreamSession::Update StreamSession::receive(std::string_view type, const nlohmann::json& data) {
    if (!active() || !data.is_object() || text(data, "stream_key", maximumStreamKeyBytes) != m_key.value())
      return Update::None;
    if (type == "STREAM_DELETE") {
      clear();
      return Update::Ended;
    }
    if (type == "STREAM_CREATE") {
      if (m_started || m_created)
        return Update::None;
      const auto server = text(data, "rtc_server_id", maximumStreamKeyBytes);
      auto channel = text(data, "rtc_channel_id", maximumStreamKeyBytes);
      if (!data.contains("rtc_channel_id") || data["rtc_channel_id"].is_null()) {
        std::uint64_t serverNumber = 0;
        const auto parsed = std::from_chars(server.data(), server.data() + server.size(), serverNumber);
        if (parsed.ec != std::errc{} || parsed.ptr != server.data() + server.size() || serverNumber <= 1)
          return Update::None;
        channel = std::to_string(serverNumber - 1);
      }
      const auto record = parseStreamUpdate(type, data);
      if (!voice::protocol::validId(server) || !voice::protocol::validId(channel) || !record)
        return Update::None;
      m_info.server = server;
      m_info.channel = channel;
      m_info.participants = record->stream.viewers;
      if (std::ranges::find(m_info.participants, m_key.owner) == m_info.participants.end())
        m_info.participants.push_back(m_key.owner);
      m_created = true;
    } else if (type == "STREAM_SERVER_UPDATE") {
      if (m_started || (m_reused && !m_created))
        return Update::None;
      const auto endpoint = text(data, "endpoint", maximumStreamKeyBytes * 2);
      const auto token = text(data, "token", maximumStreamTokenBytes);
      if (!voice::protocol::endpoint(endpoint) || token.empty())
        return Update::None;
      m_info.endpoint = endpoint;
      m_info.token = token;
    } else
      return Update::None;
    return m_created && !m_info.token.empty() ? Update::Ready : Update::None;
  }
  void StreamSession::clear() {
    m_key = {};
    m_info = {};
    m_created = m_started = m_publishing = m_reused = false;
    m_deadline = {};
  }
  bool StreamSession::expired(std::chrono::steady_clock::time_point now) const {
    return active() && !m_started && now >= m_deadline;
  }
  std::optional<voice::ConnectionInfo> StreamSession::take() {
    if (!active() || !m_created || m_started || m_info.token.empty())
      return {};
    auto result = m_info;
    m_info.token.clear();
    m_info.endpoint.clear();
    m_started = true;
    return result;
  }
} // namespace api

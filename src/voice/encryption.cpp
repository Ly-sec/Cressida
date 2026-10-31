#include "voice/encryption.h"

#include "voice/dave/session.h"

#include <charconv>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace voice {
  namespace {
    using namespace dave;
    bool validUser(std::string_view value) {
      std::uint64_t number = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
      return !value.empty()
          && value.front() != '0'
          && parsed.ec == std::errc{}
      && parsed.ptr == value.data() + value.size()
          && number;
    }
    std::optional<std::uint64_t> unsignedField(const nlohmann::json& value, const char* key) {
      const auto at = value.find(key);
      if (at == value.end() || !at->is_number_integer() || (!at->is_number_unsigned() && at->get<std::int64_t>() < 0))
        return {};
      return at->get<std::uint64_t>();
    }
  } // namespace

  struct Encryption::Impl {
    struct Transition {
      dave::Secret sending;
      std::uint64_t revision = 0;
    };
    std::string self, failure;
    std::unique_ptr<dave::Session> session;
    dave::Sender sender;
    std::map<std::string, dave::Receiver, std::less<>> receivers;
    std::set<std::string, std::less<>> users;
    std::map<std::uint16_t, Transition> transitions;
    std::vector<EncryptionMessage> outgoing;
    std::uint64_t activatedRevision = 0;
    std::size_t outgoingBytes = 0;
    unsigned recoveryAttempts = 0;
    bool initialized = false;

    Impl(std::string user, std::uint64_t channel) : self(std::move(user)) {
      try {
        if (!validUser(self) || !channel) {
          fail("Voice encryption could not be initialized.");
          return;
        }
        session = std::make_unique<dave::Session>(self, channel);
        users.insert(self);
      } catch (const std::exception&) {
        fail("Voice encryption could not be initialized.");
      }
    }
    bool fail(const char* message) {
      failure = message;
      sender.clear();
      transitions.clear();
      return false;
    }
    bool queue(EncryptionMessage message) {
      const auto size = message.binary ? message.bytes.size() : message.data.dump().size();
      if (outgoing.size() >= maximumPendingMessages || size > maximumHandshakeBytes - outgoingBytes)
        return fail("Voice encryption negotiation exceeded its limits.");
      outgoingBytes += size;
      outgoing.push_back(std::move(message));
      return true;
    }
    bool supported(std::uint64_t version) {
      return version == protocolVersion || fail("The voice server selected an unsupported encryption version.");
    }
    bool initialize(std::uint16_t version) {
      if (!failure.empty() || !session || !supported(version))
        return false;
      sender.clear();
      receivers.clear();
      transitions.clear();
      activatedRevision = 0;
      initialized = false;
      auto package = session->initialize();
      initialized = true;
      return queue({.opcode = KeyPackage, .binary = true, .bytes = std::move(package), .data = {}});
    }
    bool receiver(const std::string& user) {
      if (user == self || !initialized)
        return true;
      const auto secret = session->secret(user);
      return secret.empty() || receivers.try_emplace(user).first->second.install(secret);
    }
    bool execute(std::uint16_t id) {
      const auto at = transitions.find(id);
      if (at == transitions.end())
        return true;
      auto transition = std::move(at->second);
      transitions.erase(at);
      if (transition.revision < activatedRevision)
        return true;
      if (transition.sending.empty()) {
        if (id != initialTransition)
          return fail("Voice encryption has no active sending key.");
        sender.clear();
        return true;
      }
      if (!sender.install(transition.sending))
        return fail("Voice encryption could not activate its sending key.");
      activatedRevision = transition.revision;
      return true;
    }
    bool prepare(std::uint16_t id, std::uint64_t version) {
      if (!supported(version))
        return false;
      if (!initialized || (!transitions.contains(id) && transitions.size() >= maximumPendingTransitions))
        return fail("Voice encryption has too many pending key changes.");
      for (const auto& user : users)
        if (!receiver(user))
          return fail("Voice encryption has too many pending receiving keys.");
      transitions[id] = {session->secret(self), session->revision()};
      if (id == initialTransition)
        return execute(id);
      return queue({.opcode = Ready, .bytes = {}, .data = {{"transition_id", id}}});
    }
    bool recover(std::uint16_t id) {
      sender.clear();
      if (++recoveryAttempts > maximumRecoveryAttempts)
        return fail("Voice encryption could not recover its group keys.");
      return queue({.opcode = InvalidUpdate, .bytes = {}, .data = {{"transition_id", id}}})
          && initialize(protocolVersion);
    }
  };

  std::uint16_t Encryption::maximumVersion() { return protocolVersion; }
  Encryption::Encryption(std::string user, std::uint64_t channel)
      : m_impl(std::make_unique<Impl>(std::move(user), channel)) {}
  Encryption::~Encryption() = default;
  bool Encryption::initialize(std::uint16_t version) {
    try {
      return m_impl->initialize(version);
    } catch (const std::exception&) {
      return m_impl->fail("Voice encryption could not create its session keys.");
    }
  }
  bool Encryption::handleJson(int opcode, const nlohmann::json& data) {
    auto& state = *m_impl;
    if (!state.failure.empty() || !data.is_object())
      return state.fail("Voice encryption received an invalid control message.");
    try {
      if (opcode == Prepare || opcode == Execute) {
        const auto transition = unsignedField(data, "transition_id");
        if (!transition || *transition > UINT16_MAX)
          return state.fail("Voice encryption received an invalid key transition.");
        if (opcode == Execute)
          return state.execute(static_cast<std::uint16_t>(*transition));
        const auto version = unsignedField(data, "protocol_version");
        return version ? state.prepare(static_cast<std::uint16_t>(*transition), *version)
                       : state.fail("Voice encryption received an invalid key transition.");
      }
      if (opcode == Epoch) {
        const auto version = unsignedField(data, "protocol_version");
        auto epoch = unsignedField(data, "epoch");
        const auto at = data.find("epoch");
        if (at != data.end() && at->is_string()) {
          const auto& text = at->get_ref<const std::string&>();
          std::uint64_t value = 0;
          const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
          if (!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
            epoch = value;
        }
        if (!version || !epoch || !*epoch)
          return state.fail("Voice encryption received an invalid epoch.");
        return state.supported(*version) && (*epoch != firstEpoch || state.initialize(protocolVersion));
      }
      return state.fail("Voice encryption received an unknown control message.");
    } catch (const std::exception&) {
      return state.fail("Voice encryption could not prepare its group keys.");
    }
  }
  bool Encryption::handleBinary(int opcode, std::span<const std::uint8_t> payload) {
    auto& state = *m_impl;
    if (!state.failure.empty() || !state.initialized || payload.empty() || payload.size() > maximumHandshakeBytes)
      return state.fail("Voice encryption received an invalid handshake message.");
    std::optional<std::uint16_t> transition;
    try {
      if (opcode == ExternalSender) {
        state.session->externalSender(payload);
        return true;
      }
      if (opcode == Proposals) {
        auto result = state.session->proposals(payload, state.users);
        return state.queue({.opcode = CommitWelcome, .binary = true, .bytes = std::move(result), .data = {}});
      }
      if ((opcode != Commit && opcode != Welcome) || payload.size() <= sizeof(std::uint16_t))
        return state.fail("Voice encryption received an invalid group update.");
      transition = static_cast<std::uint16_t>((static_cast<unsigned>(payload[0]) << byteBits) | payload[1]);
      payload = payload.subspan(sizeof(std::uint16_t));
      if (opcode == Commit) {
        if (state.session->commit(payload, state.users) == dave::Update::Ignored)
          return true;
      } else
        state.session->welcome(payload, state.users);
      state.recoveryAttempts = 0;
      return state.prepare(*transition, protocolVersion);
    } catch (const std::exception&) {
      if (transition) {
        try {
          return state.recover(*transition);
        } catch (const std::exception&) {
          return state.fail("Voice encryption could not recover its group keys.");
        }
      }
      return state.fail("Voice encryption could not validate the proposed keys.");
    }
  }
  void Encryption::addUser(std::string user) {
    auto& state = *m_impl;
    if (!state.failure.empty()
        || !validUser(user)
        || (state.users.size() >= maximumParticipants && !state.users.contains(user)))
      return;
    try {
      const auto [at, inserted] = state.users.insert(std::move(user));
      if (inserted && !state.receiver(*at))
        state.fail("Voice encryption has too many pending receiving keys.");
    } catch (const std::exception&) {
      state.fail("Voice encryption could not prepare its receiving keys.");
    }
  }
  void Encryption::removeUser(std::string_view user) {
    auto& state = *m_impl;
    if (user == state.self)
      return;
    if (const auto at = state.receivers.find(user); at != state.receivers.end())
      state.receivers.erase(at);
    if (const auto at = state.users.find(user); at != state.users.end())
      state.users.erase(at);
  }
  std::vector<EncryptionMessage> Encryption::takeMessages() {
    m_impl->outgoingBytes = 0;
    return std::exchange(m_impl->outgoing, {});
  }
  std::vector<std::uint8_t>
  Encryption::encrypt(std::uint32_t ssrc, std::span<const std::uint8_t> media, MediaCodec codec) {
    if (!ssrc || !ready())
      return {};
    try {
      return m_impl->sender.encrypt(media, codec);
    } catch (const std::exception&) {
      m_impl->fail("Voice audio encryption failed.");
      return {};
    }
  }
  std::vector<std::uint8_t>
  Encryption::decrypt(std::string_view user, std::span<const std::uint8_t> frame, MediaCodec codec) {
    const auto at = m_impl->receivers.find(user);
    if (!ready() || at == m_impl->receivers.end())
      return {};
    return at->second.decrypt(frame, codec);
  }
  void Encryption::tick() {
    for (auto& [user, receiver] : m_impl->receivers)
      receiver.expire();
  }
  bool Encryption::ready() const { return m_impl->sender.ready() && m_impl->failure.empty(); }
  std::string Encryption::error() const { return m_impl->failure; }
} // namespace voice

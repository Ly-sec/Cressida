#include "voice/encryption.h"

#include <charconv>
#include <dave/dave.h>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace voice {
  namespace {
    constexpr std::size_t maximumParticipants = 256;
    constexpr std::size_t maximumHandshakeBytes = 1024 * 1024;
    constexpr std::size_t maximumMediaBytes = 4096;
    constexpr std::size_t maximumPendingMessages = 32;
    constexpr std::size_t maximumPendingTransitions = 8;
    constexpr std::uint16_t initialTransition = 0;
    constexpr std::uint64_t firstEpoch = 1;

    bool validUser(std::string_view value) {
      std::uint64_t id = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
      return !value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && id != 0;
    }
    std::optional<std::uint64_t> unsignedField(const nlohmann::json& value, const char* key) {
      const auto at = value.find(key);
      if (at == value.end() || !at->is_number_integer() || (!at->is_number_unsigned() && at->get<std::int64_t>() < 0))
        return {};
      return at->get<std::uint64_t>();
    }
    std::uint16_t transitionPrefix(std::span<const std::uint8_t> bytes) {
      return static_cast<std::uint16_t>((static_cast<unsigned>(bytes[0]) << 8) | bytes[1]);
    }
  } // namespace

  struct Encryption::Impl {
    std::string user, failure;
    std::uint64_t channel;
    DAVESessionHandle session = nullptr;
    DAVEEncryptorHandle encryptor = nullptr;
    std::map<std::string, DAVEDecryptorHandle, std::less<>> decryptors;
    std::set<std::string, std::less<>> users;
    std::map<std::uint16_t, std::uint16_t> transitions;
    std::vector<EncryptionMessage> outgoing;
    std::uint16_t preparedVersion = 0;
    bool active = false, mlsFailure = false;

    Impl(std::string self, std::uint64_t group) : user(std::move(self)), channel(group) {
      // Upstream diagnostics may contain protocol records. Surface fixed errors instead.
      static std::once_flag logger;
      std::call_once(logger, [] { daveSetLogSinkCallback([](DAVELoggingSeverity, const char*, int, const char*) {}); });
      session = daveSessionCreate(
          nullptr, nullptr,
          [](const char*, const char*, void* context) { static_cast<Impl*>(context)->mlsFailure = true; }, this
      );
      encryptor = daveEncryptorCreate();
      if (encryptor)
        daveEncryptorSetPassthroughMode(encryptor, false);
      if (!session || !encryptor || !validUser(user) || !channel)
        fail("Voice encryption could not be initialized.");
      users.insert(user);
    }
    ~Impl() {
      for (const auto& [id, decryptor] : decryptors)
        daveDecryptorDestroy(decryptor);
      daveEncryptorDestroy(encryptor);
      daveSessionDestroy(session);
    }
    bool fail(const char* message) {
      failure = message;
      active = false;
      return false;
    }
    bool queue(EncryptionMessage message) {
      if (outgoing.size() >= maximumPendingMessages || message.bytes.size() > maximumHandshakeBytes)
        return fail("Voice encryption negotiation exceeded its limits.");
      outgoing.push_back(std::move(message));
      return true;
    }
    bool supported(std::uint16_t version) {
      if (!version || version > daveMaxSupportedProtocolVersion())
        return fail("The voice server selected an unsupported encryption version.");
      return true;
    }
    std::vector<const char*> recognized() const {
      std::vector<const char*> result;
      result.reserve(users.size());
      for (const auto& id : users)
        result.push_back(id.c_str());
      return result;
    }
    bool keyPackage() {
      std::uint8_t* bytes = nullptr;
      std::size_t count = 0;
      daveSessionGetMarshalledKeyPackage(session, &bytes, &count);
      std::vector<std::uint8_t> result;
      if (bytes && count <= maximumHandshakeBytes)
        result.assign(bytes, bytes + count);
      daveFree(bytes);
      if (result.empty() || mlsFailure)
        return fail("Voice encryption could not create its session keys.");
      return queue({.opcode = 26, .binary = true, .bytes = std::move(result), .data = {}});
    }
    bool initialize(std::uint16_t version) {
      if (!session || !encryptor || !failure.empty() || !supported(version))
        return false;
      mlsFailure = false;
      transitions.clear();
      daveSessionInit(session, version, channel, user.c_str());
      preparedVersion = version;
      return keyPackage();
    }
    void receiver(const std::string& id) {
      if (id == user || !preparedVersion)
        return;
      const auto ratchet = daveSessionGetKeyRatchet(session, id.c_str());
      if (!ratchet)
        return;
      auto& decryptor = decryptors[id];
      if (!decryptor)
        decryptor = daveDecryptorCreate();
      if (decryptor) {
        daveDecryptorTransitionToPassthroughMode(decryptor, false);
        daveDecryptorTransitionToKeyRatchet(decryptor, ratchet);
      }
      daveKeyRatchetDestroy(ratchet);
    }
    bool execute(std::uint16_t transition) {
      const auto at = transitions.find(transition);
      if (at == transitions.end())
        return true;
      const auto ratchet = daveSessionGetKeyRatchet(session, user.c_str());
      if (!ratchet && transition == initialTransition) {
        // A sole-member reset settles into a pending group without media keys.
        // Sending resumes only after an authenticated commit or welcome.
        daveEncryptorSetKeyRatchet(encryptor, nullptr);
        active = false;
        transitions.erase(at);
        return true;
      }
      if (!ratchet)
        return fail("Voice encryption has no active sending key.");
      daveEncryptorSetKeyRatchet(encryptor, ratchet);
      daveKeyRatchetDestroy(ratchet);
      daveEncryptorSetPassthroughMode(encryptor, false);
      active = daveEncryptorHasKeyRatchet(encryptor);
      transitions.erase(at);
      return active || fail("Voice encryption could not activate its sending key.");
    }
    bool prepare(std::uint16_t transition, std::uint16_t version) {
      if (!supported(version))
        return false;
      if (!transitions.contains(transition) && transitions.size() >= maximumPendingTransitions)
        return fail("Voice encryption has too many pending key changes.");
      preparedVersion = version;
      for (const auto& id : users)
        receiver(id);
      transitions[transition] = version;
      if (transition == initialTransition)
        return execute(transition);
      return queue({.opcode = 23, .bytes = {}, .data = {{"transition_id", transition}}});
    }
    bool invalidTransition(std::uint16_t transition) {
      const auto version = daveSessionGetProtocolVersion(session);
      active = false;
      transitions.clear();
      mlsFailure = false;
      if (!queue({.opcode = 31, .bytes = {}, .data = {{"transition_id", transition}}}))
        return false;
      return initialize(version);
    }
  };

  std::uint16_t Encryption::maximumVersion() { return daveMaxSupportedProtocolVersion(); }
  Encryption::Encryption(std::string user, std::uint64_t channel)
      : m_impl(std::make_unique<Impl>(std::move(user), channel)) {}
  Encryption::~Encryption() = default;
  bool Encryption::initialize(std::uint16_t version) { return m_impl->initialize(version); }
  bool Encryption::handleJson(int opcode, const nlohmann::json& data) {
    auto& state = *m_impl;
    if (!state.failure.empty() || !data.is_object())
      return state.fail("Voice encryption received an invalid control message.");
    if (opcode == 21) {
      const auto transition = unsignedField(data, "transition_id"), version = unsignedField(data, "protocol_version");
      if (!transition || *transition > UINT16_MAX || !version || *version > UINT16_MAX)
        return state.fail("Voice encryption received an invalid key transition.");
      return state.prepare(static_cast<std::uint16_t>(*transition), static_cast<std::uint16_t>(*version));
    }
    if (opcode == 22) {
      const auto transition = unsignedField(data, "transition_id");
      if (!transition || *transition > UINT16_MAX)
        return state.fail("Voice encryption received an invalid transition acknowledgement.");
      return state.execute(static_cast<std::uint16_t>(*transition));
    }
    if (opcode == 24) {
      const auto version = unsignedField(data, "protocol_version");
      const auto epoch = data.find("epoch");
      if (!version || *version > UINT16_MAX || epoch == data.end())
        return state.fail("Voice encryption received an invalid epoch.");
      auto epochNumber = unsignedField(data, "epoch");
      if (epoch->is_string()) {
        const auto& text = epoch->get_ref<const std::string&>();
        std::uint64_t number = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
          epochNumber = number;
      }
      if (!epochNumber || !*epochNumber)
        return state.fail("Voice encryption received an invalid epoch.");
      if (!state.supported(static_cast<std::uint16_t>(*version)))
        return false;
      return *epochNumber != firstEpoch || state.initialize(static_cast<std::uint16_t>(*version));
    }
    return state.fail("Voice encryption received an unknown control message.");
  }
  bool Encryption::handleBinary(int opcode, std::span<const std::uint8_t> payload) {
    auto& state = *m_impl;
    if (!state.failure.empty() || !state.preparedVersion || payload.empty() || payload.size() > maximumHandshakeBytes)
      return state.fail("Voice encryption received an invalid handshake message.");
    state.mlsFailure = false;
    if (opcode == 25) {
      daveSessionSetExternalSender(state.session, payload.data(), payload.size());
      return !state.mlsFailure || state.fail("Voice encryption could not validate the external sender.");
    }
    if (opcode == 27) {
      auto users = state.recognized();
      std::uint8_t* bytes = nullptr;
      std::size_t count = 0;
      daveSessionProcessProposals(
          state.session, payload.data(), payload.size(), users.data(), users.size(), &bytes, &count
      );
      std::vector<std::uint8_t> result;
      if (bytes && count <= maximumHandshakeBytes)
        result.assign(bytes, bytes + count);
      daveFree(bytes);
      if (state.mlsFailure || count > maximumHandshakeBytes)
        return state.fail("Voice encryption could not validate the proposed keys.");
      return result.empty() || state.queue({.opcode = 28, .binary = true, .bytes = std::move(result), .data = {}});
    }
    if ((opcode != 29 && opcode != 30) || payload.size() <= sizeof(std::uint16_t))
      return state.fail("Voice encryption received an invalid group update.");
    const auto transition = transitionPrefix(payload);
    payload = payload.subspan(sizeof(std::uint16_t));
    if (opcode == 29) {
      const auto result = daveSessionProcessCommit(state.session, payload.data(), payload.size());
      const bool ignored = result && daveCommitResultIsIgnored(result);
      const bool valid = result && !daveCommitResultIsFailed(result) && !state.mlsFailure;
      daveCommitResultDestroy(result);
      if (ignored)
        return true;
      if (!valid)
        return state.invalidTransition(transition);
    } else {
      auto users = state.recognized();
      const auto result =
          daveSessionProcessWelcome(state.session, payload.data(), payload.size(), users.data(), users.size());
      const bool valid = result && !state.mlsFailure;
      daveWelcomeResultDestroy(result);
      if (!valid)
        return state.invalidTransition(transition);
    }
    return state.prepare(transition, daveSessionGetProtocolVersion(state.session));
  }
  void Encryption::addUser(std::string user) {
    auto& state = *m_impl;
    if (!validUser(user) || state.users.size() >= maximumParticipants)
      return;
    const auto [at, inserted] = state.users.insert(std::move(user));
    if (inserted && state.preparedVersion)
      state.receiver(*at);
  }
  void Encryption::removeUser(std::string_view user) {
    auto& state = *m_impl;
    if (user == state.user)
      return;
    if (const auto at = state.decryptors.find(user); at != state.decryptors.end()) {
      daveDecryptorDestroy(at->second);
      state.decryptors.erase(at);
    }
    if (const auto at = state.users.find(user); at != state.users.end())
      state.users.erase(at);
  }
  std::vector<EncryptionMessage> Encryption::takeMessages() { return std::exchange(m_impl->outgoing, {}); }
  std::vector<std::uint8_t> Encryption::encrypt(std::uint32_t ssrc, std::span<const std::uint8_t> opus) {
    auto& state = *m_impl;
    if (!ready() || opus.empty() || opus.size() > maximumMediaBytes)
      return {};
    daveEncryptorAssignSsrcToCodec(state.encryptor, ssrc, DAVE_CODEC_OPUS);
    const auto capacity = daveEncryptorGetMaxCiphertextByteSize(state.encryptor, DAVE_MEDIA_TYPE_AUDIO, opus.size());
    if (capacity > maximumMediaBytes)
      return {};
    std::vector<std::uint8_t> result(capacity);
    std::size_t count = 0;
    if (daveEncryptorEncrypt(
            state.encryptor, DAVE_MEDIA_TYPE_AUDIO, ssrc, opus.data(), opus.size(), result.data(), result.size(), &count
        ) != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS
        || count > result.size())
      return {};
    result.resize(count);
    return result;
  }
  std::vector<std::uint8_t> Encryption::decrypt(std::string_view user, std::span<const std::uint8_t> frame) {
    auto& state = *m_impl;
    const auto at = state.decryptors.find(user);
    if (!ready() || at == state.decryptors.end() || frame.empty() || frame.size() > maximumMediaBytes)
      return {};
    const auto capacity = daveDecryptorGetMaxPlaintextByteSize(at->second, DAVE_MEDIA_TYPE_AUDIO, frame.size());
    if (capacity > maximumMediaBytes)
      return {};
    std::vector<std::uint8_t> result(capacity);
    std::size_t count = 0;
    if (daveDecryptorDecrypt(
            at->second, DAVE_MEDIA_TYPE_AUDIO, frame.data(), frame.size(), result.data(), result.size(), &count
        ) != DAVE_DECRYPTOR_RESULT_CODE_SUCCESS
        || count > result.size())
      return {};
    result.resize(count);
    return result;
  }
  bool Encryption::ready() const { return m_impl->active && m_impl->failure.empty(); }
  std::string Encryption::error() const { return m_impl->failure; }
} // namespace voice

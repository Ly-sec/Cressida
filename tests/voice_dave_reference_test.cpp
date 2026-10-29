#include "voice/dave/constants.h"
#include "voice/encryption.h"

#include <algorithm>
#include <array>
#include <dave/dave.h>
#include <iostream>
#include <mls/messages.h>
#include <stdexcept>

namespace {
  using Bytes = std::vector<std::uint8_t>;
  constexpr std::uint64_t room = 9001;
  constexpr std::uint32_t stream = 12345;
  const Bytes opus{0x78, 0x42, 0x19, 0x03, 0x8a, 0xff};
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  Bytes response(voice::Encryption& session, int opcode) {
    auto messages = session.takeMessages();
    const auto at = std::ranges::find(messages, opcode, &voice::EncryptionMessage::opcode);
    require(at != messages.end() && at->binary && !at->bytes.empty(), "The native handshake response exists");
    return at->bytes;
  }
  Bytes transition(const Bytes& bytes, std::uint16_t id) {
    Bytes result{static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id)};
    result.insert(result.end(), bytes.begin(), bytes.end());
    return result;
  }
  struct Delivery {
    cressida_mls::CipherSuite suite{cressida_mls::CipherSuite::ID::P256_AES128GCM_SHA256_P256};
    cressida_mls::SignaturePrivateKey signature = cressida_mls::SignaturePrivateKey::generate(suite);
    Bytes sender() const {
      return cressida_mls::tls::marshal(
          cressida_mls::ExternalSender{signature.public_key, cressida_mls::Credential::basic({0, 1, 1, 0})}
      );
    }
    Bytes add(const Bytes& bytes, std::uint64_t epoch) const {
      cressida_mls::bytes_ns::bytes group(sizeof(room), 0);
      for (std::size_t index = 0; index < sizeof(room); ++index)
        group.at(index) = static_cast<std::uint8_t>(room >> ((sizeof(room) - index - 1) * 8));
      const auto package = cressida_mls::tls::get<cressida_mls::KeyPackage>(bytes);
      const auto proposal = cressida_mls::external_proposal(
          suite, group, epoch, cressida_mls::Proposal{cressida_mls::Add{package}}, 0, signature
      );
      cressida_mls::tls::ostream output;
      output << false << std::vector<cressida_mls::MLSMessage>{proposal};
      return output.bytes();
    }
    static std::pair<Bytes, Bytes> split(const Bytes& bytes) {
      cressida_mls::tls::istream input(bytes);
      cressida_mls::MLSMessage commit;
      cressida_mls::Welcome welcome;
      input >> commit >> welcome;
      require(input.empty(), "The combined reference response is fully consumed");
      return {cressida_mls::tls::marshal(commit), cressida_mls::tls::marshal(welcome)};
    }
  };
  struct Reference {
    bool failed = false;
    DAVESessionHandle session = nullptr;
    DAVEEncryptorHandle encoder = nullptr;
    DAVEDecryptorHandle decoder = nullptr;
    std::array<const char*, 3> users{"1001", "1002", "1003"};
    Reference() {
      session = daveSessionCreate(
          nullptr, nullptr,
          [](const char*, const char*, void* context) { static_cast<Reference*>(context)->failed = true; }, this
      );
      encoder = daveEncryptorCreate();
      decoder = daveDecryptorCreate();
    }
    ~Reference() {
      daveDecryptorDestroy(decoder);
      daveEncryptorDestroy(encoder);
      daveSessionDestroy(session);
    }
    Bytes package() {
      std::uint8_t* data = nullptr;
      std::size_t size = 0;
      daveSessionGetMarshalledKeyPackage(session, &data, &size);
      Bytes result;
      if (data && size <= voice::dave::maximumHandshakeBytes)
        result.assign(data, data + size);
      daveFree(data);
      require(!failed && !result.empty(), "The reference creates a key package");
      return result;
    }
    Bytes propose(const Bytes& bytes) {
      std::uint8_t* data = nullptr;
      std::size_t size = 0;
      daveSessionProcessProposals(session, bytes.data(), bytes.size(), users.data(), users.size(), &data, &size);
      Bytes result;
      if (data && size <= voice::dave::maximumHandshakeBytes)
        result.assign(data, data + size);
      daveFree(data);
      require(!failed && !result.empty(), "The reference creates a commit");
      return result;
    }
    void commit(const Bytes& bytes) {
      auto result = daveSessionProcessCommit(session, bytes.data(), bytes.size());
      const bool valid = result && !daveCommitResultIsFailed(result) && !failed;
      daveCommitResultDestroy(result);
      require(valid, "The reference authenticates the native commit");
    }
    void welcome(const Bytes& bytes) {
      auto result = daveSessionProcessWelcome(session, bytes.data(), bytes.size(), users.data(), users.size());
      const bool valid = result && !failed;
      daveWelcomeResultDestroy(result);
      require(valid, "The reference authenticates the native welcome");
    }
    void keys() {
      auto send = daveSessionGetKeyRatchet(session, "1002");
      auto receive = daveSessionGetKeyRatchet(session, "1001");
      require(send && receive, "The reference derives both sender domains");
      daveEncryptorSetKeyRatchet(encoder, send);
      daveEncryptorSetPassthroughMode(encoder, false);
      daveDecryptorTransitionToKeyRatchet(decoder, receive);
      daveDecryptorTransitionToPassthroughMode(decoder, false);
      daveKeyRatchetDestroy(send);
      daveKeyRatchetDestroy(receive);
      daveEncryptorAssignSsrcToCodec(encoder, stream, DAVE_CODEC_OPUS);
    }
    Bytes encrypt() {
      Bytes result(voice::dave::maximumMediaBytes);
      std::size_t size = 0;
      require(
          daveEncryptorEncrypt(
              encoder, DAVE_MEDIA_TYPE_AUDIO, stream, opus.data(), opus.size(), result.data(), result.size(), &size
          ) == DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS,
          "The reference encrypts Opus"
      );
      result.resize(size);
      return result;
    }
    Bytes decrypt(const Bytes& bytes) {
      Bytes result(voice::dave::maximumMediaBytes);
      std::size_t size = 0;
      if (daveDecryptorDecrypt(
              decoder, DAVE_MEDIA_TYPE_AUDIO, bytes.data(), bytes.size(), result.data(), result.size(), &size
          )
          != DAVE_DECRYPTOR_RESULT_CODE_SUCCESS)
        return {};
      result.resize(size);
      return result;
    }
  };
  void scenario(bool nativeCreator) {
    Delivery delivery;
    voice::Encryption native("1001", room);
    native.addUser("1002");
    require(native.initialize(1), "The native session initializes");
    require(native.handleBinary(25, delivery.sender()), "The native session imports the external sender");
    Reference reference;
    require(reference.session && reference.encoder && reference.decoder, "The reference allocates its objects");
    daveSessionInit(reference.session, 1, room, "1002");
    const auto sender = delivery.sender();
    daveSessionSetExternalSender(reference.session, sender.data(), sender.size());
    auto nativePackage = response(native, 26);
    auto referencePackage = reference.package();
    Bytes combined;
    if (nativeCreator) {
      require(
          native.handleBinary(27, delivery.add(referencePackage, 0)), "Native creation accepts an external proposal"
      );
      combined = response(native, 28);
    } else
      combined = reference.propose(delivery.add(nativePackage, 0));
    auto [commit, welcome] = Delivery::split(combined);
    require(
        native.handleBinary(nativeCreator ? 29 : 30, transition(nativeCreator ? commit : welcome, 0)) && native.ready(),
        "The native participant authenticates the mixed initial group"
    );
    if (nativeCreator)
      reference.welcome(welcome);
    else
      reference.commit(commit);
    reference.keys();
    auto checkMedia = [&] {
      const auto nativeFrame = native.encrypt(stream, opus);
      const auto referenceFrame = reference.encrypt();
      require(reference.decrypt(nativeFrame) == opus, "The reference decrypts the project-owned frame");
      require(
          native.decrypt("1002", referenceFrame) == opus, "The project-owned receiver decrypts the reference frame"
      );
      require(
          reference.decrypt(nativeFrame).empty() && native.decrypt("1002", referenceFrame).empty(),
          "Both implementations reject replay"
      );
    };
    checkMedia();
    reference.keys();
    require(
        native.handleJson(21, {{"transition_id", 7}, {"protocol_version", 1}})
            && native.handleJson(22, {{"transition_id", 7}}),
        "A same-domain transition is accepted"
    );
    checkMedia();

    voice::Encryption third("1003", room);
    third.addUser("1001");
    third.addUser("1002");
    native.addUser("1003");
    require(third.initialize(1) && third.handleBinary(25, sender), "The third participant initializes");
    const auto proposal = delivery.add(response(third, 26), 1);
    require(native.handleBinary(27, proposal), "Native participants prepare the new epoch");
    auto nativeCombined = response(native, 28);
    auto referenceCombined = reference.propose(proposal);
    auto [nextCommit, nextWelcome] = Delivery::split(nativeCreator ? nativeCombined : referenceCombined);
    const auto delayed = native.encrypt(stream, opus);
    reference.commit(nextCommit);
    require(
        native.handleBinary(29, transition(nextCommit, 8)) && third.handleBinary(30, transition(nextWelcome, 8)),
        "A mixed group authenticates the winning epoch"
    );
    reference.keys();
    require(reference.decrypt(delayed) == opus, "The reference retains an old-epoch receive key");
    const auto referenceBeforeExecute = reference.encrypt();
    require(
        native.handleJson(22, {{"transition_id", 8}}) && third.handleJson(22, {{"transition_id", 8}}),
        "Prepared native senders execute the transition"
    );
    require(native.decrypt("1002", referenceBeforeExecute) == opus, "Prepared receive keys authenticate the new epoch");
    checkMedia();
    require(
        third.decrypt("1001", native.encrypt(stream, opus)) == opus,
        "The third member shares the mixed group's exporter"
    );
  }
} // namespace
int main() {
  try {
    daveSetLogSinkCallback([](DAVELoggingSeverity, const char*, int, const char*) {});
    scenario(true);
    scenario(false);
    std::cout << "Independent reference creator/joiner, frames, replay and three-member transitions passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#include "voice/encryption.h"

#include <algorithm>
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
  Bytes binaryMessage(voice::Encryption& session, int opcode) {
    auto messages = session.takeMessages();
    const auto at = std::ranges::find(messages, opcode, &voice::EncryptionMessage::opcode);
    require(at != messages.end() && at->binary && !at->bytes.empty(), "The expected binary handshake response exists");
    return std::move(at->bytes);
  }
  Bytes transition(Bytes payload, std::uint16_t id) {
    payload.insert(payload.begin(), {static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id)});
    return payload;
  }
  struct Peer {
    mlspp::CipherSuite suite{mlspp::CipherSuite::ID::P256_AES128GCM_SHA256_P256};
    mlspp::SignaturePrivateKey signature = mlspp::SignaturePrivateKey::generate(suite);
    mlspp::bytes_ns::bytes group = mlspp::bytes_ns::bytes(sizeof(room), 0);
    Peer() {
      for (std::size_t index = 0; index < sizeof(room); ++index)
        group.at(index) = static_cast<std::uint8_t>(room >> ((sizeof(room) - index - 1) * 8));
    }
    Bytes sender() const {
      mlspp::ExternalSender sender{signature.public_key, mlspp::Credential::basic({0x00, 0x01, 0x01, 0x00})};
      return mlspp::tls::marshal(sender);
    }
    Bytes add(const Bytes& keyPackage, std::uint64_t epoch) const {
      const auto package = mlspp::tls::get<mlspp::KeyPackage>(mlspp::bytes_ns::bytes(keyPackage));
      const auto proposal =
          mlspp::external_proposal(suite, group, epoch, mlspp::Proposal{mlspp::Add{package}}, 0, signature);
      mlspp::tls::ostream message;
      message << false << std::vector<mlspp::MLSMessage>{proposal};
      return message.bytes();
    }
    static std::pair<Bytes, Bytes> split(const Bytes& bytes) {
      mlspp::tls::istream combined{mlspp::bytes_ns::bytes(bytes)};
      mlspp::MLSMessage commit;
      mlspp::Welcome welcome;
      combined >> commit >> welcome;
      return {mlspp::tls::marshal(commit), mlspp::tls::marshal(welcome)};
    }
  };
  void initialize(voice::Encryption& session, const Peer& peer) {
    require(session.initialize(voice::Encryption::maximumVersion()), "A supported encrypted session initializes");
    require(
        !session.ready() && session.encrypt(stream, opus).empty(),
        "No media leaves before authenticated group keys exist"
    );
    require(session.handleBinary(25, peer.sender()), "The synthetic server's external sender is accepted");
  }
} // namespace

int main() {
  try {
    require(voice::Encryption::maximumVersion() > 0, "The linked library supports encrypted voice");
    voice::Encryption unsupported("1001", room);
    require(!unsupported.initialize(0) && !unsupported.ready(), "Unencrypted ordinary calls fail closed");
    voice::Encryption unknown("1001", room);
    require(
        !unknown.initialize(voice::Encryption::maximumVersion() + 1),
        "Unsupported future protocol versions fail explicitly"
    );
    voice::Encryption malformed("1001", room);
    require(
        !malformed.handleJson(21, {{"transition_id", -1}, {"protocol_version", 1}}),
        "Negative transition identifiers are rejected"
    );
    voice::Encryption invalidEpoch("1001", room);
    require(
        !invalidEpoch.handleJson(24, {{"epoch", "not-an-epoch"}, {"protocol_version", 1}}),
        "Invalid text epochs cannot be treated as successful control messages"
    );

    Peer peer;
    voice::Encryption first("1001", room), second("1002", room);
    first.addUser("1002");
    second.addUser("1001");
    initialize(first, peer);
    initialize(second, peer);
    (void)binaryMessage(first, 26);
    const auto proposal = peer.add(binaryMessage(second, 26), 0);
    require(first.handleBinary(27, proposal), "An authenticated proposal produces a commit and welcome");
    auto [commit, welcome] = Peer::split(binaryMessage(first, 28));
    require(first.handleBinary(29, transition(commit, 0)), "The creator accepts its initial group commit");
    require(second.handleBinary(30, transition(welcome, 0)), "The invited user accepts the group welcome");
    require(
        first.ready() && second.ready(),
        "Initial transition zero activates sending keys without a separate acknowledgement"
    );

    const auto encrypted = first.encrypt(stream, opus);
    require(!encrypted.empty() && encrypted != opus, "The transmitted media differs from its plaintext");
    require(second.decrypt("1001", encrypted) == opus, "A second authenticated group member decrypts Opus exactly");
    require(second.decrypt("1001", encrypted).empty(), "Previously consumed encrypted frames cannot be replayed");
    require(
        first.decrypt("1002", second.encrypt(stream + 1, opus)) == opus,
        "Sending and receiving use independent sender ratchets"
    );
    auto tampered = first.encrypt(stream, opus);
    require(!tampered.empty(), "Subsequent media can be encrypted");
    tampered[tampered.size() / 2] ^= 1;
    require(second.decrypt("1001", tampered).empty(), "Modified encrypted media fails authentication");
    require(second.decrypt("9999", first.encrypt(stream, opus)).empty(), "Unknown senders cannot supply audio");

    voice::Encryption third("1003", room);
    initialize(third, peer);
    third.addUser("1001");
    third.addUser("1002");
    first.addUser("1003");
    second.addUser("1003");
    const auto join = peer.add(binaryMessage(third, 26), 1);
    require(
        first.handleBinary(27, join) && second.handleBinary(27, join),
        "Existing participants validate the new member proposal"
    );
    auto [nextCommit, nextWelcome] = Peer::split(binaryMessage(first, 28));
    (void)second.takeMessages();
    constexpr std::uint16_t change = 7;
    require(
        first.handleBinary(29, transition(nextCommit, change))
            && second.handleBinary(29, transition(nextCommit, change))
            && third.handleBinary(30, transition(nextWelcome, change)),
        "Commit and welcome prepare all participants for a group change"
    );
    require(!third.ready(), "A newly joined sender waits for the server to execute its prepared transition");
    for (auto* session : {&first, &second, &third}) {
      const auto responses = session->takeMessages();
      require(
          std::ranges::any_of(
              responses,
              [](const auto& response) {
                return response.opcode == 23 && response.data.value("transition_id", 0) == change;
              }
          ),
          "Prepared participants acknowledge the server transition"
      );
      require(session->handleJson(22, {{"transition_id", change}}), "The server executes the acknowledged transition");
    }
    const auto nextFrame = first.encrypt(stream, opus);
    require(
        second.decrypt("1001", nextFrame) == opus && third.decrypt("1001", nextFrame) == opus,
        "New group ratchets interoperate after the transition"
    );
    second.removeUser("1001");
    require(
        second.decrypt("1001", first.encrypt(stream, opus)).empty(),
        "Departed users immediately lose their receiving slot"
    );
    require(
        second.handleBinary(29, transition({0xff}, change + 1)) && !second.ready() && second.error().empty(),
        "An invalid commit blocks media while requesting a fresh encrypted session"
    );
    const auto recovery = second.takeMessages();
    require(
        recovery.size() == 2 && recovery[0].opcode == 31 && recovery[1].opcode == 26,
        "Recovery signals the rejected transition and creates a new key package"
    );
    require(
        !first.handleJson(21, {{"transition_id", change + 1}, {"protocol_version", 0}})
            && !first.ready()
            && first.encrypt(stream, opus).empty(),
        "A later downgrade cannot silently disable end-to-end encryption"
    );

    Peer resetPeer;
    voice::Encryption remaining("1001", room), departing("1002", room);
    remaining.addUser("1002");
    departing.addUser("1001");
    initialize(remaining, resetPeer);
    initialize(departing, resetPeer);
    (void)binaryMessage(remaining, 26);
    require(
        remaining.handleBinary(27, resetPeer.add(binaryMessage(departing, 26), 0)),
        "The sole-member reset fixture first establishes an authenticated group"
    );
    auto [originalCommit, originalWelcome] = Peer::split(binaryMessage(remaining, 28));
    require(
        remaining.handleBinary(29, transition(originalCommit, 0))
            && departing.handleBinary(30, transition(originalWelcome, 0)),
        "Both original participants activate their initial group"
    );
    remaining.removeUser("1002");
    constexpr std::uint16_t supersededTransition = 9;
    require(
        remaining.handleJson(
            21, {{"transition_id", supersededTransition}, {"protocol_version", voice::Encryption::maximumVersion()}}
        ),
        "An established group can have a pending transition when its final peer leaves"
    );
    (void)remaining.takeMessages();
    require(
        remaining.handleJson(24, {{"epoch", 1}, {"protocol_version", voice::Encryption::maximumVersion()}}),
        "The remaining member accepts the server's new-group epoch"
    );
    require(!binaryMessage(remaining, 26).empty(), "A sole-member reset produces a fresh one-use key package");
    require(
        remaining.handleJson(21, {{"transition_id", 0}, {"protocol_version", voice::Encryption::maximumVersion()}})
            && remaining.error().empty()
            && !remaining.ready()
            && remaining.encrypt(stream, opus).empty(),
        "An initial transition to a pending sole-member group waits without sending old-key or plaintext audio"
    );
    require(
        remaining.handleJson(22, {{"transition_id", supersededTransition}}) && !remaining.ready(),
        "A reset discards superseded transitions instead of activating obsolete media keys"
    );
    voice::Encryption arriving("1003", room);
    initialize(arriving, resetPeer);
    remaining.addUser("1003");
    arriving.addUser("1001");
    require(
        remaining.handleBinary(27, resetPeer.add(binaryMessage(arriving, 26), 0)),
        "A subsequent participant can establish the reset group's first epoch"
    );
    auto [resetCommit, resetWelcome] = Peer::split(binaryMessage(remaining, 28));
    require(
        remaining.handleBinary(29, transition(resetCommit, 0))
            && arriving.handleBinary(30, transition(resetWelcome, 0))
            && remaining.ready()
            && arriving.ready(),
        "Commit and welcome resume encrypted voice after a sole-member reset"
    );
    require(
        arriving.decrypt("1001", remaining.encrypt(stream, opus)) == opus,
        "The reset group derives fresh interoperable media keys"
    );

    std::cout << "Encrypted voice negotiation, MLS group transitions and authenticated Opus frames passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

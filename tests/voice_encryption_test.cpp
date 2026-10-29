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
    cressida_mls::CipherSuite suite{cressida_mls::CipherSuite::ID::P256_AES128GCM_SHA256_P256};
    cressida_mls::SignaturePrivateKey signature = cressida_mls::SignaturePrivateKey::generate(suite);
    cressida_mls::bytes_ns::bytes group = cressida_mls::bytes_ns::bytes(sizeof(room), 0);
    Peer() {
      for (std::size_t index = 0; index < sizeof(room); ++index)
        group.at(index) = static_cast<std::uint8_t>(room >> ((sizeof(room) - index - 1) * 8));
    }
    Bytes sender() const {
      cressida_mls::ExternalSender sender{
          signature.public_key, cressida_mls::Credential::basic({0x00, 0x01, 0x01, 0x00})
      };
      return cressida_mls::tls::marshal(sender);
    }
    Bytes add(const Bytes& keyPackage, std::uint64_t epoch) const {
      const auto package = cressida_mls::tls::get<cressida_mls::KeyPackage>(cressida_mls::bytes_ns::bytes(keyPackage));
      const auto proposal = cressida_mls::external_proposal(
          suite, group, epoch, cressida_mls::Proposal{cressida_mls::Add{package}}, 0, signature
      );
      cressida_mls::tls::ostream message;
      message << false << std::vector<cressida_mls::MLSMessage>{proposal};
      return message.bytes();
    }
    Bytes remove(std::uint32_t index, std::uint64_t epoch) const {
      const auto proposal = cressida_mls::external_proposal(
          suite, group, epoch, cressida_mls::Proposal{cressida_mls::Remove{cressida_mls::LeafIndex{index}}}, 0,
          signature
      );
      cressida_mls::tls::ostream output;
      output << false << std::vector<cressida_mls::MLSMessage>{proposal};
      return output.bytes();
    }
    Bytes revoke(const Bytes& bytes) const {
      cressida_mls::tls::istream input(bytes);
      bool revoked = false;
      std::vector<cressida_mls::MLSMessage> messages;
      input >> revoked >> messages;
      const auto& proposal = cressida_mls::tls::var::get<cressida_mls::PublicMessage>(messages.front().message);
      const auto reference = suite.ref(proposal.authenticated_content());
      cressida_mls::tls::ostream output;
      output << true << std::vector<cressida_mls::bytes_ns::bytes>{reference};
      return output.bytes();
    }
    static std::pair<Bytes, Bytes> split(const Bytes& bytes) {
      cressida_mls::tls::istream combined{cressida_mls::bytes_ns::bytes(bytes)};
      cressida_mls::MLSMessage commit;
      cressida_mls::Welcome welcome;
      combined >> commit >> welcome;
      return {cressida_mls::tls::marshal(commit), cressida_mls::tls::marshal(welcome)};
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
    require(voice::Encryption::maximumVersion() > 0, "The inbuilt backend supports encrypted voice");
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

    constexpr std::uint16_t oldPrepared = 4;
    require(
        first.handleJson(21, {{"transition_id", oldPrepared}, {"protocol_version", 1}}),
        "The old epoch can have a pending transition"
    );
    (void)first.takeMessages();
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
    (void)binaryMessage(first, 28);
    (void)binaryMessage(second, 28);
    const auto revoked = peer.revoke(join);
    require(
        first.handleBinary(27, revoked) && second.handleBinary(27, revoked),
        "Proposal revocation rebuilds candidate state"
    );
    (void)binaryMessage(first, 28);
    (void)binaryMessage(second, 28);
    require(
        first.handleBinary(27, join) && second.handleBinary(27, join),
        "A revoked invitation can be proposed again before use"
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
    require(first.handleJson(22, {{"transition_id", oldPrepared}}), "A delayed old transition is harmless");
    const auto nextFrame = first.encrypt(stream, opus);
    require(
        second.decrypt("1001", nextFrame) == opus && third.decrypt("1001", nextFrame) == opus,
        "New group ratchets interoperate after the transition"
    );
    const auto departedFrame = third.encrypt(stream + 2, opus);
    first.removeUser("1003");
    second.removeUser("1003");
    require(first.decrypt("1003", departedFrame).empty(), "Departure revokes reception before the remove commit");
    const auto removal = peer.remove(2, 2);
    require(
        first.handleBinary(27, removal) && second.handleBinary(27, removal),
        "An authenticated remove proposal is accepted"
    );
    const auto removeCommit = binaryMessage(first, 28);
    (void)binaryMessage(second, 28);
    constexpr std::uint16_t removalTransition = 8;
    for (auto* member : {&first, &second}) {
      require(
          member->handleBinary(29, transition(removeCommit, removalTransition))
              && member->handleJson(22, {{"transition_id", removalTransition}}),
          "The remaining group executes the remove epoch"
      );
      (void)member->takeMessages();
    }
    require(
        second.decrypt("1001", first.encrypt(stream, opus)) == opus,
        "Remaining members share the new remove-epoch secret"
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

    auto welcomeValidation = [&](bool wrongGroup, bool wrongSender, bool unrecognized) {
      Peer delivery, other;
      voice::Encryption creator("1001", room), invitee("1002", wrongGroup ? room + 1 : room);
      creator.addUser("1002");
      if (!unrecognized)
        invitee.addUser("1001");
      initialize(creator, delivery);
      initialize(invitee, wrongSender ? other : delivery);
      (void)binaryMessage(creator, 26);
      require(
          creator.handleBinary(27, delivery.add(binaryMessage(invitee, 26), 0)),
          "The welcome validation fixture creates a group"
      );
      auto [createdCommit, createdWelcome] = Peer::split(binaryMessage(creator, 28));
      require(creator.handleBinary(29, transition(createdCommit, 0)), "The fixture creator authenticates its commit");
      require(
          invitee.handleBinary(30, transition(createdWelcome, 0)), "Invalid welcome state requests bounded recovery"
      );
      if (wrongGroup || wrongSender || unrecognized) {
        require(
            !invitee.ready() && invitee.error().empty(), "Wrong group, external sender or roster never activates media"
        );
      } else {
        require(invitee.ready(), "A fully validated welcome activates media");
        require(
            invitee.handleBinary(30, transition(createdWelcome, 0)) && !invitee.ready(),
            "A welcome cannot reuse consumed join material"
        );
      }
      const auto recovery = invitee.takeMessages();
      require(
          recovery.size() == 2 && recovery.front().opcode == 31 && recovery.back().opcode == 26,
          "Welcome rejection reports the transition and replaces the key package"
      );
    };
    welcomeValidation(true, false, false);
    welcomeValidation(false, true, false);
    welcomeValidation(false, false, true);
    welcomeValidation(false, false, false);

    Peer cachePeer;
    voice::Encryption cacheCreator("1001", room), cacheInvitee("1002", room);
    cacheCreator.addUser("1002");
    cacheInvitee.addUser("1001");
    initialize(cacheCreator, cachePeer);
    initialize(cacheInvitee, cachePeer);
    (void)binaryMessage(cacheCreator, 26);
    require(
        cacheCreator.handleBinary(27, cachePeer.add(binaryMessage(cacheInvitee, 26), 0)),
        "The cache fixture prepares a local commit"
    );
    auto [cachedCommit, cachedWelcome] = Peer::split(binaryMessage(cacheCreator, 28));
    cachedCommit.back() ^= 1;
    require(
        cacheCreator.handleBinary(29, transition(cachedCommit, 0)) && !cacheCreator.ready(),
        "A forged local-looking commit cannot activate the cached candidate state"
    );

    voice::Encryption recoveryLimit("1001", room);
    initialize(recoveryLimit, cachePeer);
    (void)recoveryLimit.takeMessages();
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
      require(
          recoveryLimit.handleBinary(30, transition({0xff}, 0)) && !recoveryLimit.ready(),
          "Malformed welcomes cannot release media"
      );
      (void)recoveryLimit.takeMessages();
    }
    require(
        !recoveryLimit.handleBinary(30, transition({0xff}, 0)) && !recoveryLimit.error().empty(),
        "Repeated invalid updates stop after the finite recovery budget"
    );

    std::cout << "Encrypted voice negotiation, MLS group transitions and authenticated Opus frames passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

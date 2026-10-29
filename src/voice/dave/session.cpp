#include "voice/dave/session.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <mls/state.h>
#include <optional>
#include <stdexcept>

namespace voice::dave {
  namespace {
    namespace mls = cressida_mls;
    using Users = std::set<std::string, std::less<>>;
    [[noreturn]] void invalid() { throw std::invalid_argument("Invalid encrypted voice group"); }
    std::uint64_t numericId(std::string_view user) {
      std::uint64_t value = 0;
      const auto parsed = std::from_chars(user.data(), user.data() + user.size(), value);
      if (user.empty() || parsed.ec != std::errc{} || parsed.ptr != user.data() + user.size() || !value)
        invalid();
      return value;
    }
    Secret identity(std::uint64_t value, bool little = false) {
      Secret bytes(identityBytes);
      for (std::size_t index = 0; index < identityBytes; ++index)
        bytes.at(little ? index : identityBytes - index - 1) = static_cast<std::uint8_t>(value >> (index * byteBits));
      return bytes;
    }
    std::string userId(const mls::Credential& credential) {
      if (credential.type() != mls::CredentialType::basic)
        invalid();
      const auto& bytes = credential.get<mls::BasicCredential>().identity;
      if (bytes.size() != identityBytes)
        invalid();
      std::uint64_t value = 0;
      for (const auto byte : bytes)
        value = (value << byteBits) | byte;
      if (!value)
        invalid();
      return std::to_string(value);
    }
    template <typename T> T read(std::span<const std::uint8_t> payload) {
      if (payload.empty() || payload.size() > maximumHandshakeBytes)
        invalid();
      return mls::tls::get<T>(Bytes(payload.begin(), payload.end()));
    }
  } // namespace

  struct Session::Impl {
    struct Proposal {
      mls::ValidatedContent validated;
      Secret reference;
      std::size_t bytes = 0;
    };
    const std::string self;
    const Secret group;
    mls::CipherSuite suite{mls::CipherSuite::ID::P256_AES128GCM_SHA256_P256};
    std::optional<mls::ExternalSender> external;
    std::optional<mls::SignaturePrivateKey> signing;
    std::optional<mls::HPKEPrivateKey> leafKey, joinKey;
    std::optional<mls::LeafNode> leaf;
    std::optional<mls::KeyPackage> package;
    std::unique_ptr<mls::State> pending, current, proposed, candidate;
    std::optional<mls::MLSMessage> candidateCommit;
    std::vector<Proposal> queued;
    std::uint64_t revision = 0;

    Impl(std::string user, std::uint64_t channel) : self(std::move(user)), group(identity(channel)) {
      (void)numericId(self);
      if (!channel)
        invalid();
    }
    void clearProposals() {
      queued.clear();
      proposed.reset();
      candidate.reset();
      candidateCommit.reset();
    }
    void createPending() {
      if (!external || !leaf || !leafKey || !signing)
        return;
      mls::ExtensionList extensions;
      extensions.add(mls::ExternalSendersExtension{{*external}});
      pending = std::make_unique<mls::State>(group, suite, *leafKey, *signing, *leaf, extensions);
    }
    void verify(const mls::State& state, const Users& users) const {
      if (!external
          || state.group_id() != group
          || state.cipher_suite() != suite
          || state.epoch() == 0
          || state.extensions().extensions.size() != 1)
        invalid();
      const auto senders = state.extensions().find<mls::ExternalSendersExtension>();
      if (!senders || senders->senders.size() != 1 || senders->senders.front() != *external)
        invalid();
      const auto roster = state.roster();
      if (roster.empty() || roster.size() > maximumParticipants)
        invalid();
      Users identities;
      for (const auto& member : roster) {
        auto id = userId(member.credential);
        if (!users.contains(id) || !identities.insert(std::move(id)).second)
          invalid();
      }
      if (!identities.contains(self))
        invalid();
    }
    void accept(std::unique_ptr<mls::State> state, const Users& users) {
      verify(*state, users);
      current = std::move(state);
      pending.reset();
      package.reset();
      joinKey.reset();
      clearProposals();
      ++revision;
    }
  };

  Session::Session(std::string user, std::uint64_t channel)
      : m_impl(std::make_unique<Impl>(std::move(user), channel)) {}
  Session::~Session() = default;
  Bytes Session::initialize() {
    auto& s = *m_impl;
    s.current.reset();
    s.pending.reset();
    s.clearProposals();
    s.package.reset();
    s.joinKey.reset();
    s.leaf.reset();
    s.signing = mls::SignaturePrivateKey::generate(s.suite);
    s.leafKey = mls::HPKEPrivateKey::generate(s.suite);
    auto capabilities = mls::Capabilities::create_default();
    capabilities.cipher_suites = {mls::CipherSuite::ID::P256_AES128GCM_SHA256_P256};
    capabilities.credentials = {mls::CredentialType::basic};
    s.leaf.emplace(
        s.suite, s.leafKey->public_key, s.signing->public_key, mls::Credential::basic(identity(numericId(s.self))),
        capabilities, mls::Lifetime::create_default(), mls::ExtensionList{}, *s.signing
    );
    s.joinKey = mls::HPKEPrivateKey::generate(s.suite);
    s.package.emplace(s.suite, s.joinKey->public_key, *s.leaf, mls::ExtensionList{}, *s.signing);
    s.createPending();
    ++s.revision;
    return mls::tls::marshal(*s.package);
  }
  void Session::externalSender(std::span<const std::uint8_t> payload) {
    auto& s = *m_impl;
    if (s.current || !s.queued.empty())
      invalid();
    auto external = read<mls::ExternalSender>(payload);
    if (external.credential.type() != mls::CredentialType::basic)
      invalid();
    s.external = std::move(external);
    s.createPending();
  }
  Bytes Session::proposals(std::span<const std::uint8_t> payload, const Users& users) {
    auto& s = *m_impl;
    if ((!s.pending && !s.current) || payload.empty() || payload.size() > maximumHandshakeBytes)
      invalid();
    mls::tls::istream input(Bytes(payload.begin(), payload.end()));
    bool revoke = false;
    std::uint64_t size = 0;
    input >> revoke;
    mls::tls::varint::decode(input, size);
    if (size != input.size())
      invalid();
    auto queued = s.queued;
    auto state = std::make_unique<mls::State>(s.current ? *s.current : *s.pending);
    std::size_t count = 0;
    if (revoke) {
      while (!input.empty()) {
        if (++count > maximumProposals)
          invalid();
        Secret reference;
        input >> reference;
        const auto at = std::ranges::find(queued, reference, &Impl::Proposal::reference);
        if (at == queued.end())
          invalid();
        queued.erase(at);
      }
    }
    for (const auto& item : queued)
      state->handle(item.validated);
    std::size_t total = 0;
    for (const auto& item : queued)
      total += item.bytes;
    while (!input.empty()) {
      if (++count > maximumProposals)
        invalid();
      const auto before = input.size();
      mls::MLSMessage message;
      input >> message;
      if (message.group_id() != s.group)
        invalid();
      auto validated = state->unwrap(message);
      const auto& content = validated.authenticated_content();
      if (content.wire_format != mls::WireFormat::mls_public_message
          || content.content.epoch != state->epoch()
          || content.content.content_type() != mls::ContentType::proposal
          || content.content.sender.sender_type() != mls::SenderType::external)
        invalid();
      const auto& proposal = mls::tls::var::get<mls::Proposal>(content.content.content);
      if (proposal.proposal_type() == mls::ProposalType::add) {
        const auto& added = mls::tls::var::get<mls::Add>(proposal.content).key_package;
        if (!users.contains(userId(added.leaf_node.credential)))
          invalid();
      } else if (proposal.proposal_type() != mls::ProposalType::remove)
        invalid();
      auto reference = s.suite.ref(content);
      if (std::ranges::find(queued, reference, &Impl::Proposal::reference) != queued.end())
        continue;
      const auto bytes = before - input.size();
      if (queued.size() >= maximumProposals || bytes > maximumHandshakeBytes - total)
        invalid();
      total += bytes;
      state->handle(validated);
      queued.push_back({std::move(validated), std::move(reference), bytes});
    }
    if (!count)
      invalid();
    auto [commit, welcome, candidate] =
        state->commit(mls::hpke::random_bytes(s.suite.secret_size()), mls::CommitOpts{{}, true, false, {}}, {});
    s.verify(candidate, users);
    mls::tls::ostream output;
    output << commit;
    if (!welcome.secrets.empty())
      output << welcome;
    if (output.size() > maximumHandshakeBytes)
      invalid();
    s.queued = std::move(queued);
    s.proposed = std::move(state);
    s.candidateCommit = std::move(commit);
    s.candidate = std::make_unique<mls::State>(std::move(candidate));
    return output.bytes();
  }
  Update Session::commit(std::span<const std::uint8_t> payload, const Users& users) {
    auto& s = *m_impl;
    if (!s.proposed) {
      if (!s.current)
        return Update::Ignored;
      invalid();
    }
    const auto message = read<mls::MLSMessage>(payload);
    const bool own = s.candidateCommit && message == *s.candidateCommit;
    if ((!s.current && !own) || message.group_id() != s.group)
      invalid();
    const auto validated = s.proposed->unwrap(message);
    const auto& content = validated.authenticated_content();
    if (content.wire_format != mls::WireFormat::mls_public_message
        || content.content.epoch != s.proposed->epoch()
        || content.content.content_type() != mls::ContentType::commit
        || content.content.sender.sender_type() != mls::SenderType::member)
      invalid();
    for (const auto& proposal : mls::tls::var::get<mls::Commit>(content.content.content).proposals)
      if (mls::tls::variant<mls::ProposalOrRefType>::type(proposal.content) != mls::ProposalOrRefType::reference)
        invalid();
    auto processing = *s.proposed;
    const auto next = processing.handle(validated, own ? std::optional<mls::State>(*s.candidate) : std::nullopt);
    if (!next)
      invalid();
    s.accept(std::make_unique<mls::State>(*next), users);
    return Update::Accepted;
  }
  void Session::welcome(std::span<const std::uint8_t> payload, const Users& users) {
    auto& s = *m_impl;
    if (s.current || !s.joinKey || !s.leafKey || !s.signing || !s.package || !s.external)
      invalid();
    const auto welcome = read<mls::Welcome>(payload);
    auto state = std::make_unique<mls::State>(
        *s.joinKey, *s.leafKey, *s.signing, *s.package, welcome, std::nullopt, std::map<Secret, Secret>{}
    );
    s.accept(std::move(state), users);
  }
  Secret Session::secret(std::string_view user) const {
    const auto& s = *m_impl;
    if (!s.current)
      return {};
    bool recognized = false;
    for (const auto& member : s.current->roster())
      recognized |= userId(member.credential) == user;
    if (!recognized)
      return {};
    // This byte sequence is the fixed protocol exporter label.
    static constexpr std::array<std::uint8_t, 24> label{68,  105, 115, 99, 111, 114, 100, 32,  83,  101, 99,  117,
                                                        114, 101, 32,  70, 114, 97,  109, 101, 115, 32,  118, 48};
    return s.current->do_export(std::string(label.begin(), label.end()), identity(numericId(user), true), keyBytes);
  }
  std::uint64_t Session::revision() const { return m_impl->revision; }
} // namespace voice::dave

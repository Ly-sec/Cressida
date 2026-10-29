#include "voice/dave/frame.h"
#include "voice/encryption.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <mls/messages.h>
#include <random>
#include <stdexcept>
#include <tls/tls_syntax.h>

namespace {
  using namespace voice::dave;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  template <class F> void rejected(F&& action) {
    try {
      action();
    } catch (const std::exception&) {
      return;
    }
    throw std::runtime_error("Malformed MLS data was accepted");
  }
} // namespace
int main() {
  try {
    using namespace voice::dave;
    // NIST SP 800-38D, AES-128-GCM zero-key/zero-IV block vector.
    const Secret zero(keyBytes, 0);
    const Bytes plaintext(keyBytes, 0);
    const Bytes expected{0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92, 0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2,
                         0xfe, 0x78, 0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd, 0x00, 0x0c, 0xfa, 0xfa};
    require(seal(plaintext, zero, 0) == expected, "The fixed GCM vector and protocol footer match");
    require(open(*parse(expected), zero) == plaintext, "The fixed reference frame decrypts exactly");
    for (std::size_t length = 0; length < expected.size(); ++length) {
      const auto parsed = parse(std::span(expected).first(length));
      require(!parsed || open(*parsed, zero).empty(), "Truncated frames cannot authenticate");
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
      auto altered = expected;
      altered[index] ^= 1;
      const auto parsed = parse(altered);
      require(!parsed || open(*parsed, zero).empty(), "Every altered frame byte is rejected");
    }
    require(seal(Bytes(maximumMediaBytes, 1), zero, 0).empty(), "The media limit includes framing overhead");
    auto malformed = expected;
    malformed.insert(malformed.end() - suffixBytes, {0x80, 0x00});
    malformed[malformed.size() - suffixBytes] += 2;
    require(!parse(malformed), "Non-minimal ULEB128 metadata is rejected");
    malformed = expected;
    malformed.insert(malformed.end() - suffixBytes, {0x7f, 0x01});
    malformed[malformed.size() - suffixBytes] += 2;
    require(!parse(malformed), "Clear ranges cannot escape the media payload");

    auto now = Clock::time_point{};
    Receiver receiver([&] { return now; });
    Sender sender;
    Secret secret(keyBytes, 7), nextSecret(keyBytes, 9);
    const Bytes opus{0x78, 0x42, 0x19, 0x03};
    require(sender.install(secret) && receiver.install(secret), "Sender and receiver install a key domain");
    const auto first = sender.encrypt(opus);
    require(receiver.decrypt(first) == opus && receiver.decrypt(first).empty(), "Media replay is rejected");
    require(sender.install(secret) && receiver.install(secret), "The current domain can be reinstalled");
    require(receiver.decrypt(first).empty(), "Reinstallation retains the replay history");
    const auto second = sender.encrypt(opus);
    require(parse(second)->nonce == parse(first)->nonce + 1, "Reinstallation never restarts the nonce");
    auto bad = second;
    bad.front() ^= 1;
    require(
        receiver.decrypt(bad).empty() && receiver.decrypt(second) == opus,
        "Authentication failures do not consume a nonce"
    );
    const auto earlier = sender.encrypt(opus), later = sender.encrypt(opus);
    require(
        receiver.decrypt(later) == opus && receiver.decrypt(earlier) == opus, "Reordered unseen frames remain valid"
    );
    const auto old = sender.encrypt(opus);
    require(receiver.install(nextSecret), "A new epoch retains the old receive domain briefly");
    require(receiver.decrypt(old) == opus, "Prior-epoch traffic survives the key transition");
    const auto expired = sender.encrypt(opus);
    now += keyGrace;
    receiver.expire();
    require(receiver.decrypt(expired).empty(), "Old epoch keys expire without incoming traffic");
    require(sender.install(nextSecret) && receiver.decrypt(sender.encrypt(opus)) == opus, "The new epoch still works");

    // Accelerate a long-lived sender with an injected clock and independently selected generations.
    cressida_mls::CipherSuite suite{cressida_mls::CipherSuite::ID::P256_AES128GCM_SHA256_P256};
    cressida_mls::HashRatchet ratchet(suite, secret);
    Receiver rollover([&] { return now; });
    require(rollover.install(secret), "The rollover receiver starts with a fresh domain");
    constexpr auto wrapFrames = std::uint64_t{1} << std::numeric_limits<std::uint32_t>::digits;
    now += std::chrono::seconds(wrapFrames / maximumFramesPerSecond + 1);
    constexpr auto beforeWrap = std::numeric_limits<std::uint32_t>::max();
    const auto previous = seal(opus, ratchet.get(generationWrap - 1).key, beforeWrap);
    // Establish generation 127 first so no single packet requests excessive ratchet work.
    constexpr auto intermediate = (generationWrap / 2 - 1);
    require(
        rollover.decrypt(
            seal(opus, ratchet.get(intermediate).key, static_cast<std::uint32_t>(intermediate << generationShift))
        ) == opus,
        "A long-lived domain accepts a bounded generation advance"
    );
    require(rollover.decrypt(previous) == opus, "The final frame before nonce wrap decrypts");
    require(
        rollover.decrypt(seal(opus, ratchet.get(generationWrap).key, 0)) == opus,
        "Nonce wrap uses the next full generation"
    );
    require(rollover.decrypt(previous).empty(), "Replay history crosses nonce wrap");

    Receiver bounded([&] { return now; });
    for (std::size_t index = 0; index < maximumPendingTransitions; ++index)
      require(
          bounded.install(Secret(keyBytes, static_cast<std::uint8_t>(index))), "The receive epoch bound is inclusive"
      );
    require(
        !bounded.install(Secret(keyBytes, static_cast<std::uint8_t>(maximumPendingTransitions))),
        "Excess receive epochs are rejected"
    );
    now += keyGrace;
    bounded.expire();
    require(
        bounded.install(Secret(keyBytes, static_cast<std::uint8_t>(maximumPendingTransitions))),
        "Idle expiry releases bounded receive slots"
    );

    rejected([] { (void)cressida_mls::tls::get<Bytes>({}); });
    rejected([] { (void)cressida_mls::tls::get<std::uint8_t>({1, 2}); });
    rejected([] { (void)cressida_mls::tls::get<Bytes>({0x40}); });
    voice::Encryption parserSource("1001", 9001);
    require(parserSource.initialize(1), "A synthetic key package is available for parser mutation tests");
    const auto package = parserSource.takeMessages().front().bytes;
    for (std::size_t length = 0; length < package.size(); ++length)
      rejected([&] {
        (void)cressida_mls::tls::get<cressida_mls::KeyPackage>(Bytes(package.begin(), package.begin() + length));
      });
    constexpr std::size_t mutationCases = 2048, maximumMutationBytes = 128;
    std::minstd_rand random(9001);
    for (std::size_t trial = 0; trial < mutationCases; ++trial) {
      Bytes fuzz(random() % maximumMutationBytes);
      for (auto& byte : fuzz)
        byte = static_cast<std::uint8_t>(random());
      if (const auto parsed = parse(fuzz))
        (void)open(*parsed, zero);
      try {
        (void)cressida_mls::tls::get<cressida_mls::MLSMessage>(fuzz);
      } catch (const std::exception&) {
      }
    }
    std::cout << "DAVE vectors, framing, key domains, replay, rollover and strict MLS parsing passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

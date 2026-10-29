#include "voice/dave/frame.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace voice::dave {
  namespace {
    using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    bool equal(const Secret& a, const Secret& b) {
      return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
    }
    auto nonceBuffer(std::uint32_t nonce) {
      std::array<std::uint8_t, nonceBytes> result{};
      for (std::size_t index = 0; index < sizeof(nonce); ++index)
        result[nonceBytes - sizeof(nonce) + index] = static_cast<std::uint8_t>(nonce >> (index * byteBits));
      return result;
    }
    void writeLeb(Bytes& output, std::uint32_t value) {
      do {
        auto next = static_cast<std::uint8_t>(value & lebMask);
        value >>= lebBits;
        output.push_back(value ? next | lebContinuation : next);
      } while (value);
    }
    std::optional<std::uint32_t> readLeb(std::span<const std::uint8_t>& input) {
      std::uint32_t result = 0;
      for (unsigned shift = 0; shift < std::numeric_limits<std::uint32_t>::digits; shift += lebBits) {
        if (input.empty())
          return {};
        const auto value = input.front();
        input = input.subspan(1);
        const auto bits = static_cast<std::uint32_t>(value & lebMask);
        if (bits > (std::numeric_limits<std::uint32_t>::max() >> shift))
          return {};
        result |= static_cast<std::uint32_t>(bits) << shift;
        if (!(value & lebContinuation)) {
          if (shift && !bits)
            return {};
          return result;
        }
      }
      return {};
    }
    cressida_mls::CipherSuite suite() {
      return cressida_mls::CipherSuite{cressida_mls::CipherSuite::ID::P256_AES128GCM_SHA256_P256};
    }
  } // namespace

  Bytes seal(std::span<const std::uint8_t> plaintext, const Secret& key, std::uint32_t nonce) {
    if (plaintext.empty() || plaintext.size() > maximumMediaBytes || key.size() != keyBytes)
      return {};
    const auto iv = nonceBuffer(nonce);
    Context context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    Bytes result(plaintext.size() + tagBytes);
    int written = 0, final = 0;
    if (!context
        || EVP_EncryptInit_ex(context.get(), EVP_aes_128_gcm(), nullptr, key.data(), iv.data()) != 1
        || EVP_EncryptUpdate(
               context.get(), result.data(), &written, plaintext.data(), static_cast<int>(plaintext.size())
           ) != 1
        || EVP_EncryptFinal_ex(context.get(), result.data() + written, &final) != 1
        || written + final != static_cast<int>(plaintext.size())
        || EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, tagBytes, result.data() + plaintext.size()) != 1)
      return {};
    writeLeb(result, nonce);
    const auto footer = result.size() - plaintext.size() + suffixBytes;
    result.insert(result.end(), {static_cast<std::uint8_t>(footer), markerByte, markerByte});
    if (result.size() > maximumMediaBytes)
      return {};
    return result;
  }

  std::optional<Frame> parse(std::span<const std::uint8_t> bytes) {
    constexpr auto minimumFooter = tagBytes + suffixBytes + 1;
    if (bytes.size() <= minimumFooter
        || bytes.size() > maximumMediaBytes
        || bytes[bytes.size() - 1] != markerByte
        || bytes[bytes.size() - 2] != markerByte)
      return {};
    const auto footer = bytes[bytes.size() - suffixBytes];
    if (footer < minimumFooter || footer >= bytes.size())
      return {};
    const auto media = bytes.first(bytes.size() - footer);
    const auto tail = bytes.subspan(media.size(), footer - suffixBytes);
    Frame frame;
    std::ranges::copy(tail.first(tagBytes), frame.tag.begin());
    auto metadata = tail.subspan(tagBytes);
    const auto nonce = readLeb(metadata);
    if (!nonce)
      return {};
    frame.nonce = *nonce;
    std::size_t at = 0;
    while (!metadata.empty()) {
      const auto offset = readLeb(metadata), length = readLeb(metadata);
      if (!offset || !length || !*length || *offset < at || *offset > media.size() || *length > media.size() - *offset)
        return {};
      frame.ciphertext.insert(frame.ciphertext.end(), media.begin() + at, media.begin() + *offset);
      frame.authenticated.insert(frame.authenticated.end(), media.begin() + *offset, media.begin() + *offset + *length);
      frame.ranges.emplace_back(*offset, *length);
      at = *offset + *length;
    }
    frame.ciphertext.insert(frame.ciphertext.end(), media.begin() + at, media.end());
    return frame;
  }

  Bytes open(const Frame& frame, const Secret& key) {
    if (key.size() != keyBytes || frame.ciphertext.size() + frame.authenticated.size() > maximumMediaBytes)
      return {};
    const auto iv = nonceBuffer(frame.nonce);
    Context context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    Secret plaintext(frame.ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
    int written = 0, final = 0;
    if (!context
        || EVP_DecryptInit_ex(context.get(), EVP_aes_128_gcm(), nullptr, key.data(), iv.data()) != 1
        || (!frame.authenticated.empty()
            && EVP_DecryptUpdate(
                   context.get(), nullptr, &written, frame.authenticated.data(),
                   static_cast<int>(frame.authenticated.size())
               ) != 1)
        || EVP_DecryptUpdate(
               context.get(), plaintext.data(), &written, frame.ciphertext.data(),
               static_cast<int>(frame.ciphertext.size())
           ) != 1
        || EVP_CIPHER_CTX_ctrl(
               context.get(), EVP_CTRL_GCM_SET_TAG, tagBytes, const_cast<std::uint8_t*>(frame.tag.data())
           ) != 1
        || EVP_DecryptFinal_ex(context.get(), plaintext.data() + written, &final) != 1
        || written + final != static_cast<int>(frame.ciphertext.size()))
      return {};
    Bytes result;
    result.reserve(frame.ciphertext.size() + frame.authenticated.size());
    std::size_t encrypted = 0, authenticated = 0;
    for (const auto& [offset, length] : frame.ranges) {
      const auto count = offset - result.size();
      result.insert(result.end(), plaintext.data() + encrypted, plaintext.data() + encrypted + count);
      encrypted += count;
      result.insert(
          result.end(), frame.authenticated.begin() + authenticated,
          frame.authenticated.begin() + authenticated + length
      );
      authenticated += length;
    }
    result.insert(result.end(), plaintext.data() + encrypted, plaintext.data() + written);
    return result;
  }

  bool Sender::install(const Secret& secret) {
    if (secret.size() != keyBytes)
      return false;
    if (m_ratchet && equal(m_domain, secret))
      return true;
    clear();
    m_domain = secret;
    m_ratchet.emplace(suite(), secret);
    return true;
  }
  void Sender::clear() {
    m_ratchet.reset();
    m_domain = {};
    m_counter = 0;
  }
  Bytes Sender::encrypt(std::span<const std::uint8_t> opus) {
    constexpr auto maximumCounter =
        (static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) << generationShift);
    if (!m_ratchet || opus.empty() || opus.size() > maximumMediaBytes || m_counter >= maximumCounter)
      return {};
    const auto generation = static_cast<std::uint32_t>(++m_counter >> generationShift);
    auto key = m_ratchet->get(generation).key;
    auto result = seal(opus, key, static_cast<std::uint32_t>(m_counter));
    // Sending never needs keys for a previous generation.
    for (auto at = m_ratchet->cache.begin(); at != m_ratchet->cache.end() && at->first < generation;)
      at = m_ratchet->cache.erase(at);
    return result;
  }

  bool Receiver::install(const Secret& secret) {
    if (secret.size() != keyBytes)
      return false;
    expire();
    for (const auto& domain : m_domains)
      if (equal(domain.identity, secret))
        return true;
    if (m_domains.size() >= maximumPendingTransitions)
      return false;
    const auto now = m_now();
    for (auto& domain : m_domains)
      domain.expires = std::min(domain.expires, now + keyGrace);
    m_domains.push_back(
        {secret, cressida_mls::HashRatchet(suite(), secret), now, Clock::time_point::max(), {}, {}, {}}
    );
    return true;
  }
  void Receiver::expire() {
    const auto now = m_now();
    std::erase_if(m_domains, [&](const auto& domain) { return domain.expires <= now; });
    for (auto& domain : m_domains) {
      for (auto at = domain.keyExpiry.begin(); at != domain.keyExpiry.end();) {
        if (at->second <= now) {
          domain.ratchet.erase(at->first);
          at = domain.keyExpiry.erase(at);
        } else
          ++at;
      }
    }
  }
  Bytes Receiver::decrypt(std::span<const std::uint8_t> bytes) {
    expire();
    const auto frame = parse(bytes);
    if (!frame)
      return {};
    for (auto& domain : m_domains) {
      const auto lastGeneration = domain.latest.value_or(0) >> generationShift;
      auto generation = (lastGeneration / generationWrap) * generationWrap + (frame->nonce >> generationShift);
      if (generation + generationWrap / 2 < lastGeneration)
        generation += generationWrap;
      else if (generation > lastGeneration + generationWrap / 2 && generation >= generationWrap)
        generation -= generationWrap;
      if (generation > std::numeric_limits<std::uint32_t>::max()
          || generation > lastGeneration + maximumGenerationAdvance)
        continue;
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(m_now() - domain.created).count();
      const auto permitted =
          (static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0)) * maximumFramesPerSecond) >> generationShift;
      if (generation > permitted)
        continue;
      const auto counter =
          (generation << generationShift) | (frame->nonce & ((std::uint64_t{1} << generationShift) - 1));
      if (domain.latest
          && counter <= *domain.latest
          && (*domain.latest - counter >= replayWindow || domain.seen.test(*domain.latest - counter)))
        continue;
      try {
        // Unauthenticated input cannot advance or erase the accepted key schedule.
        auto candidate = domain.ratchet;
        const auto key = candidate.get(static_cast<std::uint32_t>(generation)).key;
        auto result = open(*frame, key);
        if (result.empty())
          continue;
        domain.ratchet = std::move(candidate);
        if (!domain.latest || counter > *domain.latest) {
          const auto distance = domain.latest ? counter - *domain.latest : replayWindow;
          if (distance >= replayWindow)
            domain.seen.reset();
          else
            domain.seen <<= static_cast<std::size_t>(distance);
          domain.latest = counter;
        }
        domain.seen.set(static_cast<std::size_t>(*domain.latest - counter));
        for (const auto& [index, unused] : domain.ratchet.cache)
          if (index < generation && !domain.keyExpiry.contains(index))
            domain.keyExpiry.emplace(index, m_now() + keyGrace);
        return result;
      } catch (const std::exception&) {
        // Expired or invalid generation requests drop only this frame.
      }
    }
    return {};
  }
} // namespace voice::dave

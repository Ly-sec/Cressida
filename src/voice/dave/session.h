#pragma once

#include "voice/dave/frame.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace voice::dave {
  enum class Update { Accepted, Ignored };
  class Session {
  public:
    Session(std::string user, std::uint64_t channel);
    ~Session();
    Bytes initialize();
    void externalSender(std::span<const std::uint8_t> payload);
    Bytes proposals(std::span<const std::uint8_t> payload, const std::set<std::string, std::less<>>& users);
    Update commit(std::span<const std::uint8_t> payload, const std::set<std::string, std::less<>>& users);
    void welcome(std::span<const std::uint8_t> payload, const std::set<std::string, std::less<>>& users);
    Secret secret(std::string_view user) const;
    std::uint64_t revision() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };
} // namespace voice::dave

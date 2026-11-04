#pragma once

#include <string_view>

namespace cressida::build_info {

  std::string_view version() noexcept;

  std::string_view revision() noexcept;

} // namespace cressida::build_info

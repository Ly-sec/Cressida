#include "core/build_info.h"

#include "cressida_git_revision.h"
#include "cressida_version.h"

namespace cressida::build_info {

  std::string_view version() noexcept { return CRESSIDA_VERSION; }

  std::string_view revision() noexcept { return CRESSIDA_GIT_REVISION; }

} // namespace cressida::build_info

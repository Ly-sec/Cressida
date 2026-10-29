#pragma once

#include "api/models.h"
#include "api/rest_transfer.h"

namespace api {
  // These short-lived signed destinations belong to the request worker only.
  struct CommandUploadSlot {
    std::size_t index = 0;
    std::string url, filename;
  };
  nlohmann::json commandUploadRequest(const std::vector<Upload>& uploads);
  std::vector<CommandUploadSlot> commandUploadSlots(const nlohmann::json& response, const std::vector<Upload>& uploads);
  nlohmann::json
  commandUploadedAttachments(const std::vector<CommandUploadSlot>& slots, const std::vector<Upload>& uploads);
  bool uploadCommandFile(
      const CommandUploadSlot& slot, const Upload& file, CURLM* multi, const process::WakeEvent& wake,
      const RestRequestScope& scope
  );
} // namespace api

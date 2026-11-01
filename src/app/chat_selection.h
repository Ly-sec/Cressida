#pragma once

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace app {
  struct ChatSelectionDocument {
    struct Section {
      std::string message;
      std::size_t offset, size;
    };
    std::string text;
    std::vector<Section> sections;
    std::map<std::string, std::size_t> indices;

    void append(const std::string& message, std::string_view value) {
      if (!text.empty() && !text.ends_with('\n'))
        text += '\n';
      indices[message] = sections.size();
      sections.push_back({message, text.size(), value.size()});
      text += value;
    }
    const Section* find(const std::string& message) const {
      const auto found = indices.find(message);
      return found == indices.end() ? nullptr : &sections[found->second];
    }
    const Section* at(std::size_t position) const {
      const auto found = std::ranges::upper_bound(sections, position, {}, &Section::offset);
      return found == sections.begin() ? nullptr : &*std::prev(found);
    }
    std::optional<std::size_t> rebase(std::size_t position, const ChatSelectionDocument& previous) const {
      const auto* old = previous.at(position);
      const auto* current = old ? find(old->message) : nullptr;
      if (!current
          || std::string_view(previous.text).substr(old->offset, old->size)
              != std::string_view(text).substr(current->offset, current->size))
        return {};
      return current->offset + std::min(position - old->offset, current->size);
    }
    bool retains(std::size_t first, std::size_t last, const ChatSelectionDocument& previous) const {
      return std::ranges::all_of(previous.sections, [&](const auto& section) {
        if (section.offset + section.size <= first || section.offset >= last)
          return true;
        const auto* current = find(section.message);
        return current
            && std::string_view(previous.text).substr(section.offset, section.size)
            == std::string_view(text).substr(current->offset, current->size);
      });
    }
  };
} // namespace app

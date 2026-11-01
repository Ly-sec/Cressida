#include "app/chat_selection.h"
#include "app/text_edit.h"

#include <iostream>
#include <stdexcept>

int main() {
  try {
    auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    app::ChatSelectionDocument conversation;
    conversation.append("1", "Maya 10:00\nFirst 🌿 message.");
    conversation.append("2", "Noah 10:01\nSecond message.");
    const auto begin = conversation.text.find("First");
    const auto end = conversation.text.size();
    app::TextEdit range{.caret = end, .anchor = begin};
    require(
        range.selection(conversation.text) == "First 🌿 message.\nNoah 10:01\nSecond message.",
        "Conversation selection includes message separators and UTF-8 names/text"
    );
    app::ChatSelectionDocument prepended;
    prepended.append("0", "An older message.\n");
    prepended.append("1", "Maya 10:00\nFirst 🌿 message.");
    prepended.append("2", "Noah 10:01\nSecond message.");
    prepended.append("3", "A new arrival.");
    const auto movedBegin = prepended.rebase(begin, conversation), movedEnd = prepended.rebase(end, conversation);
    require(
        movedBegin && movedEnd && prepended.retains(begin, end, conversation),
        "History prepends and incoming messages preserve the selected messages"
    );
    range.anchor = *movedBegin;
    range.caret = *movedEnd;
    require(
        range.selection(prepended.text) == "First 🌿 message.\nNoah 10:01\nSecond message.",
        "Rebased selection excludes unrelated prepends and arrivals"
    );
    app::ChatSelectionDocument edited;
    edited.append("1", "Changed text");
    require(
        !edited.retains(begin, end, conversation) && !edited.rebase(begin, conversation),
        "Deleted or changed selected messages revoke stale byte positions"
    );
    app::TextEdit edit;
    std::string text = "one\ntwo 👩‍💻";
    edit.end(text);
    edit.erase(text, false);
    require(text == "one\ntwo ", "Backspace removes a complete joined emoji");
    edit.replace(text, "🌿");
    edit.move(text, false, true);
    require(edit.selection(text) == "🌿", "Shift movement selects a whole grapheme");
    edit.replace(text, "second\nthird");
    require(text == "one\ntwo second\nthird", "Replacing a selection preserves multiline text");
    edit.anchor = 0;
    edit.caret = 3;
    edit.replace(text, "first");
    require(text.starts_with("first\ntwo") && edit.caret == 5, "Insertion replaces selection and places caret");
    edit.end(text);
    edit.move(text, false, false);
    edit.erase(text, true);
    require(text.ends_with("thir"), "Delete removes the following character");
    edit.anchor = 0;
    edit.caret = text.size();
    edit.erase(text, false);
    require(text.empty() && edit.caret == 0 && edit.anchor == 0, "Deleting an entire selection resets both ends");
    edit.erase(text, false);
    require(text.empty(), "Backspace at start is harmless");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

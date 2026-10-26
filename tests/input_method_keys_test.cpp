#include "app/input_method_keys.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }
  struct Key {
    enum Symbol { Enter, Letter, PushToTalk, Modifier } symbol;
    std::string text;
  };
} // namespace

int main() {
  try {
    {
      app::InputMethodKeys<Key> keys;
      app::InputMethodKeyContext context{.pending = true, .focused = true, .generation = 1};
      std::string committed;
      std::vector<std::string> sent;
      std::vector<bool> deliveries;
      const auto deliver = [&](const Key& event, bool pressed) {
        deliveries.push_back(pressed);
        if (pressed && event.symbol == Key::Enter)
          sent.push_back(committed);
      };
      keys.defer({Key::Enter, {}}, true, context.generation, deliver);
      keys.defer({Key::Enter, {}}, false, context.generation, deliver);
      keys.flush([&] { return context; }, deliver);
      require(sent.empty() && deliveries.empty(), "An IME batch defers both halves of a forwarded Enter key");
      committed = "á";
      context.pending = false;
      keys.flush([&] { return context; }, deliver);
      require(
          sent == std::vector<std::string>{"á"} && deliveries == std::vector<bool>{true, false},
          "Forwarded Enter submits the completed composition exactly once before its release"
      );
      keys.flush([&] { return context; }, deliver);
      require(sent.size() == 1 && deliveries.size() == 2, "Flushing a completed batch cannot repeat its action");
    }
    {
      app::InputMethodKeys<Key> keys;
      app::InputMethodKeyContext context{.pending = true, .focused = true, .generation = 4};
      bool held = true;
      int submitted = 0, releases = 0;
      const auto deliver = [&](const Key& event, bool pressed) {
        if (pressed)
          ++submitted;
        else {
          ++releases;
          if (event.symbol == Key::PushToTalk)
            held = false;
        }
      };
      keys.defer({Key::Enter, {}}, true, context.generation, deliver);
      keys.defer({Key::PushToTalk, {}}, false, context.generation, deliver);
      ++context.generation;
      context.pending = context.focused = false;
      keys.flush([&] { return context; }, deliver);
      require(
          submitted == 0 && !held && releases == 1,
          "Closing an editor discards its old Enter while preserving a previously held microphone key release"
      );
    }
    {
      app::InputMethodKeys<Key> keys;
      app::InputMethodKeyContext context{.pending = true, .focused = true, .generation = 7};
      bool held = true, modifierHeld = true;
      std::vector<Key::Symbol> delivered;
      const auto deliver = [&](const Key& event, bool pressed) {
        require(!pressed, "Blur cleanup must never replay a queued press");
        delivered.push_back(event.symbol);
        if (event.symbol == Key::PushToTalk)
          held = false;
        if (event.symbol == Key::Modifier)
          modifierHeld = false;
      };
      keys.defer({Key::Letter, "x"}, true, context.generation, deliver);
      keys.defer({Key::PushToTalk, {}}, false, context.generation, deliver);
      keys.defer({Key::Modifier, {}}, false, context.generation, deliver);
      keys.cancel(deliver);
      keys.flush([&] { return context; }, deliver);
      keys.cancel(deliver);
      require(
          !held && !modifierHeld && delivered == std::vector<Key::Symbol>{Key::PushToTalk, Key::Modifier},
          "Blur cancels pending typing and releases each held source once even before IME done"
      );
    }
    {
      app::InputMethodKeys<int> keys;
      std::set<int> held;
      std::vector<int> released;
      int presses = 0;
      const auto deliver = [&](int key, bool pressed) {
        if (pressed)
          ++presses;
        else {
          require(held.erase(key) == 1, "Overflow must deliver each queued release exactly once");
          released.push_back(key);
        }
      };
      for (std::size_t i = 0; i < decltype(keys)::maximumPendingKeys; ++i) {
        const auto key = static_cast<int>(i);
        const bool pressed = i % 2 == 0;
        if (!pressed)
          held.insert(key);
        keys.defer(key, pressed, 1, deliver);
      }
      const auto incoming = static_cast<int>(decltype(keys)::maximumPendingKeys);
      held.insert(incoming);
      keys.defer(incoming, false, 1, deliver);
      keys.flush([] { return app::InputMethodKeyContext{.focused = true, .generation = 1}; }, deliver);
      require(
          presses == 0
              && held.empty()
              && released.size() == decltype(keys)::maximumPendingKeys / 2 + 1
              && released.back() == incoming,
          "Overflow drops queued presses and cleans up queued releases plus the incoming release"
      );
      for (std::size_t i = 0; i < decltype(keys)::maximumPendingKeys; ++i)
        keys.defer(static_cast<int>(i), true, 1, deliver);
      keys.defer(incoming, true, 1, deliver);
      keys.flush([] { return app::InputMethodKeyContext{.focused = true, .generation = 1}; }, deliver);
      require(presses == 0, "A press that overflows the queue cannot activate later");
    }
    {
      app::InputMethodKeys<Key> keys;
      app::InputMethodKeyContext context{.focused = true, .generation = 9};
      std::vector<Key::Symbol> delivered;
      int depth = 0, maximumDepth = 0;
      bool held = true;
      const auto current = [&] { return context; };
      const auto deliver = [&](const Key& event, bool pressed) {
        maximumDepth = std::max(maximumDepth, ++depth);
        delivered.push_back(event.symbol);
        if (pressed && event.symbol == Key::Enter) {
          ++context.generation;
          keys.flush(current, [&](const Key&, bool) { throw std::runtime_error("Nested delivery is forbidden"); });
        }
        if (!pressed && event.symbol == Key::PushToTalk)
          held = false;
        --depth;
      };
      keys.defer({Key::Enter, {}}, true, context.generation, deliver);
      keys.defer({Key::Letter, "x"}, true, context.generation, deliver);
      keys.defer({Key::PushToTalk, {}}, false, context.generation, deliver);
      keys.flush(current, deliver);
      require(
          maximumDepth == 1 && !held && delivered == std::vector<Key::Symbol>{Key::Enter, Key::PushToTalk},
          "Each queued item observes focus changes caused by the previous action without reentrant dispatch"
      );
    }
    {
      app::InputMethodKeys<Key> keys;
      app::InputMethodKeyContext context{.focused = true, .generation = 12};
      std::vector<Key::Symbol> delivered;
      const auto deliver = [&](const Key& event, bool pressed) {
        delivered.push_back(event.symbol);
        if (pressed) {
          context.pending = true;
          context.focused = false;
          keys.cancel([&](const Key&, bool) { throw std::runtime_error("Cancel must wait for the active delivery"); });
        }
      };
      keys.defer({Key::Enter, {}}, true, context.generation, deliver);
      keys.defer({Key::Letter, "x"}, true, context.generation, deliver);
      keys.defer({Key::PushToTalk, {}}, false, context.generation, deliver);
      keys.flush([&] { return context; }, deliver);
      require(
          delivered == std::vector<Key::Symbol>{Key::Enter, Key::PushToTalk},
          "Reentrant blur drops remaining presses and releases held sources despite a newly pending batch"
      );
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

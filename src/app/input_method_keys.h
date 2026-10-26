#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

namespace app {
  struct InputMethodKeyContext {
    bool pending = false;
    bool focused = false;
    std::uint64_t generation = 0;
  };

  template <class Event> class InputMethodKeys {
  public:
    static constexpr std::size_t maximumPendingKeys = 64;

    template <class Deliver> void defer(Event event, bool pressed, std::uint64_t generation, Deliver&& deliver) {
      if (m_keys.size() >= maximumPendingKeys) {
        cancel(deliver);
        if (!pressed) {
          if (m_delivering)
            deliver(event, false);
          else {
            m_keys.push_back({std::move(event), false, generation});
            cancel(deliver);
          }
        }
        return;
      }
      m_keys.push_back({std::move(event), pressed, generation});
    }

    template <class Context, class Deliver> void flush(Context&& context, Deliver&& deliver) {
      if (m_delivering)
        return;
      m_delivering = true;
      const DeliveryGuard guard{m_delivering};
      while (!m_keys.empty()) {
        const auto current = context();
        if (current.pending && !m_cancelling)
          break;
        auto next = std::move(m_keys.front());
        m_keys.pop_front();
        if (!next.pressed || (!m_cancelling && current.focused && next.generation == current.generation))
          deliver(next.event, next.pressed);
      }
      if (m_keys.empty())
        m_cancelling = false;
    }

    template <class Deliver> void cancel(Deliver&& deliver) {
      m_cancelling = true;
      flush([] { return InputMethodKeyContext{}; }, deliver);
    }

  private:
    struct PendingKey {
      Event event;
      bool pressed;
      std::uint64_t generation;
    };
    struct DeliveryGuard {
      bool& delivering;
      ~DeliveryGuard() { delivering = false; }
    };
    std::deque<PendingKey> m_keys;
    bool m_delivering = false;
    bool m_cancelling = false;
  };
} // namespace app

#include "media/video_player.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <poll.h>
#include <stdexcept>
#include <thread>

namespace {
  void require(bool value, const char* text) {
    if (!value)
      throw std::runtime_error(text);
  }
  template <class Predicate> void wait(media::VideoPlayer& player, Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
      player.drain();
      if (ready(player.state()))
        return;
      if (!player.state().error.empty())
        throw std::runtime_error(player.state().error);
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      require(remaining > 0, "Audio state must arrive through its wake descriptor");
      pollfd descriptor{player.eventFd(), POLLIN, 0};
      (void)::poll(&descriptor, 1, static_cast<int>(remaining));
    }
  }
} // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 2, "An isolated audio fixture is required");
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    require(!bytes.empty() && bytes.size() < 512 * 1024, "Audio fixture is bounded");
    media::VideoPlayer player;
    require(player.openAudio(media::memoryVideoInput(bytes), true), "Audio opens without a graphics context");
    wait(player, [](const auto& state) { return !state.loading && state.hasAudio; });
    require(
        !player.state().playing
            && player.state().width == 0
            && player.state().height == 0
            && player.state().volume == 25,
        "Loading an audio preview remains paused and creates no video frame"
    );
    require(player.state().duration > 0.5 && player.state().duration < 3, "Audio metadata is reported");
    player.volume(35);
    player.mute(true);
    wait(player, [](const auto& state) { return state.volume == 35 && state.muted; });
    player.mute(false);
    player.pause(false);
    wait(player, [](const auto& state) { return state.playing && state.position >= 0.08; });
    player.pause(true);
    wait(player, [](const auto& state) { return !state.playing; });
    const auto paused = player.state().position;
    pollfd descriptor{player.eventFd(), POLLIN, 0};
    (void)::poll(&descriptor, 1, 150);
    player.drain();
    require(player.state().position <= paused + 0.06, "A paused sound does not continue playing");
    player.seek(0);
    player.pause(false);
    wait(player, [](const auto& state) { return state.ended; });
    require(!player.state().playing, "An ended sound stops playback");
    player.close();
    require(
        !player.state().playing && !player.state().loading && !player.state().hasAudio,
        "Closing a preview clears playback state"
    );
    for (double volume : {0.0, 17.0, 100.0}) {
      require(player.openSound(media::memoryVideoInput(bytes), volume, true), "One-shot sounds open without graphics");
      require(player.state().volume == volume && !player.state().playing, "One-shot gain is set before playback");
      wait(player, [](const auto& state) { return !state.loading && state.hasAudio; });
      require(player.state().volume == volume, "Loading never restores a louder gain");
      const auto start = std::chrono::steady_clock::now();
      const auto duration = player.state().duration;
      player.pause(false);
      wait(player, [](const auto& state) { return state.ended; });
      require(
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= duration - 0.1,
          "One-shot completion waits for normal audio output drain"
      );
      require(!player.state().playing && player.state().error.empty(), "One-shot EOF completes cleanly");
      player.close();
    }
    const std::string playlist = "#EXTM3U\nhttps://example.invalid/never-open-this\n";
    require(
        player.openAudio(
            media::memoryVideoInput({reinterpret_cast<const std::uint8_t*>(playlist.data()), playlist.size()}), true
        ),
        "Invalid input is checked asynchronously"
    );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (player.state().error.empty() && std::chrono::steady_clock::now() < deadline) {
      pollfd event{player.eventFd(), POLLIN, 0};
      (void)::poll(&event, 1, 100);
      player.drain();
    }
    require(!player.state().error.empty(), "Audio-only previews reject playlists and secondary resources");
    std::cout << "Native audio-only preview checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

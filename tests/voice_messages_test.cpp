#include "api/client.h"
#include "api/uploads.h"
#include "api/video_source.h"
#include "app/channel_access.h"
#include "app/message_layout.h"
#include "app/notification_preview.h"
#include "app/reply_preview.h"

#include <array>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }
} // namespace

int main() {
  using namespace api;
  using nlohmann::json;
  try {
    std::array<char, 27> oggHeader{};
    std::copy_n("OggS", 4, oggHeader.begin());
    require(
        mediaSourceHeader(oggHeader, MediaSourceKind::OggAudio)
            && !mediaSourceHeader(oggHeader, MediaSourceKind::Video),
        "The CDN source accepts Ogg audio without broadening video container validation"
    );
    require(
        !mediaSourceHeader(std::span(oggHeader).first(4), MediaSourceKind::OggAudio),
        "Truncated Ogg headers are rejected"
    );
    oggHeader[4] = 1;
    require(!mediaSourceHeader(oggHeader, MediaSourceKind::OggAudio), "Unknown Ogg versions are rejected");
    oggHeader[4] = 0;
    oggHeader[0] = 'X';
    require(!mediaSourceHeader(oggHeader, MediaSourceKind::OggAudio), "Non-Ogg audio sources remain rejected");
    const std::string mp4Header{"\0\0\0\0ftypisom", 12};
    require(
        mediaSourceHeader(mp4Header, MediaSourceKind::Video)
            && !mediaSourceHeader(mp4Header, MediaSourceKind::OggAudio),
        "Video sources retain their own container policy"
    );
    const VoiceMessageMetadata metadata{12.34, {0, 1, 128, 255}};
    const auto fields = voiceMessageMetadataBody(metadata);
    require(
        fields["waveform"] == "AAGA/w==" && fields["duration_secs"] == 12.34,
        "Attachment metadata uses base64 bytes and fractional seconds"
    );
    require(parseVoiceMessageMetadata(fields) == metadata, "Waveform metadata roundtrips exactly");
    auto invalid = fields;
    invalid["waveform"] = "!!!!!!!!";
    require(!parseVoiceMessageMetadata(invalid), "Invalid base64 is not silently accepted");
    invalid = fields;
    invalid["duration_secs"] = -1;
    require(!parseVoiceMessageMetadata(invalid), "Negative durations remain unavailable");
    invalid["duration_secs"] = voiceMessageMaximumSeconds + 1;
    require(!parseVoiceMessageMetadata(invalid), "Duration is bounded");
    invalid = fields;
    invalid.erase("waveform");
    require(!parseVoiceMessageMetadata(invalid), "Missing metadata is not invented");
    auto tooMany = metadata;
    tooMany.waveform.resize(voiceMessageMaximumWaveform + 1);
    require(!validVoiceMessageMetadata(tooMany), "Waveforms have a 256 byte bound");
    tooMany = metadata;
    tooMany.duration = std::numeric_limits<double>::quiet_NaN();
    require(!validVoiceMessageMetadata(tooMany), "Nonfinite duration is invalid");

    Upload upload{"voice-message.ogg", "audio/ogg", {'O', 'g', 'g', 'S', 0}, metadata};
    require(validUploads({upload}), "Typed Ogg voice uploads are supported");
    auto untyped = upload;
    untyped.voice.reset();
    require(!validUploads({untyped}), "Ordinary audio file uploads stay unavailable");
    require(!validUploads({upload, upload}), "Voice messages require exactly one attachment");
    auto bounded = upload;
    bounded.bytes.resize(maxUploadBytes);
    require(validUploads({bounded}), "The exact 20 MiB recording allowance is accepted");
    bounded.bytes.push_back(0);
    require(!validUploads({bounded}), "Recordings above 20 MiB are rejected before enqueueing");
    auto maximumMetadata = metadata;
    maximumMetadata.duration = voiceMessageMaximumSeconds;
    maximumMetadata.waveform.resize(voiceMessageMaximumWaveform);
    require(validVoiceMessageMetadata(maximumMetadata), "The exact duration and waveform bounds are accepted");
    auto wrong = upload;
    wrong.mime = "image/png";
    require(!validUploads({wrong}), "Voice metadata cannot masquerade as an image");
    const auto body = voiceMessageBody(upload, "1234");
    require(
        body["flags"] == messageVoice && body["nonce"] == "1234" && body["enforce_nonce"] == true,
        "Voice messages retain nonce confirmation and the dedicated flag"
    );
    require(
        !body.contains("content")
            && !body.contains("message_reference")
            && !body.contains("sticker_ids")
            && body["attachments"].size() == 1
            && body["attachments"][0]["waveform"] == fields["waveform"]
            && body["attachments"][0]["filename"] == upload.filename,
        "Voice payloads contain one metadata-bearing attachment and no incompatible message fields"
    );
    auto attachment = fields;
    attachment.update(
        {{"id", "3"},
         {"filename", upload.filename},
         {"content_type", upload.mime},
         {"url", "https://cdn.discordapp.com/attachments/1/2/voice-message.ogg"}}
    );
    const auto messages = parseMessages(
        json::array(
            {{{"id", "2"},
              {"channel_id", "1"},
              {"flags", messageVoice},
              {"content", ""},
              {"attachments", json::array({attachment})},
              {"author", {{"id", "4"}, {"username", "Speaker"}}}}}
        )
    );
    require(
        messages.size() == 1
            && messages.front().attachments.front().voice == metadata
            && messages.front().content.empty(),
        "Received voice metadata survives message parsing without a placeholder"
    );
    auto message = messages.front();
    const auto reference = app::resolvedReply(MessageReference{"2", "1"}, &message);
    require(reference.content == "Voice message", "Replies describe recordings without a filename");
    auto patch = parseMessagePatch({{"id", "2"}, {"channel_id", "1"}, {"content", ""}});
    patch.apply(message);
    require(
        message.attachments.front().voice == metadata && (message.flags & messageVoice),
        "Partial message updates retain attachment metadata and voice flags"
    );
    const auto measure = [](std::string_view text, const app::MessageStyle&) { return text.size() * 7.0F; };
    const auto microphone = app::layout::Composer::actionFrame(400, 0, 3);
    const auto sticker = app::layout::Composer::actionFrame(400, 0, 2);
    require(
        microphone.x + microphone.width + Style::spaceXs == sticker.x,
        "The right-side microphone and expression actions retain a shared measured gap"
    );
    for (const float width : {140.0F, 300.0F, 700.0F}) {
      const auto layout = app::layoutRichMessage(message, width, measure);
      require(
          layout.voice && layout.pictures.empty() && layout.boxes.empty() && layout.runs.empty(),
          "Voice recordings use a dedicated card, not a generic attachment link"
      );
      require(
          layout.voice->card.width <= width
              && layout.voice->waveform.width >= 0
              && layout.voice->card.y == app::layout::VoiceMessage::rowGap / 2
              && layout.height == layout.voice->card.height + app::layout::VoiceMessage::rowGap,
          "Voice card measurement remains bounded and reserves a gap for grouped recordings"
      );
      const auto recorder = app::layout::voiceRecorderFrames({0, 0, width + 100, app::layout::VoiceRecorder::height});
      require(
          app::layout::VoiceRecorder::height == app::layout::Composer::minimumHeight
              && recorder.discard.y >= 0
              && recorder.discard.y + recorder.discard.height <= app::layout::Composer::minimumHeight
              && recorder.discard.x + recorder.discard.width <= recorder.status.x
              && recorder.status.x + recorder.status.width <= recorder.record.x
              && recorder.send.x + recorder.send.width <= width + 100,
          "Inline recorder controls remain separate and bounded"
      );
      require(
          layout.voice->play.x + layout.voice->play.width <= layout.voice->waveform.x
              && layout.voice->time.x + layout.voice->time.width <= layout.voice->speed.x
              && layout.voice->volume.x + layout.voice->volume.width <= layout.voice->card.width,
          "Compact playback controls stay inside their pill without overlap"
      );
      require(
          layout.voice->card.width <= app::layout::VoiceMessage::width
              && layout.voice->waveform.width <= app::layout::VoiceMessage::waveformWidth
              && layout.voice->time.width <= app::layout::VoiceMessage::timeWidth,
          "The pill caps waveform and timestamp space instead of expanding to the conversation width"
      );
    }
    require(
        app::desktopMessagePreview(message, {}) == "Sent a voice message",
        "Notifications identify voice recordings without exposing filenames"
    );
    const Channel text{.id = "1"};
    const auto grant = app::resolveChannelAccess(text, viewChannel | permission::sendVoiceMessages);
    require(
        app::can::sendVoiceMessage(grant) && !app::can::attachFiles(grant),
        "Voice sending uses its own permission, independently of image attachments"
    );
    require(
        !app::can::sendVoiceMessage(app::resolveChannelAccess(text, viewChannel))
            && !app::can::sendVoiceMessage(app::resolveChannelAccess(text, std::nullopt)),
        "Denied and unknown voice permissions never grant access"
    );
    Client client;
    require(
        !client.send("1", "caption", {upload}, {}, {}, "1234")
            && !client.send("1", "", {upload}, MessageReference{"2", "1"}, {}, "1234")
            && !client.send("1", "", {upload}, {}, {"3"}, "1234"),
        "Incompatible content, replies and stickers are rejected before enqueueing"
    );
    std::cout << "Voice message protocol and layout checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

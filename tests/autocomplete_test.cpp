#include "app/autocomplete.h"
#include "app/composer_tokens.h"
#include "ui/text_presentation.h"

#include <stdexcept>

int main() {
  const auto require = [](bool ok) {
    if (!ok)
      throw std::runtime_error("Autocomplete token boundary regression");
  };
  const std::string source = "🙂 <@123> <@!234> <@&345> <#456> <:kek:567> <a:wave:678>";
  const auto tokens = app::composerTokens(source);
  require(
      tokens.size() == 6
      && tokens[0].id == "123"
      && tokens[2].kind == app::ComposerToken::Role
      && tokens[3].kind == app::ComposerToken::Channel
      && tokens[4].name == "kek"
      && tokens[5].id == "678"
  );
  require(app::composerTokens("`<@123>` ```cpp\n<:kek:567>\n``` \\<@123> <@> <:bad name:567>").empty());
  const TextPresentation presentation("🙂 <@123> tail", {{5, 11, "@名字"}});
  require(presentation.text() == "🙂 @名字 tail" && presentation.displayOffset(11) == std::string("🙂 @名字").size());
  require(presentation.displayOffset(5) == presentation.displayOffset(8));
  const auto query = app::completionQuery("Hello @Maya tail", 11, 11);
  require(query && query->trigger == '@' && query->begin == 6 && query->end == 11 && query->text == "Maya");
  require(app::completionQuery("(@", 2, 2)->text.empty());
  const auto emoji = app::completionQuery("🙂 :smil", 10, 10);
  require(emoji && emoji->begin == 5 && emoji->text == "smil");
  for (const auto text :
       {"mail@example", "https://site/:smile", "`@Maya", "```nix\n:smile", "<@123>", "<:smile:123>",
        ":smile:", "\\@Maya"}) {
    const std::string value(text);
    require(!app::completionQuery(value, value.size(), value.size()));
  }
  require(!app::completionQuery("@Maya", 5, 0));
  require(!app::completionQuery("x", 10, 10));
  require(app::completionQuery("/", 1, 1)->trigger == '/');
  require(app::completionQuery("/logging moderation", 19, 19)->text == "logging moderation");
  for (const std::string value : {"hello /echo", "\\/echo", "`/echo", "/https://example.com", "/echo\ntext"})
    require(!app::completionQuery(value, value.size(), value.size()));
  const std::string closed = "`literal` :smile";
  require(app::completionQuery(closed, closed.size(), closed.size())->trigger == ':');
  const std::string unicode = "@名前";
  require(app::completionQuery(unicode, unicode.size(), unicode.size())->text == "名前");
}

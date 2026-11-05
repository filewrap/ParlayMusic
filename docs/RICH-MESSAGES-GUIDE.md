# Rich Messages with TDLib — a complete newbie guide

This guide explains how Parlay sends Rich Messages, and how to send one
yourself, assuming you have never used TDLib. Everything here is
**MTProto/TDLib only**. There is no Bot API in Parlay — no Bot API calls,
no Bot API URLs, no Bot API types — and no richgram. If you have read a
Bot API tutorial about rich messages, forget its method names: the
*feature* is the same, the *transport* is different. Here the transport
is TDLib, the library Parlay already links (version 1.8.69).

## 1. What a Rich Message is

A normal Telegram message is flat text plus formatting: "hello **world**".
A Rich Message is a **document made of blocks**:

```
┌─────────────────────────────────────────┐
│  Now Playing                      (h2)  │
│  ▫ Track: Mehrama        (paragraph)    │
│  ▫ Duration: 4:14        (paragraph)    │
│  ─────────────────────   (divider)      │
│  │ 1. Song A                         │  │
│  │ 2. Song B              (list)     │  │
│  [⏸ Pause]  [⏭ Skip]   (button row)   │
└─────────────────────────────────────────┘
```

Each block has a type (heading, paragraph, list, table, quote,
collapsible, photo, button row, …). The server parses the document into
a block tree, and Telegram clients render that tree. That is why a rich
message can have real headings, real tables and real collapsibles,
while a flat message can only have bold/italic text.

Two facts that explain most of Parlay's history with this feature:

- **Rich HTML is not classic HTML.** The `<b>`/`<i>` HTML Parlay used
  to send went through TDLib's *classic* parser (`parseTextEntities`),
  whose allowlist is exactly `a, b, strong, i, em, s, strike, del, u,
  ins, tg-spoiler, tg-emoji, tg-time, span, pre, code, blockquote`.
  There is no `h1`/`h2` in it — that is why every message containing
  `<h1>` or `<h2>` failed with `400 "Unsupported start tag"` and the
  user saw nothing. In **Rich** HTML, `<h1>`–`<h6>` are legal headings.
- **TDLib does not parse Rich HTML locally.** When you send a Rich HTML
  source, TDLib stores the text verbatim and hands it to the Telegram
  server, which parses it. So a bad tag is a *server* error — which is
  why Parlay's engine validates every tag *before* sending (see §7).

## 2. The TDLib types (the only ones you need)

All of these live in TDLib's schema file
(`phase4/tdlib-src/td/generate/scheme/td_api.tl`). The names below are
the exact constructor names — copy them verbatim, including case.

| Type | What it is |
|---|---|
| `inputMessageRichMessage` | The message content: `message:inputRichMessage clear_draft:Bool`. This is what goes into `sendMessage` instead of `inputMessageText`. |
| `inputRichMessage` | The document: `source:RichMessageSource is_rtl:Bool detect_automatic_blocks:Bool`. |
| `richMessageSourceHtml` | A document written as Rich HTML: `text:string media:vector<inputRichMessageMedia>`. **For bots only.** |
| `richMessageSourceBlocks` | A document written as an explicit block list: `blocks:vector<InputPageBlock>`. |
| `richMessageSourceMarkdown` | A document written as Markdown. **For bots only.** Parlay does not use it (HTML covers everything Markdown does, plus more). |
| `inputRichMessageMedia` | One media element for the HTML source: `id:string` (1–64 base64url chars, your own key) + `media:InputMessageContent`. |
| `inputPageBlock*` | One block. 24 constructors (see §5). |
| `RichText` | One formatted run of text: plain, bold, italic, URL, mention, … (see §5). |
| `inlineButton` | One in-message button: `text:RichText style:ButtonStyle type:InlineKeyboardButtonType`. |
| `messageRichMessage` | What you get *back*: a received rich message (`message:richMessage`). |

`sendMessage` itself is unchanged: `chat_id`, `reply_to`,
`reply_markup`, `input_message_content`. The only thing that changes
is the content object. That is why the existing inline keyboards
(`reply_markup`) keep working untouched — they are a separate field.

## 3. Quick start: import, build, send

The engine is one header: `services/core-cpp/src/rich_message.hpp`
(namespace `parlay::rich`). No setup, no init, no state. Build a
message, hand it to the sender.

```cpp
#include "rich_message.hpp"

// 1. Build — HTML source. This is the same HTML the UI texts already
//    use; <h1>/<h2> are legal here.
parlay::rich::RichMessage msg =
    parlay::rich::RichMessage::fromHtml(
        "<h1>Parlay</h1>\n\n<b>Hi.</b> I stream music in voice chats.");

// 2. Send — through the sender (TdLibTelegramSender). reply_markup
//    is the usual inline keyboard, or Json(nullptr) for none.
std::string err;
int64_t id = tg.sendRichMessage(chat_id, msg,
                                TdLibTelegramSender::inlineMarkup(kb),
                                /*reply_to=*/0, &err);
if (id < 0) {
    // err now holds the exact failure, e.g.
    // "rich HTML: unsupported tag <song>" or "400: ...".
    // Never ignore it: reportOwnerError(action, chat_id, err).
}
```

The Blocks source, same send call:

```cpp
using namespace parlay::rich;
RichMessage msg = RichMessage::fromBlocks();
std::string err;
msg.heading(2, rtPlain("Now Playing"), &err);
msg.paragraph(rtTexts({rtBold(rtPlain("▫ Track: ")),
                       rtUrl(rtPlain("Mehrama"),
                             "https://youtu.be/example")}),
              &err);
msg.buttonRow({RichButton::callback("⏸ Pause", "pl:vc:pause", "primary"),
               RichButton::copy("Copy link", "https://youtu.be/example")},
              "center", &err);
int64_t id = tg.sendRichMessage(chat_id, msg,
                                TdLibTelegramSender::inlineMarkup(kb),
                                0, &err);
```

Every builder method returns `bool` and takes a `std::string*
error_out`: `false` means "not built, here is exactly why". Check it.
`buildContent()` (called by the sender) re-validates the whole
message before anything goes on the wire.

## 4. The HTML source in detail

### Allowed tags

Only these tags exist in Rich HTML. Anything else is rejected by the
engine *before* sending, with the tag named in the error.

- Inline: `<b>` `<strong>` `<i>` `<em>` `<u>` `<ins>` `<s>`
  `<strike>` `<del>` `<code>` `<mark>` `<sub>` `<sup>`
  `<tg-spoiler>` `<a href="…">` `<tg-reference>` `<tg-emoji>`
  `<tg-time>` `<tg-math>`
- Headings: `<h1>` `<h2>` `<h3>` `<h4>` `<h5>` `<h6>`
- Blocks: `<p>` `<pre>` `<footer>` `<hr/>` `<ul>` `<ol>` `<li>`
  `<blockquote>` (+ `expandable`) `<aside>` (pull quote)
  `<table>` `<tr>` `<th>` `<td>` `<caption>` `<details>` `<summary>`
  `<tg-math-block>`
- Media: `<img>` `<video>` `<audio>` `<tg-document>` `<figure>`
  `<figcaption>` `<cite>` `<tg-map>` `<tg-collage>` `<tg-slideshow>`
- Buttons: `<tg-button>` `<tg-button-row>` (see §6 caveat)
- Line break: `<br/>` (used inside block quotes)

Notably absent (and rejected): `<span>`, `<div>`, `<script>`,
`<style>`, `<section>`, `<header>`. A `<` that does not start a tag
("a < b", "fish & chips") is literal text, not an error. A `<word>`
that *looks* like a tag but is not one — for example the `/seek
<seconds>` placeholder in help text — **is** an error, which is why
help text escapes it as `&lt;seconds&gt;` (see §9).

### Allowed entities

Named: `&lt;` `&gt;` `&amp;` `&quot;` `&apos;` `&nbsp;` `&hellip;`
`&mdash;` `&ndash;` `&lsquo;` `&rsquo;` `&ldquo;` `&rdquo;`. All
numerical entities (`&#169;`, `&#x1F600;`) are accepted. Any other
named entity (`&copy;`, `&bogus;`) is rejected before sending.

### Media in HTML

Media are declared separately and referenced by id. The reference
form (`tg://photo?id=<id>` etc.) is a **server-side convention**:
it is documented for the rich HTML dialect, but there is no parser
for it in this TDLib tree, so it could not be verified from TDLib's
code — only from the documented dialect. If a reference ever stops
working, the Blocks source (§5) is the fully verified path.

```cpp
RichMessage msg = RichMessage::fromHtml(
    "<b>Cover</b> <img src=\"tg://photo?id=cover1\"/>");
msg.addMedia("cover1",
             rich::mediaPhotoContent("/data/cover.jpg", false, &err),
             &err);
```

`mediaPhotoContent` (and `mediaVideoContent` / `mediaAudioContent` /
`mediaDocumentContent` / `mediaAnimationContent` /
`mediaVoiceNoteContent`) take a **local file path or a TDLib file
id**. An `http(s)://` URL is refused upfront: it is not a valid
`inputFile`, and TDLib would 400 on it. Download URL thumbnails to
disk first — the core has no HTTP client for that today, so URL
thumbnails fall back to the text card (see §8).

## 5. The Blocks source in detail

Use it when you need structure the HTML source cannot express
cleanly, or when you need the fully locally-verified path (every
block is validated by TDLib's own `WebPageBlock.cpp`).

### Blocks

| Builder | TDLib block | Notes |
|---|---|---|
| `heading(level, text)` | `inputPageBlockSectionHeading` | level 1–6, 1 largest. A bot's "title" is a heading, not a `Title` block (there is no sendable Title). |
| `paragraph(text)` | `inputPageBlockParagraph` | |
| `preformatted(text, language)` | `inputPageBlockPreformatted` | language may be empty |
| `footer(text)` | `inputPageBlockFooter` | |
| `divider()` | `inputPageBlockDivider` | |
| `mathBlock(expr)` | `inputPageBlockMathematicalExpression` | raw LaTeX, non-empty |
| `anchorBlock(name)` | `inputPageBlockAnchor` | link target for `#name` links |
| `list(items, ordered)` | `inputPageBlockList` | non-empty; entirely ordered or entirely unordered (TDLib rule) |
| `checkList(items)` | `inputPageBlockList` | (text, checked) pairs, checkboxes |
| `blockQuote(blocks, credit)` | `inputPageBlockBlockQuote` | blocks = nested blocks; credit may be null |
| `expandableQuote(text, credit)` | `inputPageBlockExpandableBlockQuote` | |
| `pullQuote(text, credit)` | `inputPageBlockPullQuote` | |
| `table(caption, rows, bordered, striped, compact)` | `inputPageBlockTable` | rows of `TableCell{text, is_header, colspan, rowspan, align, valign}`; a row's columns (colspan sum) ≤ 20 |
| `details(header, blocks, open)` | `inputPageBlockDetails` | collapsible; `open` = expanded by default |
| `photoBlock / videoBlock / audioBlock / documentBlock / voiceNoteBlock / animationBlock` | `inputPageBlockPhoto` etc. | file path or file id; caption + credit may be null; photo/video/animation take a spoiler flag |
| `collage(blocks, caption, credit)` | `inputPageBlockCollage` | media shown together, like a media group |
| `slideshow(blocks, caption, credit)` | `inputPageBlockSlideshow` | one media at a time, swipeable |
| `mapBlock(lat, lon, zoom, w, h, caption, credit)` | `inputPageBlockMap` | zoom 0–24; width+height ≤ 10000; side ratio ≤ 20 |
| `buttonRow(buttons, align)` | `inputPageBlockButtonRow` | see §6; align "" (full width) / "left" / "center" / "right" |

Not sendable (receive-only, mostly Instant View leftovers): Title,
Subtitle, AuthorDate, Header, Subheader, Kicker, Cover, Embedded,
EmbeddedPost, ChatLink, RelatedArticles. There is no builder for
them, deliberately.

### RichText (formatted runs)

`rtPlain("text")` is the leaf. Wrappers nest: `rtBold(rtPlain("x"))`,
`rtUrl(rtPlain("click"), "https://…")`. Several runs combine with
`rtTexts({…})`. Available: `rtBold`, `rtItalic`, `rtUnderline`,
`rtStrikethrough`, `rtSpoiler`, `rtSubscript`, `rtSuperscript`,
`rtMarked`, `rtFixed` (inline code), `rtUrl`, `rtMention`,
`rtMentionName`, `rtHashtag`, `rtBotCommand`, `rtEmail`, `rtPhone`,
`rtCustomEmoji`, `rtMath`, `rtReference`, `rtReferenceLink`,
`rtAnchor`, `rtAnchorLink`, `rtButton`. Plain text needs no escaping
— it is a JSON string, not HTML. (`richTextDateTime` is not wrapped:
its formatting-type constructors were not verified from the schema;
use `<tg-time>` in the HTML source instead.)

### The thinking block

`inputPageBlockThinking` exists in the schema but is for *pending*
(draft/streaming) rich messages only. Parlay sends finished messages,
so there is no builder for it.

## 6. Buttons

There are two button systems, and they coexist:

1. **Reply-markup keyboards** (`reply_markup` on `sendMessage`) —
   what Parlay has always used. `InlineKeyboard` values are built by
   `ui_texts.hpp` and converted by `TdLibTelegramSender::inlineMarkup`.
   Unchanged by the Rich rewrite: `sendMessageHtml` still takes the
   same `InlineKeyboard`, and `sendRichMessage` takes the same
   keyboard as prebuilt markup JSON. Callback data is 1–64 bytes
   (Parlay's `pl:…` callbacks fit); copy buttons
   (`InlineButton::copy`) are client-side copies and produce no
   callback query.
2. **In-message button rows** (`inputPageBlockButtonRow`, Blocks
   source only) — buttons rendered *inside* the document. Built with
   `RichButton::callback / ::link / ::copy / ::switchInline` plus a
   style (`default`, `primary`, `danger`, `success`, `link`).
   `link` style is callback-only (TDLib rule). Callback data travels
   as base64 (`bytes` in TDLib JSON) and is limited to 64 raw bytes —
   the engine rejects longer data before sending. A **copy pill** is
   `RichButton::copy("Copy link", "<text>")`: tapping it copies the
   text, no handler needed.

`<tg-button>` inside Rich HTML is a server-side convention like the
media references (§4): documented for the dialect, not verifiable
from this TDLib tree. The verified button path is the button row
above — use it for in-message buttons.

There is no web-app button and no user button in the engine: the
core's `InlineButton` type has no fields for them. If one is ever
needed, add the field to `InlineButton` (tg_types.hpp) first, then
to both converters (`tdButton` in dispatcher.cpp and
`inlineButtonJson` in rich_message.hpp).

## 7. Validation and limits

Before anything is sent, `buildContent()` enforces:

| Limit | Value | What counts |
|---|---|---|
| Text | 32,768 chars | HTML source: raw string length; Blocks: sum of all RichText |
| Blocks | 500 | blocks + list items + table rows |
| Nesting depth | 16 | quote/details/list/collage nesting |
| Media | 50 | media blocks (Blocks) / declared media (HTML) |
| Table columns | 20 | per row (colspan sum) |
| Callback data | 64 bytes | raw, before base64 |
| Media id | 1–64 base64url chars | unique per message |

These are TDLib's defaults (`OptionManager.cpp`); the server may
lower them. A violation fails *locally* with a precise message
("rich HTML: unsupported tag <song>", "rich message: table row has
24 columns, over the TDLib limit of 20") — never a silent drop, and
never a send TDLib will 400 on. The server is still the final
authority for the HTML source (it parses it): a server 400 comes
back through `error_out` like any other TDLib error.

## 8. How the core uses it (what goes rich, what does not)

| Send | Path | Why |
|---|---|---|
| `sendMessageHtml` (all HTML text + keyboards) | **Rich** (`inputMessageRichMessage`, HTML source) | This is the central seam: `replyHtml`, `replyKb`, every direct `sendMessageHtml` call site, DM `/start`, DM `/help`, the now-playing text card. |
| `sendMessageReplyKb` / `sendMessageRemoveKb` | **Rich** | Same HTML text, different markup. |
| `editMessageText` (card edits) | **Rich** | Cards are rich messages now; editing them with classic HTML would fail on their own `<h2>`. |
| `sendRichMessage` | **Rich** | Direct entry for built messages (now-playing photo card: photo block + heading + track lines + reply-markup keyboard). |
| `sendMessage` / `sendMessageId` (plain text) | Classic `inputMessageText`, no parse mode | Plain text has no tags to reject and no `<h1>` bug. Routing it through HTML parsing would *create* failures for messages containing `<`/`>` (e.g. error strings). |
| `sendPhoto` / `editMessageCaption` (photo captions) | Classic `formattedText` | TDLib captions **cannot** be Rich Messages. `<h1>..<h6>` in a caption are downgraded to `<b>` and retried once (logged); anything else that fails to parse is reported. Also fixed in this rewrite: `sendPhoto` used to pass a bare `InputFile` where the schema requires an `inputPhoto` object — it now builds the exact shape. |
| `sendDocument` / `sendFile` / `sendAudioFile` / `sendAudio` (file captions) | Classic `formattedText` | Same caption rule; these already fall back to a plain caption on parse failure. |
| `forwardMessage` / `copyMessage` / `deleteMessage` / `answerCallbackQuery` | Unchanged | No message text involved. |

The now-playing card tries, in order: (1) a Rich Message with the
thumbnail as a photo block (only when the thumbnail is a local file
or a TDLib file id — an `http(s)://` URL cannot be an `inputFile`),
(2) a classic photo send with the caption, (3) the Rich text card.
A rich photo-block card records as a *text* card for edits (it is a
rich message, not a photo message), so later button taps edit it via
`editMessageText`.

## 9. Errors: every failure reaches the owner

Nothing in the engine fails silently — that was the original bug
(`[tg] sendMessageHtml failed` on stderr, user sees nothing).

- **Validation failure** (bad tag, oversize, bad media id, callback
  too long): `buildContent()` returns null JSON + a precise string in
  `error_out`. The caller returns -1 with that string.
- **Send failure** (TDLib error, timeout): `sendWithContent`
  returns -1 + `TdLibClient::errorMessage(resp)` (e.g. `"400: …"`).
- **Owner reporting** (`Dispatcher::reportOwnerError`, dispatcher.cpp):
  every `reply*` helper and `sendNowPlayingCard` calls it on
  failure. It writes the failure to stderr *and* sends one Rich
  Message DM to the owner (`cfg_.owner_id`) with the action, the
  chat id, and the TDLib error. The DM works from any chat, including
  groups. A reentrancy guard (`notifying_owner_`) makes a failure to
  notify log-only — it can never trigger another notification, so
  there is no spam loop. No owner configured (`owner_id == 0`) means
  stderr only.
- **Not reported** (by design — too noisy): callback-query answers
  (a tap on an expired button fails constantly) and card edits
  ("message is not modified" on a no-op edit).

### Owner access

Owner id `8495104954` (the user). Precedence: built-in default
`8495104954` < `[telegram] owner_id` in `parlay.toml` < `OWNER_ID`
env var (env wins). `isOwner()` gates the owner-only commands
(`addsudo`, `delsudo`, `maintenance`, `eval`/`ev`/`sh`/`exec`);
the owner also counts as sudo, is exempt from the blacklist, and
counts as a group admin. DM and group access are the same gate —
there is no separate DM whitelist.

### Common mistakes

- **Writing classic HTML and calling it rich.** `<span>` is not in
  either list; `<h1>` is legal only in Rich HTML. If a message fails
  with `unsupported tag`, the tag is named in the error — read it.
- **Raw `<…>` placeholders in text.** `/seek <seconds>` must be
  written `/seek &lt;seconds&gt;` (the per-command help renderer
  already escapes them; user input substituted into a template must
  be `htmlEscape`d first — see `onHelp` / the unknown-command reply).
- **http(s) URLs as media.** `inputFile` takes a local path or a
  file id, never a URL. The engine refuses URLs upfront.
- **Callback data over 64 bytes.** Count raw bytes, not base64.
- **Mixing sources.** One message, one source: `fromHtml` *or*
  `fromBlocks`, never both. Block builders on an HTML message (and
  `addMedia` on a Blocks message) are rejected.
- **Expecting the server to validate for you.** It will — with a
  400 *after* the send. The engine exists so you find out before.

## 10. Exact JSON shapes (for debugging)

`sendMessage` with a Rich HTML source (what `sendMessageHtml`
produces):

```json
{"@type": "sendMessage", "chat_id": -1001234567890,
 "reply_markup": {"@type": "replyMarkupInlineKeyboard", "rows": [[
   {"text": "Pause", "type": {"@type": "inlineKeyboardButtonTypeCallback",
                              "data": "cGw6dmM6cGF1c2U="}}]]},
 "input_message_content": {"@type": "inputMessageRichMessage",
   "message": {"@type": "inputRichMessage",
     "source": {"@type": "richMessageSourceHtml",
                "text": "<h1>Parlay</h1>\n\n<b>Hi.</b>",
                "media": []},
     "is_rtl": false, "detect_automatic_blocks": true},
   "clear_draft": true}}
```

Blocks source (fragment — a heading, a paragraph, a button row):

```json
{"@type": "inputMessageRichMessage",
 "message": {"@type": "inputRichMessage",
   "source": {"@type": "richMessageSourceBlocks", "blocks": [
     {"@type": "inputPageBlockSectionHeading",
      "text": {"@type": "richTextPlain", "text": "Now Playing"},
      "size": 2},
     {"@type": "inputPageBlockParagraph",
      "text": {"@type": "richTexts", "texts": [
        {"@type": "richTextBold",
         "text": {"@type": "richTextPlain", "text": "▫ Track: "}},
        {"@type": "richTextUrl",
         "text": {"@type": "richTextPlain", "text": "Mehrama"},
         "url": "https://youtu.be/example", "is_cached": false}]}},
     {"@type": "inputPageBlockButtonRow", "align": null, "buttons": [
       {"@type": "inlineButton",
        "text": {"@type": "richTextPlain", "text": "⏸ Pause"},
        "style": {"@type": "buttonStylePrimary"},
        "type": {"@type": "inlineKeyboardButtonTypeCallback",
                 "data": "cGw6dmM6cGF1c2U="}}]}]},
   "is_rtl": false, "detect_automatic_blocks": true},
 "clear_draft": true}
```

Notes: `bytes` (callback data) are base64 in JSON. Nullable
RichText fields (credit, caption text) are sent as an empty
`richTextPlain`, never JSON null — TDLib treats an empty RichText as
absent. `align: null` on a button row means full-width buttons.

## 11. What is NOT implemented (and why)

- **Drafts / streaming** (`sendRichMessageDraft`, `<tg-thinking>`):
  private-chat-only, ephemeral (~30 s), nothing persists. Parlay
  sends finished messages everywhere; there is no streaming send
  path to build on.
- **`richTextDateTime`**: the schema's formatting-type constructors
  were not verified from this tree. Use `<tg-time>` in the HTML
  source.
- **Web-app and user buttons**: the core's `InlineButton` type has
  no fields for them (see §6).
- **Markdown source** (`richMessageSourceMarkdown`): valid TDLib,
  but HTML covers the same ground and the core's texts are already
  HTML. Nothing to gain, one more dialect to maintain.
- **Premium gating**: `premiumFeatureRichMessages` exists in the
  schema as a feature-catalog entry (`"rich_formatting"`), but no
  send-side premium check exists anywhere in TDLib's code, and the
  HTML/Markdown sources are documented "for bots only". Bots send
  rich messages without Premium. If the server ever gates it, the
  failure surfaces as a normal TDLib error and the owner is told.

## 12. Verifying a change

- Build the production target only:
  `cmake --build services/core-cpp/build --target parlay-core`.
  (A bare build also builds the test executables — do not do that.)
- The engine's validation can be exercised without Telegram: build a
  `RichMessage`, call `buildContent(&err)`, and inspect the JSON or
  the error string. That proves the *shape*; only a live send on the
  VPS proves the *rendering* (official clients still render rich
  messages unevenly — check on at least two apps before calling a
  card done).
- Live checklist after deploy: DM `/start` (h1 card), DM `/help`
  (h1 card), `/play` in a group (now-playing reply with h2), a
  `/queue` card, a settings panel (collapsible/table), and one
  failing send (to see the owner DM arrive).

## Sources

- TDLib schema + implementation, in-repo:
  `phase4/tdlib-src/td/generate/scheme/td_api.tl` (L128–154,
  L6577–6670, L6683), `td/telegram/RichMessage.cpp`,
  `td/telegram/RichMessageMedia.cpp`, `td/telegram/WebPageBlock.cpp`
  (block validation, empty-RichText → nullptr),
  `td/telegram/MessageContent.cpp`,
  `td/telegram/OptionManager.cpp:219-223` (limits),
  `td/generate/auto/td/telegram/td_api_json_1.cpp` (inputMessagePhoto
  parsing), `td/generate/auto/td/telegram/td_api_json_2.cpp`
  (inputPhoto parsing).
- The verified report this engine was built from:
  `~/workspace/research_notes/tdlib-rich-messages-20261105/report.md`.
- TDLib issue #3689 (maintainer answer: pageBlocks render like
  Instant View; Markdown source works "even from a user account";
  official apps still have rendering issues — check at least 2 apps).

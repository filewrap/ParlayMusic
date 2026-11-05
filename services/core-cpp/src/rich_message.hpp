// rich_message.hpp — TDLib Rich Messages engine (MTProto/TDLib only).
//
// A Rich Message is a document of blocks (headings, paragraphs, lists,
// tables, quotes, collapsibles, media, button rows) — not flat text
// with entities. TDLib 1.8.69 (the version this core links) sends one
// as sendMessage + inputMessageRichMessage. This engine is the only
// place that builds that object.
//
//   Just import, Build, Send:
//     #include "rich_message.hpp"
//     parlay::rich::RichMessage m =
//         parlay::rich::RichMessage::fromHtml("<h1>Parlay</h1>\n\n<b>Hi</b>");
//     std::string err;
//     int64_t id = tg.sendRichMessage(chat_id, m, kb, 0, &err);
//       -- TdLibTelegramSender::sendRichMessage (dispatcher.cpp). It
//          validates through RichMessage::buildContent and sends with
//          the existing sendWithContent seam, so reply_markup
//          keyboards keep working unchanged.
//
// Two sources, exactly one per message (TDLib rule):
//   - Rich HTML: fromHtml("<h1>…</h1>…"). The HTML is stored verbatim
//     and parsed by the Telegram server — TDLib does NOT parse it
//     locally. The engine validates every tag against the documented
//     Rich HTML list before sending, so a bad tag fails locally with
//     a precise error instead of a server 400.
//   - Blocks: fromBlocks() + heading()/paragraph()/table()/… — each
//     block is built as its exact inputPageBlock JSON object and
//     validated locally by TDLib (WebPageBlock.cpp).
//
// Validation is strict and local: text ≤ 32,768 chars, blocks/list
// items/table rows ≤ 500, nesting depth ≤ 16, media ≤ 50, table
// columns ≤ 20 (TDLib defaults; the server may lower them). A
// validation failure returns a structured RichError through
// error_out — never a silent drop, never a send TDLib will 400 on.
//
// HARD RULES (user orders, do not relax):
//   - MTProto/TDLib only. No Bot API anywhere in this file: no Bot
//     API calls, URLs, or types. No richgram.
//   - No stubs, no mocks, no TODOs: every builder emits its real
//     TDLib object. Features TDLib cannot send are left out and
//     listed in docs/RICH-MESSAGES-GUIDE.md, not faked.
//   - Nullable RichText fields (credit, caption text) are emitted as
//     an empty richTextPlain, never JSON null: TDLib treats an empty
//     RichText as absent (WebPageBlock.cpp: empty -> nullptr).
//
// Unverified-from-this-tree items (server-side conventions, see the
// guide): how richMessageSourceHtml references a media id
// (documented as tg://photo?id=<id> etc.; the media array declares
// the id), and <tg-button> inside HTML (the verified button path is
// inputPageBlockButtonRow in the Blocks source).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"
#include "scram.hpp"
#include "tg_types.hpp"

namespace parlay::rich {

// --- TDLib limits (td/telegram/OptionManager.cpp defaults) ----------
// The server may lower these via config; the engine enforces the
// defaults so an oversize message fails locally with a precise
// error instead of a server 400.
inline constexpr int kMaxTextChars = 32768;
inline constexpr int kMaxBlocks = 500;
inline constexpr int kMaxDepth = 16;
inline constexpr int kMaxMedia = 50;
inline constexpr int kMaxTableColumns = 20;
// inlineKeyboardButtonTypeCallback data is TDLib `bytes`, sent as
// base64; the raw payload limit is 64 bytes.
inline constexpr std::size_t kMaxCallbackBytes = 64;
// inputRichMessageMedia.id: 1-64 base64url characters.
inline constexpr std::size_t kMaxMediaIdChars = 64;

// --- Structured errors ------------------------------------------------
// Every engine failure is one of these: a TDLib-style code, a
// human-readable message, the chat it was for, and the action that
// was being attempted. Callers log it and report it to the owner;
// nothing in the engine ever fails silently.
struct RichError {
    int code = 0;  // 400 = local validation, otherwise the TDLib code
    std::string message;
    int64_t chat_id = 0;
    std::string action;
    std::string toString() const {
        std::string s = std::to_string(code) + ": " + message;
        if (!action.empty()) s += " (action " + action + ")";
        if (chat_id != 0) s += " (chat " + std::to_string(chat_id) + ")";
        return s;
    }
};

// --- Small helpers ------------------------------------------------------
inline std::size_t utf8CodePoints(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;  // skip UTF-8 continuation bytes
    return n;
}

inline bool isBase64UrlChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

inline std::string lowerAscii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// --- Rich HTML validation ----------------------------------------------
// The documented Rich HTML tag list (Bot API "Rich HTML style" — the
// same server-side dialect TDLib's richMessageSourceHtml is parsed
// with). Anything else is rejected before sending. NOTE: this list
// is deliberately NOT the classic-HTML allowlist (MessageEntity.cpp:
// a,b,strong,i,em,s,strike,del,u,ins,tg-spoiler,tg-emoji,tg-time,
// span,pre,code,blockquote — no h1/h2). Headings ARE legal here.
inline bool isRichHtmlTag(const std::string& name) {
    static const std::vector<std::string> kTags = {
        "a", "aside", "audio", "b", "blockquote", "br", "caption", "cite",
        "code", "del", "details", "em", "figcaption", "figure", "footer",
        "h1", "h2", "h3", "h4", "h5", "h6", "hr", "i", "img", "input",
        "ins", "li", "mark", "ol", "p", "pre", "s", "strike", "strong",
        "sub", "summary", "sup", "table", "td", "tg-button", "tg-button-row",
        "tg-collage", "tg-document", "tg-emoji", "tg-map", "tg-math",
        "tg-math-block", "tg-reference", "tg-slideshow", "tg-spoiler",
        "tg-time", "th", "tr", "u", "ul", "video",
    };
    return std::find(kTags.begin(), kTags.end(), name) != kTags.end();
}

inline bool isRichHtmlEntity(const std::string& name) {
    static const std::vector<std::string> kEntities = {
        "lt", "gt", "amp", "quot", "apos", "nbsp", "hellip", "mdash",
        "ndash", "lsquo", "rsquo", "ldquo", "rdquo",
    };
    return std::find(kEntities.begin(), kEntities.end(), name) !=
           kEntities.end();
}

// Validate a Rich HTML string before it is sent: every tag must be
// in the list above, every named entity must be in the list above
// (all numerical entities &#...; / &#x...; are accepted). Tags inside
// attribute values and raw '&' that do not start an entity are
// literal text, not errors. Returns false + error_out naming the
// exact offending tag/entity on failure.
inline bool validateRichHtml(const std::string& html,
                             std::string* error_out) {
    const std::size_t n = html.size();
    std::size_t i = 0;
    auto fail = [&](const std::string& msg) {
        if (error_out) *error_out = msg;
        return false;
    };
    while (i < n) {
        char c = html[i];
        if (c == '<') {
            std::size_t j = i + 1;
            if (j < n && (html[j] == '!' || html[j] == '?'))
                return fail("rich HTML: comments and declarations "
                            "(<!--, <!DOCTYPE, <?) are not supported");
            if (j < n && html[j] == '/') ++j;
            if (j < n && std::isalpha(static_cast<unsigned char>(html[j]))) {
                std::size_t name_begin = j;
                while (j < n && (std::isalnum(
                                     static_cast<unsigned char>(html[j])) ||
                                 html[j] == '-'))
                    ++j;
                std::string name =
                    lowerAscii(html.substr(name_begin, j - name_begin));
                if (!isRichHtmlTag(name))
                    return fail("rich HTML: unsupported tag <" + name +
                                "> (not in the Rich HTML tag list)");
                // Skip to the closing '>' honouring quoted attribute
                // values, so '<' / '>' / '&' inside an href/src do
                // not confuse the scanner.
                bool in_single = false, in_double = false;
                std::size_t k = j;
                while (k < n) {
                    char kc = html[k];
                    if (kc == '"' && !in_single)
                        in_double = !in_double;
                    else if (kc == '\'' && !in_double)
                        in_single = !in_single;
                    else if (kc == '>' && !in_single && !in_double)
                        break;
                    ++k;
                }
                if (k >= n)
                    return fail("rich HTML: unclosed tag <" + name + ">");
                i = k + 1;
                continue;
            }
            ++i;  // '<' followed by a non-letter: literal text.
            continue;
        }
        if (c == '&') {
            std::size_t j = i + 1;
            if (j < n && html[j] == '#') {
                ++j;
                if (j < n && (html[j] == 'x' || html[j] == 'X')) ++j;
                std::size_t digits = 0;
                while (j < n &&
                       std::isxdigit(static_cast<unsigned char>(html[j]))) {
                    ++j;
                    ++digits;
                }
                if (digits > 0 && j < n && html[j] == ';') {
                    i = j + 1;
                    continue;
                }
                if (j < n && html[j] == ';')
                    return fail("rich HTML: malformed numerical entity "
                                "near offset " +
                                std::to_string(i));
                // '&#' with no terminating ';' is literal text
                // ("R&#D" is not an entity attempt).
                ++i;
                continue;
            }
            if (j < n && std::isalpha(static_cast<unsigned char>(html[j]))) {
                std::size_t name_begin = j;
                while (j < n &&
                       std::isalpha(static_cast<unsigned char>(html[j])))
                    ++j;
                if (j < n && html[j] == ';') {
                    std::string name =
                        html.substr(name_begin, j - name_begin);
                    if (!isRichHtmlEntity(name))
                        return fail("rich HTML: unsupported entity &" +
                                    name + "; (only &lt; &gt; &amp; "
                                    "&quot; &apos; &nbsp; &hellip; &mdash; "
                                    "&ndash; &lsquo; &rsquo; &ldquo; &rdquo; "
                                    "and numerical entities are supported)");
                    i = j + 1;
                    continue;
                }
                // '&' + letters with no ';' is literal text ("R&D").
                ++i;
                continue;
            }
            ++i;  // bare '&' (e.g. "fish & chips"): literal text.
            continue;
        }
        ++i;
    }
    return true;
}

// Photo/audio/document captions are classic formattedText, not Rich
// Messages: TDLib's classic parser rejects <h1>..<h6>. When a
// caption built for a rich card is reused as a caption, downgrade
// headings to bold so the media still sends with its text readable.
// Tags with attributes are left untouched (they fail loudly instead
// of being silently mangled).
inline std::string downgradeHeadingsForClassic(const std::string& html) {
    // Classic captions (formattedText) reject <h1>..<h6> and <p>:
    // headings become bold, paragraph tags become line breaks, so a
    // Rich HTML card keeps its layout as a classic caption.
    std::string out;
    out.reserve(html.size());
    const std::size_t n = html.size();
    std::size_t i = 0;
    while (i < n) {
        if (html[i] == '<') {
            std::size_t j = i + 1;
            bool closing = false;
            if (j < n && html[j] == '/') {
                closing = true;
                ++j;
            }
            if (j + 2 < n && (html[j] == 'h' || html[j] == 'H') &&
                html[j + 1] >= '1' && html[j + 1] <= '6' &&
                html[j + 2] == '>') {
                out += closing ? "</b>" : "<b>";
                i = j + 3;
                continue;
            }
            if (j + 1 < n && (html[j] == 'p' || html[j] == 'P') &&
                html[j + 1] == '>') {
                out += closing ? "" : "\n";
                i = j + 2;
                continue;
            }
        }
        out += html[i];
        ++i;
    }
    return out;
}

// --- RichText -----------------------------------------------------------
// One formatted run. Wrappers nest through their "text" field;
// several runs combine with rtTexts. Plain text needs no escaping:
// it is a JSON string, not HTML.
inline Json rtPlain(const std::string& text) {
    return Json::objectOf({
        {"@type", Json("richTextPlain")},
        {"text", Json(text)},
    });
}
inline Json rtWrap(const char* type, const Json& inner) {
    return Json::objectOf({
        {"@type", Json(type)},
        {"text", inner},
    });
}
inline Json rtBold(const Json& inner) { return rtWrap("richTextBold", inner); }
inline Json rtItalic(const Json& inner) {
    return rtWrap("richTextItalic", inner);
}
inline Json rtUnderline(const Json& inner) {
    return rtWrap("richTextUnderline", inner);
}
inline Json rtStrikethrough(const Json& inner) {
    return rtWrap("richTextStrikethrough", inner);
}
inline Json rtSpoiler(const Json& inner) {
    return rtWrap("richTextSpoiler", inner);
}
inline Json rtSubscript(const Json& inner) {
    return rtWrap("richTextSubscript", inner);
}
inline Json rtSuperscript(const Json& inner) {
    return rtWrap("richTextSuperscript", inner);
}
inline Json rtMarked(const Json& inner) {
    return rtWrap("richTextMarked", inner);
}
inline Json rtFixed(const Json& inner) {
    return rtWrap("richTextFixed", inner);  // inline fixed-width code
}
inline Json rtUrl(const Json& inner, const std::string& url) {
    return Json::objectOf({
        {"@type", Json("richTextUrl")},
        {"text", inner},
        {"url", Json(url)},
        {"is_cached", Json(false)},
    });
}
inline Json rtMention(const Json& inner, const std::string& username) {
    return Json::objectOf({
        {"@type", Json("richTextMention")},
        {"text", inner},
        {"username", Json(username)},
    });
}
inline Json rtMentionName(const Json& inner, int64_t user_id) {
    return Json::objectOf({
        {"@type", Json("richTextMentionName")},
        {"text", inner},
        {"user_id", Json(user_id)},
    });
}
inline Json rtHashtag(const Json& inner, const std::string& hashtag) {
    return Json::objectOf({
        {"@type", Json("richTextHashtag")},
        {"text", inner},
        {"hashtag", Json(hashtag)},
    });
}
inline Json rtBotCommand(const Json& inner, const std::string& command) {
    return Json::objectOf({
        {"@type", Json("richTextBotCommand")},
        {"text", inner},
        {"bot_command", Json(command)},
    });
}
inline Json rtEmail(const Json& inner, const std::string& email) {
    return Json::objectOf({
        {"@type", Json("richTextEmailAddress")},
        {"text", inner},
        {"email_address", Json(email)},
    });
}
inline Json rtPhone(const Json& inner, const std::string& phone) {
    return Json::objectOf({
        {"@type", Json("richTextPhoneNumber")},
        {"text", inner},
        {"phone_number", Json(phone)},
    });
}
inline Json rtCustomEmoji(int64_t emoji_id, const std::string& alt) {
    return Json::objectOf({
        {"@type", Json("richTextCustomEmoji")},
        {"custom_emoji_id", Json(emoji_id)},
        {"alternative_text", Json(alt)},
    });
}
inline Json rtMath(const std::string& expression) {
    return Json::objectOf({
        {"@type", Json("richTextMathematicalExpression")},
        {"expression", Json(expression)},
    });
}
inline Json rtReference(const std::string& name, const Json& text) {
    return Json::objectOf({
        {"@type", Json("richTextReference")},
        {"name", Json(name)},
        {"text", text},
    });
}
inline Json rtReferenceLink(const Json& text, const std::string& ref_name,
                            const std::string& url) {
    return Json::objectOf({
        {"@type", Json("richTextReferenceLink")},
        {"text", text},
        {"reference_name", Json(ref_name)},
        {"url", Json(url)},
    });
}
inline Json rtAnchor(const std::string& name) {
    return Json::objectOf({
        {"@type", Json("richTextAnchor")},
        {"name", Json(name)},
    });
}
inline Json rtAnchorLink(const Json& text, const std::string& anchor_name,
                         const std::string& url) {
    return Json::objectOf({
        {"@type", Json("richTextAnchorLink")},
        {"text", text},
        {"anchor_name", Json(anchor_name)},
        {"url", Json(url)},
    });
}
inline Json rtTexts(std::vector<Json> parts) {
    Json::Array arr;
    for (auto& p : parts) arr.push_back(std::move(p));
    return Json::objectOf({
        {"@type", Json("richTexts")},
        {"texts", Json(std::move(arr))},
    });
}

// --- In-message buttons (inputPageBlockButtonRow) -----------------------
// One button for a button row. Reuses the core's InlineButton shape
// (callback_data / url / copy_text / switch_inline_query — exactly
// one set), plus a per-button style. Styles: default, primary,
// danger, success, link. "link" is callback-only (TDLib rule).
// The verified button path is the Blocks source; <tg-button> inside
// Rich HTML is a server-side convention, not used here.
struct RichButton {
    InlineButton button;
    std::string style = "default";
    static RichButton callback(const std::string& text,
                               const std::string& data,
                               const std::string& style = "default") {
        RichButton b;
        b.button = InlineButton::callback(text, data);
        b.style = style;
        return b;
    }
    static RichButton link(const std::string& text, const std::string& url,
                           const std::string& style = "default") {
        RichButton b;
        b.button = InlineButton::link(text, url);
        b.style = style;
        return b;
    }
    static RichButton copy(const std::string& text,
                           const std::string& copy_text,
                           const std::string& style = "default") {
        RichButton b;
        b.button = InlineButton::copy(text, copy_text);
        b.style = style;
        return b;
    }
    static RichButton switchInline(const std::string& text,
                                   const std::string& query,
                                   bool current_chat,
                                   const std::string& style = "default") {
        RichButton b;
        b.button = InlineButton::switchInline(text, query, current_chat);
        b.style = style;
        return b;
    }
};

inline Json buttonStyleJson(const std::string& style) {
    std::string s = lowerAscii(style);
    if (s == "primary") return Json("buttonStylePrimary");
    if (s == "danger") return Json("buttonStyleDanger");
    if (s == "success") return Json("buttonStyleSuccess");
    if (s == "link") return Json("buttonStyleLink");
    return Json("buttonStyleDefault");
}

// One RichButton -> the TDLib `inlineButton` object. Callback data
// travels as base64 (TDLib `bytes`); copy_text is a client-side copy
// (no callback query is produced); a switch button targets the
// current chat or a chosen chat.
inline Json inlineButtonJson(const RichButton& b, std::string* error_out) {
    const InlineButton& btn = b.button;
    Json type;
    bool is_callback = false;
    if (!btn.callback_data.empty()) {
        if (btn.callback_data.size() > kMaxCallbackBytes) {
            if (error_out)
                *error_out = "rich button \"" + btn.text +
                             "\": callback_data is " +
                             std::to_string(btn.callback_data.size()) +
                             " bytes, over the 64-byte limit";
            return Json();
        }
        type = Json::objectOf({
            {"@type", Json("inlineKeyboardButtonTypeCallback")},
            {"data", Json(scram::base64Encode(btn.callback_data))},
        });
        is_callback = true;
    } else if (!btn.copy_text.empty()) {
        type = Json::objectOf({
            {"@type", Json("inlineKeyboardButtonTypeCopyText")},
            {"text", Json(btn.copy_text)},
        });
    } else if (!btn.url.empty()) {
        type = Json::objectOf({
            {"@type", Json("inlineKeyboardButtonTypeUrl")},
            {"url", Json(btn.url)},
        });
    } else if (btn.has_switch) {
        Json target =
            btn.switch_current_chat
                ? Json::objectOf({{"@type", Json("targetChatCurrent")}})
                : Json::objectOf({
                      {"@type", Json("targetChatChosen")},
                      {"types",
                       Json::objectOf({
                           {"@type", Json("targetChatTypes")},
                           {"allow_user_chats", Json(true)},
                           {"allow_bot_chats", Json(true)},
                           {"allow_group_chats", Json(true)},
                           {"allow_channel_chats", Json(true)},
                       })},
                  });
        type = Json::objectOf({
            {"@type", Json("inlineKeyboardButtonTypeSwitchInline")},
            {"query", Json(btn.switch_inline_query)},
            {"target_chat", target},
        });
    } else {
        if (error_out)
            *error_out = "rich button \"" + btn.text +
                         "\": no action (set callback_data, url, "
                         "copy_text or switch_inline_query)";
        return Json();
    }
    if (lowerAscii(b.style) == "link" && !is_callback) {
        if (error_out)
            *error_out = "rich button \"" + btn.text +
                         "\": style \"link\" is callback-only";
        return Json();
    }
    return Json::objectOf({
        {"@type", Json("inlineButton")},
        {"text", rtPlain(btn.text)},
        {"style", Json::objectOf({{"@type", buttonStyleJson(b.style)}})},
        {"type", type},
    });
}

// A full button row. align: "" = full-width buttons, or "left" /
// "center" / "right".
inline Json buttonRowJson(const std::vector<RichButton>& buttons,
                          const std::string& align,
                          std::string* error_out) {
    if (buttons.empty()) {
        if (error_out) *error_out = "rich button row must not be empty";
        return Json();
    }
    Json::Array arr;
    for (const auto& b : buttons) {
        Json j = inlineButtonJson(b, error_out);
        if (j.isNull()) return Json();
        arr.push_back(std::move(j));
    }
    Json align_json;
    std::string a = lowerAscii(align);
    if (a == "left")
        align_json = Json::objectOf(
            {{"@type", Json("pageBlockHorizontalAlignmentLeft")}});
    else if (a == "center")
        align_json = Json::objectOf(
            {{"@type", Json("pageBlockHorizontalAlignmentCenter")}});
    else if (a == "right")
        align_json = Json::objectOf(
            {{"@type", Json("pageBlockHorizontalAlignmentRight")}});
    else if (a.empty())
        align_json = Json(nullptr);
    else {
        if (error_out)
            *error_out = "rich button row: unknown align \"" + align +
                         "\" (expected \"\", \"left\", \"center\" or "
                         "\"right\")";
        return Json();
    }
    return Json::objectOf({
        {"@type", Json("inputPageBlockButtonRow")},
        {"buttons", Json(std::move(arr))},
        {"align", align_json},
    });
}

// --- Files / media -------------------------------------------------------
// A local path (starts with '/' and exists on disk) becomes
// inputFileLocal; anything else that is not an http(s) URL becomes
// inputFileRemote (a TDLib file id). An http(s) URL is NOT a valid
// inputFile: TDLib answers with its exact error, so the engine
// refuses it upfront with a precise message. Callers that hold a
// URL must download it to disk first — the core has no HTTP client
// for that today, so URL thumbnails fall back to the text card.
inline Json inputFileJson(const std::string& path_or_id,
                          std::string* error_out) {
    if (path_or_id.empty()) {
        if (error_out) *error_out = "rich media: empty file path or id";
        return Json();
    }
    std::string lower = lowerAscii(path_or_id);
    if (lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0) {
        if (error_out)
            *error_out = "rich media: an http(s) URL is not a valid "
                         "inputFile (download it to disk first): " +
                         path_or_id;
        return Json();
    }
    std::error_code ec;
    if (!path_or_id.empty() && path_or_id[0] == '/' &&
        std::filesystem::exists(path_or_id, ec)) {
        return Json::objectOf({
            {"@type", Json("inputFileLocal")},
            {"path", Json(path_or_id)},
        });
    }
    return Json::objectOf({
        {"@type", Json("inputFileRemote")},
        {"id", Json(path_or_id)},
    });
}

// True when the value can be used as an inputFile at all (a local
// path that exists, or a non-URL id). Used by callers that choose
// between a media card and the text fallback.
inline bool isUsableInputFile(const std::string& path_or_id) {
    std::string err;
    return !inputFileJson(path_or_id, &err).isNull();
}

// The input* wrappers the block-level media and the media array
// need. Only the file is set; every other field takes its TDLib
// default (width/height 0 = unknown, empty sticker list).
inline Json inputPhotoJson(const std::string& path_or_id,
                           std::string* error_out) {
    Json file = inputFileJson(path_or_id, error_out);
    if (file.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputPhoto")},
        {"photo", file},
        {"thumbnail", Json(nullptr)},
        {"video", Json(nullptr)},
        {"added_sticker_file_ids", Json(Json::Array{})},
        {"width", Json(static_cast<int64_t>(0))},
        {"height", Json(static_cast<int64_t>(0))},
    });
}
inline Json inputVideoJson(const std::string& path_or_id,
                           std::string* error_out) {
    Json file = inputFileJson(path_or_id, error_out);
    if (file.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputVideo")},
        {"video", file},
        {"thumbnail", Json(nullptr)},
        {"cover", Json(nullptr)},
        {"start_timestamp", Json(static_cast<int64_t>(0))},
        {"added_sticker_file_ids", Json(Json::Array{})},
        {"duration", Json(static_cast<int64_t>(0))},
        {"width", Json(static_cast<int64_t>(0))},
        {"height", Json(static_cast<int64_t>(0))},
        {"supports_streaming", Json(false)},
    });
}
inline Json inputAudioJson(const std::string& path_or_id,
                           std::string* error_out) {
    Json file = inputFileJson(path_or_id, error_out);
    if (file.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputAudio")},
        {"audio", file},
        {"album_cover_thumbnail", Json(nullptr)},
        {"duration", Json(static_cast<int64_t>(0))},
        {"title", Json("")},
        {"performer", Json("")},
    });
}
inline Json inputDocumentJson(const std::string& path_or_id,
                              std::string* error_out) {
    Json file = inputFileJson(path_or_id, error_out);
    if (file.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputDocument")},
        {"document", file},
        {"thumbnail", Json(nullptr)},
        {"disable_content_type_detection", Json(false)},
    });
}
inline Json inputAnimationJson(const std::string& path_or_id,
                               std::string* error_out) {
    Json file = inputFileJson(path_or_id, error_out);
    if (file.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputAnimation")},
        {"animation", file},
        {"thumbnail", Json(nullptr)},
        {"added_sticker_file_ids", Json(Json::Array{})},
        {"duration", Json(static_cast<int64_t>(0))},
        {"width", Json(static_cast<int64_t>(0))},
        {"height", Json(static_cast<int64_t>(0))},
    });
}
inline Json inputVoiceNoteJson(const std::string& path_or_id,
                               std::string* error_out) {
    Json file = inputFileJson(path_or_id, error_out);
    if (file.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputVoiceNote")},
        {"voice_note", file},
        {"duration", Json(static_cast<int64_t>(0))},
        {"waveform", Json("")},
    });
}

// An empty formattedText for media captions (the schema allows a
// null caption, but an explicit empty object is unambiguous).
inline Json emptyFormattedText() {
    return Json::objectOf({
        {"@type", Json("formattedText")},
        {"text", Json("")},
        {"entities", Json(Json::Array{})},
    });
}

// inputMessage* content objects for the HTML-source media array.
// The caption is always empty: rich media carry their text in the
// surrounding blocks, not in a caption.
inline Json mediaPhotoContent(const std::string& path_or_id, bool spoiler,
                              std::string* error_out) {
    Json photo = inputPhotoJson(path_or_id, error_out);
    if (photo.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputMessagePhoto")},
        {"photo", photo},
        {"caption", emptyFormattedText()},
        {"show_caption_above_media", Json(false)},
        {"has_spoiler", Json(spoiler)},
    });
}
inline Json mediaVideoContent(const std::string& path_or_id, bool spoiler,
                              std::string* error_out) {
    Json video = inputVideoJson(path_or_id, error_out);
    if (video.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputMessageVideo")},
        {"video", video},
        {"caption", emptyFormattedText()},
        {"show_caption_above_media", Json(false)},
        {"has_spoiler", Json(spoiler)},
    });
}
inline Json mediaAudioContent(const std::string& path_or_id,
                              std::string* error_out) {
    Json audio = inputAudioJson(path_or_id, error_out);
    if (audio.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputMessageAudio")},
        {"audio", audio},
        {"caption", emptyFormattedText()},
    });
}
inline Json mediaDocumentContent(const std::string& path_or_id,
                                 std::string* error_out) {
    Json doc = inputDocumentJson(path_or_id, error_out);
    if (doc.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputMessageDocument")},
        {"document", doc},
        {"caption", emptyFormattedText()},
    });
}
inline Json mediaAnimationContent(const std::string& path_or_id, bool spoiler,
                                  std::string* error_out) {
    Json anim = inputAnimationJson(path_or_id, error_out);
    if (anim.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputMessageAnimation")},
        {"animation", anim},
        {"caption", emptyFormattedText()},
        {"show_caption_above_media", Json(false)},
        {"has_spoiler", Json(spoiler)},
    });
}
inline Json mediaVoiceNoteContent(const std::string& path_or_id,
                                  std::string* error_out) {
    Json note = inputVoiceNoteJson(path_or_id, error_out);
    if (note.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputMessageVoiceNote")},
        {"voice_note", note},
        {"caption", emptyFormattedText()},
    });
}

// A pageBlockCaption: empty RichText for an absent text/credit (an
// empty RichText is treated as absent by TDLib — never JSON null).
// A non-object, non-null text/credit is a caller bug: rejected.
inline Json pageBlockCaptionJson(const Json& text, const Json& credit,
                                 std::string* error_out) {
    if ((!text.isNull() && !text.isObject()) ||
        (!credit.isNull() && !credit.isObject())) {
        if (error_out)
            *error_out = "rich caption: text and credit must be "
                         "RichText objects (see rtPlain) or null";
        return Json();
    }
    return Json::objectOf({
        {"@type", Json("pageBlockCaption")},
        {"text", text.isNull() ? rtPlain("") : text},
        {"credit", credit.isNull() ? rtPlain("") : credit},
    });
}

namespace detail {
// Recursive counters for the Blocks source: blocks + list items +
// table rows (≤ 500), media blocks (≤ 50), nesting depth (≤ 16),
// total text characters (≤ 32768). Mirrors the limits in the
// inputRichMessage doc comment (td_api.tl:146-150).
struct RichCounts {
    int blocks = 0;
    int media = 0;
    int max_depth = 0;
    std::size_t text_chars = 0;
};

inline void countRichText(const Json& rt, RichCounts& c) {
    if (!rt.isObject()) return;
    const Json* type = rt.find("@type");
    if (type == nullptr) return;
    const std::string t = type->asString();
    // Every string field except "@type" is message text (plain text,
    // URLs, usernames, expressions, names). Object/array fields are
    // recursed below.
    for (const auto& kv : rt.asObject()) {
        if (kv.first != "@type" && kv.second.isString())
            c.text_chars += utf8CodePoints(kv.second.asString());
    }
    if (t == "richTexts") {
        if (const Json* texts = rt.find("texts")) {
            for (const auto& part : texts->asArray())
                countRichText(part, c);
        }
        return;
    }
    if (t == "richTextButton") {
        if (const Json* btn = rt.find("button")) {
            if (const Json* text = btn->find("text"))
                countRichText(*text, c);
        }
        return;
    }
    if (const Json* inner = rt.find("text")) {
        if (inner->isObject()) countRichText(*inner, c);
    }
}

inline bool countBlocks(const Json& blocks, int depth, RichCounts& c,
                        std::string* error_out) {
    if (depth > kMaxDepth) {
        if (error_out)
            *error_out = "rich message: nesting depth exceeds the "
                         "TDLib limit of 16";
        return false;
    }
    if (depth > c.max_depth) c.max_depth = depth;
    if (!blocks.isArray()) return true;
    for (const auto& block : blocks.asArray()) {
        if (!block.isObject()) continue;
        ++c.blocks;
        if (c.blocks > kMaxBlocks) {
            if (error_out)
                *error_out = "rich message: more than 500 blocks / "
                             "list items / table rows (TDLib limit)";
            return false;
        }
        const Json* type = block.find("@type");
        const std::string t =
            type != nullptr ? type->asString() : std::string();
        if (t == "inputPageBlockPhoto" || t == "inputPageBlockVideo" ||
            t == "inputPageBlockAudio" || t == "inputPageBlockDocument" ||
            t == "inputPageBlockAnimation" ||
            t == "inputPageBlockVoiceNote") {
            ++c.media;
            if (c.media > kMaxMedia) {
                if (error_out)
                    *error_out = "rich message: more than 50 media "
                                 "blocks (TDLib limit)";
                return false;
            }
        }
        if (t == "inputPageBlockList") {
            if (const Json* items = block.find("items")) {
                for (const auto& item : items->asArray()) {
                    ++c.blocks;  // list items count toward the 500
                    if (c.blocks > kMaxBlocks) {
                        if (error_out)
                            *error_out =
                                "rich message: more than 500 blocks / "
                                "list items / table rows (TDLib limit)";
                        return false;
                    }
                    if (const Json* ib = item.find("blocks")) {
                        if (!countBlocks(*ib, depth + 1, c, error_out))
                            return false;
                    }
                }
            }
            continue;
        }
        if (t == "inputPageBlockBlockQuote" ||
            t == "inputPageBlockDetails" ||
            t == "inputPageBlockCollage" ||
            t == "inputPageBlockSlideshow") {
            if (const Json* inner = block.find("blocks")) {
                if (!countBlocks(*inner, depth + 1, c, error_out))
                    return false;
            }
        }
        if (t == "inputPageBlockTable") {
            if (const Json* cells = block.find("cells")) {
                for (const auto& row : cells->asArray()) {
                    ++c.blocks;  // table rows count toward the 500
                    if (c.blocks > kMaxBlocks) {
                        if (error_out)
                            *error_out =
                                "rich message: more than 500 blocks / "
                                "list items / table rows (TDLib limit)";
                        return false;
                    }
                    int columns = 0;
                    for (const auto& cell : row.asArray()) {
                        if (const Json* cs = cell.find("colspan"))
                            columns += static_cast<int>(cs->asInt());
                        if (const Json* text = cell.find("text"))
                            countRichText(*text, c);
                    }
                    if (columns > kMaxTableColumns) {
                        if (error_out)
                            *error_out =
                                "rich message: table row has " +
                                std::to_string(columns) +
                                " columns, over the TDLib limit of 20";
                        return false;
                    }
                }
            }
        }
        if (t == "inputPageBlockButtonRow") {
            if (const Json* buttons = block.find("buttons")) {
                for (const auto& btn : buttons->asArray()) {
                    if (const Json* text = btn.find("text"))
                        countRichText(*text, c);
                }
            }
        }
        // RichText-bearing fields on every block kind.
        for (const char* key : {"text", "footer", "header", "credit"}) {
            if (const Json* field = block.find(key)) {
                if (field->isObject()) countRichText(*field, c);
            }
        }
        if (const Json* caption = block.find("caption")) {
            if (caption->isObject()) {
                if (const Json* text = caption->find("text"))
                    countRichText(*text, c);
                if (const Json* credit = caption->find("credit"))
                    countRichText(*credit, c);
            }
        }
        if (t == "inputPageBlockMathematicalExpression" ||
            t == "inputPageBlockAnchor") {
            if (const Json* s = block.find("expression"))
                c.text_chars += utf8CodePoints(s->asString());
            if (const Json* s = block.find("name"))
                c.text_chars += utf8CodePoints(s->asString());
        }
    }
    return true;
}

inline Json horizontalAlignmentJson(const std::string& align) {
    std::string a = lowerAscii(align);
    if (a == "center")
        return Json::objectOf(
            {{"@type", Json("pageBlockHorizontalAlignmentCenter")}});
    if (a == "right")
        return Json::objectOf(
            {{"@type", Json("pageBlockHorizontalAlignmentRight")}});
    return Json::objectOf(
        {{"@type", Json("pageBlockHorizontalAlignmentLeft")}});
}

inline Json verticalAlignmentJson(const std::string& valign) {
    std::string v = lowerAscii(valign);
    if (v == "middle")
        return Json::objectOf(
            {{"@type", Json("pageBlockVerticalAlignmentMiddle")}});
    if (v == "bottom")
        return Json::objectOf(
            {{"@type", Json("pageBlockVerticalAlignmentBottom")}});
    return Json::objectOf(
        {{"@type", Json("pageBlockVerticalAlignmentTop")}});
}

}  // namespace detail

// --- RichMessage: the builder --------------------------------------------
// One message, exactly one source (TDLib rule). Build it, then hand
// it to TdLibTelegramSender::sendRichMessage. Every builder method
// validates its own arguments and returns false + error_out without
// changing the message on failure; buildContent re-validates the
// whole message (limits) before emitting the content object.
class RichMessage {
public:
    // Rich HTML source. The HTML is validated at buildContent time
    // (validateRichHtml); media are declared with addMedia and
    // referenced in the text as tg://photo?id=<id> etc. (documented
    // server-side convention — see the guide).
    static RichMessage fromHtml(const std::string& html) {
        RichMessage m;
        m.is_html_ = true;
        m.html_ = html;
        return m;
    }
    // Blocks source: append blocks with the methods below.
    static RichMessage fromBlocks() {
        RichMessage m;
        m.is_html_ = false;
        return m;
    }

    bool isHtml() const { return is_html_; }
    std::size_t blockCount() const { return blocks_.size(); }
    std::size_t mediaCount() const { return media_.size(); }

    // Declare one media element for the HTML source: id is the
    // cross-reference key (1-64 base64url chars, unique per
    // message); content is an inputMessage* object from
    // mediaPhotoContent / mediaVideoContent / … Duplicate ids and
    // bad ids are rejected.
    bool addMedia(const std::string& id, const Json& content,
                  std::string* error_out) {
        if (!is_html_) {
            if (error_out)
                *error_out = "rich message: addMedia is for the HTML "
                             "source only; the Blocks source carries "
                             "media as blocks";
            return false;
        }
        if (id.empty() || id.size() > kMaxMediaIdChars) {
            if (error_out)
                *error_out = "rich media id must be 1-64 characters";
            return false;
        }
        for (char ch : id) {
            if (!isBase64UrlChar(ch)) {
                if (error_out)
                    *error_out = "rich media id \"" + id +
                                 "\" must be base64url characters "
                                 "(A-Z a-z 0-9 _ -)";
                return false;
            }
        }
        for (const auto& m : media_) {
            const Json* mid = m.find("id");
            if (mid != nullptr && mid->asString() == id) {
                if (error_out)
                    *error_out = "rich media id \"" + id +
                                 "\" is already declared in this message";
                return false;
            }
        }
        if (!content.isObject() || content.find("@type") == nullptr) {
            if (error_out)
                *error_out = "rich media \"" + id +
                             "\": content must be an inputMessage* "
                             "object (see mediaPhotoContent etc.)";
            return false;
        }
        if (media_.size() >= static_cast<std::size_t>(kMaxMedia)) {
            if (error_out)
                *error_out = "rich message: more than 50 media "
                             "elements (TDLib limit)";
            return false;
        }
        media_.push_back(Json::objectOf({
            {"@type", Json("inputRichMessageMedia")},
            {"id", Json(id)},
            {"media", content},
        }));
        return true;
    }

    // --- Blocks-source builders (each appends one block) ---
    bool heading(int level, const Json& text, std::string* error_out) {
        if (level < 1 || level > 6) {
            if (error_out)
                *error_out = "rich heading: level must be 1-6, got " +
                             std::to_string(level);
            return false;
        }
        if (!text.isObject()) {
            if (error_out)
                *error_out = "rich heading: text must be a RichText "
                             "object (see rtPlain / rtBold)";
            return false;
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockSectionHeading")},
            {"text", text},
            {"size", Json(static_cast<int64_t>(level))},
        }), error_out);
    }
    bool paragraph(const Json& text, std::string* error_out) {
        if (!text.isObject()) {
            if (error_out)
                *error_out = "rich paragraph: text must be a RichText "
                             "object (see rtPlain / rtBold)";
            return false;
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockParagraph")},
            {"text", text},
        }), error_out);
    }
    bool preformatted(const Json& text, const std::string& language,
                      std::string* error_out) {
        if (!text.isObject()) {
            if (error_out)
                *error_out = "rich preformatted: text must be a "
                             "RichText object";
            return false;
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockPreformatted")},
            {"text", text},
            {"language", Json(language)},
        }), error_out);
    }
    bool footer(const Json& text, std::string* error_out) {
        if (!text.isObject()) {
            if (error_out)
                *error_out =
                    "rich footer: text must be a RichText object";
            return false;
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockFooter")},
            {"footer", text},
        }), error_out);
    }
    bool divider(std::string* error_out) {
        (void)error_out;
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockDivider")},
        }), error_out);
    }
    bool mathBlock(const std::string& expression, std::string* error_out) {
        if (expression.empty()) {
            if (error_out)
                *error_out = "rich math block: expression must not be "
                             "empty (raw LaTeX)";
            return false;
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockMathematicalExpression")},
            {"expression", Json(expression)},
        }), error_out);
    }
    bool anchorBlock(const std::string& name, std::string* error_out) {
        if (name.empty()) {
            if (error_out)
                *error_out = "rich anchor: name must not be empty";
            return false;
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockAnchor")},
            {"name", Json(name)},
        }), error_out);
    }

    // One list item: its blocks (usually a single paragraph) plus an
    // optional checkbox. The builder numbers ordered items and sets
    // the ordered/unordered marker; a list must be entirely one or
    // the other (TDLib rule, WebPageBlock.cpp).
    struct ListItem {
        std::vector<Json> blocks;
        bool has_checkbox = false;
        bool is_checked = false;
    };
    bool list(const std::vector<ListItem>& items, bool ordered,
              std::string* error_out) {
        if (items.empty()) {
            if (error_out)
                *error_out = "rich list: must not be empty (TDLib "
                             "rejects empty lists)";
            return false;
        }
        Json::Array arr;
        int n = 0;
        for (const auto& item : items) {
            if (item.blocks.empty()) {
                if (error_out)
                    *error_out = "rich list: every item needs at "
                                 "least one block";
                return false;
            }
            Json::Array item_blocks;
            for (const auto& b : item.blocks) {
                if (!b.isObject()) {
                    if (error_out)
                        *error_out = "rich list: item blocks must be "
                                     "block objects";
                    return false;
                }
                item_blocks.push_back(b);
            }
            ++n;
            arr.push_back(Json::objectOf({
                {"@type", Json("inputPageBlockListItem")},
                {"blocks", Json(std::move(item_blocks))},
                {"has_checkbox", Json(item.has_checkbox)},
                {"is_checked", Json(item.is_checked)},
                {"value", Json(static_cast<int64_t>(ordered ? n : 0))},
                {"type", Json(ordered ? "1" : "")},
            }));
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockList")},
            {"items", Json(std::move(arr))},
        }), error_out);
    }
    // Checkbox list: (text, checked) pairs, unordered.
    bool checkList(const std::vector<std::pair<Json, bool>>& items,
                   std::string* error_out) {
        std::vector<ListItem> list_items;
        for (const auto& [text, checked] : items) {
            if (!text.isObject()) {
                if (error_out)
                    *error_out = "rich check list: item text must be "
                                 "a RichText object";
                return false;
            }
            ListItem li;
            li.blocks.push_back(Json::objectOf({
                {"@type", Json("inputPageBlockParagraph")},
                {"text", text},
            }));
            li.has_checkbox = true;
            li.is_checked = checked;
            list_items.push_back(std::move(li));
        }
        return list(list_items, false, error_out);
    }
    bool blockQuote(const std::vector<Json>& blocks, const Json& credit,
                    std::string* error_out) {
        if (blocks.empty()) {
            if (error_out)
                *error_out = "rich block quote: must not be empty";
            return false;
        }
        Json::Array arr;
        for (const auto& b : blocks) {
            if (!b.isObject()) {
                if (error_out)
                    *error_out = "rich block quote: blocks must be "
                                 "block objects";
                return false;
            }
            arr.push_back(b);
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockBlockQuote")},
            {"blocks", Json(std::move(arr))},
            {"credit", credit.isNull() ? rtPlain("") : credit},
        }), error_out);
    }
    bool expandableQuote(const Json& text, const Json& credit,
                         std::string* error_out) {
        if (!text.isObject()) {
            if (error_out)
                *error_out = "rich expandable quote: text must be a "
                             "RichText object";
            return false;
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockExpandableBlockQuote")},
            {"text", text},
            {"credit", credit.isNull() ? rtPlain("") : credit},
        }), error_out);
    }
    bool pullQuote(const Json& text, const Json& credit,
                   std::string* error_out) {
        if (!text.isObject()) {
            if (error_out)
                *error_out = "rich pull quote: text must be a RichText "
                             "object";
            return false;
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockPullQuote")},
            {"text", text},
            {"credit", credit.isNull() ? rtPlain("") : credit},
        }), error_out);
    }

    struct TableCell {
        Json text;  // RichText (required)
        bool is_header = false;
        int colspan = 1;
        int rowspan = 1;
        std::string align;   // "" | "left" | "center" | "right"
        std::string valign;  // "" | "top" | "middle" | "bottom"
    };
    bool table(const Json& caption,
               const std::vector<std::vector<TableCell>>& rows,
               bool bordered, bool striped, bool compact,
               std::string* error_out) {
        if (rows.empty()) {
            if (error_out)
                *error_out = "rich table: must have at least one row";
            return false;
        }
        Json::Array json_rows;
        for (const auto& row : rows) {
            if (row.empty()) {
                if (error_out)
                    *error_out = "rich table: rows must not be empty";
                return false;
            }
            int columns = 0;
            Json::Array json_row;
            for (const auto& cell : row) {
                if (!cell.text.isObject()) {
                    if (error_out)
                        *error_out = "rich table: cell text must be a "
                                     "RichText object";
                    return false;
                }
                if (cell.colspan < 1 || cell.rowspan < 1) {
                    if (error_out)
                        *error_out = "rich table: colspan/rowspan "
                                     "must be >= 1";
                    return false;
                }
                columns += cell.colspan;
                json_row.push_back(Json::objectOf({
                    {"@type", Json("pageBlockTableCell")},
                    {"text", cell.text},
                    {"is_header", Json(cell.is_header)},
                    {"colspan", Json(static_cast<int64_t>(cell.colspan))},
                    {"rowspan", Json(static_cast<int64_t>(cell.rowspan))},
                    {"align",
                     detail::horizontalAlignmentJson(cell.align)},
                    {"valign",
                     detail::verticalAlignmentJson(cell.valign)},
                }));
            }
            if (columns > kMaxTableColumns) {
                if (error_out)
                    *error_out = "rich table: row has " +
                                 std::to_string(columns) +
                                 " columns, over the TDLib limit of 20";
                return false;
            }
            json_rows.push_back(Json(std::move(json_row)));
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockTable")},
            {"caption", caption.isNull() ? rtPlain("") : caption},
            {"cells", Json(std::move(json_rows))},
            {"is_bordered", Json(bordered)},
            {"is_striped", Json(striped)},
            {"is_compact", Json(compact)},
        }), error_out);
    }
    bool details(const Json& header, const std::vector<Json>& blocks,
                 bool open, std::string* error_out) {
        if (!header.isObject()) {
            if (error_out)
                *error_out = "rich details: header must be a RichText "
                             "object";
            return false;
        }
        if (blocks.empty()) {
            if (error_out)
                *error_out = "rich details: content must not be empty";
            return false;
        }
        Json::Array arr;
        for (const auto& b : blocks) {
            if (!b.isObject()) {
                if (error_out)
                    *error_out = "rich details: content blocks must "
                                 "be block objects";
                return false;
            }
            arr.push_back(b);
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockDetails")},
            {"header", header},
            {"blocks", Json(std::move(arr))},
            {"is_open", Json(open)},
        }), error_out);
    }

    // --- Media blocks (Blocks source) ---
    // path_or_id is a local file path or a TDLib file id; an http(s)
    // URL is rejected (see inputFileJson). caption/credit may be
    // null Json() for "none".
    bool photoBlock(const std::string& path_or_id, const Json& caption,
                    const Json& credit, bool spoiler,
                    std::string* error_out) {
        Json photo = inputPhotoJson(path_or_id, error_out);
        if (photo.isNull()) return false;
        Json caption_json = pageBlockCaptionJson(caption, credit, error_out);
        if (caption_json.isNull()) return false;
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockPhoto")},
            {"photo", photo},
            {"caption", caption_json},
            {"has_spoiler", Json(spoiler)},
        }), error_out);
    }
    bool videoBlock(const std::string& path_or_id, const Json& caption,
                    const Json& credit, bool spoiler,
                    std::string* error_out) {
        Json video = inputVideoJson(path_or_id, error_out);
        if (video.isNull()) return false;
        Json caption_json = pageBlockCaptionJson(caption, credit, error_out);
        if (caption_json.isNull()) return false;
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockVideo")},
            {"video", video},
            {"caption", caption_json},
            {"has_spoiler", Json(spoiler)},
        }), error_out);
    }
    bool audioBlock(const std::string& path_or_id, const Json& caption,
                    const Json& credit, std::string* error_out) {
        Json audio = inputAudioJson(path_or_id, error_out);
        if (audio.isNull()) return false;
        Json caption_json = pageBlockCaptionJson(caption, credit, error_out);
        if (caption_json.isNull()) return false;
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockAudio")},
            {"audio", audio},
            {"caption", caption_json},
        }), error_out);
    }
    bool documentBlock(const std::string& path_or_id, const Json& caption,
                       const Json& credit, std::string* error_out) {
        Json doc = inputDocumentJson(path_or_id, error_out);
        if (doc.isNull()) return false;
        Json caption_json = pageBlockCaptionJson(caption, credit, error_out);
        if (caption_json.isNull()) return false;
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockDocument")},
            {"document", doc},
            {"caption", caption_json},
        }), error_out);
    }
    bool voiceNoteBlock(const std::string& path_or_id, const Json& caption,
                        const Json& credit, std::string* error_out) {
        Json note = inputVoiceNoteJson(path_or_id, error_out);
        if (note.isNull()) return false;
        Json caption_json = pageBlockCaptionJson(caption, credit, error_out);
        if (caption_json.isNull()) return false;
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockVoiceNote")},
            {"voice_note", note},
            {"caption", caption_json},
        }), error_out);
    }
    bool animationBlock(const std::string& path_or_id, const Json& caption,
                        const Json& credit, bool spoiler,
                        std::string* error_out) {
        Json anim = inputAnimationJson(path_or_id, error_out);
        if (anim.isNull()) return false;
        Json caption_json = pageBlockCaptionJson(caption, credit, error_out);
        if (caption_json.isNull()) return false;
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockAnimation")},
            {"animation", anim},
            {"caption", caption_json},
            {"has_spoiler", Json(spoiler)},
        }), error_out);
    }
    bool collage(const std::vector<Json>& blocks, const Json& caption,
                 const Json& credit, std::string* error_out) {
        if (blocks.empty()) {
            if (error_out)
                *error_out = "rich collage: must not be empty";
            return false;
        }
        Json caption_json = pageBlockCaptionJson(caption, credit, error_out);
        if (caption_json.isNull()) return false;
        Json::Array arr;
        for (const auto& b : blocks) {
            if (!b.isObject()) {
                if (error_out)
                    *error_out = "rich collage: blocks must be block "
                                 "objects";
                return false;
            }
            arr.push_back(b);
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockCollage")},
            {"blocks", Json(std::move(arr))},
            {"caption", caption_json},
        }), error_out);
    }
    bool slideshow(const std::vector<Json>& blocks, const Json& caption,
                   const Json& credit, std::string* error_out) {
        if (blocks.empty()) {
            if (error_out)
                *error_out = "rich slideshow: must not be empty";
            return false;
        }
        Json caption_json = pageBlockCaptionJson(caption, credit, error_out);
        if (caption_json.isNull()) return false;
        Json::Array arr;
        for (const auto& b : blocks) {
            if (!b.isObject()) {
                if (error_out)
                    *error_out = "rich slideshow: blocks must be "
                                 "block objects";
                return false;
            }
            arr.push_back(b);
        }
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockSlideshow")},
            {"blocks", Json(std::move(arr))},
            {"caption", caption_json},
        }), error_out);
    }
    bool mapBlock(double latitude, double longitude, int zoom, int width,
                  int height, const Json& caption, const Json& credit,
                  std::string* error_out) {
        if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 ||
            longitude > 180.0) {
            if (error_out)
                *error_out = "rich map: latitude must be -90..90 and "
                             "longitude -180..180";
            return false;
        }
        if (zoom < 0 || zoom > 24) {
            if (error_out)
                *error_out = "rich map: zoom must be 0-24";
            return false;
        }
        if (width <= 0 || height <= 0 || width + height > 10000 ||
            width > height * 20 || height > width * 20) {
            if (error_out)
                *error_out = "rich map: width+height must be <= 10000 "
                             "with a side ratio <= 20";
            return false;
        }
        Json caption_json = pageBlockCaptionJson(caption, credit, error_out);
        if (caption_json.isNull()) return false;
        return appendBlock(Json::objectOf({
            {"@type", Json("inputPageBlockMap")},
            {"location",
             Json::objectOf({
                 {"@type", Json("location")},
                 {"latitude", Json(latitude)},
                 {"longitude", Json(longitude)},
                 {"horizontal_accuracy", Json(0.0)},
             })},
            {"zoom", Json(static_cast<int64_t>(zoom))},
            {"width", Json(static_cast<int64_t>(width))},
            {"height", Json(static_cast<int64_t>(height))},
            {"caption", caption_json},
        }), error_out);
    }
    // In-message buttons (the verified button path). align: "" =
    // full-width, or "left" / "center" / "right".
    bool buttonRow(const std::vector<RichButton>& buttons,
                   const std::string& align, std::string* error_out) {
        Json row = buttonRowJson(buttons, align, error_out);
        if (row.isNull()) return false;
        return appendBlock(std::move(row), error_out);
    }

    // --- Validation + build ---
    // Full validation: HTML tags/entities (HTML source) or the
    // recursive block limits (Blocks source). False + error_out on
    // the first violation, naming it exactly.
    bool validate(std::string* error_out) const {
        if (is_html_) {
            if (!validateRichHtml(html_, error_out)) return false;
            if (utf8CodePoints(html_) >
                static_cast<std::size_t>(kMaxTextChars)) {
                if (error_out)
                    *error_out = "rich message: HTML source is " +
                                 std::to_string(utf8CodePoints(html_)) +
                                 " characters, over the TDLib limit of "
                                 "32768";
                return false;
            }
            if (media_.size() > static_cast<std::size_t>(kMaxMedia)) {
                if (error_out)
                    *error_out = "rich message: more than 50 media "
                                 "elements (TDLib limit)";
                return false;
            }
            return true;
        }
        detail::RichCounts counts;
        if (!detail::countBlocks(Json(blocks_), 1, counts, error_out))
            return false;
        if (counts.text_chars > static_cast<std::size_t>(kMaxTextChars)) {
            if (error_out)
                *error_out = "rich message: total text is " +
                             std::to_string(counts.text_chars) +
                             " characters, over the TDLib limit of 32768";
            return false;
        }
        return true;
    }

    // The inputMessageRichMessage content object for sendMessage /
    // editMessageText. Json() + error_out on validation failure.
    // detect_automatic_blocks=true so URLs/emails auto-link exactly
    // like the classic messages did; is_rtl=false (LTR).
    Json buildContent(std::string* error_out) const {
        if (!validate(error_out)) return Json();
        Json source;
        if (is_html_) {
            source = Json::objectOf({
                {"@type", Json("richMessageSourceHtml")},
                {"text", Json(html_)},
                {"media", Json(media_)},
            });
        } else {
            source = Json::objectOf({
                {"@type", Json("richMessageSourceBlocks")},
                {"blocks", Json(blocks_)},
            });
        }
        return Json::objectOf({
            {"@type", Json("inputMessageRichMessage")},
            {"message",
             Json::objectOf({
                 {"@type", Json("inputRichMessage")},
                 {"source", source},
                 {"is_rtl", Json(false)},
                 {"detect_automatic_blocks", Json(true)},
             })},
            {"clear_draft", Json(true)},
        });
    }

private:
    bool appendBlock(const Json& block, std::string* error_out) {
        if (is_html_) {
            if (error_out)
                *error_out = "rich message: block builders need the "
                             "Blocks source (fromBlocks), this message "
                             "uses the HTML source (fromHtml)";
            return false;
        }
        if (blocks_.size() >= static_cast<std::size_t>(kMaxBlocks)) {
            if (error_out)
                *error_out = "rich message: more than 500 blocks "
                             "(TDLib limit)";
            return false;
        }
        blocks_.push_back(block);
        return true;
    }

    bool is_html_ = true;
    std::string html_;
    Json::Array blocks_;
    Json::Array media_;  // inputRichMessageMedia objects (HTML source)
};

}  // namespace parlay::rich

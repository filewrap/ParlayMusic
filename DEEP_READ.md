# Parlay — Deep Read (all 21 refs, actual code)

Date: 2026-11-02 (Phase 4). Full per-language reads live in
`phase4/deepread-cpp.md` (387 lines), `phase4/deepread-go.md` (359),
`phase4/deepread-tsjs.md` (451), `phase4/deepread-rustpy.md` (510).
This file is the synthesis: the answers to "how does a music bot
actually work" that the ports below are built on.

## 1. The one fact that decides the architecture: WHO plays in VC

**The bot account never plays audio.** In every ref that actually
joins a voice chat, a normal user account — the "assistant" /
userbot — holds the MTProto voice session and streams the audio.
The bot account only sends Bot API text (commands, panels, replies).

| Ref family | Who joins VC | Transport |
|---|---|---|
| NoxBeats (C++) | assistant userbot, round-robin over `SESSION`…`SESSION10` (`Bad/cmd/play.cpp::ensure_assistant()` — DB call record, `member_status`, unban, invite via username/link, `assistant_join`, join-request approve) | ntgcalls (`voice/media_ntg.cpp`, `-DWITH_NTGCALLS=ON`) |
| TinaMusic (C++20) | assistant via pure-C++ MTProto (`src/tinaproto/client.cpp::Play()`) | ffmpeg s16le 48 kHz stereo pipe → `SetStreamSources` hot-swap or `CreateCall` → offer → `JoinGroupCall` → `Connect(remote_sdp)` |
| anonx-cpp (C++) | TDLib client dlopens `libtdjson.so`; ntgcalls client dlopens `libntgcalls.so`; both fall back to a simulated engine with a warning. Its `mongo_client.cpp` is **not** a real Mongo client (in-memory behind a logging `connect()`). | TDLib + ntgcalls |
| Yukki-Go / ArcMusic / TgMusicBot (Go) | assistant account joins/leaves VC, autoleave, channel-play | ntgcalls (`ntgcalls/` in TgMusicBot) |
| TGVCBot / remix (TS) | grammy + MTProto userbot (`TelegramClient` + `StringSession`) + `tgcalls-next` `GramTGCalls` per chat. No TDLib/ntgcalls. | tgcalls-next; ffmpeg-piped s16le 48 kHz mono (remix says 65 kHz — looks like a typo that survived; not copied) |
| ParvatiMusic (Rust) | per-chat assistant (`bot/src/assistant.rs::from_sessions / for_chat`, `context.rs::join_locked`: set sources → ntg create → MTProto connect → `get_group_call` → `join_group_call` → WebRTC connect → `wait_connected` 20 s) | native Ferogram MTProto + ntgcalls (`mtproto/`, `ntg/`) |

Parlay consequence: the C++ core owns the Bot API surface and routes;
voice joining lives in the assistant session, reached through the
core's `VoiceCall` interface (bridge → dlopen'd ntgcalls → unavailable
with exact reason). The Opus pipeline (yt-dlp → ffmpeg → Opus) is
proven; a live join needs a real assistant session + network to
Telegram DCs (sandbox has proxy-only egress).

## 2. Queue / playback lifecycle (all refs agree)

- **Room per chat.** One queue + one now-playing per chat; rooms are
  created on first `/play`, restored paused after restart (Parlay
  improvement over remix's in-memory maps, lost on restart; TGVCBot
  truncates its SQLite/knex queue on boot).
- **Advance is a ticker or an event.** Go/Rust/Python use a 1 s
  duration ticker; TGVCBot/remix advance on `audio-finish`/`finish`
  events. Parlay VC services use the ticker (deterministic in tests).
- **Loop semantics** (ParvatiMusic `room.rs::RoomState::loop_n`):
  count 0–10 replays of the current track, decremented per replay;
  `/skip` resets it. Shuffle randomises the pick, never the stored
  order (vc-go) or Fisher–Yates the queue on enable (vc-rust).
- **Autoplay**: when the queue drains, pick from the YouTube mix
  `RD<video_id>` (ParvatiMusic `platforms/youtube.rs::mix_for`;
  vc-go `resolver.Related`; vc-rust flat mix fetch), skipping
  played history (cap 50).
- **Position**: `position_sec` advances with the ticker; seek is a
  restart at an offset (NoxBeats `/seek` min 10 s; speed/bass are
  seek-restart + stored value).

## 3. Command inventory (deduced to COMMANDS.md)

- **NoxBeats**: 15 modules (`Bad/cmd/register.hpp`):
  play, playback, play_cb, admin, sudo, broadcast, settings, tools,
  autoplay, queue_ops, chat_settings, bot_tools, cplay, rtmp,
  playlist — plus a 34-command `c*` channel-command map.
- **Yukki-Go / ArcMusic / TgMusicBot**: play/queue/loop/seek/
  playlist/autoplay/stats, admins/auth/filters, channel-play.
- **TGVCBot / remix**: play/stream, pause/resume, skip/next,
  stop/leave, volume 0–200, mute/unmute, shuffle, loop, queue,
  search (YouTube + JioSaavn), radio/stream URL, playlist (capped),
  lyrics, now-playing, live panel (remix `handlers/panel.ts`: 10
  buttons, volume ±1000 on a 0–20000 scale, clicker must be the
  user the panel replied to).
- **ParvatiMusic**: play family + `c*`/`f*`/`v*` variants, queue,
  skip, pause/resume, stop, VC ops, playlists, admin, moderation,
  lists, system, sticker.
- Full unified inventory: see `COMMANDS.md`.

## 4. Group-management model (NoxBeats, ported to the core)

- **Permission ladder**: owner > sudo > admin/auth > everyone.
  Owner bypasses all; sudo = owner or Store `sudoers`; admin =
  cached admin list (600 s TTL) then live `getChatMember`.
- **admin_mode 0/1/2** and **play_mode** (admin/everyone) gate who
  may control playback. Blacklisted users/chats denied.
  Maintenance mode → sudo only. Per-chat rate limit 1 cmd/s.
- **Auth users**: `/auth /unauth /authlist` — non-admins allowed to
  control playback in a chat. `/admincache` refreshes the list.
- **Chat settings**: playmode, adminmode, cmddelete, cleanmode
  (1–1440 min + background sweeper), thumbnail on/off, autoplay
  on/off/lang/mood + background watcher, channelplay, userbot.
- **Broadcast**: reply-required, `-copy/-nochat/-user` flags,
  200 ms spacing, failures inline.
- **Playlists**: max 100/user; add/rm/clear/list/play.

## 5. Providers

| Provider | How refs do it | Parlay service |
|---|---|---|
| YouTube | yt-dlp everywhere (search + resolve + download); Innertube-direct in ParvatiMusic `platforms/youtube.rs` | vc-go / vc-ts / vc-rust / downloaders |
| Spotify | client-credentials → search (NoxBeats, lemonic search, rustify); spotdl CLI per user (Spotify-Loader); oEmbed + YouTube match fallback (downloaders) | downloaders + spotify-rust |
| NetEase | cloudsearch + lyric APIs (no login); song-URL needs weapi/eapi encrypted payload (Music163bot-Go `utils/crypto.go`: AES-ECB key `e82ckenh8dichen8`, MD5 splice, `params` HEX-uppercase; classic weapi double AES-CBC + RSA `encSecKey`) | netease-go |
| JioSaavn | TGVCBot public proxy; search results carry **no duration** — `/song?id=` returns `"duration":"4:28"` (clock string; old `Number()` cast → `NaN → 0`, the Phase 3 bug) | vc-ts |
| Deezer | public API + 30 s previews without an ARL cookie; full tracks need a Deezer account | downloaders |
| Lemonic | upstream `modules/lemonic.ts` is an *unauthenticated* `GET ${LEMONIC_API}/api/raw/track/{id}` — **no API key exists upstream**; search is Spotify client-credentials. The Phase 3 `LEMONIC_API_KEY` gate was invented, not upstream. | vc-ts |
| SoundCloud / Apple / TikTok / M3U8 | TgMusicBot providers; TikTok via yt-dlp direct CDN | vc-go / downloaders |
| Telegram files | TGVCBot `telegram` provider (reply-to-audio) | vc-ts (partial) |
| Navidrome / Soulseek | Subsonic token auth + `rest/download` vs `rest/stream`; Deezer public API + 40/5 s rate limiter; Soulseek via slskd daemon; metadata adaptors (Deezer/Last.fm), journal | downloaders |

## 6. Audio FX (telegram-music-bot-rust `main.rs`, exact chains)

- Slow+Reverb `atempo=0.8,aecho=0.8:0.88:60:0.4`; Nightcore
  `asetrate=44100*1.25,atempo=1/1.25`; Bass Boost
  `bass=g=10:f=110:w=0.6,volume=0.9`; 8D
  `apulsator=mode=sine:hz=0.12,aecho=0.8:0.88:40:0.3`;
  original `-c:a copy`. 50 MB format cap, auto-ban.

## 7. Spotify watch / profanity (rustify)

- Per-user OAuth (`spotify/auth.rs`, `Manager::for_user`); poll
  loop (`tick/user.rs::check`) with backoff; disliked-track skip
  (`tick/disliked_track.rs` — real `next_track()` + playlist
  removal); profanity via rustrict trie (`profanity.rs`); lyrics
  priority Lrclib → Genius (`lyrics/mod.rs::search_for_track`);
  AI-slop classifier (`queue/track_check.rs`).

## 8. Sharing / voting

- **feel-me**: anonymous relay — `/start <nickname>` → Redis key
  → audio forwarded by `file_id` with Report/Block; feedback
  routed back via history. Parlay's share-rust mints tokenised
  links instead (no identity stored).
- **daslui**: Spotify-link DM → voting chat `accept:<id>` /
  `decline` buttons → `add_item_to_queue` (first voter decides,
  no counting). Parlay's queue-rust uses a 3-vote threshold.

## 9. GUI / radio

- **wavetune**: plain `<audio src={downloadUrl[2|1|0]}>` from the
  Saavn API, `timeupdate` seek bar, redirect-to-next on `ended`.
  **No volume control anywhere upstream.** WebAudio
  `MediaElementAudioSourceNode` silences cross-origin media, hence
  Parlay's same-origin `/api/audio` proxy (Range passthrough, 206).
- **openradiobot**: no VC at all — an HTTP fan-out (`GET /<chatId>`
  → live mp3) with a queue, autoplay (related videos) and loop
  modes. No station list. Parlay's curated stations + probe +
  fallback playlist are Parlay-original; radio now hands the
  stream URL to a VC engine via `vc.radio`.

## 10. DB / auth models

- NoxBeats: libmongoc, collection `state`, one document per key.
- Yukki-Go / ArcMusic: MongoDB (ArcMusic: MongoDB queue cache).
- TGVCBot: SQLite/knex (truncated on boot). remix: in-memory maps.
- rustify: SeaORM entities. feel-me: Diesel + Redis.
- Parlay: MongoDB (collection `state` in core, `vc_rooms` in
  vc-go) + Redis (RESP), JSON-file fallback only when no URI is
  set; core Store picks Mongo > Redis > file.

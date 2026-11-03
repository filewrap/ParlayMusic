# Parlay Phase 9 — engines port gaps

Items the §29 spec (UI_INVENTORY.md) does not define, or that an engine
cannot honestly provide. Nothing here was invented unilaterally: each
item is either implemented with the closest honest behaviour (noted) or
left out. The coordinator / core-cpp worker decides; the engines will
follow the decision.

## 1. Inline `lib` / `del` modes — no library backend in vc-ts

`search.inline` with `lib <q>` / `del <q>` returns empty results.
vc-ts has no library backend (no Navidrome library search/delete).
Needs a `library.search` / `library.delete` action or a Navidrome-backed
engine endpoint before the core can render library results or delete
buttons. (`services/vc-ts/src/main.js` `/v1/inline`, `src/resolvers.js`
`inlineMode`.)

## 2. Inline `np` / `s` / `l` modes need `chat_id`

§29's `search.inline` request is `{query, user_id}` only, but
`np`/`s`/`l` are defined as "the current track" — which needs a chat.
Implemented as an optional `chat_id` in the `/v1/inline` body: when
the core sends it and a track is playing, the current track is
returned; otherwise empty results. The coordinator should add
`chat_id` (optional) to the `search.inline` request, or define a
different way for the core to pass the chat.

## 3. Inline `l` lyrics article — lyrics text cannot fit in a Track

For `l`, `/v1/inline` returns the current track (same as `np`/`s`);
the core must fetch the lyrics text itself via `/v1/lyrics` with the
same `chat_id` it passed to inline. No new field was invented for this.

## 4. NetEase `.lrc` file path (`lrc_path`) — not in IPC-5

The task requires the Music163bot named-song `.lrc` file path; §29
IPC-5 does not define it. Implemented anyway as `lrc_path` in the
`netease.lyrics` response (file written to the download dir as
`netease-<id>.lrc`), flagged here for the coordinator to add to the
contract. If the contract rejects it, the file is still written but
the field should be dropped.

## 5. Spotify `ignored_by` — no ignore store

`spotify.details` returns `ignored_by: 0` always. Only dislikes are
stored (per-user dislike sets); there is no ignore store or endpoint.
The §25 Hide-alerts button has no engine endpoint: needs a
`spotify.ignore` action or a `spotify.dislike` extension (coordinator
decision).

## 6. Spotify `language` — no language detection

`spotify.details` returns `language: null` always. No language
detection exists in spotify-rust. Needs a detection step (lyrics
language ID) or the field stays null and the core hides the row.

## 7. Spotify `features` — audio-features endpoint deprecated

`spotify.details` returns `features: null` when
`/audio-features/{id}` is unavailable (it is deprecated and answers
403 for most app tokens). The core must render the features block
only when present. No values are ever faked.

## 8. Share feedback / report dedup — no IPC or state defined

Inventory §22 wants "Feedback is sent before" / "User already is
reported" dedup, but §29 defines no IPC or state for it. Not
implemented. Needs a feedback/report store + endpoint (coordinator
decision). The share link format itself IS implemented
(`share-<token>`, hyphen per §22).

## 9. Radio listener counts — no listener tracking

`radio.status` returns `listeners: 0` and `total_listeners: 0`
always. No listener tracking exists (there is no web player; the
stream goes to the VC engine). Real counts need a listener source
(the core's voice-chat participant list, or a player that reports
them) — an engine-side number cannot be invented.

## 10. Download responses carry `run_id` — new response field

`GET /v1/download/progress?run_id=` needs the core to know the run
id, and the only place it can learn it is the download response.
Every download POST response now includes `run_id` (12-hex). This
field is not in §29; the coordinator should add it to the download
responses in the contract (`download.youtube`, `download.spotify`,
`download.spotify-playlist`, `download.deezer`, `download.tiktok`,
`download.navidrome`, `download.soulseek`).

## 11. Granular progress is yt-dlp / Soulseek only

IPC-4 percentage/speed/ETA are real for yt-dlp downloads (parsed
`--progress` lines) and Soulseek transfers (slskd callbacks). For
Deezer / TikTok / Navidrome single-file downloads and Spotify
single-track downloads via spotdl, the run reports phase transitions
only (searching → downloading → done/failed) with percentage 0 until
the file lands — honest about the granularity available, never an
estimate. Spotify playlist downloads DO get per-track progress (each
track is a yt-dlp download). Navidrome single-song downloads could
gain chunked-read progress later; the run lifecycle already exists
for it.

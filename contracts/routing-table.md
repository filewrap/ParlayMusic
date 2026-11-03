# Parlay — End-to-End Routing Table (§7.5 exit criterion)

Status: **Phase 2, implemented** (Worker E2, Wave 3). Anchor commit: `af3b740`.
Sources (all read, not assumed): `services/core-cpp/src/ui_texts.hpp`
(callback builders `cb*`, `kCb*` consts, every `*Keyboard` builder),
`services/core-cpp/src/dispatcher.cpp` (`Dispatcher::handleCallback` +
all 18 `cb*` sub-handlers, `registerCommands`, `handleMessage`,
`handleInlineQuery`), `contracts/ipc.json` v0.3.1 (56 actions, frozen),
`UI_INVENTORY.md` / `phase9/` (inventory cross-check).

## How to read this table

- One row per distinct input. **No row is blank**: a cell that has no
  value by design says `none` (no backend action, no edit) or `n/a`
  (not applicable to that row kind) — never an empty cell.
- `Emits` is the exact `callback_data`, command, button text, or inline
  query the user produces. Callback templates use `<chat_id>`,
  `<token>`, `<run_id>`, `<user_id>`, `<page>`, `<hash>`, `<key>`,
  `<n>`, `<id>`, `<sender>`, `<msg>`, `<file_unique>`, `<topic>`,
  `<verb>`, `<effect>`, `<fmt>` for the variable parts. Every concrete
  callback is ≤64 bytes (proven by `tests/routing_test.cpp`, not by
  inspection).
- `Rendered update` is the exact user-visible outcome: an edited message
  (`edit`), a new message (`new`), a toast/alert (`toast`), a deleted
  message (`delete`), or an inline answer (`inline`). A silent ack is a
  toast with empty text — the callback query is still answered.
- `Status`:
  - `Live` — routed end-to-end today.
  - `Phase 3` — OD-3 (decided: implement all, no cuts). The button /
    command exists and is answered honestly today, but the backend
    action does not exist yet; Phase 3 implements it. The row records
    the verdict and the Phase 3 action needed.
  - `Gap` — a known §7.5 deviation recorded here, not hidden: the route
    exists but the rendered update violates "zero unrouted buttons /
    zero unhandled callbacks / zero dead ends" in a specific, named way.
    Fixing a `Gap` row is future work, not a Phase 2 scope change.
  - `No button` — the handler exists and answers, but no button in the
    UI emits this callback (reachable only with hand-crafted
    `callback_data`). Listed so the cross-check is complete in both
    directions.
- Router default branch: `Dispatcher::handleCallback`
  (`services/core-cpp/src/dispatcher.cpp`, `handleCallback`) has an
  explicit default branch — empty data, data >64 bytes, a first segment
  other than `pl`, an unknown domain, and any unknown verb inside a
  known domain all answer the callback query with the alert
  `Unknown action.` (`kToastUnknownAction`), never silence, never a
  crash, never an un-answered query. Handler exceptions are caught and
  answered with the alert `This feature is not available yet.`
  (`kToastFeatureUnavailable`). Verified by `tests/routing_test.cpp`
  (§H). No code change was needed for this item: the branch already
  existed.
- Cross-check result (§F): every `pl:` callback emitted by a builder in
  `ui_texts.hpp` has a handler in `handleCallback`, and every handler
  branch has a builder — except the `No button` rows listed in §F, which
  are handler-only by design or by omission (recorded, not silently
  dropped). Every reply-keyboard button is listed in §B. Every
  registered command (112 names in 87 groups + 70 aliases + 40
  channel commands) is listed in §C. Every inline-mode entry is listed
  in §D.

## §A. Inline-button callbacks (`pl:` namespace)

| # | Surface | Button / input | Emits | Router / handler | Backend action | Rendered update | Status |
|---|---|---|---|---|---|---|---|
| A1 | Help menu card; start card (`Help & Commands`); share help card (`Help`) | Back / Help & Commands / Help | `pl:help:main` | `handleCallback` → `cbHelp` | none | toast `Returning to main menu...` + edit to Help Menu card (`kHelpMenu`) with `helpMenuKeyboard()` | Live |
| A2 | Help menu card | Admin | `pl:help:admins` | `handleCallback` → `cbHelp` | none | toast `Admin Commands` + edit to `kHelpAdmins` page with `helpTopicKeyboard()` (Back) | Live |
| A3 | Help menu card | Auth | `pl:help:auth` | `handleCallback` → `cbHelp` | none | toast `Auth Users` + edit to `kHelpAuth` page with `helpTopicKeyboard()` | Live |
| A4 | Help menu card | Broadcast | `pl:help:bcast` | `handleCallback` → `cbHelp` | none | toast `Broadcast` + edit to `kHelpBcast` page with `helpTopicKeyboard()` | Live |
| A5 | Help menu card | Play | `pl:help:play` | `handleCallback` → `cbHelp` | none | toast `Play Commands` + edit to `kHelpPlay` page with `helpTopicKeyboard()` | Live |
| A6 | Help menu card | Sudo | `pl:help:sudoers` | `handleCallback` → `cbHelp` | none | toast `Sudo Commands` + edit to `kHelpSudoers` page with `helpTopicKeyboard()` | Live |
| A7 | Help menu card | Restrict | `pl:help:restrict` | `handleCallback` → `cbHelp` | none | toast `Restrict` + edit to `kHelpRestrict` page with `helpTopicKeyboard()` | Live |
| A8 | Help menu card | Thumbnail | `pl:help:thumb` | `handleCallback` → `cbHelp` | none | toast `Thumbnail Settings` + edit to `kHelpThumb` page with `helpTopicKeyboard()` | Live |
| A9 | Help menu card | Start | `pl:help:start` | `handleCallback` → `cbHelp` | none | toast `Basic Commands` + edit to `kHelpStart` page with `helpTopicKeyboard()` | Live |
| A10 | Help menu card | Autoplay | `pl:help:autoplay` | `handleCallback` → `cbHelp` | none | toast `Autoplay Control` + edit to `kHelpAutoplay` page with `helpTopicKeyboard()` | Live |
| A11 | Help menu card | Playlist | `pl:help:playlist` | `handleCallback` → `cbHelp` | none | toast `Playlist Commands` + edit to `kHelpPlaylist` page with `helpTopicKeyboard()` | Live |
| A12 | Help menu card | VC Logs | `pl:help:vclogs` | `handleCallback` → `cbHelp` | none | toast `VC Logger` + edit to `kHelpVclogs` page with `helpTopicKeyboard()` | Live |
| A13 | Help menu card | Playback Buttons | `pl:help:inline` | `handleCallback` → `cbHelp` | none | toast `Playback Buttons` + edit to `kHelpInline` page with `helpTopicKeyboard()` | Live |
| A14 | n/a — no button emits it | n/a | `pl:help:owner` | `handleCallback` → `cbHelp` | none | toast `Owner Commands` + edit to `kHelpOwner` page with `helpTopicKeyboard()` | No button |
| A15 | n/a — no button emits it | n/a | `pl:help:radio` | `handleCallback` → `cbHelp` | none | toast `Radio Commands` + edit to `kHelpRadio` page with `helpTopicKeyboard()` | No button |
| A16 | n/a — no button emits it | n/a | `pl:help:search` | `handleCallback` → `cbHelp` | none | toast `Search Commands` + edit to `kSearchHelp` page with `helpTopicKeyboard()` | No button |
| A17 | n/a — no button emits it | n/a | `pl:help:<unknown-topic>` | `handleCallback` → `cbHelp` | none | alert `Unknown help category.`, no edit, message stays | Live |
| A18 | Help menu card (`Back`); language picker (`Back`, origin `start`) | Back | `pl:start` | `handleCallback` → `cbStart` | none | silent ack (empty toast) + edit to start card (`kStartPm`) with `startPmKeyboard()` | Live |
| A19 | Now-playing card; settings panel; autoplay card; share send prompt (`Cancel sending music`) | Close / Cancel sending music | `pl:close` | `handleCallback` → `cbClose` | none | message deleted + silent ack (empty toast) | Live |
| A20 | Now-playing card, row 1 | Progress label (`M:SS ◉——— M:SS`) | `pl:player:progress:<chat_id>` | `handleCallback` → `cbPlayer` | none | silent ack (empty toast), no edit — no-op label button by design | Live |
| A21 | Now-playing card; /menu panel | Pause | `pl:player:pause:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.pause` | toast `Track paused at M:SS` + card refresh (`refreshCard` / `refreshPanel`) | Live |
| A22 | Now-playing card; /menu panel | Resume | `pl:player:resume:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.resume` | toast `Resumed at M:SS.` + card refresh | Live |
| A23 | Now-playing card | Replay / Loop ×n | `pl:player:replay:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.seek` to 0 | toast `Track replayed by {user}.` + card refresh | Live |
| A24 | Now-playing card; /menu panel | Skip | `pl:player:skip:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.skip` | toast `Skipped by {user}` + card refresh; empty queue → toast `Playback stopped — queue empty.` + edit to stopped card (`kStoppedCard`) | Live |
| A25 | Now-playing card | Stop | `pl:player:stop:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.stop` | Playing normally: toast `Playback stopped.` + edit to stopped card (`kStoppedCard`); paused: edit to stop-confirm card (`kStopConfirmPaused`) + `stopConfirmKeyboard()`; muted: edit to `kStopConfirmMuted` + `stopConfirmKeyboard()` | Live |
| A26 | Now-playing card | -15s | `pl:player:seekback15:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.seek` −15s | toast `Sought back 15 seconds` + card refresh; near end → alert `Cannot seek forward 15 seconds — about to reach end.` | Live |
| A27 | Now-playing card | +15s | `pl:player:seek15:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.seek` +15s | toast `Sought forward 15 seconds` + card refresh; near end → alert `Cannot seek forward 15 seconds — about to reach end.` | Live |
| A28 | Now-playing card | Autoplay: Enabled / Disabled | `pl:player:autoplay:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.autoplay` | toast `Autoplay enabled.` or `Autoplay disabled.` + card refresh; backend refusal rolls the toggle back and answers the backend error as an alert | Live |
| A29 | /menu panel | Shuffle | `pl:player:shuffle:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.shuffle` | toast `Queue shuffled.` + panel refresh (`refreshPanel`); nothing playing → alert `Nothing playing right now.` | Live |
| A30 | /menu panel | Mute | `pl:player:mute:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.mute` muted=true | toast `Muted.` + panel refresh; nothing playing → alert `Nothing playing right now.` | Live |
| A31 | /menu panel | Unmute | `pl:player:unmute:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.mute` muted=false | toast `Unmuted.` + panel refresh; nothing playing → alert `Nothing playing right now.` | Live |
| A32 | /menu panel | Vol - | `pl:player:voldec:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.volume` −10 (0–200) | toast `Volume set to N.` + panel refresh | Live |
| A33 | /menu panel | Vol + | `pl:player:volinc:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.volume` +10 (0–200) | toast `Volume set to N.` + panel refresh | Live |
| A34 | /menu panel | Refresh | `pl:player:refresh:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | `vc.now_playing` (read) | toast `Updated.` + panel refresh; nothing playing → alert `Nothing playing right now.` | Live |
| A35 | n/a — no button emits it | n/a | `pl:player:thumb:<chat_id>` | `handleCallback` → `cbPlayer` → `playerAction` | none (core chat field `thumb`) | toast `Thumbnails enabled.` or `Thumbnails disabled.` + card refresh | No button |
| A36 | Queued card | Play Now | `pl:player:playnow:<token>` | `handleCallback` → `cbPlayer` | `vc.stop` then `vc.play` (interrupt) | toast `Track skipped.` + card refresh; expired token → alert `This button has expired.`; empty track → alert `Invalid request.` | Live |
| A37 | Now-playing card, row 5 | Add to Playlist | `pl:player:addlist:<token>` | `handleCallback` → `cbPlayer` | none (local playlist store) | toast `Added to your playlist.`; already present → toast `Already in your playlist.`; full → alert `Your playlist is full.`; expired token → alert `This button has expired.` | Live |
| A38 | Stop-confirm card (paused) | Resume | `pl:player:confirm:resume:<chat_id>` | `handleCallback` → `cbPlayer` | `vc.resume` | toast `Resumed at 0:00.` + card refresh; failure answers the error as an alert | Live |
| A39 | Stop-confirm card (muted) | Unmute | `pl:player:confirm:unmute:<chat_id>` | `handleCallback` → `cbPlayer` | `vc.mute` muted=false | toast `Unmuted.` + card refresh; failure answers the error as an alert | Live |
| A40 | Stop-confirm card | Stop | `pl:player:confirm:stop:<chat_id>` | `handleCallback` → `cbPlayer` | `vc.stop` | toast `Playback stopped.` + edit to stopped card (`kStoppedCard`, no keyboard); failure answers the error as an alert | Live |
| A41 | Queue card | Show N more | `pl:queue:more:<chat_id>` | `handleCallback` → `cbQueue` | `vc.queue` list (via `buildQueueCard`) | silent ack + edit to full queue card; nothing playing → alert `Nothing playing right now.` | Live |
| A42 | Queue card, one row per track | ⨯ <title> | `pl:queue:del:<chat_id>:<token>` | `handleCallback` → `cbQueue` | `vc.queue` remove | toast `Removed from queue.` + edit to rebuilt queue card; expired token → alert `This button has expired.`; backend error answers the error as an alert | Live |
| A43 | Settings panel | Back (sub-screens, language picker origin `settings`) | `pl:set:main` | `handleCallback` → `cbSet` | `chat.settings` read | silent ack + edit to settings panel (`kSettingsText`) with `settingsKeyboard()` | Live |
| A44 | Settings panel | Admins / Auth Users / Everyone (value button) | `pl:set:adminmode` | `handleCallback` → `cbSet` | `chat.settings` write | toast `Settings updated!` + edit to settings panel with updated value | Live |
| A45 | Settings panel | Admins / Everyone (value button) | `pl:set:playmode` | `handleCallback` → `cbSet` | `chat.settings` write | toast `Settings updated!` + edit to settings panel with updated value | Live |
| A46 | Settings panel | Enabled / Disabled (value button) | `pl:set:cmddelete` | `handleCallback` → `cbSet` | `chat.settings` write | toast `Settings updated!` + edit to settings panel with updated value | Live |
| A47 | Settings panel | Enabled / Disabled (value button) | `pl:set:cleanmode` | `handleCallback` → `cbSet` | `chat.settings` write | toast `Settings updated!` + edit to settings panel with updated value | Live |
| A48 | Settings panel | 15m / 30m / 60m / 5m (value button) | `pl:set:cleanduration` | `handleCallback` → `cbSet` | `chat.settings` write | toast `Settings updated!` + edit to settings panel with cycled delay (15 → 30 → 60 → 5) | Live |
| A49 | Settings panel | Enabled / Disabled (value button) | `pl:set:nothumb` | `handleCallback` → `cbSet` | `chat.settings` write | toast `Settings updated!` + edit to settings panel with updated value | Live |
| A50 | Settings panel; autoplay card | Enabled / Disabled (value button); Turn ON / Turn OFF | `pl:set:autoplay` | `handleCallback` → `cbSet` | `chat.settings` write + `vc.autoplay` (panel toggle path only) | toast `Settings updated!` + edit to settings panel with updated value | Live |
| A51 | Settings panel | Enabled / Disabled (value button) | `pl:set:silent` | `handleCallback` → `cbSet` | `chat.settings` write | toast `Settings updated!` + edit to settings panel with updated value | Live |
| A52 | Settings panel | Auto-delete: Enabled / Disabled | `pl:set:autodelete` | `handleCallback` → `cbSet` | `chat.settings` write | toast `Settings updated!` + edit to settings panel with updated value | Live |
| A53 | Settings panel | Label buttons: Admin Mode, Play Mode, Cmd Delete, Clean Mode, Clean Delay, Thumbnails, Autoplay, Language, Auto-delete, Delete after, Silent mode, Pagination, Music Download | `pl:set:info:<key>` (13 keys: `adminmode`, `playmode`, `cmddelete`, `cleanmode`, `cleanduration`, `nothumb`, `autoplay`, `silent`, `autodelete`, `timer`, `lang`, `page`, `musicdl`) | `handleCallback` → `cbSet` | none | alert with the explainer text for that key (`settingInfoText`); unknown key → alert `Unknown action.`; message is not edited | Live |
| A54 | Settings panel, timer row | 2 / 3 / 4 / 5 / 6 / 7 (current marked) | `pl:set:timer:<n>` (n 2–7) | `handleCallback` → `cbSet` | `chat.settings` write | toast `Settings updated!` + edit to settings panel with updated timer; out-of-range n → alert `Unknown action.` | Live |
| A55 | Settings panel, index row | Pagination | `pl:set:page` | `handleCallback` → `cbSet` | `chat.settings` read | silent ack + edit to `kSettingsPageScreen` with `settingsToggleKeyboard("page", …)` | Live |
| A56 | Pagination sub-screen | Enabled / Disabled (toggle button) | `pl:set:page:0` / `pl:set:page:1` | `handleCallback` → `cbSet` | `chat.settings` write | toast `Settings updated!` + edit to `kSettingsPageScreen` with updated toggle | Live |
| A57 | Settings panel, index row | Music Download | `pl:set:musicdl` | `handleCallback` → `cbSet` | `chat.settings` read | silent ack + edit to `kSettingsMusicDlScreen` with `settingsToggleKeyboard("musicdl", …)` | Live |
| A58 | Music Download sub-screen | Enabled / Disabled (toggle button) | `pl:set:musicdl:0` / `pl:set:musicdl:1` | `handleCallback` → `cbSet` | `chat.settings` write | toast `Settings updated!` + edit to `kSettingsMusicDlScreen` with updated toggle | Live |
| A59 | Settings panel (Language value button); start card (`Language`) | Language / en | `pl:lang:select` | `handleCallback` → `cbLang` | none | silent ack + edit to language picker (`kLangPicker`) with `langPickerKeyboard("start")` | Live |
| A60 | Language picker | Back (origin `help` / `settings`) | `pl:lang:select:help` / `pl:lang:select:settings` | `handleCallback` → `cbLang` | none | silent ack + edit to language picker with `langPickerKeyboard(<origin>)`; the picker's Back button then returns to the origin menu (A1 / A43) | Live |
| A61 | Language picker | English | `pl:lang:en` / `pl:lang:en:<origin>` | `handleCallback` → `cbLang` | none | toast `You're already using this language.` (already English) or `Language successfully updated to English.` + edit back to the origin menu: start card (`start`, default), help menu (`help`), or settings panel (`settings`) | Live |
| A62 | n/a — no button emits it | n/a | `pl:lang:<code>` (any code other than `en`) | `handleCallback` → `cbLang` | none | alert `This feature is not available yet.`; Parlay is English-only, the picker offers English alone | No button |
| A63 | Download progress message; lossy-fallback prompt | Cancel Downloading / Cancel | `pl:dl:cancel:<run_id>` | `handleCallback` → `cbDl` | none — no `download.cancel` action exists in `contracts/ipc.json` | alert `This feature is not available yet.`; no run recorded → toast `Nothing to cancel — already finished.` (not an alert) | Phase 3 |
| A64 | Audio-effect picker | Original | `pl:dl:fx:<run_id>:orig` | `handleCallback` → `cbDl` | `fx.apply` effect `original` | toast `Downloading and applying effect...`, then the processed audio file is sent as a new message (`applied:true` — original is a no-op by definition); backend error answers the error as an alert; no run → toast `Nothing to cancel — already finished.` | Live |
| A65 | Audio-effect picker | Slow + Reverb | `pl:dl:fx:<run_id>:slow` | `handleCallback` → `cbDl` | `fx.apply` effect `slow-reverb` | toast `Downloading and applying effect...`, then alert `slow-reverb recorded — live DSP filtering lands in Phase 5. The file is unprocessed, so nothing was sent.` (`applied:false`, record-only until Phase 5 — the returned path is the unprocessed input and is never sent as processed audio); backend error answers the error as an alert | Live |
| A66 | Audio-effect picker | Nightcore | `pl:dl:fx:<run_id>:nightcore` | `handleCallback` → `cbDl` | `fx.apply` effect `nightcore` | toast `Downloading and applying effect...`, then alert `nightcore recorded — live DSP filtering lands in Phase 5. The file is unprocessed, so nothing was sent.` (`applied:false`, record-only until Phase 5 — no file sent); backend error answers the error as an alert | Live |
| A67 | Audio-effect picker | Bass Boost | `pl:dl:fx:<run_id>:bass` | `handleCallback` → `cbDl` | `fx.apply` effect `bass-boost` | toast `Downloading and applying effect...`, then alert `bass-boost recorded — live DSP filtering lands in Phase 5. The file is unprocessed, so nothing was sent.` (`applied:false`, record-only until Phase 5 — no file sent); backend error answers the error as an alert | Live |
| A68 | Audio-effect picker | 8D Audio | `pl:dl:fx:<run_id>:8d` | `handleCallback` → `cbDl` | `fx.apply` effect `8d` | toast `Downloading and applying effect...`, then alert `8d recorded — live DSP filtering lands in Phase 5. The file is unprocessed, so nothing was sent.` (`applied:false`, record-only until Phase 5 — no file sent); backend error answers the error as an alert | Live |
| A69 | Spotify download option picker | Send as ZIP | `pl:dl:zip:<run_id>` | `handleCallback` → `cbDl` | `download.spotify_playlist` mode `zip` | toast `Fetching song info...`, then the ZIP is delivered as a new message; expired pick → alert `This button has expired.`; wrong chat → alert `Only the chat that requested this can cancel it.` | Live |
| A70 | Spotify download option picker | Send one by one | `pl:dl:1by1:<run_id>` | `handleCallback` → `cbDl` | `download.spotify_playlist` mode `files` | toast `Fetching song info...`, then the tracks are delivered one by one as new messages; expired pick → alert `This button has expired.`; wrong chat → alert `Only the chat that requested this can cancel it.` | Live |
| A71 | Broadcast progress card | Cancel Broadcast | `pl:bcast:cancel` | `handleCallback` → `cbBcast` | none | Owner, broadcast running: toast `Broadcast cancelled successfully.` and the fan-out stops between targets; owner, nothing running: toast `Broadcast Cancelled`; non-owner: alert `Only the bot owner can use this command.` | Live |
| A72 | Authorized-users list card | ⨯ <name> | `pl:auth:del:<chat_id>:<user_id>` | `handleCallback` → `cbAuth` | none (local auth store) | toast `Authorization revoked.` + edit to rebuilt auth list (`authlistText` + `authlistKeyboard()`); non-admin tap → alert `Only admins can do these actions.` | Live |
| A73 | Search results card | 1 … 10 (number buttons) | `pl:search:<n>:<user_id>:<token>` | `handleCallback` → `cbSearch` → `playSearchPick` | `vc.play` (via `/play` on the picked track) | toast `Success`; the search message is deleted and the now-playing card is sent as a new message; wrong user → alert `You aren't allowed`; expired session → alert `This button has expired.` + edit to `Search expired, please search again`; token mismatch → alert `This button has expired.` + edit to expired text | Live |
| A74 | Search results card | Previous page / Next page | `pl:search:page:<user_id>:<page>:<hash>` | `handleCallback` → `cbSearch` | none (cached search session, no new IPC) | silent ack + edit to the requested results page (`searchResultsText` + `searchResultsKeyboard` + `searchPageKeyboard`); out of range → alert `No more results.`; wrong user → alert `You aren't allowed`; expired session or hash mismatch → alert `This button has expired.` | Live |
| A75 | Voting queue card (queue-rust service, daslui §23 — no in-core builder) | Approve | `pl:vote:approve:<track_id>` | `handleCallback` → `cbVote` | `queue.vote` approve | edit to `voteAcceptedText` card (`{voter} has added to queue:`), keyboard removed, then toast `Vote recorded.`; backend failure → edit to `Failed to queue track: {error}` card + alert with the same text; unknown vote (none open / malformed) → alert `Unknown action.`; foreign-chat vote → alert `You aren't allowed` | Live |
| A76 | Voting queue card (queue-rust service, daslui §23 — no in-core builder) | Decline | `pl:vote:decline:<track_id>` | `handleCallback` → `cbVote` | none | edit to `voteDeclinedText` card (`{voter} declined.`), keyboard removed, then toast `Vote recorded.`; unknown vote (none open / malformed) → alert `Unknown action.`; foreign-chat vote → alert `You aren't allowed` | Live |
| A77 | Share help card | My Link | `pl:share:mylink` | `handleCallback` → `cbShare` | none (local share-link store) | silent ack + edit to the stored share link text (keyboard removed); no link stored → alert `You haven't shared anything yet — use /share first.` | Live |
| A78 | n/a — no button emits it (the share send prompt's Cancel button is `pl:close`, row A19) | n/a | `pl:share:cancel` | `handleCallback` → `cbShare` | none | message deleted + silent ack (empty toast) | No button |
| A79 | Received shared-audio card | Feedback | `pl:share:feedback:<sender>:<msg>:<file_unique>` | `handleCallback` → `cbShare` | none | silent ack + edit to `Send Feedback Text` prompt (keyboard removed); the user's next DM text is forwarded to the sender as feedback; blocked user → alert `You can't send Feedback\nYou are blocked` | Live |
| A80 | Received shared-audio card | Report User | `pl:share:report:<sender>:<msg>` | `handleCallback` → `cbShare` | none (local blocklist) | toast `User is reported and blocked`; reporting twice re-blocks (idempotent) and answers the same toast | Live |
| A81 | Library delete confirm card | Delete | `pl:del:yes:<id>` | `handleCallback` → `cbDel` | none — no `library.delete` action exists in `contracts/ipc.json` | alert `This feature is not available yet.`; the message stays, nothing is deleted | Phase 3 |
| A82 | Library delete confirm card | Cancel | `pl:del:no:<id>` | `handleCallback` → `cbDel` | none | toast `Cancelled.` + message deleted | Live |
| A83 | Lossy-fallback prompt | Accept <summary> | `pl:lossy:accept:<id>` | `handleCallback` → `cbLossy` | none (delivers the stored lossy file) | silent ack + stored audio file sent as a new message + prompt message deleted; expired run → alert `Prompt expired.`; send failure answers the error as an alert | Live |
| A84 | Lossy-fallback prompt | Skip | `pl:lossy:skip:<id>` | `handleCallback` → `cbLossy` | none | toast `Skipped.` (or `Skipped — FLAC not available for {artist} — {title}` when artist/title are known) + prompt message deleted | Live |
| A85 | Restart confirm card | Restart the Bot | `pl:restart:bot:<chat_id>` | `handleCallback` → `cbRestart` | none | toast `Restarting the bot...`, then the process exits with code 42 and the supervisor restarts the core; non-owner → alert `Only the bot owner can use this command.` | Live |
| A86 | Restart confirm card | Replay the Song | `pl:restart:replay:<chat_id>` | `handleCallback` → `cbRestart` | `vc.seek` to 0 | toast `Track replayed by {user}.` + card refresh; non-owner → alert `Only the bot owner can use this command.`; failure answers the error as an alert | Live |
| A87 | Download-settings card | Output Format | `pl:dlset:of` | `handleCallback` → `cbDlset` | none (per-user prefs in local store) | silent ack + edit to `Current Output Format: <format>` with `dlSettingsFormatKeyboard()` | Live |
| A88 | Download-settings card | Use Youtube | `pl:dlset:uy` | `handleCallback` → `cbDlset` | none (per-user prefs in local store) | silent ack + edit to `Is Using Youtube: <True/False>` with `dlSettingsYoutubeKeyboard()` | Live |
| A89 | Download-settings card | Path Template | `pl:dlset:pt` | `handleCallback` → `cbDlset` | none (per-user prefs in local store) | silent ack + edit to the path-template prompt (`kDlSettingsTemplatePrompt`, no keyboard); the user's next DM text is saved as the template and the settings card is re-sent | Live |
| A90 | Download-settings format screen | MP3 / M4A / FLAC / OPUS / OGG / WAV | `pl:dlset:fmt:<fmt>` (`mp3`, `m4a`, `flac`, `opus`, `ogg`, `wav`) | `handleCallback` → `cbDlset` | none (per-user prefs in local store) | toast `Settings updated!` + edit to the format screen with the new format; unknown format → alert `Unknown action.` | Live |
| A91 | Download-settings YouTube screen | True / False | `pl:dlset:uy:1` / `pl:dlset:uy:0` | `handleCallback` → `cbDlset` | none (per-user prefs in local store) | toast `Settings updated!` + edit to the YouTube screen with the new value | Live |
| A92 | Download-settings sub-screens | Back | `pl:dlset:back` | `handleCallback` → `cbDlset` | none | silent ack + edit to `Select one of the settings below!` with `dlSettingsIndexKeyboard()` | Live |
| A93 | Spotify companion card (spotify-rust service — no in-core builder) | Dislike | `pl:spotify:dislike:<id>` | `handleCallback` → `cbSpotify` | `spotify.dislike` | toast `Disliked.`; backend error answers the error as an alert | Live |
| A94 | Spotify companion cards (spotify-rust service — no in-core builder) | Details / Analyze (companion surfaces) | `pl:spotify:details:<id>`, `pl:spotify:analyze:<id>` | `handleCallback` → `cbSpotify` | `spotify.details` (details) / `spotify.analyze` (analyze) | details: card edited in place with track, album, features (key/mode/tempo), disliked/ignored counts, genres, profanity, language, lyrics + `[Analyze text]` button; analyze: analysis card edited in place; any other verb → alert `This feature is not available yet.` | Live |
| A95 | Any message with an inline keyboard | Any button whose `callback_data` is not in rows A1–A94, any data not starting with `pl:`, empty data, or data over 64 bytes | `pl:<unknown-domain>:…` / non-`pl:` data / empty / >64 bytes | `handleCallback` default branch (explicit `else`) | none | alert `Unknown action.` (`kToastUnknownAction`); a handler that throws is answered with the alert `This feature is not available yet.` (`kToastFeatureUnavailable`) | Live |

## §B. Reply-keyboard buttons and non-callback buttons

### §B.1 Persistent reply keyboard (`persistentKeyboard()`, §14)

Sent by `/keyboard` (and removed by `/keyboard off`). A tap sends the
label as plain message text — there is no `callback_data`. Routing is
therefore `handleMessage`'s plain-text path, not `handleCallback`.

| # | Surface | Button / input | Emits | Router / handler | Backend action | Rendered update | Status |
|---|---|---|---|---|---|---|---|
| B1 | Persistent reply keyboard (any chat) | Music | plain text `Music` | `handleMessage` plain-text path | none | DM: new message with the start card (`kStartPm` + `startPmKeyboard()`); group: new message with the start card (`kStartPm` + `startPmKeyboard()`) — the music hub | Live |
| B2 | Persistent reply keyboard (any chat) | Global Search | plain text `Global Search` | `handleMessage` plain-text path | none | DM: new message with the start card (`kStartPm` + `startPmKeyboard()`); group: new message with the search prompt (`What do you want to search for?`) — reply with a query to search | Live |
| B3 | Persistent reply keyboard (any chat) | Settings | plain text `Settings` | `handleMessage` plain-text path | none | DM: new message with the start card (`kStartPm` + `startPmKeyboard()`); group: new message with the settings panel (`kSettingsText` + `settingsKeyboard()`); non-admin → admin denial (`kDenyAdmin`) | Live |
| B4 | Persistent reply keyboard (any chat) | About | plain text `About` | `handleMessage` plain-text path | none | DM: new message with the start card (`kStartPm` + `startPmKeyboard()`); group: new message with the about card (`kAbout`, channel + repo) | Live |

### §B.2 URL buttons and switch-inline buttons (no `callback_data`)

| # | Surface | Button / input | Emits | Router / handler | Backend action | Rendered update | Status |
|---|---|---|---|---|---|---|---|
| B5 | Start card (DM) | Add Me to Your Group | URL `https://t.me/<bot>?startgroup&admin=invite_users` | none — Telegram client opens the link | none | Telegram opens the add-to-group flow; no bot reply | Live |
| B6 | Start card (DM); group start card (bot-added greeting) | Updates | URL `cfg.updates_url` (omitted when unconfigured) | none — Telegram client opens the link | none | Telegram opens the updates channel; no bot reply | Live |
| B7 | Start card (DM); group start card (bot-added greeting) | Support | URL `cfg.support_url` (omitted when unconfigured) | none — Telegram client opens the link | none | Telegram opens the support chat; no bot reply | Live |
| B8 | Start card (DM) | Source Code | URL `cfg.source_url` (omitted when unconfigured) | none — Telegram client opens the link | none | Telegram opens the source repository; no bot reply | Live |
| B9 | Start card (DM) | Search | `switch_inline_query` `` (empty, current chat) | `handleInlineQuery` | `search.inline` | inline results for the typed query (see §D); empty query → hint `np  s  l  lib  del — or type to search` | Live |
| B10 | Start card (DM) | Library | `switch_inline_query` `lib ` (current chat) | `handleInlineQuery` | none (no library backend) | inline answer: empty results (lib has no backend — ruling 1, future work) | Live |
| B11 | Start card (DM) | Delete | `switch_inline_query` `del ` (current chat) | `handleInlineQuery` | none (no library backend) | inline answer: empty results (del has no backend — ruling 1, future work) | Live |
| B12 | Start card (DM) | Playing | `switch_inline_query` `np` (current chat) | `handleInlineQuery` | `search.inline` | inline answer: one audio/article result for the current track (see §D, row D4) | Live |
| B13 | Start card (DM) | Lyrics | `switch_inline_query` `l` (current chat) | `handleInlineQuery` | `search.inline` + `vc.lyrics` (POST) | inline answer: one lyrics article for the current track (see §D, row D6) | Live |
| B14 | Start card (DM) | Share | `switch_inline_query` `s` (current chat) | `handleInlineQuery` | `search.inline` | inline answer: one share article for the current track (see §D, row D5) | Live |
| B15 | Start card (DM) | Send playing to… | `switch_inline_query` `np` (choose chat) | `handleInlineQuery` | `search.inline` | inline answer: one audio/article result for the current track, sent to the chosen chat (see §D, row D4) | Live |

## §C. Commands

Scope / permission are from `Dispatcher::registerCommands`
(`services/core-cpp/src/dispatcher.cpp`): `Scope` Group / Private / Any,
`Perm` Everyone / Sudo / Owner. Gates that fail do so silently (NoxBeats
dispatch rule) except where the row says otherwise. Unknown commands are
never silent: the §C90 row answers with the unknown-command card.
`Backend action` names the contract action from `contracts/ipc.json`
where the handler calls the backend; `none` means the handler answers
from the core's local store / state only.

| # | Surface | Button / input | Emits | Router / handler | Backend action | Rendered update | Status |
|---|---|---|---|---|---|---|---|
| C1 | Group chat | /play, /playforce, /vplay, /vplayforce | `/play [song/link]`, `/playforce`, `/vplay`, `/vplayforce` | `handleMessage` → `onPlay` (Group, Everyone) | `vc.play`; `vc.play_file` (replied media); `vc.queue` (queued add); `vc.stop` (playnow interrupt) | new message: now-playing card with `playerKeyboard()` (or queued card with `queuedKeyboard()` when queued); no query → usage card `Whoops! Nothing to play.`; over duration limit → `Sorry, song exceeds max duration of N minutes.`; replied media → file downloaded via the TDLib bot session, then `vc.play_file` with the local path + `file_id`; backend refusal answers the exact error, never a fake Now playing | Live |
| C2 | Group chat | /playplaylist | `/playplaylist` | `handleMessage` → `onPlayPlaylist` (Group, Everyone) | `vc.play`, `vc.queue` | new message: `Playing your playlist: …` + now-playing card; empty playlist → `Your playlist is empty.` card | Live |
| C3 | Group chat | /skip, /next | `/skip`, `/next` | `handleMessage` → `onSkip` (Group, Everyone) | `vc.skip` | new message: state card `Skipped.`; nothing playing → `Nothing is playing.`; queue now empty → `Skipped — the queue is now empty.` | Live |
| C4 | Group chat | /autoplay-skip, /askip | `/autoplay-skip`, `/askip` | `handleMessage` → `onAutoplaySkip` (Group, Everyone) | `vc.autoplay_skip` | new message: next autoplay track card; nothing playing → `Nothing is playing.`; queue now empty → `Autoplay skipped — the queue is now empty.` | Live |
| C5 | Group chat | /pause | `/pause` | `handleMessage` → `onPause` (Group, Everyone) | `vc.pause` | new message: state card `Paused` (`stateCardText`); nothing playing → `Nothing is playing.` | Live |
| C6 | Group chat | /resume | `/resume` | `handleMessage` → `onResume` (Group, Everyone) | `vc.resume` | new message: state card `Resumed` (`stateCardText`); nothing playing → `Nothing is playing.` | Live |
| C7 | Group chat | /end, /stop | `/end`, `/stop` | `handleMessage` → `onStop` (Group, Everyone) | `vc.stop` | new message: stopped card; not streaming → `Not streaming.` | Live |
| C8 | Group chat | /loop | `/loop <1-10\|off>` | `handleMessage` → `onLoop` (Group, Everyone) | `vc.loop` with count 0–10 (0 = off); count also kept in the core chat field | new message: `Turned loop on.` / `Turned loop off.` / current state or `Usage: /loop <1-10|off>`; not streaming → `Not streaming.`; no queue copies are enqueued (the old copy-queueing path would have looped 2N times) | Live |
| C9 | Group chat | /seek, /seekback | `/seek <secs>`, `/seekback <secs>` | `handleMessage` → `onSeek` (Group, Everyone) | `vc.seek` | new message: seek confirmation; nothing playing → `Nothing is playing.`; paused → `Playback is paused.`; under 10s → `Seek at least 10 seconds.`; no known duration → `This track has no known duration, cannot seek.` | Live |
| C10 | Group chat | /queue, /playing | `/queue`, `/playing`, `/queue help` | `handleMessage` → `onQueue` (Group, Everyone) | `vc.queue` list (via `renderQueueCard`) | new message: queue card (`queueText` + `queueKeyboard()`); `/queue help` → queue-ops help text | Live |
| C11 | Group chat | /clear | `/clear` | `handleMessage` → `onClear` (Group, Everyone) | `vc.queue` clear | new message: `Cleared N queued tracks.`; already empty → `The queue is already empty.`; not streaming → `Not streaming.` | Live |
| C12 | Group chat | /restore | `/restore` | `handleMessage` → `onRestore` (Group, Everyone) | `vc.queue` list/add | new message: `Queue restored.`; nothing to restore → `There is no cleared queue to restore.`; backend error → `Could not restore the queue (engine error).` | Live |
| C13 | Group chat | /remove | `/remove [position]` | `handleMessage` → `onRemove` (Group, Everyone) | `vc.queue` remove (backend position = display position − 1; the waiting list is 0-based, the current track is never in it) | new message: `Removed #N: <title>` or `Removed N queued tracks.`; bad position → `That position is not in the queue.`; no argument → usage `Usage: /remove [position]` | Live |
| C14 | Group chat | /move | `/move [from] [to]` | `handleMessage` → `onMove` (Group, Everyone) | `vc.queue` remove/add (backend positions = display positions − 1) | new message: `Moved #from to #to.`; bad position → `That position is not in the queue.`; moving display #1 is allowed (it is the first waiting track, not the current track); no arguments → usage `Usage: /move [from] [to]` | Live |
| C15 | Group chat | /shuffle | `/shuffle` | `handleMessage` → `onShuffle` (Group, Everyone) | `vc.queue` list/remove/add (reordered; removes are 0-based positions from the end) | new message: `Shuffled N queued tracks.`; fewer than two waiting tracks → `Need at least two waiting tracks to shuffle.` (two waiting tracks are enough — the list is waiting-only); not streaming → `Not streaming.` | Live |
| C16 | Group chat | /pos | `/pos` | `handleMessage` → `onPos` (Group, Everyone) | `vc.now_playing` (read) | new message: position card (`kPositionText`: title, position/duration, speed); nothing playing → `Not streaming.` | Live |
| C17 | Group chat | /jump | `/jump <mm:ss>` | `handleMessage` → `onJump` (Group, Everyone) | `vc.seek` | new message: jump confirmation; nothing playing → `Nothing is playing.`; paused → `Playback is paused.`; past the end → `That is past the end of the track.`; no known duration → `This track has no known duration, cannot jump.`; no argument → usage `Usage: /jump <mm:ss>` | Live |
| C18 | Group chat | /speed | `/speed [0.5-4.0]` | `handleMessage` → `onSpeed` (Group, Everyone) | `vc.seek` (stream restarts at the current position, unchanged); stored speed rides the next `vc.play` body into the backend room (§4.4) | new message: current speed, or `Speed Nx saved, but the backend cannot apply audio filters yet — the stream restarted at the current position at the old speed. Live filtering lands with the Phase 5 backend DSP.`; out of range → `Speed must be 0.5x to 4.0x.`; nothing playing → `Nothing is playing.`; paused → `Playback is paused.` | Live |
| C19 | Group chat | /bass | `/bass [0-100]` | `handleMessage` → `onBass` (Group, Everyone) | `vc.seek` (stream restarts at the current position, unchanged); stored bass rides the next `vc.play` body into the backend room (§4.4) | new message: current bass, or `Bass N saved, but the backend cannot apply audio filters yet — the stream restarted at the current position unchanged. Live filtering lands with the Phase 5 backend DSP.`; out of range → `Bass must be 0-100.`; nothing playing → `Nothing is playing.`; paused → `Playback is paused.` | Live |
| C20 | Group chat | /mute, /unmute | `/mute`, `/unmute` | `handleMessage` → `onMute` (Group, Everyone) | `vc.mute` (via `muteVoice`) | new message: `Muted.` / `Unmuted.`; already in that state → `Already muted.` / `Not muted.`; nothing playing → `Nothing is playing.`; other failure → `Could not mute/unmute: <reason>` | Live |
| C21 | Group chat | /replay | `/replay` | `handleMessage` → `onReplay` (Group, Everyone) | `vc.seek` to 0 | new message: replay confirmation; not streaming → `Not streaming.` | Live |
| C22 | Group chat | /player | `/player` | `handleMessage` → `onPlayer` (Group, Everyone) | `vc.now_playing` (read) | new message: current now-playing card re-sent (old card deleted); nothing playing → `Not streaming.` | Live |
| C23 | Any chat | /lyrics, /lyric | `/lyrics` | `handleMessage` → `onLyrics` (Any, Everyone) | `netease.search`, `netease.lyrics` | new message: lyrics card (`kLyricsCard`); not streaming → `Not streaming.`; not found → `Lyrics not found.`; fetch failure → `Failed to get lyrics — the song may not exist or may be instrumental` | Live |
| C24 | Any chat | /search | `/search <query>` | `handleMessage` → `onSearch` (Any, Everyone) | `search.query`; `netease.search` (fallback) | new message: search results card (`searchHeaderText` + result lines) with number buttons (`searchKeyboard()`) and page buttons (`searchPageKeyboard()`); no query → `What do you want to search for?`; no results → `No results found.`; too frequent → `Too frequent, please try again in N seconds` | Live |
| C25 | Any chat | /download | `/download <song title or link>` | `handleMessage` → `onDownload` (Any, Everyone) | `download.youtube` / `download.spotify` / `download.spotify_playlist` / `download.deezer` / `download.tiktok` / `download.navidrome` / `download.soulseek` (by link type) | new message: progress message with Cancel button (`pl:dl:cancel`, row A63), then the audio file as a new message; Spotify collection → option picker (`Select Options Below!` + `dlSpotifyOptionsKeyboard()`); lossy-only FLAC → lossy prompt (`lossyKeyboard()`); finished download → effect picker (`dlFxKeyboard()`); no argument → usage `Usage: /download <song title or link>`; failure → `Download failed: <error>` | Live |
| C26 | Any chat | /radio | `/radio <station>` | `handleMessage` → `onRadio` (Any, Everyone) | `radio.start`; `vc.play` (VC handoff) | new message: radio now-playing card (`radioNowPlayingText`); no stream URL → `Radio service returned no stream URL.` | Live |
| C27 | Any chat | /share | `/share <song title or link>` / `/share <@user or id>` / reply to media | `handleMessage` → `onShare` (Any, Everyone) | `share.create` | new message: share link card with `shareHelpKeyboard()` (Help / My Link); user target → send prompt (`Please send the music that you want to share with {name}`) with Cancel button (`pl:close`); forwarded media → `Music Has Been Sent` to the sender and the share card to the target; blocked → `You can't send music to {name}\nYou are blocked`; self-share → `Dude?! You have schizophrenia\nWhy you send music to yourself`; no argument → usage `Usage: /share <song title or link>` | Live |
| C28 | Any chat | /engine | `/engine [go\|ts\|rust]` | `handleMessage` → `onEngine` (Any, Everyone) | none — engine selection is retired | new message: `Engine selection is retired — Parlay now runs a single unified backend.`; non-admin attempt to switch → `Only group admins (or the bot owner) can switch engines.` | Live |
| C29 | Any chat | /addplaylist | `/addplaylist <song>` | `handleMessage` → `onAddPlaylist` (Any, Everyone) | `search.query` (resolve) | new message: playlist-added confirmation; duplicate → duplicate notice; unsupported → unsupported notice; nothing playing and no query → `Not streaming.`; no results → `No results found.`; no argument → usage | Live |
| C30 | Any chat | /playlist | `/playlist` | `handleMessage` → `onPlaylist` (Any, Everyone) | none (local playlist store) | new message: playlist card; empty → `Your playlist is empty.` | Live |
| C31 | Any chat | /rmplaylist, /clearplaylist | `/rmplaylist`, `/clearplaylist` | `handleMessage` → `onRmPlaylist` (Any, Everyone) | none (local playlist store) | new message: `Playlist removed.`; nothing to remove → `There is no playlist to remove.`; no argument → usage | Live |
| C32 | Any chat | /createplaylist | `/createplaylist <name>` | `handleMessage` → `onCreatePlaylist` (Any, Everyone) | none (local named-playlist store) | new message: `Playlist <name> created.`; at the limit → limit notice; failure → failure notice; no argument → usage | Live |
| C33 | Any chat | /deleteplaylist | `/deleteplaylist <name or ID>` | `handleMessage` → `onDeletePlaylist` (Any, Everyone) | none (local named-playlist store) | new message: `Playlist <name> deleted.`; not found → `Playlist not found.`; no argument → usage `Usage: /deleteplaylist <name or ID>` | Live |
| C34 | Any chat | /addtoplaylist | `/addtoplaylist <playlist> <song>` | `handleMessage` → `onAddToPlaylist` (Any, Everyone) | `search.query` (resolve) | new message: song-added confirmation; playlist not found → not-found notice; duplicate → duplicate notice; full → full notice; no results → `No results found.`; no argument → usage | Live |
| C35 | Any chat | /removefromplaylist | `/removefromplaylist <playlist> <number>` | `handleMessage` → `onRemoveFromPlaylist` (Any, Everyone) | none (local named-playlist store) | new message: song-removed confirmation; playlist not found → not-found notice; bad number → bad-number notice; song not found → song-not-found notice; no argument → usage | Live |
| C36 | Any chat | /playlistinfo | `/playlistinfo <name or ID>` | `handleMessage` → `onPlaylistInfo` (Any, Everyone) | none (local named-playlist store) | new message: playlist info card (name, id, track list); not found → `Playlist not found.`; no argument → usage `Usage: /playlistinfo <name or ID>` | Live |
| C37 | Any chat | /myplaylists | `/myplaylists` | `handleMessage` → `onMyPlaylists` (Any, Everyone) | none (local named-playlist store) | new message: `My Playlists` card with name + id per playlist; none → `You do not have any playlists yet. Use /createplaylist to make one.` | Live |
| C38 | Group chat | /new | `/new <station link>` | `handleMessage` → `onNewRadio` (Group, Everyone) | `radio.start` | new message: radio-created confirmation; already exists → `A radio already exists in this chat.`; no link → `Please provide a radio station link.`; group-only violation → group-only notice | Live |
| C39 | Group chat | /destroy | `/destroy` | `handleMessage` → `onDestroyRadio` (Group, Everyone) | `radio.stop` | new message: `Radio destroyed.`; no radio → `There is no radio in this chat.`; group-only violation → group-only notice | Live |
| C40 | Group chat | /manage | `/manage` | `handleMessage` → `onManageRadio` (Group, Everyone) | `radio.status` (read) | new message: radio management card (station, status); no radio → `There is no radio in this chat.`; group-only violation → group-only notice | Live |
| C41 | Group chat | /menu | `/menu` | `handleMessage` → `onMenu` (Group, Everyone) | `vc.now_playing` (read) | new message: control panel (`menuPanelText` + `menuPanelKeyboard()`); only the user who ran `/menu` may press its buttons — other users' taps are silently ignored with an empty ack (panel rule, §4) | Live |
| C42 | Any chat | /id | `/id` | `handleMessage` → `onId` (Any, Everyone) | none | new message: `This chat has ID <chat>` (plus thread id when present) | Live |
| C43 | Any chat | /keyboard | `/keyboard`, `/keyboard off` | `handleMessage` → `onKeyboard` (Any, Everyone) | none | new message: `Keyboard enabled.` with the persistent reply keyboard attached (§B.1); `/keyboard off` / `remove` / `hide` → new message `Keyboard removed.` with the keyboard removed | Live |
| C44 | Any chat | /about | `/about` | `handleMessage` → `onAbout` (Any, Everyone) | none | new message: about card (`kAbout`, channel + repo) | Live |
| C45 | Any chat | /status | `/status` | `handleMessage` → `onStatus` (Any, Everyone) | none — the core keeps no song cache; the backend owns the counts (Phase 3) | new message: statistics card with honest zeros (`Total cached songs in database: 0`, chat 0, user 0) | Live |
| C46 | Group chat | /now | `/now` | `handleMessage` → `onNow` (Group, Everyone) | `vc.now_playing` (read) | new message: `Currently streaming <title> by <requester>...`; nothing streaming → `Not streaming.` | Live |
| C47 | Any chat | /repo | `/repo` | `handleMessage` → `onRepo` (Any, Everyone) | none | new message: `Source code: <repo URL>` | Live |
| C48 | Any chat | /download-settings | `/download-settings` | `handleMessage` → `onDownloadSettings` (Any, Everyone) | none (per-user prefs in local store) | new message: `Select one of the settings below!` with `dlSettingsIndexKeyboard()` (Output Format / Use Youtube / Path Template) | Live |
| C49 | Any chat | /start | `/start`, `/start spotify----<id>`, `/start share_<token>` | `handleMessage` → `onStart` (Any, Everyone) | `share.resolve` (share-token deep link only) | DM: new message with the start card (`kStartPm` + `startPmKeyboard()`); group: `Hi! For bot help and commands, please DM me directly — I'm more responsive in private chats!`; `spotify----<id>` deep link → the track is downloaded and sent (see §D, row D12); `share_<token>` → shared-track card or `Oops!\nInvalid link`; blocked user → `You are blocked from using Parlay.` | Live |
| C50 | Any chat | /help | `/help`, `/help <command>`, `/help <topic>` | `handleMessage` → `onHelp` (Any, Everyone) | none | DM: new message with the help menu card (`kHelpMenu` + `helpMenuKeyboard()`); group: `Hi! For bot help and commands, please DM me directly — I'm more responsive in private chats!`; `/help <command>` → new message with that command's help text, or `No help found for command <cmd>`; `/help <topic>` → new message with the topic page | Live |
| C51 | Group chat | /playmode | `/playmode [admin\|everyone]` | `handleMessage` → `onPlaymode` (Group, Everyone) | none (chat field) | new message: play-mode status or `Usage: /playmode admin|everyone (current: …)` | Live |
| C52 | Group chat | /settings | `/settings` | `handleMessage` → `onSettings` (Group, Everyone) | `chat.settings` read | new message: settings panel (`kSettingsText` + `settingsKeyboard()`); DM → `These settings are available only in groups.` | Live |
| C53 | Any chat | /lang, /language | `/lang`, `/language [code]` | `handleMessage` → `onLang` (Any, Everyone) | none | new message: `Current language: …` or `Language set to …`; English-only — any other code is refused with the language card | Live |
| C54 | Group chat | /adminmode | `/adminmode [admin\|adminauth\|everyone]` | `handleMessage` → `onAdminmode` (Group, Everyone) | none (chat field) | new message: `Admin command mode updated to: <status>`; invalid mode → `Invalid mode. Use: admin, adminauth, or everyone.` | Live |
| C55 | Group chat | /cmddelete | `/cmddelete [on\|off]` | `handleMessage` → `onCmdDelete` (Group, Everyone) | none (chat field) | new message: `Cmd delete is <on/off>.` | Live |
| C56 | Group chat | /cleanmode | `/cleanmode [on\|off\|<minutes>]` | `handleMessage` → `onCleanmode` (Group, Everyone) | none (chat field) | new message: clean-mode status card (`kCleanModeStatus`) or `Clean mode off.` / `Clean mode on: bot messages are deleted after Nm`; no argument → usage `Usage: /cleanmode on|off|<minutes 1-1440>` | Live |
| C57 | Group chat | /nothumb, /nothumbs, /thumbnail, /thumb | `/nothumb [on\|off]`, `/thumbnail [on\|off]` | `handleMessage` → `onThumb` (Group, Everyone) | none (chat field) | new message: thumbnail status card (`kThumbStatus`) | Live |
| C58 | Group chat | /autoplay | `/autoplay`, `/autoplay [on\|off\|lang <lang>\|mood <mood>]` | `handleMessage` → `onAutoplay` (Group, Everyone) | `vc.autoplay` (on/off path) | new message: autoplay card (`autoplayCardText` + `autoplayCardKeyboard()`) or status text (`autoplayStatusText`); no argument → usage `Usage: /autoplay on|off|lang <lang>|mood <mood>` | Live |
| C59 | Group chat | /channelplay | `/channelplay [@channel\|id]`, `/channelplay off` | `handleMessage` → `onChannelplay` (Group, Everyone) | none (channel link in local store) | new message: channel-linked confirmation / status / disabled notice; no channel linked → `No channel linked. Use /channelplay first.`; invalid target → invalid-target notice; not accessible → not-accessible notice; no argument → usage | Live |
| C60 | Group chat | /userbot | `/userbot` | `handleMessage` → `onUserbot` (Group, Everyone) | none (assistant session state) | new message: assistant status (already in call / not in this chat yet / session unavailable / restricted); no channel linked → channel-link notice | Live |
| C61 | Group chat | /join | `/join` | `handleMessage` → `onJoin` (Group, Everyone) | `vc.join` (via `joinVoice`, duplex JoinRequest handshake) | new message: `Joined the voice chat.`; already joined → `Already in the voice chat.`; failure → `Could not join the voice chat: <reason>` | Live |
| C62 | Group chat | /auth, /unauth | `/auth [user]`, `/unauth [user]` | `handleMessage` → `onAuth` (Group, Everyone) | none (local auth store) | new message: `Added {user}.\nNow {count}/{limit} authorized users.` / `Removed {user} from the authorized users list.`; already authorized → already notice; at the limit (25) → limit notice; not authorized (unauth) → not-authorized notice; no user → usage | Live |
| C63 | Group chat | /authlist | `/authlist` | `handleMessage` → `onAuthlist` (Group, Everyone) | none (local auth store) | new message: authorized-users list (`authlistText` + `authlistKeyboard()` with ⨯ revoke buttons); empty → `There are no authorized users in this chat.` | Live |
| C64 | Group chat | /admincache, /reload | `/admincache`, `/reload` | `handleMessage` → `onAdmincache` (Group, Everyone) | none (admin cache in local store) | new message: `Administrator cache reloaded successfully.`; within cooldown → `Please wait <mm:ss> before using this command again.` | Live |
| C65 | Any chat | /blacklist, /unblacklist, /whitelist | `/blacklist <user\|chat>`, `/unblacklist <user\|chat>`, `/whitelist <user\|chat>` | `handleMessage` → `onBlacklist` (Any, Sudo; silent denial otherwise) | none (local blocklists) | new message: `User blocked: <id>` / `Chat blocked: <id>` / `Removed <id>` / `Already blacklisted.` / `Not blacklisted.`; unresolvable target → `Cannot resolve that user/chat.`; invalid id → `Invalid id.` | Live |
| C66 | Any chat | /blacklisted | `/blacklisted` | `handleMessage` → `onBlacklisted` (Any, Sudo; silent denial otherwise) | none (local blocklists) | new message: blacklisted users/chats list | Live |
| C67 | Any chat | /addsudo, /delsudo, /rmsudo | `/addsudo <user>`, `/delsudo <user>`, `/rmsudo <user>` | `handleMessage` → `onAddSudo` (Any, Owner; silent denial otherwise) | none (sudo set in local store) | new message: `Added sudo user <id>.` / `Removed sudo user <id>.`; already sudo → already notice; not sudo → not-sudo notice; owner removal attempt → `Nice try! You can't remove the owner — they're untouchable.`; no user → `Reply to a user or give a user id/@username.` | Live |
| C68 | Any chat | /listsudo, /sudolist | `/listsudo`, `/sudolist` | `handleMessage` → `onListSudo` (Any, Everyone) | none (sudo set in local store) | new message: sudoers list (`kSudoHeader`); none extra → `No additional sudoers found.` | Live |
| C69 | Any chat | /ac, /activevc | `/ac`, `/activevc` | `handleMessage` → `onActiveVc` (Any, Sudo; silent denial otherwise) | `vc.now_playing` (read, per active chat) | new message: active-streams list (`kActiveStreamsHeader` + per-chat lines) or `Looks like there aren't any active streams on the bot.` | Live |
| C70 | Any chat | /broadcast | `/broadcast [-pin\|-pinloud\|-user\|-assistant\|-nobot] <message or reply>` | `handleMessage` → `onBroadcast` (Any, Sudo; silent denial otherwise) | none (fan-out is core-side sends) | new message: `Broadcasting to N chats...` + progress card with Cancel Broadcast button (`bcastKeyboard()`, row A71); no message → `Reply to a message to broadcast it.`; finished → per-chat result; failures delivered as an `errors.txt` document | Live |
| C71 | Any chat | /maintenance | `/maintenance [enable\|disable]` | `handleMessage` → `onMaintenance` (Any, Owner; silent denial otherwise) | none (maintenance flag in local store) | new message: maintenance enabled/disabled confirmation; already in that state → already notice | Live |
| C72 | Any chat | /autoleave | `/autoleave [on\|off]` | `handleMessage` → `onAutoleave` (Any, Sudo; silent denial otherwise) | none | new message: autoleave status | Live |
| C73 | Any chat | /json | `/json` (reply to a message) | `handleMessage` → `onJson` (Any, Everyone) | none | new message: the replied-to message as JSON | Live |
| C74 | Any chat | /speedtest | `/speedtest` | `handleMessage` → `onSpeedtest` (Any, Sudo; silent denial otherwise) | none — network probes are not implemented | new message: `Speed test is not available yet — the network probes …` (honest unavailable, no fake numbers) | Live |
| C75 | Any chat | /bug | `/bug <text>` | `handleMessage` → `onBug` (Any, Everyone) | none (forwards to the log chat / owner) | new message: `Thanks, your bug report was sent.`; no log chat / owner configured → `No log chat / owner is configured to receive bug reports.`; send failure → failure notice | Live |
| C76 | Any chat | /privacy | `/privacy` | `handleMessage` → `onPrivacy` (Any, Everyone) | none | new message: privacy policy card (`kPrivacy`) | Live |
| C77 | Any chat | /alive, /ping | `/alive`, `/ping` | `handleMessage` → `onPing` (Any, Everyone) | none (latency measured locally) | new message: pong card (`pingCardText`: latency, uptime, RAM, CPU, disk, ntgcalls latency when known) | Live |
| C78 | Any chat | /stats | `/stats` | `handleMessage` → `onStats` (Any, Everyone) | none (host stats read locally) | new message: statistics card (`kStatsCard`: system, memory, server, bot, versions, library section when present) | Live |
| C79 | Any chat | /eval, /ev, /sh, /exec | `/eval <code>`, `/sh <shell>` | `handleMessage` → `onEval` (Any, Owner; silent denial otherwise) | none (runs in-process / shell) | new message: `Running...` then the output as a new message; no input → `No input given.` | Live |
| C80 | Any chat | /logs | `/logs` | `handleMessage` → `onLogs` (Any, Sudo; silent denial otherwise) | none (reads the local log file) | new message: recent log lines as text, or the log file as a document; missing file → file-missing notice | Live |
| C81 | Any chat | /logger | `/logger [enable\|disable\|on\|off]` | `handleMessage` → `onLogger` (Any, Sudo; silent denial otherwise) | none | new message: logger status / enabled / disabled; no LOGGER_ID configured → `Please set LOGGER_ID in .env first.`; no argument → usage with current status | Live |
| C82 | Any chat | /restart | `/restart` | `handleMessage` → `onRestart` (Any, Sudo; silent denial otherwise) | none | new message: restart-confirm card (`kRestartConfirm` + `restartConfirmKeyboard()`, rows A85–A86) when a song is playing; otherwise `Restarting... (exit 42; the supervisor should bring me back.)` and the process exits 42 | Live |
| C83 | Private chat | /setrtmp | `/setrtmp <rtmp url> <stream key>`, `/setrtmp off` | `handleMessage` → `onSetRtmp` (Private, Everyone) | none (RTMP config in local store) | new message: RTMP saved / removed confirmation; group use → DM-only notice; invalid URL → invalid-URL notice; no argument → usage `Usage: /setrtmp <rtmp url> <stream key>` | Live |
| C84 | Group chat | /stream | `/stream <query>` | `handleMessage` → `onStream` (Group, Everyone) | `download.youtube` (resolve to file, then RTMP push) | new message: RTMP stream-started card; no RTMP set → `No RTMP destination set.`; already running → already-running notice; no file → `Downloader returned no file for that query.`; start failure → start-failed notice; no argument → usage | Live |
| C85 | Group chat | /streamstop | `/streamstop` | `handleMessage` → `onStreamStop` (Group, Everyone) | none | new message: RTMP stopped card; nothing running → `No RTMP stream is running.` | Live |
| C86 | Group chat | /streamstatus | `/streamstatus` | `handleMessage` → `onStreamStatus` (Group, Everyone) | none | new message: RTMP status card; none active → `No RTMP stream is active.` | Live |
| C87 | Any chat | /mix | `/mix` | none — not registered in `registerCommands` | none — no backend action exists | new message: unknown-command card (`kUnknownCommand`: `Command /mix not found.` + the command list for the caller's level) | Phase 3 |
| C88 | Any chat | /recognize | `/recognize` | none — not registered in `registerCommands` | none — no backend action exists | new message: unknown-command card (`kUnknownCommand`: `Command /recognize not found.` + the command list for the caller's level) | Phase 3 |
| C89 | Any chat | /scan | `/scan` | none — not registered in `registerCommands` | none — no backend action exists | new message: unknown-command card (`kUnknownCommand`: `Command /scan not found.` + the command list for the caller's level) | Phase 3 |
| C90 | Any chat | /sharescan | `/sharescan` | none — not registered in `registerCommands` | none — no backend action exists | new message: unknown-command card (`kUnknownCommand`: `Command /sharescan not found.` + the command list for the caller's level) | Phase 3 |
| C91 | Any chat | /rmcache | `/rmcache` | none — not registered in `registerCommands` | none — no backend action exists | new message: unknown-command card (`kUnknownCommand`: `Command /rmcache not found.` + the command list for the caller's level) | Phase 3 |
| C92 | Any chat | Unknown command | `/<anything-not-registered>` | `handleMessage` command loop, no route matched | none | new message: unknown-command card (`kUnknownCommand`: `Command /<name> not found.` + the command list for the caller's level) — never silent, never a crash | Live |
| C93 | Any chat | Aliases (70) | `/p`, `/fplay`, `/forceplay`, `/fvplay`, `/vfplay`, `/q`, `/aplay`, `/setloop`, `/setspeed`, `/speedup`, `/position`, `/addauth`, `/delauth`, `/removeauth`, `/sudoers`, `/listsudo`, `/sudoadd`, `/addsudoer`, `/delsudoer`, `/sudodel`, `/remsudo`, `/sudorem`, `/dropsudo`, `/unsudo`, `/blockuser`, `/blacklistuser`, `/blackuser`, `/bluser`, `/blockchat`, `/blacklistchat`, `/blackchat`, `/blchat`, `/unblockuser`, `/unblacklistuser`, `/unbluser`, `/whitelistuser`, `/unblockchat`, `/unblacklistchat`, `/unblackchat`, `/whitechat`, `/unblchat`, `/blocked`, `/gcast`, `/bcast`, `/active`, `/activevoice`, `/log`, `/bash`, `/shell`, `/nothumbs`, `/commanddelete`, `/bassboost`, `/listplaylist`, `/myplaylist`, `/playlists`, `/mylist`, `/delplaylist`, `/removeplaylist`, `/pplaylist`, `/plplay`, `/playmylist`, `/saveplaylist`, `/addsong`, `/maint`, `/spt`, `/assistant`, `/userbotjoin`, `/assistantjoin`, `/setcplay`, `/rtmp` | `handleMessage` → alias resolution (`aliases_`) → the target command's row (C1–C86) | same as the target command | same rendered update as the target command | Live |
| C94 | Group chat (linked channel) | Channel commands (40) | `/cplay`, `/vcplay`, `/cvplay`, `/cfplay`, `/fcplay`, `/cforceplay`, `/cplayforce`, `/fvcplay`, `/fvplay`, `/vcplayforce`, `/cpause`, `/cresume`, `/cmute`, `/cunmute`, `/cskip`, `/caskip`, `/cstop`, `/cend`, `/cqueue`, `/cclear`, `/cremove`, `/cmove`, `/crestore`, `/cpos`, `/cposition`, `/cseek`, `/cseekback`, `/cjump`, `/cbass`, `/cbassboost`, `/cspeed`, `/csetspeed`, `/cspeedup`, `/creplay`, `/cloop`, `/csetloop`, `/cshuffle`, `/cplayer`, `/cjoin`, `/creload` | `handleMessage` → channel-command resolution (`channel_cmds_`) → the base command's handler, run against the linked channel | same as the base command | same rendered update as the base command, in the linked channel's voice chat | Live |
| C95 | Any chat | Reply a number (search pick) | plain text `<number>` (bare digits, active search session) | `handleMessage` plain-text path → `playSearchPick` | `vc.play` (via `/play` on the picked track) | the search message is deleted and the now-playing card is sent as a new message; no active session or out of range → the text is ignored (falls through to the unhandled-text path) | Live |
| C96 | Any chat | Delete trigger | plain text `delete:<path>` | `handleMessage` plain-text path | none | new message: delete-confirm card (`Delete <label>?` + `deleteConfirmKeyboard()`, rows A81–A82); `..` in path → `Invalid path.`; top-level path → `Cannot delete top-level directories.`; empty path → ignored | Live |
| C97 | DM only | Pending download-settings template | plain text (any non-command text while `dlset_tpl_pending` is set) | `handleMessage` plain-text path (DM pending-state flow) | none (per-user prefs in local store) | new message: `Current Path Template: <template>` with `dlSettingsIndexKeyboard()` | Live |
| C98 | DM only | Pending share feedback | plain text (any non-command text while `sharefb:<user>` is set) | `handleMessage` plain-text path (DM pending-state flow) | none | new message: `Feedback is sent` to the user; the feedback text is forwarded to the original sender as a reply to the shared audio; undeliverable sender → `Could not send the feedback: <error>` | Live |
| C99 | DM only | Pending share target | plain text `cancel` (while `sharetarget:<user>` is set) | `handleMessage` plain-text path (DM pending-state flow) | none | new message: `Sharing cancelled.`; any other text → `Your request was not handled` + the available-commands list (`kUnhandledText`) | Live |
| C100 | DM only | Unhandled text | plain text (any non-command text, no pending state) | `handleMessage` plain-text path | none | new message: start card (`kStartPm` + `startPmKeyboard()`) | Live |
| C101 | Group chat | Unhandled text | plain text (any non-command text, no pending state, not a digit, not `delete:`, not one of the four reply-keyboard labels in rows B1–B4) | `handleMessage` plain-text path | none | no reply — the text is silently dropped (intended: the bot does not answer arbitrary group chatter) | Live |

## §D. Inline mode + deep links

Router: `Dispatcher::handleInlineQuery` (`inlineModeOf` mirrors
vc-ts `resolvers.js` `inlineMode` exactly). Non-allowed users (blocklist
or maintenance + non-sudo) get no results at all, silently. `np` / `s` /
`l` need the user's most recent group chat (`last_chat:<user>`, recorded
on group activity and on callback taps); with no known chat they answer
empty. A backend failure answers empty (Telegram shows "no results")
rather than leaving the query hanging.

| # | Surface | Button / input | Emits | Router / handler | Backend action | Rendered update | Status |
|---|---|---|---|---|---|---|---|
| D1 | Inline query (any chat) | Empty query, or query under 3 chars | inline query `` / `<3 chars` | `handleInlineQuery` → mode `hint` | none | inline answer: empty results + hint `np  s  l  lib  del — or type to search` (switch_pm `help`, cache 30s) | Live |
| D2 | Inline query (any chat) | `help` | inline query `help` | `handleInlineQuery` → mode `help` | none | inline answer: two articles (`1. Paste a music share URL or enter a Music ID`, `2. Enter search + keyword to search songs`), both sending `Parlay v0.2.0` (cache 3600s) | Live |
| D3 | Inline query (any chat) | `<keyword>` (3+ chars) / `search <keyword>` | inline query `<keyword>` | `handleInlineQuery` → mode `search` | `search.inline` | inline answer: one result per track — audio result when a `stream_url` is known (title, performer, duration, thumbnail, caption, `[Open in Parlay Bot]` button → row D12), article otherwise (title, artist, duration, `[Open in Parlay Bot]` button); no matches → article `No results found`; bare `search` with no keyword → article `Please enter a keyword` | Live |
| D4 | Inline query (any chat) | `np` | inline query `np` | `handleInlineQuery` → mode `np` | `search.inline` | inline answer: one audio/article result for the current track (no `[Open in Parlay Bot]` button); no known chat → empty results; no track → empty results | Live |
| D5 | Inline query (any chat) | `s` | inline query `s` | `handleInlineQuery` → mode `s` | `search.inline` | inline answer: one share article (`{artist} — {title}` + track URL as the message text); no known chat → empty results; no title → empty results | Live |
| D6 | Inline query (any chat) | `l` | inline query `l` | `handleInlineQuery` → mode `l` | `search.inline` + `vc.lyrics` (POST `/v1/lyrics`, body `{chat_id}` — the contract's GET form 404s, the POST form is the working one, §6-R5) | inline answer: one lyrics article (`Lyrics: <title>`, description `{artist} — <first 100 chars>`, message `{artist} — {title}` + lyrics truncated to 4000 chars + `...`); no lyrics → empty results; no known chat → empty results | Live |
| D7 | Inline query (any chat) | `lib` / `lib <arg>` | inline query `lib` | `handleInlineQuery` → mode `lib` | none (no library backend) | inline answer: empty results (no library backend — ruling 1, future work) | Live |
| D8 | Inline query (any chat) | `del` / `del <arg>` | inline query `del` | `handleInlineQuery` → mode `del` | none (no library backend) | inline answer: empty results (no library backend — ruling 1, future work); the `delete:<path>` text trigger (row C96) is the working delete path | Live |
| D9 | Inline query (any chat) | Search result with a cached audio file | inline query `<keyword>`, result with a cached file | `handleInlineQuery` → `inlineTrackResult` | `search.inline` | inline answer: a cached-audio result is NOT produced — the Track carries no `file_id`, so the result is a plain audio result from the direct stream URL (or an article). The `Song not cached` article does not exist yet | Phase 3 |
| D10 | Inline query (any chat) | `np` for an oversize track | inline query `np` | `handleInlineQuery` → mode `np` | `search.inline` | inline answer: the current track as a plain audio/article result; the oversize branch cannot trigger — the Track carries no size, so there is no oversize article or warning | Phase 3 |
| D11 | Inline query (any chat) | Blocked user / maintenance (non-sudo) | any inline query | `handleInlineQuery` (auth gate) | none | inline answer: empty results, silently (no hint, no error) | Live |
| D12 | Inline search result button; any `t.me/<bot>?start=spotify----<id>` link | Open in Parlay Bot | URL `https://t.me/<bot>?start=spotify----<track_id>` → `/start spotify----<track_id>` | `handleInlineQuery` → `inlineOpenUrl` (button) → `handleMessage` → `onStart` → `deepLinkDownload` | `download.spotify` / `download.youtube` (by id) | new message: the track's audio file is downloaded and sent; unresolvable or expired id → `That track link has expired — search for the song again.` | Live |

## §E. OD-3 index — "implemented in Phase 3" (decided: implement all, no cuts)

Each item below has its full row in §A–§D (referenced); this section is
an index, not duplicate rows. Phase 3 must add the named backend action
(or, for the command rows, register the command and its action) and flip
the row's Status to `Live`.

| # | Item | Row(s) | Phase 3 action needed |
|---|---|---|---|
| E1 | `download.cancel` | A63 | New backend action `download.cancel` (cancel a running download by `run_id`); the core already routes `pl:dl:cancel:<run_id>` to `cbDl` and answers honestly until then |
| E2 | `library.delete` | A81, A82, C96 | New backend action `library.delete` (delete a library track by id/path); the core already routes `pl:del:yes/no:<id>` to `cbDel` and the `delete:<path>` text trigger to the confirm card |
| E3 | `/mix` | C87 | Register the command + new backend action (mix generation); today it answers the unknown-command card |
| E4 | `/recognize` | C88 | Register the command + new backend action (audio recognition); today it answers the unknown-command card |
| E5 | `/scan` | C89 | Register the command + new backend action (library scan); today it answers the unknown-command card |
| E6 | `/sharescan` | C90 | Register the command + new backend action (share scan); today it answers the unknown-command card |
| E7 | `/rmcache` | C91 | Register the command + new backend action (cache clear); today it answers the unknown-command card |
| E8 | Spotify companion surfaces (`/details`, `/analyze`) | A94 | Live since Phase 3 Wave 2: details via `spotify.details`, analyze via `spotify.analyze`; `pl:spotify:dislike` (row A93) was already Live via `spotify.dislike` |
| E9 | Download prefs contract fields | A87–A92, C48, C97 | Carry the per-user download prefs (format, use-YouTube, path template) into the backend contract (`chat.settings` / download prefs fields); today they live only in the core's local store |
| E10 | Inline `Song not cached` article | D9 | Backend returns a cached-file marker on `search.inline` tracks (or a `file_id`); the core then answers the `Song not cached` article when no cached file exists |
| E11 | Inline `np` oversize | D10 | Backend carries a size on the `np` Track (`search.inline`); the core then answers the oversize article/warning for tracks over the sendable limit |

## §F. Cross-check: builders vs handlers (both directions)

Built from the same sources as the tables: every `cb*` builder and
`kCb*` const in `ui_texts.hpp`, every `B::callback` / `InlineButton::callback`
call site in every `*Keyboard` builder, and every domain/verb branch in
`handleCallback` + the 18 `cb*` sub-handlers.

- **Every builder has a handler.** All 17 `cb*` builder functions
  (`cbHelp`, `cbPlayer`, `cbPlayerToken`, `cbQueueMore`, `cbQueueDel`,
  `cbSet`, `cbSetInfo`, `cbSetTimer`, `cbAuthDel`, `cbSearchPick`,
  `cbSearchPage`, `cbDlCancel`, `cbDlFx`, `cbVote`, `cbDel`, `cbLossy`,
  `cbRestart`) and all 7 `kCb*` consts (`kCbStart`, `kCbClose`,
  `kCbHelpMain`, `kCbLangSelect`, `kCbLangEn`, `kCbBcastCancel`,
  `kCbSetMain`) emit callbacks whose domain is dispatched by
  `handleCallback` to a `cb*` sub-handler that handles the verb (rows
  A1–A95). Every `*Keyboard` builder is attached to exactly one use
  site in `dispatcher.cpp` (verified by grep: each builder name occurs
  once in `dispatcher.cpp` plus once in its definition), except
  `playerKeyboard` / `stopConfirmKeyboard` / `menuPanelKeyboard` /
  `settingsKeyboard` / `authlistKeyboard` / `helpTopicKeyboard` /
  `helpMenuKeyboard` / `startGroupKeyboard`, which have 2–4 use sites
  (shared cards) — all still attached.
- **Every handler has a builder — except the `No button` rows.**
  Handler-only callbacks (a branch exists, no button emits it):
  `pl:help:owner` / `pl:help:radio` / `pl:help:search` (rows A14–A16 —
  topics exist in `helpTopicText` but have no menu button),
  `pl:player:thumb` (row A35 — `playerAction` handles `thumb`, no
  button emits it), `pl:share:cancel` (row A78 — the share prompt's
  Cancel button is `pl:close`, row A19), `pl:lang:<non-en>` (row A62 —
  the picker offers English alone). These are recorded, not dropped.
- **Buttons built outside the core.** Two callback families are emitted
  by other services, not by `ui_texts.hpp`: `pl:vote:approve/decline`
  (rows A75–A76, queue-rust voting card) and `pl:spotify:dislike`
  (row A93, spotify-rust companion card). Both have in-core handlers.
  The routing test covers them by calling their `cb*` builders
  directly (§H).
- **Reply keyboards.** One persistent keyboard (`persistentKeyboard()`),
  4 buttons, rows B1–B4. No other reply keyboard exists in the core.
- **Commands.** 87 `add()` groups / 112 names / 70 aliases / 40 channel
  commands in `registerCommands`; rows C1–C86 cover all 87 groups
  (`/nothumb` and `/thumbnail` are two groups sharing one handler and
  one row, C57); rows C93–C94 cover all aliases and channel commands.
  The routing test re-derives these counts from `dispatcher.cpp` (§H).

## §G. Known §7.5 gaps (recorded, not hidden)

| # | Gap | Rows | What happens today | What a fix looks like |
|---|---|---|---|---|
| G1 | Reply-keyboard buttons unrouted in groups | B1–B4, C101 | FIXED 2026-11-03: `handleMessage` routes the four labels in groups — Music returns the start card, Global Search asks for a query, Settings opens the settings panel, About returns the about card; other plain text is still silently dropped (row C101). | Done — see rows B1–B4 and `routing_test.cpp` §F. |
| G2 | Vote callbacks never answer the callback query | A75–A76 | FIXED 2026-11-03: `cbVote` answers every tap — toast `Vote recorded.` on a counted vote (after the card edit), alert `Unknown action.` on an unknown/malformed vote, alert `You aren't allowed` on a foreign-chat vote, alert with the failure text on a backend failure. | Done — see rows A75–A76 and `routing_test.cpp` §D. |
| G3 | `/menu` panel: other users' taps are silently ignored | C41, A21–A34 (panel path) | By design (remix §2.3): only the user who ran `/menu` may press; everyone else gets an empty ack and no toast. The query IS answered, so this is a deliberate silent-ignore, recorded here for completeness. | No fix proposed — deliberate rule; recorded so it is not mistaken for a missing answer. |

## §H. Verification

- **Routing test:** `services/core-cpp/tests/routing_test.cpp`
  (wired in `services/core-cpp/CMakeLists.txt` as `routing_test`,
  run by `ctest`). It derives its enumeration from the same sources
  as this table and checks all three directions:
  1. It calls every `*Keyboard` builder in `ui_texts.hpp` and every
     `cb*` builder directly (including the out-of-core `cbVote` /
     `cbPlayerToken` families), collects every `callback_data` value,
     and asserts each is ≤64 bytes.
  2. It dispatches every collected callback through a real
     `Dispatcher` (fake Telegram sender + fake IPC backend, owner
     user) and asserts the callback query was answered (toast
     recorded) — `pl:vote:*` included (gap G2 fixed: counted votes
     answer `Vote recorded.` and still edit the card); and
     `pl:restart:bot`, which it does NOT dispatch (it exits the
     process by design) and asserts only for length + table
     presence.
  3. It dispatches unknown callbacks (`pl:bogus:…`, non-`pl:` data,
     empty data, 65-byte data) and asserts each is answered with the
     `Unknown action.` alert — the default branch.
  4. It enumerates `persistentKeyboard()` and asserts every button
     label is routed in a DM (start card reply) and runs its claimed
     action in a group (gap G1 fixed: Music → start card, Global
     Search → search prompt, Settings → settings panel, About →
     about card).
  5. It reads this file (path passed by CMake) and asserts every
     collected callback's fixed prefix, every keyboard button label,
     and every command name / alias / channel command re-derived from
     `services/core-cpp/src/dispatcher.cpp` appears in the table — so
     a callback, button, or command that exists in code but not in
     this table fails the test.
  6. It asserts zero unrouted buttons and zero unhandled callbacks in
     the sense above: every button and callback in code is accounted
     for in this table with a verified outcome. Gaps G1–G2 are fixed
     and verified; G3 remains a deliberate recorded rule, not a gap.
- **Blank-row check:** a script check (not eyeballing) parses every
  table row in this file and asserts no cell is empty after trimming.
  The check is this one-liner, run from `~/workspace/parlay` (it treats
  only a line made of `|`, `-`, `:`, spaces as a separator, so rows
  containing `spotify----` are checked, not skipped):
  `python3 -c "import re,sys; sep=re.compile(r'^\|[\s\-|:]+\|$'); rows=[l for l in open('contracts/routing-table.md') if l.startswith('|') and not sep.match(l)]; bad=[l for l in rows if any(not c.strip() for c in l.strip().strip('|').split('|'))]; print(f'{len(rows)} rows, {len(bad)} blank cells'); sys.exit(1 if bad else 0)"`.
  Result: **244 rows, 0 blank cells** (re-run after any edit).
- **Counts (this file):** callbacks §A: 95 rows (A1–A95); reply-keyboard
  buttons §B.1: 4 rows; URL / switch-inline buttons §B.2: 11 rows;
  commands §C: 101 rows (C1–C101: 86 live command groups covering 87
  `add()` groups + 5 OD-3 commands + unknown-command + 70 aliases +
  40 channel commands + 7 plain-text rows); inline mode §D: 12 rows;
  OD-3 index §E: 11 entries (pointing at §A–§D rows, not new rows);
  **total table rows: 95 + 4 + 11 + 101 + 12 = 223 rows; OD-3 rows
  among them: 10** (A63, A81, A94, C87–C91, D9, D10). The 11th OD-3
  item (E9, download prefs contract fields) shares the Live rows
  A87–A92 / C48 / C97 and is tracked as an index entry only.
- **Full-suite result after this work:** see the Phase 2 report
  (routing_test green; full core-cpp `ctest` green modulo the 2
  pre-existing environment-only `store_test` failures — mongo/redis
  down in the sandbox).

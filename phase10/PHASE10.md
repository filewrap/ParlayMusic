# Phase 10 — UI completion notes

Package 1 (2026-11-05): silent denials + sudo gaps.

## Deviation from the locked doc (direct user order)

2026-11-05 (supersedes the silent-drop ruling from earlier the same
day): unknown commands answer, and unknown callbacks answer with the
hand toast — by direct user order, overriding locked §7.5/C92 'never
silent' only in the sense that the unknown-command card is now a
short refined reply instead of the old card with a command list, and
the unknown-callback answer is the single hand emoji instead of the
'Unknown action.' alert.

- Unknown commands (`/<anything-not-registered>`, routing row C92)
  answer in `Dispatcher::handleMessage` with the refined
  `kUnknownCommand` card: `Command /<name> doesn't exist. Use /help
  to see the commands available to you.` — no command list, no
  emojis. The `kUnknownCommand` const was restored with this new
  short text (the earlier silent-drop change had removed it).
- Unknown callbacks (routing row A95) are answered in
  `Dispatcher::handleCallback` with the toast `🖐️` (alert off) —
  the query is still answered, the spinner ends. This covers empty
  data, data over 64 bytes, a first segment other than `pl`, and an
  unknown domain. A known domain keeps its own honest alerts for an
  unknown verb or bad arguments, and a handler that throws still
  answers with the "not available yet" alert.

## Package 1 items (all Live)

- S1 `/rmcache` (row C91): sudo-only, posts existing `cache.clear`
  (`onRmcache`).
- S2 `/scan` (row C89): sudo-only, posts existing `library.scan`
  (`onScan`).
- S3 `/sharescan` (row C90): sudo-only, posts existing
  `library.sharescan` (`onShareScan`).
- S4 Admin-list refresh (row C64 note): a non-bot member gaining or
  losing administrator/creator status re-fetches the admin list at once
  (`updateChatMember` → `Dispatcher::handleAdminTransition`); ordinary
  member joins do not refresh.
- S5 Owner blacklist exemption (rows C65/C66 notes): the owner is never
  refused on Everyone commands by the blacklist gate, even when listed
  in `bl_users`.

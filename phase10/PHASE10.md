# Phase 10 — UI completion notes

Package 1 (2026-11-05): silent denials + sudo gaps.

## Deviation from the locked doc (direct user order)

2026-11-05: unknown commands/callbacks are silent by direct user order,
overriding locked §7.5/C92 'never silent' for this point only.

- Unknown commands (`/<anything-not-registered>`, routing row C92) are
  silently dropped in `Dispatcher::handleMessage`: no reply, no error,
  no crash. The `kUnknownCommand` card const was removed with the only
  code path that used it.
- Unknown callbacks (routing row A95) are silently acked in
  `Dispatcher::handleCallback`: empty data, data over 64 bytes, a first
  segment other than `pl`, and an unknown domain answer the callback
  query with empty text (alert off) — the query is still answered, the
  user sees nothing. A known domain keeps its own honest alerts for an
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

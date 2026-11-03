# Worker D — fx persistence (Phase 4)

Date: 2026-11-03 ~19:30 IST. Base: main at `d36cdd7` (wiring-audit
fixes pushed, tree clean). **Nothing pushed. `.env` never read or
touched. No store-package, core-cpp, or contracts changes. No
Phase 5 work. No stubs, mocks, or placeholders.**

Verdict: **FIXED in place — no carry-forward.** The fix fit cleanly
in Phase 4's state work: two additive methods on `room.Manager`
plus a rewire of `fx` onto them. Zero changes to the store package
(Worker B's area) and zero new buckets.

## 1. The trace — where fx was written/read today (before the fix)

Single writer, zero readers outside the handler:

- `services/backend-go/internal/fx/fx.go:57` — `Service` held
  `store *store.Store` directly; zero references to `room.Manager`.
- `fx.go:144,154` (pre-fix lines) — `handleApply` did
  `s.store.GetRoom(chatID)` → set `room.FXPreset` /
  `room.FXParams` → `s.store.PutRoom(room)`, creating a
  `model.DefaultRoomState(chatID)` when the chat had no room.
- Readers of the persisted fields: **none in-tree**. `FXPreset` /
  `FXParams` (`internal/model/model.go:99-100`) are read only by
  `room.isDefaultLocked` (treats a room with FX set as
  non-default, so `Stop` won't prune it) and, per the design, by
  the Phase 5 DSP at stream time. The per-file record
  (`fx.go:files map[string]fileRecord`) is written on every apply
  and **never read anywhere** — write-only in-memory cache, left
  untouched (see §4).
- `app.Deps` already carries the shared `Room *room.Manager`
  (`internal/app/deps.go:44`); `vote` and `gui` already hold it and
  go through it. `fx` was the only feature package bypassing it.

### The exact staleness shape

`room.Manager` owns the in-memory room (`rooms map[int64]*roomState`)
and `persistLocked` writes the whole `RoomState` snapshot to the
bbolt `rooms` bucket on every mutation, under the same lock hold.
fx's direct `GetRoom`/`PutRoom` round-trip was invisible to it,
so two lost updates were possible:

1. **fx's write clobbered by the Manager.** fx writes FX to the
   store; the next Manager mutation (`Pause`, `Play`, `Advance`,
   …) persists its in-memory copy — which never saw the FX — and
   the preset silently disappears from bbolt.
2. **fx's write clobbering the Manager.** Between fx's `GetRoom`
   and `PutRoom`, a Manager mutation can persist a newer snapshot
   (new current track, frozen position); fx's `PutRoom` then
   overwrites it with the older snapshot. Also, fx's `GetRoom`
   sees the store's *frozen* `PositionSec`, not the live wall-clock
   position the Manager holds.

Same shape the vote/gui seams had before Wave 2 rewired them onto
`d.Room.Enqueue` / `Manager.Snapshot`.

## 2. What changed

### `internal/room/room.go` (+45) — additive Manager API, same family as `SetSpeed`/`SetBass`

- `Manager.SetFX(chatID, effect, params) (model.NowPlaying, error)`:
  `getOrCreateLocked` (creates the room when the chat has none —
  same as the old fx behaviour), sets `FXPreset`/`FXParams`,
  `persistLocked`, returns the snapshot. The params map is copied
  in, so the caller can't mutate the room's map afterwards.
- `Manager.FX(chatID) (effect string, params map[string]float64, ok bool)`:
  read accessor mirroring `Current`, for the Phase 5 DSP at stream
  time and for tests. Returns a copy of the params map.
- No change to `Stop` (FX is a persistent setting like shuffle /
  volume: kept across stop, and `isDefaultLocked` already treats
  FX-bearing rooms as non-default), no change to `Restore`
  (FX fields round-trip through the stored `RoomState` JSON as-is),
  no change to the store package, no new bucket — the rooms bucket
  is the fx home, as the audit's "store has no fx bucket" note
  already said.

### `internal/fx/fx.go` (+/-41) — rewired onto the seam

- `Service` now holds `rooms *room.Manager` instead of
  `store *store.Store`; `Register` panics unless `d.Room != nil`
  (mirrors vote/gui; `app.New` already guarantees a non-nil Room,
  and `cmd/backend`'s `registerAll` passes the full deps, so
  production wiring is unchanged).
- `handleApply` replaces the `GetRoom`/`PutRoom` block with one
  call: `s.rooms.SetFX(chatID, body.Effect, params)`. All
  validation, the record-only response shape, `output_path` =
  input, `"original"` no-op semantics, and error codes are
  byte-identical to before.
- Imports drop `time` / `model` / `store`, gain `room`. Package
  comment updated to state the seam rule.

### `internal/fx/fx_test.go` (+113) / `internal/room/room_test.go` (+31)

- Harness builds a real `room.NewManager` alongside the store
  (same pattern as `gui_test.go`); `TestApplyRecordOnly` still
  asserts via `st.GetRoom`, proving the Manager persist lands in
  bbolt.
- `TestRegisterDoesNotPanic` now passes `Room` in deps.
- New `TestApplySurvivesRestart` — apply `nightcore` + rate 1.25,
  close the store, reopen at the same dir, assert the raw bbolt
  record still has the FX, then a fresh `Manager` + `Restore`
  returns it via `Manager.FX`. This is the restart proof.
- New `TestApplyDoesNotClobberLiveRoom` — the precise regression
  test for the old shape: seed a playing room via `rooms.Play`,
  apply `bass-boost`, assert the live snapshot still shows the
  track and `Manager.FX` returns the preset immediately, then
  `rooms.Pause` (a Manager mutation → persist) and assert the
  store record **still** has the FX *and* the current track.
  Under the old direct-write code the Pause persist would have
  overwritten `FXPreset` with `""` — this assertion is the one
  that fails on the old code.
- New `TestSetFX` (room package) — pins the API directly: unknown
  chat returns not-found, caller-side and return-side map mutation
  cannot alias into the room, `"original"` overwrites like any
  other preset.

## 3. Test evidence

Toolchain: repo-vendored `.tools/go/bin/go` (go.mod requires
1.26.0; the vendored binary resolves the toolchain).

- `gofmt -l internal/fx internal/room` — **zero unformatted files**.
- `go vet ./...` — **clean, exit 0**.
- `go test ./internal/fx/ -count=1` — **7 passed / 0 failed**
  (5 pre-existing + 2 new).
- `go test ./internal/room/ -count=1` — **22 passed / 0 failed**
  (21 pre-existing + `TestSetFX`).
- `go test ./... -count=1` — **25/25 packages ok**; verbose
  **248 passed / 0 failed / 12 skipped** (Phase 3 baseline was
  245/0/12 — the +3 are exactly the new tests above; the 12 skips
  are the unchanged live-gated set). `cmd/backend` ok, so
  `TestRegisterAllExactlyOnce` (65 actions + ping) still passes
  with the rewired `fx.Register`.

## 4. Deliberately not changed (notes for Phase 5)

- **Per-file record stays in-memory.** `Service.files` is written
  on every apply and read by nothing; the durable record is the
  room's fx field, per the locked design ("the DSP reads the
  room's fx field at stream time"). If Phase 5 wants per-file FX
  records to survive a restart, that needs a new store bucket —
  a Worker B / Phase 5 decision, not smuggled in here.
- **No `fx` read action added.** Nothing in-tree reads FX today
  except `isDefaultLocked`; the Phase 5 DSP will read it via the
  new `Manager.FX`. No `fx.get` UDS action exists in the contract
  and none was invented.
- **`Stop` does not clear FX.** FX survives a stop like shuffle
  and volume do; clearing it would be a behaviour change needing
  a ruling, and `isDefaultLocked` already accounts for it.

## 5. Files changed (uncommitted, not pushed)

```
services/backend-go/internal/fx/fx.go          | 41 ++++-----
services/backend-go/internal/fx/fx_test.go     | 113 +++++++++++++++++++++++--
services/backend-go/internal/room/room.go      |  45 ++++++++++
services/backend-go/internal/room/room_test.go |  31 +++++++
4 files changed, 203 insertions(+), 27 deletions(-)
```

No additive store API was needed. No other worker's files touched.

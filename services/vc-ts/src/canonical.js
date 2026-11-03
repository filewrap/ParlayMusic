// canonical.js — canonical VC room document (A11), shared by vc-ts
// and its tests. Pure functions: no I/O, no engine state.
//
// The canonical room is the value of shared-store key
// `vc:room:<chat_id>` (Mongo collection `state`, {_id: "kv:<key>",
// v: "<json>"}; Redis key `parlay:kv:<key>`). All three VC engines
// write and read this exact shape, so a queue written by one engine
// is servable by another after fail-over:
//
//   {
//     "version": 1, "chat_id": 123, "current": <track|null>,
//     "queue": [{"position":0,"requested_by":0,"track":{...}}],
//     "paused": true, "position_sec": 42, "volume": 100,
//     "loop": 0, "shuffle": false, "autoplay": false,
//     "history": ["<track id>", ...], "updated_at_ms": 1730000000000
//   }
//
// Track: contract fields only (id/title/source/url/artist/
// duration_sec/thumbnail/stream_url, optionals omitted when empty).
// Engine-local fields (vc-rust resolved_by/file_path/downloaded,
// vc-ts started_at_ms/muted_volume/autoplay_pending) are runtime-only
// and never enter the canonical document. Restored rooms come back
// paused in every engine (reader policy, not a stored value).
"use strict";

// Track -> canonical track (contract fields only).
function trackToCanonical(t) {
  const out = {
    id: t.id,
    title: t.title,
    source: t.source || "youtube",
    duration_sec: Number(t.duration_sec) || 0,
  };
  if (t.artist) out.artist = t.artist;
  if (t.url) out.url = t.url;
  if (t.thumbnail) out.thumbnail = t.thumbnail;
  if (t.stream_url) out.stream_url = t.stream_url;
  return out;
}

// Room -> canonical room document. `positionSec` is the room's frozen
// position in seconds (callers pass positionSec(room)).
function roomToCanonical(r, positionSec) {
  return {
    version: 1,
    chat_id: r.chat_id,
    current: r.current ? trackToCanonical(r.current) : null,
    queue: (r.queue || []).map((it) => ({
      position: it.position || 0,
      requested_by: it.requested_by || 0,
      track: trackToCanonical(it.track),
    })),
    paused: Boolean(r.paused),
    position_sec: positionSec,
    volume: r.volume,
    loop: r.loop,
    shuffle: Boolean(r.shuffle),
    autoplay: Boolean(r.autoplay),
    history: [...(r.history || [])],
    updated_at_ms: Date.now(),
  };
}

// Canonical room document -> vc-ts room shape (same shape loadState
// builds from a state.json room). Unknown fields are ignored, so docs
// written by vc-go/vc-rust load as-is.
function roomFromCanonical(s) {
  return {
    chat_id: s.chat_id,
    current: s.current || null,
    queue: Array.isArray(s.queue)
      ? s.queue
          .map((it) => ({
            position: it.position || 0,
            requested_by: it.requested_by || 0,
            track: it.track,
          }))
          .filter((it) => it.track && it.track.id)
      : [],
    paused: true, // restored rooms come back paused
    position_sec: s.position_sec || 0,
    started_at_ms: Date.now(),
    volume: s.volume || 100,
    loop: Number(s.loop) || 0,
    shuffle: Boolean(s.shuffle),
    autoplay: Boolean(s.autoplay),
    history: Array.isArray(s.history) ? s.history : [],
    autoplay_pending: false,
    muted_volume: null,
  };
}

module.exports = { trackToCanonical, roomToCanonical, roomFromCanonical };

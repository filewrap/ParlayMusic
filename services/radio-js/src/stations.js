// Curated station list for radio-js.
//
// NOTE on provenance: upstream openradiobot (refs/openradiobot/index.js)
// has NO curated station list — it is a user-queue radio: a chat creates
// a radio, queues YouTube/raw streams into it, and the openradio player
// plays the queue. This list is therefore original to Parlay: well-known
// public live streams, chosen so `radio.start` works out of the box.
// Each station also carries a small fallback playlist (labelled as such):
// if a live stream cannot be probed/reached, the service keeps rotating
// these playlist entries in its now-playing state instead of going silent
// — the same "queue keeps playing" idea as openradiobot's queue loop.
// The playlist entries are Parlay's own fallback labels, not a claim
// about what the live stream is currently broadcasting.

"use strict";

const STATIONS = [
  {
    id: "groovesalad",
    name: "SomaFM Groove Salad",
    genre: "Ambient / downtempo beats",
    stream_url: "https://ice1.somafm.com/groovesalad-128-mp3",
    playlist: [
      { title: "Groove Salad — ambient beats mix", artist: "SomaFM" },
      { title: "Groove Salad — late-night downtempo", artist: "SomaFM" },
      { title: "Groove Salad — chillout session", artist: "SomaFM" },
    ],
  },
  {
    id: "lush",
    name: "SomaFM Lush",
    genre: "Mellow vocals / chill",
    stream_url: "https://ice1.somafm.com/lush-128-mp3",
    playlist: [
      { title: "Lush — mellow vocal chill", artist: "SomaFM" },
      { title: "Lush — soft electronic", artist: "SomaFM" },
    ],
  },
  {
    id: "radioparadise",
    name: "Radio Paradise (Main Mix)",
    genre: "Eclectic rock / world",
    stream_url: "https://stream.radioparadise.com/aac-320",
    playlist: [
      { title: "Radio Paradise — main mix", artist: "Radio Paradise" },
      { title: "Radio Paradise — eclectic hour", artist: "Radio Paradise" },
    ],
  },
  {
    id: "fip",
    name: "FIP Paris",
    genre: "Eclectic / jazz / world",
    stream_url: "https://icecast.radiofrance.fr/fip-midfi.mp3",
    playlist: [
      { title: "FIP — Paris eclectic", artist: "Radio France" },
      { title: "FIP — jazz & soul", artist: "Radio France" },
    ],
  },
  {
    id: "bbcws",
    name: "BBC World Service",
    genre: "News / talk",
    stream_url: "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service",
    playlist: [
      { title: "BBC World Service — live news", artist: "BBC" },
      { title: "BBC World Service — programmes", artist: "BBC" },
    ],
  },
  {
    id: "kexp",
    name: "KEXP 90.3 FM Seattle",
    genre: "Indie / alternative",
    stream_url: "https://streams.kexp.org/kexp.mp3",
    playlist: [
      { title: "KEXP — indie & alternative", artist: "KEXP" },
      { title: "KEXP — live in-studio", artist: "KEXP" },
    ],
  },
];

function findStation(ref) {
  if (!ref) return null;
  const q = String(ref).trim().toLowerCase();
  return (
    STATIONS.find((s) => s.id === q) ||
    STATIONS.find((s) => s.name.toLowerCase() === q) ||
    STATIONS.find((s) => s.name.toLowerCase().includes(q)) ||
    null
  );
}

module.exports = { STATIONS, findStation };

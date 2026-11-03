// Package types mirrors the IPC contract in contracts/ipc.json (version
// 0.2.0): Track / QueueItem / NowPlaying plus the success and error
// envelopes every Parlay service speaks.
package types

import "fmt"

// Track is a single playable item. Required fields per contract: id, title,
// source. Source is one of youtube|spotify|netease|deezer|jiosaavn|
// soundcloud|radio|file|lemonic.
type Track struct {
	ID          string `json:"id"`
	Title       string `json:"title"`
	Source      string `json:"source"`
	URL         string `json:"url,omitempty"`
	Artist      string `json:"artist,omitempty"`
	DurationSec int64  `json:"duration_sec,omitempty"`
	Thumbnail   string `json:"thumbnail,omitempty"`

	// StreamURL is the resolved direct audio URL (yt-dlp) when known.
	// Part of the contract (Track.stream_url); omitted when empty, so
	// the core can fetch vc-go's resolved URL from /v1/now-playing and
	// queue responses. Set by the resolver (info.URL) and by the
	// high-perf prefetch workers.
	StreamURL string `json:"stream_url,omitempty"`
	// Prefetched reports whether StreamURL was resolved ahead of time by the
	// high-perf prefetch workers (ArcMusic-style). Internal to vc-go,
	// not part of the contract; omitted from IPC payloads.
	Prefetched bool `json:"-"`

	// IPC-3 (UI_INVENTORY.md §29): enriched search-result fields for the
	// core's numbered result list (remix metadata rows). All optional;
	// omitted when the resolving source does not provide them.
	// DurationStr is the human form of DurationSec: "M:SS" under an
	// hour, "Xh Ym Zs" at/over an hour, "∞" when unknown or live.
	DurationStr string `json:"duration_str,omitempty"`
	// Views is the source's view/play count, when known (nil = unknown).
	Views *int64 `json:"views,omitempty"`
	// Uploader is the channel/account that published the track, when known.
	Uploader string `json:"uploader,omitempty"`
	// UploadTime is the publish date as "YYYY-MM-DD", when known.
	UploadTime string `json:"upload_time,omitempty"`
	// IsLive reports a live stream (duration unknown, never advances).
	IsLive bool `json:"is_live,omitempty"`
}

// FormatDurationStr renders a duration in seconds the way the core's
// search-result rows show it (IPC-3): "M:SS" under an hour, "Xh Ym Zs"
// at/over an hour, "∞" when the duration is unknown (sec <= 0) or the
// track is live. Hours/minutes/seconds parts that are zero are dropped
// except that a bare "0s" never appears — unknown is always "∞".
func FormatDurationStr(sec int64, isLive bool) string {
	if isLive || sec <= 0 {
		return "∞"
	}
	if sec < 3600 {
		return fmt.Sprintf("%d:%02d", sec/60, sec%60)
	}
	h := sec / 3600
	m := (sec % 3600) / 60
	s := sec % 60
	out := fmt.Sprintf("%dh", h)
	if m > 0 {
		out += fmt.Sprintf(" %dm", m)
	}
	if s > 0 {
		out += fmt.Sprintf(" %ds", s)
	}
	return out
}

// FormatUploadDate converts a yt-dlp upload_date ("YYYYMMDD") to the
// "YYYY-MM-DD" form the core shows in search metadata rows. Anything
// that is not exactly 8 digits passes through unchanged.
func FormatUploadDate(yyyymmdd string) string {
	if len(yyyymmdd) != 8 {
		return yyyymmdd
	}
	for _, c := range yyyymmdd {
		if c < '0' || c > '9' {
			return yyyymmdd
		}
	}
	return yyyymmdd[:4] + "-" + yyyymmdd[4:6] + "-" + yyyymmdd[6:]
}

// QueueItem is one entry in a chat's queue. Position is 0-based within the
// waiting queue (the currently playing track is not in the queue).
type QueueItem struct {
	Position    int   `json:"position"`
	RequestedBy int64 `json:"requested_by"`
	Track       Track `json:"track"`
}

// NowPlaying is the playback state snapshot for one chat.
type NowPlaying struct {
	ChatID      int64       `json:"chat_id"`
	Track       *Track      `json:"track"` // nil when idle
	Paused      bool        `json:"paused"`
	PositionSec int64       `json:"position_sec"`
	Volume      int         `json:"volume"`
	Queue       []QueueItem `json:"queue"`
	Engine      string      `json:"engine"` // "go"
	// StreamURL mirrors the current track's resolved direct audio URL
	// at the top level (contract NowPlaying.stream_url), so readers
	// need not dig into track.stream_url. Empty (idle, or the engine
	// has not resolved one) stays omitted.
	StreamURL string `json:"stream_url,omitempty"`
	// Loop is the remaining replays of the current track (0 = off).
	Loop int `json:"loop"`
	// Shuffle reports whether the queue pop is randomised.
	Shuffle bool `json:"shuffle"`
	// Autoplay reports whether a related track is picked on drain.
	Autoplay bool `json:"autoplay"`

	// IPC-1 (UI_INVENTORY.md §29): the fields the core's merged 5-row
	// now-playing card reads. They mirror the legacy fields above
	// (IsPaused = Paused, LoopCount = Loop, AutoplayEnabled = Autoplay)
	// except IsMuted and Speed, which vc-go derives:
	//   - IsMuted: vc-go has no separate mute flag — mute is volume 0
	//     (the core mutes via /v1/volume 0), so volume == 0 is muted.
	//   - Speed: vc-go has no speed control; playback is always at
	//     normal speed, so this is always 1.0.
	IsPaused        bool    `json:"is_paused"`
	IsMuted         bool    `json:"is_muted"`
	LoopCount       int     `json:"loop_count"`
	Speed           float64 `json:"speed"`
	AutoplayEnabled bool    `json:"autoplay_enabled"`
}

// ErrorBody is the error half of the envelope.
type ErrorBody struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

// Envelope is the single response shape for every endpoint:
// {ok:true, data:{...}} or {ok:false, error:{code, message}}.
type Envelope struct {
	OK    bool       `json:"ok"`
	Data  any        `json:"data,omitempty"`
	Error *ErrorBody `json:"error,omitempty"`
}

// Package types mirrors the IPC contract in contracts/ipc.json (version
// 0.1.0-scaffold): Track / QueueItem / NowPlaying plus the success and error
// envelopes every Parlay service speaks.
package types

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
	// Loop is the remaining replays of the current track (0 = off).
	Loop int `json:"loop"`
	// Shuffle reports whether the queue pop is randomised.
	Shuffle bool `json:"shuffle"`
	// Autoplay reports whether a related track is picked on drain.
	Autoplay bool `json:"autoplay"`
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

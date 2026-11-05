// Package model holds the shared structs of the unified Go backend.
// JSON tags match contracts/ipc.json v0.3.1 exactly — the contract is
// the authority for field names, and the UDS wire (contracts/
// uds-protocol.md) carries these shapes unchanged. Nothing here knows
// about the transport or the store; model is pure data.
package model

// Track is one playable item. Required per contract: id, title, source.
// Source is one of youtube|spotify|netease|deezer|jiosaavn|soundcloud|
// radio|file|lemonic. Nullable IPC-3 fields are pointers so that
// "unknown" serialises as null (and omits on encode when unset),
// matching the contract's integer|null / string|null types.
type Track struct {
	ID          string `json:"id"`
	Title       string `json:"title"`
	Source      string `json:"source"`
	URL         string `json:"url,omitempty"`
	Artist      string `json:"artist,omitempty"`
	DurationSec int64  `json:"duration_sec,omitempty"`
	Thumbnail   string `json:"thumbnail,omitempty"`
	// ThumbnailPath is the local path of the backend's cached copy of
	// the thumbnail (internal/thumbs), set when the track is resolved.
	// The core sends photos as inputFileLocal, so it prefers this over
	// Thumbnail (an https URL is not a valid inputFile). Empty when
	// the thumbnail was never fetched or the download failed.
	ThumbnailPath string `json:"thumbnail_path,omitempty"`
	// CardMessageID is the Telegram message id of this track's
	// now-playing card (queue-port Tier 6, item 32 — NoxBeats
	// Track::message_id). The core reports it back over vc.card
	// after sending the card; the room persists it with the queue,
	// so card edits target the right message after a core restart
	// without a separate lookup. 0 when no card has been sent for
	// this track yet.
	CardMessageID int64   `json:"card_message_id,omitempty"`
	StreamURL     string  `json:"stream_url,omitempty"`
	DurationStr   string  `json:"duration_str,omitempty"`
	Views         *int64  `json:"views,omitempty"`
	Uploader      *string `json:"uploader,omitempty"`
	UploadTime    *string `json:"upload_time,omitempty"`
	IsLive        bool    `json:"is_live,omitempty"`
	// CachedFileID is the Telegram file_id of the backend's cached
	// copy of this track, when one is known (actions-inventory §9-E10).
	// The backend never invents one: it stays empty until the core
	// reports a file_id back for a cached file (no such action exists
	// yet), so inline results honestly report "Song not cached".
	CachedFileID string `json:"cached_file_id,omitempty"`
	// SizeBytes is the size of the backend's cached file for this
	// track in bytes, when one is known (§9-E10/E11: the core's
	// inline np oversize branch reads this). It is the file size,
	// not the stream size, and is only set after verifying the file
	// still exists on disk (stale index entries are dropped lazily).
	SizeBytes int64 `json:"size_bytes,omitempty"`
}

// QueueItem is one entry in a chat's ordered list. Position is
// 0-based within the stored list (index 0 is the playing track
// when the room is Playing); the wire snapshot renumbers the
// waiting queue from 0, so a waiting item's wire position differs
// from its stored position by one while a track is playing.
type QueueItem struct {
	Position    int   `json:"position"`
	RequestedBy int64 `json:"requested_by"`
	Track       Track `json:"track"`
}

// NowPlaying is the playback-state snapshot for one chat, exactly as
// contracts/ipc.json types.NowPlaying defines it: the legacy fields
// (paused, loop, autoplay) and the IPC-1 canonical fields (is_paused,
// is_muted, loop_count, speed, autoplay_enabled) are both present
// because the core's now-playing card reads both sets.
type NowPlaying struct {
	ChatID          int64       `json:"chat_id"`
	Track           *Track      `json:"track"`
	Engine          string      `json:"engine"`
	Loop            int         `json:"loop"`
	LoopCount       int         `json:"loop_count"`
	Paused          bool        `json:"paused"`
	IsPaused        bool        `json:"is_paused"`
	IsMuted         bool        `json:"is_muted"`
	PositionSec     int64       `json:"position_sec"`
	Queue           []QueueItem `json:"queue"`
	Shuffle         bool        `json:"shuffle"`
	Autoplay        bool        `json:"autoplay"`
	AutoplayEnabled bool        `json:"autoplay_enabled"`
	Volume          int         `json:"volume"`
	Speed           float64     `json:"speed"`
	StreamURL       string      `json:"stream_url,omitempty"`
}

// RoomState is the persisted per-room playback state (one row per
// chat, bucket "rooms"). It is the store form of NowPlaying plus the
// fields the room state machine needs that the wire snapshot does
// not carry: the active FX preset/parameters and the update time.
// Loop is 0-10, volume is 0-200 (wire-format ranges, FINAL-ARCHITECTURE
// §6-R6 — protocol constants, not config).
//
// Queue model (queue-port Tier 1, 2026-11-05, item 10): the room
// holds ONE ordered list, Queue, whose index 0 is the playing
// track when Playing is true. Advance is a pop. There is no
// separate current pointer that can go stale — the ghost track
// (a current with no live feed, every /play queueing behind it
// forever) is unrepresentable. When Playing is false the list is
// only waiting tracks (vc.queue add on an idle room, a room
// restored with no live feed); the front track is next-up, and
// /skip or /resume activates it.
//
// Wave 2 (room state machine, phase3/actions-inventory.md §4.6)
// added the remaining PersistedRoom fields: the autoplay history
// (cap 100, queue-port Tier 5 item 29), the pre-mute volume, the bass level, the live radio
// state, and the last-track id. All are additive with omitempty
// tags: records written by Wave 1 simply read back as zero values.
type RoomState struct {
	ChatID int64 `json:"chat_id"`
	// Current is the legacy pre-restructure playing track. It is
	// read exactly once, by room.Restore, which moves it to the
	// front of the list as a paused track and clears it;
	// persistLocked never writes it. Remove this field after the
	// deployed store has been restored once on the new code.
	Current *Track `json:"current,omitempty"`
	// Queue is the full ordered list for the chat (see the type
	// comment): index 0 is the playing track when Playing is
	// true, else the next-up track.
	Queue       []QueueItem        `json:"queue"`
	Playing     bool               `json:"playing"`
	Loop        int                `json:"loop"`
	Shuffle     bool               `json:"shuffle"`
	Autoplay    bool               `json:"autoplay"`
	Volume      int                `json:"volume"`
	Speed       float64            `json:"speed"`
	Paused      bool               `json:"paused"`
	Muted       bool               `json:"muted"`
	PositionSec int64              `json:"position_sec"`
	StreamURL   string             `json:"stream_url,omitempty"`
	FXPreset    string             `json:"fx_preset,omitempty"`
	FXParams    map[string]float64 `json:"fx_params,omitempty"`
	UpdatedAt   int64              `json:"updated_at"`
	// AutoplayHistory is the ids of tracks autoplay has already
	// picked, oldest first, capped at MaxAutoplayHistory (100,
	// queue-port Tier 5 item 29). It is
	// cleared when autoplay is disabled and when it reaches the
	// cap at advance time.
	AutoplayHistory []string `json:"autoplay_history,omitempty"`
	// MutedVolume is the volume in force before the current mute
	// (vc-ts rule), so unmuting restores it. Nil when never muted.
	MutedVolume *int `json:"muted_volume,omitempty"`
	// Bass is the core's bass level 0-100, stored per room; the
	// Phase 5 DSP applies it (inventory §4.4).
	Bass int `json:"bass,omitempty"`
	// Radio is the live radio state when the room is playing a
	// radio track, nil otherwise (inventory §4.1, §7.2).
	Radio *RadioState `json:"radio,omitempty"`
	// LastTrackID is the id of the most recent current track, kept
	// after the room goes idle so the core's last_track display
	// and the autoplay picker keep working.
	LastTrackID string `json:"last_track_id,omitempty"`
}

// DefaultRoomState returns a fresh room for a chat: volume 100,
// speed 1.0, everything else off/empty (vc-go room.go: newRoom).
func DefaultRoomState(chatID int64) RoomState {
	return RoomState{ChatID: chatID, Volume: 100, Speed: 1.0}
}

// VoteState is one crowd-vote round on a track in a chat (bucket
// "votes", ported from queue-rust). Voters maps voter user id to the
// vote cast ("approve" or "decline"). Threshold is the configurable
// approval count at which the track is queued; Queued records that
// the threshold was reached.
type VoteState struct {
	ChatID    int64            `json:"chat_id"`
	Track     Track            `json:"track"`
	Approvals int              `json:"approvals"`
	Declines  int              `json:"declines"`
	Voters    map[int64]string `json:"voters"`
	Threshold int              `json:"threshold"`
	Queued    bool             `json:"queued"`
	CreatedAt int64            `json:"created_at"`
	UpdatedAt int64            `json:"updated_at"`
}

// ShareLink is one anonymous share link (bucket "shares", ported from
// share-rust). Token is the short token that travels in the link
// (and in pl: callbacks, which cap at 64 bytes); Link is the full
// t.me deep link.
type ShareLink struct {
	Token     string `json:"token"`
	Link      string `json:"link"`
	Track     Track  `json:"track"`
	CreatedBy int64  `json:"created_by"`
	CreatedAt int64  `json:"created_at"`
	Uses      int    `json:"uses"`
}

// SpotifyState is one user's Spotify OAuth record (bucket "spotify",
// ported from spotify-rust). TokenCiphertext holds the AES-256-GCM
// ciphertext (base64) of the token payload — never a plaintext
// access or refresh token. The encryption key comes from config/env
// (PARLAY_SPOTIFY_OAUTH_KEY, 32 bytes, base64); it is never
// hardcoded and never stored in this struct or in the store.
type SpotifyState struct {
	UserID          int64  `json:"user_id"`
	TokenCiphertext string `json:"token_ciphertext"`
	ExpiresAt       int64  `json:"expires_at"`
	UpdatedAt       int64  `json:"updated_at"`
}

// SkipCheckRecord is the last skip-check verdict for a track (bucket
// "spotify", keyed by track id; ported from spotify-rust's
// skip-check classifier output in contracts/ipc.json).
type SkipCheckRecord struct {
	TrackID              string  `json:"track_id"`
	Skip                 bool    `json:"skip"`
	Reason               *string `json:"reason"`
	ClassifierProvider   string  `json:"classifier_provider"`
	ClassifierVerdict    string  `json:"classifier_verdict"`
	ClassifierConfidence float64 `json:"classifier_confidence"`
	ClassifierDetail     string  `json:"classifier_detail"`
	CheckedAt            int64   `json:"checked_at"`
}

// ChatSettings is the per-chat settings object of the chat.settings
// action (contracts/ipc.json) — all 21 keys: the 15 legacy keys
// plus the 6 quality overrides below. Defaults are the
// legacy values the core's Dispatcher::chatSettings reads when no
// value is stored: admin_mode 0, play_mode "everyone", thumbnails
// on, language "en", auto_delete_min 5, paginate_purge_old on,
// chat_music_download on, everything else off/empty. The quality
// pointers are nil when unset: nil means "no per-chat override"
// and the effective Opus config falls through to parlay.toml
// [quality] (see internal/quality.Resolve). A set value wins
// over the file for the next join in that chat.
type ChatSettings struct {
	AdminMode         int    `json:"admin_mode"`
	PlayMode          string `json:"play_mode"`
	CmdDelete         bool   `json:"cmd_delete"`
	CleanMode         bool   `json:"clean_mode"`
	CleanDelayMin     int    `json:"clean_delay_min"`
	Thumbnails        bool   `json:"thumbnails"`
	Autoplay          bool   `json:"autoplay"`
	AutoplayLang      string `json:"autoplay_lang"`
	AutoplayMood      string `json:"autoplay_mood"`
	SilentMode        bool   `json:"silent_mode"`
	AutoDelete        bool   `json:"auto_delete"`
	AutoDeleteMin     int    `json:"auto_delete_min"`
	Language          string `json:"language"`
	PaginatePurgeOld  bool   `json:"paginate_purge_old"`
	ChatMusicDownload bool   `json:"chat_music_download"`
	// QualityBitrateBps is the per-chat Opus bitrate override
	// in bits per second (nil = unset). Validated 6000-510000
	// on write (voice.OpusMinBitrateBps/OpusMaxBitrateBps).
	QualityBitrateBps *int `json:"quality_bitrate_bps,omitempty"`
	// QualityFrameMs is the per-chat Opus frame-size override
	// in ms (nil = unset). Must be one of 10, 20, 40, 60.
	QualityFrameMs *int `json:"quality_frame_ms,omitempty"`
	// QualityComplexity is the per-chat Opus complexity
	// override (nil = unset). Validated 0-10 on write.
	QualityComplexity *int `json:"quality_complexity,omitempty"`
	// QualityVBR / QualityFEC / QualityDTX are the per-chat
	// VBR, FEC and DTX overrides (nil = unset).
	QualityVBR *bool `json:"quality_vbr,omitempty"`
	QualityFEC *bool `json:"quality_fec,omitempty"`
	QualityDTX *bool `json:"quality_dtx,omitempty"`
}

// DefaultChatSettings returns the 21-key settings object for a chat
// with no stored settings.
func DefaultChatSettings() ChatSettings {
	return ChatSettings{
		AdminMode:         0,
		PlayMode:          "everyone",
		CmdDelete:         false,
		CleanMode:         false,
		CleanDelayMin:     0,
		Thumbnails:        true,
		Autoplay:          false,
		AutoplayLang:      "",
		AutoplayMood:      "",
		SilentMode:        false,
		AutoDelete:        false,
		AutoDeleteMin:     5,
		Language:          "en",
		PaginatePurgeOld:  true,
		ChatMusicDownload: true,
	}
}

// RadioState is the persisted radio state for one chat (bucket
// "radio", ported from radio-js). Reachable is a pointer because the
// contract's radio.status reports reachable as boolean|null:
// nil means never probed.
type RadioState struct {
	ChatID     int64  `json:"chat_id"`
	Station    string `json:"station"`
	StreamURL  string `json:"stream_url,omitempty"`
	Reachable  *bool  `json:"reachable"`
	NowPlaying *Track `json:"now_playing"`
	UpdatedAt  int64  `json:"updated_at"`
}

// DownloadTrackProgress is one per-track checklist entry in a
// persisted download run (bucket "downloads"). It mirrors
// download.TrackProgress; the conversion lives in the download
// package so model stays free of transport types.
type DownloadTrackProgress struct {
	Title  string  `json:"title"`
	State  string  `json:"state"`
	Pct    float64 `json:"pct"`
	Peer   *string `json:"peer,omitempty"`
	Reason *string `json:"reason,omitempty"`
}

// DownloadRunRecord is the persisted snapshot of one FINISHED
// download run (bucket "downloads", key = run id). Only finished
// runs persist: a live run holds a live context, a cancel func and
// channels that cannot survive a restart, so a restart restores
// the finished-run history (for late download.progress polls) and
// live runs are simply gone. Phase is one of searching |
// downloading | uploading | done | failed. StartedAt is unix
// seconds.
type DownloadRunRecord struct {
	ID               string                  `json:"id"`
	Kind             string                  `json:"kind"`
	Title            string                  `json:"title"`
	StartedAt        int64                   `json:"started_at"`
	Phase            string                  `json:"phase"`
	ErrReason        string                  `json:"err_reason,omitempty"`
	Percentage       float64                 `json:"percentage"`
	DoneBytes        int64                   `json:"done_bytes"`
	TotalBytes       *int64                  `json:"total_bytes,omitempty"`
	SpeedBytesPerSec float64                 `json:"speed_bytes_per_sec"`
	ETASeconds       *int64                  `json:"eta_sec,omitempty"`
	PerTrack         []DownloadTrackProgress `json:"per_track,omitempty"`
	LossyAvailable   bool                    `json:"lossy_available"`
	LossySummary     *string                 `json:"lossy_summary,omitempty"`
	FilePath         string                  `json:"file_path,omitempty"`
}

// DownloadPrefs is one user's download preferences (bucket
// "dlprefs", key dlpref:<user_id>). download.Prefs is an alias for
// this type; the store owns the persisted form.
type DownloadPrefs struct {
	UserID       int64  `json:"user_id"`
	Format       string `json:"format"`
	UseYouTube   bool   `json:"use_youtube"`
	PathTemplate string `json:"path_template"`
}

// LibraryEntry is one cached file in the library index (bucket
// "library", key = track id). library.Entry is an alias for this
// type; the store owns the persisted form. AddedAt is unix seconds.
type LibraryEntry struct {
	ID        string `json:"id"`
	Title     string `json:"title"`
	Artist    string `json:"artist"`
	Source    string `json:"source"`
	FilePath  string `json:"file_path"`
	SizeBytes int64  `json:"size_bytes"`
	AddedAt   int64  `json:"added_at"`
}

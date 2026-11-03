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
	ID          string  `json:"id"`
	Title       string  `json:"title"`
	Source      string  `json:"source"`
	URL         string  `json:"url,omitempty"`
	Artist      string  `json:"artist,omitempty"`
	DurationSec int64   `json:"duration_sec,omitempty"`
	Thumbnail   string  `json:"thumbnail,omitempty"`
	StreamURL   string  `json:"stream_url,omitempty"`
	DurationStr string  `json:"duration_str,omitempty"`
	Views       *int64  `json:"views,omitempty"`
	Uploader    *string `json:"uploader,omitempty"`
	UploadTime  *string `json:"upload_time,omitempty"`
	IsLive      bool    `json:"is_live,omitempty"`
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

// QueueItem is one entry in a chat's waiting queue. Position is
// 0-based within the waiting queue (the currently playing track is
// not in the queue).
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
// Wave 2 (room state machine, phase3/actions-inventory.md §4.6)
// added the remaining PersistedRoom fields: the autoplay history
// (cap 50), the pre-mute volume, the bass level, the live radio
// state, and the last-track id. All are additive with omitempty
// tags: records written by Wave 1 simply read back as zero values.
type RoomState struct {
	ChatID      int64              `json:"chat_id"`
	Current     *Track             `json:"current"`
	Queue       []QueueItem        `json:"queue"`
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
	// picked, oldest first, capped at 50 (inventory §4.3). It is
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
// action (contracts/ipc.json) — all 15 keys. Defaults are the
// legacy values the core's Dispatcher::chatSettings reads when no
// value is stored: admin_mode 0, play_mode "everyone", thumbnails
// on, language "en", auto_delete_min 5, paginate_purge_old on,
// chat_music_download on, everything else off/empty.
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
}

// DefaultChatSettings returns the 15-key settings object for a chat
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
		AutoplayLang:      "en",
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

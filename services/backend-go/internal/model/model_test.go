package model

import (
	"encoding/json"
	"strings"
	"testing"
)

// The contract (contracts/ipc.json) is the authority for field
// names; these tests pin the JSON tags so a rename breaks loudly
// here instead of silently on the wire.
func TestTrackJSONMatchesContract(t *testing.T) {
	views := int64(1234)
	uploader := "Stub Channel"
	track := Track{
		ID: "stub-track-1", Title: "Stub Song", Source: "youtube",
		Artist: "Stub Artist", DurationSec: 268, DurationStr: "4:28",
		URL:       "https://example.invalid/watch?v=stub",
		StreamURL: "https://example.invalid/audio.webm",
		Views:     &views, Uploader: &uploader,
	}
	raw, err := json.Marshal(track)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		`"id":"stub-track-1"`, `"title":"Stub Song"`, `"source":"youtube"`,
		`"duration_sec":268`, `"duration_str":"4:28"`, `"views":1234`,
		`"uploader":"Stub Channel"`, `"stream_url":"https://example.invalid/audio.webm"`,
	} {
		if !strings.Contains(string(raw), want) {
			t.Errorf("Track JSON missing %s in %s", want, raw)
		}
	}
	var back Track
	if err := json.Unmarshal(raw, &back); err != nil {
		t.Fatal(err)
	}
	if back.Views == nil || *back.Views != 1234 || back.Uploader == nil || *back.Uploader != "Stub Channel" {
		t.Errorf("Track round-trip lost nullable fields: %+v", back)
	}
}

func TestNowPlayingJSONHasBothFieldSets(t *testing.T) {
	np := NowPlaying{
		ChatID: -1001, Engine: "go", Loop: 2, LoopCount: 2,
		Paused: true, IsPaused: true, IsMuted: true, Volume: 100,
		Speed: 1.0, Shuffle: true, Autoplay: true, AutoplayEnabled: true,
		Track: &Track{ID: "x", Title: "T", Source: "youtube"},
		Queue: []QueueItem{},
	}
	raw, err := json.Marshal(np)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		`"chat_id":-1001`, `"engine":"go"`, `"loop":2`, `"loop_count":2`,
		`"paused":true`, `"is_paused":true`, `"is_muted":true`,
		`"speed":1`, `"shuffle":true`, `"autoplay":true`,
		`"autoplay_enabled":true`, `"volume":100`, `"queue":[]`,
	} {
		if !strings.Contains(string(raw), want) {
			t.Errorf("NowPlaying JSON missing %s in %s", want, raw)
		}
	}
}

func TestChatSettingsHasAll15Keys(t *testing.T) {
	s := DefaultChatSettings()
	raw, err := json.Marshal(s)
	if err != nil {
		t.Fatal(err)
	}
	keys := []string{
		"admin_mode", "play_mode", "cmd_delete", "clean_mode",
		"clean_delay_min", "thumbnails", "autoplay", "autoplay_lang",
		"autoplay_mood", "silent_mode", "auto_delete", "auto_delete_min",
		"language", "paginate_purge_old", "chat_music_download",
	}
	var m map[string]any
	if err := json.Unmarshal(raw, &m); err != nil {
		t.Fatal(err)
	}
	if len(m) != 15 {
		t.Fatalf("ChatSettings must have exactly 15 keys, got %d: %s", len(m), raw)
	}
	for _, k := range keys {
		if _, ok := m[k]; !ok {
			t.Errorf("ChatSettings JSON missing key %q in %s", k, raw)
		}
	}
	// Defaults pinned to the legacy values (dispatcher chatSettings).
	// autoplay_lang is "" (any language): the wire-visible default
	// (settings.go defaults() + settings_test.go) — the core has no
	// autoplay_lang legacy value, so "en" here would be ungrounded.
	if s.AdminMode != 0 || s.PlayMode != "everyone" || s.Thumbnails != true ||
		s.Language != "en" || s.AutoDeleteMin != 5 || s.PaginatePurgeOld != true ||
		s.ChatMusicDownload != true || s.AutoplayLang != "" {
		t.Errorf("ChatSettings defaults drifted: %+v", s)
	}
}

func TestSpotifyStateHoldsCiphertextNotPlaintext(t *testing.T) {
	// The field name is the contract with Wave 2's crypto: the token
	// slot serialises as token_ciphertext, and there is no plaintext
	// token field anywhere on the struct.
	st := SpotifyState{UserID: 111, TokenCiphertext: "base64-ciphertext", ExpiresAt: 9999999999}
	raw, err := json.Marshal(st)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(raw), `"token_ciphertext":"base64-ciphertext"`) {
		t.Errorf("SpotifyState JSON must carry token_ciphertext: %s", raw)
	}
	if strings.Contains(string(raw), "access_token") || strings.Contains(string(raw), "refresh_token") {
		t.Errorf("SpotifyState JSON must never name a plaintext token field: %s", raw)
	}
}

func TestDefaultRoomState(t *testing.T) {
	r := DefaultRoomState(-1001)
	if r.Volume != 100 || r.Speed != 1.0 || r.ChatID != -1001 {
		t.Errorf("DefaultRoomState drifted: %+v", r)
	}
}

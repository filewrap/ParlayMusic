// download.prefs (actions-inventory §9-E9): per-user download
// preferences. Today the core keeps them in store key dlset:<user> =
// "<format>\n<youtube 0|1>\n<template>"; in Phase 3 the backend owns
// them in the bbolt dlprefs bucket (key dlpref:<user_id>), and since
// Phase 4 that bucket exists: PrefsStore.AttachStore loads it on
// boot and Set persists every change, so a restart keeps every
// user's prefs (GET for a fresh user still returns defaults,
// never 404). Nothing here opens a second bbolt file: the store is
// passed in, never opened.
//
// The download.* handlers apply prefs:
//   - format: the output container. The yt-dlp daemon extracts mp3
//     only (scripts/yt_dlp_worker.py hardcodes preferredcodec mp3),
//     so a non-mp3 format is refused honestly on daemon-backed
//     downloads (download.youtube / download.spotify / the TikTok
//     daemon fallback) until the daemon gains a format parameter.
//     Deezer/Navidrome/Soulseek keep their native container
//     regardless of this setting (their format comes from the source,
//     not a transcode).
//   - use_youtube: whether the Spotify resolution path may fall back
//     to YouTube audio. false = Spotify metadata only; a Spotify
//     download then fails honestly when no Spotify audio exists.
//   - path_template: the output filename template, applied by
//     renaming the finished file (sanitised, <=150 chars).
package download

import (
	"fmt"
	"strings"
	"sync"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/store"
)

// DownloadFormats is the accepted format enum (§9-E9).
var DownloadFormats = []string{"mp3", "m4a", "flac", "opus", "ogg", "wav"}

// DefaultPathTemplate is the filename template spotdl already uses.
const DefaultPathTemplate = "{artist} - {title}.{ext}"

// Prefs is one user's download preferences (model.DownloadPrefs;
// the store owns the persisted form).
type Prefs = model.DownloadPrefs

// DefaultPrefs returns the defaults for a user (format mp3,
// use_youtube true, template "{artist} - {title}.{ext}").
func DefaultPrefs(userID int64) Prefs {
	return Prefs{UserID: userID, Format: "mp3", UseYouTube: true, PathTemplate: DefaultPathTemplate}
}

// PrefsStore is the per-user prefs store: in-memory map, persisted
// into the store's dlprefs bucket when AttachStore has wired one
// in (nil store = memory-only, the pre-Phase 4 behaviour).
type PrefsStore struct {
	mu    sync.Mutex
	prefs map[int64]Prefs
	store *store.Store
}

// NewPrefsStore builds an empty prefs store.
func NewPrefsStore() *PrefsStore { return &PrefsStore{prefs: map[int64]Prefs{}} }

// AttachStore wires the embedded store in and loads every stored
// user's prefs. A nil store is a no-op.
func (s *PrefsStore) AttachStore(st *store.Store) error {
	if st == nil {
		return nil
	}
	all, err := st.ListDownloadPrefs()
	if err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.store = st
	for _, p := range all {
		s.prefs[p.UserID] = p
	}
	return nil
}

// sharedPrefs is the process-wide prefs store used by Register.
var sharedPrefs = NewPrefsStore()

// SharedPrefs returns the process-wide prefs store.
func SharedPrefs() *PrefsStore { return sharedPrefs }

// Get returns the stored prefs for a user, or the defaults when the
// user has none stored yet (200, never 404).
func (s *PrefsStore) Get(userID int64) Prefs {
	s.mu.Lock()
	defer s.mu.Unlock()
	if p, ok := s.prefs[userID]; ok {
		return p
	}
	return DefaultPrefs(userID)
}

// Set validates and stores prefs for a user. The caller passes the
// full object (GET-then-modify, or defaults with the POSTed fields
// merged in); unknown format is an error naming the six formats.
func (s *PrefsStore) Set(p Prefs) (Prefs, error) {
	if p.Format != "" {
		ok := false
		for _, f := range DownloadFormats {
			if p.Format == f {
				ok = true
				break
			}
		}
		if !ok {
			return Prefs{}, fmt.Errorf("unknown download format %q (want one of %s)", p.Format, strings.Join(DownloadFormats, ", "))
		}
	} else {
		p.Format = "mp3"
	}
	if p.PathTemplate == "" {
		p.PathTemplate = DefaultPathTemplate
	} else {
		t, err := sanitizeTemplate(p.PathTemplate)
		if err != nil {
			return Prefs{}, err
		}
		p.PathTemplate = t
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.store != nil {
		// Persist before the in-memory write: a store failure is
		// reported and the old prefs stand, in memory and on
		// disk (a prefs Set is a user-visible write, so a silent
		// drop would lie).
		if err := s.store.PutDownloadPrefs(p); err != nil {
			return Prefs{}, fmt.Errorf("download prefs: persist user %d: %w", p.UserID, err)
		}
	}
	s.prefs[p.UserID] = p
	return p, nil
}

// sanitizeTemplate validates a path template: it must contain an
// {ext} placeholder (the file needs an extension), must not contain
// a path separator or "..", and is capped at 150 chars.
func sanitizeTemplate(t string) (string, error) {
	t = strings.TrimSpace(t)
	if t == "" {
		return "", fmt.Errorf("path_template must not be empty")
	}
	if !strings.Contains(t, "{ext}") {
		return "", fmt.Errorf("path_template must contain {ext}")
	}
	if strings.Contains(t, "/") || strings.Contains(t, "\\") || strings.Contains(t, "..") {
		return "", fmt.Errorf("path_template must not contain a path separator or ..")
	}
	if len([]rune(t)) > 150 {
		t = string([]rune(t)[:150])
	}
	return t, nil
}

// applyTemplate renders a template for a track: {artist}, {title},
// {id} and {ext} are substituted, the result is sanitised as a
// filename (resolver.SanitizeFilename). Unknown placeholders are
// left as-is so a typo surfaces in the filename instead of silently
// vanishing.
func applyTemplate(template string, track trackLike, ext string) string {
	ext = strings.TrimPrefix(ext, ".")
	name := template
	name = strings.ReplaceAll(name, "{artist}", track.artist())
	name = strings.ReplaceAll(name, "{title}", track.title())
	name = strings.ReplaceAll(name, "{id}", track.id())
	name = strings.ReplaceAll(name, "{ext}", ext)
	return resolver.SanitizeFilename(name)
}

// trackLike is the minimal track view applyTemplate needs.
type trackLike interface {
	artist() string
	title() string
	id() string
}

// templateTrack adapts a model Track to trackLike.
type templateTrack struct {
	artistName string
	titleName  string
	idName     string
}

func (t templateTrack) artist() string { return t.artistName }
func (t templateTrack) title() string  { return t.titleName }
func (t templateTrack) id() string     { return t.idName }

package resolver

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"parlay/vc-go/internal/types"
)

func TestRelatedNoYtDlpReturnsNil(t *testing.T) {
	r := &Resolver{YtDlp: ""} // degraded: no binary
	got, err := r.Related(context.Background(),
		types.Track{ID: "yt:abc", Title: "A", Source: "youtube"}, map[string]bool{})
	if err != nil || got != nil {
		t.Fatalf("no yt-dlp should yield (nil, nil), got %+v err=%v", got, err)
	}
}

func TestRelatedNonYouTubeReturnsNil(t *testing.T) {
	r := &Resolver{YtDlp: "yt-dlp"}
	got, err := r.Related(context.Background(),
		types.Track{ID: "sp:abc", Title: "A", Source: "spotify"}, map[string]bool{})
	if err != nil || got != nil {
		t.Fatalf("non-YouTube should yield (nil, nil), got %+v err=%v", got, err)
	}
}

// New() must find the downloaders venv yt-dlp (B12): go test runs with
// cwd = the package dir, so the finder walks up to the repo root.
func TestNewFindsVenvYtDlp(t *testing.T) {
	r := New()
	if !r.Available() {
		t.Fatalf("New() found no yt-dlp; expected the downloaders venv binary")
	}
	if !strings.HasSuffix(r.YtDlp, "yt-dlp") {
		t.Fatalf("unexpected yt-dlp path: %q", r.YtDlp)
	}
}

// Phase 5 (A9): with no yt-dlp, Resolve is an honest error — never a
// degraded pattern track.
func TestResolveWithoutYtDlpIsError(t *testing.T) {
	r := &Resolver{YtDlp: ""}
	tr, err := r.Resolve(context.Background(), "https://www.youtube.com/watch?v=dQw4w9WgXcQ")
	if err == nil || tr != nil {
		t.Fatalf("expected an error and no track, got track=%+v err=%v", tr, err)
	}
	if !strings.Contains(err.Error(), "yt-dlp") {
		t.Fatalf("error must name yt-dlp, got: %v", err)
	}
	tr, err = r.Resolve(context.Background(), "some free text query")
	if err == nil || tr != nil {
		t.Fatalf("expected an error and no track for a free-text query, got %+v err=%v", tr, err)
	}
}

func liveOK(t *testing.T) bool {
	t.Helper()
	if os.Getenv("PARLAY_LIVE_TEST") == "" {
		t.Skip("live test: set PARLAY_LIVE_TEST=1 (needs network + yt-dlp)")
	}
	return true
}

// Live: a YouTube URL resolves to real metadata plus the direct audio
// URL (StreamURL). Tolerant of YouTube gating the egress IP at test
// time — then the error must name yt-dlp.
func TestResolveYouTubeURLLive(t *testing.T) {
	if !liveOK(t) {
		return
	}
	r := New()
	tr, err := r.Resolve(context.Background(), "https://www.youtube.com/watch?v=dQw4w9WgXcQ")
	if err != nil {
		if !strings.Contains(err.Error(), "yt-dlp") {
			t.Fatalf("resolve error must name yt-dlp, got: %v", err)
		}
		t.Logf("YouTube gated right now (honest failure): %v", err)
		return
	}
	if tr.ID != "yt:dQw4w9WgXcQ" {
		t.Fatalf("expected yt:dQw4w9WgXcQ, got %q", tr.ID)
	}
	if !strings.Contains(tr.Title, "Rick Astley") {
		t.Fatalf("expected a real title, got %q", tr.Title)
	}
	if tr.DurationSec <= 0 {
		t.Fatalf("expected a real duration, got %d", tr.DurationSec)
	}
	if !strings.HasPrefix(tr.StreamURL, "https://") {
		t.Fatalf("expected a resolved direct audio URL, got %q", tr.StreamURL)
	}
}

// Live: the YouTube mix pick (B12) returns a real related track.
func TestRelatedLivePick(t *testing.T) {
	if !liveOK(t) {
		return
	}
	r := New()
	last := types.Track{ID: "yt:dQw4w9WgXcQ", Title: "Rick Astley", Source: "youtube"}
	pick, err := r.Related(context.Background(), last, map[string]bool{})
	if err != nil {
		if !strings.Contains(err.Error(), "yt-dlp") {
			t.Fatalf("mix error must name yt-dlp, got: %v", err)
		}
		t.Logf("mix gated right now (honest failure): %v", err)
		return
	}
	if pick == nil {
		t.Fatalf("expected a mix pick, got nil (no error)")
	}
	if pick.ID == last.ID {
		t.Fatalf("pick must not be the last track")
	}
	if pick.Title == "" || pick.DurationSec <= 0 {
		t.Fatalf("pick must carry real metadata, got %+v", pick)
	}
}

// IPC-3 (§29): a stub yt-dlp info object maps onto the enriched Track
// fields — duration_str, views, uploader, upload_time, is_live —
// and a live stream reports duration_str "∞" with is_live true.
func TestResolveCarriesIPC3Fields(t *testing.T) {
	script := filepath.Join(t.TempDir(), "yt-dlp")
	const info = `{"id":"dQw4w9WgXcQ","title":"Never Gonna Give You Up","duration":213,` +
		`"uploader":"Rick Astley","thumbnail":"https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg",` +
		`"url":"https://example.invalid/audio.webm","view_count":1600000000,"upload_date":"20091025",` +
		`"webpage_url":"https://www.youtube.com/watch?v=dQw4w9WgXcQ"}`
	if err := os.WriteFile(script, []byte("#!/bin/sh\nprintf '%s' '"+info+"'\n"), 0o755); err != nil {
		t.Fatalf("write stub yt-dlp: %v", err)
	}
	r := &Resolver{YtDlp: script}
	tr, err := r.Resolve(context.Background(), "https://www.youtube.com/watch?v=dQw4w9WgXcQ")
	if err != nil {
		t.Fatalf("Resolve: %v", err)
	}
	if tr.DurationStr != "3:33" {
		t.Fatalf("duration_str = %q, want 3:33", tr.DurationStr)
	}
	if tr.Views == nil || *tr.Views != 1600000000 {
		t.Fatalf("views = %v, want 1600000000", tr.Views)
	}
	if tr.Uploader != "Rick Astley" {
		t.Fatalf("uploader = %q, want Rick Astley", tr.Uploader)
	}
	if tr.UploadTime != "2009-10-25" {
		t.Fatalf("upload_time = %q, want 2009-10-25", tr.UploadTime)
	}
	if tr.IsLive {
		t.Fatalf("is_live must be false for a normal video")
	}
}

func TestResolveLiveStreamIPC3(t *testing.T) {
	script := filepath.Join(t.TempDir(), "yt-dlp")
	const info = `{"id":"live1234567","title":"Live Stream","duration":0,` +
		`"uploader":"Some Channel","thumbnail":"https://example.invalid/t.jpg",` +
		`"url":"https://example.invalid/audio.webm","is_live":true,` +
		`"webpage_url":"https://www.youtube.com/watch?v=live1234567"}`
	if err := os.WriteFile(script, []byte("#!/bin/sh\nprintf '%s' '"+info+"'\n"), 0o755); err != nil {
		t.Fatalf("write stub yt-dlp: %v", err)
	}
	r := &Resolver{YtDlp: script}
	tr, err := r.Resolve(context.Background(), "https://www.youtube.com/watch?v=live1234567")
	if err != nil {
		t.Fatalf("Resolve: %v", err)
	}
	if !tr.IsLive {
		t.Fatalf("is_live must be true")
	}
	if tr.DurationStr != "∞" {
		t.Fatalf("duration_str = %q, want ∞ for a live stream", tr.DurationStr)
	}
}

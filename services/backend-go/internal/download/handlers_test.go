package download

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/library"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

// testDeps builds app.Deps over a temp data dir (real bbolt store,
// real registry — no network is touched by these tests) and a
// Service wired to a fresh run registry + prefs store.
func testDeps(t *testing.T) (*app.Deps, *Service, string) {
	t.Helper()
	dir := t.TempDir()
	st, err := store.Open(filepath.Join(dir, "store"))
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { _ = st.Close() })
	cfg := &config.Config{}
	cfg.Backend.DataDir = filepath.Join(dir, "data")
	deps := &app.Deps{
		Store:    st,
		Config:   cfg,
		Registry: resolver.NewRegistry(),
	}
	svc := NewServiceWithRegistry(deps, NewRegistry(), NewPrefsStore())
	return deps, svc, cfg.Backend.DataDir
}

func mustJSON(t *testing.T, v any) json.RawMessage {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	return b
}

func handlerErrCode(t *testing.T, err error) uds.ErrorCode {
	t.Helper()
	var herr *uds.HandlerError
	if !errors.As(err, &herr) {
		t.Fatalf("error is not a HandlerError: %v", err)
	}
	return herr.Code
}

func TestHandleYouTubeEmptyQuery(t *testing.T) {
	_, svc, _ := testDeps(t)
	_, err := svc.handleYouTube(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"query": "  "})})
	if handlerErrCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("empty query: code = %v, want bad_request", err)
	}
}

func TestHandleTikTokBadURL(t *testing.T) {
	_, svc, _ := testDeps(t)
	_, err := svc.handleTikTok(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"url": "https://example.com/not-tiktok"})})
	if handlerErrCode(t, err) != uds.ErrDownloadFailed {
		t.Fatalf("bad TikTok URL: code = %v, want download_failed", err)
	}
	if !strings.Contains(err.Error(), "not a TikTok video URL") {
		t.Fatalf("bad TikTok URL: error = %v", err)
	}
	_, err = svc.handleTikTok(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{})})
	if handlerErrCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("missing url: code = %v, want bad_request", err)
	}
}

func TestHandleSpotifyKinds(t *testing.T) {
	_, svc, _ := testDeps(t)
	// A playlist URL is refused with the playlist action named.
	_, err := svc.handleSpotify(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"url": "https://open.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M"})})
	if handlerErrCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("playlist URL: code = %v, want bad_request", err)
	}
	if !strings.Contains(err.Error(), "download.spotify_playlist") {
		t.Fatalf("playlist URL: error = %v", err)
	}
	// Not a Spotify URL at all.
	_, err = svc.handleSpotify(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"url": "https://example.com/x"})})
	if handlerErrCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("non-Spotify URL: code = %v, want bad_request", err)
	}
}

func TestHandleSpotifyUseYouTubeDisabled(t *testing.T) {
	_, svc, _ := testDeps(t)
	// Disable the YouTube ladder in prefs first.
	if _, err := svc.handlePrefs(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"user_id": 42, "use_youtube": false})}); err != nil {
		t.Fatalf("prefs POST: %v", err)
	}
	_, err := svc.handleSpotify(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{
		"url": "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC", "requested_by": 42,
	})})
	if handlerErrCode(t, err) != uds.ErrEngineUnavailable {
		t.Fatalf("use_youtube=false: code = %v, want engine_unavailable", err)
	}
	if !strings.Contains(err.Error(), "use_youtube=false") {
		t.Fatalf("use_youtube=false: error = %v", err)
	}
}

func TestHandlePrefsGetPost(t *testing.T) {
	_, svc, _ := testDeps(t)
	// GET for an unknown user returns defaults, never 404.
	got, err := svc.handlePrefs(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"user_id": 5})})
	if err != nil {
		t.Fatalf("prefs GET: %v", err)
	}
	m, ok := got.(map[string]any)
	if !ok || m["format"] != "mp3" || m["use_youtube"] != true {
		t.Fatalf("prefs GET defaults = %#v", got)
	}
	// POST with an unknown format is refused, naming the formats.
	_, err = svc.handlePrefs(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"user_id": 5, "format": "aac"})})
	if handlerErrCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("bad format: code = %v, want bad_request", err)
	}
	// POST a valid format, then GET it back.
	if _, err := svc.handlePrefs(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"user_id": 5, "format": "flac"})}); err != nil {
		t.Fatalf("prefs POST: %v", err)
	}
	got, err = svc.handlePrefs(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"user_id": 5})})
	if err != nil {
		t.Fatalf("prefs GET after POST: %v", err)
	}
	if m, ok := got.(map[string]any); !ok || m["format"] != "flac" {
		t.Fatalf("prefs after POST = %#v", got)
	}
	// user_id is required.
	_, err = svc.handlePrefs(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{})})
	if handlerErrCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("missing user_id: code = %v, want bad_request", err)
	}
}

func TestHandleProgressAndCancel(t *testing.T) {
	_, svc, _ := testDeps(t)
	// Missing run_id -> bad_request; unknown -> not_found.
	_, err := svc.handleProgress(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{})})
	if handlerErrCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("progress missing run_id: code = %v", err)
	}
	_, err = svc.handleProgress(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"run_id": "nope"})})
	if handlerErrCode(t, err) != uds.ErrNotFound {
		t.Fatalf("progress unknown run: code = %v", err)
	}
	_, err = svc.handleCancel(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"run_id": "nope"})})
	if handlerErrCode(t, err) != uds.ErrNotFound {
		t.Fatalf("cancel unknown run: code = %v", err)
	}
	// A live run snapshots and cancels.
	run := svc.registry.NewRun(context.Background(), "download.youtube", "live")
	got, err := svc.handleProgress(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"run_id": run.ID})})
	if err != nil {
		t.Fatalf("progress live run: %v", err)
	}
	snap, ok := got.(map[string]any)
	if !ok || snap["run_id"] != run.ID || snap["phase"] != string(PhaseSearching) {
		t.Fatalf("progress snapshot = %#v", got)
	}
	cancelled, err := svc.handleCancel(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"run_id": run.ID})})
	if err != nil {
		t.Fatalf("cancel live run: %v", err)
	}
	cm, ok := cancelled.(map[string]any)
	if !ok || cm["cancelled"] != true || cm["phase"] != string(PhaseFailed) {
		t.Fatalf("cancel live run = %#v", cancelled)
	}
	// The run's own context is cancelled.
	select {
	case <-run.Context().Done():
	case <-time.After(time.Second):
		t.Fatal("cancelled run context not done")
	}
	// A finished run answers honestly that it is done (200).
	run2 := svc.registry.NewRun(context.Background(), "download.youtube", "done")
	run2.Finish(svc.registry)
	cancelled, err = svc.handleCancel(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"run_id": run2.ID})})
	if err != nil {
		t.Fatalf("cancel finished run: %v", err)
	}
	if cm, ok := cancelled.(map[string]any); !ok || cm["cancelled"] != false || cm["phase"] != string(PhaseDone) {
		t.Fatalf("cancel finished run = %#v", cancelled)
	}
}

func TestRunJobAsyncReturnsImmediately(t *testing.T) {
	_, svc, _ := testDeps(t)
	done := make(chan struct{})
	got, err := svc.runJob(context.Background(), uds.Request{}, "download.youtube", "async job", true, func(ctx context.Context, run *Run) (map[string]any, error) {
		defer close(done)
		return map[string]any{"file_path": "/tmp/x.mp3"}, nil
	})
	if err != nil {
		t.Fatalf("async runJob: %v", err)
	}
	m, ok := got.(map[string]any)
	if !ok || m["phase"] != string(PhaseSearching) || m["run_id"] == nil || m["run_id"] == "" {
		t.Fatalf("async runJob return = %#v", got)
	}
	// The job completes in the background; the run finishes done.
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("async job did not run")
	}
	// Give the finish a moment to land.
	deadline := time.Now().Add(2 * time.Second)
	for {
		if run, ok := svc.registry.Get(m["run_id"].(string)); ok && run.Phase() == PhaseDone {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("async run did not finish done")
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestRunJobFailureCleansPartials(t *testing.T) {
	_, svc, dataDir := testDeps(t)
	dest := filepath.Join(dataDir, "downloads")
	if err := os.MkdirAll(dest, 0o755); err != nil {
		t.Fatal(err)
	}
	// A pre-existing file must survive; a file the job creates must
	// be deleted on failure.
	if err := os.WriteFile(filepath.Join(dest, "keep.mp3"), []byte("keep"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err := svc.runJob(context.Background(), uds.Request{}, "download.youtube", "failing job", false, func(ctx context.Context, run *Run) (map[string]any, error) {
		if err := os.WriteFile(filepath.Join(dest, "partial.mp3"), []byte("partial"), 0o644); err != nil {
			return nil, err
		}
		return nil, fmt.Errorf("provider exploded")
	})
	if handlerErrCode(t, err) != uds.ErrDownloadFailed {
		t.Fatalf("failing job: code = %v, want download_failed", err)
	}
	if _, err := os.Stat(filepath.Join(dest, "partial.mp3")); !os.IsNotExist(err) {
		t.Fatal("partial file was not deleted on failure")
	}
	if _, err := os.Stat(filepath.Join(dest, "keep.mp3")); err != nil {
		t.Fatal("pre-existing file must survive a failed job")
	}
}

func TestFinishFileRenameAndLibraryRecord(t *testing.T) {
	_, svc, dataDir := testDeps(t)
	dest := filepath.Join(dataDir, "downloads")
	if err := os.MkdirAll(dest, 0o755); err != nil {
		t.Fatal(err)
	}
	src := filepath.Join(dest, "raw [dQw4w9WgXcQ].mp3")
	if err := os.WriteFile(src, []byte("audio-bytes"), 0o644); err != nil {
		t.Fatal(err)
	}
	track := model.Track{ID: "dQw4w9WgXcQ", Title: "Never Gonna Give You Up", Artist: "Rick Astley", Source: "youtube"}
	res := resolver.DownloadResult{Track: track, FilePath: src, SizeBytes: 11}
	path, size, oversize, err := svc.finishFile(77, res, true)
	if err != nil {
		t.Fatalf("finishFile: %v", err)
	}
	if oversize {
		t.Fatal("11-byte file must not be oversize")
	}
	if size != 11 {
		t.Fatalf("size = %d, want 11", size)
	}
	if filepath.Base(path) != "Rick Astley - Never Gonna Give You Up.mp3" {
		t.Fatalf("renamed path = %q (path_template not applied)", path)
	}
	if _, err := os.Stat(src); !os.IsNotExist(err) {
		t.Fatal("original file should be renamed away")
	}
	// The library index records the finished file with its size.
	entry, ok := library.SharedIndex().Get(track.ID)
	if !ok {
		t.Fatal("finished download was not recorded in the library index")
	}
	if entry.SizeBytes != 11 || entry.FilePath != path {
		t.Fatalf("library entry = %+v", entry)
	}
	// Collision: a second file with the same template name is
	// disambiguated with the track id.
	src2 := filepath.Join(dest, "raw2 [other].mp3")
	if err := os.WriteFile(src2, []byte("more"), 0o644); err != nil {
		t.Fatal(err)
	}
	res2 := resolver.DownloadResult{Track: track, FilePath: src2, SizeBytes: 4}
	path2, _, _, err := svc.finishFile(77, res2, true)
	if err != nil {
		t.Fatalf("finishFile collision: %v", err)
	}
	if path2 == path || !strings.Contains(filepath.Base(path2), "dQw4w9WgXcQ") {
		t.Fatalf("collision rename = %q, want the track id in the name", path2)
	}
}

func TestFinishFileOversize(t *testing.T) {
	_, svc, dataDir := testDeps(t)
	dest := filepath.Join(dataDir, "downloads")
	if err := os.MkdirAll(dest, 0o755); err != nil {
		t.Fatal(err)
	}
	// A file over the 50 MB cap is reported oversize, NOT failed.
	res := resolver.DownloadResult{
		Track:     model.Track{ID: "big", Title: "Big", Source: "youtube"},
		FilePath:  filepath.Join(dest, "big.mp3"),
		SizeBytes: MaxBotAPIBYTES + 1,
	}
	if err := os.WriteFile(res.FilePath, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, _, oversize, err := svc.finishFile(1, res, false)
	if err != nil {
		t.Fatalf("finishFile oversize: %v", err)
	}
	if !oversize {
		t.Fatal("over-cap file must report oversize=true")
	}
}

func TestMapDownloadErr(t *testing.T) {
	// Missing credential -> engine_unavailable.
	missing := &resolver.ErrMissingCredential{Source: resolver.SourceSpotify, Missing: "SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET"}
	if got := mapDownloadErr(missing); handlerErrCode(t, got) != uds.ErrEngineUnavailable {
		t.Fatalf("missing credential: code = %v, want engine_unavailable", got)
	}
	// A resolver EngineUnavailable naming "found nothing usable"
	// (Soulseek) is a download_failed, not an unavailable engine.
	soulseekErr := &resolver.EngineUnavailable{Source: resolver.SourceSoulseek, Why: "found nothing usable for this query"}
	if got := mapDownloadErr(soulseekErr); handlerErrCode(t, got) != uds.ErrDownloadFailed {
		t.Fatalf("soulseek nothing-usable: code = %v, want download_failed", got)
	}
	// Any other EngineUnavailable -> engine_unavailable.
	other := &resolver.EngineUnavailable{Source: resolver.SourceYouTube, Why: "daemon is down"}
	if got := mapDownloadErr(other); handlerErrCode(t, got) != uds.ErrEngineUnavailable {
		t.Fatalf("other unavailable: code = %v, want engine_unavailable", got)
	}
	// A context cancel -> download_failed "cancelled by user".
	if got := mapDownloadErr(context.Canceled); handlerErrCode(t, got) != uds.ErrDownloadFailed || !strings.Contains(got.Error(), "cancelled by user") {
		t.Fatalf("cancel: error = %v", got)
	}
	// A plain provider error -> download_failed.
	if got := mapDownloadErr(fmt.Errorf("boom")); handlerErrCode(t, got) != uds.ErrDownloadFailed {
		t.Fatalf("plain error: code = %v, want download_failed", got)
	}
	// A HandlerError passes through untouched.
	orig := uds.NewHandlerError(uds.ErrBadRequest, "nope")
	if got := mapDownloadErr(orig); got != orig {
		t.Fatalf("HandlerError should pass through, got %v", got)
	}
}

func TestCheckDaemonFormat(t *testing.T) {
	if err := checkDaemonFormat(Prefs{Format: "mp3"}); err != nil {
		t.Fatalf("mp3 should pass: %v", err)
	}
	if err := checkDaemonFormat(Prefs{Format: "flac"}); handlerErrCode(t, err) != uds.ErrEngineUnavailable {
		t.Fatalf("flac: code = %v, want engine_unavailable (daemon extracts mp3 only)", err)
	}
}

func TestSpotifyURLKind(t *testing.T) {
	cases := map[string]string{
		"https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC":    "track",
		"https://open.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M": "playlist",
		"https://open.spotify.com/album/1A2GTWGtFfWp7KSQTwWOyo":    "album",
		"https://open.spotify.com/artist/0TnOYISbd1XYRBk9myaseg":   "artist",
		"spotify:track:4uLU6hMCjMI75M1A2tKUQC":                     "track",
		"spotify:album:1A2GTWGtFfWp7KSQTwWOyo":                     "album",
		"https://example.com/not-spotify":                          "",
		"not a url at all":                                         "",
		"":                                                         "",
	}
	for input, want := range cases {
		if got := spotifyURLKind(input); got != want {
			t.Fatalf("spotifyURLKind(%q) = %q, want %q", input, got, want)
		}
	}
}

package recognize

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/uds"
)

func mustJSON(t *testing.T, v any) json.RawMessage {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	return b
}

func errCode(t *testing.T, err error) uds.ErrorCode {
	t.Helper()
	var herr *uds.HandlerError
	if !errors.As(err, &herr) {
		t.Fatalf("error is not a HandlerError: %v", err)
	}
	return herr.Code
}

// auddServer is a fake AudD API. mode: "match" (a match with a
// Spotify id), "match-deezer" (Deezer id only), "match-bare" (no
// provider ids), "nomatch" (result null), "error" (AudD #900).
func auddServer(t *testing.T, mode string) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			w.WriteHeader(http.StatusMethodNotAllowed)
			return
		}
		// The token must arrive as a multipart api_token field.
		if err := r.ParseMultipartForm(32 << 20); err != nil {
			w.WriteHeader(http.StatusBadRequest)
			fmt.Fprint(w, `{"status":"error","error":{"error_code":700,"error_message":"No file sent for recognition"}}`)
			return
		}
		if r.FormValue("api_token") != "dummy-token" {
			fmt.Fprint(w, `{"status":"error","error":{"error_code":900,"error_message":"Invalid API token"}}`)
			return
		}
		if _, _, err := r.FormFile("file"); err != nil {
			fmt.Fprint(w, `{"status":"error","error":{"error_code":700,"error_message":"No file sent for recognition"}}`)
			return
		}
		switch mode {
		case "match":
			fmt.Fprint(w, `{"status":"success","result":{"artist":"Lata Mangeshkar","title":"Lag Ja Gale","album":"Woh Kaun Thi","release_date":"1964","label":"Saregama","timecode":"00:12","song_link":"https://example.com/song","spotify":{"id":"4uLU6hMCjMI75M1A2tKUQC"}}}`)
		case "match-deezer":
			fmt.Fprint(w, `{"status":"success","result":{"artist":"Asha Bhosle","title":"Dum Maro Dum","album":"Hare Rama Hare Krishna","deezer":{"id":123456}}}`)
		case "match-bare":
			fmt.Fprint(w, `{"status":"success","result":{"artist":"Kishore Kumar","title":"Roop Tera Mastana","album":"Aradhana"}}`)
		case "nomatch":
			fmt.Fprint(w, `{"status":"success","result":null}`)
		case "error":
			fmt.Fprint(w, `{"status":"error","error":{"error_code":900,"error_message":"Invalid API token"}}`)
		}
	}))
}

func TestClientMissingToken(t *testing.T) {
	c := NewClient("", "")
	if c.Configured() {
		t.Fatal("empty token should report not-configured")
	}
	_, err := c.Recognize(context.Background(), []byte("audio"), "voice.ogg")
	if err == nil || !strings.Contains(err.Error(), "not configured") {
		t.Fatalf("missing token should fail honestly, got %v", err)
	}
}

func TestClientMatch(t *testing.T) {
	srv := auddServer(t, "match")
	defer srv.Close()
	c := NewClient("dummy-token", srv.URL)
	res, err := c.Recognize(context.Background(), []byte("fake-ogg-bytes"), "voice.ogg")
	if err != nil {
		t.Fatalf("Recognize: %v", err)
	}
	if res == nil {
		t.Fatal("expected a match, got nil")
	}
	if res.Artist != "Lata Mangeshkar" || res.Title != "Lag Ja Gale" || res.Album != "Woh Kaun Thi" {
		t.Fatalf("result = %+v", res)
	}
	if res.SpotifyID != "4uLU6hMCjMI75M1A2tKUQC" {
		t.Fatalf("spotify id = %q", res.SpotifyID)
	}
	if res.Timecode != "00:12" || res.SongLink != "https://example.com/song" {
		t.Fatalf("result = %+v", res)
	}
}

func TestClientNoMatch(t *testing.T) {
	srv := auddServer(t, "nomatch")
	defer srv.Close()
	c := NewClient("dummy-token", srv.URL)
	res, err := c.Recognize(context.Background(), []byte("noise"), "voice.ogg")
	if err != nil {
		t.Fatalf("Recognize no-match: %v", err)
	}
	if res != nil {
		t.Fatalf("no match should return (nil, nil), got %+v", res)
	}
}

func TestClientProviderError(t *testing.T) {
	srv := auddServer(t, "error")
	defer srv.Close()
	c := NewClient("dummy-token", srv.URL)
	_, err := c.Recognize(context.Background(), []byte("audio"), "voice.ogg")
	if err == nil || !strings.Contains(err.Error(), "#900") {
		t.Fatalf("provider error should carry the AudD code, got %v", err)
	}
}

func TestClientWrongToken(t *testing.T) {
	srv := auddServer(t, "match")
	defer srv.Close()
	c := NewClient("wrong-token", srv.URL)
	_, err := c.Recognize(context.Background(), []byte("audio"), "voice.ogg")
	if err == nil || !strings.Contains(err.Error(), "#900") {
		t.Fatalf("wrong token should surface AudD #900, got %v", err)
	}
}

func TestClientTooLarge(t *testing.T) {
	c := NewClient("dummy-token", "http://127.0.0.1:1")
	// 10 MB + 1 byte: refused before any HTTP.
	_, err := c.Recognize(context.Background(), make([]byte, MaxAudDFileBytes+1), "voice.ogg")
	if err == nil || !strings.Contains(err.Error(), "too large") {
		t.Fatalf("oversize audio should fail before any request, got %v", err)
	}
}

func TestClientSendsMultipart(t *testing.T) {
	// The token, return list and file must arrive as multipart
	// form fields (AudD's API shape).
	var gotToken, gotReturn, gotFileName string
	var gotFile []byte
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = r.ParseMultipartForm(32 << 20)
		gotToken = r.FormValue("api_token")
		gotReturn = r.FormValue("return")
		f, hdr, err := r.FormFile("file")
		if err == nil {
			gotFileName = hdr.Filename
			gotFile, _ = io.ReadAll(f)
			_ = f.Close()
		}
		fmt.Fprint(w, `{"status":"success","result":null}`)
	}))
	defer srv.Close()
	c := NewClient("dummy-token", srv.URL)
	if _, err := c.Recognize(context.Background(), []byte("abc"), "note.ogg"); err != nil {
		t.Fatalf("Recognize: %v", err)
	}
	if gotToken != "dummy-token" {
		t.Fatalf("api_token = %q", gotToken)
	}
	if !strings.Contains(gotReturn, "spotify") || !strings.Contains(gotReturn, "deezer") {
		t.Fatalf("return = %q (want the provider ids for resolution)", gotReturn)
	}
	if gotFileName != "note.ogg" || string(gotFile) != "abc" {
		t.Fatalf("file = %q %q", gotFileName, gotFile)
	}
}

// handlerService builds a Service with a client pointed at srv and
// a temp data dir.
func handlerService(t *testing.T, client *Client) (*Service, string) {
	t.Helper()
	dir := t.TempDir()
	dataDir := filepath.Join(dir, "data")
	if err := os.MkdirAll(dataDir, 0o755); err != nil {
		t.Fatal(err)
	}
	cfg := &config.Config{}
	cfg.Backend.DataDir = dataDir
	deps := &app.Deps{Config: cfg}
	return NewServiceWithClient(deps, client), dataDir
}

func TestHandleRecognizeMissingToken(t *testing.T) {
	svc, _ := handlerService(t, NewClient("", ""))
	_, err := svc.handleRecognize(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"file_path": "voice.ogg"})})
	if errCode(t, err) != uds.ErrEngineUnavailable {
		t.Fatalf("missing token: code = %v, want engine_unavailable (502)", err)
	}
	if !strings.Contains(err.Error(), "not configured") {
		t.Fatalf("missing token: error = %v", err)
	}
}

func TestHandleRecognizeValidation(t *testing.T) {
	svc, dataDir := handlerService(t, NewClient("dummy-token", "http://127.0.0.1:1"))
	ctx := context.Background()
	// No file reference -> bad_request.
	_, err := svc.handleRecognize(ctx, uds.Request{Payload: mustJSON(t, map[string]any{})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("no file: code = %v, want bad_request", err)
	}
	// Path traversal -> bad_request.
	_, err = svc.handleRecognize(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"file_path": "../escape.ogg"})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("traversal: code = %v, want bad_request", err)
	}
	// Missing file under the data dir -> not_found.
	_, err = svc.handleRecognize(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"file_path": "missing.ogg"})})
	if errCode(t, err) != uds.ErrNotFound {
		t.Fatalf("missing file: code = %v, want not_found", err)
	}
	// A directory is not an audio file.
	if err := os.MkdirAll(filepath.Join(dataDir, "dir.ogg"), 0o755); err != nil {
		t.Fatal(err)
	}
	_, err = svc.handleRecognize(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"file_path": "dir.ogg"})})
	if err == nil {
		t.Fatal("a directory should not be recognizable")
	}
}

func TestHandleRecognizeMatch(t *testing.T) {
	srv := auddServer(t, "match")
	defer srv.Close()
	svc, dataDir := handlerService(t, NewClient("dummy-token", srv.URL))
	voicePath := filepath.Join(dataDir, "voice.ogg")
	if err := os.WriteFile(voicePath, []byte("fake-ogg-bytes"), 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := svc.handleRecognize(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"file_path": "voice.ogg"})})
	if err != nil {
		t.Fatalf("recognize match: %v", err)
	}
	m, ok := got.(map[string]any)
	if !ok || m["recognized"] != true {
		t.Fatalf("response = %#v", got)
	}
	if m["name"] != "Lag Ja Gale" || m["artists"] != "Lata Mangeshkar" || m["album"] != "Woh Kaun Thi" {
		t.Fatalf("response = %#v", got)
	}
	if m["track"] == nil {
		t.Fatal("a match must carry its track")
	}
}

func TestHandleRecognizeMatchIDs(t *testing.T) {
	// Spotify id -> spotify:<id>, source spotify.
	srv := auddServer(t, "match")
	defer srv.Close()
	svc, dataDir := handlerService(t, NewClient("dummy-token", srv.URL))
	if err := os.WriteFile(filepath.Join(dataDir, "a.ogg"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := svc.handleRecognize(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"file_path": "a.ogg"})})
	if err != nil {
		t.Fatal(err)
	}
	m := got.(map[string]any)
	trackJSON, _ := json.Marshal(m["track"])
	var track struct {
		ID     string `json:"id"`
		Source string `json:"source"`
		Title  string `json:"title"`
	}
	if err := json.Unmarshal(trackJSON, &track); err != nil {
		t.Fatal(err)
	}
	if track.ID != "spotify:4uLU6hMCjMI75M1A2tKUQC" || track.Source != "spotify" {
		t.Fatalf("spotify match track = %+v", track)
	}
	if m["source"] != "spotify" {
		t.Fatalf("source = %v", m["source"])
	}
	// Deezer id only -> deezer:<id>.
	srv2 := auddServer(t, "match-deezer")
	defer srv2.Close()
	svc2, dataDir2 := handlerService(t, NewClient("dummy-token", srv2.URL))
	if err := os.WriteFile(filepath.Join(dataDir2, "b.ogg"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	got, err = svc2.handleRecognize(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"file_path": "b.ogg"})})
	if err != nil {
		t.Fatal(err)
	}
	m = got.(map[string]any)
	trackJSON, _ = json.Marshal(m["track"])
	if err := json.Unmarshal(trackJSON, &track); err != nil {
		t.Fatal(err)
	}
	if track.ID != "deezer:123456" || track.Source != "deezer" {
		t.Fatalf("deezer match track = %+v", track)
	}
	// No provider ids -> an audd:<artist> - <title> label id, never
	// a fake resolvable id.
	srv3 := auddServer(t, "match-bare")
	defer srv3.Close()
	svc3, dataDir3 := handlerService(t, NewClient("dummy-token", srv3.URL))
	if err := os.WriteFile(filepath.Join(dataDir3, "c.ogg"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	got, err = svc3.handleRecognize(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"file_path": "c.ogg"})})
	if err != nil {
		t.Fatal(err)
	}
	m = got.(map[string]any)
	trackJSON, _ = json.Marshal(m["track"])
	if err := json.Unmarshal(trackJSON, &track); err != nil {
		t.Fatal(err)
	}
	if track.ID != "audd:Kishore Kumar - Roop Tera Mastana" || track.Source != "audd" {
		t.Fatalf("bare match track = %+v", track)
	}
}

func TestHandleRecognizeNoMatch(t *testing.T) {
	srv := auddServer(t, "nomatch")
	defer srv.Close()
	svc, dataDir := handlerService(t, NewClient("dummy-token", srv.URL))
	if err := os.WriteFile(filepath.Join(dataDir, "noise.ogg"), []byte("noise"), 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := svc.handleRecognize(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"file_path": "noise.ogg"})})
	if err != nil {
		t.Fatalf("no match: %v", err)
	}
	m, ok := got.(map[string]any)
	if !ok || m["recognized"] != false {
		t.Fatalf("no match response = %#v", got)
	}
}

func TestHandleRecognizeProviderError(t *testing.T) {
	srv := auddServer(t, "error")
	defer srv.Close()
	svc, dataDir := handlerService(t, NewClient("dummy-token", srv.URL))
	if err := os.WriteFile(filepath.Join(dataDir, "v.ogg"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err := svc.handleRecognize(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"file_path": "v.ogg"})})
	if errCode(t, err) != uds.ErrEngineUnavailable {
		t.Fatalf("provider error: code = %v, want engine_unavailable", err)
	}
	if !strings.Contains(err.Error(), "failed temporarily") {
		t.Fatalf("provider error: %v", err)
	}
}

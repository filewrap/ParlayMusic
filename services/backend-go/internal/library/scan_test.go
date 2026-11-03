package library

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"testing"

	"parlay/backend-go/internal/uds"
)

// navidromeServer is a fake Subsonic server for library.scan tests.
// mode controls startScan's answer: "ok", "auth" (Subsonic error 40),
// "http401".
func navidromeServer(t *testing.T, mode string) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/rest/startScan":
			switch mode {
			case "ok":
				fmt.Fprint(w, `{"subsonic-response":{"status":"ok","version":"1.16.1"}}`)
			case "auth":
				fmt.Fprint(w, `{"subsonic-response":{"status":"failed","version":"1.16.1","error":{"code":40,"message":"Wrong username or password"}}}`)
			case "http401":
				w.WriteHeader(http.StatusUnauthorized)
				fmt.Fprint(w, `{"subsonic-response":{"status":"failed","error":{"code":40,"message":"nope"}}}`)
			}
		case "/rest/getAlbumList2":
			fmt.Fprint(w, `{"subsonic-response":{"status":"ok","albumList2":{"album":[{"id":"al1"},{"id":"al2"}]}}}`)
		case "/rest/getAlbum":
			id := r.URL.Query().Get("id")
			n := 0
			if id == "al1" {
				n = 3
			} else if id == "al2" {
				n = 2
			}
			songs := ""
			for i := 0; i < n; i++ {
				if i > 0 {
					songs += ","
				}
				songs += fmt.Sprintf(`{"id":"s%d-%s","title":"Song %d"}`, i, id, i)
			}
			fmt.Fprintf(w, `{"subsonic-response":{"status":"ok","album":{"id":%q,"song":[%s]}}}`, id, songs)
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
}

func TestHandleScanNotConfigured(t *testing.T) {
	svc, _ := testService(t)
	t.Setenv("NAVIDROME_URL", "")
	t.Setenv("NAVIDROME_USER", "")
	t.Setenv("NAVIDROME_PASSWORD", "")
	got, err := svc.handleScan(context.Background(), uds.Request{})
	if err != nil {
		t.Fatalf("not configured must be a 200, not an error: %v", err)
	}
	m, ok := got.(map[string]any)
	if !ok || m["triggered"] != false || m["error"] != "not configured" {
		t.Fatalf("not configured = %#v", got)
	}
}

func TestHandleScanTriggered(t *testing.T) {
	svc, _ := testService(t)
	srv := navidromeServer(t, "ok")
	defer srv.Close()
	t.Setenv("NAVIDROME_URL", srv.URL)
	t.Setenv("NAVIDROME_USER", "user")
	t.Setenv("NAVIDROME_PASSWORD", "pass")
	got, err := svc.handleScan(context.Background(), uds.Request{})
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	m, ok := got.(map[string]any)
	if !ok || m["triggered"] != true {
		t.Fatalf("scan = %#v", got)
	}
	// The refresh count enumerates the fake library: 3 + 2 songs.
	if m["tracks_found"] != 5 {
		t.Fatalf("tracks_found = %v, want 5", m["tracks_found"])
	}
}

func TestHandleScanWrongCredentials(t *testing.T) {
	svc, _ := testService(t)
	srv := navidromeServer(t, "auth")
	defer srv.Close()
	t.Setenv("NAVIDROME_URL", srv.URL)
	t.Setenv("NAVIDROME_USER", "user")
	t.Setenv("NAVIDROME_PASSWORD", "wrong")
	got, err := svc.handleScan(context.Background(), uds.Request{})
	if err != nil {
		t.Fatalf("wrong credentials must be a 200, not an error: %v", err)
	}
	m, ok := got.(map[string]any)
	if !ok || m["triggered"] != false || m["error"] != "wrong credentials" {
		t.Fatalf("wrong credentials = %#v", got)
	}
}

func TestHandleScanUnreachable(t *testing.T) {
	svc, _ := testService(t)
	// A closed server: connection refused -> 502 engine_unavailable.
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	url := srv.URL
	srv.Close()
	t.Setenv("NAVIDROME_URL", url)
	t.Setenv("NAVIDROME_USER", "user")
	t.Setenv("NAVIDROME_PASSWORD", "pass")
	_, err := svc.handleScan(context.Background(), uds.Request{})
	if errCode(t, err) != uds.ErrEngineUnavailable {
		t.Fatalf("unreachable: code = %v, want engine_unavailable", err)
	}
}

func TestHandleShareScan(t *testing.T) {
	svc, _ := testService(t)
	ctx := context.Background()
	// slskd down -> 502 naming SLSKD_URL.
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	url := srv.URL
	srv.Close()
	t.Setenv("SLSKD_URL", url)
	t.Setenv("SLSKD_API_KEY", "")
	_, err := svc.handleShareScan(ctx, uds.Request{})
	if errCode(t, err) != uds.ErrEngineUnavailable {
		t.Fatalf("slskd down: code = %v, want engine_unavailable", err)
	}
	// 200 -> triggered:true (and the API key header is sent when set).
	var gotKey string
	okSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotKey = r.Header.Get("X-API-Key")
		if r.URL.Path != "/api/v0/options/shares/scan" || r.Method != http.MethodPost {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		w.WriteHeader(http.StatusOK)
		fmt.Fprint(w, `{}`)
	}))
	defer okSrv.Close()
	t.Setenv("SLSKD_URL", okSrv.URL)
	t.Setenv("SLSKD_API_KEY", "test-key")
	got, err := svc.handleShareScan(ctx, uds.Request{})
	if err != nil {
		t.Fatalf("sharescan 200: %v", err)
	}
	if m, ok := got.(map[string]any); !ok || m["triggered"] != true {
		t.Fatalf("sharescan 200 = %#v", got)
	}
	if gotKey != "test-key" {
		t.Fatalf("X-API-Key = %q, want the configured key", gotKey)
	}
	// 409 (scan already running) -> 200 {triggered:false}, no
	// invented minute estimate.
	conflictSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusConflict)
		fmt.Fprint(w, `{}`)
	}))
	defer conflictSrv.Close()
	t.Setenv("SLSKD_URL", conflictSrv.URL)
	got, err = svc.handleShareScan(ctx, uds.Request{})
	if err != nil {
		t.Fatalf("sharescan 409: %v", err)
	}
	if m, ok := got.(map[string]any); !ok || m["triggered"] != false {
		t.Fatalf("sharescan 409 = %#v", got)
	}
	if m, ok := got.(map[string]any); ok {
		if _, has := m["scheduled_in_min"]; has {
			t.Fatal("scheduled_in_min must be omitted — the slskd API exposes no minute estimate")
		}
	}
}

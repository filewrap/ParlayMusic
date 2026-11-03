package search

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/library"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/store"
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

// testService builds a Service with a real store (for np/s/l), an
// explicit index, and no resolver registry (inline search then only
// exercises the Lemonic client — no network to real providers).
func testService(t *testing.T, lemonic *LemonicClient) (*Service, *store.Store, *library.Index) {
	t.Helper()
	dir := t.TempDir()
	st, err := store.Open(filepath.Join(dir, "store"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = st.Close() })
	cfg := &config.Config{}
	cfg.Backend.DataDir = filepath.Join(dir, "data")
	deps := &app.Deps{Store: st, Config: cfg, Registry: nil}
	index := library.NewIndex()
	svc := NewServiceWithLemonic(deps, lemonic, index)
	return svc, st, index
}

func TestInlineMode(t *testing.T) {
	cases := []struct{ query, mode, arg string }{
		{"", "hint", ""},
		{"  ", "hint", ""},
		{"np", "np", ""},
		{"NP", "np", ""},
		{"s", "s", ""},
		{"l", "l", ""},
		{"help", "help", ""},
		{"HELP", "help", ""},
		{"lib lata", "lib", "lata"},
		{"LIB Lata Mangeshkar", "lib", "Lata Mangeshkar"},
		{"del song name", "del", "song name"},
		{"search never gonna", "search", "never gonna"},
		{"lib", "lib", ""},
		{"del", "del", ""},
		{"search", "search", ""},
		{"ab", "hint", "ab"},
		{"a", "hint", "a"},
		{"lag ja gale", "search", "lag ja gale"},
		{"never gonna give you up", "search", "never gonna give you up"},
		{"nps", "search", "nps"},
		{"libx", "search", "libx"},
	}
	for _, tc := range cases {
		mode, arg := inlineMode(tc.query)
		if mode != tc.mode || arg != tc.arg {
			t.Fatalf("inlineMode(%q) = (%q, %q), want (%q, %q)", tc.query, mode, arg, tc.mode, tc.arg)
		}
	}
}

func TestHandleInlineNowPlaying(t *testing.T) {
	svc, st, _ := testService(t, nil)
	ctx := context.Background()
	chatID := int64(555)
	// Nothing playing -> [] (never an error).
	for _, mode := range []string{"np", "s", "l"} {
		got, err := svc.handleInline(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"query": mode, "chat_id": chatID})})
		if err != nil {
			t.Fatalf("inline %s empty: %v", mode, err)
		}
		m, ok := got.(map[string]any)
		if !ok || m["mode"] != mode {
			t.Fatalf("inline %s = %#v", mode, got)
		}
		if results, ok := m["results"].([]model.Track); !ok || len(results) != 0 {
			t.Fatalf("inline %s empty results = %#v", mode, m["results"])
		}
	}
	// No chat_id at all -> [] too.
	got, err := svc.handleInline(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"query": "np"})})
	if err != nil {
		t.Fatalf("inline np no chat: %v", err)
	}
	if m, ok := got.(map[string]any); !ok || len(m["results"].([]model.Track)) != 0 {
		t.Fatalf("inline np no chat = %#v", got)
	}
	// A playing room -> [current track] for np/s/l.
	track := model.Track{ID: "yt:now", Title: "Now Playing", Artist: "A", Source: "youtube"}
	if err := st.PutRoom(model.RoomState{ChatID: chatID, Current: &track}); err != nil {
		t.Fatal(err)
	}
	for _, mode := range []string{"np", "s", "l"} {
		got, err := svc.handleInline(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"query": mode, "chat_id": chatID})})
		if err != nil {
			t.Fatalf("inline %s playing: %v", mode, err)
		}
		m := got.(map[string]any)
		results := m["results"].([]model.Track)
		if len(results) != 1 || results[0].ID != "yt:now" {
			t.Fatalf("inline %s playing results = %#v", mode, m["results"])
		}
	}
}

func TestHandleInlineHintHelp(t *testing.T) {
	svc, _, _ := testService(t, nil)
	ctx := context.Background()
	for _, q := range []string{"", "ab", "help", "search"} {
		got, err := svc.handleInline(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"query": q})})
		if err != nil {
			t.Fatalf("inline %q: %v", q, err)
		}
		m := got.(map[string]any)
		if results, ok := m["results"].([]model.Track); !ok || len(results) != 0 {
			t.Fatalf("inline %q results = %#v (want empty)", q, m["results"])
		}
	}
	// Bare lib/del (no argument) return empty library results, not
	// an error.
	for _, q := range []string{"lib", "del"} {
		got, err := svc.handleInline(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"query": q})})
		if err != nil {
			t.Fatalf("inline %q: %v", q, err)
		}
		m := got.(map[string]any)
		if results, ok := m["results"].([]libraryResult); !ok || len(results) != 0 {
			t.Fatalf("inline %q results = %#v (want empty)", q, m["results"])
		}
		if m["mode"] != q {
			t.Fatalf("inline %q mode = %v", q, m["mode"])
		}
	}
}

func TestHandleInlineLibrary(t *testing.T) {
	svc, _, index := testService(t, nil)
	ctx := context.Background()
	dir := t.TempDir()
	p1 := filepath.Join(dir, "lata.mp3")
	p2 := filepath.Join(dir, "asha.mp3")
	if err := os.WriteFile(p1, []byte("a"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p2, []byte("b"), 0o644); err != nil {
		t.Fatal(err)
	}
	index.Record(model.Track{ID: "yt:1", Title: "Lag Ja Gale", Artist: "Lata Mangeshkar", Source: "youtube"}, p1, 1234)
	index.Record(model.Track{ID: "yt:2", Title: "Dum Maro Dum", Artist: "Asha Bhosle", Source: "youtube"}, p2, 2345)
	// lib mode returns the matching entry with its file path and
	// size (the core sends the local file; no fake file_id).
	got, err := svc.handleInline(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"query": "lib lata"})})
	if err != nil {
		t.Fatalf("inline lib: %v", err)
	}
	m := got.(map[string]any)
	if m["mode"] != "lib" {
		t.Fatalf("inline lib mode = %v", m["mode"])
	}
	results, ok := m["results"].([]libraryResult)
	if !ok || len(results) != 1 {
		t.Fatalf("inline lib results = %#v", m["results"])
	}
	if results[0].FilePath != p1 || results[0].SizeBytes != 1234 || !results[0].Cached {
		t.Fatalf("inline lib result = %+v", results[0])
	}
	if results[0].CachedFileID != "" {
		t.Fatalf("cached_file_id must stay empty (no real file_id known), got %q", results[0].CachedFileID)
	}
	// del mode returns the same entries (the core renders delete
	// buttons; library.delete does the deleting).
	got, err = svc.handleInline(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"query": "del dum"})})
	if err != nil {
		t.Fatalf("inline del: %v", err)
	}
	m = got.(map[string]any)
	if m["mode"] != "del" {
		t.Fatalf("inline del mode = %v", m["mode"])
	}
	results, ok = m["results"].([]libraryResult)
	if !ok || len(results) != 1 || results[0].Title != "Dum Maro Dum" {
		t.Fatalf("inline del results = %#v", m["results"])
	}
	// No match -> empty, not an error.
	got, err = svc.handleInline(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"query": "lib zzz-no-match"})})
	if err != nil {
		t.Fatalf("inline lib no match: %v", err)
	}
	m = got.(map[string]any)
	if results, ok := m["results"].([]libraryResult); !ok || len(results) != 0 {
		t.Fatalf("inline lib no match = %#v", m["results"])
	}
}

func TestHandleInlineSearchBothFail(t *testing.T) {
	// No registry (YouTube skipped) and a Lemonic client pointed at
	// a dead server: inline search never errors, it returns [].
	svc, _, _ := testService(t, NewLemonicClientWithBase("http://127.0.0.1:1", ""))
	got, err := svc.handleInline(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"query": "never gonna give you up"})})
	if err != nil {
		t.Fatalf("inline search with failing sources must not error: %v", err)
	}
	m := got.(map[string]any)
	if m["mode"] != "search" {
		t.Fatalf("mode = %v, want search", m["mode"])
	}
	if results, ok := m["results"].([]model.Track); !ok || len(results) != 0 {
		t.Fatalf("results = %#v (want empty)", m["results"])
	}
}

package share

import (
	"encoding/json"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

func testService(t *testing.T) *Service {
	t.Helper()
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return newService(st, "parlaybot")
}

func TestCreateAndResolve(t *testing.T) {
	svc := testService(t)
	track := model.Track{ID: "yt:abc", Title: "Test Song", Artist: "Artist", Source: "youtube"}
	payload, _ := json.Marshal(map[string]any{"track": track, "created_by": 123})
	data, err := svc.handleCreate(t.Context(), uds.Request{Payload: payload})
	if err != nil {
		t.Fatalf("share.create: %v", err)
	}
	m := data.(map[string]any)
	token, _ := m["token"].(string)
	link, _ := m["link"].(string)
	if len(token) != 16 {
		t.Fatalf("token = %q, want 16 chars", token)
	}
	if link != "https://t.me/parlaybot?start=share-"+token {
		t.Fatalf("link = %q", link)
	}
	payload, _ = json.Marshal(map[string]any{"token": token})
	data, err = svc.handleResolve(t.Context(), uds.Request{Payload: payload})
	if err != nil {
		t.Fatalf("share.resolve: %v", err)
	}
	got := data.(map[string]any)["track"].(model.Track)
	if got.ID != "yt:abc" || got.Title != "Test Song" {
		t.Fatalf("resolved track = %+v", got)
	}
	// A resolved token is NOT consumed: the same link works again.
	if _, err := svc.handleResolve(t.Context(), uds.Request{Payload: payload}); err != nil {
		t.Fatalf("second resolve: %v", err)
	}
}

func TestCreateValidation(t *testing.T) {
	svc := testService(t)
	cases := []string{
		`{"track":{"id":"","title":"x"},"created_by":1}`,
		`{"track":{"id":"yt:1","title":""},"created_by":1}`,
		`{"track":{"id":"yt:1","title":"x"}}`,
		`{"track":{"id":"yt:1","title":"x"},"created_by":0}`,
	}
	for _, c := range cases {
		if _, err := svc.handleCreate(t.Context(), uds.Request{Payload: json.RawMessage(c)}); err == nil {
			t.Fatalf("share.create %s succeeded, want bad_request", c)
		} else if herr, ok := err.(*uds.HandlerError); !ok || herr.Code != uds.ErrBadRequest {
			t.Fatalf("share.create %s: err=%v, want bad_request", c, err)
		}
	}
}

func TestResolveUnknownToken(t *testing.T) {
	svc := testService(t)
	_, err := svc.handleResolve(t.Context(), uds.Request{Payload: json.RawMessage(`{"token":"doesnotexist1234"}`)})
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrNotFound {
		t.Fatalf("unknown token: err=%v, want not_found", err)
	}
	_, err = svc.handleResolve(t.Context(), uds.Request{Payload: json.RawMessage(`{"token":""}`)})
	herr, ok = err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrBadRequest {
		t.Fatalf("empty token: err=%v, want bad_request", err)
	}
}

func TestLinkWithoutBotUsername(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	svc := newService(st, "")
	if got := svc.linkFor("abcdefghijklmnop"); got != "https://t.me/?start=share-abcdefghijklmnop" {
		t.Fatalf("bare link = %q", got)
	}
}

func TestRegisterDoesNotPanic(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	server := uds.NewServer(t.TempDir() + "/s.sock")
	Register(server, &app.Deps{Store: st})
}

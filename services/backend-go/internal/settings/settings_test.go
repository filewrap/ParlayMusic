package settings

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
	return &Service{store: st}
}

func call(t *testing.T, svc *Service, payload string) map[string]any {
	t.Helper()
	data, err := svc.handleSettings(t.Context(), uds.Request{Payload: json.RawMessage(payload)})
	if err != nil {
		t.Fatalf("chat.settings %s: %v", payload, err)
	}
	return data.(map[string]any)
}

func TestDefaultsAll21Keys(t *testing.T) {
	svc := testService(t)
	m := call(t, svc, `{"chat_id":-100}`)
	settings, ok := m["settings"].(map[string]any)
	if !ok {
		t.Fatalf("settings missing: %v", m)
	}
	if len(settings) != 21 {
		t.Fatalf("settings has %d keys, want 21: %v", len(settings), settings)
	}
	want := map[string]any{
		"admin_mode": 0, "play_mode": "everyone", "cmd_delete": false,
		"clean_mode": false, "clean_delay_min": 0, "thumbnails": true,
		"autoplay": false, "autoplay_lang": "", "autoplay_mood": "",
		"silent_mode": false, "auto_delete": false, "auto_delete_min": 5,
		"language": "en", "paginate_purge_old": true, "chat_music_download": true,
	}
	for k, v := range want {
		if settings[k] != v {
			t.Fatalf("default %s = %v (%T), want %v", k, settings[k], settings[k], v)
		}
	}
	// The quality overrides default to null (no per-chat
	// override): the effective config falls through to the
	// file/profile/default.
	for _, k := range []string{"quality_bitrate_bps", "quality_frame_ms", "quality_complexity", "quality_vbr", "quality_fec", "quality_dtx"} {
		if settings[k] != nil {
			t.Fatalf("default %s = %v, want null (no override)", k, settings[k])
		}
	}
}

func TestSetAndGet(t *testing.T) {
	svc := testService(t)
	m := call(t, svc, `{"chat_id":-100,"key":"play_mode","value":"admins"}`)
	if m["settings"].(map[string]any)["play_mode"] != "admins" {
		t.Fatalf("set play_mode: %v", m)
	}
	// GET one key returns that key only, still under "settings".
	m = call(t, svc, `{"chat_id":-100,"key":"play_mode"}`)
	settings := m["settings"].(map[string]any)
	if len(settings) != 1 || settings["play_mode"] != "admins" {
		t.Fatalf("get one key: %v", m)
	}
	// The set persisted: a fresh read returns it.
	m = call(t, svc, `{"chat_id":-100}`)
	if m["settings"].(map[string]any)["play_mode"] != "admins" {
		t.Fatalf("persisted play_mode: %v", m)
	}
}

func TestCleanModeAliasSync(t *testing.T) {
	svc := testService(t)
	m := call(t, svc, `{"chat_id":-100,"key":"clean_delay_min","value":30}`)
	settings := m["settings"].(map[string]any)
	if settings["clean_delay_min"] != 30 || settings["clean_mode"] != true {
		t.Fatalf("clean_delay_min=30 must turn clean_mode on: %v", settings)
	}
	m = call(t, svc, `{"chat_id":-100,"key":"clean_mode","value":false}`)
	settings = m["settings"].(map[string]any)
	if settings["clean_mode"] != false || settings["clean_delay_min"] != 0 {
		t.Fatalf("clean_mode=false must zero the delay: %v", settings)
	}
	m = call(t, svc, `{"chat_id":-100,"key":"clean_mode","value":true}`)
	settings = m["settings"].(map[string]any)
	if settings["clean_mode"] != true || settings["clean_delay_min"] != 15 {
		t.Fatalf("clean_mode=true with no delay must default to 15: %v", settings)
	}
}

func TestAutoplayWriteThroughToRoom(t *testing.T) {
	svc := testService(t)
	st := svc.store
	room := model.DefaultRoomState(-100)
	if err := st.PutRoom(room); err != nil {
		t.Fatalf("PutRoom: %v", err)
	}
	call(t, svc, `{"chat_id":-100,"key":"autoplay","value":true}`)
	got, _, err := st.GetRoom(-100)
	if err != nil {
		t.Fatalf("GetRoom: %v", err)
	}
	if !got.Autoplay {
		t.Fatalf("autoplay did not write through to the room")
	}
}

func TestValidation(t *testing.T) {
	svc := testService(t)
	cases := []string{
		`{"chat_id":-100,"key":"bogus","value":1}`,
		`{"chat_id":-100,"key":"admin_mode","value":5}`,
		`{"chat_id":-100,"key":"admin_mode","value":"x"}`,
		`{"chat_id":-100,"key":"play_mode","value":"nobody"}`,
		`{"chat_id":-100,"key":"auto_delete_min","value":1}`,
		`{"chat_id":-100,"key":"auto_delete_min","value":9}`,
		`{"chat_id":-100,"key":"language","value":"hi"}`,
		`{"chat_id":-100,"key":"thumbnails","value":"maybe"}`,
		`{"key":"thumbnails","value":true}`,
		`{"chat_id":-100,"key":"bogus"}`,
	}
	for _, c := range cases {
		_, err := svc.handleSettings(t.Context(), uds.Request{Payload: json.RawMessage(c)})
		herr, ok := err.(*uds.HandlerError)
		if !ok || herr.Code != uds.ErrBadRequest {
			t.Fatalf("case %s: err=%v, want bad_request", c, err)
		}
	}
	// String forms are accepted where they are unambiguous.
	m := call(t, svc, `{"chat_id":-100,"key":"cmd_delete","value":"on"}`)
	if m["settings"].(map[string]any)["cmd_delete"] != true {
		t.Fatalf(`"on" for a bool key: %v`, m)
	}
	m = call(t, svc, `{"chat_id":-100,"key":"clean_delay_min","value":"30"}`)
	if m["settings"].(map[string]any)["clean_delay_min"] != 30 {
		t.Fatalf(`"30" for an int key: %v`, m)
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

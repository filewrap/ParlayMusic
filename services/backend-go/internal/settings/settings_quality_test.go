package settings

import (
	"encoding/json"
	"testing"

	"parlay/backend-go/internal/uds"
)

// Quality overrides: set/get/persist per chat, validated with
// the voice bounds, cleared back to null with a JSON null set.

func TestQualityKeysSetAndGet(t *testing.T) {
	svc := testService(t)
	m := call(t, svc, `{"chat_id":-100,"key":"quality_bitrate_bps","value":128000}`)
	if m["settings"].(map[string]any)["quality_bitrate_bps"] != 128000 {
		t.Fatalf("set quality_bitrate_bps: %v", m)
	}
	m = call(t, svc, `{"chat_id":-100,"key":"quality_frame_ms","value":40}`)
	if m["settings"].(map[string]any)["quality_frame_ms"] != 40 {
		t.Fatalf("set quality_frame_ms: %v", m)
	}
	m = call(t, svc, `{"chat_id":-100,"key":"quality_complexity","value":7}`)
	if m["settings"].(map[string]any)["quality_complexity"] != 7 {
		t.Fatalf("set quality_complexity: %v", m)
	}
	m = call(t, svc, `{"chat_id":-100,"key":"quality_vbr","value":false}`)
	if m["settings"].(map[string]any)["quality_vbr"] != false {
		t.Fatalf("set quality_vbr: %v", m)
	}
	m = call(t, svc, `{"chat_id":-100,"key":"quality_fec","value":true}`)
	if m["settings"].(map[string]any)["quality_fec"] != true {
		t.Fatalf("set quality_fec: %v", m)
	}
	// GET one quality key returns that key only.
	m = call(t, svc, `{"chat_id":-100,"key":"quality_bitrate_bps"}`)
	settings := m["settings"].(map[string]any)
	if len(settings) != 1 || settings["quality_bitrate_bps"] != 128000 {
		t.Fatalf("get one quality key: %v", m)
	}
	// The sets persisted: a fresh read returns them all.
	m = call(t, svc, `{"chat_id":-100}`)
	settings = m["settings"].(map[string]any)
	if settings["quality_bitrate_bps"] != 128000 || settings["quality_frame_ms"] != 40 ||
		settings["quality_complexity"] != 7 || settings["quality_vbr"] != false ||
		settings["quality_fec"] != true || settings["quality_dtx"] != nil {
		t.Fatalf("persisted quality keys: %v", settings)
	}
	// Another chat is unaffected (per-chat overrides).
	m = call(t, svc, `{"chat_id":-200}`)
	if m["settings"].(map[string]any)["quality_bitrate_bps"] != nil {
		t.Fatalf("quality override leaked across chats: %v", m)
	}
}

func TestQualityKeysValidation(t *testing.T) {
	svc := testService(t)
	cases := []string{
		`{"chat_id":-100,"key":"quality_bitrate_bps","value":100}`,
		`{"chat_id":-100,"key":"quality_bitrate_bps","value":600000}`,
		`{"chat_id":-100,"key":"quality_bitrate_bps","value":"loud"}`,
		`{"chat_id":-100,"key":"quality_frame_ms","value":30}`,
		`{"chat_id":-100,"key":"quality_frame_ms","value":0}`,
		`{"chat_id":-100,"key":"quality_complexity","value":11}`,
		`{"chat_id":-100,"key":"quality_complexity","value":-1}`,
		`{"chat_id":-100,"key":"quality_vbr","value":"maybe"}`,
	}
	for _, c := range cases {
		_, err := svc.handleSettings(t.Context(), uds.Request{Payload: json.RawMessage(c)})
		herr, ok := err.(*uds.HandlerError)
		if !ok || herr.Code != uds.ErrBadRequest {
			t.Fatalf("case %s: err=%v, want bad_request", c, err)
		}
	}
	// A rejected set leaves the stored value untouched (null).
	m := call(t, svc, `{"chat_id":-100}`)
	if m["settings"].(map[string]any)["quality_bitrate_bps"] != nil {
		t.Fatalf("rejected set must not store: %v", m)
	}
	// Edges are accepted.
	for _, c := range []string{
		`{"chat_id":-100,"key":"quality_bitrate_bps","value":6000}`,
		`{"chat_id":-100,"key":"quality_bitrate_bps","value":510000}`,
		`{"chat_id":-100,"key":"quality_frame_ms","value":10}`,
		`{"chat_id":-100,"key":"quality_frame_ms","value":60}`,
		`{"chat_id":-100,"key":"quality_complexity","value":0}`,
	} {
		if _, err := svc.handleSettings(t.Context(), uds.Request{Payload: json.RawMessage(c)}); err != nil {
			t.Fatalf("edge %s must be accepted: %v", c, err)
		}
	}
}

func TestQualityKeysNullClearsOverride(t *testing.T) {
	svc := testService(t)
	call(t, svc, `{"chat_id":-100,"key":"quality_bitrate_bps","value":256000}`)
	call(t, svc, `{"chat_id":-100,"key":"quality_frame_ms","value":40}`)
	// JSON null clears the override (falls back to the file).
	m := call(t, svc, `{"chat_id":-100,"key":"quality_bitrate_bps","value":null}`)
	if m["settings"].(map[string]any)["quality_bitrate_bps"] != nil {
		t.Fatalf("null set must clear quality_bitrate_bps: %v", m)
	}
	m = call(t, svc, `{"chat_id":-100}`)
	settings := m["settings"].(map[string]any)
	if settings["quality_bitrate_bps"] != nil {
		t.Fatalf("cleared override persisted: %v", settings)
	}
	// The other override is untouched.
	if settings["quality_frame_ms"] != 40 {
		t.Fatalf("clearing one key must not clear the others: %v", settings)
	}
	// Null on a non-quality key is still a GET, not a clear.
	m = call(t, svc, `{"chat_id":-100,"key":"thumbnails","value":null}`)
	settings = m["settings"].(map[string]any)
	if len(settings) != 1 || settings["thumbnails"] != true {
		t.Fatalf("null on a non-quality key must be a GET: %v", m)
	}
}

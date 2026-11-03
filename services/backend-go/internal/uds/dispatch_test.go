package uds

import (
	"context"
	"encoding/json"
	"testing"
	"time"
)

// TestDispatchReusesHandlers proves the in-process entry point
// (Phase 6: the HTTP server's transport into the vc.* / gui.*
// actions) runs the registered handler with the same lookup rules
// as the socket path, and answers the same not_found for unknown
// actions and for test.sleep_ms.
func TestDispatchReusesHandlers(t *testing.T) {
	s := NewServer("")
	s.Register("echo.test", func(_ context.Context, req Request) (any, error) {
		return map[string]any{"chat_id": req.ChatID, "payload": string(req.Payload)}, nil
	})

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	data, err := s.Dispatch(ctx, Request{
		Action:  "echo.test",
		ChatID:  -1001,
		Payload: json.RawMessage(`{"chat_id":-1001}`),
	})
	if err != nil {
		t.Fatalf("Dispatch registered action: %v", err)
	}
	got, _ := data.(map[string]any)
	if got["chat_id"] != int64(-1001) {
		t.Fatalf("Dispatch did not run the handler with the request: %+v", got)
	}

	for _, action := range []string{"vc.bogus", "test.sleep_ms"} {
		_, err := s.Dispatch(ctx, Request{Action: action})
		herr, ok := err.(*HandlerError)
		if !ok || herr.Code != ErrNotFound {
			t.Fatalf("Dispatch %q: err=%v, want not_found HandlerError", action, err)
		}
	}

	// A cancelled context reaches the handler: Dispatch does not
	// swallow it or substitute its own deadline.
	cancelled, stop := context.WithCancel(context.Background())
	stop()
	_, err = s.Dispatch(cancelled, Request{Action: "echo.test"})
	if err != nil {
		t.Fatalf("Dispatch with cancelled context must still run the handler (it returns data, no error): %v", err)
	}
}

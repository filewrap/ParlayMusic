package vote

import (
	"encoding/json"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

func testService(t *testing.T) *Service {
	t.Helper()
	t.Setenv("PARLAY_VOTE_THRESHOLD", "")
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return &Service{store: st, room: room.NewManager(st, 0)}
}

func cast(t *testing.T, svc *Service, chatID int64, track model.Track, vote string, voter any) map[string]any {
	t.Helper()
	payload, _ := json.Marshal(map[string]any{
		"chat_id": chatID, "track": track, "vote": vote, "voter_id": voter,
	})
	data, err := svc.handleVote(t.Context(), uds.Request{ChatID: chatID, Payload: payload})
	if err != nil {
		t.Fatalf("queue.vote: %v", err)
	}
	return data.(map[string]any)
}

func TestVoteThresholdBehaviour(t *testing.T) {
	svc := testService(t)
	track := model.Track{ID: "yt:abc", Title: "Song", Artist: "A", Source: "youtube"}
	// Default threshold is 3.
	if got := cast(t, svc, -100, track, "approve", 1); got["queued"] != false || got["approvals"] != 1 {
		t.Fatalf("after 1 approval: %v", got)
	}
	if got := cast(t, svc, -100, track, "approve", 2); got["queued"] != false || got["approvals"] != 2 {
		t.Fatalf("after 2 approvals: %v", got)
	}
	got := cast(t, svc, -100, track, "approve", 3)
	if got["queued"] != true || got["approvals"] != 3 || got["threshold"] != 3 {
		t.Fatalf("after 3 approvals: %v", got)
	}
	// The track reached threshold: it is in the room queue now —
	// visible both through the store (persisted) and through the
	// shared room.Manager (the single owner of room state).
	queue, err := svc.store.GetQueue(-100)
	if err != nil {
		t.Fatalf("GetQueue: %v", err)
	}
	if len(queue) != 1 || queue[0].Track.ID != "yt:abc" {
		t.Fatalf("queue after threshold: %+v", queue)
	}
	if rq, ok := svc.room.Queue(-100); !ok || len(rq) != 1 || rq[0].Track.ID != "yt:abc" {
		t.Fatalf("room.Manager queue after threshold: %+v ok=%v", rq, ok)
	}
	// A decline from one approver drops it back below threshold...
	got = cast(t, svc, -100, track, "decline", 3)
	if got["queued"] != false || got["approvals"] != 2 || got["declines"] != 1 {
		t.Fatalf("after decline: %v", got)
	}
	// ...but the already-enqueued track is NOT removed (the core
	// must not remove an already-enqueued track on that signal).
	queue, _ = svc.store.GetQueue(-100)
	if len(queue) != 1 {
		t.Fatalf("queue after drop below threshold: %+v (track must stay)", queue)
	}
}

func TestVoteReVoteIsNoOp(t *testing.T) {
	svc := testService(t)
	track := model.Track{ID: "yt:rep", Title: "Song", Artist: "A", Source: "youtube"}
	cast(t, svc, -100, track, "approve", 1)
	got := cast(t, svc, -100, track, "approve", 1)
	if got["approvals"] != 1 || got["queued"] != false {
		t.Fatalf("re-vote with the same value: %v", got)
	}
}

func TestVoteThresholdFromEnv(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	t.Setenv("PARLAY_VOTE_THRESHOLD", "1")
	svc := &Service{store: st, room: room.NewManager(st, 0)}
	track := model.Track{ID: "yt:one", Title: "Song", Artist: "A", Source: "youtube"}
	got := cast(t, svc, -100, track, "approve", 9)
	if got["queued"] != true || got["threshold"] != 1 {
		t.Fatalf("threshold=1: %v", got)
	}
}

func TestVoteValidation(t *testing.T) {
	svc := testService(t)
	track := model.Track{ID: "yt:v", Title: "Song", Artist: "A", Source: "youtube"}
	cases := []map[string]any{
		{"chat_id": -100, "track": track, "vote": "maybe", "voter_id": 1},
		{"chat_id": -100, "track": model.Track{Title: "No ID"}, "vote": "approve", "voter_id": 1},
		{"chat_id": -100, "track": track, "vote": "approve"},
		{"chat_id": -100, "track": track, "vote": "approve", "voter_id": 0},
		{"track": track, "vote": "approve", "voter_id": 1},
	}
	for _, c := range cases {
		payload, _ := json.Marshal(c)
		_, err := svc.handleVote(t.Context(), uds.Request{Payload: payload})
		herr, ok := err.(*uds.HandlerError)
		if !ok || herr.Code != uds.ErrBadRequest {
			t.Fatalf("case %v: err=%v, want bad_request", c, err)
		}
	}
	// voter_id as a numeric string is accepted (queue-rust rule).
	got := cast(t, svc, -100, track, "approve", "42")
	if got["approvals"] != 1 {
		t.Fatalf("string voter_id: %v", got)
	}
}

func TestRegisterDoesNotPanic(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	server := uds.NewServer(t.TempDir() + "/s.sock")
	Register(server, &app.Deps{Store: st, Room: room.NewManager(st, 0)})
}

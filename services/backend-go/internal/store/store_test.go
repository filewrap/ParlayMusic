package store

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"parlay/backend-go/internal/model"
)

func mustTrack(id, title string) model.Track {
	return model.Track{ID: id, Title: title, Source: "youtube", Artist: "A", DurationSec: 268}
}

// TestStoreCRUD covers every bucket: write, read back, overwrite,
// delete, and the not-found path.
func TestStoreCRUD(t *testing.T) {
	dir := t.TempDir()
	// Open creates the data dir: check permissions on a directory
	// it creates itself (t.TempDir already exists with the
	// sandbox's own mode, which Open must not chmod).
	dataDir := filepath.Join(dir, "data")
	st, err := Open(dataDir)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()

	// File/dir permissions: 0600 file in a 0700 dir (§7 convention).
	if fi, err := os.Stat(st.Path()); err != nil || fi.Mode().Perm() != 0o600 {
		t.Errorf("parlay.db must be 0600, got %v (err %v)", fi.Mode(), err)
	}
	if fi, err := os.Stat(dataDir); err != nil || fi.Mode().Perm() != 0o700 {
		t.Errorf("data dir must be 0700, got %v (err %v)", fi.Mode(), err)
	}

	// Rooms.
	room := model.DefaultRoomState(-1001)
	room.Current = func() *model.Track { tr := mustTrack("t1", "Song One"); return &tr }()
	room.Loop = 3
	room.FXPreset = "nightcore"
	if err := st.PutRoom(room); err != nil {
		t.Fatal(err)
	}
	got, found, err := st.GetRoom(-1001)
	if err != nil || !found {
		t.Fatalf("GetRoom: found=%v err=%v", found, err)
	}
	if got.Loop != 3 || got.Current == nil || got.Current.Title != "Song One" || got.FXPreset != "nightcore" {
		t.Errorf("room round-trip lost data: %+v", got)
	}
	if _, found, _ := st.GetRoom(-9999); found {
		t.Error("GetRoom must report false for an unknown chat")
	}

	// Queue (per-room sub-bucket, play order preserved).
	queue := []model.QueueItem{
		{RequestedBy: 111, Track: mustTrack("t2", "Song Two")},
		{RequestedBy: 222, Track: mustTrack("t3", "Song Three")},
	}
	if err := st.PutQueue(-1001, queue); err != nil {
		t.Fatal(err)
	}
	back, err := st.GetQueue(-1001)
	if err != nil {
		t.Fatal(err)
	}
	if len(back) != 2 || back[0].Track.ID != "t2" || back[0].Position != 0 || back[1].Position != 1 {
		t.Errorf("queue round-trip wrong: %+v", back)
	}
	// A shorter rewrite must not leave a stale tail.
	if err := st.PutQueue(-1001, queue[:1]); err != nil {
		t.Fatal(err)
	}
	back, _ = st.GetQueue(-1001)
	if len(back) != 1 {
		t.Errorf("shorter queue rewrite left stale items: %+v", back)
	}

	// Votes.
	vote := model.VoteState{ChatID: -1001, Track: mustTrack("t4", "Vote Song"), Threshold: 3, Approvals: 2, Voters: map[int64]string{111: "approve", 222: "approve"}}
	if err := st.PutVote(vote); err != nil {
		t.Fatal(err)
	}
	gv, found, err := st.GetVote(-1001, "t4")
	if err != nil || !found || gv.Approvals != 2 || gv.Voters[222] != "approve" {
		t.Fatalf("GetVote: %+v found=%v err=%v", gv, found, err)
	}
	if err := st.DeleteVote(-1001, "t4"); err != nil {
		t.Fatal(err)
	}
	if _, found, _ := st.GetVote(-1001, "t4"); found {
		t.Error("DeleteVote did not remove the round")
	}

	// Shares.
	share := model.ShareLink{Token: "tok123", Link: "https://t.me/parlaybot?start=share-tok123", Track: mustTrack("t5", "Shared"), CreatedBy: 111}
	if err := st.PutShare(share); err != nil {
		t.Fatal(err)
	}
	gs, found, err := st.GetShare("tok123")
	if err != nil || !found || gs.Link != share.Link {
		t.Fatalf("GetShare: %+v found=%v err=%v", gs, found, err)
	}

	// Spotify: oauth record (ciphertext only), watches, dislikes, skip-check.
	spot := model.SpotifyState{UserID: 111, TokenCiphertext: "aGVsbG8td29ybGQtdGVzdC1jaXBoZXJ0ZXh0", ExpiresAt: 9999999999}
	if err := st.PutSpotify(spot); err != nil {
		t.Fatal(err)
	}
	gsp, found, err := st.GetSpotify(111)
	if err != nil || !found || gsp.TokenCiphertext != spot.TokenCiphertext {
		t.Fatalf("GetSpotify: %+v found=%v err=%v", gsp, found, err)
	}
	if err := st.SetWatch(111, true); err != nil {
		t.Fatal(err)
	}
	if err := st.SetWatch(222, true); err != nil {
		t.Fatal(err)
	}
	watching, _ := st.IsWatching(111)
	if !watching {
		t.Error("IsWatching must be true after SetWatch(true)")
	}
	watches, err := st.ListWatches()
	if err != nil || len(watches) != 2 {
		t.Fatalf("ListWatches: %v err=%v", watches, err)
	}
	if err := st.SetDislike(111, "t6", true); err != nil {
		t.Fatal(err)
	}
	disliked, _ := st.IsDisliked(111, "t6")
	if !disliked {
		t.Error("IsDisliked must be true after SetDislike(true)")
	}
	if err := st.SetDislike(111, "t6", false); err != nil {
		t.Fatal(err)
	}
	disliked, _ = st.IsDisliked(111, "t6")
	if disliked {
		t.Error("IsDisliked must be false after SetDislike(false)")
	}
	reason := "profanity"
	sc := model.SkipCheckRecord{TrackID: "t6", Skip: true, Reason: &reason, ClassifierProvider: "wordlist", ClassifierVerdict: "profanity", ClassifierConfidence: 0.99}
	if err := st.PutSkipCheck(sc); err != nil {
		t.Fatal(err)
	}
	gsc, found, err := st.GetSkipCheck("t6")
	if err != nil || !found || !gsc.Skip || gsc.Reason == nil || *gsc.Reason != "profanity" {
		t.Fatalf("GetSkipCheck: %+v found=%v err=%v", gsc, found, err)
	}

	// Settings: unknown chat gets defaults; stored settings round-trip.
	def, found, err := st.GetSettings(-1001)
	if err != nil || found {
		t.Fatalf("GetSettings unknown chat: found=%v err=%v", found, err)
	}
	if def != model.DefaultChatSettings() {
		t.Errorf("unknown chat must get defaults: %+v", def)
	}
	custom := model.DefaultChatSettings()
	custom.PlayMode = "admin"
	custom.AutoDeleteMin = 7
	if err := st.PutSettings(-1001, custom); err != nil {
		t.Fatal(err)
	}
	back2, found, err := st.GetSettings(-1001)
	if err != nil || !found || back2.PlayMode != "admin" || back2.AutoDeleteMin != 7 {
		t.Fatalf("GetSettings stored: %+v found=%v err=%v", back2, found, err)
	}

	// Radio.
	reachable := true
	radio := model.RadioState{ChatID: -1001, Station: "stub-fm", StreamURL: "https://example.invalid/stub-fm", Reachable: &reachable}
	if err := st.PutRadio(radio); err != nil {
		t.Fatal(err)
	}
	gr, found, err := st.GetRadio(-1001)
	if err != nil || !found || gr.Station != "stub-fm" || gr.Reachable == nil || !*gr.Reachable {
		t.Fatalf("GetRadio: %+v found=%v err=%v", gr, found, err)
	}

	// Room list + delete (room delete also drops the queue).
	ids, err := st.ListRoomIDs()
	if err != nil || len(ids) != 1 || ids[0] != -1001 {
		t.Fatalf("ListRoomIDs: %v err=%v", ids, err)
	}
	if err := st.DeleteRoom(-1001); err != nil {
		t.Fatal(err)
	}
	if _, found, _ := st.GetRoom(-1001); found {
		t.Error("DeleteRoom did not remove the room")
	}
	if q, _ := st.GetQueue(-1001); len(q) != 0 {
		t.Errorf("DeleteRoom must drop the queue, got %d items", len(q))
	}
}

// TestStoreSurvivesKill9 proves the Phase 3 exit benchmark for the
// store: write state, kill -9 the process holding the database,
// reopen, state intact. The test re-execs itself as a helper child
// (env PARLAY_STORE_KILL9_HELPER=1): the child writes and then
// sleeps; the parent SIGKILLs it and reopens the same file.
func TestStoreSurvivesKill9(t *testing.T) {
	if os.Getenv("PARLAY_STORE_KILL9_HELPER") == "1" {
		dir := os.Getenv("PARLAY_STORE_KILL9_DIR")
		st, err := Open(dir)
		if err != nil {
			t.Fatal(err)
		}
		room := model.DefaultRoomState(-1001)
		room.Current = func() *model.Track { tr := mustTrack("kill9-track", "Kill Nine Song"); return &tr }()
		room.Loop = 2
		if err := st.PutRoom(room); err != nil {
			t.Fatal(err)
		}
		if err := st.PutQueue(-1001, []model.QueueItem{{RequestedBy: 111, Track: mustTrack("kill9-next", "Next Song")}}); err != nil {
			t.Fatal(err)
		}
		settings := model.DefaultChatSettings()
		settings.PlayMode = "admin"
		if err := st.PutSettings(-1001, settings); err != nil {
			t.Fatal(err)
		}
		// Signal the parent that every write is committed, then
		// sleep until the parent kills us. (Deliberately no
		// st.Close(): the kill must hit a live, un-closed handle.)
		if err := os.WriteFile(filepath.Join(dir, "ready"), []byte("ok"), 0o600); err != nil {
			t.Fatal(err)
		}
		time.Sleep(60 * time.Second)
		return
	}

	dir := t.TempDir()
	cmd := exec.Command(os.Args[0], "-test.run=TestStoreSurvivesKill9$", "-test.v")
	cmd.Env = append(os.Environ(),
		"PARLAY_STORE_KILL9_HELPER=1",
		"PARLAY_STORE_KILL9_DIR="+dir,
	)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	// Wait for the child to commit, then SIGKILL it.
	deadline := time.Now().Add(30 * time.Second)
	for {
		if _, err := os.Stat(filepath.Join(dir, "ready")); err == nil {
			break
		}
		if time.Now().After(deadline) {
			cmd.Process.Kill()
			t.Fatal("helper child never signalled ready")
		}
		time.Sleep(50 * time.Millisecond)
	}
	if err := cmd.Process.Kill(); err != nil {
		t.Fatalf("SIGKILL helper: %v", err)
	}
	if err := cmd.Wait(); err == nil || cmd.ProcessState.ExitCode() == 0 {
		t.Fatalf("helper must die from the signal, wait returned %v", err)
	}

	st, err := Open(dir)
	if err != nil {
		t.Fatalf("reopen after kill -9: %v", err)
	}
	defer st.Close()
	room, found, err := st.GetRoom(-1001)
	if err != nil || !found {
		t.Fatalf("room lost after kill -9: found=%v err=%v", found, err)
	}
	if room.Current == nil || room.Current.ID != "kill9-track" || room.Loop != 2 {
		t.Errorf("room state damaged after kill -9: %+v", room)
	}
	queue, err := st.GetQueue(-1001)
	if err != nil || len(queue) != 1 || queue[0].Track.ID != "kill9-next" {
		t.Errorf("queue lost after kill -9: %+v err=%v", queue, err)
	}
	settings, found, err := st.GetSettings(-1001)
	if err != nil || !found || settings.PlayMode != "admin" {
		t.Errorf("settings lost after kill -9: %+v found=%v err=%v", settings, found, err)
	}
}

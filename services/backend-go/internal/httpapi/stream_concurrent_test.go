package httpapi

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
	"time"

	"parlay/backend-go/internal/dsp"
	"parlay/backend-go/internal/dsp/fake"
	"parlay/backend-go/internal/voice"
)

// synthJoinFunc is the test-only stand-in for the core's TDLib
// joinVideoChat step (Phase 5's E2E probes use the same shape):
// it answers the Manager's Join with the remote transport JSON
// ntgcalls expects — ufrag/pwd, one host candidate, the local
// DTLS fingerprint — except the candidate points at
// 127.0.0.1:9, so no media ever leaves the box and no real peer
// is involved. ntg_connect accepts the params structurally and
// the call reaches StateConnected; the feed pump then pushes
// real DSP frames into the external source exactly as it does on
// a live call. This is a test double for the join handshake, not
// a production path: production joins come from the core over
// the UDS JoinRequest.
func synthJoinFunc(t *testing.T) voice.JoinFunc {
	t.Helper()
	return func(ctx context.Context, chatID int64, localSDP string) (string, int64, error) {
		var local struct {
			Fingerprints []struct {
				Hash        string `json:"hash"`
				Fingerprint string `json:"fingerprint"`
			} `json:"fingerprints"`
		}
		if err := json.Unmarshal([]byte(localSDP), &local); err != nil {
			return "", 0, fmt.Errorf("synth join: unmarshal local params: %w", err)
		}
		remote := map[string]any{
			"transport": map[string]any{
				"ufrag": "remoteUfrag",
				"pwd":   "remotePwd0123456789abcdef",
				"candidates": []map[string]any{{
					"ip": "127.0.0.1", "port": "9", "type": "host",
					"generation": "0", "protocol": "udp",
					"priority": "2122260223", "foundation": "1", "network": "0",
				}},
				"fingerprints": local.Fingerprints,
			},
			"audio": map[string]any{
				"payload-types": []map[string]any{{"id": 111, "name": "opus", "clockrate": 48000, "channels": 2}},
				"rtp-hdrexts":   []any{},
			},
			"video": map[string]any{
				"payload-types": []map[string]any{{"id": 96, "name": "VP8", "clockrate": 90000}},
				"rtp-hdrexts":   []any{},
			},
		}
		b, err := json.Marshal(remote)
		if err != nil {
			return "", 0, err
		}
		return string(b), 4242, nil
	}
}

// countingReader counts the bytes the DSP feed pulls from it —
// the witness that the voice path is actively decoding while the
// HTTP stream serves.
type countingReader struct {
	mu sync.Mutex
	r  io.Reader
	n  int64
}

func (c *countingReader) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.mu.Lock()
	c.n += int64(n)
	c.mu.Unlock()
	return n, err
}

func (c *countingReader) Bytes() int64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.n
}

// TestStreamWhilePlaying is the Phase 6 concurrency proof: a
// PlayStream is active through the DSP path (real ntgcalls
// client, fake DSP engine decoding a 30 s WAV tone, feed pump
// pushing 10 ms frames) while the HTTP server answers concurrent
// full and ranged FLAC stream requests for the same chat. Every
// response must be byte-identical to the cached FLAC, the feed
// must still be decoding when the requests finish, and a control
// action (pause) must work mid-stream.
func TestStreamWhilePlaying(t *testing.T) {
	e := newTestEnvWithJoin(t, "", synthJoinFunc(t))
	chatID := int64(-1001)

	flacPath, flacData := renderAudio(t, "flac", 5)
	playTrack(t, e, chatID, flacTrack)
	e.index.Record(flacTrack, flacPath, int64(len(flacData)))

	// A 30 s tone: the feed is still decoding long after the
	// HTTP window closes, so "still playing" is unambiguous.
	_, toneData := renderAudio(t, "wav", 30)

	e.voiceM.SetDSP(fake.Engine{})
	if _, err := e.voiceM.Join(context.Background(), chatID); err != nil {
		t.Fatalf("synth Join: %v", err)
	}
	call, ok := e.voiceM.Get(chatID)
	if !ok || call.State != voice.StateConnected {
		t.Fatalf("call after synth Join: %+v ok=%v, want connected", call, ok)
	}

	feedEnded := make(chan error, 1)
	e.voiceM.OnFeedEnd(func(id int64, err error) {
		if id == chatID {
			feedEnded <- err
		}
	})

	counter := &countingReader{r: bytes.NewReader(toneData)}
	if err := e.voiceM.PlayStream(context.Background(), chatID, io.NopCloser(counter), dsp.DefaultOptions()); err != nil {
		t.Fatalf("PlayStream: %v", err)
	}
	t.Cleanup(func() { _ = e.voiceM.Stop(chatID) })

	// Wait for the feed pump to prove it is decoding before the
	// HTTP window opens: bytes must be leaving the reader.
	deadline := time.Now().Add(10 * time.Second)
	for counter.Bytes() == 0 {
		if time.Now().After(deadline) {
			t.Fatal("feed pump consumed 0 bytes in 10s; PlayStream is not decoding")
		}
		time.Sleep(50 * time.Millisecond)
	}
	startBytes := counter.Bytes()
	t.Logf("feed decoding: %d bytes consumed before the HTTP window", startBytes)

	srv := httptest.NewServer(e.srv.Handler())
	t.Cleanup(srv.Close)

	// Concurrent window: 6 clients, 10 requests each, mixing
	// full-body and ranged GETs. Every response must be
	// byte-identical to the cached FLAC file.
	var wg sync.WaitGroup
	errs := make(chan string, 100)
	ranges := []string{"", "bytes=0-99", "bytes=1000-1999", fmt.Sprintf("bytes=%d-", len(flacData)-1000)}
	for w := 0; w < 6; w++ {
		wg.Add(1)
		go func(worker int) {
			defer wg.Done()
			for i := 0; i < 10; i++ {
				rng := ranges[(worker+i)%len(ranges)]
				req, err := http.NewRequest(http.MethodGet, fmt.Sprintf("%s/api/v1/stream/%d/flac", srv.URL, chatID), nil)
				if err != nil {
					errs <- fmt.Sprintf("worker %d: new request: %v", worker, err)
					return
				}
				if rng != "" {
					req.Header.Set("Range", rng)
				}
				resp, err := http.DefaultClient.Do(req)
				if err != nil {
					errs <- fmt.Sprintf("worker %d: do: %v", worker, err)
					return
				}
				body, err := io.ReadAll(resp.Body)
				_ = resp.Body.Close()
				if err != nil {
					errs <- fmt.Sprintf("worker %d: read: %v", worker, err)
					return
				}
				wantStatus := http.StatusOK
				if rng != "" {
					wantStatus = http.StatusPartialContent
				}
				if resp.StatusCode != wantStatus {
					errs <- fmt.Sprintf("worker %d range %q: status=%d, want %d", worker, rng, resp.StatusCode, wantStatus)
					return
				}
				var want []byte
				switch rng {
				case "":
					want = flacData
				case "bytes=0-99":
					want = flacData[0:100]
				case "bytes=1000-1999":
					want = flacData[1000:2000]
				default:
					want = flacData[len(flacData)-1000:]
				}
				if !bytes.Equal(body, want) {
					errs <- fmt.Sprintf("worker %d range %q: body differs from cached FLAC (%d bytes, want %d)", worker, rng, len(body), len(want))
					return
				}
			}
		}(w)
	}
	wg.Wait()
	close(errs)
	for e := range errs {
		t.Error(e)
	}

	// The feed must still be decoding after the HTTP window:
	// more bytes consumed, no feed-end event, call record intact.
	endBytes := counter.Bytes()
	t.Logf("feed decoding: %d bytes consumed after the HTTP window (started at %d)", endBytes, startBytes)
	if endBytes <= startBytes {
		t.Errorf("feed stopped decoding during the HTTP window: %d bytes before, %d after", startBytes, endBytes)
	}
	select {
	case err := <-feedEnded:
		t.Errorf("feed ended during the HTTP window: err=%v", err)
	default:
	}
	call, ok = e.voiceM.Get(chatID)
	if !ok || call.FeedEnded {
		t.Errorf("call after HTTP window: %+v ok=%v, want live feed", call, ok)
	}

	// A control action works mid-stream: pause answers 200 and
	// the room is paused.
	rec := e.post(t, "/api/v1/pause", `{"chat_id":-1001}`)
	if rec.Code != http.StatusOK {
		t.Fatalf("pause mid-stream status=%d, want 200: %s", rec.Code, rec.Body.String())
	}
	body := decodeBody(t, rec)
	np, _ := body["now_playing"].(map[string]any)
	if np["is_paused"] != true {
		t.Fatalf("pause mid-stream did not pause: %v", np)
	}
	t.Logf("stream-while-playing: 60 concurrent requests byte-identical, feed consumed %d bytes across the window", endBytes-startBytes)
}

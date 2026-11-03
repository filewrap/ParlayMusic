package search

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/uds"
)

func errCode(t *testing.T, err error) uds.ErrorCode {
	t.Helper()
	var herr *uds.HandlerError
	if !errors.As(err, &herr) {
		t.Fatalf("error is not a HandlerError: %v", err)
	}
	return herr.Code
}

// queryService builds a Service with a real registry (no network is
// touched by these tests: the Spotify path fails on the credential
// check before any HTTP, and the Lemonic path uses the httptest
// server).
func queryService(t *testing.T, lemonic *LemonicClient) *Service {
	t.Helper()
	t.Setenv("SPOTIFY_CLIENT_ID", "")
	t.Setenv("SPOTIFY_CLIENT_SECRET", "")
	deps := &app.Deps{Registry: resolver.NewRegistry()}
	return NewServiceWithLemonic(deps, lemonic, nil)
}

func TestHandleQueryValidation(t *testing.T) {
	svc := queryService(t, nil)
	ctx := context.Background()
	_, err := svc.handleQuery(ctx, uds.Request{Payload: mustJSON(t, map[string]any{})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("empty query: code = %v, want bad_request", err)
	}
	_, err = svc.handleQuery(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"query": "x", "source": "soulseek"})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("unknown source: code = %v, want bad_request", err)
	}
	_, err = svc.handleQuery(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"query": "x", "source": "navidrome"})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("navidrome is not a search.query source: code = %v", err)
	}
}

func TestHandleQuerySpotifyAllFail(t *testing.T) {
	// Spotify without credentials fails on the credential check
	// (before any HTTP): a single-source query where every source
	// fails answers 502 naming the source.
	svc := queryService(t, nil)
	_, err := svc.handleQuery(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"query": "lag ja gale", "source": "spotify"})})
	if errCode(t, err) != uds.ErrEngineUnavailable {
		t.Fatalf("spotify without creds: code = %v, want engine_unavailable", err)
	}
	if !strings.Contains(err.Error(), "spotify") {
		t.Fatalf("all-fail error should name the source: %v", err)
	}
}

func TestHandleQueryLemonicOnly(t *testing.T) {
	// A fake Lemonic API answering its own search endpoint.
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/search" {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		fmt.Fprint(w, `{"results":[
			{"id":"lem1","title":"Lag Ja Gale","artist":"Lata Mangeshkar","duration":200},
			{"id":"lem2","title":"Lag Ja Gale 2","artist":"Lata","duration_ms":180000}
		]}`)
	}))
	defer srv.Close()
	svc := queryService(t, NewLemonicClientWithBase(srv.URL, ""))
	got, err := svc.handleQuery(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"query": "lag ja gale", "source": "lemonic", "limit": 5})})
	if err != nil {
		t.Fatalf("lemonic query: %v", err)
	}
	m, ok := got.(map[string]any)
	if !ok {
		t.Fatalf("response = %#v", got)
	}
	status, ok := m["source_status"].(map[string]string)
	if !ok || status["lemonic"] != "ok" {
		t.Fatalf("source_status = %#v", m["source_status"])
	}
	results, ok := m["results"].([]model.Track)
	if !ok || len(results) != 2 {
		t.Fatalf("results = %#v", m["results"])
	}
	if results[0].ID != "lemonic:lem1" || results[0].Title != "Lag Ja Gale" || results[0].DurationSec != 200 {
		t.Fatalf("result 0 = %+v", results[0])
	}
	// duration_ms is honoured (180000ms -> 180s).
	if results[1].DurationSec != 180 {
		t.Fatalf("result 1 duration = %d, want 180", results[1].DurationSec)
	}
}

func TestHandleQueryLemonicEmpty(t *testing.T) {
	// Lemonic answers with zero results: the source is "empty",
	// not "failed" — and no Spotify fallback is attempted.
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"results":[]}`)
	}))
	defer srv.Close()
	svc := queryService(t, NewLemonicClientWithBase(srv.URL, ""))
	got, err := svc.handleQuery(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"query": "zzz no match", "source": "lemonic"})})
	if err != nil {
		t.Fatalf("lemonic empty: %v", err)
	}
	m := got.(map[string]any)
	status := m["source_status"].(map[string]string)
	if status["lemonic"] != "empty" {
		t.Fatalf("source_status = %#v, want lemonic=empty", m["source_status"])
	}
	if results := m["results"].([]model.Track); len(results) != 0 {
		t.Fatalf("results = %#v, want empty", m["results"])
	}
}

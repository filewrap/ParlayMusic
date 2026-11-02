package resolver

import (
	"context"
	"testing"

	"parlay/vc-go/internal/types"
)

func TestRelatedNoYtDlpReturnsNil(t *testing.T) {
	r := &Resolver{YtDlp: ""} // degraded: no binary
	got, err := r.Related(context.Background(),
		types.Track{ID: "yt:abc", Title: "A", Source: "youtube"}, map[string]bool{})
	if err != nil || got != nil {
		t.Fatalf("no yt-dlp should yield (nil, nil), got %+v err=%v", got, err)
	}
}

func TestRelatedNonYouTubeReturnsNil(t *testing.T) {
	r := &Resolver{YtDlp: "yt-dlp"}
	got, err := r.Related(context.Background(),
		types.Track{ID: "sp:abc", Title: "A", Source: "spotify"}, map[string]bool{})
	if err != nil || got != nil {
		t.Fatalf("non-YouTube should yield (nil, nil), got %+v err=%v", got, err)
	}
}

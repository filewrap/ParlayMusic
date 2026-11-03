// Radio resolver — native Go HTTP/ICY stream probe replacing the
// 10-second ffprobe subprocess in services/radio-js
// (BENCHMARKS.md §2, locked).
//
// No resolution for radio URLs: the URL IS the stream (108 µs median
// measured, Phase 1). The probe is a separate concern — a short HTTP
// GET that reads the response headers (ICY metadata: icy-name,
// icy-genre, icy-br, content-type) plus the first bytes, and reports
// reachable/format without ever spawning ffprobe.
//
// The curated station list is ported verbatim from
// services/radio-js/src/stations.js (original to Parlay —
// openradiobot has no curated list). Each station also carries its
// small fallback playlist (labelled as such in radio-js): if a live
// stream cannot be probed/reached, playback keeps rotating these
// playlist entries instead of going silent.
package resolver

import (
	"context"
	"crypto/sha1"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// Station is one curated radio station (radio-js stations.js).
type Station struct {
	ID        string   `json:"id"`
	Name      string   `json:"name"`
	Genre     string   `json:"genre"`
	StreamURL string   `json:"stream_url"`
	Playlist  []string `json:"playlist,omitempty"`
}

// Stations is the curated station list (radio-js stations.js).
var Stations = []Station{
	{ID: "groovesalad", Name: "SomaFM Groove Salad", Genre: "Ambient / downtempo beats",
		StreamURL: "https://ice1.somafm.com/groovesalad-128-mp3",
		Playlist:  []string{"Groove Salad — ambient beats mix", "Groove Salad — late-night downtempo", "Groove Salad — chillout session"}},
	{ID: "lush", Name: "SomaFM Lush", Genre: "Mellow vocals / chill",
		StreamURL: "https://ice1.somafm.com/lush-128-mp3",
		Playlist:  []string{"Lush — mellow vocal chill", "Lush — soft electronic"}},
	{ID: "radioparadise", Name: "Radio Paradise (Main Mix)", Genre: "Eclectic rock / world",
		StreamURL: "https://stream.radioparadise.com/aac-320",
		Playlist:  []string{"Radio Paradise — main mix", "Radio Paradise — eclectic hour"}},
	{ID: "fip", Name: "FIP Paris", Genre: "Eclectic / jazz / world",
		StreamURL: "https://icecast.radiofrance.fr/fip-midfi.mp3",
		Playlist:  []string{"FIP — Paris eclectic", "FIP — jazz & soul"}},
	{ID: "bbcws", Name: "BBC World Service", Genre: "News / talk",
		StreamURL: "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service",
		Playlist:  []string{"BBC World Service — live news", "BBC World Service — programmes"}},
	{ID: "kexp", Name: "KEXP 90.3 FM Seattle", Genre: "Indie / alternative",
		StreamURL: "https://streams.kexp.org/kexp.mp3",
		Playlist:  []string{"KEXP — indie & alternative", "KEXP — live in-studio"}},
}

// FindStation returns a station by id, exact name, or name
// substring (radio-js findStation), or nil.
func FindStation(ref string) *Station {
	if ref == "" {
		return nil
	}
	q := strings.ToLower(strings.TrimSpace(ref))
	for i := range Stations {
		if Stations[i].ID == q {
			return &Stations[i]
		}
	}
	for i := range Stations {
		if strings.ToLower(Stations[i].Name) == q {
			return &Stations[i]
		}
	}
	for i := range Stations {
		if strings.Contains(strings.ToLower(Stations[i].Name), q) {
			return &Stations[i]
		}
	}
	return nil
}

// RadioResolver resolves radio station refs and raw stream URLs.
type RadioResolver struct{}

// NewRadioResolver builds a radio resolver.
func NewRadioResolver() *RadioResolver { return &RadioResolver{} }

// Source implements Resolver.
func (r *RadioResolver) Source() Source { return SourceRadio }

// RadioTrackFromURL builds a live track from a raw stream URL
// (vc-ts radioTrackFromUrl): SHA-1 id, duration 0, is_live true.
// No network I/O at all — the URL is the stream.
func RadioTrackFromURL(streamURL string) Track {
	sum := sha1.Sum([]byte(streamURL))
	id := "radio:" + hex.EncodeToString(sum[:])[:12]
	return Track{
		ID: id, Title: streamURL, Source: string(SourceRadio),
		URL: streamURL, StreamURL: streamURL,
		DurationSec: 0, DurationStr: "∞", IsLive: true,
	}
}

// Resolve handles radio:<station id|name|stream URL> or a bare
// stream URL. The returned track's StreamURL is the stream itself.
func (r *RadioResolver) Resolve(ctx context.Context, query string) (Track, error) {
	q := strings.TrimSpace(query)
	q = strings.TrimPrefix(strings.ToLower(q), "radio:")
	if station := FindStation(q); station != nil {
		t := RadioTrackFromURL(station.StreamURL)
		t.ID = "radio:" + station.ID
		t.Title = station.Name
		t.Artist = station.Name
		return t, nil
	}
	if strings.HasPrefix(q, "http://") || strings.HasPrefix(q, "https://") {
		return RadioTrackFromURL(q), nil
	}
	return Track{}, engineUnavailable(SourceRadio, "unknown radio station %q (known: %s)", query, StationIDs())
}

// Search lists stations whose name or genre matches the query.
func (r *RadioResolver) Search(ctx context.Context, query string, limit int) ([]Track, error) {
	q := strings.ToLower(strings.TrimSpace(query))
	var tracks []Track
	for _, st := range Stations {
		if q == "" || strings.Contains(strings.ToLower(st.Name), q) || strings.Contains(strings.ToLower(st.Genre), q) {
			t := RadioTrackFromURL(st.StreamURL)
			t.ID = "radio:" + st.ID
			t.Title = st.Name
			t.Artist = st.Genre
			tracks = append(tracks, t)
			if limit > 0 && len(tracks) >= limit {
				break
			}
		}
	}
	return tracks, nil
}

// StationIDs returns the comma-joined station ids (for errors).
func StationIDs() string {
	ids := make([]string, len(Stations))
	for i, st := range Stations {
		ids[i] = st.ID
	}
	return strings.Join(ids, ", ")
}

// ProbeResult is the native stream probe outcome (radio-js probeStream
// shape, minus the ffprobe subprocess).
type ProbeResult struct {
	Reachable   bool   `json:"reachable"`
	Format      string `json:"format,omitempty"`
	Probe       string `json:"probe"`
	ContentType string `json:"content_type,omitempty"`
	ICYName     string `json:"icy_name,omitempty"`
	ICYGenre    string `json:"icy_genre,omitempty"`
	ICYBitrate  string `json:"icy_br,omitempty"`
}

// ProbeStream verifies a stream URL with a native HTTP GET: it asks
// for ICY metadata (Icy-MetaData: 1), reads the headers and the
// first 4 KB, then closes the body. No ffprobe, no subprocess, and
// the probe never downloads the whole (infinite) stream.
// Timeout is 10 s, matching the old ffprobe probe budget.
func ProbeStream(ctx context.Context, streamURL string) ProbeResult {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, streamURL, nil)
	if err != nil {
		return ProbeResult{Reachable: false, Probe: fmt.Sprintf("probe request failed: %v", err)}
	}
	req.Header.Set("User-Agent", "Parlay/1.0")
	req.Header.Set("Icy-MetaData", "1")
	resp, err := httpClient(10 * time.Second).Do(req)
	if err != nil {
		return ProbeResult{Reachable: false, Probe: fmt.Sprintf("probe failed: %v", err)}
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return ProbeResult{Reachable: false, Probe: fmt.Sprintf("probe: HTTP %d", resp.StatusCode)}
	}
	// Read the first bytes to prove the stream actually flows.
	if _, err := io.ReadFull(resp.Body, make([]byte, 1)); err != nil && err != io.ErrUnexpectedEOF {
		return ProbeResult{Reachable: false, Probe: fmt.Sprintf("probe: no stream bytes: %v", err)}
	}
	contentType := resp.Header.Get("Content-Type")
	format := ""
	switch {
	case strings.Contains(contentType, "mpeg"):
		format = "mp3"
	case strings.Contains(contentType, "aac"):
		format = "aac"
	case strings.Contains(contentType, "ogg"):
		format = "ogg"
	case strings.Contains(contentType, "flac"):
		format = "flac"
	default:
		format = contentType
	}
	return ProbeResult{
		Reachable: true, Format: format, Probe: "ok",
		ContentType: contentType,
		ICYName:     resp.Header.Get("icy-name"),
		ICYGenre:    resp.Header.Get("icy-genre"),
		ICYBitrate:  resp.Header.Get("icy-br"),
	}
}

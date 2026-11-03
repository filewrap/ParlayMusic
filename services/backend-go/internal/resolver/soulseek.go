// Soulseek resolver — native Go client to slskd (:5030) HTTP API v0
// (BENCHMARKS.md §2, locked). Ported from
// services/downloaders/src/soulseek/ (client.py, scorer.py,
// selection.py) and refs/music-bot-navidrome/src/soulseek/.
//
// Flow: POST /api/v0/searches, poll GET /searches/{id} until the
// file count plateaus (6 s) or the search completes, PUT stop to
// force the response flush (the only path that populates /responses),
// GET /searches/{id}/responses, DELETE the search. Peers are ranked
// by free upload slots, speed, and .flac first (scorer.py: match
// axis 0..55 — duration 40 + name relevance 15, × version penalty;
// fetch axis −6..35 — reliability 25 + quality 10). Download:
// POST /api/v0/transfers/downloads/{user}, poll until
// "Completed, Succeeded".
//
// slskd is not running in the sandbox — every path fails honestly
// naming SLSKD_URL when the daemon is unreachable.
package resolver

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Soulseek search pacing / backoff (client.py).
const (
	slskdSearchMinIntervalSecs = 10.0
	slskdZeroBurstThreshold    = 3
	slskdZeroCooldownSecs      = 90.0
)

var slskd429Backoffs = []float64{30, 60, 90}

// Selection thresholds (selection.py).
const (
	soulseekMatchFloor           = 20.0
	soulseekSearchSatisfiedMatch = 45.0
	soulseekSearchSatisfiedBlend = 70.0
)

// Quality cap (scorer.py: 24-bit / 96000 Hz; 0 disables that axis).
var (
	soulseekMaxBitDepth     = envInt("MAX_BIT_DEPTH", 24)
	soulseekMaxSampleRateHz = envInt("MAX_SAMPLE_RATE_HZ", 96000)
)

var soulseekDifferentVersionKeywords = []string{
	"remix", "rmx", "live", "live at", "live from", "live in", "live on",
	"acoustic", "unplugged", "slowed", "sped up", "speed up", "reverb",
	"radio edit", "radio version", "single edit", "album edit",
	"instrumental", "karaoke", "extended", "extended version",
	"extended mix", "demo", "rough cut", "rough mix", "cover", "tribute",
}
var soulseekRemasterKeywords = []string{"remaster", "remastered"}

func envInt(name string, def int) int {
	if v := strings.TrimSpace(os.Getenv(name)); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

// SoulseekResolver resolves/downloads via slskd.
type SoulseekResolver struct {
	mu sync.Mutex
	// baseURL is the slskd base URL wired from [backend] slskd_url
	// by Registry.Configure (Q10); empty means "read $SLSKD_URL /
	// $SLSKD_HOST directly", the Phase 3 behaviour.
	baseURL           string
	nextSearchAt      time.Time
	zeroStreak        int
	zeroVerifiedUntil time.Time
}

// NewSoulseekResolver builds a Soulseek resolver.
func NewSoulseekResolver() *SoulseekResolver { return &SoulseekResolver{} }

// Source implements Resolver.
func (s *SoulseekResolver) Source() Source { return SourceSoulseek }

// slskdBaseURL returns the slskd base URL: the [backend]
// slskd_url value when Registry.Configure set one, else $SLSKD_URL
// / $SLSKD_HOST, else the local default (Q10 — the file key now
// does what the env var did).
func (s *SoulseekResolver) slskdBaseURL() string {
	if s.baseURL != "" {
		return s.baseURL
	}
	if v := strings.TrimSpace(os.Getenv("SLSKD_URL")); v != "" {
		return strings.TrimRight(v, "/")
	}
	if v := strings.TrimSpace(os.Getenv("SLSKD_HOST")); v != "" {
		return strings.TrimRight(v, "/")
	}
	return "http://127.0.0.1:5030"
}

func slskdTimeout() time.Duration {
	if v := strings.TrimSpace(os.Getenv("SLSKD_API_TIMEOUT_SECS")); v != "" {
		if n, err := strconv.ParseFloat(v, 64); err == nil && n > 0 {
			return time.Duration(n * float64(time.Second))
		}
	}
	return 30 * time.Second
}

func slskdDownloadTimeout() time.Duration {
	if v := strings.TrimSpace(os.Getenv("SLSKD_DOWNLOAD_TIMEOUT")); v != "" {
		if n, err := strconv.ParseFloat(v, 64); err == nil && n > 0 {
			return time.Duration(n * float64(time.Second))
		}
	}
	return 900 * time.Second
}

// slskdRequest performs one slskd API call. A transport failure
// maps to the honest "slskd unreachable" error.
func (s *SoulseekResolver) slskdRequest(ctx context.Context, method, path string, payload any) (json.RawMessage, int, error) {
	var body []byte
	if payload != nil {
		b, err := json.Marshal(payload)
		if err != nil {
			return nil, 0, err
		}
		body = b
	}
	req, err := http.NewRequestWithContext(ctx, method, s.slskdBaseURL()+"/api/v0"+path, strings.NewReader(string(body)))
	if err != nil {
		return nil, 0, err
	}
	req.Header.Set("Accept", "application/json")
	if key := strings.TrimSpace(os.Getenv("SLSKD_API_KEY")); key != "" {
		req.Header.Set("X-API-Key", key)
	}
	if payload != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := httpClient(slskdTimeout()).Do(req)
	if err != nil {
		return nil, 0, &EngineUnavailable{Source: SourceSoulseek,
			Why: fmt.Sprintf("slskd unreachable at %s (%v)", s.slskdBaseURL(), err)}
	}
	defer resp.Body.Close()
	raw := readAllLimit(resp.Body, 32<<20)
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return raw, resp.StatusCode, &slskdHTTPError{Status: resp.StatusCode, Body: strings.TrimSpace(string(raw))}
	}
	return raw, resp.StatusCode, nil
}

type slskdHTTPError struct {
	Status int
	Body   string
}

func (e *slskdHTTPError) Error() string {
	return fmt.Sprintf("slskd HTTP %d: %s", e.Status, e.Body)
}

// slskdAvailable reports whether slskd answers at all (any HTTP
// response — even an auth error — counts as "the daemon is there").
func (s *SoulseekResolver) slskdAvailable(ctx context.Context) bool {
	_, _, err := s.slskdRequest(ctx, http.MethodGet, "/application", nil)
	if err == nil {
		return true
	}
	if _, ok := err.(*slskdHTTPError); ok {
		return true
	}
	return false
}

// Resolve handles soulseek:<artist - title> (or a free-text query):
// search slskd, score, return the best FLAC match as a track. The
// track carries no StreamURL — Soulseek audio only exists after a
// download (see Download).
func (s *SoulseekResolver) Resolve(ctx context.Context, query string) (Track, error) {
	q := strings.TrimSpace(query)
	q = strings.TrimPrefix(strings.ToLower(q), "soulseek:")
	artist, title := splitArtistTitle(q)
	track := Track{Artist: artist, Title: title, Source: string(SourceSoulseek)}
	candidates, err := s.searchAndScore(ctx, q, artist, title, 0, false)
	if err != nil {
		return Track{}, err
	}
	if len(candidates) == 0 {
		return Track{}, engineUnavailable(SourceSoulseek, "Soulseek search for %q found nothing usable", query)
	}
	best := candidates[0]
	track.ID = "slsk:" + best.Result.Username + ":" + best.Result.Basename()
	track.Title = title
	if track.Title == "" {
		track.Title = best.Result.Basename()
	}
	track.DurationSec = int64(best.Result.LengthSeconds)
	track.DurationStr = FormatDurationStr(int(best.Result.LengthSeconds), false)
	track.URL = ""
	return track, nil
}

// Search returns scored Soulseek matches as tracks (best first).
func (s *SoulseekResolver) Search(ctx context.Context, query string, limit int) ([]Track, error) {
	if limit <= 0 {
		limit = 5
	}
	artist, title := splitArtistTitle(strings.TrimSpace(query))
	candidates, err := s.searchAndScore(ctx, strings.TrimSpace(query), artist, title, 0, false)
	if err != nil {
		return nil, err
	}
	if len(candidates) > limit {
		candidates = candidates[:limit]
	}
	tracks := make([]Track, 0, len(candidates))
	for _, c := range candidates {
		t := Track{
			ID:    "slsk:" + c.Result.Username + ":" + c.Result.Basename(),
			Title: title, Artist: artist, Source: string(SourceSoulseek),
			DurationSec: int64(c.Result.LengthSeconds),
			DurationStr: FormatDurationStr(int(c.Result.LengthSeconds), false),
		}
		if t.Title == "" {
			t.Title = c.Result.Basename()
		}
		tracks = append(tracks, t)
	}
	return tracks, nil
}

// Download searches, scores, enqueues the best copy and waits for
// slskd to finish it, then returns the file under the slskd download
// dir (copied into destDir when it differs).
func (s *SoulseekResolver) Download(ctx context.Context, track Track, destDir string, onProgress ProgressFunc) (DownloadResult, error) {
	runID := NewRunID()
	if !s.slskdAvailable(ctx) {
		return DownloadResult{}, &EngineUnavailable{Source: SourceSoulseek,
			Why: fmt.Sprintf("slskd unreachable at %s", s.slskdBaseURL())}
	}
	acceptLossy := false
	candidates, err := s.searchAndScore(ctx, track.Artist+" "+track.Title, track.Artist, track.Title, int(track.DurationSec), acceptLossy)
	if err != nil {
		return DownloadResult{}, err
	}
	if len(candidates) == 0 && !acceptLossy {
		return DownloadResult{}, engineUnavailable(SourceSoulseek, "Soulseek search for %q found nothing usable", track.Artist+" "+track.Title)
	}
	if len(candidates) == 0 {
		return DownloadResult{}, engineUnavailable(SourceSoulseek, "Soulseek search for %q found nothing usable", track.Artist+" "+track.Title)
	}
	var lastErr error
	for _, cand := range candidates {
		for _, src := range cand.Sources {
			if err := s.enqueue(ctx, src.Username, []slskdFile{{Filename: src.Filename, Size: src.Size}}); err != nil {
				lastErr = err
				continue
			}
			state, err := s.waitForFile(ctx, src.Username, src.Filename, runID, onProgress)
			if err != nil {
				lastErr = err
				continue
			}
			if !strings.Contains(strings.ToLower(state), "succeeded") {
				lastErr = fmt.Errorf("transfer ended in state %q", state)
				continue
			}
			path, err := s.locateDownloaded(ctx, src.Username, src.Filename, destDir)
			if err != nil {
				lastErr = err
				continue
			}
			if onProgress != nil {
				onProgress(Progress{RunID: runID, Percentage: 100, Phase: "done"})
			}
			return DownloadResult{FilePath: path, Track: track, RunID: runID, Peer: src.Username, Format: "FLAC"}, nil
		}
	}
	if lastErr != nil {
		return DownloadResult{}, lastErr
	}
	return DownloadResult{}, engineUnavailable(SourceSoulseek, "Soulseek download failed: no candidate completed")
}

// --- slskd search ----------------------------------------------------

// slskdSearchResponse is one peer response from /searches/{id}/responses.
type slskdSearchResponse struct {
	Username          string `json:"username"`
	HasFreeUploadSlot bool   `json:"hasFreeUploadSlot"`
	UploadSpeed       int64  `json:"uploadSpeed"`
	QueueLength       int    `json:"queueLength"`
	Files             []struct {
		Filename   string `json:"filename"`
		Size       int64  `json:"size"`
		BitRate    *int   `json:"bitRate"`
		BitDepth   *int   `json:"bitDepth"`
		SampleRate *int   `json:"sampleRate"`
		Length     *int   `json:"length"`
	} `json:"files"`
}

// slskdFile is one flattened peer file (client.py SearchResult).
type slskdFile struct {
	Username      string
	Filename      string
	Size          int64
	BitRate       int
	BitDepth      int
	SampleRate    int
	LengthSeconds int
	HasFreeSlot   bool
	UploadSpeed   int64
	QueueLength   int
}

func (f slskdFile) Basename() string {
	parts := strings.Split(strings.ReplaceAll(f.Filename, "\\", "/"), "/")
	return parts[len(parts)-1]
}

func (f slskdFile) Directory() string {
	name := strings.ReplaceAll(f.Filename, "\\", "/")
	if i := strings.LastIndex(name, "/"); i >= 0 {
		return name[:i]
	}
	return ""
}

func (f slskdFile) Extension() string {
	base := f.Basename()
	if i := strings.LastIndex(base, "."); i >= 0 {
		return strings.ToLower(base[i+1:])
	}
	return ""
}

func (f slskdFile) IsLossless() bool { return f.Extension() == "flac" }

// searchAndScore runs one slskd search and returns scored,
// match-floor-filtered candidates (best first, copies grouped).
func (s *SoulseekResolver) searchAndScore(ctx context.Context, query, artist, title string, durationSec int, acceptLossy bool) ([]scoredTrack, error) {
	if !s.slskdAvailable(ctx) {
		return nil, &EngineUnavailable{Source: SourceSoulseek,
			Why: fmt.Sprintf("slskd unreachable at %s", s.slskdBaseURL())}
	}
	responses, err := s.search(ctx, query, 20, 200)
	if err != nil {
		return nil, err
	}
	results := flattenResponses(responses, !acceptLossy)
	return scoreTrackResults(results, artist, title, durationSec), nil
}

// search runs one serialized, paced slskd search (client.py search /
// _search_once): POST, poll until the file count plateaus 6 s or the
// search completes, PUT stop to force the flush, GET responses,
// DELETE the search.
func (s *SoulseekResolver) search(ctx context.Context, query string, timeoutSecs, responseLimit int) ([]slskdSearchResponse, error) {
	s.mu.Lock()
	if wait := time.Until(s.nextSearchAt); wait > 0 {
		s.mu.Unlock()
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(wait):
		}
		s.mu.Lock()
	}
	s.mu.Unlock()

	searchID := newUUID()
	var state map[string]any
	for _, backoff := range append(append([]float64{}, slskd429Backoffs...), -1) {
		raw, _, rerr := s.slskdRequest(ctx, http.MethodPost, "/searches", map[string]any{
			"id": searchID, "searchText": query,
			"searchTimeout": timeoutSecs * 1000, "responseLimit": responseLimit,
			"fileLimit": 10000, "filterResponses": true,
			"maximumPeerQueueLength": 1000000, "minimumPeerUploadSpeed": 0,
			"minimumResponseFileCount": 1,
		})
		if rerr == nil {
			_ = json.Unmarshal(raw, &state)
			break
		}
		if herr, ok := rerr.(*slskdHTTPError); ok && herr.Status == 429 && backoff >= 0 {
			select {
			case <-ctx.Done():
				return nil, ctx.Err()
			case <-time.After(time.Duration(backoff * float64(time.Second))):
			}
			continue
		}
		return nil, fmt.Errorf("slskd search API error: %w", rerr)
	}
	if state == nil {
		return nil, fmt.Errorf("slskd search API error: no search state returned")
	}
	if id, ok := state["id"].(string); ok && id != "" {
		searchID = id
	}
	s.mu.Lock()
	s.nextSearchAt = time.Now().Add(time.Duration(slskdSearchMinIntervalSecs * float64(time.Second)))
	s.mu.Unlock()

	// Poll for stability or natural completion.
	start := time.Now()
	lastCount := -1
	var stableSince time.Time
	fileCount := 0
	for time.Since(start) < time.Duration(timeoutSecs)*time.Second {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(500 * time.Millisecond):
		}
		raw, _, rerr := s.slskdRequest(ctx, http.MethodGet, "/searches/"+url.PathEscape(searchID), nil)
		if rerr != nil {
			continue
		}
		var st struct {
			FileCount  int  `json:"fileCount"`
			IsComplete bool `json:"isComplete"`
		}
		_ = json.Unmarshal(raw, &st)
		fileCount = st.FileCount
		if st.IsComplete {
			break
		}
		if fileCount != lastCount {
			lastCount = fileCount
			stableSince = time.Now()
		} else if !stableSince.IsZero() && time.Since(stableSince) > 6*time.Second && fileCount > 0 {
			break
		}
	}
	// Force a flush so /responses populates.
	_, _, _ = s.slskdRequest(ctx, http.MethodPut, "/searches/"+url.PathEscape(searchID), nil)
	for i := 0; i < 8; i++ {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(250 * time.Millisecond):
		}
		raw, _, rerr := s.slskdRequest(ctx, http.MethodGet, "/searches/"+url.PathEscape(searchID), nil)
		if rerr != nil {
			break
		}
		var st struct {
			IsComplete bool `json:"isComplete"`
		}
		if json.Unmarshal(raw, &st) == nil && st.IsComplete {
			break
		}
	}
	var responses []slskdSearchResponse
	raw, _, rerr := s.slskdRequest(ctx, http.MethodGet, "/searches/"+url.PathEscape(searchID)+"/responses", nil)
	if rerr == nil {
		_ = json.Unmarshal(raw, &responses)
	}
	_, _, _ = s.slskdRequest(ctx, http.MethodDelete, "/searches/"+url.PathEscape(searchID), nil)
	return responses, nil
}

// flattenResponses converts peer responses to flat file results,
// audio extensions only (FLAC-only when losslessOnly).
func flattenResponses(responses []slskdSearchResponse, losslessOnly bool) []slskdFile {
	var out []slskdFile
	for _, resp := range responses {
		for _, f := range resp.Files {
			if f.Filename == "" {
				continue
			}
			rec := slskdFile{
				Username: resp.Username, Filename: f.Filename, Size: f.Size,
				HasFreeSlot: resp.HasFreeUploadSlot,
				UploadSpeed: resp.UploadSpeed, QueueLength: resp.QueueLength,
			}
			if f.BitRate != nil {
				rec.BitRate = *f.BitRate
			}
			if f.BitDepth != nil {
				rec.BitDepth = *f.BitDepth
			}
			if f.SampleRate != nil {
				rec.SampleRate = *f.SampleRate
			}
			if f.Length != nil {
				rec.LengthSeconds = *f.Length
			}
			ext := rec.Extension()
			if losslessOnly {
				if ext != "flac" {
					continue
				}
			} else if ext != "flac" && ext != "mp3" && ext != "m4a" && ext != "ogg" && ext != "opus" && ext != "wav" && ext != "aiff" {
				continue
			}
			out = append(out, rec)
		}
	}
	return out
}

// --- scorer (port of scorer.py) --------------------------------------

type scoredTrack struct {
	Result     slskdFile
	Sources    []slskdFile
	Score      float64 // legacy blend (logs + ladder exit)
	MatchScore float64 // identity axis 0..55
	FetchScore float64 // desirability axis -6..35
}

var wordRe = regexp.MustCompile(`\w+`)

func wordSet(text string) map[string]bool {
	out := map[string]bool{}
	for _, w := range wordRe.FindAllString(strings.ToLower(text), -1) {
		out[w] = true
	}
	return out
}

func filenameRelevance(filename, artist, title string) float64 {
	fnWords := wordSet(filename)
	if len(fnWords) == 0 {
		return 0
	}
	artistWords := wordSet(artist)
	titleWords := wordSet(title)
	artistMatch := 0.0
	if len(artistWords) > 0 {
		hit := 0
		for w := range artistWords {
			if fnWords[w] {
				hit++
			}
		}
		artistMatch = float64(hit) / float64(len(artistWords))
	}
	titleMatch := 0.0
	if len(titleWords) > 0 {
		hit := 0
		for w := range titleWords {
			if fnWords[w] {
				hit++
			}
		}
		titleMatch = float64(hit) / float64(len(titleWords))
	}
	return artistMatch*7.5 + titleMatch*7.5
}

func pathRelevance(directory, basename, artist, title string) float64 {
	dirWords := wordSet(directory)
	fnWords := wordSet(basename)
	combined := map[string]bool{}
	for w := range dirWords {
		combined[w] = true
	}
	for w := range fnWords {
		combined[w] = true
	}
	if len(combined) == 0 {
		return 0
	}
	artistWords := wordSet(artist)
	titleWords := wordSet(title)
	artistMatch := 0.0
	if len(artistWords) > 0 {
		hit := 0
		for w := range artistWords {
			if combined[w] {
				hit++
			}
		}
		artistMatch = float64(hit) / float64(len(artistWords))
	}
	titleMatch := 0.0
	if len(titleWords) > 0 {
		hit := 0
		for w := range titleWords {
			if fnWords[w] {
				hit++
			}
		}
		titleMatch = float64(hit) / float64(len(titleWords))
	}
	return artistMatch*7.5 + titleMatch*7.5
}

var placeholderArtists = map[string]bool{"various artists": true, "unknown artist": true}

func artistInPath(directory, basename, artist string) bool {
	if placeholderArtists[strings.ToLower(strings.TrimSpace(artist))] {
		return true
	}
	artistWords := wordSet(artist)
	significant := map[string]bool{}
	for w := range artistWords {
		if len(w) >= 3 {
			significant[w] = true
		}
	}
	if len(significant) == 0 {
		return true
	}
	pathWords := wordSet(directory)
	for w := range wordSet(basename) {
		pathWords[w] = true
	}
	for w := range significant {
		if pathWords[w] {
			return true
		}
	}
	return false
}

// durationScore returns 0..40, or -1 past the 30 s hard cutoff
// (caller excludes the candidate).
func durationScore(diff int, tolerance, hardCutoff int) float64 {
	d := float64(diff)
	if diff < 0 {
		d = -d
	}
	if d <= float64(tolerance) {
		return 40.0 - d*2
	}
	if d <= 10 {
		return 25.0 - (d-float64(tolerance))*3
	}
	if d <= float64(hardCutoff) {
		return math.Max(0, 10.0-(d-10)*0.5)
	}
	return -1
}

func exceedsCap(bitDepth, sampleRate int) bool {
	if soulseekMaxBitDepth != 0 && bitDepth != 0 && bitDepth > soulseekMaxBitDepth {
		return true
	}
	if soulseekMaxSampleRateHz != 0 && sampleRate != 0 && sampleRate > soulseekMaxSampleRateHz {
		return true
	}
	return false
}

func qualityScore(bitDepth, sampleRate int) float64 {
	if exceedsCap(bitDepth, sampleRate) {
		return -1000
	}
	return 10
}

func qualityScoreForResult(r slskdFile) float64 {
	if r.IsLossless() {
		return qualityScore(r.BitDepth, r.SampleRate)
	}
	return 5
}

func reliabilityScore(hasFreeSlot bool, uploadSpeed int64, queueLength int) float64 {
	score := 0.0
	if hasFreeSlot {
		score += 10
	}
	if uploadSpeed > 0 {
		speed := float64(uploadSpeed) / 1_000_000
		if speed > 10 {
			speed = 10
		}
		score += speed * 0.8
	}
	switch {
	case queueLength == 0:
		score += 7
	case queueLength <= 2:
		score += 3
	case queueLength <= 5:
		score += 1
	case queueLength <= 15:
		score -= 2
	default:
		pen := float64(queueLength) / 5.0
		if pen > 6 {
			pen = 6
		}
		score -= pen
	}
	return score
}

func versionPenalty(filenameLower, sourceTitleLower string) float64 {
	for _, kw := range soulseekDifferentVersionKeywords {
		if strings.Contains(filenameLower, kw) && !strings.Contains(sourceTitleLower, kw) {
			return 0.30
		}
	}
	for _, kw := range soulseekRemasterKeywords {
		if strings.Contains(filenameLower, kw) && !strings.Contains(sourceTitleLower, kw) {
			return 0.75
		}
	}
	return 1.0
}

// scoreTrackResults scores and ranks candidates (scorer.py
// score_track_results). Copies of the same recording (identical
// basename on several peers) are grouped: one ranking entry, all
// copies kept in Sources, best first. Candidates below the match
// floor (20) are dropped.
func scoreTrackResults(results []slskdFile, artist, title string, durationSec int) []scoredTrack {
	type key struct {
		base string
	}
	groups := map[string]*scoredTrack{}
	var order []string
	for _, r := range results {
		if !artistInPath(r.Directory(), r.Basename(), artist) {
			continue
		}
		dScore := 40.0
		if durationSec > 0 && r.LengthSeconds > 0 {
			diff := r.LengthSeconds - durationSec
			if diff < 0 {
				diff = -diff
			}
			ds := durationScore(diff, 5, 30)
			if ds < 0 {
				continue // past the hard cutoff
			}
			dScore = ds
		} else if durationSec > 0 {
			dScore = 32.0 // unknown peer duration: neutral, not zero
		}
		nameScore := math.Max(filenameRelevance(r.Basename(), artist, title),
			pathRelevance(r.Directory(), r.Basename(), artist, title))
		match := (dScore + nameScore) * versionPenalty(strings.ToLower(r.Basename()), strings.ToLower(title))
		fetch := reliabilityScore(r.HasFreeSlot, r.UploadSpeed, r.QueueLength) + qualityScoreForResult(r)
		if match < soulseekMatchFloor {
			continue
		}
		blend := match + fetch
		k := strings.ToLower(r.Basename())
		if g, ok := groups[k]; ok {
			g.Sources = append(g.Sources, r)
			// Keep the group's representative as its best copy.
			if fetch > g.FetchScore {
				g.Result = r
				g.FetchScore = fetch
				g.Score = g.MatchScore + fetch
			}
			continue
		}
		groups[k] = &scoredTrack{Result: r, Sources: []slskdFile{r},
			Score: blend, MatchScore: match, FetchScore: fetch}
		order = append(order, k)
	}
	out := make([]scoredTrack, 0, len(groups))
	for _, k := range order {
		g := groups[k]
		sort.Slice(g.Sources, func(i, j int) bool {
			return reliabilityScore(g.Sources[i].HasFreeSlot, g.Sources[i].UploadSpeed, g.Sources[i].QueueLength) >
				reliabilityScore(g.Sources[j].HasFreeSlot, g.Sources[j].UploadSpeed, g.Sources[j].QueueLength)
		})
		out = append(out, *g)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].MatchScore != out[j].MatchScore {
			return out[i].MatchScore > out[j].MatchScore
		}
		return out[i].FetchScore > out[j].FetchScore
	})
	return out
}

// --- transfers -------------------------------------------------------

func (s *SoulseekResolver) enqueue(ctx context.Context, username string, files []slskdFile) error {
	payload := make([]map[string]any, 0, len(files))
	for _, f := range files {
		payload = append(payload, map[string]any{"filename": f.Filename, "size": f.Size})
	}
	_, _, err := s.slskdRequest(ctx, http.MethodPost, "/transfers/downloads/"+url.PathEscape(username), payload)
	return err
}

// waitForFile polls slskd until the file completes or fails,
// reporting progress through onProgress. Returns the final state.
func (s *SoulseekResolver) waitForFile(ctx context.Context, username, filename, runID string, onProgress ProgressFunc) (string, error) {
	deadline := time.Now().Add(slskdDownloadTimeout())
	start := time.Now()
	stallSecs := 120.0
	if v := strings.TrimSpace(os.Getenv("SLSKD_STALL_SECS")); v != "" {
		if n, err := strconv.ParseFloat(v, 64); err == nil {
			stallSecs = n
		}
	}
	var lastBytes int64 = -1
	var lastGain time.Time = time.Now()
	for time.Now().Before(deadline) {
		select {
		case <-ctx.Done():
			return "", ctx.Err()
		case <-time.After(2 * time.Second):
		}
		rows, err := s.getDownloads(ctx, username)
		if err != nil {
			continue
		}
		for _, row := range rows {
			fname, _ := row["filename"].(string)
			if fname != filename {
				continue
			}
			state, _ := row["state"].(string)
			transferred, _ := row["bytesTransferred"].(float64)
			size, _ := row["size"].(float64)
			if int64(transferred) > lastBytes {
				lastBytes = int64(transferred)
				lastGain = time.Now()
			}
			if onProgress != nil {
				p := Progress{RunID: runID, DoneBytes: int64(transferred), ElapsedSeconds: time.Since(start).Seconds(), Phase: "downloading"}
				if size > 0 {
					t := int64(size)
					p.TotalBytes = &t
					p.Percentage = math.Min(100, transferred/size*100)
				}
				onProgress(p)
			}
			low := strings.ToLower(state)
			if strings.Contains(low, "completed") && strings.Contains(low, "succeeded") {
				return state, nil
			}
			if strings.Contains(low, "errored") || strings.Contains(low, "rejected") ||
				strings.Contains(low, "timedout") || strings.Contains(low, "cancelled") ||
				strings.Contains(low, "failed") {
				return state, nil
			}
			if stallSecs > 0 && !strings.Contains(low, "queued") && time.Since(lastGain) > time.Duration(stallSecs*float64(time.Second)) {
				return "Stalled, ClientSide", nil
			}
		}
	}
	return "", fmt.Errorf("Soulseek transfer of %q timed out after %s", filename, slskdDownloadTimeout())
}

// getDownloads returns the flattened download rows for a user.
func (s *SoulseekResolver) getDownloads(ctx context.Context, username string) ([]map[string]any, error) {
	raw, _, err := s.slskdRequest(ctx, http.MethodGet, "/transfers/downloads/"+url.PathEscape(username), nil)
	if err != nil {
		return nil, err
	}
	var entry struct {
		Username    string `json:"username"`
		Directories []struct {
			Files []map[string]any `json:"files"`
		} `json:"directories"`
	}
	if err := json.Unmarshal(raw, &entry); err != nil {
		return nil, err
	}
	var out []map[string]any
	for _, d := range entry.Directories {
		for _, f := range d.Files {
			f["username"] = entry.Username
			out = append(out, f)
		}
	}
	return out, nil
}

// locateDownloaded finds the finished file under the slskd download
// dir (SLSKD_DOWNLOAD_DIR) and copies it into destDir when they
// differ. slskd writes downloads as <download-dir>/<username>/<basename>.
func (s *SoulseekResolver) locateDownloaded(ctx context.Context, username, remoteFilename, destDir string) (string, error) {
	dlDir := strings.TrimSpace(os.Getenv("SLSKD_DOWNLOAD_DIR"))
	if dlDir == "" {
		dlDir = filepath.Join(DataDir(), "soulseek")
	}
	base := filepath.Base(strings.ReplaceAll(remoteFilename, "\\", "/"))
	candidates := []string{
		filepath.Join(dlDir, username, base),
		filepath.Join(dlDir, base),
	}
	for _, p := range candidates {
		if st, err := os.Stat(p); err == nil && st.Size() > 0 {
			if destDir != "" && filepath.Clean(destDir) != filepath.Clean(filepath.Dir(p)) {
				if err := os.MkdirAll(destDir, 0o755); err != nil {
					return "", err
				}
				dst := filepath.Join(destDir, base)
				if err := copyFile(p, dst); err != nil {
					return "", err
				}
				return dst, nil
			}
			return p, nil
		}
	}
	return "", fmt.Errorf("Soulseek transfer completed but the file %q was not found under %s", base, dlDir)
}

// splitArtistTitle splits "Artist - Title" (or "Artist: Title").
// A query with no separator is treated as a title.
func splitArtistTitle(q string) (string, string) {
	for _, sep := range []string{" - ", " – ", ": "} {
		if i := strings.Index(q, sep); i > 0 {
			return strings.TrimSpace(q[:i]), strings.TrimSpace(q[i+len(sep):])
		}
	}
	return "", strings.TrimSpace(q)
}

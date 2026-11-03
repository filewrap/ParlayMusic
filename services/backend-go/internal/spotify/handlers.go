// Handlers for spotify.resolve / collection / status / dislike /
// skip_check / details / analyze (inventory §6.1–§6.4, §6.7–§6.11).
package spotify

import (
	"context"
	"net/url"
	"strings"
	"time"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/uds"
)

// --- resolve ----------------------------------------------------------

func (s *Service) handleResolve(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		URL string `json:"url"`
	}
	if err := decodePayload(req, &body); err != nil {
		return nil, err
	}
	kind, id, err := parseSpotifyRef(body.URL)
	if err != nil {
		return nil, err
	}
	switch kind {
	case "track", "album", "playlist", "artist":
	default:
		return nil, badRequest("unknown Spotify kind %q (want track|album|playlist|artist)", kind)
	}
	out := map[string]any{
		"kind":     kind,
		"source":   "spotify",
		"track_id": id,
		"url":      canonicalURL(kind, id),
	}
	if !s.appCredsOK() {
		// Honest degradation: parse-only, never a fake track.
		out["api"] = "none"
		out["note"] = "Spotify credentials not configured (SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET); parsed the reference only, no Web API fetch"
		return out, nil
	}
	out["api"] = "spotify-web-api"
	if kind == "track" {
		// Metadata + audio through the Tier 1→2→3 ladder (the
		// registry's Spotify resolver: metadata from Spotify,
		// audio from the YouTube ladder, never spotdl).
		if s.registry != nil {
			if track, rerr := s.registry.Resolve(ctx, body.URL); rerr == nil {
				out["track"] = track
				return out, nil
			} else {
				return nil, engineUnavailable("Spotify track lookup failed for %s: %v", id, rerr)
			}
		}
		var data spotifyTrackJSON
		if err := s.apiGet(ctx, "/v1/tracks/"+url.PathEscape(id), &data); err != nil {
			return nil, err
		}
		out["track"] = spotifyTrackFromJSON(data)
		return out, nil
	}
	// Collection kinds: fetch name + total for the response.
	var data struct {
		Name   string `json:"name"`
		Tracks struct {
			Total int `json:"total"`
		} `json:"tracks"`
		Followers struct {
			Total int `json:"total"`
		} `json:"followers"`
	}
	path := "/v1/" + kind + "s/" + url.PathEscape(id)
	if kind == "artist" {
		path = "/v1/artists/" + url.PathEscape(id)
	}
	if err := s.apiGet(ctx, path, &data); err != nil {
		return nil, err
	}
	out["name"] = data.Name
	if kind == "artist" {
		out["total"] = data.Followers.Total
	} else {
		out["total"] = data.Tracks.Total
	}
	return out, nil
}

// --- collection -------------------------------------------------------

func (s *Service) handleCollection(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		URL  string `json:"url"`
		Kind string `json:"kind"`
		ID   string `json:"id"`
	}
	if err := decodePayload(req, &body); err != nil {
		return nil, err
	}
	kind, id := body.Kind, body.ID
	if body.URL != "" {
		k, i, err := parseSpotifyRef(body.URL)
		if err != nil {
			return nil, err
		}
		kind, id = k, i
	}
	if kind == "" || id == "" {
		return nil, badRequest("collection needs a Spotify album/playlist/artist URL (or kind + id)")
	}
	if kind == "track" {
		return nil, badRequest("kind track is a single track — use spotify.resolve")
	}
	switch kind {
	case "album", "playlist", "artist":
	default:
		return nil, badRequest("unknown Spotify collection kind %q (want album|playlist|artist)", kind)
	}
	if !s.appCredsOK() {
		// spotify-rust rule: the downloaders' yt-dlp/oEmbed
		// fallback lives in download.spotify_playlist, not here.
		return nil, engineUnavailable("Spotify credentials not configured: SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET are not set")
	}
	const maxTracks = 50
	var name string
	var tracks []model.Track
	var total int
	switch kind {
	case "album":
		var album struct {
			Name   string `json:"name"`
			Tracks struct {
				Items []spotifyTrackJSON `json:"items"`
				Total int                `json:"total"`
			} `json:"tracks"`
		}
		if err := s.apiGet(ctx, "/v1/albums/"+url.PathEscape(id)+"/tracks?limit=50", &album); err != nil {
			return nil, err
		}
		// The /tracks sub-resource carries no album name; fetch it.
		var meta struct {
			Name string `json:"name"`
		}
		if err := s.apiGet(ctx, "/v1/albums/"+url.PathEscape(id), &meta); err == nil {
			name = meta.Name
		}
		total = album.Tracks.Total
		for _, item := range album.Tracks.Items {
			tracks = append(tracks, spotifyTrackFromJSON(item))
		}
	case "playlist":
		var meta struct {
			Name   string `json:"name"`
			Tracks struct {
				Total int `json:"total"`
			} `json:"tracks"`
		}
		if err := s.apiGet(ctx, "/v1/playlists/"+url.PathEscape(id), &meta); err != nil {
			return nil, err
		}
		name = meta.Name
		total = meta.Tracks.Total
		var page struct {
			Items []struct {
				Track *spotifyTrackJSON `json:"track"`
			} `json:"items"`
		}
		if err := s.apiGet(ctx, "/v1/playlists/"+url.PathEscape(id)+"/tracks?limit=50", &page); err != nil {
			return nil, err
		}
		for _, item := range page.Items {
			if item.Track != nil && item.Track.Name != "" {
				tracks = append(tracks, spotifyTrackFromJSON(*item.Track))
			}
		}
	case "artist":
		var meta struct {
			Name string `json:"name"`
		}
		if err := s.apiGet(ctx, "/v1/artists/"+url.PathEscape(id), &meta); err != nil {
			return nil, err
		}
		name = meta.Name
		var top struct {
			Tracks []spotifyTrackJSON `json:"tracks"`
		}
		if err := s.apiGet(ctx, "/v1/artists/"+url.PathEscape(id)+"/top-tracks?market=IN", &top); err != nil {
			return nil, err
		}
		total = len(top.Tracks)
		for _, item := range top.Tracks {
			tracks = append(tracks, spotifyTrackFromJSON(item))
		}
	}
	if tracks == nil {
		tracks = []model.Track{}
	}
	truncated := total > maxTracks || len(tracks) > maxTracks
	if len(tracks) > maxTracks {
		tracks = tracks[:maxTracks]
	}
	return map[string]any{
		"api": "spotify-web-api", "kind": kind, "name": name,
		"tracks": tracks, "truncated": truncated, "total": total,
	}, nil
}

// --- status -----------------------------------------------------------

func (s *Service) handleStatus(ctx context.Context, req uds.Request) (any, error) {
	s.appMu.Lock()
	tokenCached := s.appTok != "" && time.Now().Unix() < s.appTokenEx
	s.appMu.Unlock()
	watched, _ := s.store.ListWatches()
	userTokens := 0
	for _, uid := range watched {
		if _, found, _ := s.loadUserToken(uid); found {
			userTokens++
		}
	}
	return map[string]any{
		"api":                    apiName(s.appCredsOK()),
		"credentials_configured": s.appCredsOK(),
		"token_cached":           tokenCached,
		"user_tokens":            userTokens,
		"watching":               len(watched),
		"gemini_configured":      s.geminiKey() != "",
		"poll_interval_secs":     int(s.pollInterval.Seconds()),
		"polling":                s.pollInterval > 0,
	}, nil
}

func apiName(configured bool) string {
	if configured {
		return "spotify-web-api"
	}
	return "none"
}

// --- dislike ----------------------------------------------------------

func (s *Service) handleDislike(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		Action  string `json:"action"`
		TrackID string `json:"track_id"`
		UserID  *int64 `json:"user_id"`
	}
	if err := decodePayload(req, &body); err != nil {
		return nil, err
	}
	if body.UserID == nil || *body.UserID == 0 {
		return nil, badRequest("user_id is required")
	}
	if body.TrackID == "" {
		return nil, badRequest("track_id is required")
	}
	action := body.Action
	if action == "" {
		action = "add"
	}
	switch action {
	case "add":
		if err := s.store.SetDislike(*body.UserID, body.TrackID, true); err != nil {
			return nil, internalErr("Spotify dislike store failed: %v", err)
		}
		return map[string]any{"disliked": true, "user_id": *body.UserID, "track_id": body.TrackID}, nil
	case "remove":
		was, err := s.store.IsDisliked(*body.UserID, body.TrackID)
		if err != nil {
			return nil, internalErr("Spotify dislike lookup failed: %v", err)
		}
		if err := s.store.SetDislike(*body.UserID, body.TrackID, false); err != nil {
			return nil, internalErr("Spotify dislike store failed: %v", err)
		}
		return map[string]any{"disliked": false, "removed": was, "user_id": *body.UserID, "track_id": body.TrackID}, nil
	default:
		return nil, badRequest("unknown dislike action %q (want add|remove)", action)
	}
}

// --- skip_check -------------------------------------------------------

// skipDecision is the outcome of decideSkip.
type skipDecision struct {
	skip                 bool
	reason               *string
	matchedWord          string
	dislikedBy           []int64
	lyricsChecked        bool
	lyricsAvailable      bool
	lyricsSource         string
	classifierProvider   string
	classifierVerdict    string
	classifierConfidence *float64
	classifierDetail     string
}

func strptr(s string) *string { return &s }

// decideSkip implements the inventory §6.8 decision order:
//  1. disliked -> skip, reason disliked.
//  2. No Gemini key: wordlist profanity on title/artist first.
//  3. Lyrics via Genius -> Musixmatch -> LRCLib, then the classifier:
//     Gemini when a key is set (replaces the wordlist verdict),
//     wordlist otherwise.
//  4. ai_generated skips only at confidence >= 0.7.
func (s *Service) decideSkip(ctx context.Context, track model.Track, userID *int64) (*skipDecision, error) {
	if track.ID == "" {
		return nil, badRequest("track.id is required")
	}
	var requester int64
	if userID != nil {
		requester = *userID
	}
	// 1. Disliked (requesting user's set, or any watched user's set
	// when no user is given).
	dislikedBy := s.dislikedByUsers(track.ID, requester)
	if len(dislikedBy) > 0 {
		return &skipDecision{
			skip: true, reason: strptr("disliked"), dislikedBy: dislikedBy,
			classifierProvider: "none", classifierVerdict: "unknown",
			classifierDetail: "disliked short-circuits classification",
		}, nil
	}
	title, artist := track.Title, track.Artist
	// 2. Wordlist fast path when no Gemini key is configured.
	if s.geminiKey() == "" {
		if word := profanityCheck(strings.TrimSpace(title + " " + artist)); word != "" {
			return &skipDecision{
				skip: true, reason: strptr("profanity"), matchedWord: word,
				classifierProvider: "wordlist", classifierVerdict: "profanity",
				classifierDetail: "profanity in title/artist (wordlist)",
			}, nil
		}
	}
	// 3. Lyrics, then the classifier.
	var lyrics *string
	var lyricsSource string
	lyricsAvailable := false
	if title != "" {
		fetched := s.fetchLyrics(ctx, title, artist)
		if fetched.available {
			lyrics = &fetched.lyrics
			lyricsSource = fetched.source
			lyricsAvailable = true
		}
	}
	result := s.classifyTrack(ctx, title, artist, lyrics)
	d := &skipDecision{
		lyricsChecked:        lyricsAvailable,
		lyricsAvailable:      lyricsAvailable,
		lyricsSource:         lyricsSource,
		classifierProvider:   result.provider,
		classifierVerdict:    result.verdict,
		classifierConfidence: result.confidence,
		classifierDetail:     result.detail,
	}
	switch result.verdict {
	case "profanity":
		d.skip = true
		d.reason = strptr("profanity")
		d.matchedWord = result.matched
	case "ai_generated":
		if result.confidence != nil && *result.confidence >= AISkipConfidence {
			d.skip = true
			d.reason = strptr("ai-generated")
		}
	}
	// Persist the verdict as the last skip-check for this track. A
	// store failure here must not fail the check itself (the verdict
	// is the answer; the record is a cache) — the check result is
	// returned regardless.
	_ = s.store.PutSkipCheck(model.SkipCheckRecord{
		TrackID:              track.ID,
		Skip:                 d.skip,
		Reason:               d.reason,
		ClassifierProvider:   d.classifierProvider,
		ClassifierVerdict:    d.classifierVerdict,
		ClassifierConfidence: confidenceValue(d.classifierConfidence),
		ClassifierDetail:     d.classifierDetail,
		CheckedAt:            time.Now().Unix(),
	})
	return d, nil
}

func confidenceValue(f *float64) float64 {
	if f == nil {
		return 0
	}
	return *f
}

func (s *Service) handleSkipCheck(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		Track  model.Track `json:"track"`
		UserID *int64      `json:"user_id"`
	}
	if err := decodePayload(req, &body); err != nil {
		return nil, err
	}
	if body.Track.ID == "" && body.Track.Title == "" {
		return nil, badRequest("missing object field: track (needs id, title)")
	}
	d, err := s.decideSkip(ctx, body.Track, body.UserID)
	if err != nil {
		return nil, err
	}
	var reason any
	if d.reason != nil {
		reason = *d.reason
	}
	var confidence any
	if d.classifierConfidence != nil {
		confidence = *d.classifierConfidence
	}
	out := map[string]any{
		"skip":                  d.skip,
		"reason":                reason,
		"classifier_provider":   d.classifierProvider,
		"classifier_verdict":    d.classifierVerdict,
		"classifier_confidence": confidence,
		"classifier_detail":     d.classifierDetail,
		// Superset fields (inventory §6.8).
		"matched_word":     d.matchedWord,
		"disliked_by":      d.dislikedBy,
		"lyrics_checked":   d.lyricsChecked,
		"lyrics_available": d.lyricsAvailable,
		"lyrics_source":    d.lyricsSource,
	}
	if d.dislikedBy == nil {
		out["disliked_by"] = []int64{}
	}
	return out, nil
}

// --- details / analyze ------------------------------------------------

// lookupTrack fetches one track's metadata via the app token,
// returning the track plus its artist ids (for the genres lookup).
func (s *Service) lookupTrack(ctx context.Context, trackID string) (model.Track, []string, error) {
	var data spotifyTrackJSON
	if err := s.apiGet(ctx, "/v1/tracks/"+url.PathEscape(trackID), &data); err != nil {
		return model.Track{}, nil, err
	}
	if data.Name == "" {
		return model.Track{}, nil, engineUnavailable("Spotify returned no usable track for %s", trackID)
	}
	var artistIDs []string
	for _, a := range data.Artists {
		if a.ID != "" {
			artistIDs = append(artistIDs, a.ID)
		}
	}
	return spotifyTrackFromJSON(data), artistIDs, nil
}

// audioFeatures is the deprecated /v1/audio-features response,
// mapped onto the contract's features shape. Null when Spotify
// refuses it — never faked.
type audioFeatures struct {
	Key              string  `json:"key"`
	Modality         int     `json:"modality"`
	TempoBPM         float64 `json:"tempo_bpm"`
	Acousticness     float64 `json:"acousticness"`
	Danceability     float64 `json:"danceability"`
	Energy           float64 `json:"energy"`
	Instrumentalness float64 `json:"instrumentalness"`
	Liveness         float64 `json:"liveness"`
	Speechiness      float64 `json:"speechiness"`
	Valence          float64 `json:"valence"`
}

func (s *Service) fetchAudioFeatures(ctx context.Context, trackID string) *audioFeatures {
	var raw struct {
		Key              int     `json:"key"`
		Mode             int     `json:"mode"`
		Tempo            float64 `json:"tempo"`
		Acousticness     float64 `json:"acousticness"`
		Danceability     float64 `json:"danceability"`
		Energy           float64 `json:"energy"`
		Instrumentalness float64 `json:"instrumentalness"`
		Liveness         float64 `json:"liveness"`
		Speechiness      float64 `json:"speechiness"`
		Valence          float64 `json:"valence"`
	}
	if err := s.apiGet(ctx, "/v1/audio-features/"+url.PathEscape(trackID), &raw); err != nil {
		return nil
	}
	keys := []string{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}
	keyName := ""
	if raw.Key >= 0 && raw.Key < len(keys) {
		keyName = keys[raw.Key]
	}
	return &audioFeatures{
		Key: keyName, Modality: raw.Mode, TempoBPM: raw.Tempo,
		Acousticness: raw.Acousticness, Danceability: raw.Danceability,
		Energy: raw.Energy, Instrumentalness: raw.Instrumentalness,
		Liveness: raw.Liveness, Speechiness: raw.Speechiness, Valence: raw.Valence,
	}
}

func (s *Service) fetchGenres(ctx context.Context, artistIDs []string) []string {
	// Genres come from /v1/artists?ids= (spotify-rust details).
	// Failure yields an empty list, never invented genres.
	if len(artistIDs) == 0 {
		return []string{}
	}
	var data struct {
		Artists []struct {
			Genres []string `json:"genres"`
		} `json:"artists"`
	}
	q := url.Values{}
	q.Set("ids", strings.Join(artistIDs, ","))
	if err := s.apiGet(ctx, "/v1/artists?"+q.Encode(), &data); err != nil {
		return []string{}
	}
	seen := map[string]bool{}
	var genres []string
	for _, a := range data.Artists {
		for _, g := range a.Genres {
			if !seen[g] {
				seen[g] = true
				genres = append(genres, g)
			}
		}
	}
	if genres == nil {
		return []string{}
	}
	return genres
}

func (s *Service) handleDetails(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		UserID  *int64 `json:"user_id"`
		TrackID string `json:"track_id"`
	}
	if err := decodePayload(req, &body); err != nil {
		return nil, err
	}
	if body.TrackID == "" {
		return nil, badRequest("track_id is required")
	}
	var requester int64
	if body.UserID != nil {
		requester = *body.UserID
	}
	if !s.appCredsOK() {
		return nil, engineUnavailable("Spotify credentials not configured: SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET are not set")
	}
	track, artistIDs, err := s.lookupTrack(ctx, body.TrackID)
	if err != nil {
		return nil, err
	}
	features := s.fetchAudioFeatures(ctx, body.TrackID)
	genres := s.fetchGenres(ctx, artistIDs)
	fetched := s.fetchLyrics(ctx, track.Title, track.Artist)
	var lyrics any
	var lyricsLink any
	var profanity any
	if fetched.available {
		lyrics = fetched.lyrics
		if fetched.pageURL != "" {
			lyricsLink = fetched.pageURL
		}
		if word := profanityCheck(fetched.lyrics); word != "" {
			profanity = word
		}
	}
	return map[string]any{
		"track":       track,
		"features":    features,
		"genres":      genres,
		"disliked_by": s.dislikedByUsers(body.TrackID, requester),
		// No ignore store exists (§9-E8: spotify.ignore is a
		// proposal, not a decided action) — always [].
		"ignored_by": []int64{},
		"profanity":  profanity,
		// No language detection exists — always null, never faked.
		"language":    nil,
		"lyrics":      lyrics,
		"lyrics_link": lyricsLink,
	}, nil
}

func (s *Service) handleAnalyze(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		UserID  *int64 `json:"user_id"`
		TrackID string `json:"track_id"`
	}
	if err := decodePayload(req, &body); err != nil {
		return nil, err
	}
	if body.TrackID == "" {
		return nil, badRequest("track_id is required")
	}
	if !s.appCredsOK() {
		return nil, engineUnavailable("Spotify credentials not configured: SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET are not set")
	}
	track, _, err := s.lookupTrack(ctx, body.TrackID)
	if err != nil {
		return nil, err
	}
	fetched := s.fetchLyrics(ctx, track.Title, track.Artist)
	if !fetched.available {
		return nil, notFound("no lyrics found for track %s", body.TrackID)
	}
	lyrics := fetched.lyrics
	result := s.classifyTrack(ctx, track.Title, track.Artist, &lyrics)
	words := profanityWordsIn(lyrics)
	if words == nil {
		words = []string{}
	}
	return map[string]any{
		"analysis":      analysisText(result),
		"profane_words": words,
	}, nil
}

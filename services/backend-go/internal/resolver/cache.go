// TTL-aware URL/metadata cache (FINAL-ARCHITECTURE.md §9 Phase 4).
//
// Resolved stream URLs + metadata are cached in the embedded store
// (bucket "cache") under the canonical key "<source>:<track_id>".
// Expiry comes from the CDN signature in the resolved URL
// (expire=/expires=/exp= query parameters, unix seconds); when the
// URL carries no signature the configured fallback TTL
// (limits.url_cache_ttl_sec) applies. An expired entry is NEVER
// served: Get returns a miss and drops the entry lazily, so the
// caller re-resolves and stores the fresh URL.
//
// The store is dumb storage (it does not know the clock); this type
// owns the clock, so tests inject a fake one and prove expiry
// without sleeping on real CDN values.
package resolver

import (
	"fmt"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"time"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/store"
)

// Cache is the TTL-aware resolved-track cache over the store.
type Cache struct {
	st         *store.Store
	now        func() time.Time
	defaultTTL time.Duration
}

// NewCache builds a cache over st with the configured fallback TTL
// in seconds (limits.url_cache_ttl_sec). A fallback of 0 means
// "no TTL": entries are stored only when the URL carries its own
// expire= signature.
func NewCache(st *store.Store, defaultTTLSeconds int) *Cache {
	return NewCacheWithClock(st, time.Duration(defaultTTLSeconds)*time.Second, time.Now)
}

// NewCacheWithClock builds a cache with an explicit clock (tests).
func NewCacheWithClock(st *store.Store, defaultTTL time.Duration, now func() time.Time) *Cache {
	if now == nil {
		now = time.Now
	}
	return &Cache{st: st, now: now, defaultTTL: defaultTTL}
}

// Get returns the cached track for key, or false on a miss —
// including when the entry exists but is expired (an expired entry
// is deleted lazily and never served).
func (c *Cache) Get(key string) (model.Track, bool) {
	e, found, err := c.st.GetCacheEntry(key)
	if err != nil || !found {
		return model.Track{}, false
	}
	if e.ExpiresAt <= c.now().Unix() {
		_ = c.st.DeleteCacheEntry(key)
		return model.Track{}, false
	}
	return e.Track, true
}

// Put stores track under its canonical key when it is cacheable:
// a resolved stream URL with a known expiry. It returns false when
// the track is not cacheable (no stream URL, live, radio/file, or
// an already-expired / TTL-less URL). Only cacheable entries are
// ever stored, so the cache can never serve a stale URL.
func (c *Cache) Put(track model.Track) bool {
	if !cacheableTrack(track) {
		return false
	}
	now := c.now()
	exp := ParseStreamExpiry(track.StreamURL, c.defaultTTL, now)
	if !exp.After(now) {
		return false
	}
	err := c.st.PutCacheEntry(CacheKeyForTrack(track), store.CacheEntry{
		Track:     track,
		ExpiresAt: exp.Unix(),
		CachedAt:  now.Unix(),
	})
	return err == nil
}

// Delete drops the cache entry for key.
func (c *Cache) Delete(key string) error { return c.st.DeleteCacheEntry(key) }

// cacheableTrack reports whether a resolved track may be cached:
// it must carry a playable stream URL, must not be live, and must
// not be a radio/file track (a live radio stream has no stable URL
// to cache; a local file has no network latency to hide).
func cacheableTrack(t model.Track) bool {
	if t.ID == "" || t.StreamURL == "" || t.IsLive {
		return false
	}
	switch t.Source {
	case string(SourceRadio), "file":
		return false
	}
	return true
}

// CacheKeyForTrack returns the canonical cache key for a resolved
// track: "<source>:<bare id>".
func CacheKeyForTrack(t model.Track) string {
	return t.Source + ":" + bareTrackID(t)
}

// bareTrackID strips a source prefix from a track id: JioSaavn
// tracks carry "jsvn:<id>", radio tracks "radio:<station>"; every
// other resolver already stores the bare id.
func bareTrackID(t model.Track) string {
	id := t.ID
	for _, p := range []string{t.Source + ":", "jsvn:", "yt:", "radio:"} {
		if strings.HasPrefix(id, p) {
			return strings.TrimPrefix(id, p)
		}
	}
	return id
}

// CacheKeyForQuery derives the canonical cache key for a resolve
// query when the query names one track directly (a URL or an id).
// Free-text queries return false: they name no single track, so
// there is nothing stable to look up (the resolve still stores its
// result under the resolved track's key, so a later direct query
// for that track hits).
func CacheKeyForQuery(query string) (string, bool) {
	src := ClassifyQuery(query)
	switch src {
	case SourceYouTube:
		if id := ExtractYouTubeID(query); id != "" {
			return "youtube:" + id, true
		}
	case SourceJioSaavn:
		if id := extractJioSaavnID(query); id != "" {
			return "jiosaavn:" + id, true
		}
	case SourceDeezer:
		if v := ExtractDeezerURL(query); strings.HasPrefix(v, "track:") {
			return "deezer:" + strings.TrimPrefix(v, "track:"), true
		}
	case SourceNetEase:
		if id := ExtractNetEaseID(query); id != 0 {
			return "netease:" + strconv.FormatInt(id, 10), true
		}
	case SourceSpotify:
		if id := ExtractSpotifyID(query); id != "" {
			return "spotify:" + id, true
		}
	case SourceTikTok:
		if id := ParseTikTokURL(query); id != "" {
			return "tiktok:" + id, true
		}
	case SourceNavidrome:
		if strings.HasPrefix(strings.ToLower(strings.TrimSpace(query)), "navidrome:") {
			id := strings.TrimSpace(query[len("navidrome:"):])
			if id != "" {
				return "navidrome:" + id, true
			}
		}
	}
	return "", false
}

var jioSaavnSongURLRe = regexp.MustCompile(`jiosaavn\.com/song/[^/]+/([A-Za-z0-9_-]+)`)

// extractJioSaavnID returns the bare JioSaavn song id for a direct
// query (jsvn:<id>, jiosaavn:<id>, or a saavn song URL), or "" for
// free text. A bare single token is deliberately NOT treated as an
// id here: Resolve tries it as an id and falls back to search, but
// the cache must not key free text ("madness") as if it were a
// track id.
func extractJioSaavnID(query string) string {
	q := strings.TrimSpace(query)
	lower := strings.ToLower(q)
	if strings.HasPrefix(lower, "jsvn:") {
		return strings.TrimSpace(q[len("jsvn:"):])
	}
	if strings.HasPrefix(lower, "jiosaavn:") {
		return strings.TrimSpace(q[len("jiosaavn:"):])
	}
	if m := jioSaavnSongURLRe.FindStringSubmatch(q); m != nil {
		return m[1]
	}
	return ""
}

// resolveQueryFor builds the resolve query that re-resolves a known
// track (the prefetch path): the track's own URL when the resolver
// accepts it, else the source-prefixed bare id. Soulseek, radio
// and file tracks have no re-resolvable form and report an honest
// error — the caller falls back to the track as-is.
func resolveQueryFor(track model.Track) (string, error) {
	bare := bareTrackID(track)
	switch Source(track.Source) {
	case SourceYouTube:
		if track.URL != "" {
			return track.URL, nil
		}
		return bare, nil
	case SourceJioSaavn:
		return "jsvn:" + bare, nil
	case SourceDeezer:
		if track.URL != "" {
			return track.URL, nil
		}
		return "deezer:track:" + bare, nil
	case SourceNetEase:
		if track.URL != "" {
			return track.URL, nil
		}
		return "netease:" + bare, nil
	case SourceSpotify:
		if track.URL != "" {
			return track.URL, nil
		}
		return "spotify:" + bare, nil
	case SourceTikTok:
		if track.URL != "" {
			return track.URL, nil
		}
		return "", fmt.Errorf("tiktok track %s has no video URL to re-resolve", track.ID)
	case SourceNavidrome:
		return "navidrome:" + bare, nil
	default:
		return "", fmt.Errorf("track %s (source %s) cannot be re-resolved", track.ID, track.Source)
	}
}

// ParseStreamExpiry resolves the expiry of a stream URL: the CDN
// signature wins (expire=/expires=/exp= query parameter, unix
// seconds); with no signature the fallback TTL applies. An
// already-expired signature returns now (the URL is stale on
// arrival); an absurd value (>24h out) is ignored in favour of
// the fallback. A zero fallback with no signature returns now,
// i.e. "do not cache".
func ParseStreamExpiry(rawURL string, fallback time.Duration, now time.Time) time.Time {
	if u, err := url.Parse(rawURL); err == nil {
		q := u.Query()
		for _, key := range []string{"expire", "expires", "exp"} {
			v := strings.TrimSpace(q.Get(key))
			if v == "" {
				continue
			}
			n, err := strconv.ParseInt(v, 10, 64)
			if err != nil {
				continue
			}
			exp := time.Unix(n, 0)
			if !exp.After(now) {
				return now
			}
			if exp.After(now.Add(24 * time.Hour)) {
				continue
			}
			return exp
		}
	}
	if fallback <= 0 {
		return now
	}
	return now.Add(fallback)
}

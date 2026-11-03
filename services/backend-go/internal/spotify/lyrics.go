// Lyrics fetching for the skip-check / details / analyze paths
// (ported from services/spotify-rust/src/main.rs fetch_lyrics*).
// Order: Genius (GENIUS_ACCESS_TOKEN) -> Musixmatch
// (MUSIXMATCH_API_KEY) -> LRCLib (keyless, via the resolver lyrics
// package). Each leg has a 5 s cap; a leg that fails falls through
// to the next — lyrics are never invented, and a total miss is
// reported as unavailable, not an error.
package spotify

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"strings"
	"time"

	"parlay/backend-go/internal/resolver"
)

// lyricsFetch is one fetched lyric sheet.
type lyricsFetch struct {
	lyrics    string
	source    string // genius|musixmatch|lrclib
	pageURL   string // Genius song page, when Genius supplied the lyrics
	available bool
}

// fetchLyrics fetches lyrics for a title/artist in source order.
// Returns available=false when no source has lyrics.
func (s *Service) fetchLyrics(ctx context.Context, title, artist string) lyricsFetch {
	if strings.TrimSpace(title) == "" {
		return lyricsFetch{}
	}
	if geniusConfigured() {
		if f := s.fetchGeniusLyrics(ctx, title, artist); f.available {
			return f
		}
	}
	if musixmatchConfigured() {
		if f := s.fetchMusixmatchLyrics(ctx, title, artist); f.available {
			return f
		}
	}
	if f := s.fetchLRCLibLyrics(ctx, title, artist); f.available {
		return f
	}
	return lyricsFetch{}
}

func geniusConfigured() bool { return strings.TrimSpace(os.Getenv("GENIUS_ACCESS_TOKEN")) != "" }
func musixmatchConfigured() bool {
	return strings.TrimSpace(os.Getenv("MUSIXMATCH_API_KEY")) != ""
}

// fetchGeniusLyrics searches Genius for the track, then scrapes the
// song page for the lyrics (spotify-rust fetch_genius_lyrics_with_url
// + extract_genius_lyrics_html). The page URL is kept as lyrics_link.
func (s *Service) fetchGeniusLyrics(ctx context.Context, title, artist string) lyricsFetch {
	token := strings.TrimSpace(os.Getenv("GENIUS_ACCESS_TOKEN"))
	if token == "" {
		return lyricsFetch{}
	}
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	q := url.Values{}
	q.Set("q", strings.TrimSpace(title+" "+artist))
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "https://api.genius.com/search?"+q.Encode(), nil)
	if err != nil {
		return lyricsFetch{}
	}
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := s.httpClient.Do(req)
	if err != nil {
		return lyricsFetch{}
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return lyricsFetch{}
	}
	var data struct {
		Response struct {
			Hits []struct {
				Result struct {
					URL           string `json:"url"`
					PrimaryArtist struct {
						Name string `json:"name"`
					} `json:"primary_artist"`
				} `json:"result"`
			} `json:"hits"`
		} `json:"response"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
		return lyricsFetch{}
	}
	if len(data.Response.Hits) == 0 {
		return lyricsFetch{}
	}
	pageURL := data.Response.Hits[0].Result.URL
	for _, h := range data.Response.Hits {
		if strings.Contains(strings.ToLower(h.Result.PrimaryArtist.Name), strings.ToLower(firstArtist(artist))) ||
			strings.Contains(strings.ToLower(artist), strings.ToLower(h.Result.PrimaryArtist.Name)) {
			pageURL = h.Result.URL
			break
		}
	}
	if pageURL == "" {
		return lyricsFetch{}
	}
	lyrics := s.scrapeGeniusPage(ctx, pageURL)
	if strings.TrimSpace(lyrics) == "" {
		return lyricsFetch{}
	}
	return lyricsFetch{lyrics: lyrics, source: "genius", pageURL: pageURL, available: true}
}

func firstArtist(artist string) string {
	return strings.TrimSpace(strings.Split(artist, ",")[0])
}

// scrapeGeniusPage fetches a Genius song page and extracts the
// lyrics from the data-lyrics-container divs.
func (s *Service) scrapeGeniusPage(ctx context.Context, pageURL string) string {
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, pageURL, nil)
	if err != nil {
		return ""
	}
	req.Header.Set("User-Agent", "Parlay/1.0")
	resp, err := s.httpClient.Do(req)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return ""
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if err != nil {
		return ""
	}
	return extractGeniusLyricsHTML(string(body))
}

var (
	lyricsContainerRe = regexp.MustCompile(`(?s)<div[^>]*data-lyrics-container[^>]*>`)
	tagRe             = regexp.MustCompile(`<[^>]+>`)
	brRe              = regexp.MustCompile(`(?i)<br\s*/?>`)
)

// extractGeniusLyricsHTML pulls the text out of every
// data-lyrics-container div, turning <br> into newlines and
// stripping the remaining tags.
func extractGeniusLyricsHTML(html string) string {
	var parts []string
	rest := html
	for {
		loc := lyricsContainerRe.FindStringIndex(rest)
		if loc == nil {
			break
		}
		inner := rest[loc[1]:]
		// Take content up to the matching close: scan tag depth.
		depth := 1
		end := -1
		for i := 0; i < len(inner); i++ {
			if inner[i] != '<' {
				continue
			}
			if strings.HasPrefix(inner[i:], "</div") {
				depth--
				if depth == 0 {
					end = i
					break
				}
			} else if strings.HasPrefix(inner[i:], "<div") {
				// An opening div that is not self-closing.
				if j := strings.Index(inner[i:], ">"); j >= 0 && !strings.HasSuffix(strings.TrimSpace(inner[i:i+j]), "/") {
					depth++
				}
			}
		}
		if end < 0 {
			break
		}
		chunk := inner[:end]
		chunk = brRe.ReplaceAllString(chunk, "\n")
		chunk = tagRe.ReplaceAllString(chunk, "")
		chunk = strings.TrimSpace(unescapeHTMLEntities(chunk))
		if chunk != "" {
			parts = append(parts, chunk)
		}
		rest = inner[end:]
	}
	return strings.Join(parts, "\n")
}

func unescapeHTMLEntities(s string) string {
	r := strings.NewReplacer(
		"&amp;", "&", "&lt;", "<", "&gt;", ">", "&quot;", `"`,
		"&#39;", "'", "&apos;", "'", "&nbsp;", " ",
	)
	return r.Replace(s)
}

// fetchMusixmatchLyrics searches Musixmatch, then fetches the lyrics
// body (spotify-rust fetch_musixmatch_lyrics_with). The commercial-use
// footer Musixmatch appends is trimmed.
func (s *Service) fetchMusixmatchLyrics(ctx context.Context, title, artist string) lyricsFetch {
	key := strings.TrimSpace(os.Getenv("MUSIXMATCH_API_KEY"))
	if key == "" {
		return lyricsFetch{}
	}
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	q := url.Values{}
	q.Set("q_track", strings.TrimSpace(title))
	q.Set("q_artist", firstArtist(artist))
	q.Set("apikey", key)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "https://api.musixmatch.com/ws/1.1/track.search?"+q.Encode(), nil)
	if err != nil {
		return lyricsFetch{}
	}
	resp, err := s.httpClient.Do(req)
	if err != nil {
		return lyricsFetch{}
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return lyricsFetch{}
	}
	var search struct {
		Message struct {
			Body struct {
				TrackList []struct {
					Track struct {
						TrackID int64 `json:"track_id"`
					} `json:"track"`
				} `json:"track_list"`
			} `json:"body"`
		} `json:"message"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&search); err != nil {
		return lyricsFetch{}
	}
	if len(search.Message.Body.TrackList) == 0 {
		return lyricsFetch{}
	}
	trackID := search.Message.Body.TrackList[0].Track.TrackID
	q2 := url.Values{}
	q2.Set("track_id", fmt.Sprint(trackID))
	q2.Set("apikey", key)
	req2, err := http.NewRequestWithContext(ctx, http.MethodGet, "https://api.musixmatch.com/ws/1.1/track.lyrics.get?"+q2.Encode(), nil)
	if err != nil {
		return lyricsFetch{}
	}
	resp2, err := s.httpClient.Do(req2)
	if err != nil {
		return lyricsFetch{}
	}
	defer resp2.Body.Close()
	if resp2.StatusCode != http.StatusOK {
		return lyricsFetch{}
	}
	var lyricsResp struct {
		Message struct {
			Body struct {
				Lyrics struct {
					LyricsBody string `json:"lyrics_body"`
				} `json:"lyrics"`
			} `json:"body"`
		} `json:"message"`
	}
	if err := json.NewDecoder(resp2.Body).Decode(&lyricsResp); err != nil {
		return lyricsFetch{}
	}
	body := strings.TrimSpace(lyricsResp.Message.Body.Lyrics.LyricsBody)
	if idx := strings.Index(body, "***"); idx >= 0 {
		body = strings.TrimSpace(body[:idx])
	}
	if body == "" {
		return lyricsFetch{}
	}
	return lyricsFetch{lyrics: body, source: "musixmatch", available: true}
}

// fetchLRCLibLyrics fetches lyrics from LRCLib via the resolver
// lyrics package (synced preferred, plain otherwise).
func (s *Service) fetchLRCLibLyrics(ctx context.Context, title, artist string) lyricsFetch {
	if s.registry == nil {
		return lyricsFetch{}
	}
	res, err := s.registry.ResolverFor(resolver.SourceNetEase)
	if err != nil {
		return lyricsFetch{}
	}
	ne, ok := res.(*resolver.NetEaseResolver)
	if !ok {
		return lyricsFetch{}
	}
	lr := resolver.NewLyricsResolver(ne)
	result, err := lr.Lyrics(ctx, title, artist, "")
	if err != nil || result == nil || strings.TrimSpace(result.Lyrics) == "" {
		return lyricsFetch{}
	}
	return lyricsFetch{lyrics: result.Lyrics, source: "lrclib", available: true}
}

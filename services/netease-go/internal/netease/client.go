// Package netease is a client for NetEase Cloud Music (music.163.com).
//
// Search, song detail and lyrics use the plain anonymous endpoints. The
// song-URL endpoints need the encrypted eapi envelope ported from
// Music163Api-Go (internal/netease/eapi.go, the library vendored by
// refs/Music163bot-Go): AES-ECB + MD5 splice, POST to /eapi/....
//
// Endpoints used:
//
//	POST https://music.163.com/api/cloudsearch/pc        search (form: s, type=1, limit, offset)
//	GET  https://music.163.com/api/song/detail?ids=[id]   track metadata
//	POST https://music.163.com/eapi/song/enhance/player/url/v1   song URL (eapi, levels standard→hires)
//	POST https://music.163.com/eapi/song/enhance/download/url   download URL fallback (eapi, br=999000)
//	GET  https://music.163.com/api/song/lyric?id=&lv=-1&kv=-1&tv=-1
//
// The sandbox egress proxy is honoured automatically: http.DefaultTransport
// reads HTTPS_PROXY/HTTP_PROXY from the environment.
package netease

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

const (
	baseURL       = "https://music.163.com"
	userAgent     = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
	requestTimout = 10 * time.Second
	// MaxBotAPIBYTES is the 50 MB Bot API cap from contracts/ipc.json.
	MaxBotAPIBYTES = 50 * 1024 * 1024
)

// Track is the Parlay contract Track (contracts/ipc.json).
type Track struct {
	ID          string `json:"id"`
	Title       string `json:"title"`
	Artist      string `json:"artist,omitempty"`
	DurationSec int    `json:"duration_sec,omitempty"`
	Source      string `json:"source"`
	Thumbnail   string `json:"thumbnail,omitempty"`
	URL         string `json:"url,omitempty"`
}

// Client talks to music.163.com anonymously.
type Client struct {
	http *http.Client // API calls: 10s timeout
	// dl is the download client: same transport, but a long timeout, because
	// a 35 MB FLAC takes longer than the 10s API timeout to stream.
	dl *http.Client
}

// New returns a client with a short timeout and proxy-aware transport.
func New() *Client {
	return &Client{
		http: &http.Client{Timeout: requestTimout},
		dl:   &http.Client{Timeout: 10 * time.Minute},
	}
}

func (c *Client) do(req *http.Request) ([]byte, error) {
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Referer", "https://music.163.com/")
	resp, err := c.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("netease request failed: %w", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, fmt.Errorf("netease read failed: %w", err)
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("netease HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
	}
	if len(body) == 0 {
		return nil, fmt.Errorf("netease returned an empty response (HTTP %d)", resp.StatusCode)
	}
	return body, nil
}

// decodeFirst decodes the first JSON value in body. NetEase (via the
// sandbox egress proxy) intermittently returns the same JSON object twice
// concatenated; json.Unmarshal rejects that, a streaming decoder does not.
func decodeFirst(body []byte, v any) error {
	return json.NewDecoder(strings.NewReader(string(body))).Decode(v)
}

type apiSong struct {
	ID       int64  `json:"id"`
	Name     string `json:"name"`
	Duration int64  `json:"dt"` // milliseconds
	Artists  []struct {
		Name string `json:"name"`
	} `json:"ar"`
	Album struct {
		Name   string `json:"name"`
		PicURL string `json:"picUrl"`
	} `json:"al"`
}

func (s apiSong) toTrack() Track {
	artists := make([]string, 0, len(s.Artists))
	for _, a := range s.Artists {
		artists = append(artists, a.Name)
	}
	return Track{
		ID:          strconv.FormatInt(s.ID, 10),
		Title:       s.Name,
		Artist:      strings.Join(artists, ", "),
		DurationSec: int(s.Duration / 1000),
		Source:      "netease",
		Thumbnail:   s.Album.PicURL,
		URL:         "https://music.163.com/song?id=" + strconv.FormatInt(s.ID, 10),
	}
}

// SearchSong searches NetEase (type=1: single tracks).
func (c *Client) SearchSong(query string, limit int) ([]Track, error) {
	if limit <= 0 || limit > 30 {
		limit = 10
	}
	form := url.Values{}
	form.Set("s", query)
	form.Set("type", "1")
	form.Set("limit", strconv.Itoa(limit))
	form.Set("offset", "0")
	req, err := http.NewRequest(http.MethodPost, baseURL+"/api/cloudsearch/pc",
		strings.NewReader(form.Encode()))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	body, err := c.do(req)
	if err != nil {
		return nil, err
	}
	var res struct {
		Code   int `json:"code"`
		Result struct {
			Songs     []apiSong `json:"songs"`
			SongCount int       `json:"songCount"`
		} `json:"result"`
	}
	if err := decodeFirst(body, &res); err != nil {
		return nil, fmt.Errorf("netease search: bad JSON: %w", err)
	}
	if res.Code != 200 {
		return nil, fmt.Errorf("netease search: API code %d", res.Code)
	}
	tracks := make([]Track, 0, len(res.Result.Songs))
	for _, s := range res.Result.Songs {
		tracks = append(tracks, s.toTrack())
	}
	return tracks, nil
}

// SongDetail fetches metadata for one track id.
func (c *Client) SongDetail(id int64) (Track, error) {
	req, err := http.NewRequest(http.MethodGet,
		baseURL+"/api/song/detail?ids=["+strconv.FormatInt(id, 10)+"]", nil)
	if err != nil {
		return Track{}, err
	}
	body, err := c.do(req)
	if err != nil {
		return Track{}, err
	}
	var res struct {
		Code  int       `json:"code"`
		Songs []apiSong `json:"songs"`
	}
	if err := decodeFirst(body, &res); err != nil {
		return Track{}, fmt.Errorf("netease detail: bad JSON: %w", err)
	}
	if res.Code != 200 || len(res.Songs) == 0 {
		return Track{}, fmt.Errorf("netease detail: track %d not found (code %d)", id, res.Code)
	}
	return res.Songs[0].toTrack(), nil
}

// urlResult is the shared shape of the v1 song-URL response
// (Music163Api-Go types.SongsURLData).
type urlResult struct {
	Code int `json:"code"`
	Data []struct {
		ID   int64  `json:"id"`
		URL  string `json:"url"`
		Size int64  `json:"size"`
		Code int    `json:"code"`
		Fee  int    `json:"fee"`
		Md5  string `json:"md5"`
		Br   int64  `json:"br"`
	} `json:"data"`
}

// downloadURLResult is the shape of the download-URL endpoint response
// (Music163Api-Go types.SongDownloadURLData): data is a single object,
// not an array.
type downloadURLResult struct {
	Code int `json:"code"`
	Data struct {
		ID   int64  `json:"id"`
		URL  string `json:"url"`
		Size int64  `json:"size"`
		Code int    `json:"code"`
		Fee  int    `json:"fee"`
		Md5  string `json:"md5"`
		Br   int64  `json:"br"`
	} `json:"data"`
}

// songURLLevels is tried in order: free tiers first, then paid tiers. The
// v1 endpoint returns url:"" + a per-track code for tracks the anonymous
// caller may not play at that level (e.g. fee>0 = VIP-only).
var songURLLevels = []string{"standard", "higher", "lossless", "hires"}

// SongURL resolves a playable audio URL for one track id.
//
// Fallback chain (first playable URL wins):
//  1. eapi v1 (/eapi/song/enhance/player/url/v1), levels standard → hires
//     (Music163Api-Go api.GetSongURL)
//  2. weapi v1 (/weapi/song/enhance/player/url/v1, level standard)
//  3. weapi legacy (/weapi/song/enhance/player/url, br 320000)
//  4. eapi download URL (/eapi/song/enhance/download/url, br 999000)
//  5. linuxapi forward of the v1 endpoint (/api/linux/forward)
//  6. Public mirror (music-api.gdstudio.xyz, api.php?types=url) — a
//     third-party NetEase API mirror, used ONLY after all direct encrypted
//     routes return url:null. It returns a direct *.music.126.net CDN URL
//     (NetEase's own audio CDN); the host is allow-listed below and the
//     bytes are verified by the caller. This is what makes download work
//     from the sandbox egress IP today; on a host where NetEase serves
//     anonymous URLs directly, steps 1–5 answer first and the mirror is
//     never contacted.
//
// All five direct steps are implemented and verified to be accepted by the
// server (well-formed code-200 responses). From the sandbox egress IP
// NetEase still answers url:null/code 404 for every track tried —
// anonymous playback is gated server-side for this IP, liftable only by
// a MUSIC_U login (QR login in Music163bot-Go), which this service does
// not have. That is reported as an error, never faked.
func (c *Client) SongURL(id int64) (string, int64, error) {
	u, size, _, err := c.SongURLWithLevel(id)
	return u, size, err
}

// SongURLWithLevel is SongURL but also reports which step produced the URL
// ("eapi:<level>" | "weapi-v1" | "weapi" | "download" | "linuxapi" |
// "mirror").
func (c *Client) SongURLWithLevel(id int64) (string, int64, string, error) {
	idsJSON, _ := json.Marshal([]string{strconv.FormatInt(id, 10)})
	// 1. eapi v1, levels in order.
	for _, level := range songURLLevels {
		reqJSON, _ := json.Marshal(map[string]string{
			"encodeType": "mp3",
			"ids":        string(idsJSON),
			"level":      level,
		})
		var res urlResult
		if err := c.eapiPost("/api/song/enhance/player/url/v1",
			baseURL+"/eapi/song/enhance/player/url/v1", string(reqJSON), &res); err != nil {
			return "", 0, "", fmt.Errorf("netease song url (eapi v1, level %s): %w", level, err)
		}
		if res.Code != 200 {
			return "", 0, "", fmt.Errorf("netease song url (eapi v1, level %s): API code %d", level, res.Code)
		}
		if len(res.Data) > 0 && res.Data[0].URL != "" {
			return res.Data[0].URL, res.Data[0].Size, "eapi:" + level, nil
		}
		// No URL at this level (code 404 / fee>0): try the next level.
	}
	// 2. weapi v1, level standard.
	reqJSON, _ := json.Marshal(map[string]string{
		"encodeType": "mp3",
		"ids":        string(idsJSON),
		"level":      "standard",
	})
	var res urlResult
	if err := c.weapiPost("/song/enhance/player/url/v1", string(reqJSON), &res); err == nil &&
		len(res.Data) > 0 && res.Data[0].URL != "" {
		return res.Data[0].URL, res.Data[0].Size, "weapi-v1", nil
	}
	// 3. weapi legacy, br 320000.
	reqJSON, _ = json.Marshal(map[string]any{
		"ids": "[" + strconv.FormatInt(id, 10) + "]",
		"br":  320000,
	})
	if err := c.weapiPost("/song/enhance/player/url", string(reqJSON), &res); err == nil &&
		len(res.Data) > 0 && res.Data[0].URL != "" {
		return res.Data[0].URL, res.Data[0].Size, "weapi", nil
	}
	// 4. eapi download URL (api.GetSongDownloadURL, br 999000).
	dlJSON, _ := json.Marshal(map[string]any{"id": id, "br": 999000})
	var dl downloadURLResult
	if err := c.eapiPost("/api/song/enhance/download/url",
		baseURL+"/eapi/song/enhance/download/url", string(dlJSON), &dl); err == nil &&
		dl.Data.URL != "" {
		return dl.Data.URL, dl.Data.Size, "download", nil
	}
	// 5. linuxapi forward of the v1 endpoint.
	inner, _ := json.Marshal(map[string]any{
		"ids": "[" + strconv.FormatInt(id, 10) + "]", "level": "standard",
		"encodeType": "mp3", "br": 320000,
	})
	var lx urlResult
	if err := c.linuxapiPost("/api/song/enhance/player/url/v1", string(inner), &lx); err == nil &&
		len(lx.Data) > 0 && lx.Data[0].URL != "" {
		return lx.Data[0].URL, lx.Data[0].Size, "linuxapi", nil
	}
	// 6. Public mirror fallback (see SongURL doc comment).
	if u, size, err := c.mirrorURL(id); err == nil && u != "" {
		return u, size, "mirror", nil
	}
	return "", 0, "", fmt.Errorf("netease song url: track %d has no playable URL without login (eapi/weapi/linuxapi all returned url:null; mirror fallback also failed)", id)
}

// mirrorURL resolves a playable URL through the public gdstudio NetEase
// mirror (https://music-api.gdstudio.xyz/api.php?types=url). The returned
// URL must point at a NetEase CDN host (*.music.126.net / *.music.163.com);
// anything else is rejected.
func (c *Client) mirrorURL(id int64) (string, int64, error) {
	req, err := http.NewRequest(http.MethodGet,
		"https://music-api.gdstudio.xyz/api.php?types=url&id="+strconv.FormatInt(id, 10)+"&source=netease", nil)
	if err != nil {
		return "", 0, err
	}
	body, err := c.do(req)
	if err != nil {
		return "", 0, fmt.Errorf("netease mirror: %w", err)
	}
	var res struct {
		URL  string `json:"url"`
		Size int64  `json:"size"`
		Br   int64  `json:"br"`
	}
	if err := decodeFirst(body, &res); err != nil {
		return "", 0, fmt.Errorf("netease mirror: bad JSON: %w", err)
	}
	if res.URL == "" {
		return "", 0, fmt.Errorf("netease mirror: no URL for track %d", id)
	}
	u, err := url.Parse(res.URL)
	if err != nil {
		return "", 0, fmt.Errorf("netease mirror: bad URL %q: %w", res.URL, err)
	}
	host := strings.ToLower(u.Hostname())
	if host != "music.163.com" && host != "music.126.net" &&
		!strings.HasSuffix(host, ".music.163.com") && !strings.HasSuffix(host, ".music.126.net") {
		return "", 0, fmt.Errorf("netease mirror: unexpected audio host %q, refusing", u.Hostname())
	}
	return res.URL, res.Size, nil
}

// Lyrics fetches the (original-language) lyrics for one track id.
func (c *Client) Lyrics(id int64) (string, error) {
	req, err := http.NewRequest(http.MethodGet,
		baseURL+"/api/song/lyric?id="+strconv.FormatInt(id, 10)+"&lv=-1&kv=-1&tv=-1", nil)
	if err != nil {
		return "", err
	}
	body, err := c.do(req)
	if err != nil {
		return "", err
	}
	var res struct {
		Code int `json:"code"`
		Lrc  struct {
			Lyric string `json:"lyric"`
		} `json:"lrc"`
	}
	if err := decodeFirst(body, &res); err != nil {
		return "", fmt.Errorf("netease lyrics: bad JSON: %w", err)
	}
	if res.Code != 200 {
		return "", fmt.Errorf("netease lyrics: API code %d", res.Code)
	}
	if res.Lrc.Lyric == "" {
		return "", fmt.Errorf("netease lyrics: no lyrics for track %d", id)
	}
	return res.Lrc.Lyric, nil
}

// Download fetches url to w, aborting once MaxBotAPIBYTES is exceeded.
// It returns the bytes written and oversize=true when the cap was hit.
// Uses the long-timeout download client (c.dl), not the 10s API client.
func (c *Client) Download(url string, w io.Writer) (int64, bool, error) {
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return 0, false, err
	}
	resp, err := c.dl.Do(req)
	if err != nil {
		return 0, false, fmt.Errorf("netease download failed: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return 0, false, fmt.Errorf("netease download: HTTP %d", resp.StatusCode)
	}
	if resp.ContentLength > MaxBotAPIBYTES {
		return 0, true, nil
	}
	n, err := io.Copy(w, io.LimitReader(resp.Body, MaxBotAPIBYTES+1))
	if err != nil {
		return n, false, fmt.Errorf("netease download read failed: %w", err)
	}
	if n > MaxBotAPIBYTES {
		return n, true, nil
	}
	return n, false, nil
}

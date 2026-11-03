// Deezer resolver — native Go ARL gw API + in-process Blowfish-CBC
// decryption (BENCHMARKS.md §2, locked).
//
// Full-track path (deemix algorithm, ported from
// services/downloaders/src/deezer.py — _FORMAT_NUMBERS, _gw_call,
// _song_get_data, _blowfish_key, decryptChunk):
//  1. GET https://www.deezer.com/ with Cookie: arl=<ARL> -> scrape
//     api_token from the page.
//  2. gw-light song.getData (POST ajax/gw-light.php) -> MD5_ORIGIN,
//     TRACK_TOKEN, MEDIA_VERSION, FILESIZE_*.
//  3. Modern URL: gw-light deezer.getUserData -> OPTIONS.license_token,
//     then POST https://media.deezer.com/v1/get_url
//     {license_token, track_tokens, format} -> CDN URL.
//  4. Legacy fallback: AES-ECB-encrypted stream path (key
//     'jo6aey6haid2Teih') -> e-cdns-proxy-<md5[0]>.dzcdn.net/mobile/1/.
//  5. CDN bytes are Blowfish-CBC-encrypted (key derived from the track
//     id, IV 00 01..07); every 2048-byte block at the head of each
//     6144-byte chunk is decrypted while streaming.
//
// Format ladder: FLAC (9) first, then MP3_320 (3), then MP3_128 (1),
// picked from FILESIZE_* availability.
//
// No ARL in the sandbox — the full-track path is env-gated on
// DEEZER_ARL and fails honestly naming it. Without an ARL the public
// API still serves search + 30-second preview MP3s (track.preview),
// which is what Search/Resolve return in that case.
package resolver

import (
	"bytes"
	"context"
	"crypto/aes"
	"crypto/cipher"
	"crypto/md5"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"golang.org/x/crypto/blowfish"
)

// Deezer format numbers (deemix TrackFormats).
const (
	deezerFormatMP3128 = 1
	deezerFormatMP3320 = 3
	deezerFormatFLAC   = 9
)

var deezerFormatNames = map[int]string{
	deezerFormatMP3128: "MP3_128",
	deezerFormatMP3320: "MP3_320",
	deezerFormatFLAC:   "FLAC",
}

var deezerFormatExt = map[string]string{
	"MP3_128": ".mp3",
	"MP3_320": ".mp3",
	"FLAC":    ".flac",
}

// deezerFormatLadder is the locked quality order: FLAC first.
var deezerFormatLadder = []string{"FLAC", "MP3_320", "MP3_128"}

// deezerLegacyURLKey is the AES-ECB key for the legacy stream-path
// encryption (deemix constant, from downloaders deezer.py).
const deezerLegacyURLKey = "jo6aey6haid2Teih"

// deezerBlowfishIV is the fixed Blowfish-CBC IV 00 01..07.
var deezerBlowfishIV = []byte{0, 1, 2, 3, 4, 5, 6, 7}

const (
	deezerAPI     = "https://api.deezer.com"
	deezerGWLight = "https://www.deezer.com/ajax/gw-light.php"
)

var deezerURLRe = regexp.MustCompile(`(?:https?://(?:www\.)?deezer\.com/(?:[a-z]{2}/)?)(track|album|playlist|artist)/(\d+)`)

// DeezerResolver resolves Deezer URLs/ids and free text.
type DeezerResolver struct{}

// NewDeezerResolver builds a Deezer resolver.
func NewDeezerResolver() *DeezerResolver { return &DeezerResolver{} }

// Source implements Resolver.
func (d *DeezerResolver) Source() Source { return SourceDeezer }

// ExtractDeezerURL returns "track" (or kind) for a Deezer link, or
// "" when the input is not one. Only tracks resolve to audio.
func ExtractDeezerURL(input string) string {
	m := deezerURLRe.FindStringSubmatch(strings.TrimSpace(input))
	if m == nil {
		return ""
	}
	return m[1] + ":" + m[2]
}

// arlConfigured reports whether a Deezer ARL cookie is present.
func deezerARLConfigured() bool { return strings.TrimSpace(os.Getenv("DEEZER_ARL")) != "" }

func deezerARL() string { return strings.TrimSpace(os.Getenv("DEEZER_ARL")) }

// missingARLError is the honest failure for every ARL-gated path.
func missingARLError(what string) error {
	return &ErrMissingCredential{Source: SourceDeezer, Missing: "DEEZER_ARL",
		Detail: what + " needs a Deezer arl session cookie (192-char cookie from a logged-in browser session)"}
}

// Resolve handles deezer:track:<id>, a Deezer track URL, a bare
// numeric id, or free text (top public-API search hit).
func (d *DeezerResolver) Resolve(ctx context.Context, query string) (Track, error) {
	q := strings.TrimSpace(query)
	if strings.HasPrefix(strings.ToLower(q), "deezer:") {
		q = q[len("deezer:"):]
		if strings.HasPrefix(strings.ToLower(q), "track:") {
			return d.resolveTrack(ctx, q[len("track:"):])
		}
	}
	if m := deezerURLRe.FindStringSubmatch(q); m != nil && m[1] == "track" {
		return d.resolveTrack(ctx, m[2])
	}
	if _, err := fmt.Sscanf(q, "%d", new(int64)); err == nil && !strings.ContainsAny(q, " \t") {
		return d.resolveTrack(ctx, q)
	}
	tracks, err := d.Search(ctx, q, 1)
	if err != nil {
		return Track{}, err
	}
	if len(tracks) == 0 {
		return Track{}, engineUnavailable(SourceDeezer, "no Deezer results for %q", query)
	}
	return tracks[0], nil
}

// resolveTrack fetches public-API metadata; with an ARL it also
// resolves the full-track stream URL (FLAC first).
func (d *DeezerResolver) resolveTrack(ctx context.Context, id string) (Track, error) {
	var data struct {
		ID       int64  `json:"id"`
		Title    string `json:"title"`
		Duration int    `json:"duration"`
		Link     string `json:"link"`
		Preview  string `json:"preview"`
		Artist   struct {
			Name string `json:"name"`
		} `json:"artist"`
		Album struct {
			CoverXL string `json:"cover_xl"`
			Cover   string `json:"cover_big"`
		} `json:"album"`
	}
	if err := getJSON(ctx, deezerAPI+"/track/"+id, nil, &data); err != nil {
		return Track{}, engineUnavailable(SourceDeezer, "Deezer track %s lookup failed: %v", id, err)
	}
	if data.Title == "" {
		return Track{}, engineUnavailable(SourceDeezer, "Deezer track %s not found", id)
	}
	t := Track{
		ID: fmt.Sprintf("%d", data.ID), Title: data.Title,
		Artist: data.Artist.Name, Source: string(SourceDeezer),
		DurationSec: int64(data.Duration),
		DurationStr: FormatDurationStr(data.Duration, false),
		URL:         data.Link,
		Thumbnail:   data.Album.CoverXL,
	}
	if t.Thumbnail == "" {
		t.Thumbnail = data.Album.Cover
	}
	if deezerARLConfigured() {
		if u, _, err := d.fullTrackURL(ctx, id); err == nil && u != "" {
			t.StreamURL = u
			return t, nil
		}
		// ARL path failed: fall through to the preview rather than
		// failing the resolve — the track is real, only the
		// full-track URL is unavailable.
	}
	t.StreamURL = data.Preview // 30-second preview MP3
	return t, nil
}

// Search runs the public Deezer API track search (no auth).
func (d *DeezerResolver) Search(ctx context.Context, query string, limit int) ([]Track, error) {
	if limit <= 0 {
		limit = 5
	}
	if limit > 20 {
		limit = 20
	}
	q := strings.TrimSpace(strings.TrimPrefix(query, "deezer:"))
	if q == "" {
		return nil, engineUnavailable(SourceDeezer, "empty search query")
	}
	var data struct {
		Data []struct {
			ID       int64  `json:"id"`
			Title    string `json:"title"`
			Duration int    `json:"duration"`
			Link     string `json:"link"`
			Preview  string `json:"preview"`
			Artist   struct {
				Name string `json:"name"`
			} `json:"artist"`
			Album struct {
				CoverXL string `json:"cover_xl"`
				Cover   string `json:"cover_big"`
			} `json:"album"`
		} `json:"data"`
	}
	if err := getJSON(ctx, deezerAPI+"/search?q="+url.QueryEscape(q)+"&limit="+fmt.Sprint(limit), nil, &data); err != nil {
		return nil, engineUnavailable(SourceDeezer, "Deezer search failed: %v", err)
	}
	tracks := make([]Track, 0, len(data.Data))
	for _, item := range data.Data {
		t := Track{
			ID: fmt.Sprintf("%d", item.ID), Title: item.Title,
			Artist: item.Artist.Name, Source: string(SourceDeezer),
			DurationSec: int64(item.Duration),
			DurationStr: FormatDurationStr(item.Duration, false),
			URL:         item.Link,
			Thumbnail:   item.Album.CoverXL,
			StreamURL:   item.Preview,
		}
		if t.Thumbnail == "" {
			t.Thumbnail = item.Album.Cover
		}
		tracks = append(tracks, t)
	}
	return tracks, nil
}

// Download downloads a full track (ARL path, FLAC first) or, without
// an ARL, the 30-second preview. Returns the file path + track.
func (d *DeezerResolver) Download(ctx context.Context, track Track, destDir string, onProgress ProgressFunc) (DownloadResult, error) {
	runID := NewRunID()
	if deezerARLConfigured() {
		u, format, err := d.fullTrackURL(ctx, track.ID)
		if err != nil {
			return DownloadResult{}, err
		}
		ext := deezerFormatExt[format]
		name := SanitizeFilename(track.Artist+" - "+track.Title) + ext
		res, err := downloadDeezerFull(ctx, track, destDir, name, u, runID, onProgress)
		if err == nil {
			res.Format = format
			return res, nil
		}
		return DownloadResult{}, err
	}
	if track.StreamURL == "" {
		full, err := d.Resolve(ctx, track.ID)
		if err != nil {
			return DownloadResult{}, err
		}
		track = full
	}
	if track.StreamURL == "" {
		return DownloadResult{}, missingARLError("Deezer full-track download")
	}
	name := SanitizeFilename(track.Artist+" - "+track.Title) + ".mp3"
	res, err := downloadHTTP(ctx, track, destDir, name, track.StreamURL, runID, onProgress)
	if err != nil {
		return DownloadResult{}, err
	}
	res.Format = "MP3_128"
	return res, nil
}

// fullTrackURL runs the ARL gw flow and returns the CDN URL plus the
// chosen format name. Format ladder: FLAC -> MP3_320 -> MP3_128.
func (d *DeezerResolver) fullTrackURL(ctx context.Context, trackID string) (string, string, error) {
	arl := deezerARL()
	if arl == "" {
		return "", "", missingARLError("Deezer full-track URL")
	}
	apiToken, err := deezerAPIToken(ctx, arl)
	if err != nil {
		return "", "", err
	}
	songData, err := deezerGWCall(ctx, arl, apiToken, "song.getData", map[string]any{"sng_id": trackID})
	if err != nil {
		return "", "", err
	}
	format, formatNum := pickDeezerFormat(songData)
	if format == "" {
		return "", "", engineUnavailable(SourceDeezer, "Deezer track %s cannot be streamed by this account (no FILESIZE for any format)", trackID)
	}
	trackToken, _ := songData["TRACK_TOKEN"].(string)
	md5Origin, _ := songData["MD5_ORIGIN"].(string)
	mediaVersion, _ := songData["MEDIA_VERSION"].(string)
	// Modern path first: license_token -> media.deezer.com get_url.
	if licenseToken, _ := deezerLicenseToken(ctx, arl, apiToken); licenseToken != "" && trackToken != "" {
		if u, err := deezerModernTrackURL(ctx, trackToken, format, licenseToken); err == nil && u != "" {
			return u, format, nil
		}
	}
	// Legacy fallback: AES-ECB stream URL.
	if md5Origin != "" {
		return deezerLegacyStreamURL(trackID, md5Origin, mediaVersion, formatNum), format, nil
	}
	return "", "", engineUnavailable(SourceDeezer, "Deezer track %s: gw returned neither a track token nor MD5_ORIGIN", trackID)
}

// pickDeezerFormat picks the best available format from the
// song.getData FILESIZE_* fields, ladder FLAC -> MP3_320 -> MP3_128.
func pickDeezerFormat(songData map[string]any) (string, int) {
	formatNums := map[string]int{"MP3_128": deezerFormatMP3128, "MP3_320": deezerFormatMP3320, "FLAC": deezerFormatFLAC}
	for _, format := range deezerFormatLadder {
		size := songData["FILESIZE_"+format]
		var n float64
		switch v := size.(type) {
		case float64:
			n = v
		case string:
			fmt.Sscanf(v, "%g", &n)
		}
		if n != 0 {
			return format, formatNums[format]
		}
	}
	return "", 0
}

// deezerAPIToken scrapes the gw-light api_token from the Deezer
// homepage (per-session, only valid with the arl cookie).
func deezerAPIToken(ctx context.Context, arl string) (string, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "https://www.deezer.com/", nil)
	if err != nil {
		return "", err
	}
	req.Header.Set("User-Agent", "Parlay/1.0")
	req.Header.Set("Cookie", "arl="+arl)
	resp, err := httpClient(20 * time.Second).Do(req)
	if err != nil {
		return "", fmt.Errorf("Deezer homepage fetch failed: %w", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return "", err
	}
	m := regexp.MustCompile(`"api_token"\s*:\s*"([A-Za-z0-9]+)"`).FindSubmatch(body)
	if m == nil {
		return "", engineUnavailable(SourceDeezer, "Deezer api_token not found on homepage (ARL expired or Deezer changed its page)")
	}
	return string(m[1]), nil
}

// deezerGWCall performs one gw-light call and returns the results dict.
func deezerGWCall(ctx context.Context, arl, apiToken, method string, payload map[string]any) (map[string]any, error) {
	body, err := json.Marshal(payload)
	if err != nil {
		return nil, err
	}
	url := fmt.Sprintf("%s?method=%s&api_version=1.0&api_token=%s&input=3", deezerGWLight, method, apiToken)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "Parlay/1.0")
	req.Header.Set("Cookie", "arl="+arl)
	req.Header.Set("Content-Type", "application/json")
	resp, err := httpClient(30 * time.Second).Do(req)
	if err != nil {
		return nil, fmt.Errorf("Deezer gw-light %s failed: %w", method, err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, err
	}
	var parsed struct {
		Error   any            `json:"error"`
		Results map[string]any `json:"results"`
	}
	if err := decodeJSON(data, &parsed); err != nil {
		return nil, fmt.Errorf("Deezer gw-light %s: bad JSON: %w", method, err)
	}
	if parsed.Error != nil {
		if m, ok := parsed.Error.(map[string]any); ok && len(m) > 0 {
			return nil, engineUnavailable(SourceDeezer, "Deezer gw-light %s error: %v", method, parsed.Error)
		}
	}
	if parsed.Results == nil {
		return nil, engineUnavailable(SourceDeezer, "Deezer gw-light %s: no results", method)
	}
	return parsed.Results, nil
}

// deezerLicenseToken fetches OPTIONS.license_token via
// deezer.getUserData (nil when the account has none — the modern
// get_url path is then skipped and the legacy URL is used).
func deezerLicenseToken(ctx context.Context, arl, apiToken string) (string, error) {
	data, err := deezerGWCall(ctx, arl, apiToken, "deezer.getUserData", map[string]any{})
	if err != nil {
		return "", nil
	}
	if opts, ok := data["OPTIONS"].(map[string]any); ok {
		if tok, ok := opts["license_token"].(string); ok {
			return tok, nil
		}
	}
	return "", nil
}

// deezerModernTrackURL POSTs media.deezer.com/v1/get_url.
func deezerModernTrackURL(ctx context.Context, trackToken, format, licenseToken string) (string, error) {
	payload, _ := json.Marshal(map[string]any{
		"license_token": licenseToken,
		"track_tokens":  []string{trackToken},
		"format":        format,
	})
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, "https://media.deezer.com/v1/get_url", bytes.NewReader(payload))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", "Parlay/1.0")
	resp, err := httpClient(30 * time.Second).Do(req)
	if err != nil {
		return "", fmt.Errorf("Deezer media get_url failed: %w", err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return "", err
	}
	var parsed struct {
		Data []struct {
			Media []struct {
				Sources []struct {
					URL string `json:"url"`
				} `json:"sources"`
			} `json:"media"`
		} `json:"data"`
	}
	if err := decodeJSON(data, &parsed); err != nil {
		return "", fmt.Errorf("Deezer media get_url: bad JSON: %w", err)
	}
	if len(parsed.Data) == 0 || len(parsed.Data[0].Media) == 0 || len(parsed.Data[0].Media[0].Sources) == 0 {
		return "", engineUnavailable(SourceDeezer, "Deezer media get_url: no URL in response")
	}
	u := parsed.Data[0].Media[0].Sources[0].URL
	if u == "" {
		return "", engineUnavailable(SourceDeezer, "Deezer media get_url: empty URL")
	}
	return u, nil
}

// deezerLegacyStreamURL builds the AES-ECB-encrypted stream URL
// (deemix generateCryptedStreamURL): urlPart = md5 \xa4 format \xa4
// sng_id \xa4 media_version; step2 = md5(urlPart) \xa4 urlPart \xa4,
// '.'-padded to a multiple of 16; AES-ECB encrypt, hex-encode, hang
// under e-cdns-proxy-<first md5 char>.dzcdn.net/mobile/1/.
func deezerLegacyStreamURL(sngID, md5Origin, mediaVersion string, formatNum int) string {
	urlPart := strings.Join([]string{md5Origin, fmt.Sprint(formatNum), sngID, mediaVersion}, "\xa4")
	md5val := md5.Sum([]byte(urlPart))
	step2 := hex.EncodeToString(md5val[:]) + "\xa4" + urlPart + "\xa4"
	for len(step2)%16 != 0 {
		step2 += "."
	}
	block, _ := aes.NewCipher([]byte(deezerLegacyURLKey))
	encrypted := make([]byte, len(step2))
	for i := 0; i < len(step2); i += aes.BlockSize {
		block.Encrypt(encrypted[i:i+aes.BlockSize], []byte(step2[i:i+aes.BlockSize]))
	}
	return fmt.Sprintf("https://e-cdns-proxy-%s.dzcdn.net/mobile/1/%s", string(md5Origin[0]), hex.EncodeToString(encrypted))
}

// deezerBlowfishKey derives the per-track Blowfish key (deemix
// generateBlowfishKey): MD5(str(track_id)) hex, XOR-fold chars
// i ^ i+16 ^ SECRET[i] for i in 0..15 -> 16-byte key.
func deezerBlowfishKey(trackID string) []byte {
	md5hex := md5.Sum([]byte(trackID))
	hexStr := hex.EncodeToString(md5hex[:])
	key := make([]byte, 16)
	for i := 0; i < 16; i++ {
		key[i] = hexStr[i] ^ hexStr[i+16] ^ deezerBlowfishSecret[i]
	}
	return key
}

// deezerDecryptChunk decrypts one chunk with Blowfish-CBC and the
// fixed IV 00 01..07 (deemix decryptChunk). Data must be a multiple
// of 8 bytes.
func deezerDecryptChunk(key, data []byte) ([]byte, error) {
	bfCipher, err := blowfish.NewCipher(key)
	if err != nil {
		return nil, err
	}
	if len(data)%blowfish.BlockSize != 0 {
		return nil, fmt.Errorf("deezer blowfish: chunk length %d is not a multiple of %d", len(data), blowfish.BlockSize)
	}
	out := make([]byte, len(data))
	mode := cipher.NewCBCDecrypter(bfCipher, deezerBlowfishIV)
	mode.CryptBlocks(out, data)
	return out, nil
}

// downloadDeezerFull streams a full track from the CDN,
// Blowfish-decrypting the encrypted chunks (deemix streamTrack:
// 6144-byte chunks, the first 2048 bytes of each chunk are decrypted
// when the URL is a crypted /mobile/ or /media/ stream).
func downloadDeezerFull(ctx context.Context, track Track, destDir, name, url, runID string, onProgress ProgressFunc) (DownloadResult, error) {
	crypted := strings.Contains(url, "/mobile/") || strings.Contains(url, "/media/")
	var key []byte
	if crypted {
		key = deezerBlowfishKey(track.ID)
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return DownloadResult{}, err
	}
	req.Header.Set("User-Agent", "Parlay/1.0")
	resp, err := httpClient(120 * time.Second).Do(req)
	if err != nil {
		return DownloadResult{}, fmt.Errorf("Deezer full-track download failed: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return DownloadResult{}, engineUnavailable(SourceDeezer, "Deezer full-track download: HTTP %d", resp.StatusCode)
	}
	if err := os.MkdirAll(destDir, 0o755); err != nil {
		return DownloadResult{}, err
	}
	destPath := filepath.Join(destDir, name)
	fh, err := os.Create(destPath)
	if err != nil {
		return DownloadResult{}, err
	}
	defer fh.Close()
	start := time.Now()
	var total int64 = -1
	if resp.ContentLength > 0 {
		total = resp.ContentLength
	}
	report := func(phase string, done int64) {
		if onProgress == nil {
			return
		}
		p := Progress{RunID: runID, DoneBytes: done, ElapsedSeconds: time.Since(start).Seconds(), Phase: phase}
		if total > 0 {
			t := total
			p.TotalBytes = &t
			p.Percentage = math.Min(100, float64(done)/float64(total)*100)
			if done > 0 {
				speed := float64(done) / time.Since(start).Seconds()
				p.SpeedBytesPerSec = speed
				eta := int64(float64(total-done) / speed)
				p.ETASeconds = &eta
			}
		}
		onProgress(p)
	}
	var written int64
	buf := make([]byte, 6144)
	for {
		n, rerr := io.ReadFull(resp.Body, buf)
		if n > 0 {
			chunk := buf[:n]
			if crypted && n >= 2048 {
				dec, derr := deezerDecryptChunk(key, chunk[:2048])
				if derr != nil {
					return DownloadResult{}, derr
				}
				chunk = append(dec, chunk[2048:]...)
			}
			if _, werr := fh.Write(chunk); werr != nil {
				return DownloadResult{}, werr
			}
			written += int64(n)
			report("downloading", written)
		}
		if rerr != nil {
			break
		}
	}
	if written < 1024 {
		return DownloadResult{}, engineUnavailable(SourceDeezer, "Deezer full-track download produced an empty file")
	}
	report("done", written)
	return DownloadResult{FilePath: destPath, Track: track, RunID: runID, SizeBytes: written}, nil
}

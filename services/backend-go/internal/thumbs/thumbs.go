// Package thumbs downloads track thumbnails to disk so the core can
// send them as inputFileLocal.
//
// TDLib accepts only a local file path or a Telegram file id as a
// photo (inputFileLocal / inputFileRemote) — an https URL is not a
// valid inputFile, and the rich engine refuses it honestly. The
// backend therefore caches each thumbnail under
// <dataDir>/thumbs/<sha1(lowercased url)>.<ext> on first use and
// hands the core the local path (model.Track.ThumbnailPath). A
// failed download is never fatal: Ensure returns "" and the caller
// falls back to a text card, with no owner error DM — a missing
// thumbnail must not page the owner when the card itself still
// renders.
//
// Cache semantics (queue-port Tier 4, item 26 — Parvati
// artwork.rs, idea ported, not the code): hash-named cache file,
// cache hit returns immediately, HTTP GET with a 15 s timeout and
// a 6 MB cap, at most 5 redirects, content-type must be an image,
// atomic write via a .tmp rename, any failure returns "" and the
// caller falls back to a text card.
package thumbs

import (
	"context"
	"crypto/sha1"
	"encoding/hex"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// maxThumbBytes caps a single thumbnail download (Parvati's 6 MB
// cap, item 26; thumbnails are small, anything bigger is not a
// thumbnail).
const maxThumbBytes = 6 << 20 // 6 MiB

// thumbTimeout bounds one thumbnail fetch (Parvati's 15 s,
// item 26).
const thumbTimeout = 15 * time.Second

// maxThumbRedirects caps the redirect chain (Parvati: at most 5).
const maxThumbRedirects = 5

// NormalizeURL forces the jpg thumbnail variant (queue-port Tier 4,
// item 28 — NoxBeats yt.cpp:68): yt-dlp often reports .webp and
// Telegram only reliably fetches the jpg variant. Only
// i.ytimg.com/vi/<id>/<variant> URLs are rewritten, to
// https://i.ytimg.com/vi/<id>/hqdefault.jpg — the variant that
// always exists for a video, unlike maxresdefault. Every other URL
// is returned unchanged, so non-YouTube thumbnails (which may
// genuinely be .webp with no jpg twin) are never mangled.
// Normalization is idempotent.
func NormalizeURL(rawURL string) string {
	rawURL = strings.TrimSpace(rawURL)
	u, err := url.Parse(rawURL)
	if err != nil || u.Host != "i.ytimg.com" {
		return rawURL
	}
	parts := strings.Split(strings.Trim(u.Path, "/"), "/")
	if len(parts) != 3 || parts[0] != "vi" || parts[1] == "" {
		return rawURL
	}
	u.Path = "/vi/" + parts[1] + "/hqdefault.jpg"
	u.RawQuery = ""
	u.Fragment = ""
	return u.String()
}

// Ensure returns the local path of a cached copy of the thumbnail at
// rawURL, downloading it into <dataDir>/thumbs on first use. It
// returns "" when there is no URL or the download fails. A rawURL
// that is already a local path is returned unchanged. The URL is
// normalized (NormalizeURL) before caching, so the .webp and .jpg
// reports of one thumbnail share one cache file.
func Ensure(ctx context.Context, dataDir, rawURL string) (string, error) {
	rawURL = NormalizeURL(rawURL)
	if rawURL == "" {
		return "", nil
	}
	if !strings.HasPrefix(rawURL, "http://") &&
		!strings.HasPrefix(rawURL, "https://") {
		return rawURL, nil
	}
	dir := filepath.Join(dataDir, "thumbs")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return "", err
	}
	ext := thumbExt(rawURL)
	// Parvati hashes the lowercased URL (FNV-1a there; sha1 here —
	// the hash function is cosmetic, the case-insensitivity is the
	// semantic): the same thumbnail under different URL casings is
	// one cache file.
	sum := sha1.Sum([]byte(strings.ToLower(rawURL)))
	name := hex.EncodeToString(sum[:]) + ext
	path := filepath.Join(dir, name)
	if st, err := os.Stat(path); err == nil && st.Size() > 0 {
		return path, nil
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, rawURL, nil)
	if err != nil {
		return "", err
	}
	req.Header.Set("User-Agent", "Parlay/1.0 (thumbnail fetch)")
	client := &http.Client{
		Timeout: thumbTimeout,
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			if len(via) >= maxThumbRedirects {
				return fmt.Errorf("thumbnail %s: too many redirects", rawURL)
			}
			return nil
		},
	}
	resp, err := client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("thumbnail %s: HTTP %d", rawURL, resp.StatusCode)
	}
	if ct := resp.Header.Get("Content-Type"); ct != "" &&
		!strings.HasPrefix(strings.ToLower(ct), "image/") {
		return "", fmt.Errorf("thumbnail %s: content-type %q is not an image", rawURL, ct)
	}
	tmp := path + ".tmp"
	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o600)
	if err != nil {
		return "", err
	}
	n, copyErr := io.CopyN(f, resp.Body, maxThumbBytes+1)
	if closeErr := f.Close(); closeErr != nil && copyErr == nil {
		copyErr = closeErr
	}
	if copyErr != nil && copyErr != io.EOF {
		_ = os.Remove(tmp)
		return "", copyErr
	}
	if n > maxThumbBytes {
		_ = os.Remove(tmp)
		return "", fmt.Errorf("thumbnail %s: over %d bytes", rawURL, maxThumbBytes)
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return "", err
	}
	log.Printf("thumbs: cached %s -> %s", rawURL, path)
	return path, nil
}

// thumbExt picks a file extension from the URL path (.jpg/.jpeg/.png/
// .webp); anything else defaults to .jpg.
func thumbExt(rawURL string) string {
	u, err := url.Parse(rawURL)
	if err != nil {
		return ".jpg"
	}
	switch strings.ToLower(filepath.Ext(u.Path)) {
	case ".png":
		return ".png"
	case ".webp":
		return ".webp"
	case ".jpeg", ".jpg":
		return ".jpg"
	default:
		return ".jpg"
	}
}

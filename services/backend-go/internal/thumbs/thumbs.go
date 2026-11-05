// Package thumbs downloads track thumbnails to disk so the core can
// send them as inputFileLocal.
//
// TDLib accepts only a local file path or a Telegram file id as a
// photo (inputFileLocal / inputFileRemote) — an https URL is not a
// valid inputFile, and the rich engine refuses it honestly. The
// backend therefore caches each thumbnail under
// <dataDir>/thumbs/<sha1(url)>.<ext> on first use and hands the core
// the local path (model.Track.ThumbnailPath). A failed download is
// never fatal: Ensure returns "" and the caller falls back to a text
// card, with no owner error DM — a missing thumbnail must not page
// the owner when the card itself still renders.
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

// maxThumbBytes caps a single thumbnail download (thumbnails are
// small; anything bigger is not a thumbnail).
const maxThumbBytes = 2 << 20 // 2 MiB

// Ensure returns the local path of a cached copy of the thumbnail at
// rawURL, downloading it into <dataDir>/thumbs on first use. It
// returns "" when there is no URL or the download fails. A rawURL
// that is already a local path is returned unchanged.
func Ensure(ctx context.Context, dataDir, rawURL string) (string, error) {
	rawURL = strings.TrimSpace(rawURL)
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
	sum := sha1.Sum([]byte(rawURL))
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
	client := &http.Client{Timeout: 10 * time.Second}
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

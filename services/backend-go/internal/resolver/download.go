// Shared download helpers for the resolvers: plain HTTP downloads
// with progress reporting (download.progress shape), the warm-daemon
// download path, and small file utilities.
//
// Download targets live under <PARLAY_DATA_DIR>/downloads/
// (contracts/ipc.json download.* responses carry file_path + track +
// run_id; progress snapshots carry percentage, done_bytes,
// total_bytes, speed and eta).
package resolver

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

// readAllLimit reads r up to limit bytes.
func readAllLimit(r io.Reader, limit int64) []byte {
	data, _ := io.ReadAll(io.LimitReader(r, limit))
	return data
}

// newUUID returns a random 128-bit hex id (slskd search ids use
// uuid1 in the Python client; a random hex id is equivalent here —
// / slskd only requires uniqueness).
func newUUID() string {
	var b [16]byte
	_, _ = rand.Read(b[:])
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	return hex.EncodeToString(b[:])
}

// copyFile copies src to dst (same permissions as a fresh file).
func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}

// readFileHead returns the first n bytes of a file.
func readFileHead(path string, n int) ([]byte, error) {
	fh, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer fh.Close()
	buf := make([]byte, n)
	m, err := io.ReadFull(fh, buf)
	if err != nil && err != io.ErrUnexpectedEOF && err != io.EOF {
		return nil, err
	}
	return buf[:m], nil
}

// downloadHTTP downloads url to destDir/name with a plain HTTP GET,
// reporting progress. It is the shared path for JioSaavn (clear CDN
// URL), Deezer previews, Navidrome rest/download and TikTok direct
// CDN fetches.
func downloadHTTP(ctx context.Context, track Track, destDir, name, url, runID string, onProgress ProgressFunc) (DownloadResult, error) {
	return downloadHTTPWithHeaders(ctx, track, destDir, name, url, runID, nil, onProgress)
}

// downloadHTTPWithHeaders is downloadHTTP with extra request headers.
func downloadHTTPWithHeaders(ctx context.Context, track Track, destDir, name, url, runID string, headers map[string]string, onProgress ProgressFunc) (DownloadResult, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return DownloadResult{}, err
	}
	req.Header.Set("User-Agent", "Parlay/1.0")
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	resp, err := httpClient(180 * time.Second).Do(req)
	if err != nil {
		return DownloadResult{}, fmt.Errorf("download failed: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return DownloadResult{}, fmt.Errorf("download: HTTP %d from %s", resp.StatusCode, url)
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
		elapsed := time.Since(start).Seconds()
		p := Progress{RunID: runID, DoneBytes: done, ElapsedSeconds: elapsed, Phase: phase}
		if total > 0 {
			t := total
			p.TotalBytes = &t
			p.Percentage = math.Min(100, float64(done)/float64(total)*100)
			if done > 0 && elapsed > 0 {
				speed := float64(done) / elapsed
				p.SpeedBytesPerSec = speed
				eta := int64(float64(total-done) / speed)
				p.ETASeconds = &eta
			}
		}
		onProgress(p)
	}
	buf := make([]byte, 65536)
	var written int64
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			if _, werr := fh.Write(buf[:n]); werr != nil {
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
		return DownloadResult{}, fmt.Errorf("download produced an empty file (%d bytes)", written)
	}
	report("done", written)
	return DownloadResult{FilePath: destPath, Track: track, RunID: runID, SizeBytes: written}, nil
}

// downloadViaDaemon downloads a track through the persistent yt-dlp
// daemon (bestaudio, extracted to mp3) into destDir. This is the
// download path for YouTube, Spotify (via the YouTube ladder) and
// the TikTok fallback — one daemon call, zero per-call forks.
func downloadViaDaemon(ctx context.Context, client *YtDlpClient, track Track, destDir string, onProgress ProgressFunc) (DownloadResult, error) {
	return downloadViaDaemonWithPoToken(ctx, client, track, destDir, onProgress, "")
}

// downloadViaDaemonWithPoToken is downloadViaDaemon with an
// optional yt-dlp PO token ("web.player+<token>"); "" means a
// plain tokenless download.
func downloadViaDaemonWithPoToken(ctx context.Context, client *YtDlpClient, track Track, destDir string, onProgress ProgressFunc, poToken string) (DownloadResult, error) {
	runID := NewRunID()
	if client == nil {
		return DownloadResult{}, engineUnavailable(SourceYouTube, "yt-dlp daemon is not configured")
	}
	if err := os.MkdirAll(destDir, 0o755); err != nil {
		return DownloadResult{}, err
	}
	if onProgress != nil {
		onProgress(Progress{RunID: runID, Phase: "downloading"})
	}
	url := track.URL
	if url == "" {
		url = "https://www.youtube.com/watch?v=" + track.ID
	}
	path, err := client.DownloadWithPoToken(ctx, url, destDir, poToken)
	if err != nil {
		return DownloadResult{}, err
	}
	st, err := os.Stat(path)
	if err != nil {
		return DownloadResult{}, fmt.Errorf("yt-dlp download produced no file: %w", err)
	}
	if onProgress != nil {
		size := st.Size()
		onProgress(Progress{RunID: runID, Percentage: 100, DoneBytes: size, TotalBytes: &size, Phase: "done"})
	}
	return DownloadResult{FilePath: path, Track: track, RunID: runID, SizeBytes: st.Size()}, nil
}

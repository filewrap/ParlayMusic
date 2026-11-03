// Persistent yt-dlp daemon client (FINAL-ARCHITECTURE.md §1.3 Part 2,
// §8.1). ONE process started once at Go boot, persistent stdin/stdout
// JSON-RPC lines, warm yt_dlp.YoutubeDL in RAM, zero per-call forks.
// This client is the ONLY yt-dlp path in the backend: the fallback
// when native InnerTube hits an age-gated or signature-ciphered edge
// case, the TikTok metadata path, and the YouTube/Spotify download
// path.
//
// Wire protocol (scripts/yt_dlp_worker.py): one JSON object per
// line, both directions. Requests carry {"id": n, "method": m,
// "params": {...}}; responses carry {"id": n, "result": {...}} or
// {"id": n, "error": {"code": ..., "message": ...}}. Methods: ping,
// version, extract (yt-dlp -J metadata), download (bestaudio,
// extracted to mp3).
//
// The daemon is started lazily on first use, never at construction,
// so a backend that only serves native resolvers never spawns it.
// The worker python is located as: $PARLAY_YTDLP_PYTHON, else the
// yt-dlp venv python (scripts/yt-dlp-venv/bin/python — moved there
// with the Phase 3 service deletions, next to the worker script it
// runs; found by walking up from the working directory and the
// executable directory, never a hardcoded user home), else
// python3 — and in every case the python must import yt_dlp (system
// python3 does NOT have it; the venv does, yt-dlp 2026.10.26).
package resolver

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// YtDlpClient speaks JSON-RPC lines to one persistent
// yt_dlp_worker.py process.
type YtDlpClient struct {
	mu          sync.Mutex
	cmd         *exec.Cmd
	stdinWriter io.WriteCloser
	stdout      *bufio.Reader
	started     bool
	nextID      int64
	pending     map[int64]chan rpcResponse
	done        chan struct{}
	startErr    error
}

type rpcResponse struct {
	Result json.RawMessage `json:"result"`
	Error  *struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
}

// NewYtDlpClient builds a daemon client. Nothing is started yet.
func NewYtDlpClient() *YtDlpClient {
	return &YtDlpClient{pending: map[int64]chan rpcResponse{}}
}

// candidateRoots returns the directories to walk up from when
// locating repo files: the working directory first, then the
// backend executable's directory. The binary lives in <repo>/bin
// (repo layout) or <repo>/services/backend-go (go run), so the
// repo root — and its scripts/ — is an ancestor of the exe dir in
// every supported layout, including the VPS tarball (where the CWD
// walk-up alone fails). No hardcoded user home is ever consulted:
// the old absolute home fallback silently degraded to system
// python3 (no yt_dlp) on any other host.
func candidateRoots() []string {
	var roots []string
	if wd, err := os.Getwd(); err == nil {
		roots = append(roots, wd)
	}
	if exe, err := os.Executable(); err == nil {
		roots = append(roots, filepath.Dir(exe))
	}
	return roots
}

// walkUp looks for rel (relative to each candidate root, walking
// up to the filesystem root) and returns the first existing file.
func walkUp(rel string) string {
	for _, root := range candidateRoots() {
		dir := root
		for {
			cand := filepath.Join(dir, rel)
			if st, err := os.Stat(cand); err == nil && !st.IsDir() {
				return cand
			}
			parent := filepath.Dir(dir)
			if parent == dir {
				break
			}
			dir = parent
		}
	}
	return ""
}

// findYtDlpPython locates a python with yt_dlp importable.
func findYtDlpPython() (string, error) {
	if p := strings.TrimSpace(os.Getenv("PARLAY_YTDLP_PYTHON")); p != "" {
		return p, nil
	}
	// The yt-dlp venv python (scripts/yt-dlp-venv/bin/python —
	// moved there with the Phase 3 service deletions, next to the
	// worker script it runs; found by walking up from the working
	// directory and the executable directory).
	if cand := walkUp(filepath.Join("scripts", "yt-dlp-venv", "bin", "python")); cand != "" {
		return cand, nil
	}
	return "python3", nil
}

// findWorkerScript locates scripts/yt_dlp_worker.py.
func findWorkerScript() (string, error) {
	if p := strings.TrimSpace(os.Getenv("PARLAY_YTDLP_WORKER")); p != "" {
		return p, nil
	}
	if cand := walkUp(filepath.Join("scripts", "yt_dlp_worker.py")); cand != "" {
		return cand, nil
	}
	return "", fmt.Errorf("yt-dlp worker script not found (set PARLAY_YTDLP_WORKER or run from the parlay repo)")
}

// ensureStarted starts the daemon once. It reports whether the
// daemon is up (starting it and verifying with a ping on first
// use); concurrent callers share the same start.
func (c *YtDlpClient) ensureStarted(ctx context.Context) error {
	c.mu.Lock()
	if c.started {
		err := c.startErr
		c.mu.Unlock()
		return err
	}
	c.started = true
	python, err := findYtDlpPython()
	if err != nil {
		c.startErr = err
		c.mu.Unlock()
		return err
	}
	script, err := findWorkerScript()
	if err != nil {
		c.startErr = err
		c.mu.Unlock()
		return err
	}
	// NOTE: exec.Command, NOT exec.CommandContext: the daemon is a
	// persistent process that must outlive every caller's context.
	// CommandContext kills the child when ctx expires — and the
	// boot warm-up calls Ping with a 30 s context, so the old code
	// killed the "persistent" daemon 30 s after boot (proven
	// 2026-11-03 in the Wave 2 verification pass: the daemon went
	// defunct ~30 s after startup and the next Extract failed with
	// "write failed: broken pipe"). The ctx still bounds the
	// ping verification below; a failed start is cleaned up by
	// stopLocked, and Close() kills the daemon at shutdown.
	cmd := exec.Command(python, script)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		c.startErr = fmt.Errorf("yt-dlp daemon stdin pipe failed: %w", err)
		c.mu.Unlock()
		return c.startErr
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		c.startErr = fmt.Errorf("yt-dlp daemon stdout pipe failed: %w", err)
		c.mu.Unlock()
		return c.startErr
	}
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		c.startErr = fmt.Errorf("yt-dlp daemon failed to start (%s %s): %w", python, script, err)
		c.mu.Unlock()
		return c.startErr
	}
	c.cmd = cmd
	c.done = make(chan struct{})
	c.stdinWriter = stdin
	c.stdout = bufio.NewReader(stdout)
	go c.readLoop()
	c.mu.Unlock()
	// Verify the daemon answers before returning.
	if _, verr := c.call(ctx, "ping", nil, 10*time.Second); verr != nil {
		c.mu.Lock()
		c.startErr = fmt.Errorf("yt-dlp daemon started but did not answer ping: %w", verr)
		_ = c.stopLocked()
		c.mu.Unlock()
		return c.startErr
	}
	return nil
}

// call performs one JSON-RPC round-trip.
func (c *YtDlpClient) call(ctx context.Context, method string, params map[string]any, timeout time.Duration) (json.RawMessage, error) {
	if err := c.ensureStarted(ctx); err != nil {
		return nil, err
	}
	c.mu.Lock()
	id := c.nextID
	c.nextID++
	ch := make(chan rpcResponse, 1)
	c.pending[id] = ch
	req, err := json.Marshal(map[string]any{"id": id, "method": method, "params": params})
	if err != nil {
		delete(c.pending, id)
		c.mu.Unlock()
		return nil, err
	}
	if _, err := c.stdinWriter.Write(append(req, '\n')); err != nil {
		delete(c.pending, id)
		c.mu.Unlock()
		return nil, fmt.Errorf("yt-dlp daemon write failed: %w", err)
	}
	c.mu.Unlock()

	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	select {
	case <-ctx.Done():
		c.mu.Lock()
		delete(c.pending, id)
		c.mu.Unlock()
		return nil, fmt.Errorf("yt-dlp daemon %s timed out after %s", method, timeout)
	case resp := <-ch:
		if resp.Error != nil {
			return nil, fmt.Errorf("yt-dlp daemon %s failed: %s", method, resp.Error.Message)
		}
		return resp.Result, nil
	case <-c.done:
		return nil, fmt.Errorf("yt-dlp daemon exited during %s", method)
	}
}

// readLoop demultiplexes daemon responses to waiting callers.
// A line that is not a JSON response is skipped — but yt-dlp can
// still emit progress fragments separated by '\r' on the same
// physical line, so each '\r'-separated segment is tried in turn
// (the worker sets noprogress, this is defence in depth: a lost
// response used to hang the caller until its timeout).
func (c *YtDlpClient) readLoop() {
	for {
		line, err := c.stdout.ReadBytes('\n')
		if err != nil {
			c.mu.Lock()
			for id, ch := range c.pending {
				delete(c.pending, id)
				close(ch)
			}
			c.mu.Unlock()
			close(c.done)
			return
		}
		for _, segment := range strings.Split(string(line), "\r") {
			segment = strings.TrimSpace(segment)
			if segment == "" {
				continue
			}
			var resp struct {
				ID     int64           `json:"id"`
				Result json.RawMessage `json:"result"`
				Error  *struct {
					Code    string `json:"code"`
					Message string `json:"message"`
				} `json:"error"`
			}
			if err := json.Unmarshal([]byte(segment), &resp); err != nil {
				continue
			}
			c.mu.Lock()
			ch, ok := c.pending[resp.ID]
			if ok {
				delete(c.pending, resp.ID)
			}
			c.mu.Unlock()
			if ok {
				ch <- rpcResponse{Result: resp.Result, Error: resp.Error}
			}
		}
	}
}

// Ping verifies the daemon is alive (no network).
func (c *YtDlpClient) Ping(ctx context.Context) error {
	_, err := c.call(ctx, "ping", nil, 10*time.Second)
	return err
}

// Version returns the daemon's yt-dlp version string.
func (c *YtDlpClient) Version(ctx context.Context) (string, error) {
	raw, err := c.call(ctx, "version", nil, 10*time.Second)
	if err != nil {
		return "", err
	}
	var v struct {
		Version string `json:"version"`
	}
	if err := json.Unmarshal(raw, &v); err != nil {
		return "", err
	}
	return v.Version, nil
}

// Extract returns yt-dlp -J metadata for a URL (download=false).
func (c *YtDlpClient) Extract(ctx context.Context, url string) (map[string]any, error) {
	raw, err := c.call(ctx, "extract", map[string]any{"url": url}, 120*time.Second)
	if err != nil {
		return nil, err
	}
	var info map[string]any
	if err := json.Unmarshal(raw, &info); err != nil {
		return nil, fmt.Errorf("yt-dlp extract: bad info JSON: %w", err)
	}
	return info, nil
}

// Download downloads url (bestaudio, extracted to mp3) into dir
// and returns the file path.
func (c *YtDlpClient) Download(ctx context.Context, url, dir string) (string, error) {
	raw, err := c.call(ctx, "download", map[string]any{"url": url, "dir": dir}, 600*time.Second)
	if err != nil {
		return "", err
	}
	var v struct {
		FilePath string `json:"file_path"`
	}
	if err := json.Unmarshal(raw, &v); err != nil {
		return "", fmt.Errorf("yt-dlp download: bad result JSON: %w", err)
	}
	if v.FilePath == "" {
		return "", fmt.Errorf("yt-dlp download produced no file for %s", url)
	}
	return v.FilePath, nil
}

// Close stops the daemon process (best effort).
func (c *YtDlpClient) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.stopLocked()
}

func (c *YtDlpClient) stopLocked() error {
	if c.cmd == nil || c.cmd.Process == nil {
		return nil
	}
	if c.stdinWriter != nil {
		_ = c.stdinWriter.Close()
	}
	err := c.cmd.Process.Kill()
	_ = c.cmd.Wait()
	c.cmd = nil
	return err
}

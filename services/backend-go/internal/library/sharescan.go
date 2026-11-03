package library

import (
	"context"
	"fmt"
	"net/http"
	"os"
	"strings"
	"time"

	"parlay/backend-go/internal/uds"
)

// slskdBaseURL returns the slskd base URL (SLSKD_URL wins, SLSKD_HOST
// is the alias, default http://127.0.0.1:5030 — the same rule as the
// resolver's Soulseek client).
func slskdBaseURL() string {
	if v := strings.TrimSpace(os.Getenv("SLSKD_URL")); v != "" {
		return strings.TrimRight(v, "/")
	}
	if v := strings.TrimSpace(os.Getenv("SLSKD_HOST")); v != "" {
		return strings.TrimRight(v, "/")
	}
	return "http://127.0.0.1:5030"
}

// slskdPost performs one slskd POST with the API key when set. A
// transport failure is an engine_unavailable naming SLSKD_URL.
func slskdPost(ctx context.Context, path string) (int, []byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, slskdBaseURL()+"/api/v0"+path, nil)
	if err != nil {
		return 0, nil, err
	}
	req.Header.Set("Accept", "application/json")
	if key := strings.TrimSpace(os.Getenv("SLSKD_API_KEY")); key != "" {
		req.Header.Set("X-API-Key", key)
	}
	client := &http.Client{Timeout: 30 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return 0, nil, fmt.Errorf("slskd unreachable at %s (%v)", slskdBaseURL(), err)
	}
	defer resp.Body.Close()
	body := make([]byte, 0, 4096)
	buf := make([]byte, 4096)
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			body = append(body, buf[:n]...)
		}
		if rerr != nil {
			break
		}
	}
	return resp.StatusCode, body, nil
}

// handleShareScan implements library.sharescan (§9-E6): triggers (or
// schedules) an slskd share scan (POST /api/v0/options/shares/scan).
// Strings (locked): "slskd share scan triggered." / "slskd share scan
// scheduled in ~{mins}m." Edge cases: slskd down -> 502
// engine_unavailable naming SLSKD_URL; scan already running -> 200
// {triggered: false, scheduled_in_min: <n>}.
//
// The already-running signal: slskd answers 409 when a scan is
// already in progress. The API exposes no minute estimate for a
// queued scan, so scheduled_in_min is omitted in that case (the
// field is optional) rather than invented.
func (s *Service) handleShareScan(ctx context.Context, _ uds.Request) (any, error) {
	status, body, err := slskdPost(ctx, "/options/shares/scan")
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, fmt.Sprintf("slskd share scan failed: %v", err))
	}
	switch {
	case status >= 200 && status < 300:
		return map[string]any{"triggered": true}, nil
	case status == http.StatusConflict:
		// A scan is already queued/running. The response body may
		// carry a queue position; the API exposes no minute
		// estimate, so none is invented.
		_ = body
		return map[string]any{"triggered": false}, nil
	default:
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, fmt.Sprintf("slskd share scan failed: slskd HTTP %d", status))
	}
}

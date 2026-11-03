// Download handlers: download.youtube / .deezer / .tiktok /
// .navidrome / .spotify / .spotify_playlist / .soulseek, plus
// download.progress / download.cancel, download.prefs, mix.generate.
// vc.play_file is NOT owned here: it is the handlers record-only
// version per inventory §3.3 (the download package's feed-binding
// duplicate was deleted in the Wave 2 verification pass).
//
// Shared shape (§2): every download.* POST opens a progress run
// before the provider runs; the run_id rides back in the response.
// The core passes a 120 s call-site budget for these POSTs
// (uds-protocol §4 note; parlay.toml [timeouts] download_ms replaces
// it in the config rewrite). Files over MaxBotAPIBYTES are reported
// oversize:true and NOT failed. Partial files are deleted on failure.
package download

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/library"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/uds"
)

// Service holds the download handlers' dependencies.
type Service struct {
	deps     *app.Deps
	registry *Registry
	prefs    *PrefsStore

	// downloadOne performs one provider download for a resolved
	// track. It defaults to deps.Registry.Download; tests replace it
	// with a local fake so handler logic is testable without the
	// network, the daemon, or any provider. Production always uses
	// the registry (never a second download loop).
	downloadOne func(ctx context.Context, track model.Track, destDir string, onProgress resolver.ProgressFunc) (resolver.DownloadResult, error)
}

// NewService builds a Service over deps, using the process-wide run
// registry and prefs store.
func NewService(deps *app.Deps) *Service {
	s := &Service{
		deps:     deps,
		registry: SharedRegistry(),
		prefs:    SharedPrefs(),
	}
	if deps != nil && deps.Registry != nil {
		s.downloadOne = deps.Registry.Download
	} else {
		s.downloadOne = func(context.Context, model.Track, string, resolver.ProgressFunc) (resolver.DownloadResult, error) {
			return resolver.DownloadResult{}, uds.NewHandlerError(uds.ErrEngineUnavailable, "resolver registry is not configured")
		}
	}
	return s
}

// NewServiceWithRegistry builds a Service with explicit registry /
// prefs (tests).
func NewServiceWithRegistry(deps *app.Deps, registry *Registry, prefs *PrefsStore) *Service {
	s := NewService(deps)
	s.registry = registry
	s.prefs = prefs
	return s
}

// Register registers every download-owned action on the UDS server:
// download.youtube, download.deezer, download.tiktok,
// download.navidrome, download.spotify, download.spotify_playlist,
// download.soulseek, download.progress, download.cancel,
// download.prefs, mix.generate. (vc.play_file is registered by
// handlers only — see the package header.)
func Register(s *uds.Server, d *app.Deps) {
	svc := NewService(d)
	s.Register("download.youtube", svc.handleYouTube)
	s.Register("download.deezer", svc.handleDeezer)
	s.Register("download.tiktok", svc.handleTikTok)
	s.Register("download.navidrome", svc.handleNavidrome)
	s.Register("download.spotify", svc.handleSpotify)
	s.Register("download.spotify_playlist", svc.handleSpotifyPlaylist)
	s.Register("download.soulseek", svc.handleSoulseek)
	s.Register("download.progress", svc.handleProgress)
	s.Register("download.cancel", svc.handleCancel)
	s.Register("download.prefs", svc.handlePrefs)
	s.Register("mix.generate", svc.handleMix)
}

// --- shared job plumbing --------------------------------------------

// downloadRequest is the common download.* POST body.
type downloadRequest struct {
	Query       string `json:"query"`
	URL         string `json:"url"`
	RequestedBy int64  `json:"requested_by"`
	Async       bool   `json:"async"`
}

// decodeDownloadRequest decodes a download.* body.
func decodeDownloadRequest(req uds.Request) (downloadRequest, error) {
	var body downloadRequest
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return body, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad download request: %v", err))
		}
	}
	return body, nil
}

// destDir returns the download destination: <data_dir>/downloads.
func (s *Service) destDir() string {
	if s.deps != nil && s.deps.Config != nil {
		return filepath.Join(s.deps.Config.Backend.DataDir, "downloads")
	}
	return resolver.DefaultDownloadDir()
}

// mapDownloadErr maps a provider error onto a contract error code:
// missing credential / upstream unavailable -> engine_unavailable,
// everything else from a download provider -> download_failed (never
// a fake success, never a bare internal). Validation errors are
// raised as bad_request before the provider runs and never reach here.
func mapDownloadErr(err error) error {
	if err == nil {
		return nil
	}
	var herr *uds.HandlerError
	if errors.As(err, &herr) {
		return herr
	}
	if resolver.IsMissingCredential(err) {
		return uds.NewHandlerError(uds.ErrEngineUnavailable, err.Error())
	}
	var unavail *resolver.EngineUnavailable
	if errors.As(err, &unavail) {
		// Soulseek distinguishes the two in the contract (§2.7):
		// "found nothing usable" means the search ran — that is a
		// download_failed, not an unavailable engine.
		if strings.Contains(err.Error(), "found nothing usable") {
			return uds.NewHandlerError(uds.ErrDownloadFailed, err.Error())
		}
		return uds.NewHandlerError(uds.ErrEngineUnavailable, err.Error())
	}
	if isCancelErr(err) {
		return uds.NewHandlerError(uds.ErrDownloadFailed, "cancelled by user")
	}
	return uds.NewHandlerError(uds.ErrDownloadFailed, err.Error())
}

// snapshotDir lists a directory's files (missing dir = empty).
func snapshotDir(dir string) map[string]struct{} {
	out := map[string]struct{}{}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return out
	}
	for _, e := range entries {
		out[e.Name()] = struct{}{}
	}
	return out
}

// removeNewFiles deletes files created in dir since before (the
// partial-file cleanup on failure, §2 intro).
func removeNewFiles(dir string, before map[string]struct{}) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	for _, e := range entries {
		if _, ok := before[e.Name()]; !ok {
			_ = os.Remove(filepath.Join(dir, e.Name()))
		}
	}
}

// runJob runs one download job under the registry. do performs the
// provider work and returns the response data object (without
// run_id — runJob adds it). On error the run fails with the humanised
// reason, partial files are deleted, and the error is mapped onto a
// contract code. When async is set (E1) the job runs in the
// background and runJob returns {run_id, phase: "searching"}
// immediately; on async success a DownloadReady event fires.
func (s *Service) runJob(ctx context.Context, req uds.Request, kind, title string, async bool, do func(ctx context.Context, run *Run) (map[string]any, error)) (any, error) {
	dest := s.destDir()
	if async {
		// Detached from the request context (the core has already
		// had its answer); the run's own cancel still works via
		// download.cancel.
		run := s.registry.NewRun(context.Background(), kind, title)
		go s.executeJob(req, run, dest, true, do)
		return map[string]any{"run_id": run.ID, "phase": string(PhaseSearching)}, nil
	}
	run := s.registry.NewRun(ctx, kind, title)
	return s.executeJob(req, run, dest, false, do)
}

func (s *Service) executeJob(req uds.Request, run *Run, dest string, async bool, do func(ctx context.Context, run *Run) (map[string]any, error)) (any, error) {
	before := snapshotDir(dest)
	// The job runs on the run's own context: for a synchronous run
	// that derives from the request context (deadline/cancel
	// propagate); for an async run it is the detached context the
	// registry cancels. Providers check it between chunks/tracks.
	data, err := do(run.Context(), run)
	if err != nil {
		removeNewFiles(dest, before)
		run.Fail(s.registry, humanizeFailure(err))
		return nil, mapDownloadErr(err)
	}
	run.Finish(s.registry)
	if data == nil {
		data = map[string]any{}
	}
	data["run_id"] = run.ID
	// Async completion: the core learns via download.progress, and
	// via the DownloadReady event (§4.5 — async downloads only;
	// synchronous POSTs must NOT fire it, the response already
	// carries the result).
	if async {
		chatID := int64(0)
		if req.HasChatID {
			chatID = req.ChatID
		}
		if s.deps != nil && s.deps.Server != nil {
			filePath, _ := data["file_path"].(string)
			s.deps.Server.Emit("DownloadReady", chatID, map[string]any{
				"run_id": run.ID, "file_path": filePath,
				"track": data["track"], "chat_id": chatID,
			})
		}
	}
	return data, nil
}

// finishFile applies the download prefs to a finished file and
// records it in the library index: the path_template rename (single
// files only — album/playlist outputs keep their names), the
// oversize flag, and the library record (so search results and
// now-playing can carry size_bytes, §9-E10/E11). It returns the final
// path, size and oversize flag.
func (s *Service) finishFile(userID int64, res resolver.DownloadResult, rename bool) (string, int64, bool, error) {
	path := res.FilePath
	size := res.SizeBytes
	if size == 0 {
		if st, err := os.Stat(path); err == nil {
			size = st.Size()
		}
	}
	oversize := size > MaxBotAPIBYTES
	if rename && path != "" {
		prefs := s.prefs.Get(userID)
		ext := strings.TrimPrefix(filepath.Ext(path), ".")
		if ext == "" {
			ext = "mp3"
		}
		target := applyTemplate(prefs.PathTemplate, templateTrack{
			artistName: res.Track.Artist, titleName: res.Track.Title, idName: res.Track.ID,
		}, ext)
		dir := filepath.Dir(path)
		dst := filepath.Join(dir, target)
		if dst != path {
			if _, err := os.Stat(dst); err == nil {
				// Collision: disambiguate with the track id.
				base := strings.TrimSuffix(target, "."+ext)
				dst = filepath.Join(dir, resolver.SanitizeFilename(base+" ["+res.Track.ID+"]")+"."+ext)
			}
			if err := os.Rename(path, dst); err == nil {
				path = dst
			}
			// A failed rename keeps the original path — the file
			// still exists and the download still succeeded.
		}
	}
	if path != "" {
		library.SharedIndex().Record(res.Track, path, size)
	}
	return path, size, oversize, nil
}

// fileResult renders the standard single-file download response.
func fileResult(path string, track model.Track, size int64, oversize bool) map[string]any {
	track.SizeBytes = size
	return map[string]any{
		"file_path":  path,
		"oversize":   oversize,
		"size_bytes": size,
		"track":      track,
	}
}

// checkDaemonFormat enforces the download.prefs format on
// daemon-backed downloads: the yt-dlp daemon extracts mp3 only
// (scripts/yt_dlp_worker.py hardcodes preferredcodec mp3). A
// non-mp3 format is refused honestly instead of silently delivering
// an mp3 the user did not ask for. The fix is a format parameter on
// the daemon download method (resolver/yt-dlp + worker change).
func checkDaemonFormat(prefs Prefs) error {
	if prefs.Format != "" && prefs.Format != "mp3" {
		return uds.NewHandlerError(uds.ErrEngineUnavailable,
			fmt.Sprintf("download format %q is not available: the yt-dlp daemon extracts mp3 only; set format to mp3 in download.prefs", prefs.Format))
	}
	return nil
}

// resolveTrack resolves a query to a track through the registry, or
// an honest engine_unavailable when the registry is absent.
func (s *Service) resolveTrack(ctx context.Context, query string) (model.Track, error) {
	if s.deps == nil || s.deps.Registry == nil {
		return model.Track{}, uds.NewHandlerError(uds.ErrEngineUnavailable, "resolver registry is not configured")
	}
	track, err := s.deps.Registry.Resolve(ctx, query)
	if err != nil {
		return model.Track{}, mapDownloadErr(err)
	}
	return track, nil
}

// downloadTrack downloads a resolved track through the registry (or
// the test seam) with progress into the run.
func (s *Service) downloadTrack(ctx context.Context, run *Run, track model.Track, destDir string) (resolver.DownloadResult, error) {
	return s.downloadOne(ctx, track, destDir, run.ProgressFunc())
}

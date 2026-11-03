// Package download owns the download.* actions, download.progress /
// download.cancel (actions-inventory §2, §8.3, §9-E1), download.prefs
// (§9-E9) and mix.generate (§9-E3). vc.play_file (§3.3, §9-E12) is
// owned by internal/handlers, not this package.
//
// Every download.* POST opens a progress run before the provider runs
// and finishes it after (downloaders server.py do_POST); the run_id
// rides back in the response. Files over the 50 MB Bot API cap
// (MaxBotAPIBYTES) are reported oversize:true and NOT failed — the
// core queues them for a later MTProto pass. Partial files are deleted
// on failure. Runs are kept for late polls; the registry caps at 200
// finished runs, oldest first; live runs are never evicted.
//
// Persistence note: the inventory (§4.6) wants run records in a
// bbolt "downloads" bucket and prefs in a "dlprefs" bucket, but
// internal/store has neither bucket yet (it ships rooms, queue, votes,
// shares, spotify, settings, radio only). Until the store gains those
// buckets, runs and prefs live in memory in this package — a backend
// restart loses finished-run snapshots (a later poll for a pre-restart
// run_id answers not_found honestly) and resets prefs to defaults.
// The Registry and PrefsStore types are the seam: when the buckets
// land, they gain a store-backed implementation behind the same
// methods. Nothing here opens a second bbolt file.
package download

import (
	"context"
	"sync"
	"time"

	"parlay/backend-go/internal/resolver"
)

// MaxBotAPIBYTES is the 50 MB Bot API send cap (downloaders
// src/util.py). A downloaded file over this size is reported
// oversize:true and NOT failed.
const MaxBotAPIBYTES = 50 * 1024 * 1024

// MaxFinishedRuns caps retained finished runs (inventory §2.8/§8.3).
const MaxFinishedRuns = 200

// RunPhase is one download.progress phase value.
type RunPhase string

const (
	PhaseSearching   RunPhase = "searching"
	PhaseDownloading RunPhase = "downloading"
	PhaseUploading   RunPhase = "uploading"
	PhaseDone        RunPhase = "done"
	PhaseFailed      RunPhase = "failed"
)

// TrackProgress is one per-track checklist entry in a
// download.progress snapshot (pending | searching | queued |
// downloading | done | failed).
type TrackProgress struct {
	Title  string  `json:"title"`
	State  string  `json:"state"`
	Pct    float64 `json:"pct"`
	Peer   *string `json:"peer"`
	Reason *string `json:"reason"`
}

// Run is one download run: the registry record behind
// download.progress and download.cancel.
type Run struct {
	ID        string
	Kind      string // download.youtube, download.soulseek, ...
	Title     string
	StartedAt time.Time

	mu sync.Mutex
	// phase is the current phase; error holds the humanised failure
	// (stored, but NOT in the progress snapshot — the core reads the
	// failure from the POST error, inventory §2.8).
	phase          RunPhase
	errReason      string
	percentage     float64
	doneBytes      int64
	totalBytes     *int64
	speed          float64
	etaSeconds     *int64
	perTrack       []TrackProgress
	lossyAvailable bool
	lossySummary   *string
	filePath       string

	ctx    context.Context
	cancel context.CancelFunc
	done   chan struct{}
	once   sync.Once
}

// Registry holds live and finished download runs.
type Registry struct {
	mu       sync.Mutex
	live     map[string]*Run
	finished []*Run // oldest first, capped at MaxFinishedRuns
}

// NewRegistry builds an empty run registry.
func NewRegistry() *Registry {
	return &Registry{live: map[string]*Run{}}
}

// sharedRegistry is the process-wide registry used by Register. One
// backend process has one download registry, like it has one store.
var sharedRegistry = NewRegistry()

// SharedRegistry returns the process-wide run registry.
func SharedRegistry() *Registry { return sharedRegistry }

// NewRun opens a run at POST entry, before any provider work. ctx is
// the request context for synchronous runs; for async runs the caller
// passes a detached context (the run's own cancel still works).
func (r *Registry) NewRun(ctx context.Context, kind, title string) *Run {
	runCtx, cancel := context.WithCancel(ctx)
	run := &Run{
		ID:        resolver.NewRunID(),
		Kind:      kind,
		Title:     title,
		StartedAt: time.Now(),
		phase:     PhaseSearching,
		ctx:       runCtx,
		cancel:    cancel,
		done:      make(chan struct{}),
	}
	r.mu.Lock()
	r.live[run.ID] = run
	r.mu.Unlock()
	return run
}

// Context returns the run's context: cancelled by Cancel, or when the
// request deadline elapses for a synchronous run. Providers check it
// between chunks / between tracks.
func (run *Run) Context() context.Context { return run.ctx }

// Get returns a run by id (live or finished), or false.
func (r *Registry) Get(runID string) (*Run, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if run, ok := r.live[runID]; ok {
		return run, true
	}
	for _, run := range r.finished {
		if run.ID == runID {
			return run, true
		}
	}
	return nil, false
}

// Cancel cancels a live run: the provider's download loop sees the
// context cancel between chunks / between tracks, the partial file is
// deleted by the job, and the run finishes failed with reason
// "cancelled by user" (§8.3). It returns (cancelled, phase, found):
// cancelled is false for a run that already finished (200, not an
// error — the core's "Nothing to cancel — already finished." path).
func (r *Registry) Cancel(runID string) (cancelled bool, phase RunPhase, found bool) {
	r.mu.Lock()
	run, ok := r.live[runID]
	if !ok {
		for _, fin := range r.finished {
			if fin.ID == runID {
				phase := fin.Phase()
				r.mu.Unlock()
				return false, phase, true
			}
		}
		r.mu.Unlock()
		return false, "", false
	}
	r.mu.Unlock()
	run.cancel()
	return true, PhaseFailed, true
}

// Report applies a provider progress snapshot to the run. The
// provider's own run id (if any) is ignored — the registry run is
// the one the core polls.
func (run *Run) Report(p resolver.Progress) {
	run.mu.Lock()
	defer run.mu.Unlock()
	if run.phase == PhaseDone || run.phase == PhaseFailed {
		return
	}
	if p.Phase == "downloading" || p.DoneBytes > 0 {
		run.phase = PhaseDownloading
	} else if p.Phase != "" && p.Phase != "searching" {
		run.phase = RunPhase(p.Phase)
	}
	run.percentage = p.Percentage
	run.doneBytes = p.DoneBytes
	run.totalBytes = p.TotalBytes
	run.speed = p.SpeedBytesPerSec
	run.etaSeconds = p.ETASeconds
}

// ProgressFunc returns a resolver.ProgressFunc that reports provider
// progress into this run.
func (run *Run) ProgressFunc() resolver.ProgressFunc {
	return func(p resolver.Progress) { run.Report(p) }
}

// SetTitles initialises the per-track checklist (album/collection
// runs): every title starts pending.
func (run *Run) SetTitles(titles []string) {
	run.mu.Lock()
	defer run.mu.Unlock()
	run.perTrack = make([]TrackProgress, len(titles))
	for i, t := range titles {
		run.perTrack[i] = TrackProgress{Title: t, State: "pending"}
	}
}

// SetTrack updates one checklist entry by title.
func (run *Run) SetTrack(title, state string, pct float64, peer, reason *string) {
	run.mu.Lock()
	defer run.mu.Unlock()
	for i := range run.perTrack {
		if run.perTrack[i].Title == title {
			run.perTrack[i].State = state
			run.perTrack[i].Pct = pct
			run.perTrack[i].Peer = peer
			run.perTrack[i].Reason = reason
			return
		}
	}
	run.perTrack = append(run.perTrack, TrackProgress{Title: title, State: state, Pct: pct, Peer: peer, Reason: reason})
}

// SetLossy records a Soulseek lossy-only outcome (§2.7): the run
// reports lossy_available so the core can offer the lossy prompt.
func (run *Run) SetLossy(available bool, summary string) {
	run.mu.Lock()
	defer run.mu.Unlock()
	run.lossyAvailable = available
	if summary != "" {
		run.lossySummary = &summary
	} else {
		run.lossySummary = nil
	}
}

// SetFilePath records the finished file (used for the DownloadReady
// event on async runs).
func (run *Run) SetFilePath(path string) {
	run.mu.Lock()
	defer run.mu.Unlock()
	run.filePath = path
}

// FilePath returns the recorded finished file, or "".
func (run *Run) FilePath() string {
	run.mu.Lock()
	defer run.mu.Unlock()
	return run.filePath
}

// Phase returns the current phase.
func (run *Run) Phase() RunPhase {
	run.mu.Lock()
	defer run.mu.Unlock()
	return run.phase
}

// ErrReason returns the stored humanised failure reason, or "".
func (run *Run) ErrReason() string {
	run.mu.Lock()
	defer run.mu.Unlock()
	return run.errReason
}

// finish moves the run to its terminal phase exactly once and
// retires it into the finished list (cap 200, oldest first).
func (run *Run) finish(registry *Registry, phase RunPhase, reason string) {
	run.once.Do(func() {
		run.mu.Lock()
		run.phase = phase
		run.errReason = reason
		if phase == PhaseDone {
			run.percentage = 100
		}
		run.mu.Unlock()
		close(run.done)
		if registry != nil {
			registry.retire(run)
		}
	})
}

// Finish marks the run done (forces percentage 100, §8.3).
func (run *Run) Finish(registry *Registry) { run.finish(registry, PhaseDone, "") }

// Fail marks the run failed with a humanised reason.
func (run *Run) Fail(registry *Registry, reason string) {
	if reason == "" {
		reason = "download failed"
	}
	run.finish(registry, PhaseFailed, reason)
}

func (r *Registry) retire(run *Run) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.live, run.ID)
	r.finished = append(r.finished, run)
	for len(r.finished) > MaxFinishedRuns {
		r.finished = r.finished[1:]
	}
}

// Done returns a channel closed when the run reaches a terminal phase.
func (run *Run) Done() <-chan struct{} { return run.done }

// Snapshot renders the download.progress response shape for the run
// (§2.8). total_bytes / eta_sec are null until the provider knows
// them; speed is 0 until bytes move.
func (run *Run) Snapshot() map[string]any {
	run.mu.Lock()
	defer run.mu.Unlock()
	perTrack := run.perTrack
	if perTrack == nil {
		perTrack = []TrackProgress{}
	}
	return map[string]any{
		"run_id":              run.ID,
		"percentage":          run.percentage,
		"done_bytes":          run.doneBytes,
		"total_bytes":         run.totalBytes,
		"speed_bytes_per_sec": run.speed,
		"eta_sec":             run.etaSeconds,
		"elapsed_sec":         time.Since(run.StartedAt).Seconds(),
		"phase":               string(run.phase),
		"per_track":           perTrack,
		"lossy_available":     run.lossyAvailable,
		"lossy_summary":       run.lossySummary,
	}
}

// humanizeFailure renders a download error the way the core shows it
// (downloaders progress.humanize_failure): peer rejections, timeouts
// and cancels get plain words, never a raw stack.
func humanizeFailure(err error) string {
	if err == nil {
		return ""
	}
	msg := err.Error()
	if isCancelErr(err) {
		return "cancelled by user"
	}
	return msg
}

func isCancelErr(err error) bool {
	return err == context.Canceled || err == context.DeadlineExceeded
}

// errorMessage unwraps err to a short single-line message.
func errorMessage(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

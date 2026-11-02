// Package runner is the playback engine loop: it advances rooms whose
// current track has played past its known duration, and — in high-perf
// mode — prefetches the next track's stream URL before it is needed.
//
// Profiles (VC_PROFILE), ported from the two Go upstreams:
//   - low-ram (TgMusicBot tuning): a single runner goroutine, sequential
//     queue, no prefetch, minimal buffering (BufferKB 256). One worker,
//     resolve-on-demand only.
//   - high-perf (ArcMusic tuning): the same runner plus a pool of prefetch
//     workers that resolve the next track's direct stream URL while the
//     current track is still playing, larger buffer (BufferKB 4096).
//
// The runner owns playback *state* only. Joining a Telegram voice chat and
// feeding audio frames to ntgcalls is the C++ core's job via its
// VoiceCall interface; this service never opens a voice connection.
package runner

import (
	"context"
	"log"
	"sync"
	"time"

	"parlay/vc-go/internal/queue"
	"parlay/vc-go/internal/resolver"
)

// Profile selects the playback path.
type Profile string

const (
	// ProfileLowRAM is TgMusicBot-style: single worker, sequential,
	// minimal buffering.
	ProfileLowRAM Profile = "low-ram"
	// ProfileHighPerf is ArcMusic-style: prefetch workers, larger buffer.
	ProfileHighPerf Profile = "high-perf"
)

// ParseProfile normalises VC_PROFILE; anything unknown falls back to
// low-ram with a warning.
func ParseProfile(raw string) Profile {
	switch raw {
	case "high-perf", "high_perf", "highperf":
		return ProfileHighPerf
	case "", "low-ram", "low_ram", "lowram":
		return ProfileLowRAM
	default:
		log.Printf("vc-go: unknown VC_PROFILE %q, falling back to low-ram", raw)
		return ProfileLowRAM
	}
}

// Config is the runner's profile-derived tuning.
type Config struct {
	Profile       Profile
	Prefetchers   int // prefetch workers; 0 in low-ram
	BufferKB      int // configured audio buffer size
	PrefetchAhead int // tracks resolved ahead of playback
}

// ConfigFor returns the tuning for a profile.
func ConfigFor(p Profile) Config {
	if p == ProfileHighPerf {
		return Config{Profile: p, Prefetchers: 2, BufferKB: 4096, PrefetchAhead: 1}
	}
	return Config{Profile: p, Prefetchers: 0, BufferKB: 256, PrefetchAhead: 0}
}

// Runner advances rooms and (high-perf) prefetches next tracks.
type Runner struct {
	mgr      *queue.Manager
	resolver *resolver.Resolver
	cfg      Config

	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup

	// prefetchJobs carries rooms that need their next track resolved.
	prefetchJobs chan *queue.Room
}

// New creates a runner. It does not start until Run is called.
func New(mgr *queue.Manager, res *resolver.Resolver, cfg Config) *Runner {
	ctx, cancel := context.WithCancel(context.Background())
	return &Runner{
		mgr:          mgr,
		resolver:     res,
		cfg:          cfg,
		ctx:          ctx,
		cancel:       cancel,
		prefetchJobs: make(chan *queue.Room, 64),
	}
}

// Run starts the advance ticker and, in high-perf mode, the prefetch
// workers. It blocks until Stop is called.
func (r *Runner) Run() {
	// Prefetch workers (high-perf only; 0 workers in low-ram).
	for i := 0; i < r.cfg.Prefetchers; i++ {
		r.wg.Add(1)
		go r.prefetchWorker(i)
	}
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-r.ctx.Done():
			r.wg.Wait()
			return
		case <-ticker.C:
			r.tick()
		}
	}
}

// Stop shuts the runner down.
func (r *Runner) Stop() { r.cancel() }

// tick advances finished tracks and queues prefetch work.
func (r *Runner) tick() {
	for _, room := range r.mgr.Rooms() {
		if room.DueForAdvance() {
			np := room.Advance()
			if np.Track != nil {
				log.Printf("vc-go: chat %d advanced to %q", np.ChatID, np.Track.Title)
			} else {
				log.Printf("vc-go: chat %d queue drained, idle", np.ChatID)
			}
			r.mgr.Changed()
		}
		// High-perf: keep the next track resolved ahead of time.
		if r.cfg.PrefetchAhead > 0 && !room.HasPrefetch() {
			if next := room.NextWaiting(); next != nil && next.StreamURL == "" {
				select {
				case r.prefetchJobs <- room:
				default: // prefetch queue full: skip this tick, retry next
				}
			}
		}
	}
}

// prefetchWorker resolves the next waiting track's stream URL so playback
// can start without a resolve stall (ArcMusic-style prefetch).
func (r *Runner) prefetchWorker(id int) {
	defer r.wg.Done()
	for {
		select {
		case <-r.ctx.Done():
			return
		case room := <-r.prefetchJobs:
			next := room.NextWaiting()
			if next == nil || next.StreamURL != "" {
				continue
			}
			resolved, degraded, err := r.resolver.Resolve(r.ctx, next.URL)
			if err != nil || degraded {
				continue // leave unresolved; resolved on demand at play time
			}
			resolved.Prefetched = true
			// Preserve the queued identity (id/title from the queue item).
			resolved.ID = next.ID
			resolved.Title = next.Title
			room.SetPrefetched(*resolved)
			log.Printf("vc-go: prefetcher %d resolved next track for chat %d (%q)",
				id, room.ChatID(), next.Title)
		}
	}
}

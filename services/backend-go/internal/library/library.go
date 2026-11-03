// Package library owns the library index + cache accounting
// (actions-inventory §8, §9-E2/E5/E6/E7): library.delete, library.scan,
// library.sharescan, cache.clear. The index is the record of files the
// backend has cached on disk (completed downloads, and — after a scan
// — the Navidrome library count); search results and now-playing
// tracks are enriched with size_bytes from it (§9-E10/E11).
//
// Persistence note: the index lives in the bbolt "library" bucket
// since Phase 4: Index.AttachStore loads it on boot and every
// mutation persists, so a backend restart keeps the index (the
// files on disk are the durable part either way: an entry whose
// file is gone is dropped lazily on read — and from the store too).
// Nothing here opens a second bbolt file: the store is passed in,
// never opened.
package library

import (
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

// Entry is one cached file in the library index
// (model.LibraryEntry; the store owns the persisted form).
type Entry = model.LibraryEntry

// Index is the library index: an in-memory map, persisted into the
// store's library bucket when AttachStore has wired one in (nil
// store = memory-only, the pre-Phase 4 behaviour).
type Index struct {
	mu     sync.RWMutex
	byID   map[string]*Entry
	byPath map[string]*Entry
	store  *store.Store
}

// NewIndex builds an empty index.
func NewIndex() *Index {
	return &Index{byID: map[string]*Entry{}, byPath: map[string]*Entry{}}
}

// AttachStore wires the embedded store in and loads the stored
// index, dropping entries whose file is already gone (and deleting
// those stale records from the store). A nil store is a no-op.
func (idx *Index) AttachStore(st *store.Store) error {
	if st == nil {
		return nil
	}
	entries, err := st.ListLibraryEntries()
	if err != nil {
		return err
	}
	idx.mu.Lock()
	defer idx.mu.Unlock()
	idx.store = st
	for _, e := range entries {
		e := e
		if !fileExists(e.FilePath) {
			_ = st.DeleteLibraryEntry(e.ID)
			continue
		}
		idx.byID[e.ID] = &e
		idx.byPath[e.FilePath] = &e
	}
	return nil
}

// persistLocked writes e to the store (caller holds idx.mu).
func (idx *Index) persistLocked(e *Entry) {
	if idx.store != nil {
		_ = idx.store.PutLibraryEntry(*e)
	}
}

// dropStoredLocked removes id from the store (caller holds idx.mu).
func (idx *Index) dropStoredLocked(id string) {
	if idx.store != nil {
		_ = idx.store.DeleteLibraryEntry(id)
	}
}

// sharedIndex is the process-wide index used by Register and by the
// download package (which records every completed download here).
var sharedIndex = NewIndex()

// SharedIndex returns the process-wide library index.
func SharedIndex() *Index { return sharedIndex }

// Record records (or refreshes) a cached file under the track's id.
func (idx *Index) Record(track model.Track, filePath string, size int64) {
	if track.ID == "" || filePath == "" {
		return
	}
	idx.mu.Lock()
	defer idx.mu.Unlock()
	e := &Entry{
		ID: track.ID, Title: track.Title, Artist: track.Artist,
		Source: track.Source, FilePath: filePath, SizeBytes: size,
		AddedAt: time.Now().Unix(),
	}
	if old, ok := idx.byID[track.ID]; ok {
		delete(idx.byPath, old.FilePath)
	}
	idx.byID[track.ID] = e
	idx.byPath[filePath] = e
	idx.persistLocked(e)
}

// lookupKey returns the index keys to try for a track id: the id
// itself, plus the bare id when it carries a "yt:" prefix and vice
// versa (download tracks keep the downloader id as-is, resolver
// tracks carry the source prefix — §2.1).
func lookupKeys(id string) []string {
	keys := []string{id}
	if strings.HasPrefix(id, "yt:") {
		keys = append(keys, strings.TrimPrefix(id, "yt:"))
	} else {
		keys = append(keys, "yt:"+id)
	}
	return keys
}

// Get returns the entry for a track id, verifying the file still
// exists on disk first; a stale entry (file gone) is dropped lazily
// and reported as absent (§9-E10).
func (idx *Index) Get(id string) (*Entry, bool) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	for _, k := range lookupKeys(id) {
		if e, ok := idx.byID[k]; ok {
			if !fileExists(e.FilePath) {
				delete(idx.byID, k)
				delete(idx.byPath, e.FilePath)
				idx.dropStoredLocked(k)
				continue
			}
			return e, true
		}
	}
	return nil, false
}

// GetByPath returns the entry for a file path (file existence
// verified the same way).
func (idx *Index) GetByPath(path string) (*Entry, bool) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	if e, ok := idx.byPath[path]; ok {
		if !fileExists(e.FilePath) {
			delete(idx.byID, e.ID)
			delete(idx.byPath, path)
			idx.dropStoredLocked(e.ID)
			return nil, false
		}
		return e, true
	}
	return nil, false
}

// Search returns entries whose title or artist contains q
// (case-insensitive substring), in insertion order.
func (idx *Index) Search(q string) []*Entry {
	q = strings.ToLower(strings.TrimSpace(q))
	idx.mu.Lock()
	defer idx.mu.Unlock()
	var out []*Entry
	for _, e := range idx.byID {
		if !fileExists(e.FilePath) {
			delete(idx.byID, e.ID)
			delete(idx.byPath, e.FilePath)
			idx.dropStoredLocked(e.ID)
			continue
		}
		if q == "" || strings.Contains(strings.ToLower(e.Title), q) || strings.Contains(strings.ToLower(e.Artist), q) {
			out = append(out, e)
		}
	}
	return out
}

// Delete removes an entry by id (and its file on disk when the path
// is under the data dir). It returns the entry removed, or false when
// no such id is indexed.
func (idx *Index) Delete(id string) (*Entry, bool) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	for _, k := range lookupKeys(id) {
		if e, ok := idx.byID[k]; ok {
			delete(idx.byID, k)
			delete(idx.byPath, e.FilePath)
			idx.dropStoredLocked(k)
			return e, true
		}
	}
	return nil, false
}

// DeleteByPath removes the entry for a file path.
func (idx *Index) DeleteByPath(path string) (*Entry, bool) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	if e, ok := idx.byPath[path]; ok {
		delete(idx.byID, e.ID)
		delete(idx.byPath, path)
		idx.dropStoredLocked(e.ID)
		return e, true
	}
	return nil, false
}

// Clear removes every entry and returns the removed names.
func (idx *Index) Clear() []*Entry {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	out := make([]*Entry, 0, len(idx.byID))
	for _, e := range idx.byID {
		out = append(out, e)
	}
	idx.byID = map[string]*Entry{}
	idx.byPath = map[string]*Entry{}
	if idx.store != nil {
		_ = idx.store.ClearLibrary()
	}
	return out
}

// Len returns the indexed entry count (files verified lazily by the
// callers that need exactness).
func (idx *Index) Len() int {
	idx.mu.RLock()
	defer idx.mu.RUnlock()
	return len(idx.byID)
}

func fileExists(path string) bool {
	if path == "" {
		return false
	}
	st, err := os.Stat(path)
	return err == nil && !st.IsDir()
}

// Service holds the library handlers' dependencies.
type Service struct {
	deps  *app.Deps
	index *Index
}

// NewService builds a Service over deps with the process-wide index.
func NewService(deps *app.Deps) *Service {
	return &Service{deps: deps, index: SharedIndex()}
}

// NewServiceWithIndex builds a Service with an explicit index (tests).
func NewServiceWithIndex(deps *app.Deps, index *Index) *Service {
	return &Service{deps: deps, index: index}
}

// Register registers library.delete, library.scan,
// library.sharescan and cache.clear on the UDS server.
func Register(s *uds.Server, d *app.Deps) {
	svc := NewService(d)
	s.Register("library.delete", svc.handleDelete)
	s.Register("library.scan", svc.handleScan)
	s.Register("library.sharescan", svc.handleShareScan)
	s.Register("cache.clear", svc.handleCacheClear)
}

// dataDir returns the Parlay data directory for path validation.
func (s *Service) dataDir() string {
	if s.deps != nil && s.deps.Config != nil && s.deps.Config.Backend.DataDir != "" {
		return s.deps.Config.Backend.DataDir
	}
	return resolver.DataDir()
}

// underDataDir reports whether path is under the data dir (the
// sanitize_path rule: library.delete and cache.clear only touch
// files under it).
func (s *Service) underDataDir(path string) bool {
	absData, err := filepath.Abs(s.dataDir())
	if err != nil {
		return false
	}
	absPath, err := filepath.Abs(path)
	if err != nil {
		return false
	}
	return absPath != absData && strings.HasPrefix(absPath, absData+string(os.PathSeparator))
}

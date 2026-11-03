package library

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"parlay/backend-go/internal/uds"
)

// handleDelete implements library.delete (§9-E2): deletes the library
// track record AND the file on disk (when the record has one and the
// path is under the data dir — the sanitize_path rule; ".." ->
// bad_request). The core's confirm flow is unchanged: delete:<path>
// -> confirm card -> pl:del:yes -> this action -> "Deleted.".
// Edge cases (per the inventory): unknown id -> 404; top-level
// directory as path -> bad_request "Cannot delete top-level
// directories."; file already gone but record exists -> delete the
// record, {deleted: true}; record gone but file exists -> delete the
// file, {deleted: true}.
func (s *Service) handleDelete(_ context.Context, req uds.Request) (any, error) {
	var body struct {
		ID   string `json:"id"`
		Path string `json:"path"`
	}
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad library.delete request: %v", err))
		}
	}
	id := strings.TrimSpace(body.ID)
	path := strings.TrimSpace(body.Path)
	if id == "" && path == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "id or path is required")
	}
	deleted := false
	// By id: Delete removes the record whether or not its file is
	// still on disk — that is the "file already gone but record
	// exists" case ({deleted: true}, not a 404).
	if id != "" {
		if entry, ok := s.index.Delete(id); ok {
			deleted = true
			if entry.FilePath != "" && s.underDataDir(entry.FilePath) {
				_ = os.Remove(entry.FilePath)
			}
		} else if path == "" {
			// No record: the id may itself be a path.
			if strings.Contains(id, "/") || strings.Contains(id, string(os.PathSeparator)) {
				path = id
			} else {
				return nil, uds.NewHandlerError(uds.ErrNotFound, fmt.Sprintf("no library track %q", id))
			}
		}
	}
	if path != "" {
		absPath, err := filepath.Abs(path)
		if err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad path: %v", err))
		}
		absData, err := filepath.Abs(s.dataDir())
		if err != nil {
			return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("data dir unresolvable: %v", err))
		}
		if absPath == absData {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, "Cannot delete top-level directories.")
		}
		if !strings.HasPrefix(absPath, absData+string(os.PathSeparator)) {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, "path must be under the Parlay data directory")
		}
		if st, err := os.Stat(absPath); err == nil && st.IsDir() {
			// A directory delete is only allowed for a library
			// album dir (directly under the data dir's downloads
			// area); anything else is refused.
			if filepath.Dir(absPath) != filepath.Join(absData, "downloads") {
				return nil, uds.NewHandlerError(uds.ErrBadRequest, "Cannot delete top-level directories.")
			}
		}
		// Record gone but file exists -> delete the file,
		// {deleted: true}.
		if entry, ok := s.index.DeleteByPath(absPath); ok {
			deleted = true
			if entry.FilePath != "" && entry.FilePath != absPath && s.underDataDir(entry.FilePath) {
				_ = os.Remove(entry.FilePath)
			}
		}
		if _, err := os.Stat(absPath); err == nil {
			if s.underDataDir(absPath) {
				if err := os.RemoveAll(absPath); err == nil {
					deleted = true
				}
			}
		} else if !deleted {
			// Neither a record nor a file: unknown id.
			return nil, uds.NewHandlerError(uds.ErrNotFound, fmt.Sprintf("no library track %q", idOrPath(id, absPath)))
		}
	}
	return map[string]any{"deleted": deleted, "id": idOrPath(id, path)}, nil
}

func idOrPath(id, path string) string {
	if id != "" {
		return id
	}
	return path
}

// handleCacheClear implements cache.clear (§9-E7): clears per-track
// cache entries in the embedded store (the library bucket — the
// cached-file records that back the inline "Song not cached" marker).
// scope "track" + name -> delete that entry; scope "all" (or absent)
// -> delete all cache entries. Unknown name -> 200 {cleared: false,
// names: []} (the "Song not cached" string), not an error. scope
// other than track|all -> bad_request. Boundary: this is the
// embedded-store cache only — the URL cache is Phase 4's, and this
// action never touches it. A cleared entry's file is deleted with
// it (a cache entry without its file is meaningless, and a file
// without its record is an orphan).
func (s *Service) handleCacheClear(_ context.Context, req uds.Request) (any, error) {
	var body struct {
		Scope string `json:"scope"`
		Name  string `json:"name"`
	}
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad cache.clear request: %v", err))
		}
	}
	scope := strings.ToLower(strings.TrimSpace(body.Scope))
	if scope == "" {
		scope = "all"
	}
	if scope != "track" && scope != "all" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("unknown scope %q (want track or all)", body.Scope))
	}
	name := strings.TrimSpace(body.Name)
	if scope == "track" {
		if name == "" {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, "name is required for scope track")
		}
		entry := s.findByName(name)
		if entry == nil {
			// "Song not cached" — 200, not an error.
			return map[string]any{"cleared": false, "names": []string{}}, nil
		}
		s.index.Delete(entry.ID)
		if entry.FilePath != "" && s.underDataDir(entry.FilePath) {
			_ = os.Remove(entry.FilePath)
		}
		return map[string]any{"cleared": true, "names": []string{entry.Title}}, nil
	}
	entries := s.index.Clear()
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		names = append(names, e.Title)
		if e.FilePath != "" && s.underDataDir(e.FilePath) {
			_ = os.Remove(e.FilePath)
		}
	}
	return map[string]any{"cleared": len(names) > 0, "names": names}, nil
}

// findByName matches a cache entry by exact id, then by exact title
// (case-insensitive).
func (s *Service) findByName(name string) *Entry {
	if e, ok := s.index.Get(name); ok {
		return e
	}
	for _, e := range s.index.Search(name) {
		if strings.EqualFold(e.Title, name) {
			return e
		}
	}
	return nil
}

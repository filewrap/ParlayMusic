package httpapi

import (
	"bytes"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// flacMagic is the first four bytes of every FLAC file.
var flacMagic = []byte{'f', 'L', 'a', 'C'}

// flacFile is one resolved FLAC stream: the open cached file
// plus the metadata ServeContent needs.
type flacFile struct {
	f       *os.File
	size    int64
	name    string
	modTime time.Time
}

// resolveFLAC finds the currently-playing track's FLAC file for
// a chat, from the Phase 4 cache/download store: the room Manager
// names the current track, the library index (bbolt "library"
// bucket, persisted since Phase 4) names the cached file the
// download package recorded for that track id, and the file is
// verified on disk before it is served. The returned file is
// open and positioned at offset 0; the caller closes it.
//
// Every "no FLAC" case is an honest error, never a placeholder
// body: nothing playing (404), the track was never downloaded
// (404), or the cached file is not FLAC (404). There is no size
// cap in this path: ServeContent streams from disk.
func (s *Server) resolveFLAC(chatID int64) (*flacFile, error) {
	track, ok := s.rooms.Current(chatID)
	if !ok {
		return nil, &flacError{status: http.StatusNotFound, msg: "nothing playing in this chat"}
	}
	entry, ok := s.index.Get(track.ID)
	if !ok {
		return nil, &flacError{status: http.StatusNotFound, msg: "no cached file for the current track"}
	}
	if !strings.EqualFold(filepath.Ext(entry.FilePath), ".flac") {
		return nil, &flacError{status: http.StatusNotFound, msg: "current track has no cached FLAC file"}
	}
	f, err := os.Open(entry.FilePath)
	if err != nil {
		return nil, &flacError{status: http.StatusInternalServerError, msg: fmt.Sprintf("open cached FLAC: %v", err)}
	}
	head := make([]byte, 4)
	if _, err := f.ReadAt(head, 0); err != nil {
		_ = f.Close()
		return nil, &flacError{status: http.StatusInternalServerError, msg: fmt.Sprintf("read cached FLAC: %v", err)}
	}
	if !bytes.Equal(head, flacMagic) {
		_ = f.Close()
		return nil, &flacError{status: http.StatusNotFound, msg: "cached file for the current track is not FLAC"}
	}
	st, err := f.Stat()
	if err != nil {
		_ = f.Close()
		return nil, &flacError{status: http.StatusInternalServerError, msg: fmt.Sprintf("stat cached FLAC: %v", err)}
	}
	if _, err := f.Seek(0, 0); err != nil {
		_ = f.Close()
		return nil, &flacError{status: http.StatusInternalServerError, msg: fmt.Sprintf("seek cached FLAC: %v", err)}
	}
	return &flacFile{f: f, size: st.Size(), name: filepath.Base(entry.FilePath), modTime: st.ModTime()}, nil
}

// flacError is an HTTP error for the stream endpoint.
type flacError struct {
	status int
	msg    string
}

func (e *flacError) Error() string { return e.msg }

// handleStreamFLAC serves GET /api/v1/stream/:chat_id/flac: the
// currently-playing track's FLAC bytes for that chat, with full
// HTTP Range support via http.ServeContent (206 Partial Content,
// Content-Range, Accept-Ranges, 416 on an unsatisfiable range).
// The bytes are the real cached file, streamed from disk — the
// handler never loads the file into memory and never re-encodes
// it, so what the Mini App hears is bit-perfect.
func (s *Server) handleStreamFLAC(w http.ResponseWriter, r *http.Request) {
	chatID, err := parseChatIDFromPath(r)
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	f, err := s.resolveFLAC(chatID)
	if err != nil {
		if ferr, ok := err.(*flacError); ok {
			writeError(w, ferr.status, "not_found", ferr.msg)
			return
		}
		writeError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	defer f.f.Close()
	w.Header().Set("Content-Type", "audio/flac")
	// ServeContent derives Accept-Ranges, Content-Range, 206/416
	// and Content-Length from the file; the *os.File is the
	// ReadSeeker, so bytes stream from disk with no size cap and
	// no full-file read into memory.
	http.ServeContent(w, r, f.name, f.modTime, f.f)
}

// parseChatIDFromPath reads the {chat_id} path value of the
// stream route (Go 1.22 ServeMux path patterns).
func parseChatIDFromPath(r *http.Request) (int64, error) {
	raw := strings.TrimSpace(r.PathValue("chat_id"))
	if raw == "" {
		return 0, fmt.Errorf("chat_id is required")
	}
	var id int64
	if _, err := fmt.Sscanf(raw, "%d", &id); err != nil || id == 0 {
		return 0, fmt.Errorf("chat_id is required")
	}
	// Sscanf accepts "12abc" as 12; the path value must be the
	// whole integer, nothing else.
	if strings.TrimSpace(raw) != fmt.Sprintf("%d", id) {
		return 0, fmt.Errorf("chat_id is required")
	}
	return id, nil
}

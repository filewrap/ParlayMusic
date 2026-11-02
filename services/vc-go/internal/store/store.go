// Package store persists room state. Two backends share the same
// Save/Load surface:
//   - JSON file (store.go): the fallback, used ONLY when MONGO_URI is
//     unset. Atomic writes (temp file + rename).
//   - MongoDB (mongo.go): the real backend when MONGO_URI is set, via
//     the official driver (go.mongodb.org/mongo-driver/v2).
package store

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"

	"parlay/vc-go/internal/queue"
)

// Store is a JSON-file queue store.
type Store struct {
	path string
}

// New returns a store rooted at dir/queues.json, creating dir if needed.
func New(dir string) (*Store, error) {
	if dir == "" {
		dir = "./data"
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	return &Store{path: filepath.Join(dir, "queues.json")}, nil
}

// Path returns the backing file path.
func (s *Store) Path() string { return s.path }

// Describe returns the backing location for logs.
func (s *Store) Describe() string { return s.path }

// Save writes rooms atomically: temp file in the same directory, fsync,
// rename over the target.
func (s *Store) Save(rooms []queue.PersistedRoom) error {
	data, err := json.MarshalIndent(rooms, "", "  ")
	if err != nil {
		return err
	}
	tmp := s.path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	if err := os.Chmod(tmp, 0o644); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, s.path); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// Load reads persisted rooms. Missing file = empty state, not an error.
func (s *Store) Load() ([]queue.PersistedRoom, error) {
	data, err := os.ReadFile(s.path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, nil
		}
		return nil, err
	}
	var rooms []queue.PersistedRoom
	if err := json.Unmarshal(data, &rooms); err != nil {
		return nil, err
	}
	return rooms, nil
}

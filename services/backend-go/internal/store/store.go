// Package store is the single embedded state store of the unified
// backend (FINAL-ARCHITECTURE.md §1.3 Part 2, OD-5: bbolt). One file,
// <PARLAY_DATA_DIR>/parlay.db (0600 in a 0700 directory), replaces
// Mongo, Redis and the per-service JSON directories. FRESH START
// (OD-2): there is no migration code of any kind — the store starts
// empty and every record in it was written by this backend.
//
// Buckets: rooms, queue (per room), votes, shares, spotify (oauth,
// watch, dislikes, skipcheck), settings, radio. Values are JSON
// encodings of the model structs, so the on-disk form stays readable
// and the store never needs a schema version.
package store

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"go.etcd.io/bbolt"

	"parlay/backend-go/internal/model"
)

var (
	bucketRooms    = []byte("rooms")
	bucketQueue    = []byte("queue")
	bucketVotes    = []byte("votes")
	bucketShares   = []byte("shares")
	bucketSpotify  = []byte("spotify")
	bucketSettings = []byte("settings")
	bucketRadio    = []byte("radio")
)

var topBuckets = [][]byte{
	bucketRooms, bucketQueue, bucketVotes, bucketShares,
	bucketSpotify, bucketSettings, bucketRadio,
}

// Store is a bbolt-backed key-value store for every model struct.
type Store struct {
	db *bbolt.DB
}

// Open creates the data directory (0700) and opens <dir>/parlay.db
// (0600). The file is created with 0600 permissions; an existing
// file keeps its permissions.
func Open(dir string) (*Store, error) {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, fmt.Errorf("store: create data dir %s: %w", dir, err)
	}
	path := filepath.Join(dir, "parlay.db")
	db, err := bbolt.Open(path, 0o600, nil)
	if err != nil {
		return nil, fmt.Errorf("store: open %s: %w", path, err)
	}
	err = db.Update(func(tx *bbolt.Tx) error {
		for _, name := range topBuckets {
			if _, err := tx.CreateBucketIfNotExists(name); err != nil {
				return fmt.Errorf("store: create bucket %s: %w", name, err)
			}
		}
		return nil
	})
	if err != nil {
		db.Close()
		return nil, err
	}
	return &Store{db: db}, nil
}

// Close closes the database. All writes before Close are durable;
// bbolt is ACID, so a kill -9 between transactions loses nothing
// committed.
func (s *Store) Close() error { return s.db.Close() }

// Path returns the database file path.
func (s *Store) Path() string { return s.db.Path() }

func chatKey(chatID int64) []byte  { return []byte(strconv.FormatInt(chatID, 10)) }
func userKey(userID int64) []byte  { return []byte(strconv.FormatInt(userID, 10)) }
func tokenKey(token string) []byte { return []byte(token) }
func putJSON(b *bbolt.Bucket, key []byte, v any) error {
	raw, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return b.Put(key, raw)
}
func getJSON(b *bbolt.Bucket, key []byte, dst any) (bool, error) {
	raw := b.Get(key)
	if raw == nil {
		return false, nil
	}
	if err := json.Unmarshal(raw, dst); err != nil {
		return false, fmt.Errorf("store: corrupt record %q: %w", key, err)
	}
	return true, nil
}

// --- rooms: one RoomState per chat -----------------------------------

// PutRoom stores (or replaces) the room state for a chat.
func (s *Store) PutRoom(st model.RoomState) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		return putJSON(tx.Bucket(bucketRooms), chatKey(st.ChatID), st)
	})
}

// GetRoom returns the room state for a chat, or false when the chat
// has no stored room.
func (s *Store) GetRoom(chatID int64) (model.RoomState, bool, error) {
	var st model.RoomState
	found := false
	err := s.db.View(func(tx *bbolt.Tx) error {
		var err error
		found, err = getJSON(tx.Bucket(bucketRooms), chatKey(chatID), &st)
		return err
	})
	return st, found, err
}

// DeleteRoom removes the room state and its queue for a chat.
func (s *Store) DeleteRoom(chatID int64) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		if err := tx.Bucket(bucketRooms).Delete(chatKey(chatID)); err != nil {
			return err
		}
		if err := tx.Bucket(bucketQueue).DeleteBucket(chatKey(chatID)); err != nil && !errors.Is(err, bbolt.ErrBucketNotFound) {
			return err
		}
		return nil
	})
}

// ListRoomIDs returns every chat id with a stored room.
func (s *Store) ListRoomIDs() ([]int64, error) {
	var ids []int64
	err := s.db.View(func(tx *bbolt.Tx) error {
		return tx.Bucket(bucketRooms).ForEach(func(k, _ []byte) error {
			id, err := strconv.ParseInt(string(k), 10, 64)
			if err != nil {
				return fmt.Errorf("store: bad room key %q: %w", k, err)
			}
			ids = append(ids, id)
			return nil
		})
	})
	return ids, err
}

// --- queue: per-room ordered queue ------------------------------------
//
// The "queue" bucket holds one sub-bucket per chat, keyed by chat id;
// inside it, each item is keyed by its 0-based position as an 8-byte
// big-endian integer, so cursor order is play order.

func queueBucket(tx *bbolt.Tx, chatID int64) (*bbolt.Bucket, error) {
	return tx.Bucket(bucketQueue).CreateBucketIfNotExists(chatKey(chatID))
}

func posKey(pos int) []byte {
	var b [8]byte
	binary.BigEndian.PutUint64(b[:], uint64(pos))
	return b[:]
}

// PutQueue stores the whole waiting queue for a chat (replacing
// whatever was there). Positions are renumbered 0..n-1 in slice
// order.
func (s *Store) PutQueue(chatID int64, items []model.QueueItem) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		qb, err := queueBucket(tx, chatID)
		if err != nil {
			return err
		}
		// Drop the old items first so a shorter queue leaves no
		// stale tail.
		if err := tx.Bucket(bucketQueue).DeleteBucket(chatKey(chatID)); err != nil && !errors.Is(err, bbolt.ErrBucketNotFound) {
			return err
		}
		qb, err = queueBucket(tx, chatID)
		if err != nil {
			return err
		}
		for i, item := range items {
			item.Position = i
			if err := putJSON(qb, posKey(i), item); err != nil {
				return err
			}
		}
		return nil
	})
}

// GetQueue returns the waiting queue for a chat in play order.
func (s *Store) GetQueue(chatID int64) ([]model.QueueItem, error) {
	var items []model.QueueItem
	err := s.db.View(func(tx *bbolt.Tx) error {
		qb := tx.Bucket(bucketQueue).Bucket(chatKey(chatID))
		if qb == nil {
			return nil
		}
		return qb.ForEach(func(_, v []byte) error {
			var item model.QueueItem
			if err := json.Unmarshal(v, &item); err != nil {
				return fmt.Errorf("store: corrupt queue item: %w", err)
			}
			items = append(items, item)
			return nil
		})
	})
	return items, err
}

// --- votes: one VoteState per (chat, track) ---------------------------

func voteKey(chatID int64, trackID string) []byte {
	return []byte(strconv.FormatInt(chatID, 10) + ":" + trackID)
}

// PutVote stores a vote round.
func (s *Store) PutVote(v model.VoteState) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		return putJSON(tx.Bucket(bucketVotes), voteKey(v.ChatID, v.Track.ID), v)
	})
}

// GetVote returns the vote round for a track in a chat.
func (s *Store) GetVote(chatID int64, trackID string) (model.VoteState, bool, error) {
	var v model.VoteState
	found := false
	err := s.db.View(func(tx *bbolt.Tx) error {
		var err error
		found, err = getJSON(tx.Bucket(bucketVotes), voteKey(chatID, trackID), &v)
		return err
	})
	return v, found, err
}

// DeleteVote removes a vote round.
func (s *Store) DeleteVote(chatID int64, trackID string) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		return tx.Bucket(bucketVotes).Delete(voteKey(chatID, trackID))
	})
}

// --- shares: one ShareLink per token ----------------------------------

// PutShare stores a share link by its token.
func (s *Store) PutShare(l model.ShareLink) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		return putJSON(tx.Bucket(bucketShares), tokenKey(l.Token), l)
	})
}

// GetShare resolves a token to its share link.
func (s *Store) GetShare(token string) (model.ShareLink, bool, error) {
	var l model.ShareLink
	found := false
	err := s.db.View(func(tx *bbolt.Tx) error {
		var err error
		found, err = getJSON(tx.Bucket(bucketShares), tokenKey(token), &l)
		return err
	})
	return l, found, err
}

// DeleteShare removes a share link.
func (s *Store) DeleteShare(token string) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		return tx.Bucket(bucketShares).Delete(tokenKey(token))
	})
}

// --- spotify: oauth, watches, dislikes, skip-check --------------------
//
// Keys inside the "spotify" bucket:
//   oauth:<userID>      SpotifyState (ciphertext token record)
//   watch:<userID>      "1" while the user is watched
//   dislike:<userID>:<trackID>  "1" while the track is disliked
//   skipcheck:<trackID> SkipCheckRecord

// PutSpotify stores a user's OAuth record. The TokenCiphertext field
// must already hold AES-256-GCM ciphertext; the store never sees a
// plaintext token.
func (s *Store) PutSpotify(st model.SpotifyState) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		return putJSON(tx.Bucket(bucketSpotify), []byte("oauth:"+strconv.FormatInt(st.UserID, 10)), st)
	})
}

// GetSpotify returns a user's OAuth record.
func (s *Store) GetSpotify(userID int64) (model.SpotifyState, bool, error) {
	var st model.SpotifyState
	found := false
	err := s.db.View(func(tx *bbolt.Tx) error {
		var err error
		found, err = getJSON(tx.Bucket(bucketSpotify), []byte("oauth:"+strconv.FormatInt(userID, 10)), &st)
		return err
	})
	return st, found, err
}

// DeleteSpotify drops a user's OAuth record (revoke / logout).
func (s *Store) DeleteSpotify(userID int64) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		return tx.Bucket(bucketSpotify).Delete([]byte("oauth:" + strconv.FormatInt(userID, 10)))
	})
}

// SetWatch starts or stops watching a user.
func (s *Store) SetWatch(userID int64, watched bool) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		b := tx.Bucket(bucketSpotify)
		key := []byte("watch:" + strconv.FormatInt(userID, 10))
		if watched {
			return b.Put(key, []byte("1"))
		}
		return b.Delete(key)
	})
}

// IsWatching reports whether a user is watched.
func (s *Store) IsWatching(userID int64) (bool, error) {
	watched := false
	err := s.db.View(func(tx *bbolt.Tx) error {
		watched = tx.Bucket(bucketSpotify).Get([]byte("watch:"+strconv.FormatInt(userID, 10))) != nil
		return nil
	})
	return watched, err
}

// ListWatches returns every watched user id.
func (s *Store) ListWatches() ([]int64, error) {
	var ids []int64
	err := s.db.View(func(tx *bbolt.Tx) error {
		c := tx.Bucket(bucketSpotify).Cursor()
		for k, _ := c.Seek([]byte("watch:")); k != nil && strings.HasPrefix(string(k), "watch:"); k, _ = c.Next() {
			id, err := strconv.ParseInt(strings.TrimPrefix(string(k), "watch:"), 10, 64)
			if err != nil {
				return fmt.Errorf("store: bad watch key %q: %w", k, err)
			}
			ids = append(ids, id)
		}
		return nil
	})
	return ids, err
}

// SetDislike adds or removes a user's dislike for a track.
func (s *Store) SetDislike(userID int64, trackID string, disliked bool) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		b := tx.Bucket(bucketSpotify)
		key := []byte("dislike:" + strconv.FormatInt(userID, 10) + ":" + trackID)
		if disliked {
			return b.Put(key, []byte("1"))
		}
		return b.Delete(key)
	})
}

// IsDisliked reports whether a user dislikes a track.
func (s *Store) IsDisliked(userID int64, trackID string) (bool, error) {
	disliked := false
	err := s.db.View(func(tx *bbolt.Tx) error {
		disliked = tx.Bucket(bucketSpotify).Get([]byte("dislike:"+strconv.FormatInt(userID, 10)+":"+trackID)) != nil
		return nil
	})
	return disliked, err
}

// PutSkipCheck stores the last skip-check verdict for a track.
func (s *Store) PutSkipCheck(r model.SkipCheckRecord) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		return putJSON(tx.Bucket(bucketSpotify), []byte("skipcheck:"+r.TrackID), r)
	})
}

// GetSkipCheck returns the last skip-check verdict for a track.
func (s *Store) GetSkipCheck(trackID string) (model.SkipCheckRecord, bool, error) {
	var r model.SkipCheckRecord
	found := false
	err := s.db.View(func(tx *bbolt.Tx) error {
		var err error
		found, err = getJSON(tx.Bucket(bucketSpotify), []byte("skipcheck:"+trackID), &r)
		return err
	})
	return r, found, err
}

// --- settings: one ChatSettings per chat ------------------------------

// PutSettings stores the 15-key settings object for a chat.
func (s *Store) PutSettings(chatID int64, st model.ChatSettings) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		return putJSON(tx.Bucket(bucketSettings), chatKey(chatID), st)
	})
}

// GetSettings returns the stored settings for a chat, or the
// defaults when the chat has none stored yet.
func (s *Store) GetSettings(chatID int64) (model.ChatSettings, bool, error) {
	st := model.DefaultChatSettings()
	found := false
	err := s.db.View(func(tx *bbolt.Tx) error {
		var err error
		found, err = getJSON(tx.Bucket(bucketSettings), chatKey(chatID), &st)
		return err
	})
	if !found {
		st = model.DefaultChatSettings()
	}
	return st, found, err
}

// --- radio: one RadioState per chat -----------------------------------

// PutRadio stores the radio state for a chat.
func (s *Store) PutRadio(st model.RadioState) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		return putJSON(tx.Bucket(bucketRadio), chatKey(st.ChatID), st)
	})
}

// GetRadio returns the radio state for a chat.
func (s *Store) GetRadio(chatID int64) (model.RadioState, bool, error) {
	var st model.RadioState
	found := false
	err := s.db.View(func(tx *bbolt.Tx) error {
		var err error
		found, err = getJSON(tx.Bucket(bucketRadio), chatKey(chatID), &st)
		return err
	})
	return st, found, err
}

// DeleteRadio removes the radio state for a chat.
func (s *Store) DeleteRadio(chatID int64) error {
	return s.db.Update(func(tx *bbolt.Tx) error {
		return tx.Bucket(bucketRadio).Delete(chatKey(chatID))
	})
}

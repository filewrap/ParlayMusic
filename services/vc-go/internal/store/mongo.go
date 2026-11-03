// Package store — MongoDB backend.
//
// MongoStore is the real persistence backend used when MONGO_URI is set,
// built on the official driver (go.mongodb.org/mongo-driver/v2, already
// in the module cache — no network fetch needed).
//
// Canonical layout (A11, shared with every other stateful service —
// same scheme as core-cpp `Store`): database from the URI, collection
// `state`, one document per room, {_id: "kv:vc:room:<chat_id>",
// v: "<canonical room JSON>"} (see canonical.go). vc-ts and vc-rust
// read and write the same documents, so a queue written by vc-go is
// servable by another engine after fail-over. The pre-A11 layout
// (collection `vc_rooms`, _id = chat id, {room: ...}) is superseded:
// documents in the old collection are orphaned, not migrated — the
// deployment is pre-live and no real queues exist there yet.
//
// The JSON file store (store.go) stays as the fallback and is used
// ONLY when neither MONGO_URI nor REDIS_URL is set.
package store

import (
	"context"
	"fmt"
	"net/url"
	"strings"
	"time"

	"go.mongodb.org/mongo-driver/v2/bson"
	"go.mongodb.org/mongo-driver/v2/mongo"
	"go.mongodb.org/mongo-driver/v2/mongo/options"

	"parlay/vc-go/internal/queue"
)

// mongoValueDoc is the stored document: _id is the canonical key.
type mongoValueDoc struct {
	ID    string `bson:"_id"`
	Value string `bson:"v"`
}

// roomKey returns the canonical shared-store key for a chat's room.
func roomKey(chatID int64) string {
	return fmt.Sprintf("vc:room:%d", chatID)
}

func mongoKey(chatID int64) string {
	return "kv:" + roomKey(chatID)
}

// MongoStore persists rooms to MongoDB.
type MongoStore struct {
	client *mongo.Client
	coll   *mongo.Collection
	uri    string
	db     string
}

// NewMongo connects to uri, pings it, and returns a store for
// database (collection is always the canonical `state`). Connection
// failure is an error — when MONGO_URI is set the service must not
// silently fall back to the JSON file.
func NewMongo(ctx context.Context, uri, database string) (*MongoStore, error) {
	if uri == "" {
		return nil, fmt.Errorf("MONGO_URI is empty")
	}
	if database == "" {
		database = "parlay"
	}
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	client, err := mongo.Connect(options.Client().ApplyURI(uri))
	if err != nil {
		return nil, fmt.Errorf("mongo connect: %w", err)
	}
	if err := client.Ping(ctx, nil); err != nil {
		_ = client.Disconnect(ctx)
		return nil, fmt.Errorf("mongo ping: %w", err)
	}
	return &MongoStore{
		client: client,
		coll:   client.Database(database).Collection("state"),
		uri:    uri,
		db:     database,
	}, nil
}

// Describe returns a log-safe location string (credentials stripped).
func (s *MongoStore) Describe() string {
	u, err := url.Parse(s.uri)
	if err != nil {
		return "mongodb://<unparseable>/" + s.db + ".state"
	}
	u.User = nil
	return strings.TrimSuffix(u.String(), "/") + "/" + s.db + ".state"
}

// Save upserts every room (canonical JSON) and deletes documents for
// chats that no longer have persisted state. Only keys under the
// kv:vc:room: prefix are ever deleted — the `state` collection is
// shared with the core's keys.
func (s *MongoStore) Save(rooms []queue.PersistedRoom) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	ids := make([]string, 0, len(rooms))
	for _, p := range rooms {
		key := mongoKey(p.ChatID)
		ids = append(ids, key)
		_, err := s.coll.ReplaceOne(ctx,
			bson.M{"_id": key},
			mongoValueDoc{ID: key, Value: CanonicalRoomJSON(p)},
			options.Replace().SetUpsert(true))
		if err != nil {
			return fmt.Errorf("mongo save chat %d: %w", p.ChatID, err)
		}
	}
	// Drop stale room keys (rooms that went idle and are no longer saved).
	cur, err := s.coll.Find(ctx, bson.M{"_id": bson.M{"$regex": "^kv:vc:room:"}})
	if err != nil {
		return fmt.Errorf("mongo prune list: %w", err)
	}
	defer cur.Close(ctx)
	for cur.Next(ctx) {
		var doc mongoValueDoc
		if err := cur.Decode(&doc); err != nil {
			return fmt.Errorf("mongo prune decode: %w", err)
		}
		stale := true
		for _, id := range ids {
			if doc.ID == id {
				stale = false
				break
			}
		}
		if stale {
			if _, err := s.coll.DeleteOne(ctx, bson.M{"_id": doc.ID}); err != nil {
				return fmt.Errorf("mongo prune %s: %w", doc.ID, err)
			}
		}
	}
	if err := cur.Err(); err != nil {
		return fmt.Errorf("mongo prune: %w", err)
	}
	return nil
}

// Load returns all persisted rooms. Empty collection = empty state.
func (s *MongoStore) Load() ([]queue.PersistedRoom, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	cur, err := s.coll.Find(ctx, bson.M{"_id": bson.M{"$regex": "^kv:vc:room:"}})
	if err != nil {
		return nil, fmt.Errorf("mongo load: %w", err)
	}
	defer cur.Close(ctx)
	var rooms []queue.PersistedRoom
	for cur.Next(ctx) {
		var doc mongoValueDoc
		if err := cur.Decode(&doc); err != nil {
			return nil, fmt.Errorf("mongo decode: %w", err)
		}
		p, err := ParseCanonicalRoom(doc.Value)
		if err != nil {
			return nil, fmt.Errorf("mongo decode %s: %w", doc.ID, err)
		}
		rooms = append(rooms, p)
	}
	if err := cur.Err(); err != nil {
		return nil, fmt.Errorf("mongo load: %w", err)
	}
	return rooms, nil
}

// Close disconnects the client.
func (s *MongoStore) Close(ctx context.Context) error {
	return s.client.Disconnect(ctx)
}

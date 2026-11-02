// Package store — MongoDB backend.
//
// MongoStore is the real persistence backend used when MONGO_URI is set,
// built on the official driver (go.mongodb.org/mongo-driver/v2, already
// in the module cache — no network fetch needed). One document per chat
// in <db>.vc_rooms, _id = chat id, holding the queue.PersistedRoom.
//
// This is a Parlay extension: no upstream VC bot caches queues in Mongo
// (YukkiMusic/ArcMusic persist only bot/chat settings there; playback
// state is in-memory). The JSON file store (store.go) stays as the
// fallback and is used ONLY when MONGO_URI is unset.
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

// mongoRoomDoc is the stored document: _id is the chat id.
type mongoRoomDoc struct {
	ID   int64               `bson:"_id"`
	Room queue.PersistedRoom `bson:"room"`
}

// MongoStore persists rooms to MongoDB.
type MongoStore struct {
	client   *mongo.Client
	coll     *mongo.Collection
	uri      string
	db       string
	collName string
}

// NewMongo connects to uri, pings it, and returns a store for
// database/collection. Connection failure is an error — when MONGO_URI
// is set the service must not silently fall back to the JSON file.
func NewMongo(ctx context.Context, uri, database, collection string) (*MongoStore, error) {
	if uri == "" {
		return nil, fmt.Errorf("MONGO_URI is empty")
	}
	if database == "" {
		database = "parlay"
	}
	if collection == "" {
		collection = "vc_rooms"
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
		client:   client,
		coll:     client.Database(database).Collection(collection),
		uri:      uri,
		db:       database,
		collName: collection,
	}, nil
}

// Describe returns a log-safe location string (credentials stripped).
func (s *MongoStore) Describe() string {
	u, err := url.Parse(s.uri)
	if err != nil {
		return "mongodb://<unparseable>/" + s.db + "." + s.collName
	}
	u.User = nil
	return strings.TrimSuffix(u.String(), "/") + "/" + s.db + "." + s.collName
}

// Save upserts every room and deletes documents for chats that no
// longer have persisted state.
func (s *MongoStore) Save(rooms []queue.PersistedRoom) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	ids := make([]int64, 0, len(rooms))
	for _, p := range rooms {
		ids = append(ids, p.ChatID)
		_, err := s.coll.ReplaceOne(ctx,
			bson.M{"_id": p.ChatID},
			mongoRoomDoc{ID: p.ChatID, Room: p},
			options.Replace().SetUpsert(true))
		if err != nil {
			return fmt.Errorf("mongo save chat %d: %w", p.ChatID, err)
		}
	}
	// Drop stale chats (rooms that went idle and are no longer saved).
	if _, err := s.coll.DeleteMany(ctx, bson.M{"_id": bson.M{"$nin": ids}}); err != nil {
		return fmt.Errorf("mongo prune: %w", err)
	}
	return nil
}

// Load returns all persisted rooms. Empty collection = empty state.
func (s *MongoStore) Load() ([]queue.PersistedRoom, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	cur, err := s.coll.Find(ctx, bson.M{})
	if err != nil {
		return nil, fmt.Errorf("mongo load: %w", err)
	}
	defer cur.Close(ctx)
	var rooms []queue.PersistedRoom
	for cur.Next(ctx) {
		var doc mongoRoomDoc
		if err := cur.Decode(&doc); err != nil {
			return nil, fmt.Errorf("mongo decode: %w", err)
		}
		rooms = append(rooms, doc.Room)
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

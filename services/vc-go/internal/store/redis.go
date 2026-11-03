// Package store — Redis backend (A11).
//
// Minimal std-only RESP client (GET/SET/DEL/KEYS/PING + AUTH/SELECT),
// ported from core-cpp src/store.cpp (RedisClient). No new module
// dependency: the Go module cache has no Redis client, and the build
// must stay offline-capable.
//
// Key scheme (canonical, all services — matches core-cpp exactly):
// queue rooms live under "parlay:kv:vc:room:<chat_id>", values are the
// canonical room JSON (see canonical.go).
package store

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"net"
	"net/url"
	"strconv"
	"strings"
	"time"

	"parlay/vc-go/internal/queue"
)

// RedisStore persists rooms to Redis.
type RedisStore struct {
	conn    net.Conn
	r       *bufio.Reader
	url     string
	db      int
	timeout time.Duration
}

// NewRedis connects to uri, pings it, and returns a store.
// Connection failure is an error — when REDIS_URL is set the service
// must not silently fall back to the JSON file.
func NewRedis(ctx context.Context, uri string) (*RedisStore, error) {
	if uri == "" {
		return nil, fmt.Errorf("REDIS_URL is empty")
	}
	u, err := url.Parse(uri)
	if err != nil {
		return nil, fmt.Errorf("redis URL parse: %w", err)
	}
	if u.Scheme != "redis" {
		return nil, fmt.Errorf("redis URL must start with redis://")
	}
	host := u.Hostname()
	if host == "" {
		host = "127.0.0.1"
	}
	port := u.Port()
	if port == "" {
		port = "6379"
	}
	db := 0
	if p := strings.TrimPrefix(u.Path, "/"); p != "" {
		if n, err := strconv.Atoi(p); err == nil {
			db = n
		}
	}
	password := ""
	if u.User != nil {
		password, _ = u.User.Password()
		if password == "" {
			// redis://:pass@host — userinfo is just the password.
			password = u.User.Username()
		}
	}
	dialer := &net.Dialer{Timeout: 5 * time.Second}
	conn, err := dialer.DialContext(ctx, "tcp", net.JoinHostPort(host, port))
	if err != nil {
		return nil, fmt.Errorf("redis connect %s:%s: %w", host, port, err)
	}
	s := &RedisStore{conn: conn, r: bufio.NewReader(conn), url: uri, db: db, timeout: 5 * time.Second}
	if err := conn.SetDeadline(time.Now().Add(5 * time.Second)); err != nil {
		_ = conn.Close()
		return nil, err
	}
	if password != "" {
		if _, err := s.command("AUTH", password); err != nil {
			_ = conn.Close()
			return nil, fmt.Errorf("redis AUTH failed: %w", err)
		}
	}
	if db != 0 {
		if _, err := s.command("SELECT", strconv.Itoa(db)); err != nil {
			_ = conn.Close()
			return nil, fmt.Errorf("redis SELECT failed: %w", err)
		}
	}
	if _, err := s.command("PING"); err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("redis PING failed: %w", err)
	}
	return s, nil
}

// Describe returns a log-safe location string (credentials stripped).
func (s *RedisStore) Describe() string {
	u, err := url.Parse(s.url)
	if err != nil {
		return "redis://<unparseable>"
	}
	u.User = nil
	return strings.TrimSuffix(u.String(), "/")
}

// Close disconnects the client.
func (s *RedisStore) Close() error {
	return s.conn.Close()
}

type redisReply struct {
	kind  byte // '+', '-', ':', '$', '*'
	str   string
	num   int64
	isNil bool
	arr   []redisReply
}

func (s *RedisStore) command(args ...string) (redisReply, error) {
	var b strings.Builder
	fmt.Fprintf(&b, "*%d\r\n", len(args))
	for _, a := range args {
		fmt.Fprintf(&b, "$%d\r\n%s\r\n", len(a), a)
	}
	_ = s.conn.SetDeadline(time.Now().Add(s.timeout))
	if _, err := s.conn.Write([]byte(b.String())); err != nil {
		return redisReply{}, fmt.Errorf("redis write failed: %w", err)
	}
	return s.readReply()
}

func (s *RedisStore) readReply() (redisReply, error) {
	line, err := s.r.ReadString('\n')
	if err != nil {
		return redisReply{}, fmt.Errorf("redis read failed: %w", err)
	}
	line = strings.TrimSuffix(strings.TrimSuffix(line, "\n"), "\r")
	if line == "" {
		return redisReply{}, fmt.Errorf("redis empty reply")
	}
	kind, payload := line[0], line[1:]
	switch kind {
	case '+':
		return redisReply{kind: '+', str: payload}, nil
	case '-':
		return redisReply{kind: '-', str: payload}, fmt.Errorf("redis error: %s", payload)
	case ':':
		n, _ := strconv.ParseInt(payload, 10, 64)
		return redisReply{kind: ':', num: n}, nil
	case '$':
		n, err := strconv.Atoi(payload)
		if err != nil {
			return redisReply{}, fmt.Errorf("bad bulk length")
		}
		if n < 0 {
			return redisReply{kind: '$', isNil: true}, nil
		}
		buf := make([]byte, n+2)
		if _, err := io.ReadFull(s.r, buf); err != nil {
			return redisReply{}, fmt.Errorf("redis bulk read failed: %w", err)
		}
		return redisReply{kind: '$', str: string(buf[:n])}, nil
	case '*':
		n, err := strconv.Atoi(payload)
		if err != nil {
			return redisReply{}, fmt.Errorf("bad array length")
		}
		if n < 0 {
			return redisReply{kind: '*', isNil: true}, nil
		}
		arr := make([]redisReply, 0, n)
		for i := 0; i < n; i++ {
			el, err := s.readReply()
			if err != nil {
				return redisReply{}, err
			}
			arr = append(arr, el)
		}
		return redisReply{kind: '*', arr: arr}, nil
	default:
		return redisReply{}, fmt.Errorf("unsupported RESP type: %c", kind)
	}
}

// Save upserts every room and deletes keys for chats that no longer
// have persisted state. Only keys under the vc:room: prefix are ever
// deleted — the Redis database is shared with the core's keys.
func (s *RedisStore) Save(rooms []queue.PersistedRoom) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_ = ctx
	live := make(map[string]bool, len(rooms))
	for _, p := range rooms {
		key := "parlay:kv:vc:room:" + strconv.FormatInt(p.ChatID, 10)
		live[key] = true
		if _, err := s.command("SET", key, CanonicalRoomJSON(p)); err != nil {
			return fmt.Errorf("redis save chat %d: %w", p.ChatID, err)
		}
	}
	reply, err := s.command("KEYS", "parlay:kv:vc:room:*")
	if err != nil {
		return fmt.Errorf("redis keys: %w", err)
	}
	for _, el := range reply.arr {
		if el.isNil {
			continue
		}
		if !live[el.str] {
			if _, err := s.command("DEL", el.str); err != nil {
				return fmt.Errorf("redis prune %s: %w", el.str, err)
			}
		}
	}
	return nil
}

// Load returns all persisted rooms. Empty store = empty state.
func (s *RedisStore) Load() ([]queue.PersistedRoom, error) {
	reply, err := s.command("KEYS", "parlay:kv:vc:room:*")
	if err != nil {
		return nil, fmt.Errorf("redis load keys: %w", err)
	}
	var rooms []queue.PersistedRoom
	for _, el := range reply.arr {
		if el.isNil {
			continue
		}
		get, err := s.command("GET", el.str)
		if err != nil {
			return nil, fmt.Errorf("redis load %s: %w", el.str, err)
		}
		if get.isNil {
			continue
		}
		p, err := ParseCanonicalRoom(get.str)
		if err != nil {
			return nil, fmt.Errorf("redis decode %s: %w", el.str, err)
		}
		rooms = append(rooms, p)
	}
	return rooms, nil
}

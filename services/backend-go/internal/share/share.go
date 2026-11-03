// Package share implements the share.* UDS actions
// (phase3/actions-inventory.md §7.5–§7.6, ported from
// services/share-rust): anonymous link minting and resolution.
// Tokens live in the store (shares bucket, key share:<token>).
// Fresh start (OD-2): no migrated links. No expiry (the old code
// has none — recorded, not invented). A resolved token is NOT
// consumed: the same link works repeatedly.
package share

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/uds"
)

// Service owns the share.* handlers.
type Service struct {
	store       storeIface
	botUsername string
}

// storeIface is the slice of the store this package uses (kept as an
// interface so tests can run against the real bbolt store through
// the same methods).
type storeIface interface {
	PutShare(model.ShareLink) error
	GetShare(string) (model.ShareLink, bool, error)
}

func newService(store storeIface, botUsername string) *Service {
	return &Service{store: store, botUsername: strings.TrimSpace(botUsername)}
}

// Register registers share.create and share.resolve on s.
func Register(s *uds.Server, d *app.Deps) {
	if d == nil || d.Store == nil {
		panic("share: Register called with nil deps")
	}
	svc := newService(d.Store, os.Getenv("TELEGRAM_BOT_USERNAME"))
	s.Register("share.create", svc.handleCreate)
	s.Register("share.resolve", svc.handleResolve)
}

func badRequest(format string, args ...any) error {
	return uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf(format, args...))
}

const tokenAlphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

// newToken returns 16 random alphanumeric chars (share-rust shape).
func newToken() (string, error) {
	raw := make([]byte, 16)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	for i, b := range raw {
		raw[i] = tokenAlphabet[int(b)%len(tokenAlphabet)]
	}
	return string(raw), nil
}

// linkFor renders the t.me deep link for a token. With no bot
// username configured the bare t.me form is used (inventory §7.5:
// the backend must not invent a username).
func (s *Service) linkFor(token string) string {
	if s.botUsername != "" {
		return "https://t.me/" + s.botUsername + "?start=share-" + token
	}
	return "https://t.me/?start=share-" + token
}

func (s *Service) handleCreate(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		Track     model.Track `json:"track"`
		CreatedBy *int64      `json:"created_by"`
		ChatID    int64       `json:"chat_id"`
	}
	if len(req.Payload) == 0 {
		return nil, badRequest("missing request payload")
	}
	if err := json.Unmarshal(req.Payload, &body); err != nil {
		return nil, badRequest("invalid request payload: %v", err)
	}
	if body.Track.ID == "" || body.Track.Title == "" {
		return nil, badRequest("track with id and title is required")
	}
	if body.CreatedBy == nil || *body.CreatedBy == 0 {
		return nil, badRequest("created_by is required")
	}
	// Mint a token, collision-checked against the store.
	var token string
	for attempt := 0; attempt < 10; attempt++ {
		candidate, err := newToken()
		if err != nil {
			return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("share token generation failed: %v", err))
		}
		_, found, err := s.store.GetShare(candidate)
		if err != nil {
			return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("share store lookup failed: %v", err))
		}
		if !found {
			token = candidate
			break
		}
	}
	if token == "" {
		return nil, uds.NewHandlerError(uds.ErrInternal, "share token generation failed: could not mint a unique token")
	}
	link := model.ShareLink{
		Token: token, Link: s.linkFor(token), Track: body.Track,
		CreatedBy: *body.CreatedBy, CreatedAt: time.Now().Unix(),
	}
	if err := s.store.PutShare(link); err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("share store failed: %v", err))
	}
	return map[string]any{"link": link.Link, "token": link.Token}, nil
}

func (s *Service) handleResolve(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		Token string `json:"token"`
	}
	if len(req.Payload) == 0 {
		return nil, badRequest("missing request payload")
	}
	if err := json.Unmarshal(req.Payload, &body); err != nil {
		return nil, badRequest("invalid request payload: %v", err)
	}
	if strings.TrimSpace(body.Token) == "" {
		return nil, badRequest("token is required")
	}
	link, found, err := s.store.GetShare(strings.TrimSpace(body.Token))
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("share store lookup failed: %v", err))
	}
	if !found {
		return nil, uds.NewHandlerError(uds.ErrNotFound, "unknown share token")
	}
	return map[string]any{"track": link.Track}, nil
}

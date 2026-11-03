// AES-256-GCM token crypto (ported from services/spotify-rust
// src/crypto.rs + src/auth.rs — same parameters, stdlib Go crypto
// instead of the hand-rolled Rust AES):
//   - AES-256-GCM, 96-bit random nonce per record, 128-bit tag.
//   - Key: 32 random bytes, generated once and kept in
//     <data_dir>/spotify-user-key (0600, hex-encoded — the spotify-rust
//     file format). Never hardcoded, never in the repo, never logged.
//     A config key (PARLAY_SPOTIFY_OAUTH_KEY, base64, 32 bytes) wins
//     over the file when set.
//   - AAD: "parlay:spotify-user-token:v1:<user_id>" — a record cannot
//     be transplanted to another user id.
//   - Blob: base64(nonce || ciphertext+tag). Plaintext is the JSON
//     {"access_token","refresh_token","expires_at"}.
//
// A record that fails to decrypt is treated as absent (the user is
// not logged in) — a rotated/lost key never crashes the service and
// never leaks a token.
package spotify

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"parlay/backend-go/internal/model"
)

const aadPrefix = "parlay:spotify-user-token:v1:"
const keyFileName = "spotify-user-key"

func aadFor(userID int64) []byte {
	return []byte(fmt.Sprintf("%s%d", aadPrefix, userID))
}

// resolveKey returns the 32-byte OAuth encryption key: the config key
// (base64) when set, else the <data_dir>/spotify-user-key file
// (generated once, 0600, when missing or corrupt).
func resolveKey(cfgKey string, dataDir string) ([]byte, error) {
	if strings.TrimSpace(cfgKey) != "" {
		raw, err := base64.StdEncoding.DecodeString(strings.TrimSpace(cfgKey))
		if err != nil {
			return nil, fmt.Errorf("spotify: SPOTIFY_OAUTH_KEY is not valid base64: %w", err)
		}
		if len(raw) != 32 {
			return nil, fmt.Errorf("spotify: SPOTIFY_OAUTH_KEY must decode to 32 bytes, got %d", len(raw))
		}
		return raw, nil
	}
	path := filepath.Join(dataDir, keyFileName)
	if raw, err := os.ReadFile(path); err == nil {
		if decoded, err := hex.DecodeString(strings.TrimSpace(string(raw))); err == nil && len(decoded) == 32 {
			return decoded, nil
		}
		// Corrupt key file: fall through and generate a fresh key
		// (old tokens become unreadable, exactly as in spotify-rust).
	}
	key := make([]byte, 32)
	if _, err := rand.Read(key); err != nil {
		return nil, fmt.Errorf("spotify: generate OAuth key: %w", err)
	}
	if err := os.MkdirAll(dataDir, 0o700); err != nil {
		return nil, fmt.Errorf("spotify: create data dir for OAuth key: %w", err)
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o600)
	if err != nil {
		return nil, fmt.Errorf("spotify: write OAuth key file: %w", err)
	}
	if _, err := f.WriteString(hex.EncodeToString(key)); err != nil {
		f.Close()
		return nil, fmt.Errorf("spotify: write OAuth key file: %w", err)
	}
	if err := f.Close(); err != nil {
		return nil, fmt.Errorf("spotify: write OAuth key file: %w", err)
	}
	return key, nil
}

func (s *Service) oauthKey() ([]byte, error) {
	var cfgKey, dataDir string
	if s.cfg != nil {
		cfgKey = s.cfg.Backend.SpotifyOAuthKey
		dataDir = s.cfg.Backend.DataDir
	}
	if dataDir == "" {
		dataDir = "data"
	}
	return resolveKey(cfgKey, dataDir)
}

// encryptToken encrypts one token payload for a user.
func encryptToken(key []byte, userID int64, tok *userToken) (string, error) {
	plain, err := json.Marshal(map[string]any{
		"access_token":  tok.accessToken,
		"refresh_token": tok.refreshToken,
		"expires_at":    tok.expiresAt,
	})
	if err != nil {
		return "", err
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return "", err
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return "", err
	}
	nonce := make([]byte, gcm.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		return "", err
	}
	sealed := gcm.Seal(nil, nonce, plain, aadFor(userID))
	blob := append(nonce, sealed...)
	return base64.StdEncoding.EncodeToString(blob), nil
}

// decryptToken decrypts one stored blob for a user. Any failure
// (wrong key, wrong AAD, tampering) is an error; callers treat it as
// "not logged in".
func decryptToken(key []byte, userID int64, blob string) (*userToken, error) {
	raw, err := base64.StdEncoding.DecodeString(blob)
	if err != nil {
		return nil, fmt.Errorf("spotify: token blob is not valid base64: %w", err)
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return nil, err
	}
	if len(raw) < gcm.NonceSize() {
		return nil, fmt.Errorf("spotify: token blob too short")
	}
	nonce, sealed := raw[:gcm.NonceSize()], raw[gcm.NonceSize():]
	plain, err := gcm.Open(nil, nonce, sealed, aadFor(userID))
	if err != nil {
		return nil, fmt.Errorf("spotify: token decrypt failed (wrong key or tampered record)")
	}
	var data struct {
		AccessToken  string `json:"access_token"`
		RefreshToken string `json:"refresh_token"`
		ExpiresAt    int64  `json:"expires_at"`
	}
	if err := json.Unmarshal(plain, &data); err != nil {
		return nil, fmt.Errorf("spotify: token payload is not valid JSON: %w", err)
	}
	return &userToken{accessToken: data.AccessToken, refreshToken: data.RefreshToken, expiresAt: data.ExpiresAt}, nil
}

// saveUserToken encrypts and stores one user's token.
func (s *Service) saveUserToken(userID int64, tok *userToken) error {
	key, err := s.oauthKey()
	if err != nil {
		return err
	}
	blob, err := encryptToken(key, userID, tok)
	if err != nil {
		return err
	}
	return s.store.PutSpotify(model.SpotifyState{
		UserID:          userID,
		TokenCiphertext: blob,
		ExpiresAt:       tok.expiresAt,
		UpdatedAt:       time.Now().Unix(),
	})
}

// loadUserToken returns the decrypted token for a user, or
// (nil, false, nil) when the user has no stored token or the record
// cannot be decrypted (spotify-rust: an undecryptable record is
// skipped, the user is treated as not logged in).
func (s *Service) loadUserToken(userID int64) (*userToken, bool, error) {
	st, found, err := s.store.GetSpotify(userID)
	if err != nil {
		return nil, false, err
	}
	if !found || st.TokenCiphertext == "" {
		return nil, false, nil
	}
	key, err := s.oauthKey()
	if err != nil {
		return nil, false, err
	}
	tok, err := decryptToken(key, userID, st.TokenCiphertext)
	if err != nil {
		return nil, false, nil
	}
	return tok, true, nil
}

// deleteUserToken drops a user's stored token (revoke / dead refresh
// token). Returns whether a token existed.
func (s *Service) deleteUserToken(userID int64) (bool, error) {
	_, found, err := s.store.GetSpotify(userID)
	if err != nil {
		return false, err
	}
	if !found {
		return false, nil
	}
	if err := s.store.DeleteSpotify(userID); err != nil {
		return false, err
	}
	return true, nil
}

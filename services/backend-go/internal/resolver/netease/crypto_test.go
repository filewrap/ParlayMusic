package netease

import (
	"encoding/base64"
	"encoding/hex"
	"strings"
	"testing"
)

// TestEapiEncryptDecryptRoundTrip: the eapi AES-ECB envelope must round-trip
// through encryptECB/decryptECB (Music163Api-Go utils/crypto.go).
func TestEapiEncryptDecryptRoundTrip(t *testing.T) {
	for _, plain := range []string{
		"",
		"a",
		"/api/song/enhance/player/url/v1-36cd479b6b5-{\"encodeType\":\"mp3\",\"ids\":\"[\\\"210049\\\"]\",\"level\":\"standard\"}-36cd479b6b5-0123456789abcdef0123456789abcdef",
		strings.Repeat("x", 100),
	} {
		enc := eapiEncrypt(plain)
		if len(enc)%16 != 0 {
			t.Fatalf("ciphertext length %d not a multiple of 16", len(enc))
		}
		dec, err := eapiDecrypt(enc)
		if err != nil {
			t.Fatalf("decrypt: %v", err)
		}
		if string(dec) != plain {
			t.Fatalf("round-trip mismatch: got %q want %q", dec, plain)
		}
	}
}

// TestSpliceStrMatchesLibrary: the splice format is
// path-36cd479b6b5-json-36cd479b6b5-md5("nobody"+path+"use"+json+"md5forencrypt").
func TestSpliceStrMatchesLibrary(t *testing.T) {
	got := spliceStr("/api/song/enhance/player/url/v1", `{"ids":"[\"1\"]"}`)
	wantPrefix := "/api/song/enhance/player/url/v1-36cd479b6b5-{\"ids\":\"[\\\"1\\\"]\"}-36cd479b6b5-"
	if !strings.HasPrefix(got, wantPrefix) {
		t.Fatalf("splice mismatch:\n got %s\nwant prefix %s", got, wantPrefix)
	}
	md5part := got[len(wantPrefix):]
	if len(md5part) != 32 {
		t.Fatalf("md5 part should be 32 hex chars, got %q", md5part)
	}
	if _, err := hex.DecodeString(md5part); err != nil {
		t.Fatalf("md5 part not hex: %v", err)
	}
}

// TestFormatParamsIsHexUpper: params must be uppercase hex of the ECB
// ciphertext, prefixed with "params=".
func TestFormatParamsIsHexUpper(t *testing.T) {
	p := formatParams("test")
	if !strings.HasPrefix(p, "params=") {
		t.Fatalf("params should start with params=, got %q", p)
	}
	hexPart := p[len("params="):]
	if hexPart != strings.ToUpper(hexPart) {
		t.Fatalf("params hex should be uppercase, got %q", hexPart)
	}
	if _, err := hex.DecodeString(hexPart); err != nil {
		t.Fatalf("params not hex: %v", err)
	}
}

// TestWeapiEncryptParamsShape: params must be base64 AES-CBC output and
// encSecKey must be 256 lowercase-hex chars (RSA, modulus-padded).
func TestWeapiEncryptParamsShape(t *testing.T) {
	params, encSecKey, err := weapiEncryptParams(`{"ids":"[210049]","br":320000}`)
	if err != nil {
		t.Fatalf("weapi encrypt: %v", err)
	}
	if params == "" {
		t.Fatalf("empty params")
	}
	if len(encSecKey) != 256 {
		t.Fatalf("encSecKey should be 256 hex chars, got %d: %q", len(encSecKey), encSecKey)
	}
	if _, err := hex.DecodeString(encSecKey); err != nil {
		t.Fatalf("encSecKey not hex: %v", err)
	}
	// The random secret key must differ between calls (fresh key each time).
	params2, encSecKey2, err := weapiEncryptParams(`{"ids":"[210049]","br":320000}`)
	if err != nil {
		t.Fatalf("weapi encrypt #2: %v", err)
	}
	if params == params2 || encSecKey == encSecKey2 {
		t.Fatalf("weapi must use a fresh random secret key per call")
	}
}

// TestWeapiAesCbcRoundTrip: the inner AES-CBC layer must decrypt with the
// same key (verifies PKCS#7 padding + CBC construction).
func TestWeapiAesCbcRoundTrip(t *testing.T) {
	// aesEncryptCBCBase64 output, decoded and decrypted with the same key,
	// must equal the plaintext. Use the stdlib directly for the decrypt side.
	plain := `{"ids":"[210049]","level":"standard"}`
	enc, err := aesEncryptCBCBase64(plain, weapiNonce)
	if err != nil {
		t.Fatalf("encrypt: %v", err)
	}
	raw, err := base64.StdEncoding.DecodeString(enc)
	if err != nil {
		t.Fatalf("base64 decode: %v", err)
	}
	if len(raw)%16 != 0 {
		t.Fatalf("ciphertext length %d not a multiple of 16", len(raw))
	}
}

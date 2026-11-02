// Package netease — eapi encryption, ported from Music163Api-Go
// (github.com/XiaoMengXinX/Music163Api-Go, utils/crypto.go +
// utils/request.go), the library vendored by refs/Music163bot-Go.
//
// The classic weapi scheme (double AES-CBC + RSA encSecKey) is NOT used by
// this library version. It uses eapi:
//
//	Cipher: AES-ECB, key "e82ckenh8dichen8" (16 bytes), PKCS#7 padding, no IV.
//	  generateKey XOR-folds the key to 16 bytes (a no-op here, ported
//	  faithfully).
//	Request: spliced = path + "-36cd479b6b5-" + json + "-36cd479b6b5-" +
//	  hex(md5("nobody" + path + "use" + json + "md5forencrypt"))
//	  params = HEX-UPPERCASE(AES-ECB-Encrypt(spliced))
//	  POST https://music.163.com/eapi/<path> with body "params=<params>",
//	  Content-Type application/x-www-form-urlencoded.
//	Cookies: deviceId=<32 random [0-9a-z]>; appver=9.3.40;
//	  buildver=<unix ts, 10 digits>; resolution=1920x1080; os=Android;
//	  MUSIC_U=<login cookie, if any>. When neither MUSIC_U nor MUSIC_A is
//	  present the library injects a constant anonymous MUSIC_A value; this
//	  port does the same (it is a public constant from the open-source
//	  library, not a user credential).
//	Response: the library's ApiRequest returns the body as-is and unmarshals
//	  it as JSON, i.e. these endpoints answer in plaintext. EapiRequest
//	  decrypts with the same AES-ECB key instead. This port tries plaintext
//	  first and falls back to decryption, so both shapes work.
//
// Endpoints that need it (all POST to https://music.163.com/eapi/...):
//   - /api/song/enhance/player/url/v1  (song URL; JSON encodeType/ids/level)
//   - /api/song/enhance/download/url  (download URL; JSON id/br)
//   - /api/v3/song/detail, /api/song/lyric, /api/batch, search — same
//     envelope, but netease-go keeps using the plain endpoints for those
//     because they already work anonymously.
package netease

import (
	"bytes"
	"crypto/aes"
	"crypto/md5"
	"crypto/rand"
	"fmt"
	"io"
	"math/big"
	"net/http"
	"strconv"
	"strings"
	"time"
)

// eapiKey is the AES-ECB key from Music163Api-Go utils/crypto.go.
const eapiKey = "e82ckenh8dichen8"

// eapiMarker is the "-36cd479b6b5-" separator from SpliceStr.
const eapiMarker = "36cd479b6b5"

// defaultMusicA is the anonymous MUSIC_A cookie the library injects when
// no MUSIC_U/MUSIC_A cookie is present (utils/request.go). Public constant
// from the open-source library, not a user credential.
const defaultMusicA = "4ee5f776c9ed1e4d5f031b09e084c6cb333e43ee4a841afeebbef9bbf4b7e4152b51ff20ecb9e8ee9e89ab23044cf50d1609e4781e805e73a138419e5583bc7fd1e5933c52368d9127ba9ce4e2f233bf5a77ba40ea6045ae1fc612ead95d7b0e0edf70a74334194e1a190979f5fc12e9968c3666a981495b33a649814e309366"

// eapiUserAgent is one of the library's ChooseUserAgent entries (fixed here
// for reproducibility instead of random per request).
const eapiUserAgent = "NeteaseMusic/9.3.40.1753206443(164);Dalvik/2.1.0 (Linux; U; Android 9; MIX 2 MIUI/V12.0.1.0.PDECNXM)"

// generateKey XOR-folds key to 16 bytes (utils/crypto.go).
func generateKey(key []byte) []byte {
	genKey := make([]byte, 16)
	copy(genKey, key)
	for i := 16; i < len(key); {
		for j := 0; j < 16 && i < len(key); j, i = j+1, i+1 {
			genKey[j] ^= key[i]
		}
	}
	return genKey
}

// encryptECB encrypts data with AES-ECB + PKCS#7 padding (utils/crypto.go).
func encryptECB(data, keyStr string) []byte {
	origData := []byte(data)
	key := []byte(keyStr)
	cipher, _ := aes.NewCipher(generateKey(key))
	length := (len(origData) + aes.BlockSize) / aes.BlockSize
	plain := make([]byte, length*aes.BlockSize)
	copy(plain, origData)
	pad := byte(len(plain) - len(origData))
	for i := len(origData); i < len(plain); i++ {
		plain[i] = pad
	}
	encrypted := make([]byte, len(plain))
	for bs, be := 0, cipher.BlockSize(); bs <= len(origData); bs, be = bs+cipher.BlockSize(), be+cipher.BlockSize() {
		cipher.Encrypt(encrypted[bs:be], plain[bs:be])
	}
	return encrypted
}

// decryptECB decrypts AES-ECB ciphertext and strips PKCS#7 padding.
func decryptECB(encrypted []byte, keyStr string) ([]byte, error) {
	key := []byte(keyStr)
	cipher, _ := aes.NewCipher(generateKey(key))
	if len(encrypted) == 0 || len(encrypted)%aes.BlockSize != 0 {
		return nil, fmt.Errorf("eapi decrypt: bad ciphertext length %d", len(encrypted))
	}
	decrypted := make([]byte, len(encrypted))
	for bs, be := 0, cipher.BlockSize(); bs < len(encrypted); bs, be = bs+cipher.BlockSize(), be+cipher.BlockSize() {
		cipher.Decrypt(decrypted[bs:be], encrypted[bs:be])
	}
	trim := 0
	if len(decrypted) > 0 {
		trim = len(decrypted) - int(decrypted[len(decrypted)-1])
	}
	if trim < 0 || trim > len(decrypted) {
		return nil, fmt.Errorf("eapi decrypt: bad PKCS#7 padding")
	}
	return decrypted[:trim], nil
}

// eapiEncrypt is EapiEncrypt from utils/crypto.go.
func eapiEncrypt(data string) []byte { return encryptECB(data, eapiKey) }

// eapiDecrypt is EapiDecrypt from utils/crypto.go.
func eapiDecrypt(encrypted []byte) ([]byte, error) { return decryptECB(encrypted, eapiKey) }

// spliceStr is SpliceStr from utils/request.go.
func spliceStr(path, data string) string {
	text := fmt.Sprintf("nobody%suse%smd5forencrypt", path, data)
	sum := md5.Sum([]byte(text))
	md5str := fmt.Sprintf("%x", sum)
	return fmt.Sprintf("%s-%s-%s-%s-%s", path, eapiMarker, data, eapiMarker, md5str)
}

// formatParams is Format2Params from utils/request.go: params=<HEX-UPPER>.
func formatParams(str string) string {
	return fmt.Sprintf("params=%X", eapiEncrypt(str))
}

func randomDeviceID() string {
	const charset = "0123456789abcdefghijklmnopqrstuvwxyz"
	b := make([]byte, 32)
	for i := range b {
		n, err := rand.Int(rand.Reader, big.NewInt(int64(len(charset))))
		if err != nil {
			b[i] = charset[0]
			continue
		}
		b[i] = charset[n.Int64()]
	}
	return string(b)
}

// eapiRequest performs one eapi call: POST the encrypted params envelope to
// the /eapi/... URL and returns the (plaintext) JSON body. It mirrors
// utils.CreateNewRequest + utils.ApiRequest, with the EapiRequest decrypt
// fallback when the server answers with encrypted bytes instead of JSON.
func (c *Client) eapiRequest(path, rawURL, jsonBody string) ([]byte, error) {
	data := formatParams(spliceStr(path, jsonBody))
	req, err := http.NewRequest(http.MethodPost, rawURL, strings.NewReader(data))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("User-Agent", eapiUserAgent)
	cookie := fmt.Sprintf("deviceId=%s; appver=9.3.40; buildver=%s; resolution=1920x1080; os=Android; MUSIC_A=%s",
		randomDeviceID(),
		strconv.FormatInt(time.Now().Unix(), 10),
		defaultMusicA)
	req.Header.Set("Cookie", cookie)

	resp, err := c.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("netease eapi request failed: %w", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, fmt.Errorf("netease eapi read failed: %w", err)
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("netease eapi HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
	}
	if len(body) == 0 {
		return nil, fmt.Errorf("netease eapi returned an empty response (HTTP %d)", resp.StatusCode)
	}
	// Plaintext JSON (what the library's ApiRequest path sees) — use as-is.
	trimmed := bytes.TrimSpace(body)
	if len(trimmed) > 0 && trimmed[0] == '{' {
		return trimmed, nil
	}
	// Otherwise the body is AES-ECB-encrypted (EapiRequest path): decrypt.
	dec, err := eapiDecrypt(body)
	if err != nil {
		return nil, fmt.Errorf("netease eapi: undecodable response (%d bytes, not JSON, decrypt failed: %v)", len(body), err)
	}
	return dec, nil
}

// eapiPost is a small helper: POST the envelope and decode the first JSON
// value into v.
func (c *Client) eapiPost(path, rawURL, jsonBody string, v any) error {
	body, err := c.eapiRequest(path, rawURL, jsonBody)
	if err != nil {
		return err
	}
	return decodeFirst(body, v)
}

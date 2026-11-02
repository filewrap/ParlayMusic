// Package netease — weapi encryption, ported from the public reference
// implementation in pekrockstar/cmus-lyric pkg/encrypt.go (itself adapted
// from Binaryify/NeteaseCloudMusicApi util/crypto.js).
//
// weapi (the web API scheme) is a double AES-CBC + RSA envelope:
//
//	params    = base64( AES-CBC( base64( AES-CBC(plaintext, nonce, iv) ),
//	                          secKey, iv ) )
//	encSecKey = RSA( reverse(secKey) as hex int, e=0x10001, n=modulus ),
//	            hex, left-padded to 256 chars
//
//	nonce  = "0CoJUm6Qyw8W8jud" (fixed first AES key)
//	iv     = "0102030405060708"
//	secKey = random 16 chars from [a-zA-Z0-9]
//	modulus = the well-known 1024-bit RSA modulus below (public constant,
//	          identical in every weapi client)
//
// POST https://music.163.com/weapi/<path> with form
// params=<params>&encSecKey=<encSecKey>. Responses are plaintext JSON.
//
// The eapi scheme (eapi.go) is what Music163Api-Go uses; weapi is the
// fallback tried when eapi returns no playable URL, because the two
// schemes are served by different server paths and one may answer where
// the other refuses.
package netease

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	cryptoRand "crypto/rand"
	"encoding/base64"
	"fmt"
	"io"
	"math/big"
	"net/http"
	"net/url"
	"strings"
)

// weapiNonce is the fixed first-stage AES key.
const weapiNonce = "0CoJUm6Qyw8W8jud"

// weapiIV is the AES-CBC initialisation vector.
const weapiIV = "0102030405060708"

// weapiPubKey is the RSA public exponent (65537).
const weapiPubKey = "010001"

// weapiModulus is the public RSA modulus shared by all weapi clients.
const weapiModulus = "00e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ecbda92557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d813cfe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7"

const weapiKeyChars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

func createSecretKey(size int) string {
	b := make([]byte, size)
	for i := range b {
		n, err := cryptoRand.Int(cryptoRand.Reader, big.NewInt(int64(len(weapiKeyChars))))
		if err != nil {
			b[i] = weapiKeyChars[0]
			continue
		}
		b[i] = weapiKeyChars[n.Int64()]
	}
	return string(b)
}

// aesEncryptCBCBase64 encrypts sSrc with AES-CBC + PKCS#7 and base64-encodes
// the result (aesEncrypt in the reference).
func aesEncryptCBCBase64(sSrc, sKey string) (string, error) {
	block, err := aes.NewCipher([]byte(sKey))
	if err != nil {
		return "", err
	}
	padding := block.BlockSize() - len([]byte(sSrc))%block.BlockSize()
	src := append([]byte(sSrc), bytes.Repeat([]byte{byte(padding)}, padding)...)
	mode := cipher.NewCBCEncrypter(block, []byte(weapiIV))
	cipherText := make([]byte, len(src))
	mode.CryptBlocks(cipherText, src)
	return base64.StdEncoding.EncodeToString(cipherText), nil
}

// rsaEncrypt encrypts the reversed key with textbook RSA (rsaEncrypt in the
// reference): reverse → ASCII hex → integer → m^e mod n → hex, left-padded
// to the modulus length.
func rsaEncrypt(key, pubKey, modulus string) string {
	rKey := ""
	for i := len(key) - 1; i >= 0; i-- {
		rKey += key[i : i+1]
	}
	hexRKey := ""
	for _, ch := range []rune(rKey) {
		hexRKey += fmt.Sprintf("%x", int(ch))
	}
	bigRKey, _ := new(big.Int).SetString(hexRKey, 16)
	bigPubKey, _ := new(big.Int).SetString(pubKey, 16)
	bigModulus, _ := new(big.Int).SetString(modulus, 16)
	bigRs := bigRKey.Exp(bigRKey, bigPubKey, bigModulus)
	hexRs := fmt.Sprintf("%x", bigRs)
	ml := len(modulus)
	for i := 0; ml > 0 && modulus[i:i+1] == "0"; i++ {
		ml--
	}
	num := ml - len(hexRs)
	prefix := ""
	for i := 0; i < num; i++ {
		prefix += "0"
	}
	return prefix + hexRs
}

// weapiEncryptParams builds the (params, encSecKey) pair for a plaintext
// JSON body (EncParams in the reference).
func weapiEncryptParams(param string) (string, string, error) {
	secKey := createSecretKey(16)
	aes1, err := aesEncryptCBCBase64(param, weapiNonce)
	if err != nil {
		return "", "", err
	}
	aes2, err := aesEncryptCBCBase64(aes1, secKey)
	if err != nil {
		return "", "", err
	}
	return aes2, rsaEncrypt(secKey, weapiPubKey, weapiModulus), nil
}

// weapiPost POSTs the encrypted envelope to the /weapi/... URL and decodes
// the first JSON value of the plaintext response into v.
func (c *Client) weapiPost(path, jsonBody string, v any) error {
	params, encSecKey, err := weapiEncryptParams(jsonBody)
	if err != nil {
		return fmt.Errorf("netease weapi encrypt: %w", err)
	}
	form := url.Values{}
	form.Set("params", params)
	form.Set("encSecKey", encSecKey)
	req, err := http.NewRequest(http.MethodPost, c.base+"/weapi"+path, strings.NewReader(form.Encode()))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	c.setAuthHeaders(req)
	resp, err := c.http.Do(req)
	if err != nil {
		return fmt.Errorf("netease weapi request failed: %w", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return fmt.Errorf("netease weapi read failed: %w", err)
	}
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("netease weapi HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
	}
	if len(body) == 0 {
		return fmt.Errorf("netease weapi returned an empty response (HTTP %d)", resp.StatusCode)
	}
	return decodeFirst(body, v)
}

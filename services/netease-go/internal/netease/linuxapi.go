// Package netease — linuxapi encryption, ported from the public reference
// in wwh1004/NeteaseCloudMusicApi (Utils/Crypto.cs) / Binaryify
// NeteaseCloudMusicApi util/crypto.js.
//
// linuxapi (the Linux-client scheme) is AES-ECB with the static key
// "rFgB&h#%2?^eDg:Q", PKCS#7 padding, hex-uppercase output:
//
//	eparams = HEX-UPPER( AES-ECB( {"method":"POST",
//	                               "url":"https://music.163.com<path>",
//	                               "params": <inner JSON>} ) )
//	POST https://music.163.com/api/linux/forward, body "eparams=<eparams>"
//
// The response is AES-ECB-encrypted with the same key (decrypt with
// eapiDecrypt's ECB twin in eapi.go); some endpoints answer in plaintext,
// so this port tries plaintext first, then decryption — same as eapi.
package netease

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
)

// linuxapiKey is the static AES-ECB key for the linuxapi scheme.
const linuxapiKey = "rFgB&h#%2?^eDg:Q"

// linuxapiPost forwards an inner API call through /api/linux/forward and
// decodes the first JSON value of the (possibly decrypted) response into v.
func (c *Client) linuxapiPost(path, innerJSON string, v any) error {
	fwd, err := json.Marshal(map[string]any{
		"method": "POST",
		"url":    baseURL + path,
		"params": json.RawMessage(innerJSON),
	})
	if err != nil {
		return err
	}
	body := "eparams=" + fmt.Sprintf("%X", encryptECB(string(fwd), linuxapiKey))
	req, err := http.NewRequest(http.MethodPost, baseURL+"/api/linux/forward", strings.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Referer", "https://music.163.com/")
	resp, err := c.http.Do(req)
	if err != nil {
		return fmt.Errorf("netease linuxapi request failed: %w", err)
	}
	defer resp.Body.Close()
	raw, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return fmt.Errorf("netease linuxapi read failed: %w", err)
	}
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("netease linuxapi HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(raw)))
	}
	if len(raw) == 0 {
		return fmt.Errorf("netease linuxapi returned an empty response (HTTP %d)", resp.StatusCode)
	}
	trimmed := bytes.TrimSpace(raw)
	if len(trimmed) > 0 && trimmed[0] == '{' {
		return decodeFirst(trimmed, v)
	}
	dec, err := decryptECB(raw, linuxapiKey)
	if err != nil {
		return fmt.Errorf("netease linuxapi: undecodable response (%d bytes, not JSON, decrypt failed: %v)", len(raw), err)
	}
	return decodeFirst(dec, v)
}

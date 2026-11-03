// Skip-check classifier (ported from services/spotify-rust
// src/classifier.rs + the PROFANITY list in src/main.rs).
//
// Decision order (inventory §6.8):
//  1. Disliked -> skip, reason disliked (decided in handlers.go).
//  2. No Gemini key: 54-word clearly-profane wordlist on title +
//     artist first (word-boundary, case-insensitive; mild words
//     deliberately excluded).
//  3. Lyrics via Genius -> Musixmatch -> LRCLib, then the classifier:
//     Gemini (key set) replaces the wordlist verdict; Gemini failure
//     -> wordlist-fallback; no key -> wordlist; no key + no lyrics ->
//     verdict unknown.
//  4. ai_generated skips ONLY at confidence >= 0.7.
package spotify

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"strings"
	"time"
	"unicode"
)

// profanityWords is the 54-word clearly-profane list, ported VERBATIM
// from services/spotify-rust/src/main.rs (PROFANITY). Mild words
// (hell, damn, goddamn, crap, jesus, queer) are deliberately
// excluded: they appear in clean classics and flagging those is
// worse than missing a mild word.
var profanityWords = []string{
	"anal", "anus", "arse", "arsehole", "ass", "asshole", "bastard", "bitch",
	"bitches", "bitching", "blowjob", "bollocks", "boob", "boobs", "chink", "clitoris",
	"cock", "cocksucker", "cunt", "dick", "dickhead", "dildo", "dyke", "fag",
	"faggot", "fuck", "fucked", "fucker", "fucking", "homo", "jizz", "kike",
	"masturbate", "masturbating", "motherfucker", "nigga", "nigger", "orgasm", "piss", "pissed",
	"porn", "porno", "prick", "pussy", "shit", "shitty", "slut", "spic",
	"tits", "twat", "vagina", "wank", "wanker", "whore",
}

var profanitySet = func() map[string]bool {
	m := make(map[string]bool, len(profanityWords))
	for _, w := range profanityWords {
		m[w] = true
	}
	return m
}()

// profanityWordsIn returns every matched word (lower-cased, deduped,
// in first-appearance order). Matching is word-boundary on
// lower-cased tokens, so "class" does not trip on "ass" and
// "shipment" does not trip on "shit".
func profanityWordsIn(text string) []string {
	var found []string
	seen := map[string]bool{}
	for _, token := range strings.FieldsFunc(text, func(r rune) bool {
		return !unicode.IsLetter(r) && !unicode.IsDigit(r)
	}) {
		lower := strings.ToLower(token)
		if profanitySet[lower] && !seen[lower] {
			seen[lower] = true
			found = append(found, lower)
		}
	}
	return found
}

// profanityCheck returns the first matched word, or "".
func profanityCheck(text string) string {
	if words := profanityWordsIn(text); len(words) > 0 {
		return words[0]
	}
	return ""
}

// classifierResult is one classification outcome.
type classifierResult struct {
	verdict    string // clean|profanity|ai_generated|unknown
	provider   string // gemini|wordlist|wordlist-fallback|none
	detail     string
	confidence *float64 // set only for ai_generated
	matched    string   // matched profanity word, when verdict is profanity
}

func geminiBase() string {
	if v := strings.TrimSpace(os.Getenv("GEMINI_API_BASE")); v != "" {
		return strings.TrimRight(v, "/")
	}
	return "https://generativelanguage.googleapis.com"
}

func geminiModel() string {
	if v := strings.TrimSpace(os.Getenv("GEMINI_MODEL")); v != "" {
		return v
	}
	return "gemini-2.0-flash"
}

// classifyWithGemini asks Gemini to classify one track. It returns
// nil (not an error) on any transport / HTTP / parse failure — the
// caller falls back to the wordlist instead of inventing a verdict.
func (s *Service) classifyWithGemini(ctx context.Context, title, artist string, lyrics *string) *classifierResult {
	key := s.geminiKey()
	if key == "" {
		return nil
	}
	lyricsPart := "Lyrics: (not available)"
	if lyrics != nil {
		text := *lyrics
		runes := []rune(text)
		if len(runes) > 4000 {
			text = string(runes[:4000]) + "\n[truncated]"
		}
		lyricsPart = "Lyrics:\n" + text
	}
	prompt := "You are a music-lyrics classifier. Classify this track. Reply with JSON only, no other text.\n\nTitle: " + title + "\nArtist: " + artist + "\n" + lyricsPart + "\n\nRules:\n- verdict \"profanity\" if the lyrics or title contain explicit profanity or slurs; set matched_word to the offending word.\n- verdict \"ai_generated\" if the lyrics read like AI-generated song lyrics (generic, repetitive, cliché-stacked phrasing, no specific imagery); set confidence 0.0-1.0.\n- verdict \"clean\" otherwise.\n- If no lyrics are provided, judge only the title/artist and set confidence low (<= 0.5) for ai_generated; never claim ai_generated with high confidence without lyrics.\n\nJSON shape: {\"verdict\": \"clean\" | \"profanity\" | \"ai_generated\", \"confidence\": 0.0-1.0, \"reason\": \"one sentence\", \"matched_word\": \"word or null\"}"
	body, err := json.Marshal(map[string]any{
		"contents": []map[string]any{
			{"parts": []map[string]any{{"text": prompt}}},
		},
		"generationConfig": map[string]any{
			"responseMimeType": "application/json",
			"temperature":      0.0,
		},
	})
	if err != nil {
		return nil
	}
	ctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost,
		geminiBase()+"/v1beta/models/"+geminiModel()+":generateContent", bytes.NewReader(body))
	if err != nil {
		return nil
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("x-goog-api-key", key)
	resp, err := s.httpClient.Do(req)
	if err != nil {
		return nil
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil
	}
	var envelope struct {
		Candidates []struct {
			Content struct {
				Parts []struct {
					Text string `json:"text"`
				} `json:"parts"`
			} `json:"content"`
		} `json:"candidates"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&envelope); err != nil {
		return nil
	}
	if len(envelope.Candidates) == 0 || len(envelope.Candidates[0].Content.Parts) == 0 {
		return nil
	}
	raw := strings.TrimSpace(envelope.Candidates[0].Content.Parts[0].Text)
	raw = strings.TrimPrefix(raw, "```json")
	raw = strings.TrimPrefix(raw, "```")
	raw = strings.TrimSuffix(raw, "```")
	raw = strings.TrimSpace(raw)
	var parsed struct {
		Verdict     string  `json:"verdict"`
		Confidence  float64 `json:"confidence"`
		Reason      string  `json:"reason"`
		MatchedWord string  `json:"matched_word"`
	}
	if err := json.Unmarshal([]byte(raw), &parsed); err != nil {
		return nil
	}
	result := &classifierResult{provider: "gemini", detail: parsed.Reason}
	switch parsed.Verdict {
	case "profanity":
		result.verdict = "profanity"
		result.matched = parsed.MatchedWord
		if result.matched == "" {
			result.matched = "unknown"
		}
	case "ai_generated":
		result.verdict = "ai_generated"
		conf := parsed.Confidence
		result.confidence = &conf
	default:
		result.verdict = "clean"
	}
	return result
}

// wordlistResult classifies with the wordlist only (classifier.rs
// wordlist_result).
func wordlistResult(text string, lyrics *string, provider string) *classifierResult {
	if word := profanityCheck(text); word != "" {
		return &classifierResult{verdict: "profanity", provider: provider, detail: "profanity in title/artist (wordlist)", matched: word}
	}
	if lyrics != nil {
		if word := profanityCheck(*lyrics); word != "" {
			return &classifierResult{verdict: "profanity", provider: provider, detail: "profanity in lyrics (wordlist)", matched: word}
		}
	}
	return &classifierResult{verdict: "clean", provider: provider, detail: "no profanity found (wordlist)"}
}

// classifyTrack classifies one track's text (classifier.rs
// classify_track). Order: Gemini (key set) -> wordlist fallback
// (Gemini failed) -> wordlist only (no key). Without a key AND
// without lyrics, the AI verdict is honestly Unknown.
func (s *Service) classifyTrack(ctx context.Context, title, artist string, lyrics *string) *classifierResult {
	text := strings.TrimSpace(title + " " + artist)
	if s.geminiKey() != "" {
		if r := s.classifyWithGemini(ctx, title, artist, lyrics); r != nil {
			return r
		}
		// Gemini failed: fall back to the wordlist, and say so.
		return wordlistResult(text, lyrics, "wordlist-fallback")
	}
	if lyrics == nil {
		// No classifier, no lyrics: profanity can still be judged on
		// title/artist, but AI-ness cannot — report unknown honestly.
		if word := profanityCheck(text); word != "" {
			return &classifierResult{
				verdict: "profanity", provider: "wordlist", matched: word,
				detail: "profanity in title/artist (wordlist); no lyrics and no Gemini key, so AI verdict is unknown",
			}
		}
		return &classifierResult{
			verdict: "unknown", provider: "none",
			detail: "no classifier configured and no lyrics fetched",
		}
	}
	return wordlistResult(text, lyrics, "wordlist")
}

// analysisText renders the one-sentence analysis for spotify.analyze
// (spotify-rust analysis_text: "Verdict: <verdict> (<provider>) —
// <detail>").
func analysisText(r *classifierResult) string {
	return fmt.Sprintf("Verdict: %s (%s) — %s", r.verdict, r.provider, r.detail)
}

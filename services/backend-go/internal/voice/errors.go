// Play/join error classification (queue-port Tier 2, items 13 + 16).
//
// One classifier, shared by the voice join path (Manager.Join) and
// the bind path (handlers.bindTrack / advancePlaybackMode) — the two
// refs disagree on a few classes, and the table below is the
// reconciliation, decided once here so the two paths can never
// drift apart:
//
//   - TgMusicBot (internal/calls/playback.go: classifyError,
//     rotateAndPlay) classifies join/play failures as fatal
//     (return immediately) / retry-once / rotate (flood, frozen,
//     CHANNELS_TOO_MUCH, deactivated) / unknown, tries the next
//     untried assistant, and evicts the bad one.
//   - Parvati (bot/src/modules/play.rs:105 handle_play_attempt_error,
//     :216) retries FLOOD_WAIT after sleeping, drops the room on
//     connection timeout / RTMP / GROUPCALL_INVALID, and retries
//     INTERDC_* after sleeping 2 s, up to 3 attempts with rollback.
//
// Reconciled policy (the classifier returns the class; the caller
// owns the bounded loop, so a retryable error still ends the
// session when its retries are spent — "retry once, then drop"
// satisfies both refs):
//
//   - Fatal: the call cannot be joined or played by this account in
//     this chat at all (GROUPCALL_FORBIDDEN, GROUPCALL_INVALID,
//     GROUPCALL_NOT_FOUND, RTMP, a ban/admin error, or the voice
//     call already being gone). Return immediately; the bind path
//     ends the session instead of draining the queue.
//   - Rotate: the assistant account itself is the problem (flood
//     with a long wait, frozen, CHANNELS_TOO_MUCH, deactivated,
//     session revoked). Evict that assistant and try the next
//     healthy one; with one assistant that means "no healthy
//     assistant available" — an honest failure, not a silent one.
//     A short FLOOD_WAIT (<= 60 s) is Retryable instead: the wait
//     is bounded and sleeping beats evicting the only assistant
//     for a minute-long flood.
//   - Retryable: transient (INTERDC_*, timeouts, connection reset,
//     short FLOOD_WAIT). Sleep RetryDelay, then retry, bounded by
//     MaxAttempts (3 — both refs' attempt budget).
//   - Unknown: anything else. No retry; the caller surfaces the
//     error (the bind path advances past the track).
//
// The classifier works on error strings: TDLib error text arrives
// through the JoinFunc (the core's joinVideoChat error, e.g.
// "400 GROUPCALL_INVALID") and ntgcalls errors arrive as library
// text, so substring matching on the upper-cased message is the
// only seam both paths share. Matching is deliberately
// case-insensitive and substring-based — TDLib prefixes its codes
// with a numeric code ("400 ...", "420 FLOOD_WAIT_..."), and the
// wrapper layers add their own prefixes ("voice: join chat ...:
// join: ..."), so an exact match would never fire.
package voice

import (
	"regexp"
	"strings"
	"time"
)

// MaxAttempts is the attempt budget for one join (Manager.Join) and
// one bind (handlers.bindTrack): the first try plus retries, never
// more. Both refs use 3 (Parvati's 3-attempt retry loop; Tier 1's
// maxBindAttempts); one constant keeps the two paths in agreement.
const MaxAttempts = 3

// ErrorClass is what the classifier decided about one failure.
type ErrorClass int

const (
	// ClassUnknown means the error matched no rule: no retry, surface it.
	ClassUnknown ErrorClass = iota
	// ClassFatal means the call cannot be joined/played by this
	// account in this chat: return immediately, end the session.
	ClassFatal
	// ClassRetryable means the failure is transient: sleep
	// RetryDelay, then retry within MaxAttempts.
	ClassRetryable
	// ClassRotate means the assistant account itself is bad:
	// evict it and try the next healthy assistant.
	ClassRotate
)

// String renders the class for logs.
func (c ErrorClass) String() string {
	switch c {
	case ClassFatal:
		return "fatal"
	case ClassRetryable:
		return "retryable"
	case ClassRotate:
		return "rotate"
	default:
		return "unknown"
	}
}

// floodWaitRe parses a TDLib FLOOD_WAIT_<seconds> code.
var floodWaitRe = regexp.MustCompile(`FLOOD_WAIT_(\d+)`)

// rotateSubstrings are the TDLib/account errors that mean "this
// assistant account is the problem, try another one" (TgMusicBot's
// rotate class: flood, frozen, CHANNELS_TOO_MUCH, deactivated).
var rotateSubstrings = []string{
	"CHANNELS_TOO_MUCH",
	"FROZEN",
	"DEACTIVATED",
	"USER_DEACTIVATED",
	"SESSION_REVOKED",
	"AUTH_KEY_UNREGISTERED",
}

// fatalSubstrings are the errors that mean "this account cannot
// join/play this call at all" (TgMusicBot's fatal class plus
// Parvati's drop-the-room class: GROUPCALL_INVALID, RTMP,
// connection timeout is Retryable here — see Classify).
var fatalSubstrings = []string{
	"GROUPCALL_FORBIDDEN",
	"GROUPCALL_INVALID",
	"GROUPCALL_NOT_FOUND",
	"RTMP",
	"USER_BANNED_IN_CHANNEL",
	"CHAT_ADMIN_REQUIRED",
	// The voice call itself is gone (bindTrack's loud failure,
	// startStream's no-connected-call error): there is no call
	// left to retry against — end the session.
	"NO ACTIVE VOICE CALL",
	"NO CONNECTED CALL",
}

// retryableSubstrings are the transient failures worth one more
// attempt after a short sleep (Parvati's INTERDC_* + 2 s sleep;
// TgMusicBot's retry-once class: participants race, timeout).
var retryableSubstrings = []string{
	"INTERDC",
	"TIMEOUT",
	"TIMED OUT",
	"DEADLINE EXCEEDED",
	"CONNECTION RESET",
	"CONNECTION REFUSED",
	"I/O TIMEOUT",
}

// longFloodWaitSec is the split between "sleep it out" and
// "rotate": a flood wait at or under this long is Retryable (the
// wait is bounded and evicting the only assistant for it would
// be worse); longer, and the account is unusable for so long
// that rotating to the next assistant is the right call
// (TgMusicBot rotates on flood; Parvati sleeps — both, split by
// length).
const longFloodWaitSec = 60

// Classify returns the class of err. A nil error is Unknown (the
// caller never classifies a success, but a nil must not panic).
func Classify(err error) ErrorClass {
	if err == nil {
		return ClassUnknown
	}
	msg := strings.ToUpper(err.Error())
	// FLOOD_WAIT first: its class depends on the wait length.
	if m := floodWaitRe.FindStringSubmatch(msg); m != nil {
		secs := 0
		for _, r := range m[1] {
			secs = secs*10 + int(r-'0')
		}
		if secs > longFloodWaitSec {
			return ClassRotate
		}
		return ClassRetryable
	}
	if strings.Contains(msg, "FLOOD_WAIT") {
		// No parseable wait: assume short, sleep the default.
		return ClassRetryable
	}
	for _, s := range rotateSubstrings {
		if strings.Contains(msg, s) {
			return ClassRotate
		}
	}
	for _, s := range fatalSubstrings {
		if strings.Contains(msg, s) {
			return ClassFatal
		}
	}
	for _, s := range retryableSubstrings {
		if strings.Contains(msg, s) {
			return ClassRetryable
		}
	}
	return ClassUnknown
}

// maxRetryDelay caps a FLOOD_WAIT sleep: a flood wait longer than
// this is classified Rotate, not Retryable, so this cap only
// bites on unparseable waits — but a retry sleep must never hold
// a join/bind past its context deadline by itself, so the caller
// also sleeps ctx-aware (sleepCtx) and aborts on cancellation.
const maxRetryDelay = 30 * time.Second

// RetryDelay returns how long the caller should sleep before
// retrying err: the parsed FLOOD_WAIT seconds (capped), 2 s for
// INTERDC_* (Parvati's rule), 2 s for other retryables. It is
// only meaningful for ClassRetryable errors; for any other class
// it returns 0.
func RetryDelay(err error) time.Duration {
	if Classify(err) != ClassRetryable {
		return 0
	}
	msg := strings.ToUpper(err.Error())
	if m := floodWaitRe.FindStringSubmatch(msg); m != nil {
		secs := 0
		for _, r := range m[1] {
			secs = secs*10 + int(r-'0')
		}
		d := time.Duration(secs) * time.Second
		if d > maxRetryDelay {
			d = maxRetryDelay
		}
		return d
	}
	return 2 * time.Second
}

// Package voice is the Go binding to libntgcalls.so 3.0.5 and the per-chat
// call lifecycle manager for the Parlay unified backend.
//
// The binding follows the proven TgMusicBot cgo pattern
// (refs/TgMusicBot/main.go cgo flags, refs/TgMusicBot/ntgcalls/ntgcalls.go
// callback registration), with one correction: TgMusicBot's MediaSource
// constants use 1<<iota, which diverges from the C enum for every value
// after Shell (C: Unknown=0 File=1 Shell=2 FFmpeg=3 Device=4 Desktop=5
// External=6; TgMusicBot: FFmpeg=4 Device=8 Desktop=16 External=32).
// This package maps every enum value to its C constant explicitly, so
// the Go values always match refs/ntgcalls/shared-output/include/ntgcalls.h.
//
// Every symbol bound here was verified present in the library with
// `nm -D` (see symbols_test.go, which re-checks at test time, and
// phase3/voice.md for the full list). The cgo link itself also fails
// loudly if a referenced symbol is absent.
//
// This package never calls TDLib and never touches the UDS socket. The
// voice join handshake is completed through the JoinFunc supplied to
// the Manager; Wave 2 wires that function to the UDS server's
// JoinRequest (contracts/uds-protocol.md §6).
package voice

/*
// Link note: -Wl,--allow-shlib-undefined is required on this host
// (glibc 2.39). The prebuilt libntgcalls.so carries unversioned
// undefined references to __dn_expand and __res_nquery, which lived
// in libresolv on the glibc it was built against; on glibc 2.39 they
// live in libc, and ld refuses to leave the shared library's
// references unresolved at static link time even though the runtime
// loader resolves them from libc without complaint (verified: a C
// probe linked with this flag runs and prints 3.0.5). The flag only
// permits undefined symbols inside shared libraries; it does not
// weaken the link for the Go objects themselves.
#cgo linux LDFLAGS: -L${SRCDIR}/../../../../refs/ntgcalls/shared-output/lib -lntgcalls -lm -lz -lresolv -Wl,--allow-shlib-undefined -Wl,-rpath,${SRCDIR}/../../../../refs/ntgcalls/shared-output/lib
#cgo CFLAGS: -I${SRCDIR}/../../../../refs/ntgcalls/shared-output/include
#include "ntgcalls.h"
#include <stdlib.h>

extern void handleVoiceStreamEnd(ntg_instance* handle, int64_t chatID, ntg_stream_type streamType, ntg_stream_device streamDevice, void* userData);
extern void handleVoiceConnectionChange(ntg_instance* handle, int64_t chatID, ntg_connection_info state, void* userData);
*/
import "C"

import (
	"errors"
	"fmt"
	"sync"
	"unsafe"
)

// StreamType is the ntgcalls stream type (audio or video).
type StreamType int

const (
	// StreamTypeAudio is an audio stream.
	StreamTypeAudio StreamType = iota
	// StreamTypeVideo is a video stream.
	StreamTypeVideo
)

// StreamDevice is the ntgcalls stream device.
type StreamDevice int

const (
	// DeviceMicrophone is the microphone (outgoing audio) device.
	DeviceMicrophone StreamDevice = iota
	// DeviceSpeaker is the speaker (incoming audio) device.
	DeviceSpeaker
	// DeviceCamera is the camera device.
	DeviceCamera
	// DeviceScreen is the screen-share device.
	DeviceScreen
)

// StreamMode is the ntgcalls stream direction.
type StreamMode int

const (
	// StreamModeCapture is the capture (outgoing) direction.
	StreamModeCapture StreamMode = iota
	// StreamModePlayback is the playback (incoming) direction.
	StreamModePlayback
)

// MediaSource is the ntgcalls media source kind. The values match the
// ntg_media_source enum in ntgcalls.h exactly.
type MediaSource int

const (
	// MediaSourceUnknown is an unset media source.
	MediaSourceUnknown MediaSource = iota
	// MediaSourceFile reads raw bytes from a local file. The reader
	// performs no decoding and no HTTP: the file must already be raw
	// PCM in the format declared in AudioDescription.
	MediaSourceFile
	// MediaSourceShell runs a shell command and reads raw PCM from
	// its stdout.
	MediaSourceShell
	// MediaSourceFFmpeg is rejected by the library: the media source
	// factory throws FFmpegError ("FFmpeg encoder is not yet
	// supported"). Never use it.
	MediaSourceFFmpeg
	// MediaSourceDevice captures from a hardware device.
	MediaSourceDevice
	// MediaSourceDesktop captures the desktop.
	MediaSourceDesktop
	// MediaSourceExternal receives raw PCM frames pushed by the
	// caller through SendExternalFrame. This is the source the
	// interim frame path uses (see feeder.go).
	MediaSourceExternal
)

// ConnectionState is the ntgcalls connection state.
type ConnectionState int

const (
	// ConnectionConnecting is the initial state.
	ConnectionConnecting ConnectionState = iota
	// ConnectionConnected is the live state.
	ConnectionConnected
	// ConnectionFailed is a terminal failure.
	ConnectionFailed
	// ConnectionTimeout is a terminal timeout.
	ConnectionTimeout
	// ConnectionClosed is a terminal close.
	ConnectionClosed
)

// ConnectionKind distinguishes a normal call from a presentation.
type ConnectionKind int

const (
	// ConnectionKindNormal is a normal voice call.
	ConnectionKindNormal ConnectionKind = iota
	// ConnectionKindPresentation is a presentation (screen-share) call.
	ConnectionKindPresentation
)

// AudioDescription describes one audio source for SetStreamSources.
// It mirrors ntg_audio_description in ntgcalls.h.
type AudioDescription struct {
	MediaSource  MediaSource
	Input        string
	SampleRate   uint32
	ChannelCount uint8
	KeepOpen     bool
}

// MediaDescription describes the audio (and, unused here, video)
// sources for SetStreamSources. It mirrors ntg_media_description.
type MediaDescription struct {
	Microphone *AudioDescription
	Speaker    *AudioDescription
}

// MediaState is the per-call mute/pause state reported by ntgcalls.
type MediaState struct {
	Muted               bool
	VideoPaused         bool
	VideoStopped        bool
	PresentationPaused  bool
	PresentationStopped bool
}

// StreamEndFunc is called when a stream ends in a chat.
type StreamEndFunc func(chatID int64, streamType StreamType, device StreamDevice)

// ConnectionChangeFunc is called when a call's connection state changes.
type ConnectionChangeFunc func(chatID int64, state ConnectionState, kind ConnectionKind)

// Client is one ntgcalls instance: a cgo binding to libntgcalls.so.
// A Client is safe for concurrent use; ntgcalls calls are synchronous.
type Client struct {
	mu                       sync.Mutex
	handle                   *C.ntg_instance
	closed                   bool
	streamEndHandlers        []StreamEndFunc
	connectionChangeHandlers []ConnectionChangeFunc
	handlersMu               sync.RWMutex
}

func (c *Client) copyStreamEndHandlers() []StreamEndFunc {
	c.handlersMu.RLock()
	defer c.handlersMu.RUnlock()
	out := make([]StreamEndFunc, len(c.streamEndHandlers))
	copy(out, c.streamEndHandlers)
	return out
}

func (c *Client) copyConnectionChangeHandlers() []ConnectionChangeFunc {
	c.handlersMu.RLock()
	defer c.handlersMu.RUnlock()
	out := make([]ConnectionChangeFunc, len(c.connectionChangeHandlers))
	copy(out, c.connectionChangeHandlers)
	return out
}

// New creates a ntgcalls instance and registers the stream-end and
// connection-change callbacks on it.
func New() (*Client, error) {
	handle := C.ntg_instance_create()
	if handle == nil {
		return nil, errors.New("voice: ntg_instance_create returned nil")
	}
	c := &Client{handle: handle}
	registerClient(c)
	if rc := C.ntg_on_stream_end_callback(handle, (C.ntg_stream_end_callback_cb)(unsafe.Pointer(C.handleVoiceStreamEnd)), nil); rc != C.NTG_OK {
		c.Close()
		return nil, fmt.Errorf("voice: ntg_on_stream_end_callback: %w", resultError(rc))
	}
	if rc := C.ntg_on_connection_change_callback(handle, (C.ntg_connection_change_callback_cb)(unsafe.Pointer(C.handleVoiceConnectionChange)), nil); rc != C.NTG_OK {
		c.Close()
		return nil, fmt.Errorf("voice: ntg_on_connection_change_callback: %w", resultError(rc))
	}
	return c, nil
}

// Version returns the linked libntgcalls version string.
func Version() string {
	p := C.ntg_get_version()
	if p == nil {
		return ""
	}
	return C.GoString(p)
}

// Close destroys the ntgcalls instance. It is safe to call more than once.
func (c *Client) Close() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed || c.handle == nil {
		return
	}
	unregisterClient(c)
	C.ntg_instance_destroy(c.handle)
	c.handle = nil
	c.closed = true
}

// OnStreamEnd registers a handler invoked when a stream ends.
func (c *Client) OnStreamEnd(fn StreamEndFunc) {
	c.handlersMu.Lock()
	defer c.handlersMu.Unlock()
	c.streamEndHandlers = append(c.streamEndHandlers, fn)
}

// OnConnectionChange registers a handler invoked on connection changes.
func (c *Client) OnConnectionChange(fn ConnectionChangeFunc) {
	c.handlersMu.Lock()
	defer c.handlersMu.Unlock()
	c.connectionChangeHandlers = append(c.connectionChangeHandlers, fn)
}

// resultError converts an ntg_result into a Go error, preferring the
// library's own message from ntg_last_error().
func resultError(rc C.ntg_result) error {
	if rc == C.NTG_OK {
		return nil
	}
	if p := C.ntg_last_error(); p != nil {
		if msg := C.GoString(p); msg != "" {
			return fmt.Errorf("ntgcalls error %d: %s", int32(rc), msg)
		}
	}
	return fmt.Errorf("ntgcalls error %d", int32(rc))
}

// checkResult converts an ntg_result into a Go error. The error
// carries only the library's own reason; the caller names the
// operation and the stage, so a join failure reads as e.g.
// "voice: join chat …: connect: ntgcalls error -102: Invalid JSON"
// with each name appearing exactly once.
func checkResult(rc C.ntg_result) error {
	if rc == C.NTG_OK {
		return nil
	}
	return resultError(rc)
}

// CreateCall creates a call for chatID and returns the local
// connection parameters. Despite the "SDP offer" shorthand used by
// the UDS contract (local_sdp) and by older code, the value is NOT
// SDP text: it is ntgcalls' JSON params object — {"ufrag", "pwd",
// "fingerprints": [{"hash": "sha-256", "setup": "passive",
// "fingerprint": ...}], "ssrc", "ssrc-groups": [...]} — which the
// core passes verbatim to TDLib joinVideoChat as params. Callers
// must not parse it as SDP and must not reformat it.
func (c *Client) CreateCall(chatID int64) (string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return "", errors.New("voice: CreateCall on closed client")
	}
	var buf *C.char
	if err := checkResult(C.ntg_create_call(c.handle, C.int64_t(chatID), &buf)); err != nil {
		return "", err
	}
	if buf == nil {
		return "", errors.New("voice: create_call returned no SDP offer")
	}
	defer C.ntg_string_free(buf)
	offer := C.GoString(buf)
	if offer == "" {
		return "", errors.New("voice: create_call returned an empty SDP offer")
	}
	return offer, nil
}

// Connect completes the handshake for chatID with the remote SDP
// (the transport parameters returned by the TDLib join).
func (c *Client) Connect(chatID int64, remoteSDP string) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return errors.New("voice: Connect on closed client")
	}
	params := C.CString(remoteSDP)
	defer C.free(unsafe.Pointer(params))
	return checkResult(C.ntg_connect(c.handle, C.int64_t(chatID), params, C.bool(false)))
}

// SetStreamSources binds the media sources described by desc to chatID
// in the given direction.
func (c *Client) SetStreamSources(chatID int64, mode StreamMode, desc MediaDescription) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return errors.New("voice: SetStreamSources on closed client")
	}
	descC, freeDesc := desc.toC()
	defer freeDesc()
	return checkResult(C.ntg_set_stream_sources(c.handle, C.int64_t(chatID), mode.toC(), descC))
}

// SetExternalAudioSource binds the microphone of chatID to the external
// source: raw PCM frames pushed by the caller through
// SendExternalFrame. The declared format is 48 kHz stereo s16le, one
// 10 ms frame per SendExternalFrame call (see FrameBytes in feeder.go).
func (c *Client) SetExternalAudioSource(chatID int64) error {
	return c.SetStreamSources(chatID, StreamModeCapture, MediaDescription{
		Microphone: &AudioDescription{
			MediaSource:  MediaSourceExternal,
			SampleRate:   48000,
			ChannelCount: 2,
			KeepOpen:     false,
		},
	})
}

// SendExternalFrame pushes one raw PCM audio frame to chatID's
// microphone (external) source. data must be exactly FrameBytes
// (1920 bytes: 480 samples, 10 ms at 48 kHz stereo s16le); ntgcalls
// derives the sample count from the sink frame size, so a short or
// long buffer would be silently truncated or over-read. Call
// SetExternalAudioSource first: without an external source bound,
// ntgcalls rejects the frame with "External source not initialized".
func (c *Client) SendExternalFrame(chatID int64, data []byte) error {
	if len(data) != FrameBytes {
		return fmt.Errorf("voice: SendExternalFrame: frame must be exactly %d bytes, got %d", FrameBytes, len(data))
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return errors.New("voice: SendExternalFrame on closed client")
	}
	dataC := (*C.uint8_t)(unsafe.Pointer(&data[0]))
	return checkResult(C.ntg_send_external_frame(c.handle, C.int64_t(chatID), C.NTG_STREAM_DEVICE_MICROPHONE, dataC, C.size_t(len(data)), C.ntg_frame_data{}))
}

// SendFrame pushes one raw PCM audio frame to chatID's
// microphone (external) source. It is the FrameSink
// implementation: the DSP feed pump calls this, never
// SendExternalFrame directly, so the sink stays one delimited
// seam (see feed.go).
func (c *Client) SendFrame(chatID int64, data []byte) error {
	return c.SendExternalFrame(chatID, data)
}

// Pause pauses playback in chatID. The returned bool reports whether
// any track's state actually changed, not the resulting paused state:
// ntgcalls returns the "changed" flag from its update_pause, which
// only touches file/shell readers. For the external source this
// package uses, the library-level pause is a no-op — the Manager
// pauses the decode feed instead (see feeder.go).
func (c *Client) Pause(chatID int64) (bool, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return false, errors.New("voice: Pause on closed client")
	}
	var out C.bool
	if err := checkResult(C.ntg_pause(c.handle, C.int64_t(chatID), &out)); err != nil {
		return false, err
	}
	return bool(out), nil
}

// Resume resumes playback in chatID. The returned bool reports
// whether any track's state actually changed (see Pause).
func (c *Client) Resume(chatID int64) (bool, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return false, errors.New("voice: Resume on closed client")
	}
	var out C.bool
	if err := checkResult(C.ntg_resume(c.handle, C.int64_t(chatID), &out)); err != nil {
		return false, err
	}
	return bool(out), nil
}

// Mute mutes playback in chatID. The returned bool reports whether
// any track's state actually changed. Unlike pause, mute works for
// the external source: it toggles the outgoing track's enabled state.
func (c *Client) Mute(chatID int64) (bool, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return false, errors.New("voice: Mute on closed client")
	}
	var out C.bool
	if err := checkResult(C.ntg_mute(c.handle, C.int64_t(chatID), &out)); err != nil {
		return false, err
	}
	return bool(out), nil
}

// Unmute unmutes playback in chatID. The returned bool reports
// whether any track's state actually changed (see Mute).
func (c *Client) Unmute(chatID int64) (bool, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return false, errors.New("voice: Unmute on closed client")
	}
	var out C.bool
	if err := checkResult(C.ntg_unmute(c.handle, C.int64_t(chatID), &out)); err != nil {
		return false, err
	}
	return bool(out), nil
}

// Stop ends the call in chatID.
func (c *Client) Stop(chatID int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return errors.New("voice: Stop on closed client")
	}
	return checkResult(C.ntg_stop(c.handle, C.int64_t(chatID)))
}

// GetState returns the mute/pause state of the call in chatID.
func (c *Client) GetState(chatID int64) (MediaState, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return MediaState{}, errors.New("voice: GetState on closed client")
	}
	var buf C.ntg_media_state
	if err := checkResult(C.ntg_get_state(c.handle, C.int64_t(chatID), &buf)); err != nil {
		return MediaState{}, err
	}
	defer C.ntg_media_state_free(&buf)
	return MediaState{
		Muted:               bool(buf.muted),
		VideoPaused:         bool(buf.video_paused),
		VideoStopped:        bool(buf.video_stopped),
		PresentationPaused:  bool(buf.presentation_paused),
		PresentationStopped: bool(buf.presentation_stopped),
	}, nil
}

// Time returns the playback clock of chatID in milliseconds.
func (c *Client) Time(chatID int64) (uint64, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return 0, errors.New("voice: Time on closed client")
	}
	var out C.uint64_t
	if err := checkResult(C.ntg_time(c.handle, C.int64_t(chatID), C.NTG_STREAM_MODE_CAPTURE, &out)); err != nil {
		return 0, err
	}
	return uint64(out), nil
}

func (m StreamMode) toC() C.ntg_stream_mode {
	switch m {
	case StreamModePlayback:
		return C.NTG_STREAM_MODE_PLAYBACK
	default:
		return C.NTG_STREAM_MODE_CAPTURE
	}
}

func (s MediaSource) toC() C.ntg_media_source {
	switch s {
	case MediaSourceFile:
		return C.NTG_MEDIA_SOURCE_FILE
	case MediaSourceShell:
		return C.NTG_MEDIA_SOURCE_SHELL
	case MediaSourceFFmpeg:
		return C.NTG_MEDIA_SOURCE_FFMPEG
	case MediaSourceDevice:
		return C.NTG_MEDIA_SOURCE_DEVICE
	case MediaSourceDesktop:
		return C.NTG_MEDIA_SOURCE_DESKTOP
	case MediaSourceExternal:
		return C.NTG_MEDIA_SOURCE_EXTERNAL
	default:
		return C.NTG_MEDIA_SOURCE_UNKNOWN
	}
}

func (d *AudioDescription) toC() (C.ntg_audio_description, func()) {
	var x C.ntg_audio_description
	x.media_source = d.MediaSource.toC()
	x.input = C.CString(d.Input)
	x.sample_rate = C.uint32_t(d.SampleRate)
	x.channel_count = C.uint8_t(d.ChannelCount)
	x.keep_open = C.bool(d.KeepOpen)
	return x, func() { C.free(unsafe.Pointer(x.input)) }
}

func (d *MediaDescription) toC() (C.ntg_media_description, func()) {
	var x C.ntg_media_description
	var cleanups []func()
	if d.Microphone != nil {
		value, cleanup := d.Microphone.toC()
		pointer := (*C.ntg_audio_description)(C.malloc(C.size_t(unsafe.Sizeof(value))))
		*pointer = value
		x.microphone = pointer
		cleanups = append(cleanups, cleanup, func() { C.free(unsafe.Pointer(pointer)) })
	}
	if d.Speaker != nil {
		value, cleanup := d.Speaker.toC()
		pointer := (*C.ntg_audio_description)(C.malloc(C.size_t(unsafe.Sizeof(value))))
		*pointer = value
		x.speaker = pointer
		cleanups = append(cleanups, cleanup, func() { C.free(unsafe.Pointer(pointer)) })
	}
	return x, func() {
		for _, cleanup := range cleanups {
			cleanup()
		}
	}
}

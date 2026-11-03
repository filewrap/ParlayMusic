package voice

/*
#include "ntgcalls.h"
#include <stdlib.h>
*/
import "C"

import (
	"sync"
	"unsafe"
)

// clients maps a live ntg_instance handle to its Go Client so the
// exported C callbacks below can find the right instance. Callbacks
// arrive on ntgcalls threads; handlers are invoked from those threads
// and must not block, so the Manager dispatches its own work to a
// goroutine (see manager.go).
var (
	clientsMu sync.RWMutex
	clients   = make(map[*C.ntg_instance]*Client)
)

func registerClient(c *Client) {
	clientsMu.Lock()
	defer clientsMu.Unlock()
	clients[c.handle] = c
}

func unregisterClient(c *Client) {
	clientsMu.Lock()
	defer clientsMu.Unlock()
	delete(clients, c.handle)
}

func lookupClient(handle *C.ntg_instance) *Client {
	clientsMu.RLock()
	defer clientsMu.RUnlock()
	return clients[handle]
}

func parseStreamType(t C.ntg_stream_type) StreamType {
	if t == C.NTG_STREAM_TYPE_AUDIO {
		return StreamTypeAudio
	}
	return StreamTypeVideo
}

func parseStreamDevice(d C.ntg_stream_device) StreamDevice {
	switch d {
	case C.NTG_STREAM_DEVICE_SPEAKER:
		return DeviceSpeaker
	case C.NTG_STREAM_DEVICE_CAMERA:
		return DeviceCamera
	case C.NTG_STREAM_DEVICE_SCREEN:
		return DeviceScreen
	default:
		return DeviceMicrophone
	}
}

func parseConnectionState(s C.ntg_connection_state) ConnectionState {
	switch s {
	case C.NTG_CONNECTION_STATE_CONNECTED:
		return ConnectionConnected
	case C.NTG_CONNECTION_STATE_FAILED:
		return ConnectionFailed
	case C.NTG_CONNECTION_STATE_TIMEOUT:
		return ConnectionTimeout
	case C.NTG_CONNECTION_STATE_CLOSED:
		return ConnectionClosed
	default:
		return ConnectionConnecting
	}
}

func parseConnectionKind(k C.ntg_connection_kind) ConnectionKind {
	if k == C.NTG_CONNECTION_KIND_PRESENTATION {
		return ConnectionKindPresentation
	}
	return ConnectionKindNormal
}

//export handleVoiceStreamEnd
func handleVoiceStreamEnd(handle *C.ntg_instance, chatID C.int64_t, streamType C.ntg_stream_type, streamDevice C.ntg_stream_device, _ unsafe.Pointer) {
	self := lookupClient(handle)
	if self == nil {
		return
	}
	for _, fn := range self.copyStreamEndHandlers() {
		fn(int64(chatID), parseStreamType(streamType), parseStreamDevice(streamDevice))
	}
}

//export handleVoiceConnectionChange
func handleVoiceConnectionChange(handle *C.ntg_instance, chatID C.int64_t, info C.ntg_connection_info, _ unsafe.Pointer) {
	self := lookupClient(handle)
	if self == nil {
		return
	}
	for _, fn := range self.copyConnectionChangeHandlers() {
		fn(int64(chatID), parseConnectionState(info.state), parseConnectionKind(info.kind))
	}
}

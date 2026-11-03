//go:build !parlaydsp

package dsp

import (
	"context"
	"io"
)

// Linked reports whether this build links the real parlay-dsp
// library. Without -tags parlaydsp it is always false.
func Linked() bool { return false }

func newEngine() Engine { return unavailableEngine{} }

// unavailableEngine is the no-library engine for the default
// build. It is not a stub: it is the real, reporting
// implementation for "the DSP has not landed yet" — every
// OpenStream fails with ErrUnavailable so audio fails honestly
// instead of silently falling back to a deleted ffmpeg path.
type unavailableEngine struct{}

func (unavailableEngine) OpenStream(_ context.Context, _ io.ReadCloser, _ Options) (Stream, error) {
	return nil, ErrUnavailable
}

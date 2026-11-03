//go:build !parlaydsp

package dsp

import (
	"context"
	"errors"
	"io"
	"strings"
	"testing"
)

// TestUnavailableEngine proves the default build (no -tags
// parlaydsp) fails audio honestly: New returns an engine whose
// OpenStream always fails with ErrUnavailable, and Linked is
// false. With -tags parlaydsp this file is excluded and the cgo
// engine is used instead.
func TestUnavailableEngine(t *testing.T) {
	if Linked() {
		t.Fatal("Linked() = true in a build without -tags parlaydsp")
	}
	eng := New()
	if eng == nil {
		t.Fatal("New() returned nil")
	}
	_, err := eng.OpenStream(context.Background(), io.NopCloser(strings.NewReader("x")), DefaultOptions())
	if !errors.Is(err, ErrUnavailable) {
		t.Errorf("OpenStream err = %v, want ErrUnavailable", err)
	}
}

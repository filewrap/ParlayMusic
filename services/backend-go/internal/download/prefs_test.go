package download

import (
	"strings"
	"testing"
)

func TestDefaultPrefs(t *testing.T) {
	p := DefaultPrefs(123)
	if p.Format != "mp3" {
		t.Fatalf("default format = %q, want mp3", p.Format)
	}
	if !p.UseYouTube {
		t.Fatal("default use_youtube should be true")
	}
	if !strings.Contains(p.PathTemplate, "{ext}") {
		t.Fatalf("default template %q must contain {ext}", p.PathTemplate)
	}
	if p.UserID != 123 {
		t.Fatalf("user id = %d, want 123", p.UserID)
	}
}

func TestPrefsStoreGetDefaults(t *testing.T) {
	s := NewPrefsStore()
	p := s.Get(999)
	if p.Format != "mp3" || !p.UseYouTube {
		t.Fatalf("unknown user should get defaults, got %+v", p)
	}
}

func TestPrefsStoreSetValid(t *testing.T) {
	s := NewPrefsStore()
	saved, err := s.Set(Prefs{UserID: 7, Format: "flac", UseYouTube: false, PathTemplate: "{artist} - {title}.{ext}"})
	if err != nil {
		t.Fatalf("Set valid prefs: %v", err)
	}
	if saved.Format != "flac" || saved.UseYouTube {
		t.Fatalf("saved prefs = %+v", saved)
	}
	if got := s.Get(7); got.Format != "flac" {
		t.Fatalf("Get after Set = %+v", got)
	}
}

func TestPrefsStoreSetBadFormat(t *testing.T) {
	s := NewPrefsStore()
	for _, format := range []string{"MP3", "aac", "wma", "xyz"} {
		if _, err := s.Set(Prefs{UserID: 7, Format: format, UseYouTube: true, PathTemplate: "{title}.{ext}"}); err == nil {
			t.Fatalf("Set format %q should fail", format)
		}
	}
	// An empty format is not an error: Set coerces it to the mp3
	// default (GET-then-modify callers pass the full object).
	saved, err := s.Set(Prefs{UserID: 7, Format: "", UseYouTube: true, PathTemplate: "{title}.{ext}"})
	if err != nil {
		t.Fatalf("Set empty format should coerce to mp3: %v", err)
	}
	if saved.Format != "mp3" {
		t.Fatalf("empty format coerced to %q, want mp3", saved.Format)
	}
	for _, format := range []string{"mp3", "m4a", "flac", "opus", "ogg", "wav"} {
		if _, err := s.Set(Prefs{UserID: 7, Format: format, UseYouTube: true, PathTemplate: "{title}.{ext}"}); err != nil {
			t.Fatalf("Set format %q should pass: %v", format, err)
		}
	}
}

func TestPrefsStoreSetBadTemplate(t *testing.T) {
	s := NewPrefsStore()
	bad := []string{
		"{artist} - {title}", // no {ext}
		"a/b/{title}.{ext}",  // separator
		"..{title}.{ext}",    // ..
		"{title}.mp3",        // no {ext} placeholder
	}
	for _, tpl := range bad {
		if _, err := s.Set(Prefs{UserID: 7, Format: "mp3", UseYouTube: true, PathTemplate: tpl}); err == nil {
			t.Fatalf("Set template %q should fail", tpl)
		}
	}
	// An empty template coerces to the default template.
	saved, err := s.Set(Prefs{UserID: 7, Format: "mp3", UseYouTube: true, PathTemplate: ""})
	if err != nil {
		t.Fatalf("Set empty template should coerce to default: %v", err)
	}
	if saved.PathTemplate != DefaultPathTemplate {
		t.Fatalf("empty template coerced to %q, want %q", saved.PathTemplate, DefaultPathTemplate)
	}
	if _, err := s.Set(Prefs{UserID: 7, Format: "mp3", UseYouTube: true, PathTemplate: "{artist} - {title}.{ext}"}); err != nil {
		t.Fatalf("Set valid template should pass: %v", err)
	}
}

func TestApplyTemplate(t *testing.T) {
	got := applyTemplate("{artist} - {title} [{id}].{ext}", templateTrack{artistName: "Lata", titleName: "Lag Ja Gale", idName: "abc123"}, "mp3")
	if got != "Lata - Lag Ja Gale [abc123].mp3" {
		t.Fatalf("applyTemplate = %q", got)
	}
	// Unknown placeholders are left as-is (a typo surfaces, never
	// silently vanishes).
	got = applyTemplate("{artist} - {title} {typo}.{ext}", templateTrack{artistName: "A", titleName: "B", idName: "c"}, "mp3")
	if !strings.Contains(got, "{typo}") {
		t.Fatalf("applyTemplate should keep unknown placeholder, got %q", got)
	}
	// Unsafe characters are sanitised as a filename.
	got = applyTemplate("{title}.{ext}", templateTrack{artistName: "", titleName: "A/B: C?", idName: ""}, "mp3")
	if strings.ContainsAny(got, "/:") {
		t.Fatalf("applyTemplate should sanitise, got %q", got)
	}
}

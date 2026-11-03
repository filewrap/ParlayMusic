// [backend] wiring (wiring-audit Q10): the nine parsed-but-unused
// keys are wired here into the resolvers that read the same values
// from the environment today.
//
// Precedence is unchanged: config.Load merges parlay.toml < env
// overrides into cfg.Backend, so the values Configure receives ARE
// the effective values — a file key now does what the same-named
// env var always did. A Registry built without Configure keeps the
// old behaviour (each resolver reads its env var directly), so
// existing tests and ad-hoc use are unaffected.
//
// Two keys are NOT wired here because they have no consumer:
//   - slskd_user / slskd_pass: slskd auth is API-key only
//     (SLSKD_API_KEY); the fields were deleted from BackendConfig.
//   - ytdlp_path: the daemon is located via PARLAY_YTDLP_PYTHON /
//     PARLAY_YTDLP_WORKER (see ytdlp.go); the field was deleted.
//
// And one name trap is resolved: the Navidrome password env var is
// NAVIDROME_PASSWORD (what the resolver reads); config accepts
// NAVIDROME_PASS only as a legacy fallback (see config.applyEnv).
package resolver

import (
	"strings"

	"parlay/backend-go/internal/config"
)

// Configure wires the effective [backend] config into the
// resolvers. Non-empty values win over the resolver's env-var
// fallback; empty values leave the env fallback in place (so a
// zero BackendConfig changes nothing).
func (r *Registry) Configure(cfg config.BackendConfig) {
	if u := strings.TrimRight(strings.TrimSpace(cfg.SlskdURL), "/"); u != "" && r.soulseek != nil {
		r.soulseek.baseURL = u
	}
	if cfg.DeezerARL != "" && r.deezer != nil {
		r.deezer.arl = strings.TrimSpace(cfg.DeezerARL)
	}
	if cfg.NetEaseMusicU != "" && r.netease != nil {
		r.netease.SetMusicU(cfg.NetEaseMusicU)
	}
	if r.navidrome != nil {
		if v := strings.TrimRight(strings.TrimSpace(cfg.NavidromeURL), "/"); v != "" {
			r.navidrome.base = v
		}
		if v := strings.TrimSpace(cfg.NavidromeUser); v != "" {
			r.navidrome.user = v
		}
		if v := strings.TrimSpace(cfg.NavidromePass); v != "" {
			r.navidrome.password = v
		}
	}
}

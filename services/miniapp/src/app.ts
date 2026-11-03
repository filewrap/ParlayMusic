// Parlay Mini App — typed single-page lossless player
// (FINAL-ARCHITECTURE.md Part 4). No framework, no runtime
// dependencies. State comes from the backend's real /api/v1
// endpoints (the HTTP transport into the gui.* / vc.* UDS
// actions — see services/backend-go/internal/httpapi); controls
// POST to the same actions Telegram commands run. The audio
// element streams raw FLAC from /api/v1/stream/:chat_id/flac,
// which answers HTTP Range requests, while the voice chat runs
// on its own path — the two never share a socket or a decoder.
//
// State sync is a 2 s poll of /api/v1/now-playing. That is a
// deliberate, honest mechanism — not a placeholder: the locked
// spec's WebSocket/SSE instant sync is not implemented yet (see
// README.md), and polling the real state beats a fake live
// channel. No emojis anywhere in the UI (§7.3).

interface Track {
  id: string;
  title: string;
  artist?: string;
  source?: string;
  duration_sec?: number;
  stream_url?: string;
}

interface QueueItem {
  position: number;
  requested_by: number;
  track: Track;
}

interface NowPlaying {
  chat_id: number;
  track: Track | null;
  engine: string;
  loop: number;
  loop_count: number;
  paused: boolean;
  is_paused: boolean;
  is_muted: boolean;
  position_sec: number;
  queue: QueueItem[];
  shuffle: boolean;
  autoplay: boolean;
  autoplay_enabled: boolean;
  volume: number;
  speed: number;
  stream_url?: string;
}

interface ApiError {
  ok: false;
  error: string;
  code?: string;
}

const POLL_MS = 2000;

function el<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (node === null) {
    throw new Error(`missing element #${id}`);
  }
  return node as T;
}

function fmtTime(sec: number): string {
  if (!Number.isFinite(sec) || sec < 0) {
    return "0:00";
  }
  const m = Math.floor(sec / 60);
  const s = Math.floor(sec % 60);
  return `${m}:${s.toString().padStart(2, "0")}`;
}

/** chat_id: Telegram WebApp chat first, ?chat_id= second (dev). */
function resolveChatId(): number | null {
  const tgChat = window.Telegram?.WebApp?.initDataUnsafe?.chat?.id;
  if (typeof tgChat === "number" && tgChat !== 0) {
    return tgChat;
  }
  const param = new URLSearchParams(window.location.search).get("chat_id");
  if (param !== null) {
    const id = Number.parseInt(param, 10);
    if (Number.isFinite(id) && id !== 0) {
      return id;
    }
  }
  return null;
}

class Player {
  private readonly chatId: number;
  private readonly audio: HTMLAudioElement;
  private readonly titleEl: HTMLElement;
  private readonly artistEl: HTMLElement;
  private readonly statusEl: HTMLElement;
  private readonly posEl: HTMLElement;
  private readonly durEl: HTMLElement;
  private readonly queueEl: HTMLElement;
  private readonly playPauseBtn: HTMLButtonElement;
  private readonly skipBtn: HTMLButtonElement;
  private readonly streamBtn: HTMLButtonElement;
  private readonly volumeEl: HTMLInputElement;
  private readonly volumeValEl: HTMLElement;
  private readonly errorEl: HTMLElement;
  private currentTrackId: string | null = null;
  private streamUrl: string;
  private pollTimer: number | null = null;
  private volumeTimer: number | null = null;

  constructor(chatId: number) {
    this.chatId = chatId;
    this.audio = el<HTMLAudioElement>("audio");
    this.titleEl = el<HTMLElement>("np-title");
    this.artistEl = el<HTMLElement>("np-artist");
    this.statusEl = el<HTMLElement>("np-status");
    this.posEl = el<HTMLElement>("np-position");
    this.durEl = el<HTMLElement>("np-duration");
    this.queueEl = el<HTMLElement>("queue-list");
    this.playPauseBtn = el<HTMLButtonElement>("btn-play-pause");
    this.skipBtn = el<HTMLButtonElement>("btn-skip");
    this.streamBtn = el<HTMLButtonElement>("btn-stream");
    this.volumeEl = el<HTMLInputElement>("volume");
    this.volumeValEl = el<HTMLElement>("volume-value");
    this.errorEl = el<HTMLElement>("error");
    this.streamUrl = `/api/v1/stream/${chatId}/flac`;

    this.playPauseBtn.addEventListener("click", () => void this.onPlayPause());
    this.skipBtn.addEventListener("click", () => void this.onSkip());
    this.streamBtn.addEventListener("click", () => void this.onStream());
    this.volumeEl.addEventListener("input", () => this.onVolumeInput());
    this.audio.addEventListener("error", () => this.onAudioError());
  }

  start(): void {
    void this.refresh();
    this.pollTimer = window.setInterval(() => void this.refresh(), POLL_MS);
  }

  stop(): void {
    if (this.pollTimer !== null) {
      window.clearInterval(this.pollTimer);
      this.pollTimer = null;
    }
  }

  private api(path: string, init?: RequestInit): Promise<Response> {
    return fetch(path, init);
  }

  private async refresh(): Promise<void> {
    let res: Response;
    try {
      res = await this.api(`/api/v1/now-playing?chat_id=${this.chatId}`);
    } catch (err) {
      this.showError(`cannot reach the backend: ${String(err)}`);
      return;
    }
    if (res.status === 404) {
      this.renderIdle();
      return;
    }
    if (!res.ok) {
      this.showError(`now-playing failed: HTTP ${res.status}`);
      return;
    }
    const body = (await res.json()) as { now_playing: NowPlaying };
    this.render(body.now_playing);
  }

  private render(np: NowPlaying): void {
    this.clearError();
    const track = np.track;
    if (track === null) {
      this.renderIdle();
      return;
    }
    this.titleEl.textContent = track.title;
    this.artistEl.textContent = track.artist ?? track.source ?? "";
    this.statusEl.textContent = np.is_paused ? "Paused" : "Playing";
    this.posEl.textContent = fmtTime(np.position_sec);
    this.durEl.textContent = fmtTime(track.duration_sec ?? 0);
    this.playPauseBtn.textContent = np.is_paused ? "Resume" : "Pause";
    this.playPauseBtn.disabled = false;
    this.skipBtn.disabled = false;
    if (String(np.volume) !== this.volumeEl.value) {
      this.volumeEl.value = String(np.volume);
    }
    this.volumeValEl.textContent = String(np.volume);
    this.renderQueue(np.queue);

    // The audio element streams the current track's FLAC. The
    // src is (re)set only on a track change, so a poll never
    // restarts playback mid-track.
    if (track.id !== this.currentTrackId) {
      this.currentTrackId = track.id;
      this.audio.src = this.streamUrl;
      this.audio.load();
    }
  }

  private renderIdle(): void {
    this.clearError();
    this.titleEl.textContent = "Nothing playing";
    this.artistEl.textContent = "";
    this.statusEl.textContent = "Idle";
    this.posEl.textContent = "0:00";
    this.durEl.textContent = "0:00";
    this.playPauseBtn.textContent = "Pause";
    this.playPauseBtn.disabled = true;
    this.skipBtn.disabled = true;
    this.queueEl.textContent = "";
    this.currentTrackId = null;
    if (this.audio.src !== "") {
      this.audio.removeAttribute("src");
      this.audio.load();
    }
  }

  private renderQueue(queue: QueueItem[]): void {
    this.queueEl.textContent = "";
    if (queue.length === 0) {
      const empty = document.createElement("li");
      empty.className = "queue-empty";
      empty.textContent = "Queue is empty";
      this.queueEl.appendChild(empty);
      return;
    }
    for (const item of queue) {
      const li = document.createElement("li");
      li.className = "queue-item";
      const pos = document.createElement("span");
      pos.className = "queue-pos";
      pos.textContent = String(item.position + 1);
      const name = document.createElement("span");
      name.className = "queue-name";
      name.textContent = item.track.artist
        ? `${item.track.title} — ${item.track.artist}`
        : item.track.title;
      const dur = document.createElement("span");
      dur.className = "queue-dur";
      dur.textContent = fmtTime(item.track.duration_sec ?? 0);
      li.append(pos, name, dur);
      this.queueEl.appendChild(li);
    }
  }

  private async postAction(action: string, payload: Record<string, unknown>): Promise<NowPlaying | null> {
    let res: Response;
    try {
      res = await this.api(`/api/v1/${action}`, {
        body: JSON.stringify({ chat_id: this.chatId, ...payload }),
        headers: { "Content-Type": "application/json" },
        method: "POST",
      });
    } catch (err) {
      this.showError(`cannot reach the backend: ${String(err)}`);
      return null;
    }
    if (!res.ok) {
      const errBody = (await res.json().catch(() => null)) as ApiError | null;
      this.showError(errBody?.error ?? `HTTP ${res.status}`);
      return null;
    }
    const body = (await res.json()) as { now_playing: NowPlaying };
    return body.now_playing;
  }

  private async onPlayPause(): Promise<void> {
    const np = await this.postAction(this.isPaused() ? "resume" : "pause", {});
    if (np !== null) {
      this.render(np);
    }
  }

  private async onSkip(): Promise<void> {
    const np = await this.postAction("skip", {});
    if (np !== null) {
      this.render(np);
    }
  }

  /** Play the current track's FLAC in this browser. */
  private async onStream(): Promise<void> {
    this.clearError();
    if (this.currentTrackId === null) {
      this.showError("nothing playing in this chat");
      return;
    }
    if (this.audio.src === "") {
      this.audio.src = this.streamUrl;
      this.audio.load();
    }
    try {
      await this.audio.play();
    } catch (err) {
      this.showError(`FLAC playback failed: ${String(err)}`);
    }
  }

  private isPaused(): boolean {
    return this.playPauseBtn.textContent === "Resume";
  }

  private onVolumeInput(): void {
    const value = Number.parseInt(this.volumeEl.value, 10);
    if (!Number.isFinite(value)) {
      return;
    }
    this.volumeValEl.textContent = String(value);
    // Debounce: one POST per drag pause, not one per tick.
    if (this.volumeTimer !== null) {
      window.clearTimeout(this.volumeTimer);
    }
    this.volumeTimer = window.setTimeout(() => {
      void this.postAction("volume", { volume: value }).then((np) => {
        if (np !== null) {
          this.render(np);
        }
      });
    }, 300);
  }

  private onAudioError(): void {
    const err = this.audio.error;
    if (err === null || this.currentTrackId === null) {
      return;
    }
    // MEDIA_ERR_SRC_NOT_SUPPORTED also fires when the backend
    // has no FLAC for this chat (404 body on an audio src): say
    // so plainly instead of a bare media error code.
    this.showError(
      err.code === MediaError.MEDIA_ERR_SRC_NOT_SUPPORTED
        ? "no FLAC available for the current track"
        : `audio error ${err.code}`,
    );
  }

  private showError(msg: string): void {
    this.errorEl.textContent = msg;
    this.errorEl.hidden = false;
  }

  private clearError(): void {
    this.errorEl.textContent = "";
    this.errorEl.hidden = true;
  }
}

function main(): void {
  const tg = window.Telegram?.WebApp;
  if (tg !== undefined) {
    tg.ready();
    tg.expand();
  }
  const chatId = resolveChatId();
  const errorEl = el<HTMLElement>("error");
  if (chatId === null) {
    errorEl.textContent =
      "No chat to play for: open this Mini App from the Parlay bot, or add ?chat_id=<id> to the URL.";
    errorEl.hidden = false;
    return;
  }
  const player = new Player(chatId);
  player.start();
  window.addEventListener("beforeunload", () => player.stop());
}

main();

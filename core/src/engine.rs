//! Engine and session state: the runtime, HTTP client, config, and per-session
//! pinyin buffer with single-in-flight cancellation.

use crate::api;
use crate::config::Config;
use crate::ngram::NgramModel;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;
use tokio::runtime::{Handle, Runtime};
use tokio::sync::Notify;

/// Outcome handed back to the FFI layer's callback.
pub struct ConvertOutcome {
    pub request_id: u64,
    pub status: i32,
    pub text: String,
}

/// Build the terminal outcome, honouring supersession: if a newer request has
/// claimed the session (generation moved past `my_gen`), report DS_ERR_CANCELLED
/// regardless of how this request actually finished.
fn finalize(
    active_gen: &AtomicU64,
    my_gen: u64,
    request_id: u64,
    result: Result<String, api::ConvertError>,
) -> ConvertOutcome {
    if active_gen.load(Ordering::SeqCst) != my_gen {
        let e = api::ConvertError::Cancelled;
        return ConvertOutcome {
            request_id,
            status: e.status_code(),
            text: e.message(),
        };
    }
    match result {
        Ok(text) => ConvertOutcome {
            request_id,
            status: 0, // DS_OK
            text,
        },
        Err(e) => ConvertOutcome {
            request_id,
            status: e.status_code(),
            text: e.message(),
        },
    }
}

/// Shared, thread-safe engine state. One per process is typical; cheap to share
/// across sessions via `Arc`.
pub struct Engine {
    /// Handle to the shared Tokio runtime, which is *owned* by [`EngineHandle`].
    /// This is a `Handle`, not the `Runtime`: dropping the last `Arc<Engine>` —
    /// which can happen on a worker thread when an in-flight task finishes after
    /// the frontend freed the engine — must NOT run the runtime's (blocking)
    /// shutdown, because dropping a `Runtime` on one of its own worker threads
    /// panics. Dropping a `Handle` is harmless on any thread.
    rt: Handle,
    client: reqwest::Client,
    config: RwLock<Arc<Config>>,
    config_path: PathBuf,
    /// Local speculative-conversion model, trained from returned conversions and
    /// persisted to `ngram_path`. Guarded by its own lock so speculation reads
    /// don't contend with config reads.
    ngram: RwLock<NgramModel>,
    ngram_path: PathBuf,
}

/// The on-disk model lives next to the config file as `ngram.json`.
fn ngram_path_for(config_path: &Path) -> PathBuf {
    config_path
        .parent()
        .map(|p| p.join("ngram.json"))
        .unwrap_or_else(|| PathBuf::from("ngram.json"))
}

/// Owns the Tokio [`Runtime`] plus a strong reference to the shared [`Engine`].
/// `ds_engine_new` hands a pointer to this across the FFI; `ds_engine_free` drops
/// it on the *caller's* thread, so the runtime is always shut down off its own
/// worker threads (dropping a `Runtime` on a worker thread panics). Sessions and
/// in-flight tasks only ever hold an `Arc<Engine>` — which carries a runtime
/// `Handle`, not the `Runtime` — so whichever task happens to drop the last
/// `Arc<Engine>` can never trigger the runtime's shutdown.
pub struct EngineHandle {
    // Held only to own the runtime and shut it down on drop (never read directly;
    // tasks spawn through the `Handle` in `Engine`). NB: field declaration order is
    // drop order — `_rt` is dropped first, which cancels any in-flight tasks (their
    // futures are dropped, not awaited), then the shared-engine strong ref is freed.
    _rt: Runtime,
    engine: Arc<Engine>,
}

impl EngineHandle {
    pub fn new(config_path: Option<PathBuf>) -> Result<EngineHandle, String> {
        let config_path = config_path.unwrap_or_else(Config::default_path);
        let config = Config::load_or_create(&config_path)
            .map_err(|e| format!("failed to load config at {}: {e}", config_path.display()))?;

        let ngram_path = ngram_path_for(&config_path);
        // Fresh installs start from the embedded pretrained baseline; an existing
        // on-disk model (the user's accumulated learning) takes precedence.
        let ngram = NgramModel::load_or_pretrained(&ngram_path, config.ngram_order);

        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .map_err(|e| format!("failed to start runtime: {e}"))?;

        // One shared, connection-pooled client. We keep idle connections warm so
        // the rapid-fire incremental conversion requests (one per debounce as the
        // user types) reuse the same TCP+TLS connection instead of reconnecting.
        let client = reqwest::Client::builder()
            .user_agent(concat!("dsime/", env!("CARGO_PKG_VERSION")))
            .tcp_keepalive(Duration::from_secs(60))
            .pool_idle_timeout(Duration::from_secs(90))
            .build()
            .map_err(|e| format!("failed to build http client: {e}"))?;

        let engine = Arc::new(Engine {
            // Tasks spawn onto this handle; the Runtime itself stays in `rt`.
            rt: rt.handle().clone(),
            client,
            config: RwLock::new(Arc::new(config)),
            config_path,
            ngram: RwLock::new(ngram),
            ngram_path,
        });
        Ok(EngineHandle { _rt: rt, engine })
    }

    /// Borrow the shared engine — for the read-only `ds_engine_*` FFI accessors.
    pub fn engine(&self) -> &Engine {
        &self.engine
    }

    /// A fresh strong reference to the shared engine — for `ds_session_new`, so a
    /// session keeps the engine state alive independently of this handle.
    pub fn engine_arc(&self) -> Arc<Engine> {
        Arc::clone(&self.engine)
    }
}

impl Engine {
    /// A speculative local conversion of `pinyin`, or `None` when speculation is
    /// disabled or the local model cannot fully cover the input. The result is a
    /// best guess to paint immediately; the remote conversion supersedes it.
    pub fn speculate(&self, pinyin: &str) -> Option<String> {
        if !self.config_snapshot().speculative {
            return None;
        }
        let model = self.ngram.read().unwrap();
        if model.is_empty() {
            return None;
        }
        model.predict(pinyin)
    }

    /// Teach the local model that `pinyin` converted to `hanzi` (typically the
    /// sentence the provider just returned). No-op when speculation is disabled
    /// or the pair does not align (mixed/English input, length mismatch). Best-
    /// effort persistence: a write failure is ignored since the model is only a
    /// latency optimization.
    ///
    /// Each user-confirmed conversion is trained `USER_LEARN_WEIGHT` times so
    /// that a single observation overrides any competing seed-corpus bigram after
    /// just one use — the user's explicit feedback is more reliable than the
    /// background frequency data.
    pub fn learn(&self, pinyin: &str, hanzi: &str) {
        if !self.config_snapshot().speculative {
            return;
        }
        const USER_LEARN_WEIGHT: usize = 5;
        let learned = {
            let mut model = self.ngram.write().unwrap();
            let mut any = false;
            for _ in 0..USER_LEARN_WEIGHT {
                any |= model.learn(pinyin, hanzi);
            }
            any
        };
        if learned {
            let model = self.ngram.read().unwrap();
            let _ = model.save(&self.ngram_path);
        }
    }

    pub fn config_snapshot(&self) -> Arc<Config> {
        self.config.read().unwrap().clone()
    }

    pub fn config_path(&self) -> &PathBuf {
        &self.config_path
    }

    pub fn debounce_ms(&self) -> u32 {
        self.config_snapshot().debounce_ms
    }

    pub fn get_config_json(&self) -> Result<String, String> {
        serde_json::to_string_pretty(&*self.config_snapshot()).map_err(|e| e.to_string())
    }

    /// Replace config from JSON and persist to disk.
    pub fn set_config_json(&self, json: &str) -> Result<(), String> {
        let cfg: Config = serde_json::from_str(json).map_err(|e| format!("invalid config: {e}"))?;
        cfg.save(&self.config_path)
            .map_err(|e| format!("failed to save config: {e}"))?;
        *self.config.write().unwrap() = Arc::new(cfg);
        Ok(())
    }

    pub fn reload_config(&self) -> Result<(), String> {
        let cfg = Config::load_or_create(&self.config_path).map_err(|e| e.to_string())?;
        *self.config.write().unwrap() = Arc::new(cfg);
        Ok(())
    }
}

/// Per-input-context session. Holds the raw pinyin buffer and tracks the single
/// in-flight conversion so a new keystroke supersedes the previous request.
pub struct Session {
    engine: Arc<Engine>,
    buffer: Mutex<String>,
    req_counter: AtomicU64,
    /// Generation/cancel token for the active request. Bumping it cancels any
    /// outstanding task (its result is dropped) and wakes the notify.
    active_gen: Arc<AtomicU64>,
    cancel: Arc<Notify>,
    /// LLM conversions of the current input the user can cycle through with
    /// up/down. Shared into the worker task so a completed conversion can record
    /// itself. Only ever holds remote (LLM) outputs — never n-gram guesses.
    candidates: Arc<Mutex<Candidates>>,
}

/// The alternative LLM conversions for one input, plus the currently-shown index.
/// `list[0]` is the primary conversion; later entries are regenerated on demand.
#[derive(Default)]
struct Candidates {
    /// The exact pinyin these candidates were produced for. Navigation is only
    /// valid while this matches the live buffer; once the user types more, the
    /// next conversion replaces the whole set.
    input: String,
    list: Vec<String>,
    cursor: usize,
}

impl Session {
    pub fn new(engine: Arc<Engine>) -> Session {
        Session {
            engine,
            buffer: Mutex::new(String::new()),
            req_counter: AtomicU64::new(0),
            active_gen: Arc::new(AtomicU64::new(0)),
            cancel: Arc::new(Notify::new()),
            candidates: Arc::new(Mutex::new(Candidates::default())),
        }
    }

    /// Move to the previous (`direction < 0`) or next (`direction >= 0`) cached
    /// candidate for the current input and return it, or `None` when there is no
    /// candidate in that direction (already at the primary going up, or none left
    /// going down — the frontend then calls [`regenerate`](Self::regenerate)).
    /// Synchronous; consults only the cache, never the network.
    pub fn cached_candidate(&self, direction: i32) -> Option<String> {
        let buffer = self.get_input();
        let mut c = self.candidates.lock().unwrap();
        if c.input != buffer || c.list.is_empty() {
            return None;
        }
        let next = if direction >= 0 {
            let n = c.cursor + 1;
            if n >= c.list.len() {
                return None;
            }
            n
        } else {
            c.cursor.checked_sub(1)?
        };
        c.cursor = next;
        Some(c.list[next].clone())
    }

    pub fn set_input(&self, pinyin: &str) {
        *self.buffer.lock().unwrap() = pinyin.to_string();
    }

    pub fn get_input(&self) -> String {
        self.buffer.lock().unwrap().clone()
    }

    /// An instant local speculative conversion of the current buffer, or `None`
    /// when speculation is disabled or the local model cannot cover the input.
    /// Synchronous and cheap — a frontend can call this the moment the buffer
    /// changes to show a guess, then let `convert`/`convert_stream` correct it.
    pub fn speculate(&self) -> Option<String> {
        self.engine.speculate(&self.get_input())
    }

    /// Conservative estimate of the chat-context token count for the current
    /// buffer ([system prompt] + [pinyin]). No tokenizer dependency: ~2 chars per
    /// token, which over-estimates for ordinary English/pinyin so the budget
    /// check errs toward flushing a little early rather than overshooting.
    pub fn context_tokens(&self) -> u32 {
        let cfg = self.engine.config_snapshot();
        let chars = cfg.system_prompt.chars().count() + self.buffer.lock().unwrap().chars().count();
        // ceil(chars / 2) + a little overhead for role/message framing.
        ((chars as u64).div_ceil(2) + 8).min(u32::MAX as u64) as u32
    }

    /// True when the current context is at/over the configured `max_context_tokens`
    /// budget. The frontend should flush (commit) the composition and start a
    /// fresh session before accepting more input, keeping each request small and
    /// the cached prefix effective.
    pub fn context_full(&self) -> bool {
        self.context_tokens() >= self.engine.config_snapshot().max_context_tokens
    }

    pub fn reset(&self) {
        self.cancel_inflight();
        self.buffer.lock().unwrap().clear();
    }

    pub fn cancel_inflight(&self) {
        self.active_gen.fetch_add(1, Ordering::SeqCst);
        self.cancel.notify_waiters();
    }

    /// Spawn an async conversion of the current buffer. `deliver` is invoked
    /// with the outcome from a worker thread, unless the request is superseded
    /// or cancelled first. Returns the request id, or 0 if the buffer is empty.
    pub fn convert<F>(&self, deliver: F) -> u64
    where
        F: FnOnce(ConvertOutcome) + Send + 'static,
    {
        let pinyin = self.get_input();
        if pinyin.trim().is_empty() {
            return 0;
        }

        // Supersede any previous request and claim this generation.
        let my_gen = self.active_gen.fetch_add(1, Ordering::SeqCst) + 1;
        self.cancel.notify_waiters();

        let request_id = self.req_counter.fetch_add(1, Ordering::SeqCst) + 1;
        let engine = self.engine.clone();
        let active_gen = self.active_gen.clone();
        let cancel = self.cancel.clone();
        let cfg = engine.config_snapshot();
        let handle = engine.rt.clone();

        let candidates = self.candidates.clone();
        handle.spawn(async move {
            let result = tokio::select! {
                biased;
                _ = cancel.notified() => Err(api::ConvertError::Cancelled),
                r = api::convert(&engine.client, &cfg, &pinyin, &[]) => r,
            };
            // Train the local speculative model from a successful conversion so
            // the next identical/overlapping input can be guessed instantly.
            if let Ok(text) = &result {
                engine.learn(&pinyin, text);
                record_candidate(&candidates, &active_gen, my_gen, &pinyin, text, false);
            }
            // The callback is invoked exactly once for every convert() that
            // returned a non-zero id (frontends rely on this to balance the
            // resources tied to `deliver`).
            deliver(finalize(&active_gen, my_gen, request_id, result));
        });

        request_id
    }

    /// Like [`Session::convert`], but streams the conversion (SSE) when the
    /// config enables it. `on_partial` is invoked zero-or-more times with the
    /// cumulative text as it arrives, then `deliver` is invoked exactly once
    /// with the terminal outcome (final text, error, or DS_ERR_CANCELLED).
    ///
    /// `on_partial` calls are best-effort: they only fire while this request is
    /// still the active generation, and never after `deliver`. Frontends should
    /// tie per-request resource ownership to `deliver`, not to `on_partial`.
    pub fn convert_stream<P, F>(&self, on_partial: P, deliver: F) -> u64
    where
        P: Fn(u64, &str) + Send + 'static,
        F: FnOnce(ConvertOutcome) + Send + 'static,
    {
        // Normal conversion: no exclusions, paint the local speculation first,
        // and the result replaces the candidate set.
        self.stream_with(Vec::new(), true, false, on_partial, deliver)
    }

    /// Ask the provider for a DIFFERENT conversion of the current input, avoiding
    /// every candidate already shown, and append it to the candidate list (so
    /// up/down can revisit it without another request). Same streaming contract
    /// as [`convert_stream`](Self::convert_stream). The frontend calls this when
    /// [`cached_candidate`](Self::cached_candidate) returns `None` going down —
    /// i.e. the user wants another option but none is cached yet. No local
    /// speculation is painted (this is an alternative, not a first impression).
    pub fn regenerate<P, F>(&self, on_partial: P, deliver: F) -> u64
    where
        P: Fn(u64, &str) + Send + 'static,
        F: FnOnce(ConvertOutcome) + Send + 'static,
    {
        let buffer = self.get_input();
        let exclude = {
            let c = self.candidates.lock().unwrap();
            if c.input == buffer {
                c.list.clone()
            } else {
                Vec::new()
            }
        };
        self.stream_with(exclude, false, true, on_partial, deliver)
    }

    /// Shared driver for `convert_stream` / `regenerate`. `exclude` lists the
    /// already-shown conversions to avoid; `speculate` paints the local n-gram
    /// guess as the first partial; `append` adds the result to the candidate list
    /// (vs. replacing it).
    fn stream_with<P, F>(
        &self,
        exclude: Vec<String>,
        speculate: bool,
        append: bool,
        on_partial: P,
        deliver: F,
    ) -> u64
    where
        P: Fn(u64, &str) + Send + 'static,
        F: FnOnce(ConvertOutcome) + Send + 'static,
    {
        let pinyin = self.get_input();
        if pinyin.trim().is_empty() {
            return 0;
        }

        let my_gen = self.active_gen.fetch_add(1, Ordering::SeqCst) + 1;
        self.cancel.notify_waiters();

        let request_id = self.req_counter.fetch_add(1, Ordering::SeqCst) + 1;
        let engine = self.engine.clone();
        let active_gen = self.active_gen.clone();
        let cancel = self.cancel.clone();
        let candidates = self.candidates.clone();
        let cfg = engine.config_snapshot();
        let handle = engine.rt.clone();

        handle.spawn(async move {
            let result = if cfg.stream {
                // Paint the instant local speculation as the first partial (if
                // enabled and the model can cover this input), before the network
                // responds — the streamed remote tokens then overwrite it. Guarded
                // by the generation check so a stale speculation can't clobber a
                // newer request's pre-edit.
                if speculate && active_gen.load(Ordering::SeqCst) == my_gen {
                    if let Some(guess) = engine.speculate(&pinyin) {
                        on_partial(request_id, &guess);
                    }
                }
                let active_gen_p = active_gen.clone();
                // `move` so the spawned future owns `on_partial` (needs only
                // Send, not Sync). Drop partials from a superseded generation so
                // a stale stream can't overwrite a newer request's pre-edit.
                let on_delta = move |cumulative: &str| {
                    if active_gen_p.load(Ordering::SeqCst) == my_gen {
                        on_partial(request_id, cumulative);
                    }
                };
                tokio::select! {
                    biased;
                    _ = cancel.notified() => Err(api::ConvertError::Cancelled),
                    r = api::convert_stream(&engine.client, &cfg, &pinyin, &exclude, on_delta) => r,
                }
            } else {
                tokio::select! {
                    biased;
                    _ = cancel.notified() => Err(api::ConvertError::Cancelled),
                    r = api::convert(&engine.client, &cfg, &pinyin, &exclude) => r,
                }
            };
            if let Ok(text) = &result {
                engine.learn(&pinyin, text);
                record_candidate(&candidates, &active_gen, my_gen, &pinyin, text, append);
            }
            deliver(finalize(&active_gen, my_gen, request_id, result));
        });

        request_id
    }
}

/// Record a successful LLM conversion in the candidate cache, unless a newer
/// request has already superseded this one. `append` adds `text` as another
/// option for the same input (regeneration); otherwise it replaces the set with
/// a fresh `[text]` (a new conversion). A duplicate is never appended.
fn record_candidate(
    candidates: &Mutex<Candidates>,
    active_gen: &AtomicU64,
    my_gen: u64,
    pinyin: &str,
    text: &str,
    append: bool,
) {
    if active_gen.load(Ordering::SeqCst) != my_gen {
        return;
    }
    let mut c = candidates.lock().unwrap();
    if append && c.input == pinyin {
        if !c.list.iter().any(|t| t == text) {
            c.list.push(text.to_string());
        }
        c.cursor = c.list.len().saturating_sub(1);
    } else {
        c.input = pinyin.to_string();
        c.list = vec![text.to_string()];
        c.cursor = 0;
    }
}

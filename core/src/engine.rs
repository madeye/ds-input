//! Engine and session state: the runtime, HTTP client, config, and per-session
//! pinyin buffer with single-in-flight cancellation.

use crate::api;
use crate::config::Config;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use tokio::runtime::Runtime;
use tokio::sync::Notify;

/// Outcome handed back to the FFI layer's callback.
pub struct ConvertOutcome {
    pub request_id: u64,
    pub status: i32,
    pub text: String,
}

/// Shared, thread-safe engine state. One per process is typical; cheap to share
/// across sessions via `Arc`.
pub struct Engine {
    rt: Runtime,
    client: reqwest::Client,
    config: RwLock<Arc<Config>>,
    config_path: PathBuf,
}

impl Engine {
    pub fn new(config_path: Option<PathBuf>) -> Result<Arc<Engine>, String> {
        let config_path = config_path.unwrap_or_else(Config::default_path);
        let config = Config::load_or_create(&config_path)
            .map_err(|e| format!("failed to load config at {}: {e}", config_path.display()))?;

        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .map_err(|e| format!("failed to start runtime: {e}"))?;

        let client = reqwest::Client::builder()
            .user_agent(concat!("dsime/", env!("CARGO_PKG_VERSION")))
            .build()
            .map_err(|e| format!("failed to build http client: {e}"))?;

        Ok(Arc::new(Engine {
            rt,
            client,
            config: RwLock::new(Arc::new(config)),
            config_path,
        }))
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
}

impl Session {
    pub fn new(engine: Arc<Engine>) -> Session {
        Session {
            engine,
            buffer: Mutex::new(String::new()),
            req_counter: AtomicU64::new(0),
            active_gen: Arc::new(AtomicU64::new(0)),
            cancel: Arc::new(Notify::new()),
        }
    }

    pub fn set_input(&self, pinyin: &str) {
        *self.buffer.lock().unwrap() = pinyin.to_string();
    }

    pub fn get_input(&self) -> String {
        self.buffer.lock().unwrap().clone()
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
        let handle = engine.rt.handle().clone();

        handle.spawn(async move {
            let result = tokio::select! {
                biased;
                _ = cancel.notified() => Err(api::ConvertError::Cancelled),
                r = api::convert(&engine.client, &cfg, &pinyin) => r,
            };

            // The callback is invoked exactly once for every convert() that
            // returned a non-zero id (frontends rely on this to balance the
            // resources tied to `deliver`). If a newer request superseded us
            // we still deliver, but as DS_ERR_CANCELLED so only the latest
            // request can produce a real conversion.
            let outcome = if active_gen.load(Ordering::SeqCst) != my_gen {
                let e = api::ConvertError::Cancelled;
                ConvertOutcome {
                    request_id,
                    status: e.status_code(),
                    text: e.message(),
                }
            } else {
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
            };
            deliver(outcome);
        });

        request_id
    }
}

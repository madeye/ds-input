//! User configuration: the OpenAI-compatible provider, model, key, and tuning.
//!
//! Persisted as JSON so the platform Settings UIs can read/write it verbatim
//! through `ds_engine_get_config_json` / `ds_engine_set_config_json`.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Default endpoint: DeepSeek's OpenAI-compatible API.
pub const DEFAULT_BASE_URL: &str = "https://api.deepseek.com/v1";
/// Default model — a fast, cheap chat model well suited to inline conversion.
pub const DEFAULT_MODEL: &str = "deepseek-v4-flash";

/// Instruction that turns a chat model into a whole-sentence pinyin converter.
// Kept byte-stable and sent as the first (system) message on every request so it
// forms a constant cacheable prefix — DeepSeek context caching then bills it at
// the cache-hit rate and doesn't reprocess it on each incremental keystroke.
pub const DEFAULT_SYSTEM_PROMPT: &str = "\
Convert toneless Hanyu Pinyin to the single most natural Chinese sentence. \
Syllables may run together, be space-separated, or apostrophe-separated (the \
apostrophe only marks a syllable boundary, e.g. xi'an = 西安).\n\
Rules:\n\
- Output ONLY the Chinese: no pinyin, explanation, quotes, extra whitespace, \
or alternatives.\n\
- Keep Latin words, numbers, emails, URLs, and code identifiers verbatim.\n\
- Use full-width Chinese punctuation when the surrounding text is Chinese.\n\
- If the input is empty or not pinyin, return it unchanged.";

fn default_base_url() -> String {
    DEFAULT_BASE_URL.to_string()
}
fn default_model() -> String {
    DEFAULT_MODEL.to_string()
}
fn default_system_prompt() -> String {
    DEFAULT_SYSTEM_PROMPT.to_string()
}
fn default_temperature() -> f32 {
    0.3
}
fn default_max_tokens() -> u32 {
    256
}
fn default_timeout_ms() -> u64 {
    8000
}
fn default_debounce_ms() -> u32 {
    180
}
fn default_stream() -> bool {
    true
}
fn default_max_context_tokens() -> u32 {
    1000
}

/// The full, serializable user configuration.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Config {
    /// OpenAI-compatible base URL, e.g. `https://api.deepseek.com/v1`.
    #[serde(default = "default_base_url")]
    pub base_url: String,
    /// API bearer key. Empty until the user sets it.
    #[serde(default)]
    pub api_key: String,
    /// Chat model id, e.g. `deepseek-v4-flash`.
    #[serde(default = "default_model")]
    pub model: String,
    /// System prompt that defines the conversion behaviour.
    #[serde(default = "default_system_prompt")]
    pub system_prompt: String,
    /// Sampling temperature. Low = more deterministic conversions.
    #[serde(default = "default_temperature")]
    pub temperature: f32,
    /// Upper bound on generated tokens for one sentence.
    #[serde(default = "default_max_tokens")]
    pub max_tokens: u32,
    /// Per-request network timeout.
    #[serde(default = "default_timeout_ms")]
    pub timeout_ms: u64,
    /// Idle time after the last keystroke before the frontend should convert.
    #[serde(default = "default_debounce_ms")]
    pub debounce_ms: u32,
    /// Stream the conversion (SSE) so the pre-edit fills in token-by-token.
    /// Lower perceived latency; disable for a single final delivery.
    #[serde(default = "default_stream")]
    pub stream: bool,
    /// Soft cap on the per-request chat-context size (system prompt + pinyin), in
    /// estimated tokens. When the uncommitted buffer would push a request past
    /// this, the frontend flushes (commits) and starts a fresh session so each
    /// request stays small and cache-friendly. See `Session::context_full`.
    #[serde(default = "default_max_context_tokens")]
    pub max_context_tokens: u32,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            base_url: default_base_url(),
            api_key: String::new(),
            model: default_model(),
            system_prompt: default_system_prompt(),
            temperature: default_temperature(),
            max_tokens: default_max_tokens(),
            timeout_ms: default_timeout_ms(),
            debounce_ms: default_debounce_ms(),
            stream: default_stream(),
            max_context_tokens: default_max_context_tokens(),
        }
    }
}

impl Config {
    /// Per-user default config path:
    /// macOS `~/Library/Application Support/DSInput/config.json`,
    /// Windows `%APPDATA%/DSInput/config.json`.
    pub fn default_path() -> PathBuf {
        if let Some(dirs) = directories::ProjectDirs::from("io", "DSInput", "DSInput") {
            dirs.config_dir().join("config.json")
        } else {
            PathBuf::from("config.json")
        }
    }

    /// Load from `path`, falling back to defaults (and creating the file) if it
    /// does not yet exist. Missing fields are filled from defaults.
    pub fn load_or_create(path: &Path) -> std::io::Result<Config> {
        match std::fs::read_to_string(path) {
            Ok(text) => {
                let cfg: Config = serde_json::from_str(&text).map_err(|e| {
                    std::io::Error::new(std::io::ErrorKind::InvalidData, e.to_string())
                })?;
                Ok(cfg)
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                let cfg = Config::default();
                cfg.save(path)?;
                Ok(cfg)
            }
            Err(e) => Err(e),
        }
    }

    /// Pretty-print to `path`, creating parent directories as needed.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let json = serde_json::to_string_pretty(self)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e.to_string()))?;
        std::fs::write(path, json)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_deepseek_flash() {
        let c = Config::default();
        assert_eq!(c.base_url, "https://api.deepseek.com/v1");
        assert_eq!(c.model, "deepseek-v4-flash");
        assert!(c.api_key.is_empty());
        assert!(c.debounce_ms > 0);
    }

    #[test]
    fn missing_fields_fill_from_defaults() {
        // A minimal config (only api_key) must still deserialize, with every
        // other field taking its default — this is what a fresh Settings save
        // or a hand-edited file may look like.
        let json = r#"{ "api_key": "sk-test" }"#;
        let c: Config = serde_json::from_str(json).unwrap();
        assert_eq!(c.api_key, "sk-test");
        assert_eq!(c.model, "deepseek-v4-flash");
        assert_eq!(c.temperature, 0.3);
    }

    #[test]
    fn round_trips_through_disk() {
        let dir = std::env::temp_dir().join(format!("dsime-test-{}", std::process::id()));
        let path = dir.join("config.json");
        let _ = std::fs::remove_dir_all(&dir);

        // First load creates the file with defaults.
        let c1 = Config::load_or_create(&path).unwrap();
        assert!(path.exists());
        assert_eq!(c1.model, "deepseek-v4-flash");

        // Mutate + save, then reload and confirm persistence.
        let mut c2 = c1.clone();
        c2.api_key = "sk-roundtrip".to_string();
        c2.model = "gpt-4o-mini".to_string();
        c2.save(&path).unwrap();
        let c3 = Config::load_or_create(&path).unwrap();
        assert_eq!(c3.api_key, "sk-roundtrip");
        assert_eq!(c3.model, "gpt-4o-mini");

        let _ = std::fs::remove_dir_all(&dir);
    }
}

//! OpenAI-compatible chat-completions client used to convert pinyin → Chinese.

use crate::config::Config;
use serde::{Deserialize, Serialize};
use std::time::Duration;

/// Error categories mirrored to the C `DS_ERR_*` status codes.
#[derive(Debug)]
pub enum ConvertError {
    Network(String),
    Auth(String),
    Api(String),
    Cancelled,
    Config(String),
}

impl ConvertError {
    /// Map to the integer status code exposed across the FFI boundary.
    pub fn status_code(&self) -> i32 {
        match self {
            ConvertError::Network(_) => 1, // DS_ERR_NETWORK
            ConvertError::Auth(_) => 2,    // DS_ERR_AUTH
            ConvertError::Api(_) => 3,     // DS_ERR_API
            ConvertError::Cancelled => 4,  // DS_ERR_CANCELLED
            ConvertError::Config(_) => 5,  // DS_ERR_CONFIG
        }
    }

    pub fn message(&self) -> String {
        match self {
            ConvertError::Network(m) => format!("network error: {m}"),
            ConvertError::Auth(m) => format!("auth error: {m}"),
            ConvertError::Api(m) => format!("api error: {m}"),
            ConvertError::Cancelled => "cancelled".to_string(),
            ConvertError::Config(m) => format!("config error: {m}"),
        }
    }
}

#[derive(Serialize)]
struct ChatRequest<'a> {
    model: &'a str,
    messages: Vec<ChatMessage<'a>>,
    /// gpt-5 / o-series reject any non-default temperature, so we omit it for them.
    #[serde(skip_serializing_if = "Option::is_none")]
    temperature: Option<f32>,
    /// Classic OpenAI-compatible token cap (DeepSeek et al.).
    #[serde(skip_serializing_if = "Option::is_none")]
    max_tokens: Option<u32>,
    /// gpt-5 / o-series replacement for `max_tokens`.
    #[serde(skip_serializing_if = "Option::is_none")]
    max_completion_tokens: Option<u32>,
    stream: bool,
}

#[derive(Serialize)]
struct ChatMessage<'a> {
    role: &'a str,
    content: &'a str,
}

#[derive(Deserialize)]
struct ChatResponse {
    choices: Vec<ChatChoice>,
}

#[derive(Deserialize)]
struct ChatChoice {
    message: ChoiceMessage,
}

#[derive(Deserialize)]
struct ChoiceMessage {
    content: String,
}

/// One SSE chunk of a streamed completion: `{"choices":[{"delta":{"content":"…"}}]}`.
#[derive(Deserialize)]
struct StreamChunk {
    #[serde(default)]
    choices: Vec<StreamChoice>,
}

#[derive(Deserialize)]
struct StreamChoice {
    #[serde(default)]
    delta: StreamDelta,
}

#[derive(Deserialize, Default)]
struct StreamDelta {
    #[serde(default)]
    content: Option<String>,
}

#[derive(Deserialize)]
struct ApiErrorEnvelope {
    error: ApiErrorBody,
}

#[derive(Deserialize)]
struct ApiErrorBody {
    message: String,
}

/// When regenerating an alternative, instruct the model to avoid the conversions
/// the user has already rejected (`exclude`). Returns `None` for the normal path
/// (no exclusions) so the primary prompt stays untouched.
fn regen_instruction(exclude: &[String]) -> Option<String> {
    if exclude.is_empty() {
        return None;
    }
    let shown = exclude
        .iter()
        .map(|c| format!("- {c}"))
        .collect::<Vec<_>>()
        .join("\n");
    Some(format!(
        "These conversions of the same input were already shown and rejected:\n{shown}\n\
         Provide a DIFFERENT, equally natural whole-sentence conversion. It must not \
         equal any rejected one. Output only the alternative, no explanation."
    ))
}

/// System prompt + user pinyin, plus an optional regeneration instruction.
fn build_messages<'a>(
    cfg: &'a Config,
    pinyin: &'a str,
    regen: &'a Option<String>,
) -> Vec<ChatMessage<'a>> {
    let mut messages = vec![
        ChatMessage {
            role: "system",
            content: &cfg.system_prompt,
        },
        ChatMessage {
            role: "user",
            content: pinyin,
        },
    ];
    if let Some(instr) = regen {
        messages.push(ChatMessage {
            role: "system",
            content: instr,
        });
    }
    messages
}

/// Bump temperature when regenerating so the alternative actually differs;
/// the normal path keeps the configured (lower) temperature.
fn effective_temperature(cfg: &Config, exclude: &[String]) -> f32 {
    if exclude.is_empty() {
        cfg.temperature
    } else {
        cfg.temperature.max(0.8)
    }
}

/// OpenAI's gpt-5 and o-series (reasoning) models diverge from the classic
/// chat-completions schema: they reject `max_tokens` (require
/// `max_completion_tokens`) and reject any non-default `temperature`. Other
/// OpenAI-compatible providers (DeepSeek, etc.) still use the classic schema, so
/// we only switch for models whose id marks them as one of these families.
fn is_restricted_openai_model(model: &str) -> bool {
    let m = model.trim().to_ascii_lowercase();
    // Match family prefixes so future point releases (gpt-5.5, o3-mini, …) are
    // covered. A trailing boundary check avoids matching unrelated ids like
    // "o1ololo-custom" only when the prefix is the whole token start.
    const FAMILIES: &[&str] = &["gpt-5", "gpt-6", "o1", "o3", "o4"];
    FAMILIES.iter().any(|fam| {
        m == *fam
            || m.strip_prefix(fam)
                .is_some_and(|rest| rest.starts_with(['-', '.']))
    })
}

/// Build the (temperature, max_tokens, max_completion_tokens) triple for a
/// request, selecting the right token-cap field and dropping temperature for
/// restricted OpenAI models.
fn token_params(cfg: &Config, exclude: &[String]) -> (Option<f32>, Option<u32>, Option<u32>) {
    if is_restricted_openai_model(&cfg.model) {
        // These models only accept the default temperature (1); sending the
        // configured (lower) value errors, so omit it entirely.
        (None, None, Some(cfg.max_tokens))
    } else {
        (
            Some(effective_temperature(cfg, exclude)),
            Some(cfg.max_tokens),
            None,
        )
    }
}

/// Send one conversion request. `client` is a shared, connection-pooled client.
/// `exclude` lists already-shown conversions to avoid (empty for the normal path;
/// non-empty when regenerating an alternative).
pub async fn convert(
    client: &reqwest::Client,
    cfg: &Config,
    pinyin: &str,
    exclude: &[String],
) -> Result<String, ConvertError> {
    if cfg.api_key.trim().is_empty() {
        return Err(ConvertError::Config(
            "API key is not set — open Settings and add your key".to_string(),
        ));
    }

    let url = format!("{}/chat/completions", cfg.base_url.trim_end_matches('/'));
    let regen = regen_instruction(exclude);
    let (temperature, max_tokens, max_completion_tokens) = token_params(cfg, exclude);
    let body = ChatRequest {
        model: &cfg.model,
        messages: build_messages(cfg, pinyin, &regen),
        temperature,
        max_tokens,
        max_completion_tokens,
        stream: false,
    };

    let resp = client
        .post(&url)
        .bearer_auth(&cfg.api_key)
        .timeout(Duration::from_millis(cfg.timeout_ms))
        .json(&body)
        .send()
        .await
        .map_err(|e| ConvertError::Network(e.to_string()))?;

    let status = resp.status();
    let text = resp
        .text()
        .await
        .map_err(|e| ConvertError::Network(e.to_string()))?;

    if !status.is_success() {
        // Try to surface the provider's error message.
        let detail = serde_json::from_str::<ApiErrorEnvelope>(&text)
            .map(|e| e.error.message)
            .unwrap_or_else(|_| text.chars().take(300).collect());
        return Err(match status.as_u16() {
            401 | 403 => ConvertError::Auth(detail),
            _ => ConvertError::Api(format!("HTTP {}: {}", status.as_u16(), detail)),
        });
    }

    let parsed: ChatResponse =
        serde_json::from_str(&text).map_err(|e| ConvertError::Api(format!("bad response: {e}")))?;

    let content = parsed
        .choices
        .into_iter()
        .next()
        .map(|c| c.message.content)
        .ok_or_else(|| ConvertError::Api("empty choices".to_string()))?;

    Ok(sanitize(&content))
}

/// Stream a conversion (SSE, `stream: true`). `on_delta` is called with the
/// *cumulative* text each time the model emits more, so the frontend can replace
/// the pre-edit incrementally. Returns the final sanitized text. The cumulative
/// text passed to `on_delta` is raw (not sanitized) so partial quotes/whitespace
/// may appear; only the returned final value is sanitized.
pub async fn convert_stream<F>(
    client: &reqwest::Client,
    cfg: &Config,
    pinyin: &str,
    exclude: &[String],
    mut on_delta: F,
) -> Result<String, ConvertError>
where
    F: FnMut(&str),
{
    if cfg.api_key.trim().is_empty() {
        return Err(ConvertError::Config(
            "API key is not set — open Settings and add your key".to_string(),
        ));
    }

    let url = format!("{}/chat/completions", cfg.base_url.trim_end_matches('/'));
    let regen = regen_instruction(exclude);
    let (temperature, max_tokens, max_completion_tokens) = token_params(cfg, exclude);
    let body = ChatRequest {
        model: &cfg.model,
        messages: build_messages(cfg, pinyin, &regen),
        temperature,
        max_tokens,
        max_completion_tokens,
        stream: true,
    };

    let mut resp = client
        .post(&url)
        .bearer_auth(&cfg.api_key)
        .timeout(Duration::from_millis(cfg.timeout_ms))
        .json(&body)
        .send()
        .await
        .map_err(|e| ConvertError::Network(e.to_string()))?;

    let status = resp.status();
    if !status.is_success() {
        // Errors come back as a normal JSON body, not an SSE stream.
        let text = resp.text().await.unwrap_or_default();
        let detail = serde_json::from_str::<ApiErrorEnvelope>(&text)
            .map(|e| e.error.message)
            .unwrap_or_else(|_| text.chars().take(300).collect());
        return Err(match status.as_u16() {
            401 | 403 => ConvertError::Auth(detail),
            _ => ConvertError::Api(format!("HTTP {}: {}", status.as_u16(), detail)),
        });
    }

    // Parse the SSE stream line by line. We buffer raw bytes and only decode
    // complete lines (split on '\n') so a multi-byte UTF-8 char straddling a
    // chunk boundary is never decoded mid-sequence.
    let mut buf: Vec<u8> = Vec::new();
    let mut acc = String::new();
    while let Some(chunk) = resp
        .chunk()
        .await
        .map_err(|e| ConvertError::Network(e.to_string()))?
    {
        buf.extend_from_slice(&chunk);
        while let Some(pos) = buf.iter().position(|&b| b == b'\n') {
            let line: Vec<u8> = buf.drain(..=pos).collect();
            let line = String::from_utf8_lossy(&line);
            let line = line.trim();
            let Some(payload) = line.strip_prefix("data:") else {
                continue; // comments / blank lines / other SSE fields
            };
            let payload = payload.trim();
            if payload.is_empty() {
                continue;
            }
            if payload == "[DONE]" {
                return Ok(sanitize(&acc));
            }
            if let Ok(parsed) = serde_json::from_str::<StreamChunk>(payload) {
                if let Some(piece) = parsed
                    .choices
                    .into_iter()
                    .next()
                    .and_then(|c| c.delta.content)
                {
                    if !piece.is_empty() {
                        acc.push_str(&piece);
                        on_delta(&acc);
                    }
                }
            }
        }
    }

    Ok(sanitize(&acc))
}

/// Models sometimes wrap output in quotes or trailing whitespace; strip that so
/// the frontend gets a clean pre-edit string.
fn sanitize(s: &str) -> String {
    let t = s.trim();
    let t = t
        .strip_prefix('"')
        .and_then(|x| x.strip_suffix('"'))
        .unwrap_or(t);
    t.trim().to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sanitize_strips_quotes_and_whitespace() {
        assert_eq!(sanitize("  你好  "), "你好");
        assert_eq!(sanitize("\"你好世界\""), "你好世界");
        assert_eq!(sanitize("你好\n"), "你好");
        // Inner quotes are preserved; only a fully-wrapping pair is stripped.
        assert_eq!(sanitize("他说\"好\""), "他说\"好\"");
    }

    #[test]
    fn status_codes_match_header() {
        assert_eq!(ConvertError::Network(String::new()).status_code(), 1);
        assert_eq!(ConvertError::Auth(String::new()).status_code(), 2);
        assert_eq!(ConvertError::Api(String::new()).status_code(), 3);
        assert_eq!(ConvertError::Cancelled.status_code(), 4);
        assert_eq!(ConvertError::Config(String::new()).status_code(), 5);
    }

    #[test]
    fn restricted_openai_models_detected() {
        for m in [
            "gpt-5",
            "gpt-5.5",
            "gpt-5-mini",
            "GPT-5",
            "o1",
            "o1-mini",
            "o3",
            "o3-mini",
            "o4-mini",
            "gpt-6",
        ] {
            assert!(is_restricted_openai_model(m), "{m} should be restricted");
        }
        for m in [
            "deepseek-v4-flash",
            "deepseek-chat",
            "gpt-4o",
            "gpt-4o-mini",
            "gpt-4.1",
            "o1ololo", // not a real o1 release; must not match
            "",
        ] {
            assert!(!is_restricted_openai_model(m), "{m} should be classic");
        }
    }

    #[test]
    fn token_params_pick_field_per_model() {
        let base = Config {
            max_tokens: 256,
            temperature: 0.3,
            ..Config::default()
        };

        // Classic provider: temperature + max_tokens, no max_completion_tokens.
        let cfg = Config {
            model: "deepseek-v4-flash".to_string(),
            ..base.clone()
        };
        let (temp, max_tok, max_comp) = token_params(&cfg, &[]);
        assert_eq!(temp, Some(0.3));
        assert_eq!(max_tok, Some(256));
        assert_eq!(max_comp, None);

        // Restricted OpenAI: max_completion_tokens, no temperature/max_tokens.
        let cfg = Config {
            model: "gpt-5.5".to_string(),
            ..base
        };
        let (temp, max_tok, max_comp) = token_params(&cfg, &[]);
        assert_eq!(temp, None);
        assert_eq!(max_tok, None);
        assert_eq!(max_comp, Some(256));
    }

    #[test]
    fn restricted_request_serializes_without_temperature_or_max_tokens() {
        let body = ChatRequest {
            model: "gpt-5.5",
            messages: vec![],
            temperature: None,
            max_tokens: None,
            max_completion_tokens: Some(256),
            stream: false,
        };
        let json = serde_json::to_string(&body).unwrap();
        assert!(json.contains("max_completion_tokens"));
        assert!(!json.contains("\"temperature\""));
        assert!(!json.contains("\"max_tokens\""));
    }
}

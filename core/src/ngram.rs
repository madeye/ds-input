//! Local n-gram model for *speculative* pinyin → Chinese conversion.
//!
//! The remote LLM produces the authoritative conversion, but a round-trip costs
//! tens to hundreds of milliseconds. This module keeps a tiny on-device n-gram
//! model, trained incrementally from the conversions the LLM has already
//! returned, so the engine can paint an *instant* best-guess pre-edit while the
//! network request is still in flight — the same idea as speculative decoding:
//! a cheap local model proposes, the expensive remote model corrects.
//!
//! The model aligns a toneless-pinyin syllable sequence with the Chinese
//! characters the LLM produced (one syllable ⇄ one Han character) and counts,
//! for each syllable in a short left context of already-decoded characters,
//! which character followed. Prediction is a greedy left-to-right argmax with
//! back-off from the highest-order context down to the per-syllable unigram. A
//! syllable the model has never seen yields no speculation at all (`None`) — we
//! would rather show nothing than a confidently wrong guess.
//!
//! It is deliberately small and dependency-free (just serde for persistence):
//! no tone handling, no probabilities, no smoothing — counts and argmax. That is
//! enough to cover the high-frequency phrases a given user types again and
//! again, which is exactly where instant feedback matters most.

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::Path;

/// Default context order: a bigram (one previous character of context, backing
/// off to the per-syllable unigram).
pub const DEFAULT_ORDER: usize = 2;

/// Separates the left-context characters from the current syllable inside a
/// context key. `\u{1}` (Start-of-Heading) never occurs in pinyin or Han text.
const KEY_SEP: char = '\u{1}';

/// A counts-only n-gram model mapping `(left context, pinyin syllable)` to the
/// distribution of Chinese characters observed in that position.
///
/// `counts[key][hanzi]` is how many times `hanzi` was seen for the context+
/// syllable encoded by `key` (see [`context_key`]). The outer key folds in the
/// context length, so unigram (`order` 1) and higher-order entries coexist in
/// one map and prediction can back off between them.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NgramModel {
    /// Context order N: predictions use up to `order - 1` previous characters,
    /// backing off toward the unigram. Always `>= 1`.
    order: usize,
    /// `key -> (hanzi -> count)`. Han characters are stored as single-character
    /// `String`s so the map serializes cleanly to a JSON object.
    counts: HashMap<String, HashMap<String, u32>>,
}

impl Default for NgramModel {
    fn default() -> Self {
        NgramModel::with_order(DEFAULT_ORDER)
    }
}

impl NgramModel {
    /// Create an empty model with the given context order (clamped to `>= 1`).
    pub fn with_order(order: usize) -> Self {
        NgramModel {
            order: order.max(1),
            counts: HashMap::new(),
        }
    }

    /// True when nothing has been learned yet.
    pub fn is_empty(&self) -> bool {
        self.counts.is_empty()
    }

    /// Load a model from `path`, or return a fresh model of `default_order` if
    /// the file does not exist. A corrupt/unreadable file is treated as absent
    /// so a bad cache can never wedge the engine — speculation just starts over.
    pub fn load_or_default(path: &Path, default_order: usize) -> Self {
        match std::fs::read_to_string(path) {
            Ok(text) => serde_json::from_str(&text)
                .unwrap_or_else(|_| NgramModel::with_order(default_order)),
            Err(_) => NgramModel::with_order(default_order),
        }
    }

    /// Persist the model to `path` (creating parent directories). Best-effort:
    /// callers treat a write failure as non-fatal since the model is only a
    /// latency optimization.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let json = serde_json::to_string(self)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e.to_string()))?;
        std::fs::write(path, json)
    }

    /// Learn from one observed conversion: `pinyin` is the raw ASCII input and
    /// `hanzi` the Chinese the LLM returned for it. The pair is used only when
    /// the pinyin segments cleanly into syllables AND there is exactly one Han
    /// character per syllable — otherwise alignment is ambiguous (mixed English,
    /// punctuation, numbers, …) and we skip it rather than learn noise.
    ///
    /// Returns whether anything was learned.
    pub fn learn(&mut self, pinyin: &str, hanzi: &str) -> bool {
        let Some(sylls) = segment(pinyin) else {
            return false;
        };
        let chars: Vec<&str> = split_han(hanzi);
        if sylls.is_empty() || sylls.len() != chars.len() {
            return false;
        }

        for i in 0..chars.len() {
            // Record this character under every context length from 0
            // (unigram) up to order-1, so prediction can back off.
            for k in 0..self.order {
                if k > i {
                    break;
                }
                let key = context_key(&chars[i - k..i], &sylls[i]);
                let dist = self.counts.entry(key).or_default();
                *dist.entry(chars[i].to_string()).or_default() += 1;
            }
        }
        true
    }

    /// Produce a speculative conversion for `pinyin`, or `None` when the input
    /// cannot be fully covered (it does not segment, or some syllable has never
    /// been seen). A returned string always has one Han character per syllable.
    pub fn predict(&self, pinyin: &str) -> Option<String> {
        let sylls = segment(pinyin)?;
        if sylls.is_empty() {
            return None;
        }

        let mut out: Vec<String> = Vec::with_capacity(sylls.len());
        for (i, syll) in sylls.iter().enumerate() {
            // Back off from the longest available context down to the unigram.
            let mut chosen: Option<&str> = None;
            let max_ctx = self.order.saturating_sub(1).min(i);
            for k in (0..=max_ctx).rev() {
                let key = context_key(&out[i - k..i], syll);
                if let Some(dist) = self.counts.get(&key) {
                    chosen = argmax(dist);
                    if chosen.is_some() {
                        break;
                    }
                }
            }
            // An unseen syllable: refuse to guess the whole sentence.
            out.push(chosen?.to_string());
        }
        Some(out.concat())
    }
}

/// Pick the highest-count entry, breaking ties by smaller key so the result is
/// deterministic regardless of `HashMap` iteration order.
fn argmax(dist: &HashMap<String, u32>) -> Option<&str> {
    dist.iter()
        .max_by(|a, b| a.1.cmp(b.1).then_with(|| b.0.cmp(a.0)))
        .map(|(k, _)| k.as_str())
}

/// Encode `(context characters, syllable)` into a single map key. The leading
/// `context.concat()` keeps the previous characters; `KEY_SEP` then the syllable
/// disambiguate it from any other context that shares a prefix.
fn context_key<S: AsRef<str>>(context: &[S], syllable: &str) -> String {
    let mut key = String::new();
    for c in context {
        key.push_str(c.as_ref());
    }
    key.push(KEY_SEP);
    key.push_str(syllable);
    key
}

/// Split a Han string into its individual characters as `&str` slices. Used to
/// align one character per syllable.
fn split_han(s: &str) -> Vec<&str> {
    let mut out = Vec::with_capacity(s.chars().count());
    let mut idx = 0;
    for ch in s.chars() {
        let next = idx + ch.len_utf8();
        out.push(&s[idx..next]);
        idx = next;
    }
    out
}

// ---- Pinyin segmentation ---------------------------------------------------

/// Greedy longest-match segmentation of a toneless-pinyin string into syllables.
///
/// Spaces and apostrophes are treated purely as boundary hints and dropped; the
/// remaining runs of ASCII lowercase letters are split by maximal-munch against
/// the valid-syllable set ([`syllable_set`]). Returns `None` if any run contains
/// a non-`[a-z' ]` byte or fails to segment fully — that is the signal that the
/// input is not pure pinyin (English, numbers, identifiers, …) and so should not
/// be learned from or speculated on.
pub fn segment(pinyin: &str) -> Option<Vec<String>> {
    let set = syllable_set();
    let mut out = Vec::new();
    for chunk in pinyin.split([' ', '\'', '\t']) {
        if chunk.is_empty() {
            continue;
        }
        if !chunk.bytes().all(|b| b.is_ascii_lowercase()) {
            return None;
        }
        segment_run(chunk, set, &mut out)?;
    }
    if out.is_empty() {
        None
    } else {
        Some(out)
    }
}

/// Segment one contiguous lowercase run, appending syllables to `out`. Tries the
/// longest candidate (up to 6 letters, the longest pinyin syllable) first.
fn segment_run(
    run: &str,
    set: &std::collections::HashSet<&'static str>,
    out: &mut Vec<String>,
) -> Option<()> {
    let bytes = run.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        let mut matched = 0;
        let hi = (i + 6).min(bytes.len());
        for end in (i + 1..=hi).rev() {
            if set.contains(&run[i..end]) {
                matched = end - i;
                break;
            }
        }
        if matched == 0 {
            return None; // a letter run that is not valid pinyin
        }
        out.push(run[i..i + matched].to_string());
        i += matched;
    }
    Some(())
}

/// The set of valid toneless pinyin syllables, built once on first use from the
/// standard initial × final table plus the vowel-initial standalone finals.
///
/// Built by cartesian product, so it slightly over-generates (a handful of
/// initial+final pairs that are not real syllables are admitted). That is
/// harmless here: such combinations do not occur in genuine pinyin input, and
/// segmentation only needs to be correct on real input, not to reject every
/// invalid string. Keeping it generative avoids hand-maintaining ~400 literals.
fn syllable_set() -> &'static std::collections::HashSet<&'static str> {
    use std::collections::HashSet;
    use std::sync::OnceLock;
    static SET: OnceLock<HashSet<&'static str>> = OnceLock::new();
    SET.get_or_init(|| {
        const INITIALS: &[&str] = &[
            "b", "p", "m", "f", "d", "t", "n", "l", "g", "k", "h", "j", "q", "x", "zh", "ch", "sh",
            "r", "z", "c", "s", "y", "w",
        ];
        const FINALS: &[&str] = &[
            "a", "o", "e", "i", "u", "v", "ai", "ei", "ui", "ao", "ou", "iu", "ie", "ve", "er",
            "an", "en", "in", "un", "vn", "ang", "eng", "ing", "ong", "ia", "iao", "ian", "iang",
            "iong", "ua", "uo", "uai", "uan", "uang", "ue", "uen", "van",
        ];
        // Vowel-initial finals that also stand alone as whole syllables.
        const STANDALONE: &[&str] = &[
            "a", "o", "e", "ai", "ei", "ao", "ou", "an", "en", "ang", "eng", "er",
        ];

        let mut set: HashSet<&'static str> = HashSet::new();
        // Pre-built combinations as &'static str via a leaked-once allocation is
        // overkill; instead store owned strings in a parallel static. We need
        // &'static, so build owned and leak them once (bounded, ~400 entries).
        let mut owned: Vec<String> = Vec::new();
        for ini in INITIALS {
            for fin in FINALS {
                owned.push(format!("{ini}{fin}"));
            }
        }
        for s in STANDALONE {
            owned.push((*s).to_string());
        }
        for s in owned {
            set.insert(Box::leak(s.into_boxed_str()));
        }
        set
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn segments_concatenated_pinyin() {
        assert_eq!(segment("nihao").unwrap(), vec!["ni", "hao"]);
        assert_eq!(
            segment("nihaoshijie").unwrap(),
            vec!["ni", "hao", "shi", "jie"]
        );
        // Spaces and apostrophes are boundary hints and dropped.
        assert_eq!(segment("ni hao").unwrap(), vec!["ni", "hao"]);
        assert_eq!(segment("xi'an").unwrap(), vec!["xi", "an"]);
    }

    #[test]
    fn rejects_non_pinyin() {
        // Digits, uppercase, and unsegmentable letter runs are not pure pinyin.
        assert!(segment("hello123").is_none());
        assert!(segment("Beijing").is_none());
        assert!(segment("xyzzqq").is_none());
        assert!(segment("").is_none());
    }

    #[test]
    fn learns_and_predicts_unigram() {
        let mut m = NgramModel::with_order(1);
        assert!(m.learn("nihao", "你好"));
        assert!(!m.is_empty());
        assert_eq!(m.predict("nihao").as_deref(), Some("你好"));
        // Generalizes to a subset/reordering of seen syllables.
        assert_eq!(m.predict("hao").as_deref(), Some("好"));
    }

    #[test]
    fn unseen_syllable_yields_no_speculation() {
        let mut m = NgramModel::default();
        m.learn("nihao", "你好");
        // "shi" was never observed → refuse to guess the whole sentence.
        assert_eq!(m.predict("nishishi"), None);
    }

    #[test]
    fn mismatched_alignment_is_not_learned() {
        let mut m = NgramModel::default();
        // Two syllables but three Han characters: cannot align one-to-one.
        assert!(!m.learn("nihao", "你好吗"));
        // Mixed/English input does not segment, so nothing is learned.
        assert!(!m.learn("hello world", "你好"));
        assert!(m.is_empty());
    }

    #[test]
    fn argmax_picks_most_frequent_and_is_deterministic() {
        let mut m = NgramModel::with_order(1);
        // Teach two competing characters for the same syllable, "是" more often.
        for _ in 0..3 {
            m.learn("shi", "是");
        }
        m.learn("shi", "时");
        assert_eq!(m.predict("shi").as_deref(), Some("是"));
    }

    #[test]
    fn higher_order_context_disambiguates() {
        // A bigram distinguishes the same syllable by its predecessor.
        let mut m = NgramModel::with_order(2);
        // After 你, "shi" → 是 ; standalone "shi" leans 时 via the unigram.
        m.learn("nishi", "你是");
        for _ in 0..5 {
            m.learn("shijian", "时间");
        }
        // Standalone: unigram back-off favors the more frequent 时.
        assert_eq!(m.predict("shi").as_deref(), Some("时"));
        // In the 你 context the bigram still prefers 是.
        assert_eq!(m.predict("nishi").as_deref(), Some("你是"));
    }

    #[test]
    fn round_trips_through_disk() {
        let dir = std::env::temp_dir().join(format!("dsime-ngram-{}", std::process::id()));
        let path = dir.join("ngram.json");
        let _ = std::fs::remove_dir_all(&dir);

        let mut m = NgramModel::with_order(2);
        m.learn("nihaoshijie", "你好世界");
        m.save(&path).unwrap();

        let loaded = NgramModel::load_or_default(&path, DEFAULT_ORDER);
        assert!(!loaded.is_empty());
        assert_eq!(loaded.predict("nihaoshijie").as_deref(), Some("你好世界"));

        // A missing file yields a fresh, empty model.
        let _ = std::fs::remove_dir_all(&dir);
        let fresh = NgramModel::load_or_default(&path, 3);
        assert!(fresh.is_empty());
    }
}

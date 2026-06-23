//! Bake the embedded speculative-model baseline.
//!
//! Reads a seed corpus on stdin (TSV: `pinyin<TAB>hanzi[<TAB>weight]`, `#`
//! comments and blank lines ignored), trains an [`NgramModel`] with the *real*
//! engine logic, and writes the compact `DSN1` binary to the path in argv[1].
//! This is the ONE place the corpus is parsed and trained; the shipped library
//! only ever decodes the result, so training stays single-sourced here.
//!
//! Regenerate the embedded baseline from the repo root:
//!
//! ```sh
//! python3 core/data/gen_corpus.py \
//!     | cargo run --release --example bake_seed -- core/data/seed_model.bin
//! ```

use std::io::Read;

use dsime::ngram::{NgramModel, DEFAULT_ORDER};

fn main() {
    let out = match std::env::args().nth(1) {
        Some(p) => p,
        None => {
            eprintln!("usage: bake_seed <out.bin>   (corpus TSV on stdin)");
            std::process::exit(2);
        }
    };

    let mut corpus = String::new();
    std::io::stdin()
        .read_to_string(&mut corpus)
        .expect("read corpus TSV from stdin");

    let mut model = NgramModel::with_order(DEFAULT_ORDER);
    let lines = model.train_from_corpus(&corpus);
    let bytes = model.to_seed_bytes();

    std::fs::write(&out, &bytes).expect("write baked model");
    eprintln!(
        "baked {lines} corpus lines (order {DEFAULT_ORDER}) -> {out} ({} bytes)",
        bytes.len()
    );
}

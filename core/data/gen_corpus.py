#!/usr/bin/env python3
"""
Build seed_corpus.tsv from open-source Chinese frequency data.

SOURCES (all open-source / MIT or Apache 2.0):
  1. jieba word frequency dictionary — MIT License
       https://github.com/fxsjy/jieba
       General-purpose ~350K word list trained on news + web; primary source.
  2. THUOCL — Apache 2.0 License
       https://github.com/thunlp/THUOCL  (Tsinghua Open Chinese Lexicon)
       Domain-specific frequency lists.  Only IT, food, and place-names (diming)
       are included — medical and law are skipped because their specialised
       vocabulary causes pinyin-ambiguity collisions with common words
       (e.g. 泄泻 vs 谢谢 both romanise to "xiexie").
  3. pypinyin — MIT License
       https://github.com/mozillazg/python-pinyin
       Chinese → toneless ASCII pinyin.
  4. Conversational supplement — MIT (same as this repo)
       Phrases underweighted in news corpora: greetings, everyday expressions,
       social / mobile-messaging vocabulary.

NOTE on other considered sources:
  - Google Web 5-gram (LDC2010T14): requires an LDC license, not free.
  - Google Books Chinese n-gram: free, but per-year/POS-tagged format needs
    heavy pre-processing and still only covers printed books, not web text.
  - WeNet / Kaldi ARPA LMs: excellent data, but stored as KenLM binary or
    ARPA log-prob files inside large model archives — parsing them would
    require an additional dependency (KenLM or a custom ARPA reader) and a
    separate data-fetch step. Worth revisiting if we ever want a proper
    trigram+ LM.

WEIGHT POLICY (avoids domain-bias collisions):
  - jieba entries: log-scaled from jieba frequency, 1–10.
  - THUOCL entries: ONLY entries absent from jieba top-N are added, at a
    conservative weight (THUOCL_W, default 3).  This ensures THUOCL never
    overrides jieba's globally-calibrated frequencies for shared pinyin
    sequences.
  - Supplement entries: hand-assigned weight, or jieba weight if known.
  - Highest weight wins on any (pinyin, hanzi) conflict.

TARGET SIZE: ~12 000–16 000 entries → ~400 KB embedded in the binary.

Run from any directory:
    python3 core/data/gen_corpus.py
"""

import urllib.request
import re, math, sys
from pathlib import Path
from collections import defaultdict
from pypinyin import lazy_pinyin, Style

# ── Config ────────────────────────────────────────────────────────────────────

OUT          = Path(__file__).parent / "seed_corpus.tsv"
JIEBA_URL    = "https://raw.githubusercontent.com/fxsjy/jieba/master/jieba/dict.txt"
THUOCL_BASE  = "https://raw.githubusercontent.com/thunlp/THUOCL/master/data"
JIEBA_N      = 8000    # top N by jieba frequency (covers freq ≥ 873)
DIMING_TOP   = 2000    # only the N most-frequent place names (cap obscure villages)
MAX_WORD_LEN = 5
MIN_FREQ     = 3
MAX_WEIGHT   = 10
THUOCL_W     = 3       # conservative weight for THUOCL-only entries

HAN_RE = re.compile(r"^[一-鿿㐀-䶿\U00020000-\U0002a6df]+$")

# ── Conversational supplement ─────────────────────────────────────────────────

SUPPLEMENT: list[tuple[str, int]] = [
    ("你好",5), ("你好吗",3), ("再见",5), ("拜拜",3),
    ("谢谢",6), ("谢谢你",4), ("不谢",3), ("不用谢",3),
    ("对不起",5), ("没关系",4), ("请问",4), ("打扰了",3),
    ("早上好",4), ("晚上好",4), ("晚安",4), ("加油",5),
    ("没问题",5), ("好的",5), ("可以",5), ("行",4),
    ("嗯",4), ("哦",3), ("啊",4),
    ("吃饭",5), ("喝水",4), ("睡觉",5), ("起床",4),
    ("回家",5), ("上班",5), ("下班",5), ("上学",4),
    ("买东西",3), ("打电话",4), ("发消息",4), ("看电视",4),
    ("做饭",4), ("洗澡",4), ("刷牙",3), ("跑步",4),
    ("手机",6), ("电脑",6), ("网络",5), ("上网",5),
    ("下载",5), ("软件",5), ("应用",5), ("密码",5),
    ("登录",5), ("注册",4), ("搜索",5), ("百度",5),
    ("微信",6), ("微博",5), ("邮件",5), ("邮箱",4),
    ("视频",5), ("音乐",5), ("图片",5), ("文件",5),
    ("网站",5), ("程序",5), ("代码",5), ("数据",5),
    ("系统",5), ("设置",5), ("更新",5), ("安装",5),
    ("人工智能",4), ("机器学习",3), ("深度学习",3),
    ("北京",6), ("上海",6), ("广州",5), ("深圳",5),
    ("机场",5), ("火车站",4), ("地铁",5), ("公交车",4),
    ("出租车",4), ("共享单车",3), ("飞机",5), ("高铁",5),
    ("米饭",5), ("面条",5), ("饺子",4), ("包子",4),
    ("汤",5), ("菜",5), ("水果",5), ("蔬菜",4),
    ("咖啡",5), ("茶",5), ("啤酒",4), ("牛奶",5),
    ("外卖",5), ("餐厅",5), ("超市",5),
    ("天气",5), ("下雨",5), ("下雪",4), ("晴天",4),
    ("喜欢",6), ("爱",6), ("讨厌",4), ("高兴",5),
    ("伤心",4), ("担心",4), ("紧张",4), ("开心",5),
    ("累了",4), ("疼",4), ("生病",4), ("医院",5),
    ("多少钱",5), ("便宜",5), ("贵",5), ("打折",4),
    ("支付",5), ("付款",5), ("红包",5),
    ("作业",5), ("考试",5), ("成绩",4), ("学习",5),
    ("工作",6), ("老师",5), ("学生",5), ("同学",5),
    ("会议",5), ("报告",5), ("文档",5), ("项目",5),
    ("妈妈",6), ("爸爸",6), ("孩子",5), ("朋友",5),
    ("男朋友",4), ("女朋友",4), ("老公",4), ("老婆",4),
    ("同事",5), ("领导",4), ("老板",4),
    ("生日快乐",4), ("新年快乐",4), ("节日快乐",3),
    ("身体健康",3), ("万事如意",3), ("恭喜发财",3),
    ("我爱你",5), ("想你",4), ("爱你",4),
]

# ── Helpers ───────────────────────────────────────────────────────────────────

def fetch(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "curl/7.88"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read().decode("utf-8", errors="replace")

def to_pinyin(word: str) -> str | None:
    sylls = lazy_pinyin(word, style=Style.NORMAL, errors="ignore")
    if len(sylls) != len(word):
        return None
    for s in sylls:
        if not s.isascii() or not s.isalpha():
            return None
    return "".join(sylls)

# ── 1. jieba (primary source) ─────────────────────────────────────────────────

print("Downloading jieba (MIT) …", flush=True)
jieba_freq: dict[str, int] = {}
for line in fetch(JIEBA_URL).splitlines():
    parts = line.split()
    if len(parts) < 2:
        continue
    word, freq_str = parts[0], parts[1]
    try:
        freq = int(freq_str)
    except ValueError:
        continue
    if freq >= MIN_FREQ and HAN_RE.match(word) and len(word) <= MAX_WORD_LEN:
        jieba_freq[word] = freq

top_jieba = sorted(jieba_freq.items(), key=lambda x: -x[1])[:JIEBA_N]
freq_floor = top_jieba[-1][1]
print(f"  {len(top_jieba)} entries, freq {freq_floor}–{top_jieba[0][1]}", flush=True)

all_freqs = [f for _, f in top_jieba]
max_log   = math.log1p(max(all_freqs))
min_log   = math.log1p(min(all_freqs))

def jieba_weight(freq: int) -> int:
    if max_log == min_log:
        return 5
    w = (math.log1p(freq) - min_log) / (max_log - min_log)
    return max(1, round(1 + w * (MAX_WEIGHT - 1)))

# (pinyin, hanzi) → weight
combined: dict[tuple[str, str], int] = {}

def add(py: str | None, word: str, w: int) -> None:
    if py is None:
        return
    key = (py, word)
    if w > combined.get(key, 0):
        combined[key] = w

# jieba pinyin set — used to prevent THUOCL from overriding jieba entries
jieba_py_set: set[str] = set()

skipped_j = 0
for word, freq in top_jieba:
    py = to_pinyin(word)
    if py:
        add(py, word, jieba_weight(freq))
        jieba_py_set.add(py)
    else:
        skipped_j += 1
print(f"  jieba pairs: {len(combined)} (skipped {skipped_j})", flush=True)

# ── 2. THUOCL (supplement only — new pinyin sequences absent from jieba) ──────
# Domains chosen: IT (tech), food, diming (place names).
# Medical and law are excluded: their specialised vocabulary collides with
# common pinyin sequences (e.g. 泄泻/谢谢 both map to "xiexie").

THUOCL_DOMAINS: dict[str, int | None] = {
    "THUOCL_IT.txt":   None,       # all entries
    "THUOCL_food.txt": None,
    "THUOCL_diming.txt": DIMING_TOP,  # cap to top N
}

print("Downloading THUOCL (Apache 2.0) …", flush=True)
thuocl_added = thuocl_skip = 0
for fname, top_n in THUOCL_DOMAINS.items():
    try:
        text = fetch(f"{THUOCL_BASE}/{fname}")
    except Exception as e:
        print(f"  {fname}: SKIP ({e})", flush=True)
        continue
    entries: list[tuple[str, int]] = []
    for line in text.splitlines():
        parts = line.strip().split()
        if len(parts) < 2:
            continue
        word = parts[0]
        try:
            count = int(parts[1])
        except ValueError:
            continue
        if HAN_RE.match(word) and 1 < len(word) <= MAX_WORD_LEN:
            entries.append((word, count))
    entries.sort(key=lambda x: -x[1])
    if top_n:
        entries = entries[:top_n]
    new_from_domain = 0
    for word, _ in entries:
        py = to_pinyin(word)
        if py is None:
            thuocl_skip += 1
            continue
        # Only add if this pinyin sequence is NOT already covered by jieba,
        # to avoid domain-frequency bias overriding globally-calibrated weights.
        if py not in jieba_py_set:
            add(py, word, THUOCL_W)
            jieba_py_set.add(py)   # prevent same-py from another THUOCL domain
            thuocl_added += 1
            new_from_domain += 1
    print(f"  {fname}: {len(entries)} loaded, {new_from_domain} new pinyin seqs", flush=True)

print(f"  THUOCL total added: {thuocl_added} (skipped {thuocl_skip} non-pinyin)", flush=True)

# ── 3. Supplement ─────────────────────────────────────────────────────────────

supp_new = supp_up = 0
for word, w in SUPPLEMENT:
    py = to_pinyin(word)
    if py is None:
        continue
    if word in jieba_freq:
        w = max(w, jieba_weight(jieba_freq[word]))
    key = (py, word)
    if key not in combined:
        supp_new += 1
    elif w > combined[key]:
        supp_up += 1
    add(py, word, w)
print(f"Supplement: {supp_new} new, {supp_up} upgraded", flush=True)
print(f"Total unique (pinyin, hanzi) pairs: {len(combined)}", flush=True)

# ── 4. Write TSV ──────────────────────────────────────────────────────────────

rows_sorted = sorted(combined.items(), key=lambda x: (-x[1], x[0][0], x[0][1]))

HEADER = """\
# Seed corpus for the local speculative n-gram model (core/src/ngram.rs).
#
# SOURCES (open-source):
#   jieba word frequency dictionary  (MIT License)
#     https://github.com/fxsjy/jieba
#   THUOCL — Tsinghua Open Chinese Lexicon  (Apache 2.0)
#     https://github.com/thunlp/THUOCL  — IT, food, diming (place names) only
#   pypinyin  (MIT License)
#     https://github.com/mozillazg/python-pinyin
#   Conversational supplement  (MIT, same as this repository)
#
# FORMAT: pinyin<TAB>hanzi[<TAB>weight]
#   weight 1–10 (log-scaled from jieba frequency, or rank-based for THUOCL)
#   no weight column = counted once
#   lines starting with '#' or blank lines are ignored
#
# Regenerate: python3 core/data/gen_corpus.py
"""

rows: list[str] = []
for (py, hanzi), w in rows_sorted:
    rows.append(f"{py}\t{hanzi}\t{w}" if w > 1 else f"{py}\t{hanzi}")

with open(OUT, "w", encoding="utf-8") as f:
    f.write(HEADER)
    f.write("\n")
    f.write("\n".join(rows))
    f.write("\n")

print(f"\nWritten {len(rows)} entries → {OUT}", flush=True)

from collections import Counter
wdist = Counter(int(r.split("\t")[2]) if len(r.split("\t")) == 3 else 1 for r in rows)
ldist = Counter(len(r.split("\t")[1]) for r in rows)
size_kb = OUT.stat().st_size // 1024
print(f"File size: {size_kb} KB")
print(f"Weight dist:   {dict(sorted(wdist.items()))}")
print(f"Word-len dist: {dict(sorted(ldist.items()))}")

checks = ["nihao","zaijian","xiexie","zhongguo","beijing","gongzuo",
          "shouji","wangluo","shengrikuaile","rengongzhineng","chengdu"]
rows_by_py: dict[str, list] = defaultdict(list)
for r in rows:
    rows_by_py[r.split("\t")[0]].append(r)
print("\nSpot-check:")
for p in checks:
    print(f"  {p}: {rows_by_py.get(p, ['(missing)'])[:3]}")

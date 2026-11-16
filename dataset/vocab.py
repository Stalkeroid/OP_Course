"""Vocabulary construction for the dataset generator (standard library only)."""

import os
import random

VOCAB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vocab")
GENERAL = "general"
VOWELS = "aeiou"


def read_words(name):
    """Read one word per line, dropping blanks and duplicates, keeping order."""
    seen, words = set(), []
    with open(os.path.join(VOCAB_DIR, name + ".txt"), encoding="utf-8") as fh:
        for line in fh:
            w = line.strip().lower()
            if w and w not in seen:
                seen.add(w)
                words.append(w)
    return words


def topic_names():
    names = [f[:-4] for f in sorted(os.listdir(VOCAB_DIR)) if f.endswith(".txt")]
    return [n for n in names if n != GENERAL]


def derived_forms(word):
    """Plural, -ing, -ed, -er, -ly forms by simple spelling rules."""
    if len(word) < 3 or word.endswith(("ing", "ed", "ly", "er", "s")):
        return []
    stem = word[:-1] if word.endswith("e") else word
    if word.endswith(("s", "x", "ch", "sh")):
        plural = word + "es"
    elif word.endswith("y") and word[-2] not in VOWELS:
        plural = word[:-1] + "ies"
    else:
        plural = word + "s"
    ing = stem + "ing"
    ed = word + "d" if word.endswith("e") else stem + "ed"
    er = word + "r" if word.endswith("e") else stem + "er"
    ly = (word[:-1] + "ily") if word.endswith("y") else word + "ly"
    return [plural, ing, ed, er, ly]


def zipf_cum_weights(n, s=1.0):
    total, cum = 0.0, []
    for rank in range(1, n + 1):
        total += 1.0 / rank**s
        cum.append(total)
    return cum


def build_topic_pool(base, rng, compounds=1500):
    """Ordered pool: base words (head), derived forms, then compounds, codes (tail)."""
    pool = list(base)
    for w in base:
        pool.extend(derived_forms(w))
    for _ in range(compounds):
        a, b = rng.choice(base), rng.choice(base)
        if a != b:
            pool.append(a + "-" + b)
    for _ in range(compounds // 3):
        pool.append(
            "model %s%d" % (rng.choice("abcdefghjkmnpqrstuvwxyz"), rng.randint(10, 999))
        )
    pool.extend(str(y) for y in range(1950, 2031))
    return list(dict.fromkeys(pool))


def build_general_pool():
    base = read_words(GENERAL)
    pool = list(base)
    for w in base:
        pool.extend(derived_forms(w))
    pool.extend(str(n) for n in range(0, 1000))
    return list(dict.fromkeys(pool))

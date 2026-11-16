# Dataset generator

Deterministic generator of an English-like corpus (standard library only).

    python3 dataset/generate.py --out DIR --size-mb 2100 --docs 20000 --seed 42

Output: `DIR/<topic>/doc_NNNNNN.txt` (16 everyday topics, UTF-8), so a `doc_id` looks
like `science/doc_000123.txt`. The marker file `DIR/.complete` (JSON with size, docs,
seed) is written last. With `--if-needed` the script exits at once if the marker
matches the requested size/docs/seed; otherwise it wipes `DIR` and regenerates.

Vocabulary: `vocab/<topic>.txt` and `vocab/general.txt` (one word per line); the
script derives plurals, -ing/-ed/-er/-ly forms, hyphenated compounds, years and
model codes (about 38k distinct terms). Words are drawn with a Zipf-like
distribution; document sizes vary +-50% around size/docs.

In Docker, the `dataset-init` compose service runs it into the `dataset` volume.
Development: `DATASET_SIZE_MB=100 DATASET_DOCS=2000 docker compose up`.

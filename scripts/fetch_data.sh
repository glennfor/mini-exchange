#!/usr/bin/env bash
# Downloads the market data samples this project validates against.
#
# **Nothing downloaded here is ever committed.** `data/` is gitignored. Market
# data files are hundreds of megabytes, they are not ours to redistribute, and
# a repository that carries them cannot be cloned in under five minutes.
#
#     ./scripts/fetch_data.sh lobster     # LOBSTER sample day (free)
#     ./scripts/fetch_data.sh itch        # Nasdaq TotalView-ITCH 5.0 sample
#     ./scripts/fetch_data.sh all
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA_DIR="${MINIEX_DATA_DIR:-$ROOT/data}"
mkdir -p "$DATA_DIR"

LOBSTER_URL="https://lobsterdata.com/info/sample/LOBSTER_SampleFile_AMZN_2012-06-21_10.zip"
ITCH_URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"

have() { command -v "$1" >/dev/null 2>&1; }

fetch() {
  local url="$1" dest="$2"
  if [ -f "$dest" ]; then
    echo "already have $dest"
    return 0
  fi
  echo "fetching $url"
  if have curl; then
    curl -fL --progress-bar -o "$dest.part" "$url"
  elif have wget; then
    wget -q --show-progress -O "$dest.part" "$url"
  else
    echo "need curl or wget" >&2
    return 1
  fi
  mv "$dest.part" "$dest"
}

fetch_lobster() {
  local zip="$DATA_DIR/lobster_sample.zip"
  cat <<'NOTE'

LOBSTER sample data
-------------------
lobsterdata.com publishes free sample files (one symbol, one day, 1/5/10/30
levels). The URL below is the AMZN 2012-06-21 10-level sample. If it has moved,
get the current link from https://lobsterdata.com/info/DataSamples.php and pass
it as MINIEX_LOBSTER_URL.

Each sample is a pair, in lockstep, one orderbook row per message:
  <SYMBOL>_<DATE>_<START>_<END>_message_<LEVELS>.csv
  <SYMBOL>_<DATE>_<START>_<END>_orderbook_<LEVELS>.csv

NOTE
  fetch "${MINIEX_LOBSTER_URL:-$LOBSTER_URL}" "$zip"
  if have unzip; then
    unzip -o -q "$zip" -d "$DATA_DIR/lobster"
    echo "extracted to $DATA_DIR/lobster:"
    ls -la "$DATA_DIR/lobster"
  else
    echo "unzip not found; extract $zip yourself into $DATA_DIR/lobster" >&2
  fi
}

fetch_itch() {
  local gz="$DATA_DIR/nasdaq_itch50.gz"
  cat <<'NOTE'

Nasdaq TotalView-ITCH 5.0 sample
--------------------------------
Nasdaq publishes full-day samples at https://emi.nasdaq.com/ITCH/. They are
several gigabytes compressed and considerably more expanded, so this is a
deliberate, explicit download.

Also: obtain the official TotalView-ITCH 5.0 specification PDF from
nasdaqtrader.com and check every field offset in feed/itch50.hpp against it.
The table in docs/itch-notes.md is a working reference, not an authority.

NOTE
  fetch "${MINIEX_ITCH_URL:-$ITCH_URL}" "$gz"
  echo "decompress with: gunzip -k $gz"
}

case "${1:-all}" in
  lobster) fetch_lobster ;;
  itch)    fetch_itch ;;
  all)     fetch_lobster; fetch_itch ;;
  *)
    echo "usage: $0 [lobster|itch|all]" >&2
    exit 2
    ;;
esac

cat <<NOTE

Next steps
----------
  ./build/release/tools/itch_stats --file $DATA_DIR/<file>.NASDAQ_ITCH50
  ./build/release/tools/book_dump --file <itch> --locate <n> --levels 10 --stats
  ./build/release/tools/lobster_check \\
      --messages $DATA_DIR/lobster/<...>_message_10.csv \\
      --book     $DATA_DIR/lobster/<...>_orderbook_10.csv \\
      --levels 10

A LOBSTER sample that starts at 09:30 begins mid-session only if the file was
cut that way; if lobster_check reports unknown references, that is what
happened. See docs/validation.md.
NOTE

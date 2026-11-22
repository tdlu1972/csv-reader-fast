# csv-reader-fast

[![make.fun · csv-reader-speed-01-run-20261121](https://make.fun/badge/csv-reader-speed-01-run-20261121.svg)](https://make.fun/bounty/csv-reader-speed-01-run-20261121?utm_source=github&utm_medium=readme&utm_campaign=csv-reader-speed-01-run-20261121)
[![rank](https://make.fun/badge/csv-reader-speed-01-run-20261121/tdlu1972/csv-reader-fast/rank.svg)](https://make.fun/bounty/csv-reader-speed-01-run-20261121/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=csv-reader-speed-01-run-20261121)
[![reward](https://make.fun/badge/csv-reader-speed-01-run-20261121/tdlu1972/csv-reader-fast/reward.svg)](https://make.fun/bounty/csv-reader-speed-01-run-20261121?utm_source=github&utm_medium=readme&utm_campaign=csv-reader-speed-01-run-20261121)

A faster drop-in for csv-reader 1.0.0 that returns identical `{columns, rows, errors}` answers, broken rows and all.

`main.ts` never parses the input JSON. `csv.wasm` (built from `csv.c` with `build.sh`: clang's wasm32 backend, SIMD128) reads the bytes of `cases.json` straight into its memory and transduces each case's JSON-escaped text into the JSON of its answer in one left-to-right pass: one 16-byte SIMD compare finds the next comma, backslash or quote, plain bytes are copied a block at a time, and every other escape is copied as it is spelled. An input spelled in a way the transducer does not expect (a `\u` escape for a comma, quote or line break, an unexpected case shape) makes it trap, and `main.ts` then answers the file the house way with `solve.js`, a slice-based single pass over char codes.

## Run it

```sh
./build.sh   # only after changing csv.c; csv.wasm is committed
MAKEFUN_INPUT_DIR=practice/input MAKEFUN_OUTPUT_DIR=out node main.ts
```

---

<sub>Entered on [make.fun](https://make.fun/?ref=psm22q7h&utm_source=github&utm_medium=readme&utm_campaign=csv-reader-speed-01-run-20261121) · AI agents build software for cash rewards</sub>

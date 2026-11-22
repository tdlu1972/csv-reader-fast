// main.ts's fallback, for a file csv.wasm traps on: answers it the house way, JSON.parse, solve.js's run() per case,
// JSON.stringify. Kept out of main.ts, which then needs no top-level await and starts faster.
import { readFileSync, writeFileSync } from 'node:fs';
import { setFlagsFromString } from 'node:v8';
import { run } from './solve.js';

setFlagsFromString('--no-incremental-marking');
const env = process.env;
const INPUT = (env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const OUTPUT = (env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';
const cases = JSON.parse(readFileSync(INPUT, 'utf8'));
const answers = [];
for (const c of cases) answers.push(run(c.input));
writeFileSync(OUTPUT, JSON.stringify(answers));

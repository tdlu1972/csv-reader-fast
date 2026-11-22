// The program the harness runs (`node /bot/main.ts`, node22): reads every case from /input/cases.json and writes
// /output/answers.json, csv-reader 1.0.0's answer for each, in order. Outside a container MAKEFUN_INPUT_DIR and
// MAKEFUN_OUTPUT_DIR stand in for /input and /output.
//
// csv.wasm (built from csv.c by build.sh) reads the file's bytes into its own memory and writes the whole answers file
// there, never decoding a string. Where it traps (an input spelled in a way it does not handle), the file is answered
// the house way instead: JSON.parse, solve.js's run() per case, JSON.stringify.

import { closeSync, fstatSync, openSync, readFileSync, readSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { setFlagsFromString } from 'node:v8';

// run() is called once, so a TurboFan tier-up of its hot functions costs more compile time than it saves: stay in Liftoff.
setFlagsFromString('--liftoff-only');

const INPUT = join(process.env.MAKEFUN_INPUT_DIR ?? '/input', 'cases.json');
const OUTPUT = join(process.env.MAKEFUN_OUTPUT_DIR ?? '/output', 'answers.json');

let answered = false;
try {
  const wasm = new WebAssembly.Instance(new WebAssembly.Module(readFileSync(new URL('./csv.wasm', import.meta.url)))).exports as any;
  const fd = openSync(INPUT, 'r');
  const size = fstatSync(fd).size;
  const at = wasm.input(size); // may grow memory, detaching any buffer taken before it
  const into = new Uint8Array(wasm.memory.buffer, at, size);
  let got = 0;
  while (got < size) {
    const k = readSync(fd, into, got, size - got, null);
    if (k === 0) break;
    got += k;
  }
  closeSync(fd);
  if (got === size) {
    wasm.run();
    writeFileSync(OUTPUT, new Uint8Array(wasm.memory.buffer, wasm.out_ptr(), wasm.out_len()));
    answered = true;
  }
} catch {}

if (!answered) {
  setFlagsFromString('--no-incremental-marking');
  const { run } = (await import(new URL('./solve.js', import.meta.url).href)) as { run: (input: unknown) => unknown };
  const cases = JSON.parse(readFileSync(INPUT, 'utf8')) as { input: unknown }[];
  const answers: unknown[] = [];
  for (const c of cases) answers.push(run(c.input));
  writeFileSync(OUTPUT, JSON.stringify(answers));
}

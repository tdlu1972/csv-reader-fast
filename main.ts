// The program the harness runs (`node /bot/main.ts`, node22): reads every case from /input/cases.json and writes
// /output/answers.json, csv-reader 1.0.0's answer for each, in order. Outside a container MAKEFUN_INPUT_DIR and
// MAKEFUN_OUTPUT_DIR stand in for /input and /output.
//
// csv.wasm (built from csv.c by build.sh) reads the file's bytes into its own memory and hands back the answers file a
// case at a time, never decoding a string. Where it traps (an input spelled in a way it does not handle), the file is
// answered the house way instead, by fallback.js.
//
// Built-in modules come from process.getBuiltinModule rather than import statements: an ES module with none starts
// about as fast as an empty one.

const fs = process.getBuiltinModule('node:fs');
const env = process.env;
const INPUT = (env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const OUTPUT = (env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';

// run() is called once, so a TurboFan tier-up of its hot functions costs more compile time than it saves: stay in Liftoff.
process.getBuiltinModule('node:v8').setFlagsFromString('--liftoff-only');

let answered = false;
let out = -1;
try {
  let memory: WebAssembly.Memory;
  const flush = (p: number, n: number) => {
    fs.writeSync(out, new Uint8Array(memory.buffer, p, n));
  };
  const wasm = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(new URL('./csv.wasm', import.meta.url))), { env: { flush } }).exports as any;
  memory = wasm.memory;
  const fd = fs.openSync(INPUT, 'r');
  const size = fs.fstatSync(fd).size;
  const at = wasm.input(size); // may grow memory, detaching any buffer taken before it
  const into = new Uint8Array(memory.buffer, at, size);
  let got = 0;
  while (got < size) {
    const k = fs.readSync(fd, into, got, size - got, null);
    if (k === 0) break;
    got += k;
  }
  fs.closeSync(fd);
  if (got === size) {
    out = fs.openSync(OUTPUT, 'w');
    wasm.run();
    flush(wasm.out_ptr(), wasm.out_len());
    fs.closeSync(out);
    answered = true;
  }
} catch {}

if (!answered) import(new URL('./fallback.js', import.meta.url).href);

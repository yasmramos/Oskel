// Runs an Oskel WebAssembly module and forwards its output.
//
// The module imports four functions:
//
//   (import "oskel" "write" (func (param i32 i32 i32) (result i32)))
//   (import "oskel" "exit"  (func (param i32)))
//   (import "oskel" "time"  (func (result i64)))
//   (import "oskel" "clock" (func (result f64)))
//
// `write(fd, ptr, len)` reads `len` bytes of the module's exported `memory`
// and writes them to the matching stream; `exit(code)` terminates the process
// with `code`; `time()` is the epoch second and `clock()` a monotonic second
// count. `_start` is the entry point.
'use strict';

const fs = require('fs');

class ExitSignal extends Error {
  constructor(code) {
    super(`exit ${code}`);
    this.code = code;
  }
}

let instance = null;

const imports = {
  oskel: {
    write(fd, ptr, len) {
      // The JS API hands an `i32` parameter over as a *signed* number, but a
      // linear-memory address is unsigned: once the arena passes 2 GiB `ptr`
      // arrives negative and `Uint8Array` rejects it. Mask both.
      const view = new Uint8Array(instance.exports.memory.buffer, ptr >>> 0, len >>> 0);
      const bytes = Buffer.from(view);
      if ((fd >>> 0) === 2) process.stderr.write(bytes);
      else process.stdout.write(bytes);
      return len;
    },
    exit(code) {
      throw new ExitSignal(code >>> 0);
    },
    time() {
      return BigInt(Math.floor(Date.now() / 1000));
    },
    clock() {
      return performance.now() / 1000;
    },
  },
};

async function main() {
  const file = process.argv[2];
  if (!file) {
    process.stderr.write('usage: run_wasm.cjs <module.wasm>\n');
    return 64;
  }
  let bytes;
  try {
    bytes = fs.readFileSync(file);
  } catch (error) {
    process.stderr.write(`run_wasm: cannot read '${file}': ${error.message}\n`);
    return 66;
  }
  if (!WebAssembly.validate(bytes)) {
    process.stderr.write(`run_wasm: '${file}' is not a valid WebAssembly module\n`);
    return 65;
  }
  try {
    const compiled = await WebAssembly.instantiate(bytes, imports);
    instance = compiled.instance;
    instance.exports._start();
    return 0;
  } catch (error) {
    if (error instanceof ExitSignal) return error.code;
    process.stderr.write(`run_wasm: ${error && error.stack ? error.stack : error}\n`);
    return 70;
  }
}

main().then((code) => {
  process.exitCode = code;
});

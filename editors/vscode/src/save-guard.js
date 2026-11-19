'use strict';

// Keeps a programmatic save from starting a second check.
//
// `document.save()` fires `onDidSaveTextDocument`. Whether that event arrives
// before or after the promise resolves is not documented, so a flag cleared in
// `finally` suppresses the event in only one of the two orderings and leaks a
// duplicate check in the other — two compiler processes for one save, with the
// second killing the first.
//
// Counting the expected events removes the dependency on the ordering: the
// event consumes an expectation whenever it arrives, and a save that throws
// cancels its own expectation because no event will follow.
//
// No `vscode` import, so both orderings are unit-tested directly.

class SaveGuard {
  constructor() {
    this.expected = new Map();
  }

  expect(uri) {
    this.expected.set(uri, (this.expected.get(uri) || 0) + 1);
  }

  cancel(uri) {
    const pending = this.expected.get(uri) || 0;
    if (pending <= 1) this.expected.delete(uri);
    else this.expected.set(uri, pending - 1);
  }

  // True when this save was started here, so the hook must not check again.
  consume(uri) {
    if ((this.expected.get(uri) || 0) === 0) return false;
    this.cancel(uri);
    return true;
  }
}

module.exports = { SaveGuard };

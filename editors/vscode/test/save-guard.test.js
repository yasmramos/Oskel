'use strict';

const test = require('node:test');
const assert = require('node:assert');

const { SaveGuard } = require('../src/save-guard.js');

const URI = 'file:///a.oskl';

test('an expected save is consumed, so the hook does not check twice', () => {
  const guard = new SaveGuard();
  guard.expect(URI);
  assert.strictEqual(guard.consume(URI), true);
  assert.strictEqual(guard.consume(URI), false, 'one save, one suppression');
});

test('a save the user made is not suppressed', () => {
  const guard = new SaveGuard();
  assert.strictEqual(guard.consume(URI), false);
});

test('ordering A: the event arrives before save() resolves', () => {
  // expect -> event -> cleanup. The expectation is already gone; the cleanup
  // must not cancel somebody else's expectation.
  const guard = new SaveGuard();
  guard.expect(URI);
  assert.strictEqual(guard.consume(URI), true);
  guard.cancel(URI);
  assert.strictEqual(guard.consume(URI), false);
});

test('ordering B: the event arrives after save() resolves', () => {
  // expect -> cleanup -> event. The expectation must survive the cleanup for
  // the event to be suppressed.
  const guard = new SaveGuard();
  guard.expect(URI);
  guard.cancel(URI);
  assert.strictEqual(guard.consume(URI), false);
});

test('a save that throws cancels its expectation, so no stale suppression', () => {
  const guard = new SaveGuard();
  guard.expect(URI);
  guard.cancel(URI);
  assert.strictEqual(guard.consume(URI), false, 'the next real save must check');
});

test('two nested saves for one document each suppress their own event', () => {
  const guard = new SaveGuard();
  guard.expect(URI);
  guard.expect(URI);
  assert.strictEqual(guard.consume(URI), true);
  assert.strictEqual(guard.consume(URI), true);
  assert.strictEqual(guard.consume(URI), false);
});

test('documents are independent', () => {
  const guard = new SaveGuard();
  guard.expect(URI);
  assert.strictEqual(guard.consume('file:///b.oskl'), false);
  assert.strictEqual(guard.consume(URI), true);
});

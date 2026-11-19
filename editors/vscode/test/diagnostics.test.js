'use strict';

// Fixtures below are the compiler's real stderr, captured from `oskel -c`.
// Keeping them literal is the point: a regex that only matches a tidied-up
// version of the output would pass here and fail in the editor.

const test = require('node:test');
const assert = require('node:assert');

const { parseDiagnostics, messageForFailure, clamp } = require('../src/diagnostics.js');

const TYPE_ERROR = `bad.oskl:3:7: error: cannot assign 'string' to 'int' in 'x'
  |
3 |   x = "no";
  |       ^
`;

const WARNING = `warn.oskl:4:4: warning: the result of '@MustUse' function 'f' is discarded; assign it to '_' to acknowledge
  |
4 |   f();
  |    ^
`;

const LEXER_ERROR = `lex.oskl:2:16: error: unknown escape sequence '\\q'
  |
2 |   var s = "bad \\q";
  |                ^
`;

const SOURCE = 'fn main() {\n  var x = 1;\n  x = "no";\n}\n';

// The warning names line 4, column 4, which in the real file is `  f();`. The
// fixture keeps that line where the diagnostic points, so the assertion is
// about parsing and not about clamping.
const WARNING_SOURCE = 'fn f(): int => 1;\nfn main() {\n\n  f();\n}\n';

test('parses an error with its 1-based position and message', () => {
  const found = parseDiagnostics(TYPE_ERROR, SOURCE);
  assert.strictEqual(found.length, 1);
  assert.deepStrictEqual(found[0], {
    line: 3,
    column: 7,
    severity: 'error',
    message: "cannot assign 'string' to 'int' in 'x'",
  });
});

test('parses a warning as a warning, not an error', () => {
  const found = parseDiagnostics(WARNING, WARNING_SOURCE);
  assert.strictEqual(found.length, 1);
  assert.strictEqual(found[0].severity, 'warning');
  assert.strictEqual(found[0].line, 4);
  assert.strictEqual(found[0].column, 4);
  assert.match(found[0].message, /@MustUse/);
});

test('keeps a message that contains quotes and backslashes intact', () => {
  const found = parseDiagnostics(LEXER_ERROR, SOURCE);
  assert.strictEqual(found.length, 1);
  assert.strictEqual(found[0].message, "unknown escape sequence '\\q'");
});

test('ignores the caret snippet lines', () => {
  const found = parseDiagnostics(TYPE_ERROR, SOURCE);
  assert.strictEqual(found.length, 1, 'the snippet must not parse as a second problem');
});

test('parses several problems from one run', () => {
  const both = TYPE_ERROR + WARNING;
  const found = parseDiagnostics(both, SOURCE);
  assert.strictEqual(found.length, 2);
  assert.deepStrictEqual(
    found.map((d) => d.severity),
    ['error', 'warning']
  );
});

test('handles a Windows path, whose drive letter is a colon too', () => {
  // The greedy `(.*)` up to the last `:<line>:<col>:` is what makes the drive
  // letter a non-issue; a non-greedy or `[^:]*` file group would fail here.
  const output = 'C:\\src\\a.oskl:3:7: error: boom\n';
  const found = parseDiagnostics(output, SOURCE);
  assert.strictEqual(found.length, 1);
  assert.strictEqual(found[0].line, 3);
  assert.strictEqual(found[0].column, 7);
  assert.strictEqual(found[0].message, 'boom');
});

test('handles CRLF output', () => {
  const found = parseDiagnostics(TYPE_ERROR.replace(/\n/g, '\r\n'), SOURCE);
  assert.strictEqual(found.length, 1);
  assert.strictEqual(found[0].column, 7);
});

test('returns nothing for output with no problems', () => {
  assert.deepStrictEqual(parseDiagnostics('', SOURCE), []);
  assert.deepStrictEqual(parseDiagnostics('oskel: some note\n', SOURCE), []);
});

test('clamps a position past the end of the document', () => {
  // The real case: an error inside an imported module is reported under the
  // root file's name with the module's line, so line 9 does not exist here.
  // `'fn main() {}\n'` is two lines because the trailing newline opens a last
  // empty one, so the clamp lands on line 2, column 1.
  const output = 'root.oskl:9:42: error: cannot assign\n';
  const found = parseDiagnostics(output, 'fn main() {}\n');
  assert.strictEqual(found[0].line, 2);
  assert.strictEqual(found[0].column, 1, 'column is clamped to the line that survived');
});

test('clamps a column past the end of its line', () => {
  const found = parseDiagnostics('a.oskl:2:99: error: boom\n', SOURCE);
  assert.strictEqual(found[0].line, 2);
  assert.strictEqual(found[0].column, 13, 'line 2 is "  var x = 1;" (12 chars) so column 13');
});

test('clamp keeps a valid position untouched', () => {
  assert.deepStrictEqual(clamp({ line: 2, column: 3 }, [5, 12, 4]), { line: 2, column: 3 });
});

test('treats a runtime error as an error', () => {
  const found = parseDiagnostics('a.oskl:1:1: runtime error: division by zero\n', SOURCE);
  assert.strictEqual(found[0].severity, 'error');
});

test('a failed run with output reports the first line', () => {
  const output = "oskelc: '-c' is not implemented yet; this compiler emits native binaries\n";
  assert.strictEqual(
    messageForFailure(output, 64),
    "oskelc: '-c' is not implemented yet; this compiler emits native binaries"
  );
});

test('a failed run with no output still says something', () => {
  assert.match(messageForFailure('   ', 65), /exited with code 65/);
});

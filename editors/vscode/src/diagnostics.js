'use strict';

// Turns the compiler's stderr into VS Code diagnostics.
//
// The compiler prints one header per problem and then a three-line snippet:
//
//   bad.oskl:3:7: error: cannot assign 'string' to 'int' in 'x'
//     |
//   3 |   x = "no";
//     |       ^
//
// Only the header carries a position, so the snippet lines are ignored. The
// file name is greedy up to the last `:<line>:<col>:` triple because a Windows
// path holds a colon of its own (`C:\src\a.oskl:3:7: ...`).
//
// This module deliberately does not import `vscode`: it is pure text in, plain
// objects out, so `node --test` can exercise it without a running editor.

const HEADER = /^(.*):(\d+):(\d+): (error|warning|runtime error): (.*)$/;

// A diagnostic that points past the end of the document would be dropped by the
// editor, so the position is clamped to a line that exists. That happens with a
// real compiler behaviour: an error inside an imported module is reported under
// the *root* file's name with the module's line and column (see
// `Compiler::ImportLoader`), so the position can name a line the open document
// does not have. The message still names the module, so nothing is lost but the
// exact column.
function clamp(position, lengths) {
  const line = Math.min(Math.max(position.line, 1), Math.max(lengths.length, 1));
  // The column is clamped against the line that survived the clamp above, not
  // against the line the compiler named.
  const column = Math.min(Math.max(position.column, 1), (lengths[line - 1] || 0) + 1);
  return { line, column };
}

function lineLengthsOf(source) {
  return source.split('\n').map((text) => (text.endsWith('\r') ? text.length - 1 : text.length));
}

// `parseDiagnostics` returns `{ line, column, severity, message }` with both
// positions 1-based, which is what the compiler reports and what VS Code's
// `Range` does not use — the caller subtracts one.
function parseDiagnostics(output, source) {
  const lengths = lineLengthsOf(source);
  const found = [];
  for (const raw of String(output).split(/\r?\n/)) {
    const match = HEADER.exec(raw);
    if (!match) continue;
    const [, , lineText, columnText, kind, message] = match;
    const position = clamp(
      { line: Number(lineText), column: Number(columnText) },
      lengths
    );
    found.push({
      line: position.line,
      column: position.column,
      severity: kind === 'error' || kind === 'runtime error' ? 'error' : 'warning',
      message: message.trim(),
    });
  }
  return found;
}

// The compiler exits 65 for a compile error and 0 otherwise, warnings included.
// A non-zero exit with nothing parsed means the binary itself refused the
// invocation (a stale `oskelc -c`, a bad flag), and the user has to see why
// instead of a silent "no problems".
function messageForFailure(output, exitCode) {
  const text = String(output).trim();
  if (!text) return `oskel exited with code ${exitCode} and printed nothing.`;
  return text.split(/\r?\n/)[0].trim();
}

module.exports = { parseDiagnostics, messageForFailure, clamp, HEADER };

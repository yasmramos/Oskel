'use strict';

// Runs the compiler in check mode (`oskel -c`) and publishes what it says as
// editor diagnostics, so a type error shows on the line that caused it instead
// of in a terminal the author has to switch to.
//
// There is no language server here on purpose. The compiler already prints
// `file:line:col: error: message` and exits 65, which is everything a
// `DiagnosticCollection` needs; a protocol would add a second process and a
// second vocabulary without adding a single diagnostic. A check costs about
// 2 ms, so one per save is free.
//
// `oskelc` cannot be used: its `-c` is not implemented (it exits 64 with "not
// implemented yet"), so the binary must be `oskel`, the development engine.

const vscode = require('vscode');
const path = require('path');
const { execFile } = require('child_process');
const { parseDiagnostics, messageForFailure } = require('./src/diagnostics.js');
const { SaveGuard } = require('./src/save-guard.js');

const LANGUAGE = 'oskel';
const CONFIG = 'oskel.check';

// A check is a compile, not a run: a file that takes this long is a compiler
// hang, and the editor must not wait on it forever.
const CHECK_TIMEOUT_MS = 30000;

// One running check per document. A save that arrives while a check is in
// flight supersedes it: the older result describes a file the user has already
// changed, so the process is killed and its result discarded.
const inFlight = new Map();

// `document.save()` fires `onDidSaveTextDocument`, which would start a second
// check for the same content and leave the first one's result racing it. The
// guard counts the expected events instead of clearing a flag, because the
// event's ordering relative to `save()` resolving is not documented.
const saveGuard = new SaveGuard();

let collection = null;
let warnedMissingBinary = false;

function config() {
  const settings = vscode.workspace.getConfiguration(CONFIG);
  return {
    enabled: settings.get('enable', true),
    onSave: settings.get('onSave', true),
    binary: (settings.get('path', '') || '').trim(),
  };
}

// Finds the compiler: the configured path first (absolute, or relative to the
// workspace folder), then the `oskel` on `PATH`. `oskel` ships as
// `bin/oskel` + `share/oskel/std`, so a configured path pointing at an
// unpacked archive works too.
function resolveBinary() {
  const { binary } = config();
  if (binary) {
    if (path.isAbsolute(binary)) return binary;
    const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (folder) return path.join(folder.uri.fsPath, binary);
    return binary;
  }
  return process.platform === 'win32' ? 'oskel.exe' : 'oskel';
}

function reportMissingBinary(error) {
  if (warnedMissingBinary) return;
  warnedMissingBinary = true;
  const where = config().binary || 'oskel';
  vscode.window
    .showErrorMessage(
      `Oskel: cannot run '${where}' (${error.code || error.message}). ` +
        'Install Oskel or set "oskel.check.path" to the compiler.',
      'Open settings'
    )
    .then((choice) => {
      if (choice === 'Open settings') {
        vscode.commands.executeCommand('workbench.action.openSettings', 'oskel.check.path');
      }
    });
}

function toVscodeDiagnostics(found) {
  return found.map((item) => {
    // The compiler is 1-based on both axes; `Range` is 0-based. A one-character
    // range is what the caret in the compiler's snippet points at.
    const line = item.line - 1;
    const character = item.column - 1;
    const range = new vscode.Range(line, character, line, character + 1);
    const severity =
      item.severity === 'error'
        ? vscode.DiagnosticSeverity.Error
        : vscode.DiagnosticSeverity.Warning;
    const diagnostic = new vscode.Diagnostic(range, item.message, severity);
    diagnostic.source = 'oskel';
    return diagnostic;
  });
}

// `onStart` receives the child so the caller can kill it when a newer check
// supersedes this one.
function runCheck(binary, file, onStart) {
  return new Promise((resolve) => {
    const child = execFile(
      binary,
      ['-c', file],
      { cwd: path.dirname(file), timeout: CHECK_TIMEOUT_MS, windowsHide: true },
      (error, stdout, stderr) => resolve({ error, stdout, stderr })
    );
    onStart(child);
  });
}

async function check(document) {
  if (!document || document.languageId !== LANGUAGE) return;
  if (document.uri.scheme !== 'file') return;

  if (!config().enabled) {
    collection.delete(document.uri);
    return;
  }

  // A check reads the file from disk, so an unsaved buffer would be checked in
  // its previous state. Saving first is the caller's job (the save hook does it
  // by definition); a manual run on a dirty buffer says so instead of lying.
  if (document.isDirty) {
    const choice = await vscode.window.showWarningMessage(
      'Oskel: the file has unsaved changes; the check uses the version on disk.',
      'Save and check'
    );
    if (choice !== 'Save and check') return;
    const uri = document.uri.toString();
    saveGuard.expect(uri);
    try {
      await document.save();
    } catch (error) {
      // No event will follow a save that failed, so the expectation must go or
      // it would swallow the next real save's check.
      saveGuard.cancel(uri);
      throw error;
    }
  }

  const key = document.uri.toString();
  const previous = inFlight.get(key);
  if (previous && previous.kill) previous.kill();

  const token = {};
  inFlight.set(key, token);
  const binary = resolveBinary();
  const { error, stdout, stderr } = await runCheck(binary, document.uri.fsPath, (child) => {
    token.kill = () => child.kill();
  });

  // A newer check for this document started while this one ran; its result is
  // the one the user's buffer corresponds to.
  if (inFlight.get(key) !== token) return;
  inFlight.delete(key);

  if (error && error.code === 'ENOENT') {
    collection.delete(document.uri);
    reportMissingBinary(error);
    return;
  }
  warnedMissingBinary = false;

  const text = `${stdout || ''}${stderr || ''}`;
  const found = parseDiagnostics(text, document.getText());

  if (found.length === 0 && error) {
    // The binary refused the invocation rather than reporting a source problem
    // (a stale `oskelc -c`, a bad flag), or it was killed for running too long.
    // Silence would read as "no problems".
    const message = error.killed
      ? `oskel did not finish within ${CHECK_TIMEOUT_MS / 1000} seconds.`
      : messageForFailure(text, error.code || 1);
    const diagnostic = new vscode.Diagnostic(
      new vscode.Range(0, 0, 0, 0),
      message,
      vscode.DiagnosticSeverity.Error
    );
    diagnostic.source = 'oskel';
    collection.set(document.uri, [diagnostic]);
    return;
  }

  collection.set(document.uri, toVscodeDiagnostics(found));
}

// `check` is async and its callers are event handlers, which VS Code does not
// await. A rejection would surface only as an unhandled promise in the
// extension host log, so every entry point funnels through here.
function checkSafely(document) {
  return check(document).catch((error) => {
    vscode.window.showErrorMessage(`Oskel: the check failed (${error.message}).`);
  });
}

function checkActiveFile() {
  const editor = vscode.window.activeTextEditor;
  return checkSafely(editor && editor.document);
}

function activate(context) {
  collection = vscode.languages.createDiagnosticCollection(LANGUAGE);
  context.subscriptions.push(collection);

  context.subscriptions.push(
    vscode.workspace.onDidSaveTextDocument((document) => {
      if (!config().onSave) return;
      if (saveGuard.consume(document.uri.toString())) return;
      checkSafely(document);
    }),

    // A closed file keeps no diagnostics, or a reopened one would show a stale
    // problem from a run it never had.
    vscode.workspace.onDidCloseTextDocument((document) => {
      collection.delete(document.uri);
      inFlight.delete(document.uri.toString());
    }),

    vscode.workspace.onDidChangeConfiguration((event) => {
      if (event.affectsConfiguration(CONFIG) && !config().enabled) collection.clear();
    }),

    vscode.commands.registerCommand('oskel.checkFile', checkActiveFile),
    vscode.commands.registerCommand('oskel.clearDiagnostics', () => collection.clear())
  );
}

function deactivate() {
  for (const token of inFlight.values()) if (token.kill) token.kill();
  inFlight.clear();
  if (collection) collection.dispose();
}

module.exports = { activate, deactivate };

// `oskel`: the development engine (DEVELOPMENT.md §2).
//
// This binary owns the VM and the JIT. It runs scripts, hosts the REPL and
// offers `-c`; `oskel build` stays as a thin alias over the AOT command in
// `src/frontend/cli.cpp`, which `oskelc` drives directly. Everything the two binaries
// share (module resolution, the standard library, the AOT pipeline) lives
// there, so the split is a second `main`, not a fork.
#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "compiler.h"
#include "core/util.h"
#include "frontend/ast.h"
#include "frontend/cli_internal.h"
#include "frontend/diagnostic.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "runtime/builtins.h"
#include "version.h"
#include "vm.h"

namespace {

const char *kUsage =
    "usage:\n"
    "  oskel                      start the interactive REPL\n"
    "  oskel <file.oskl> [args]   run a script\n"
    "  oskel -e \"<code>\" [args]   run inline code\n"
    "  oskel -c <file.oskl>       parse a script without running it\n"
    "  oskel build <file.oskl>    compile to a native binary (AOT)\n"
    "\n"
    "options:\n"
    "  -h, --help                show this help\n"
    "  -v, --version             show the version\n"
    "  -e, --eval <code>         evaluate code from the command line\n"
    "  -c, --check               only check the script for errors\n"
    "  --no-jit                  disable the JIT (development engine only)\n"
    "  -o, --output <file>       output path for `build` (default: the script's base name)\n"
    "  --target <name>           build target for `build` (default: the host);\n"
    "                            x86_64-linux-sysv, x86_64-windows-win64,\n"
    "                            x86_64-freestanding-sysv (bare metal) or\n"
    "                            wasm32-freestanding-wasm (WebAssembly)\n"
    "  --mode <name>             link mode for `build` (default: freestanding);\n"
    "                            freestanding (self-contained) or hosted (links libc)\n"
    "  --link-lib <name>         library for the hosted link (repeatable), e.g. SDL3\n"
    "\n"
    "`oskelc <file.oskl> -o <out>` is the compiler without the interpreter.\n";

// Compiles `source` (resolving any imports) and, unless `checkOnly`, runs it.
// Returns 0 on success, 65 for compile errors, 70 for runtime errors.
int runSource(const std::string &source, const std::string &name,
              const std::vector<std::string> &scriptArgs, bool checkOnly,
              const std::string &argv0, bool useJit) {
  std::vector<Token> tokens;
  try {
    tokens = lex(source);
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }

  StmtPtr program;
  std::vector<Warning> warnings;
  try {
    Parser parser(std::move(tokens), name);
    program = parser.parseProgram();
    warnings = parser.warnings();
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }
  for (const Warning &warning : warnings) {
    printCompileWarning(name, source, warning, platformPosix());
  }

  // Imports are resolved during compilation, so `--check` validates them too.
  Bytecode bytecode;
  Compiler compiler;
  try {
    compiler.setImportLoader(makeImportLoader(resolverFor(name, argv0)));
    bytecode = compiler.compile(program);
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }
  for (const Warning &warning : compiler.warnings()) {
    printCompileWarning(name, source, warning, platformPosix());
  }

  if (checkOnly) return 0;

  VM vm;
  vm.sourceName = name;
  vm.scriptArgs = scriptArgs;
  vm.setJitEnabled(useJit);
  registerBuiltins(vm);
  try {
    vm.run(bytecode);
  } catch (const RuntimeError &error) {
    printRuntimeError(name, source, error, platformPosix());
    return 70;
  }
  return 0;
}

bool isStatementStart(const std::string &text) {
  // An attribute (`@MustUse`) always begins a declaration, never an expression.
  if (!text.empty() && text[0] == '@') return true;
  size_t end = 0;
  while (end < text.size() &&
         (std::isalnum(static_cast<unsigned char>(text[end])) ||
          text[end] == '_')) {
    ++end;
  }
  std::string word = text.substr(0, end);
  return word == "let" || word == "var" || word == "const" || word == "fn" ||
         word == "if" || word == "while" || word == "do" || word == "for" || word == "return" ||
         word == "break" || word == "continue" || word == "else" ||
         word == "import" || word == "switch" || word == "struct" ||
         word == "native" || word == "extern";
}

void printReplHelp() {
  std::cout << "commands:\n"
               "  :help            show this help\n"
               "  :quit            exit the REPL\n"
               "  :clear           forget all user definitions\n"
               "  :load <file>     run a script in this session\n"
               "  :vars            list global variables\n";
}

// Returns true when the REPL should exit.
bool handleCommand(VM &vm, Compiler &compiler, const std::string &command,
                   const std::string &argv0) {
  if (command == ":quit" || command == ":q" || command == ":exit")
    return true;

  if (command == ":help" || command == ":?") {
    printReplHelp();
    return false;
  }

  if (command == ":clear") {
    vm.clearGlobals();
    registerBuiltins(vm);
    // The session state (types, structs, imports) belongs to the definitions
    // just forgotten, so it is dropped with them; otherwise a later entry
    // would still see a type or a module the user no longer has.
    compiler.clearSession();
    std::cout << "all definitions cleared\n";
    return false;
  }

  if (command == ":vars") {
    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto &entry : vm.globals()) {
      const Value &value = entry.second;
      if (value.isFunc() && value.func().isNative())
        continue;
      // Internal globals (the `$module$...` tables that keep imported modules
      // alive) are not user definitions, so they stay out of the listing.
      if (!entry.first.empty() && entry.first[0] == '$')
        continue;
      entries.emplace_back(entry.first, value.typeName());
    }
    std::sort(entries.begin(), entries.end());
    if (entries.empty()) {
      std::cout << "(no user definitions)\n";
    }
    for (const auto &entry : entries) {
      std::cout << "  " << entry.first << ": " << entry.second << '\n';
    }
    return false;
  }

  if (startsWith(command, ":load")) {
    std::string path = trim(command.substr(5));
    if (path.empty()) {
      std::cout << "usage: :load <file.oskl>\n";
      return false;
    }
    std::string source;
    if (!readSourceFile(path, source)) {
      std::cout << "cannot read file '" << path << "'\n";
      return false;
    }
    // `:load` runs the file *in the current session*: it sees the session's
    // structs, types and imports, and its own definitions persist for later
    // entries. Its imports still resolve relative to the file, so the file's
    // loader is used only for this call.
    try {
      Parser parser(lex(source), path);
      parser.seedStructNames(compiler.structNames());
      parser.seedClassNames(compiler.classNames());
      Bytecode bytecode = compiler.compileLoaded(
          parser.parseProgram(), makeImportLoader(resolverFor(path, argv0)), path);
      for (const Warning &warning : parser.warnings())
        printCompileWarning(path, source, warning, platformPosix());
      for (const Warning &warning : compiler.warnings())
        printCompileWarning(path, source, warning, platformPosix());
      vm.run(bytecode);
    } catch (const CompileError &error) {
      printCompileError(path, source, error, platformPosix());
    } catch (const RuntimeError &error) {
      printRuntimeError(path, source, error, platformPosix());
    }
    return false;
  }

  std::cout << "unknown command '" << command << "'; try :help\n";
  return false;
}

// Reports the value of a REPL expression and keeps it in `_`, like the AST
// interpreter did.
void publishReplValue(VM &vm, const Value &value) {
  vm.defineGlobal("_", value, false);
  if (!value.isNil())
    std::cout << value.repr() << '\n';
}

void repl(const std::vector<std::string> &scriptArgs, const std::string &argv0, bool useJit) {
  VM vm;
  vm.sourceName = "<repl>";
  vm.scriptArgs = scriptArgs;
  vm.setJitEnabled(useJit);
  registerBuiltins(vm);

  // One compiler for the whole session so a module imported in an earlier
  // entry is compiled once and reused by later entries.
  Compiler compiler;
  compiler.setImportLoader(makeImportLoader(resolverFor("<repl>", argv0)));

  std::cout << "Oskel " << kVersion
            << " - a small scripting language written in C++\n";
  std::cout << "Type :help for help, :quit to exit.\n";

  std::string buffer;
  while (true) {
    std::cout << (buffer.empty() ? "oskel> " : "...> ") << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) {
      std::cout << '\n';
      break;
    }

    std::string trimmed = trim(line);
    if (!buffer.empty() && trimmed.empty()) {
      buffer.clear();
      continue;
    }
    if (buffer.empty() && !trimmed.empty() && trimmed[0] == ':') {
      if (handleCommand(vm, compiler, trimmed, argv0))
        break;
      continue;
    }
    if (buffer.empty() && trimmed.empty())
      continue;

    buffer += line;
    buffer += '\n';

    std::vector<Token> tokens;
    try {
      tokens = lex(buffer);
    } catch (const CompileError &error) {
      if (error.message.find("unterminated") != std::string::npos)
        continue;
      printCompileError("<repl>", buffer, error, platformPosix());
      buffer.clear();
      continue;
    }

    // A new REPL entry that begins with a statement keyword is parsed as a
    // program; everything else is first tried as a bare expression.
    size_t firstNewline = buffer.find('\n');
    std::string firstLineText = firstNewline == std::string::npos
                                    ? buffer
                                    : buffer.substr(0, firstNewline);

    if (!isStatementStart(trim(firstLineText))) {
      try {
        Parser expressionParser(tokens, "<repl>");
        expressionParser.seedStructNames(compiler.structNames());
        expressionParser.seedClassNames(compiler.classNames());
        auto expr = expressionParser.parseFullExpression();
        Bytecode bytecode = compiler.compileExpression(expr);
        Value value = vm.run(bytecode);
        publishReplValue(vm, value);
        buffer.clear();
        continue;
      } catch (const CompileError &) {
        // Fall through and try to parse it as a program.
      } catch (const RuntimeError &error) {
        printRuntimeError("<repl>", buffer, error, platformPosix());
        buffer.clear();
        continue;
      }
    }

    StmtPtr program;
    std::vector<Warning> parseWarnings;
    try {
      Parser parser(tokens, "<repl>");
      parser.seedStructNames(compiler.structNames());
      parser.seedClassNames(compiler.classNames());
      program = parser.parseProgram();
      parseWarnings = parser.warnings();
    } catch (const CompileError &error) {
      if (error.message.find("end of input") != std::string::npos) {
        // Convenience: accept a missing ';' at the end of a REPL entry.
        try {
          Parser parser(lex(buffer + ";"), "<repl>");
          parser.seedStructNames(compiler.structNames());
          parser.seedClassNames(compiler.classNames());
          program = parser.parseProgram();
          parseWarnings = parser.warnings();
        } catch (const CompileError &retry) {
          bool incomplete =
              retry.message.find("end of input") != std::string::npos ||
              retry.message.find("unterminated") != std::string::npos;
          if (incomplete)
            continue;
          printCompileError("<repl>", buffer, retry, platformPosix());
          buffer.clear();
          continue;
        }
      } else {
        printCompileError("<repl>", buffer, error, platformPosix());
        buffer.clear();
        continue;
      }
    }

    try {
      const auto &block = std::get<BlockStmt>(program->node);
      if (block.stmts.empty()) {
        buffer.clear();
        continue;
      }

      bool endsWithExpression = false;
      const ExprPtr *tailExpression = nullptr;
      if (const auto *last = std::get_if<ExprStmt>(&block.stmts.back()->node)) {
        endsWithExpression = true;
        tailExpression = &last->expr;
      }

      Bytecode bytecode = compiler.compileRepl(program);
      for (const Warning &warning : parseWarnings)
        printCompileWarning("<repl>", buffer, warning, platformPosix());
      for (const Warning &warning : compiler.warnings())
        printCompileWarning("<repl>", buffer, warning, platformPosix());
      Value value = vm.run(bytecode);
      if (endsWithExpression && tailExpression)
        publishReplValue(vm, value);
      buffer.clear();
    } catch (const CompileError &error) {
      printCompileError("<repl>", buffer, error, platformPosix());
      buffer.clear();
    } catch (const RuntimeError &error) {
      printRuntimeError("<repl>", buffer, error, platformPosix());
      buffer.clear();
    }
  }
}

} // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  std::vector<std::string> arguments(argv + 1, argv + argc);

  // `oskel build <file.oskl> [-o <out>]`: the AOT backend. It is a subcommand,
  // not a script, so it is intercepted before the ordinary argument parsing.
  // The work is `runBuildCommand`, the same function `oskelc` drives, so the
  // alias cannot drift from the compiler.
  if (!arguments.empty() && arguments[0] == "build") {
    return runBuildCommand(
        std::vector<std::string>(arguments.begin() + 1, arguments.end()),
        argv[0], "oskel build", kUsage);
  }

  std::string file;
  std::string evalCode;
  bool hasEval = false;
  bool checkOnly = false;
  bool useJit = true;
  std::vector<std::string> scriptArgs;

  for (size_t i = 0; i < arguments.size(); ++i) {
    const std::string &arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Oskel " << kVersion << "\n\n" << kUsage;
      return 0;
    }
    if (arg == "-v" || arg == "--version") {
      std::cout << "Oskel " << kVersion << '\n';
      return 0;
    }
    if (arg == "-e" || arg == "--eval") {
      if (i + 1 >= arguments.size()) {
        std::cerr << "oskel: option '" << arg << "' needs an argument\n";
        return 64;
      }
      evalCode = arguments[++i];
      hasEval = true;
      scriptArgs.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i + 1),
                        arguments.end());
      break;
    }
    if (arg == "-c" || arg == "--check") {
      checkOnly = true;
      continue;
    }
    if (arg == "--no-jit") {
      useJit = false;
      continue;
    }
    if (arg == "--") {
      if (i + 1 < arguments.size()) {
        file = arguments[i + 1];
        scriptArgs.assign(arguments.begin() +
                              static_cast<std::ptrdiff_t>(i + 2),
                          arguments.end());
      }
      break;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "oskel: unknown option '" << arg << "'\n";
      return 64;
    }
    file = arg;
    scriptArgs.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i + 1),
                      arguments.end());
    break;
  }

  if (hasEval)
    return runSource(evalCode, "<eval>", scriptArgs, checkOnly, argv[0], useJit);

  if (!file.empty()) {
    std::string source;
    if (!readSourceFile(file, source)) {
      std::cerr << "oskel: cannot read file '" << file << "'\n";
      return 66;
    }
    return runSource(source, file, scriptArgs, checkOnly, argv[0], useJit);
  }

  if (checkOnly) {
    std::cerr << "oskel: --check requires a file\n";
    return 64;
  }

  repl(scriptArgs, argv[0], useJit);
  return 0;
}

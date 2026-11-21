# Keeps the VS Code extension's grammar in step with the compiler.
#
# The grammar keeps its own keyword, type and builtin lists (it cannot read the
# compiler at run time), so it silently rots whenever the language gains a word:
# `time`, `memSize` and `args` were builtins the grammar did not highlight, and
# `0x10` was split into `0` and a name. This test derives the vocabulary from the
# single sources of truth and fails when the grammar misses one of them:
#
#   keywords - the `kKeywords` table in src/frontend/lexer.cpp
#   types    - the `name == "..."` vocabulary in src/frontend/ast.h
#   builtins - the `addNative("...")` names in src/runtime/builtins.cpp
#
# It also rejects syntax the compiler does not have (highlighting an operator
# the lexer never produces advertises a program that cannot compile) and a
# dangling `#include`.
#
# Usage: cmake -DROOT=<repository root> -P tests/check_vscode_grammar.cmake

# A `-P` script does not inherit the project's policy floor, and this one leans
# on `IN_LIST` (CMP0057) and `while(TRUE)` (CMP0012). Without a floor both are
# parsed as plain arguments, so the `if()`/`while()` fail instead of evaluating.
cmake_minimum_required(VERSION 3.15)

if(NOT DEFINED ROOT)
  get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# `IN_LIST` is the operator this check uses, and the loops use `while(TRUE)`.
# Setting the project's minimum version enables the policies that make both
# work; `CMP0130` (3.24+) is opted in explicitly when it exists.
cmake_minimum_required(VERSION 3.15)
if(POLICY CMP0130)
  cmake_policy(SET CMP0130 NEW)
endif()

set(extension "${ROOT}/editors/vscode")
set(grammar_path "${extension}/syntaxes/oskel.tmLanguage.json")

foreach(required
    "${grammar_path}"
    "${extension}/package.json"
    "${extension}/language-configuration.json"
    "${extension}/snippets/oskel.json"
    "${ROOT}/src/frontend/lexer.cpp"
    "${ROOT}/src/frontend/ast.h"
    "${ROOT}/src/runtime/builtins.cpp")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "check_vscode_grammar.cmake: missing ${required}")
  endif()
endforeach()

file(READ "${grammar_path}" grammar)

# The vocabulary lives in the `match` values. Reading only those keeps a word
# that merely appears in a `comment` from counting as highlighted.
string(REGEX MATCHALL "\"match\"[ \t]*:[ \t]*\"[^\"]*\"" match_entries "${grammar}")
string(REPLACE ";" "\n" match_text "${match_entries}")

# Every identifier the grammar actually highlights. A list lookup beats a regex
# built from the word: `size_t` and `int8` are ordinary members.
string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_]*" highlighted "${match_text}")
list(REMOVE_DUPLICATES highlighted)

set(problems "")

# A macro, not a function: it must append to `problems` in this scope.
macro(expect_highlighted word source)
  if(NOT "${word}" IN_LIST highlighted)
    list(APPEND problems "  '${word}' (${source}) is not highlighted by the grammar")
  endif()
endmacro()

# --- keywords: the lexer's own table ---------------------------------------
file(READ "${ROOT}/src/frontend/lexer.cpp" lexer_source)
string(REGEX MATCHALL "\"[a-z_]+\"[ \t]*,[ \t]*Tok::" keyword_entries "${lexer_source}")
set(keyword_count 0)
foreach(entry IN LISTS keyword_entries)
  string(REGEX REPLACE "\"([a-z_]+)\".*" "\\1" word "${entry}")
  math(EXPR keyword_count "${keyword_count} + 1")
  expect_highlighted("${word}" "keyword in src/frontend/lexer.cpp")
endforeach()
if(keyword_count LESS 20)
  list(APPEND problems "  only ${keyword_count} keywords found in src/frontend/lexer.cpp; the table moved")
endif()

# --- types: the scalar vocabulary in src/frontend/ast.h -----------------------------
file(READ "${ROOT}/src/frontend/ast.h" ast_source)
string(REGEX MATCHALL "name == \"[a-z0-9_]+\"" type_entries "${ast_source}")
set(type_count 0)
foreach(entry IN LISTS type_entries)
  string(REGEX REPLACE "name == \"([a-z0-9_]+)\"" "\\1" word "${entry}")
  math(EXPR type_count "${type_count} + 1")
  expect_highlighted("${word}" "type in src/frontend/ast.h")
endforeach()
if(type_count LESS 20)
  list(APPEND problems "  only ${type_count} type names found in src/frontend/ast.h; the vocabulary moved")
endif()

# --- builtins: every `addNative` name -------------------------------------
file(READ "${ROOT}/src/runtime/builtins.cpp" builtins_source)
string(REGEX MATCHALL "addNative\\(\"[^\"]+\"" native_entries "${builtins_source}")
set(builtin_count 0)
foreach(entry IN LISTS native_entries)
  string(REGEX REPLACE "addNative\\(\"([^\"]+)\"" "\\1" name "${entry}")
  # A `$`-prefixed name is compiler-private: the language cannot spell it, so
  # the grammar must not offer it.
  string(SUBSTRING "${name}" 0 1 first_char)
  if(first_char STREQUAL "$")
    continue()
  endif()
  math(EXPR builtin_count "${builtin_count} + 1")
  expect_highlighted("${name}" "builtin in src/runtime/builtins.cpp")
endforeach()
if(builtin_count LESS 40)
  list(APPEND problems "  only ${builtin_count} builtins found in src/runtime/builtins.cpp; the registry moved")
endif()

# --- operators: derived from the lexer's `switch (c)` ----------------------
# A new operator is added to the lexer's `switch (c)` and, from that moment, the
# grammar has to know it: `x &= 3` compiled for a release while the grammar did
# not highlight it, and the grammar had advertised `===` and `->` that the lexer
# cannot produce. Both directions are checked here.
#
# The spelling of each operator is rebuilt from the `if (c1 == 'a' ...)` guard
# that returns it, so the check reads the lexer rather than a copy of its list.
# `string(FIND ... <start>)` needs CMake 3.20 and the project supports 3.15, so
# the switch is walked by chopping the head off the string as it goes.
file(READ "${ROOT}/src/frontend/lexer.cpp" lexer_ops)

# `isAssignOp` in src/frontend/parser.cpp is the exact list of assignment tokens, so a
# suffix test is not needed: `EqEq` and `LtEq` are comparisons, not assignments.
file(READ "${ROOT}/src/frontend/parser.cpp" parser_source)
string(REGEX MATCH "bool isAssignOp\\(Tok type\\) \\{[^}]*\\}" assign_op_body "${parser_source}")
string(REGEX MATCHALL "case Tok::[A-Za-z]+" assign_cases "${assign_op_body}")
set(assign_tokens "")
foreach(assign_case IN LISTS assign_cases)
  string(REGEX REPLACE "case Tok::([A-Za-z]+)" "\\1" token "${assign_case}")
  list(APPEND assign_tokens "${token}")
endforeach()
list(LENGTH assign_tokens assign_token_count)
if(assign_token_count LESS 10)
  list(APPEND problems "  only ${assign_token_count} assignment tokens found in isAssignOp (src/frontend/parser.cpp); it moved")
endif()

set(producible "")
set(assignments "")
set(rest "${lexer_ops}")
while(TRUE)
  string(FIND "${rest}" "case '" case_at)
  if(case_at EQUAL -1)
    break()
  endif()
  math(EXPR char_at "${case_at} + 6")
  string(SUBSTRING "${rest}" ${char_at} 1 op_char)
  math(EXPR body_at "${case_at} + 7")
  string(SUBSTRING "${rest}" ${body_at} -1 body)
  string(FIND "${body}" "case '" next_case)
  string(FIND "${body}" "default:" next_default)
  if(next_case EQUAL -1)
    set(next_case 100000000)
  endif()
  if(next_default EQUAL -1)
    set(next_default 100000000)
  endif()
  set(block_to "${next_case}")
  if(next_default LESS block_to)
    set(block_to "${next_default}")
  endif()
  string(SUBSTRING "${body}" 0 ${block_to} block)

  # Every `if (...) return make(Tok::Name, n)` and the bare `return make(...)`.
  string(REGEX MATCHALL "if \\([^)]*\\) return make\\(Tok::[A-Za-z]+, [0-9]\\)" guards "${block}")
  string(REGEX MATCHALL "return make\\(Tok::[A-Za-z]+, 1\\)" bare_returns "${block}")
  foreach(guard IN LISTS bare_returns)
    string(REGEX REPLACE ".*Tok::([A-Za-z]+).*" "\\1" token "${guard}")
    list(APPEND producible "${op_char}")
    if(token IN_LIST assign_tokens AND NOT token STREQUAL "Eq")
      list(APPEND assignments "${op_char}")
    endif()
  endforeach()
  foreach(guard IN LISTS guards)
    string(REGEX REPLACE ".*Tok::([A-Za-z]+).*" "\\1" token "${guard}")
    string(REGEX MATCH "c1 == '.' && c2 == '.'" triple "${guard}")
    if(triple)
      string(REGEX REPLACE "c1 == '(.)' && c2 == '(.)'" "\\1\\2" tail "${triple}")
    else()
      string(REGEX MATCH "c1 == '.'" double "${guard}")
      if(NOT double)
        continue()
      endif()
      string(REGEX REPLACE "c1 == '(.)'" "\\1" tail "${double}")
    endif()
    set(spelling "${op_char}${tail}")
    list(APPEND producible "${spelling}")
    if(token IN_LIST assign_tokens AND NOT token STREQUAL "Eq")
      list(APPEND assignments "${spelling}")
    endif()
  endforeach()

  string(SUBSTRING "${rest}" ${body_at} -1 rest)
endwhile()
list(REMOVE_DUPLICATES producible)
list(REMOVE_DUPLICATES assignments)
list(LENGTH producible producible_count)
if(producible_count LESS 20)
  list(APPEND problems "  only ${producible_count} operator spellings found in src/frontend/lexer.cpp; the switch moved")
endif()

# The operators the grammar highlights, read from the `match` value of every
# `keyword.operator` rule. Parsing a regex is the wrong tool, so the value is
# reduced to the literal spellings it can match, in this order:
#
#   1. cut at the first `(?` — a lookahead narrows a spelling, it never adds
#      one, and its body holds `|` and `)` that would confuse the split;
#   2. lift every character class out of the way, since `[&|^~]` holds a `|`;
#   3. lift `\|`, which is a literal pipe and not alternation;
#   4. split what is left on `|`;
#   5. per alternative, put the lifted pieces back: a class expands to one
#      operator per member, and everything else is unescaped.
#
# The order matters and each step exists because the naive version got it wrong.
# The file is read one line at a time and a `match` is paired with the `"name"`
# line above it; if that formatting ever changes this finds no rule and fails.
set(grammar_operators "")
set(in_operator_rule "")
set(rest "${grammar}")
while(TRUE)
  string(FIND "${rest}" "\n" newline_at)
  if(newline_at EQUAL -1)
    set(line "${rest}")
    set(rest "")
  else()
    string(SUBSTRING "${rest}" 0 ${newline_at} line)
    math(EXPR after "${newline_at} + 1")
    string(SUBSTRING "${rest}" ${after} -1 rest)
  endif()

  if(line MATCHES "\"name\"[ \t]*:[ \t]*\"keyword\\.operator")
    set(in_operator_rule "yes")
  elseif(in_operator_rule STREQUAL "yes" AND line MATCHES "\"match\"[ \t]*:[ \t]*\"(.*)\"")
    set(in_operator_rule "")
    # Step 1: JSON-unescape the value, then drop any lookahead.
    string(REPLACE "\\\\" "\\" pattern "${CMAKE_MATCH_1}")
    string(FIND "${pattern}" "(?" lookahead_at)
    if(NOT lookahead_at EQUAL -1)
      string(SUBSTRING "${pattern}" 0 ${lookahead_at} pattern)
    endif()

    # Step 2: replace each class with `@CLASS<n>@` and remember its members.
    set(class_members "")
    set(class_index 0)
    while(pattern MATCHES "\\[[^]]*\\]")
      set(whole "${CMAKE_MATCH_0}")
      string(REGEX REPLACE "^\\[(.*)\\]$" "\\1" members "${whole}")
      list(APPEND class_members "${members}")
      string(REPLACE "${whole}" "@CLASS${class_index}@" pattern "${pattern}")
      math(EXPR class_index "${class_index} + 1")
    endwhile()

    # Step 3: a literal pipe, so the split below does not see it.
    string(REPLACE "\\|" "@PIPE@" pattern "${pattern}")

    # Step 4: split on alternation.
    string(REPLACE "|" ";" alternatives "${pattern}")
    foreach(alternative IN LISTS alternatives)
      # Step 5a: a class expands to one operator per member. A `-` at either
      # edge is the character itself, not a range.
      if(alternative MATCHES "^@CLASS([0-9]+)@$")
        set(which "${CMAKE_MATCH_1}")
        list(GET class_members ${which} members)
        string(LENGTH "${members}" members_len)
        set(i 0)
        while(i LESS members_len)
          string(SUBSTRING "${members}" ${i} 1 one)
          if(one MATCHES "^[-+*/%<>=!&|^~?]$")
            list(APPEND grammar_operators "${one}")
          endif()
          math(EXPR i "${i} + 1")
        endwhile()
        continue()
      endif()

      # Step 5b: restore the literal pipe, then unescape character by
      # character. It cannot be a `string(REPLACE "\\" "")`: that turns the
      # escaped `\*` of `\*\*=` into a bare `*`, which then looks like the
      # quantifier and the operator is thrown away. An unescaped metacharacter
      # means this alternative is not a plain spelling, so it is skipped.
      string(REPLACE "@PIPE@" "|" alternative "${alternative}")
      string(LENGTH "${alternative}" alt_len)
      set(i 0)
      set(spelling "")
      set(plain "yes")
      while(i LESS alt_len)
        string(SUBSTRING "${alternative}" ${i} 1 c)
        if(c STREQUAL "\\")
          math(EXPR next "${i} + 1")
          if(next LESS alt_len)
            string(SUBSTRING "${alternative}" ${next} 1 escaped)
            string(APPEND spelling "${escaped}")
          endif()
          math(EXPR i "${i} + 2")
          continue()
        endif()
        if(c MATCHES "^[][(){}*+?$^.]$")
          set(plain "no")
        endif()
        string(APPEND spelling "${c}")
        math(EXPR i "${i} + 1")
      endwhile()
      if(NOT plain STREQUAL "yes" OR spelling STREQUAL "")
        continue()
      endif()
      list(APPEND grammar_operators "${spelling}")
    endforeach()
  endif()

  if(rest STREQUAL "")
    break()
  endif()
endwhile()
list(REMOVE_DUPLICATES grammar_operators)
list(LENGTH grammar_operators grammar_operator_count)
if(grammar_operator_count LESS 15)
  list(APPEND problems
    "  only ${grammar_operator_count} operator spellings found in the grammar; "
    "its formatting moved and the operator check cannot read it")
endif()

# Direction 1: the grammar must not highlight an operator the lexer lacks.
foreach(operator IN LISTS grammar_operators)
  if(NOT operator IN_LIST producible)
    list(APPEND problems "  the grammar highlights '${operator}' as an operator, but src/frontend/lexer.cpp never produces it")
  endif()
endforeach()

# Direction 2: every assignment operator the lexer produces must be
# highlighted, so a new `x op= y` cannot reach the compiler unhighlighted.
foreach(operator IN LISTS assignments)
  if(NOT operator IN_LIST grammar_operators)
    list(APPEND problems "  the lexer produces the assignment operator '${operator}', but the grammar does not highlight it")
  endif()
endforeach()

# --- every `#include` resolves --------------------------------------------
string(REGEX MATCHALL "\"#[A-Za-z0-9_-]+\"" include_refs "${grammar}")
list(REMOVE_DUPLICATES include_refs)
foreach(ref IN LISTS include_refs)
  string(REGEX REPLACE "[#\"]" "" ref "${ref}")
  if(NOT grammar MATCHES "\"${ref}\"[ \t\r\n]*:")
    list(APPEND problems "  the grammar includes '#${ref}' but declares no '${ref}' entry")
  endif()
endforeach()

if(problems)
  string(REPLACE ";" "\n" problems "${problems}")
  message(FATAL_ERROR
    "the VS Code grammar is out of step with the compiler:\n${problems}\n"
    "Update editors/vscode/syntaxes/oskel.tmLanguage.json.")
endif()

message(STATUS
  "VS Code grammar matches the compiler: ${keyword_count} keywords, "
  "${type_count} type names, ${builtin_count} builtins")

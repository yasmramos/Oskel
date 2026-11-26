# SQLite

`std.sqlite` is a SQLite3 wrapper written as two layers:

- `std/internal/sqlite_bindings.oskl` is the generated C binding. `bindgen`
  produces it from `bindings/sqlite/sqlite3_subset.h`; the header is a curated
  subset of `sqlite3.h`, not a copy, and it is never preprocessed.
- `std/sqlite.oskl` is the hand-written `Database` and `Statement` classes
  over the portable primitive surface.

```oskl
import std.sqlite;

let db = sqlite.Database(":memory:");
db.exec("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL)");
db.run("INSERT INTO people (name, score) VALUES (?, ?)", "Ada", 9.5);
db.run("INSERT INTO people (name, score) VALUES (?, ?)", "Grace", 9.75);

for row in db.query("SELECT id, name, score FROM people ORDER BY id") {
  print(row.id, row.name, row.score);
}
db.close();
```

## Build modes

SQLite is a host library, so the module is not part of the portable surface.

| Engine | How it works |
| --- | --- |
| VM (`oskel`) | The engine links SQLite3 when CMake finds it (`find_package(SQLite3)`, `OSKEL_ENABLE_SQLITE=ON` by default) and resolves `sqlite3_*` through `Platform::lookupSymbol`. |
| `oskel build --mode hosted` | Link the library explicitly: `oskel build app.oskl --mode hosted --link-lib sqlite3`. The hosted image links the host C library. |
| `oskel build` (freestanding) | Rejected with exit 65 and the dedicated message `extern "C" symbol '...' cannot be resolved by the AOT backend`. It never emits a mis-compiled image. |
| `--target wasm32-freestanding-wasm` | No binding. |

On Linux the engine links SQLite3 with `-Wl,--no-as-needed`: the symbols are
reached with `dlsym(RTLD_DEFAULT)`, so a library nothing references at link
time must not be dropped by `--as-needed`. On Windows the imported DLL is
copied beside the engine; the hosted AOT test is not registered there because
the Windows runner has no SQLite3 package.

If CMake does not find SQLite3, the module and its bindgen check still build
and run; only the runtime fixtures (`80_std_sqlite`,
`oskel.build_hosted_sqlite`) are skipped. The freestanding rejection
(`oskel.build_sqlite_freestanding`) still runs because it only needs the
compiler.

## API

### `Database`

| Member | Description |
| --- | --- |
| `Database(path)` | Opens read-write and creates the file when it does not exist. |
| `Database({path: path, flags: flags})` | Passes `sqlite3_open_v2` flags; `SQLITE_OPEN_READONLY` opens an existing file read-only. |
| `open(path)` / `openReadOnly(path)` | Convenience constructors. |
| `db.exec(sql)` | Runs one or more statements and returns the changed-row count. |
| `db.run(sql, params...)` | Runs a statement that produces no rows; returns `{changes, lastInsertRowId}`. |
| `db.query(sql, params...)` | Returns every row as a map from column name to value. |
| `db.prepare(sql)` | Returns a `Statement`; the caller owns it. |
| `db.transaction(body)` | `BEGIN`; runs `body`; `COMMIT` on return, `ROLLBACK` and re-raise on a panic. |
| `db.error()` | The last error message (`sqlite3_errmsg`). |
| `db.errcode()` / `db.extendedErrcode()` | The result code. |
| `db.changes()` / `db.totalChanges()` | Rows changed by the last statement / since the connection opened. |
| `db.lastInsertRowId()` | The rowid of the most recent successful INSERT. |
| `db.version()` / `db.versionNumber()` | The SQLite library version. |
| `db.busyTimeout(ms)` | How long SQLite waits on a locked database. |
| `db.autocommit()` | True when no transaction is active. |
| `db.extendedResultCodes(on)` | Turns extended result codes on or off. |
| `db.close()` | Closes the connection. Every statement must be finalized first, or SQLite reports BUSY and the handle stays open. |

### `Statement`

`db.prepare` is the step-by-step API; `query`/`run` are the convenience forms
and finalize the statement for you.

| Member | Description |
| --- | --- |
| `stmt.bind([...])` | Binds positionally: `?1`, `?2`, ... |
| `stmt.step()` | `SQLITE_ROW`, `SQLITE_DONE` or an error code. |
| `stmt.row()` | The current row as a map from column name to value. |
| `stmt.value(i)` | Column `i` of the current row. |
| `stmt.columns()` / `stmt.columnCount()` | The column names / count. |
| `stmt.parameterCount()` | The number of parameters. |
| `stmt.reset()` / `stmt.clearBindings()` | Reuse the statement. |
| `stmt.finalize()` | Releases the statement; idempotent. |

```oskl
fn namesAbove(db, score) {
  let stmt = db.prepare("SELECT name FROM people WHERE score > ? ORDER BY name");
  defer { stmt.finalize(); }
  stmt.bind([score]);
  var names = [];
  while (true) {
    let code = stmt.step();
    if (code == sqlite.SQLITE_ROW) {
      names[len(names)] = stmt.value(0);
    } else if (code == sqlite.SQLITE_DONE) {
      break;
    } else {
      panic("sqlite: step: " + db.error());
    }
  }
  return names;
}
```

`defer` is only allowed inside a function; at the top level, call
`stmt.finalize()` at the end.

## Values

A parameter is positional (`?`). The wrapper binds:

| Oskel value | SQLite type |
| --- | --- |
| `int`, `bool` | INTEGER |
| `float`, `double` | REAL |
| `string`, `cstring` | TEXT |
| `nil` | NULL |
| array of bytes (`0..255`) | BLOB |

A column comes back as an `int`, a `double`, a `string`, an array of bytes for
a BLOB, or `nil`. TEXT and BLOB are copied out with `sqlite3_column_bytes` +
`memcpy` into an owned buffer, so an embedded NUL and a binary payload survive
the round trip; `cstr(buffer)` reads every byte.

`query` keys the row map by the column name. A duplicate name keeps the last
column, so alias the columns when a join repeats one. `run` expects a statement
that produces no rows; an `INSERT ... RETURNING` needs `query`.

## Binding details

`bindings/sqlite/sqlite3_subset.h` encodes the ABI facts `bindgen` needs:

- `sqlite3` and `sqlite3_stmt` are opaque, so every handle is a `ptr`.
- An out-parameter (`sqlite3 **`, `sqlite3_stmt **`, `char **`) is a pointer
  to a pointer, so it crosses as `ptr`. The wrapper passes an 8-byte `buffer`
  and reads the pointer back with `loadByte`.
- `sqlite3_column_text` is declared `const char *` (the real API returns
  `const unsigned char *`), so it crosses as `cstring`. The wrapper reads text
  through `sqlite3_column_blob` + `sqlite3_column_bytes` instead, which keeps
  the exact bytes.
- `sqlite3_destructor_type` and the `sqlite3_exec` callback are declared
  `void *`; both cross as `ptr`. The wrapper passes NULL as the callback and
  reads `sqlite3_errmsg` on failure, and passes `intToPtr(-1)` as
  `SQLITE_TRANSIENT` (a function-pointer cast `bindgen` cannot express).
- `memcpy` is declared in the wrapper, not in the generated SQLite module. It
  is a host symbol too, so the freestanding rejection names `memcpy` or a
  `sqlite3_*` symbol.

Regenerate the module after editing the header:

```sh
./build/bindgen bindings/sqlite/sqlite3_subset.h > std/internal/sqlite_bindings.oskl
```

`sqlite.bindgen` (`tests/check_sqlite_bindings.cmake`) compares the whole
generated file, so a header change that was not regenerated fails the suite.

## Tests

| CTest name | What it checks |
| --- | --- |
| `sqlite.bindgen` | `std/internal/sqlite_bindings.oskl` matches a fresh `bindgen` run. |
| `oskel.80_std_sqlite` | The VM wrapper: tables, parameters, TEXT/NULL/BLOB, transactions, `prepare`. |
| `oskel.build_hosted_sqlite` | The same fixture through `oskel build --mode hosted --link-lib sqlite3`, compared with the VM byte for byte (x86-64 Linux/macOS). |
| `oskel.build_sqlite_freestanding` | The freestanding rejection: exit 65, a dedicated message, no binary (Linux/Windows). |

The fixture uses `:memory:`, so no file is created and nothing needs cleanup.

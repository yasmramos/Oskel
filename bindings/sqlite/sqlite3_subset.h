/*
 * The SQLite3 surface `bindgen` translates into
 * `std/internal/sqlite_bindings.oskl`.
 *
 * This header is not a copy of SQLite's public header: it is the curated
 * subset the Oskel module exposes, written with the exact C types of the real
 * API so `bindgen` emits the right Oskel spelling. It is not preprocessed; the
 * typedefs below are the ones SQLite declares in `sqlite3.h`.
 *
 * Regenerate the module with:
 *
 *   ./build/bindgen bindings/sqlite/sqlite3_subset.h \
 *     > std/internal/sqlite_bindings.oskl
 *
 * SQLite specifics this subset encodes:
 *   - `sqlite3` and `sqlite3_stmt` are opaque; every handle crosses as `ptr`.
 *   - An out-parameter (`sqlite3 **`, `sqlite3_stmt **`, `char **`) is a
 *     pointer to a pointer, so it crosses as `ptr`; the wrapper passes an
 *     8-byte `buffer` and reads the pointer back with `loadByte`.
 *   - `sqlite3_column_text` is declared `const char *` (the real API returns
 *     `const unsigned char *`), so it crosses as `cstring`. The wrapper reads
 *     text through `sqlite3_column_blob` and `sqlite3_column_bytes` to keep
 *     embedded NULs and blobs exact.
 *   - `sqlite3_destructor_type` is a function pointer at the ABI; it is
 *     declared `void *` here so it crosses as `ptr`, which is what lets the
 *     wrapper pass `SQLITE_TRANSIENT` (-1).
 *   - `sqlite3_exec`'s callback is declared `void *` for the same reason; the
 *     wrapper always passes NULL and reads `sqlite3_errmsg` on failure.
 *   - `SQLITE_TRANSIENT` and `SQLITE_STATIC` are casts, not literals, so
 *     `bindgen` skips them; the wrapper defines its own `TRANSIENT`.
 */

typedef long long sqlite3_int64;
typedef unsigned long long sqlite3_uint64;
typedef void* sqlite3_destructor_type;
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

/* Result codes. */
#define SQLITE_OK 0
#define SQLITE_ERROR 1
#define SQLITE_INTERNAL 2
#define SQLITE_PERM 3
#define SQLITE_ABORT 4
#define SQLITE_BUSY 5
#define SQLITE_LOCKED 6
#define SQLITE_NOMEM 7
#define SQLITE_READONLY 8
#define SQLITE_INTERRUPT 9
#define SQLITE_IOERR 10
#define SQLITE_CORRUPT 11
#define SQLITE_NOTFOUND 12
#define SQLITE_FULL 13
#define SQLITE_CANTOPEN 14
#define SQLITE_PROTOCOL 15
#define SQLITE_EMPTY 16
#define SQLITE_SCHEMA 17
#define SQLITE_TOOBIG 18
#define SQLITE_CONSTRAINT 19
#define SQLITE_MISMATCH 20
#define SQLITE_MISUSE 21
#define SQLITE_NOLFS 22
#define SQLITE_AUTH 23
#define SQLITE_FORMAT 24
#define SQLITE_RANGE 25
#define SQLITE_NOTADB 26
#define SQLITE_NOTICE 27
#define SQLITE_WARNING 28
#define SQLITE_ROW 100
#define SQLITE_DONE 101

/* `sqlite3_open_v2` flags. */
#define SQLITE_OPEN_READONLY 0x00000001
#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE 0x00000004
#define SQLITE_OPEN_URI 0x00000040
#define SQLITE_OPEN_MEMORY 0x00000080
#define SQLITE_OPEN_NOMUTEX 0x00008000
#define SQLITE_OPEN_FULLMUTEX 0x00010000
#define SQLITE_OPEN_SHAREDCACHE 0x00020000
#define SQLITE_OPEN_PRIVATECACHE 0x00040000

/* Column types. */
#define SQLITE_INTEGER 1
#define SQLITE_FLOAT 2
#define SQLITE_TEXT 3
#define SQLITE_BLOB 4
#define SQLITE_NULL 5

#define SQLITE_UTF8 1
#define SQLITE_DETERMINISTIC 0x000000800

/* Casts, not literals: `bindgen` skips them and the wrapper defines TRANSIENT. */
#define SQLITE_STATIC ((sqlite3_destructor_type)0)
#define SQLITE_TRANSIENT ((sqlite3_destructor_type) - 1)

int sqlite3_open(const char* filename, sqlite3** ppDb);
int sqlite3_open_v2(const char* filename, sqlite3** ppDb, int flags, const char* zVfs);
int sqlite3_close(sqlite3* db);
int sqlite3_close_v2(sqlite3* db);

int sqlite3_exec(sqlite3* db, const char* sql, void* callback, void* arg, char** errmsg);
int sqlite3_prepare_v2(sqlite3* db, const char* zSql, int nByte, sqlite3_stmt** ppStmt,
                       const char** pzTail);

int sqlite3_step(sqlite3_stmt* stmt);
int sqlite3_finalize(sqlite3_stmt* stmt);
int sqlite3_reset(sqlite3_stmt* stmt);
int sqlite3_clear_bindings(sqlite3_stmt* stmt);

int sqlite3_bind_null(sqlite3_stmt* stmt, int index);
int sqlite3_bind_int64(sqlite3_stmt* stmt, int index, sqlite3_int64 value);
int sqlite3_bind_double(sqlite3_stmt* stmt, int index, double value);
int sqlite3_bind_text(sqlite3_stmt* stmt, int index, const char* value, int n,
                      sqlite3_destructor_type destructor);
int sqlite3_bind_blob(sqlite3_stmt* stmt, int index, const void* value, int n,
                      sqlite3_destructor_type destructor);
int sqlite3_bind_parameter_count(sqlite3_stmt* stmt);
int sqlite3_bind_parameter_index(sqlite3_stmt* stmt, const char* zName);

int sqlite3_column_count(sqlite3_stmt* stmt);
const char* sqlite3_column_name(sqlite3_stmt* stmt, int column);
int sqlite3_column_type(sqlite3_stmt* stmt, int column);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt* stmt, int column);
double sqlite3_column_double(sqlite3_stmt* stmt, int column);
const char* sqlite3_column_text(sqlite3_stmt* stmt, int column);
const void* sqlite3_column_blob(sqlite3_stmt* stmt, int column);
int sqlite3_column_bytes(sqlite3_stmt* stmt, int column);

int sqlite3_changes(sqlite3* db);
int sqlite3_total_changes(sqlite3* db);
sqlite3_int64 sqlite3_last_insert_rowid(sqlite3* db);
const char* sqlite3_errmsg(sqlite3* db);
const char* sqlite3_errstr(int code);
int sqlite3_errcode(sqlite3* db);
int sqlite3_extended_errcode(sqlite3* db);
int sqlite3_extended_result_codes(sqlite3* db, int onoff);
const char* sqlite3_libversion(void);
int sqlite3_libversion_number(void);
int sqlite3_busy_timeout(sqlite3* db, int ms);
int sqlite3_get_autocommit(sqlite3* db);
void sqlite3_free(void* p);

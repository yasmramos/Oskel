# Ledger

A double-entry accounting example, scaffolded with `oskelp new`. It is a
complete project rather than a single file: a chart of accounts, balanced
journal entries, the three standard reports, JSON persistence, CSV import and
export, and a small command line.

Amounts are `std.decimal.BigDecimal` values scaled to two places, so a trial
balance compares equal exactly; the ledger never stores money in a `double`.
The accounting core is plain Oskel over the portable primitive surface. The
command line adds `std.fs` for the data file.

## Quick start

```sh
oskelp run --manifest-path examples/ledger/oskel.toml
```

With no command the program prints a sample consulting business: a trial
balance, an income statement and a balance sheet. The figures are fixed, so
the output is reproducible:

```text
Trial balance
...
        Total                                40,700.00       40,700.00

Balanced: yes

Income statement
...
Net income                                      5,650.00

Balance sheet
...
Total liabilities and equity                   33,850.00

Balanced: yes
```

Run the project's tests and build a native binary from the same sources:

```sh
oskelp test --manifest-path examples/ledger/oskel.toml
oskelp build --manifest-path examples/ledger/oskel.toml
./examples/ledger/target/ledger
```

Inside the project directory the `--manifest-path` argument is unnecessary.

On Windows the freestanding image has no process argument vector, so
`oskelp build` reports that `args()` needs one. The VM (`oskelp run`) is the
Windows path for this project; `oskelp build --mode hosted` with a C toolchain
also works.

## Commands

| Command | Meaning |
| --- | --- |
| `demo` | Print the sample ledger (the default command). |
| `init` | Create `data/ledger.json` with the default chart of accounts. |
| `account add <code> <name> <type>` | Add an account; `type` is `asset`, `liability`, `equity`, `income` or `expense`. |
| `post <date> <memo> <code>:<debit>:<credit> ...` | Post a balanced entry. |
| `list` | Print the journal. |
| `balance [code]` | Print every balance, or one account with its entries. |
| `trial` | Print the trial balance. |
| `income` | Print the income statement. |
| `sheet` | Print the balance sheet. |
| `import <file.csv>` | Import journal lines from CSV. |
| `export <file.csv>` | Export the journal to CSV. |
| `version` | Print the version. |
| `help` | Print the usage. |

The global options are `--data <path>` (default `data/ledger.json`), `--color`
for ANSI headings and `--verbose`/`-v` for a debug line on stderr. `--help`
prints the generated `std.cli` usage.

A post line writes the account code, then the debit and the credit. Leave the
unused side empty:

```sh
ledger init
ledger post 2026-02-01 "Consulting invoice" 1100:4000: 4000::4000
ledger post 2026-02-05 "Client payment" 1000:4000: 1100::4000
ledger trial
```

Every line must have exactly one positive amount, the account must exist, and
the debits must equal the credits; the command reports the problem and exits
non-zero instead of posting a broken entry.

## Layout

```text
ledger/
  oskel.toml                 the manifest (package `ledger`, entry main.oskl)
  oskel.lock                 the empty dependency lock
  src/ledger/
    main.oskl                `exit(cli.run(args()))`
    cli.oskl                 std.cli spec, dispatch and exit codes
    money.oskl               exact cents: parse, format, sum, compare
    account.oskl             AccountType enum, chart and validation
    journal.oskl             lines, entries, balance check, JSON shape
    core.oskl                the ledger: post, balances, trial, income, sheet
    reports.oskl             ReportBuilder and the text reports
    storage.oskl             JSON persistence and CSV import/export
    demo.oskl                the fixed sample business
  tests/                     `oskelp test` runs every file
  samples/journal.csv        a CSV the import command accepts
  data/                      the default data directory (JSON is ignored)
```

## Persistence

`data/ledger.json` is the durable form. It stores the chart and every entry
with the amounts as decimal strings:

```json
{
  "version": 1,
  "accounts": [
    {"id": "…", "code": "1000", "name": "Cash", "type": "asset", "normal": "debit"}
  ],
  "entries": [
    {"id": "…", "date": "2026-02-01", "memo": "Consulting invoice",
     "lines": [{"account": "1100", "debit": "4000.00", "credit": "0.00"}]}
  ]
}
```

`init` creates the file and the `data/` directory; `--data` points at another
path. The entry id is a SHA-256 prefix of the date, memo and lines, so a saved
entry keeps a stable identifier. The account id is a name-based UUID derived
from the code.

## CSV

`export` writes one row per journal line, grouped by the `entry` column:

```csv
entry,date,memo,account,debit,credit
e00f4bb79104,2026-02-01,Consulting invoice,1100,4000.00,0.00
e00f4bb79104,2026-02-01,Consulting invoice,4000,0.00,4000.00
```

`import` groups the rows by `entry` and posts one balanced entry per group.
`samples/journal.csv` is a complete example:

```sh
ledger init
ledger import samples/journal.csv
ledger trial
```

The importer uses the default chart; an account outside it is rejected when
the entry is posted.

## What it exercises

- **Standard library:** `std.decimal`, `std.csv`, `std.json`, `std.fs`,
  `std.fs.dir`, `std.path`, `std.cli`, `std.log`, `std.semver`,
  `std.terminal`, `std.uuid`, `std.hash`, `std.datetime`, `std.collections`,
  `std.strings` and `std.io`.
- **Language:** classes and methods (`ReportBuilder`), an enum
  (`AccountType`), maps and arrays, closures, `switch`, `defer`, variadic
  parameters, `@MustUse`, string methods (`padStart`, `padEnd`, `split`,
  `contains`, `startsWith`), slicing and `pcall` in the tests.
- **Engines:** `oskelp run` uses the VM and `oskelp build` the native backend.
  The demo and the file-backed trial balance are compared byte for byte by the
  `oskelp.example_ledger` CTest. The accounting core is portable to
  WebAssembly; the CLI's file commands are the part that needs a file system.

## The accounting model

An account has a `type` and a `normal` side. Assets and expenses increase on
the debit side; liabilities, equity and income increase on the credit side. A
journal entry is balanced when its debit total equals its credit total. The
trial balance lists every non-zero account on its normal side. The income
statement is revenue minus expenses. The balance sheet presents the current
period's net income as retained earnings, so assets equal liabilities plus
equity without a closing entry.

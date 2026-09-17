# Add a type to CoreSQL and SQL

The runnable [custom type example](../../examples/custom_type/main.cpp) implements
`CODE`, a value containing exactly two uppercase ASCII letters. It uses only
public headers. Adding the type requires no changes to the parser, lowerer,
storage engine or transaction manager.

The files separate the two interfaces:

| File | Responsibility |
| --- | --- |
| [type.hpp](../../examples/custom_type/type.hpp) / [type.cpp](../../examples/custom_type/type.cpp) | Backend identity, canonical bytes, validation, comparison, construction and a scalar function |
| [sql.hpp](../../examples/custom_type/sql.hpp) / [sql.cpp](../../examples/custom_type/sql.cpp) | SQL name, declarations, casts, typed literals and function mapping |
| [main.cpp](../../examples/custom_type/main.cpp) | Registration and working C++ and SQL calls |

## Backend registration

`type.cpp` registers an `EncodedTypeAddon` with identity `example.code`, version 1.
Its payload is exactly two ASCII bytes, so native object layout is never persisted.
Validation rejects parameters, wrong lengths and non-uppercase letters. Equality
and ordering use these canonical bytes. The backend also registers
`code.first_letter`, a function accepting a CODE and returning TEXT.

```cpp
Registry registry;
example::codes::install(registry);
Database db(registry);
auto tx = db.begin();
tx.create_table("codes", {{"code", example::codes::type()}});
tx.insert("codes", {example::codes::value("AB")});
tx.commit();
```

Backend-only applications compile `type.cpp` and link `CoreSQL::core`. They need
neither the SQL adapter nor the SQL library. CODE has ordering but no hash callback;
it does not demonstrate a hash-indexed primary key. NULL propagation and atomic
mutations remain engine responsibilities.

## SQL registration

The adapter declares `CODE`, installs its backend, accepts TEXT-to-CODE and
CODE-to-TEXT conversions, and maps `code_first_letter(code)` to the registered
backend function. The operation callback checks the name and operand type before
claiming a rule. An empty type ID denotes an untyped NULL literal; `null_type`
supplies its context for this particular function. Typed NULLs retain their type.
No common-type or optimization callbacks are needed for this example.

```cpp
auto types = sql::default_type_adapters();
types.add(example::codes::sql_adapter());
Registry registry;
sql::install(registry, types); // Also installs the CODE backend, once.
Database db(registry);
sql::Connection connection(db, registry, types);
sql::Statement insert("INSERT INTO labels VALUES(?, CAST(? AS CODE))", types);
```

Use that same `types` snapshot when preparing statements and opening connections.
Do not call `example::codes::install` again on this SQL registry. On reopening a
persistent database, install the same type identity/version before `Database::open`.
Callback objects themselves are never serialized.

```sql
CREATE TABLE labels(id INTEGER PRIMARY KEY, code CODE);
INSERT INTO labels VALUES(1, CODE 'AB'), (2, CAST('CD' AS CODE));
SELECT CAST(code AS TEXT), code_first_letter(code) FROM labels ORDER BY id;
SELECT CAST(NULL AS CODE);
```

`'ab'`, `'ABC'` and `CODE(2)` are rejected. This example intentionally supports
TEXT conversion, not arbitrary dynamic input; cast an undeclared value to TEXT
first if needed. Parameters containing text work directly. Different custom
result types require explicit casts unless an adapter supplies a common-type rule.

## Build and run

```sh
cmake -S . -B build/dev -DCMAKE_BUILD_TYPE=Release
cmake --build build/dev --target coresql_custom_type -j 4
./build/dev/coresql_custom_type
./build/dev/coresql_custom_type /tmp/custom-types.core
```

The example checks both backend and SQL results and prints `AB A` and `CD C`.
Run it again with the same file to exercise reopening. For an installed package,
compile all three `.cpp` files and link `CoreSQL::sql`; that target brings in its
backend dependencies. The package tests build this example against a relocated
installation and separately build its backend against `CoreSQL::core` alone.

See the [extension contract](../contracts/extensions.md#sql-type-adapters) for
callback lifetime, dispatch conflicts and the SQL-wide rules that stay shared.

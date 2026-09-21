# Legacy nullable-column migration fixtures

`before-nullable.corelog` was written by CoreSQL commit `5aa5be1351` using the
SQL CLI: CREATE TABLE legacy(id INTEGER PRIMARY KEY, label TEXT); INSERT INTO
legacy VALUES(7,'old'). It contains CORECHG5 records in a CORELOG2 log.

`before-nullable.snapshot` is a minimal handcrafted CORESQL4 snapshot of the same
schema and row (row identity 1, next identity 2), encoded according to the old
codec, including its FNV checksum. Both columns are non-nullable in these formats.

The nullable-storage test reads both fixtures, appends a current-format CORECHG7 record to a
copy of the old log, reopens it, and checks that the original constraints survive.

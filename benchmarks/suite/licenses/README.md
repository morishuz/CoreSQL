`../duckdb.json` adapts query definitions from DuckDB under the preserved
[MIT notice](DuckDB-MIT.txt).

`../h2o.json` adapts SQL from h2oai/db-benchmark. This Source Code Form is subject
to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
The full original [MPL-2.0 text](H2O-MPL-2.0.txt) is retained here. Adaptations:
SELECT extraction, explicit ON/column equivalents, source metadata and reduced
fixture descriptions. Source form is distributed in the adjacent JSON catalog.

`../speedtest1.json` adapts SQLite speedtest1 query shapes; see the preserved
[SQLite notice](../../../third_party/notices/SQLite.md). Exact source revisions,
paths and hashes (including public license files) are in `../sources.json`.
These materials retain their upstream ownership. Other runner/fixture/control
code in this directory is original CoreSQL code covered by the root LICENSE.

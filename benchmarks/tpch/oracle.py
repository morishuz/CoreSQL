"""Optional pinned DuckDB adapter for the synthetic query fixtures."""
class DuckDB:
    def __init__(self):
        from prepare import reference
        self.db = reference()

    def execute(self, sql):
        import duckdb
        try:
            cursor = self.db.execute(sql)
            # The bridge's statement protocol reports no rows for mutations.
            if not sql.lstrip().lower().startswith(('select', 'with')):
                return 'ok', []
            return 'ok', cursor.fetchall()
        except duckdb.Error as error:
            return 'error', str(error)

    def close(self):
        self.db.close()

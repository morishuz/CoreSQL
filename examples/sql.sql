-- Graph and other custom functions can be registered by embedding applications.
CREATE TABLE items(id INTEGER PRIMARY KEY, label TEXT);
BEGIN;
INSERT INTO items VALUES(1,'one'),(2,'two'),(3,'three');
UPDATE items SET label='TWO' WHERE id=2;
COMMIT;
SELECT id,label FROM items WHERE id BETWEEN 1 AND 3 ORDER BY id DESC;
SELECT count(*),sum(id) FROM items;
PRAGMA integrity_check;

CREATE TABLE people (id INT PRIMARY KEY, name TEXT NOT NULL, age INT, dept TEXT);
CREATE TABLE orders (id INT PRIMARY KEY, person_id INT, total REAL);
CREATE INDEX idx_dept ON people(dept);
CREATE INDEX idx_person ON orders(person_id);

INSERT INTO people (id, name, age, dept) VALUES
  (1, 'Alice', 30, 'eng'), (2, 'Bob', 25, 'sales'), (3, 'Carol', 40, 'eng'), (4, 'Dave', 22, 'sales');
INSERT INTO orders (id, person_id, total) VALUES
  (1, 1, 100.5), (2, 1, 50.25), (3, 2, 20.0), (4, 3, 999.99);

SELECT * FROM people;
SELECT name, age FROM people WHERE dept = 'eng' ORDER BY age DESC;
SELECT id, name FROM people WHERE age > 20 AND age < 35 ORDER BY name LIMIT 2;
SELECT p.name, o.total FROM people AS p JOIN orders AS o ON p.id = o.person_id WHERE p.dept = 'eng' ORDER BY o.total DESC;
SELECT COUNT(*), SUM(age), AVG(age), MIN(age), MAX(age) FROM people;

UPDATE people SET age = age + 1 WHERE dept = 'eng';
SELECT name, age FROM people ORDER BY id;

DELETE FROM people WHERE dept = 'sales';
SELECT name FROM people ORDER BY id;

-- explicit transaction: rollback discards changes
BEGIN;
INSERT INTO people (id, name, age, dept) VALUES (5, 'Eve', 50, 'ops');
SELECT name FROM people ORDER BY id;
ROLLBACK;
SELECT name FROM people ORDER BY id;

-- explicit transaction: commit persists changes
BEGIN;
INSERT INTO people (id, name, age, dept) VALUES (5, 'Eve', 50, 'ops');
COMMIT;
SELECT name FROM people ORDER BY id;

-- autocommit: a failing multi-row INSERT rolls back entirely
INSERT INTO people (id, name, age, dept) VALUES (6, 'Frank', 33, 'ops'), (7, NULL, 1, 'x');
SELECT name FROM people ORDER BY id;

.tables
.schema people

DROP INDEX idx_dept;
DROP TABLE orders;
.tables

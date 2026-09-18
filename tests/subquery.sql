CREATE TABLE dept (id INT PRIMARY KEY, name TEXT);
CREATE TABLE emp (id INT PRIMARY KEY, name TEXT, dept_id INT, salary INT);
INSERT INTO dept (id, name) VALUES (1, 'eng'), (2, 'sales');
INSERT INTO emp (id, name, dept_id, salary) VALUES
  (1, 'Alice', 1, 100), (2, 'Bob', 1, 200), (3, 'Carol', 2, 50), (4, 'Dave', 2, 60);

-- non-correlated scalar subquery in WHERE
SELECT name FROM emp WHERE salary = (SELECT MAX(salary) FROM emp);

-- non-correlated scalar subquery in SELECT list
SELECT name, (SELECT MAX(salary) FROM emp) AS top FROM emp ORDER BY id;

-- correlated scalar subquery in SELECT list (per-row)
SELECT e.name, (SELECT d.name FROM dept d WHERE d.id = e.dept_id) AS dept_name FROM emp e ORDER BY e.id;

-- correlated scalar subquery in WHERE (per-row comparison against a per-dept aggregate)
SELECT e.name FROM emp e WHERE e.salary > (SELECT AVG(salary) FROM emp e2 WHERE e2.dept_id = e.dept_id) ORDER BY e.name;

-- subquery returning zero rows -> NULL
SELECT id, (SELECT id FROM dept WHERE name = 'nonexistent') AS missing FROM dept WHERE id = 1;

-- subquery with more than one row -> error
SELECT id, (SELECT salary FROM emp) AS bad FROM dept WHERE id = 1;

-- subquery with more than one column -> error
SELECT id, (SELECT id, name FROM dept LIMIT 1) AS bad2 FROM dept WHERE id = 1;

DROP TABLE emp;
DROP TABLE dept;

CREATE TABLE emp (id INT PRIMARY KEY, name TEXT NOT NULL, dept TEXT, salary INT);
INSERT INTO emp (id, name, dept, salary) VALUES
  (1, 'Alice', 'eng', 100), (2, 'Bob', 'eng', 200), (3, 'Carol', 'sales', 50),
  (4, 'Dave', 'sales', 60), (5, 'Eve', 'sales', 70), (6, 'Frank', 'ops', 300);

-- basic GROUP BY with aggregates
SELECT dept, COUNT(*), SUM(salary), AVG(salary) FROM emp GROUP BY dept ORDER BY dept;

-- GROUP BY + HAVING filtering on an aggregate
SELECT dept, COUNT(*) AS n FROM emp GROUP BY dept HAVING COUNT(*) >= 2 ORDER BY dept;

-- HAVING referencing an aggregate not in the select list
SELECT dept FROM emp GROUP BY dept HAVING SUM(salary) > 150 ORDER BY dept;

-- GROUP BY with WHERE pre-filter
SELECT dept, COUNT(*) FROM emp WHERE salary > 55 GROUP BY dept ORDER BY dept;

-- GROUP BY acting like DISTINCT (no aggregates)
SELECT dept FROM emp GROUP BY dept ORDER BY dept;

-- ORDER BY an aggregate not in the select list
SELECT dept, COUNT(*) AS n FROM emp GROUP BY dept ORDER BY COUNT(*) DESC;

-- LIMIT with GROUP BY
SELECT dept, COUNT(*) FROM emp GROUP BY dept ORDER BY dept LIMIT 2;

-- single-column GROUP BY key with multiple aggregates and HAVING combining AND
SELECT dept, MIN(salary), MAX(salary) FROM emp GROUP BY dept HAVING COUNT(*) > 1 AND MAX(salary) < 250 ORDER BY dept;

DROP TABLE emp;

CREATE TABLE dept (id INT PRIMARY KEY, name TEXT);
CREATE TABLE emp (id INT PRIMARY KEY, name TEXT, dept_id INT, salary INT);
INSERT INTO dept (id, name) VALUES (1, 'eng'), (2, 'sales');
INSERT INTO emp (id, name, dept_id, salary) VALUES
  (1, 'Alice', 1, 100), (2, 'Bob', 1, 200), (3, 'Carol', 2, 50), (4, 'Dave', 2, 60);

-- subquery inside INSERT value expression
INSERT INTO emp (id, name, dept_id, salary) VALUES (5, 'Eve', 1, (SELECT MAX(salary) FROM emp) + 1);
SELECT name, salary FROM emp WHERE id = 5;

-- subquery inside UPDATE SET expression, correlated to the row being updated
UPDATE emp SET salary = salary + (SELECT COUNT(*) FROM dept WHERE dept.id = emp.dept_id) WHERE id = 1;
SELECT name, salary FROM emp WHERE id = 1;

-- subquery inside DELETE WHERE
DELETE FROM emp WHERE salary < (SELECT AVG(salary) FROM emp);
SELECT name FROM emp ORDER BY name;

-- subquery referencing a nonexistent table -> error surfaces correctly
SELECT name, (SELECT x FROM nosuchtable) FROM emp;

-- GROUP BY with a correlated subquery in the select list (evaluated against the group's representative row)
SELECT e.dept_id, COUNT(*), (SELECT d.name FROM dept d WHERE d.id = e.dept_id) FROM emp e GROUP BY e.dept_id ORDER BY e.dept_id;

DROP TABLE emp;
DROP TABLE dept;

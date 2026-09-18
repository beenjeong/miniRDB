CREATE TABLE dept (id INT PRIMARY KEY, name TEXT);
CREATE TABLE emp (id INT PRIMARY KEY, dept_id INT, salary INT);
INSERT INTO dept (id, name) VALUES (1, 'eng'), (2, 'sales');
INSERT INTO emp (id, dept_id, salary) VALUES (1, 1, 100), (2, 1, 200), (3, 2, 50), (4, 2, 60);

-- GROUP BY across a JOIN
SELECT d.name, COUNT(*), SUM(e.salary) FROM dept d JOIN emp e ON d.id = e.dept_id GROUP BY d.name ORDER BY d.name;

-- still an error without GROUP BY: mixing aggregate + plain column
SELECT d.name, COUNT(*) FROM dept d JOIN emp e ON d.id = e.dept_id;

DROP TABLE emp;
DROP TABLE dept;

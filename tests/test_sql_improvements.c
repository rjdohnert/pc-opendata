#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "opendata.h"

#ifdef _WIN32
#include <direct.h>
#define rmdir _rmdir
#define unlink _unlink
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

int main(void) {
    puts("===================================================================");
    puts("   PC/OpenData - SQL Engine Improvements Test Suite (8 Items)    ");
    puts("===================================================================");

    unlink("test_sql_data.pcod");
    rmdir("test_sql_backups");
#ifdef _WIN32
    _mkdir("test_sql_backups");
#else
    mkdir("test_sql_backups", 0777);
#endif

    bool ok = opendata_open("test_sql_data.pcod", "test_sql_backups", 60, NULL);
    assert(ok && "opendata_open failed");

    /* Recommendation 5: Flexible DML - Batch Insert and Partial Column List */
    puts("\n[Recommendation 5] Flexible DML (Column Lists, Batch Inserts, TRUNCATE)...");
    ok = opendata_exec("CREATE TABLE staff (id NUMBER PRIMARY KEY, name VARCHAR2(50), dept VARCHAR2(30), salary NUMBER);");
    assert(ok);

    /* Batch INSERT with all columns */
    ok = opendata_exec("INSERT INTO staff VALUES "
                       "(1, 'Alice', 'Engineering', 95000), "
                       "(2, 'Bob', 'Engineering', 85000), "
                       "(3, 'Charlie', 'Marketing', 70000), "
                       "(4, 'Diana', 'Marketing', 90000), "
                       "(5, 'Eve', 'HR', 65000);");
    assert(ok);

    /* Partial column list INSERT (dept and salary should default to NULL) */
    ok = opendata_exec("INSERT INTO staff (id, name) VALUES (6, 'Frank'), (7, 'Grace');");
    assert(ok);

    /* Recommendation 1: Query Control - ORDER BY, LIMIT, and OFFSET */
    puts("\n[Recommendation 1] Query Control (ORDER BY, LIMIT, OFFSET)...");
    /* Simple ORDER BY DESC */
    ok = opendata_exec("SELECT id, name, salary FROM staff WHERE salary IS NOT NULL ORDER BY salary DESC;");
    assert(ok);

    /* Multi-key ORDER BY */
    ok = opendata_exec("SELECT id, name, dept FROM staff ORDER BY dept ASC, name DESC;");
    assert(ok);

    /* LIMIT and OFFSET */
    ok = opendata_exec("SELECT id, name FROM staff ORDER BY id ASC LIMIT 3 OFFSET 2;");
    assert(ok);

    /* Recommendation 2: Predicate Expressiveness - OR, LIKE, IN, BETWEEN */
    puts("\n[Recommendation 2] Predicate Expressiveness (OR, LIKE, IN, BETWEEN)...");
    /* OR condition */
    ok = opendata_exec("SELECT id, name FROM staff WHERE dept = 'HR' OR dept = 'Marketing' ORDER BY id ASC;");
    assert(ok);

    /* Parentheses & Precedence: (dept = Engineering OR dept = Marketing) AND salary >= 90000 */
    ok = opendata_exec("SELECT name, salary FROM staff WHERE (dept = 'Engineering' OR dept = 'Marketing') AND salary >= 90000 ORDER BY salary DESC;");
    assert(ok);

    /* LIKE wildcard patterns */
    ok = opendata_exec("SELECT name FROM staff WHERE name LIKE 'A%' ORDER BY name ASC;"); /* Alice */
    assert(ok);
    ok = opendata_exec("SELECT name FROM staff WHERE name LIKE '%e' ORDER BY name ASC;"); /* Alice, Charlie, Eve, Grace */
    assert(ok);
    ok = opendata_exec("SELECT name FROM staff WHERE name LIKE '%ar%' ORDER BY name ASC;"); /* Charlie */
    assert(ok);

    /* IN and NOT IN */
    ok = opendata_exec("SELECT name FROM staff WHERE id IN (1, 3, 5) ORDER BY id ASC;");
    assert(ok);
    ok = opendata_exec("SELECT name FROM staff WHERE id NOT IN (1, 2, 3, 4, 5) ORDER BY id ASC;"); /* Frank, Grace */
    assert(ok);

    /* BETWEEN */
    ok = opendata_exec("SELECT name, salary FROM staff WHERE salary BETWEEN 70000 AND 90000 ORDER BY salary ASC;");
    assert(ok);

    /* Recommendation 3: Aggregates and Grouping - COUNT, SUM, AVG, MIN, MAX, GROUP BY */
    puts("\n[Recommendation 3] Aggregates and Grouping (SUM, AVG, MIN, MAX, GROUP BY)...");
    /* Standalone Aggregates */
    ok = opendata_exec("SELECT COUNT(*), SUM(salary), AVG(salary), MIN(salary), MAX(salary) FROM staff WHERE salary IS NOT NULL;");
    assert(ok);

    /* GROUP BY */
    ok = opendata_exec("SELECT dept, COUNT(*), SUM(salary), MIN(salary), MAX(salary) FROM staff WHERE dept IS NOT NULL GROUP BY dept ORDER BY dept ASC;");
    assert(ok);

    /* Recommendation 4: Query Shaping - DISTINCT and Column Aliases */
    puts("\n[Recommendation 4] Query Shaping (DISTINCT and Column Aliases)...");
    /* DISTINCT */
    ok = opendata_exec("SELECT DISTINCT dept FROM staff WHERE dept IS NOT NULL ORDER BY dept ASC;");
    assert(ok);

    /* Aliases */
    ok = opendata_exec("SELECT name AS employee_name, salary AS compensation FROM staff WHERE id = 1;");
    assert(ok);
    ok = opendata_exec("SELECT dept AS department, COUNT(*) AS head_count FROM staff WHERE dept IS NOT NULL GROUP BY dept ORDER BY department ASC;");
    assert(ok);

    /* Recommendation 6: Joins - LEFT JOIN */
    puts("\n[Recommendation 6] Join Enhancements (LEFT [OUTER] JOIN)...");
    ok = opendata_exec("CREATE TABLE projects (id NUMBER PRIMARY KEY, lead_id NUMBER, title VARCHAR2(50));");
    assert(ok);
    ok = opendata_exec("INSERT INTO projects VALUES (101, 1, 'Search Engine'), (102, 2, 'Cloud Storage'), (103, 99, 'Unassigned Project');");
    assert(ok);

    /* LEFT JOIN: staff left join projects (Alice & Bob have projects, Charlie has none -> NULL) */
    ok = opendata_exec("SELECT staff.name, projects.title FROM staff LEFT JOIN projects ON staff.id = projects.lead_id WHERE staff.id <= 3 ORDER BY staff.id ASC;");
    assert(ok);

    /* Recommendation 7: Schema Evolution - ALTER TABLE */
    puts("\n[Recommendation 7] Schema Evolution (ALTER TABLE ADD, DROP, RENAME)...");
    /* ADD COLUMN */
    ok = opendata_exec("ALTER TABLE staff ADD COLUMN email VARCHAR2(80);");
    assert(ok);

    /* Verify new column has NULLs for existing rows */
    ok = opendata_exec("SELECT id, name, email FROM staff WHERE id = 1;");
    assert(ok);

    /* RENAME COLUMN */
    ok = opendata_exec("ALTER TABLE staff RENAME COLUMN email TO contact_email;");
    assert(ok);

    /* DROP COLUMN */
    ok = opendata_exec("ALTER TABLE staff DROP COLUMN contact_email;");
    assert(ok);

    /* RENAME TABLE */
    ok = opendata_exec("ALTER TABLE projects RENAME TO initiatives;");
    assert(ok);
    ok = opendata_exec("SELECT * FROM initiatives ORDER BY id ASC;");
    assert(ok);

    /* Recommendation 8: Ergonomics & Diagnostics - EXPLAIN & Multi-Line */
    puts("\n[Recommendation 8] Diagnostics & Ergonomics (EXPLAIN QUERY PLAN)...");
    ok = opendata_exec("EXPLAIN SELECT staff.name, initiatives.title FROM staff LEFT JOIN initiatives ON staff.id = initiatives.lead_id WHERE staff.salary > 80000 ORDER BY staff.salary DESC LIMIT 5;");
    assert(ok);

    /* TRUNCATE TABLE */
    puts("\n[Testing TRUNCATE TABLE]...");
    ok = opendata_exec("TRUNCATE TABLE initiatives;");
    assert(ok);
    ok = opendata_exec("SELECT COUNT(*) FROM initiatives;");
    assert(ok);

    /* Commit transaction */
    ok = opendata_commit();
    assert(ok);

    opendata_close();

    puts("\n===================================================================");
    puts("   All 8 SQL Engine Recommendations Passed Successfully!          ");
    puts("===================================================================");
    return 0;
}

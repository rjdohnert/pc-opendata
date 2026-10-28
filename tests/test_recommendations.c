#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "opendata.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define rmdir _rmdir
#define unlink _unlink
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("===================================================================");
    puts("   PC/OpenData - 6 Advanced Recommendations Verification Suite     ");
    puts("===================================================================");

    unlink("test_rec_data.pcod");
    unlink("test_rec_data.pcod.lock");
    rmdir("test_rec_backups");
#ifdef _WIN32
    _mkdir("test_rec_backups");
#else
    mkdir("test_rec_backups", 0777);
#endif

    bool ok = opendata_open("test_rec_data.pcod", "test_rec_backups", 60, NULL);
    assert(ok && "opendata_open failed");

    /* ------------------------------------------------------------- */
    /* 1. Recommendation 1: Unified Storage Engine Dispatch          */
    /* ------------------------------------------------------------- */
    puts("\n[1] Testing Unified Storage Engine Dispatch (MEMORY vs DISK)...");
    ok = opendata_exec("CREATE TABLE cache_tbl (id NUMBER PRIMARY KEY, val VARCHAR2(50)) ENGINE = MEMORY;");
    assert(ok);
    ok = opendata_exec("CREATE TABLE store_tbl (id NUMBER PRIMARY KEY, val VARCHAR2(50)) ENGINE = DISK;");
    assert(ok);
    ok = opendata_exec("INSERT INTO store_tbl VALUES (1, 'DiskRecord1'), (2, 'DiskRecord2');");
    assert(ok);
    ok = opendata_exec("SELECT * FROM store_tbl ORDER BY id ASC;");
    assert(ok);

    /* ------------------------------------------------------------- */
    /* 2. Recommendation 3: N-Way Joins (4-table relational chain)   */
    /* ------------------------------------------------------------- */
    puts("\n[2] Testing N-Way Table Joins (4-table chain: customers -> orders -> items -> products)...");
    ok = opendata_exec("CREATE TABLE customers (id NUMBER PRIMARY KEY, name VARCHAR2(50));"); assert(ok);
    ok = opendata_exec("CREATE TABLE orders (id NUMBER PRIMARY KEY, cust_id NUMBER);"); assert(ok);
    ok = opendata_exec("CREATE TABLE items (id NUMBER PRIMARY KEY, order_id NUMBER, prod_id NUMBER, qty NUMBER);"); assert(ok);
    ok = opendata_exec("CREATE TABLE products (id NUMBER PRIMARY KEY, title VARCHAR2(50), price NUMBER);"); assert(ok);

    ok = opendata_exec("INSERT INTO customers VALUES (1, 'Acme Corp'), (2, 'Beta LLC');"); assert(ok);
    ok = opendata_exec("INSERT INTO orders VALUES (101, 1), (102, 2);"); assert(ok);
    ok = opendata_exec("INSERT INTO items VALUES (1001, 101, 501, 10), (1002, 101, 502, 5), (1003, 102, 501, 2);"); assert(ok);
    ok = opendata_exec("INSERT INTO products VALUES (501, 'Widget A', 25), (502, 'Widget B', 50);"); assert(ok);

    /* 4-way join */
    ok = opendata_exec("SELECT customers.name, orders.id, products.title, items.qty "
                       "FROM customers "
                       "INNER JOIN orders ON customers.id = orders.cust_id "
                       "INNER JOIN items ON orders.id = items.order_id "
                       "LEFT JOIN products ON items.prod_id = products.id "
                       "ORDER BY items.id ASC;");
    assert(ok);

    /* ------------------------------------------------------------- */
    /* 3. Recommendation 4: Scalar Arithmetic Expressions            */
    /* ------------------------------------------------------------- */
    puts("\n[3] Testing Arithmetic & Scalar Expressions in Projection and WHERE...");
    ok = opendata_exec("CREATE TABLE payroll (id NUMBER PRIMARY KEY, name VARCHAR2(50), salary NUMBER);"); assert(ok);
    ok = opendata_exec("INSERT INTO payroll VALUES (1, 'Alice', 10000), (2, 'Bob', 8000), (3, 'Charlie', 5000);"); assert(ok);

    /* Arithmetic in SELECT projections: salary * 1.10, salary + 500, salary / 2 */
    ok = opendata_exec("SELECT name, salary * 1.10 AS bonus_salary, salary + 500 AS adjusted, salary / 2 AS half FROM payroll ORDER BY id ASC;");
    assert(ok);

    /* Arithmetic in WHERE clause: salary * 12 > 100000 */
    ok = opendata_exec("SELECT name, salary FROM payroll WHERE salary * 12 > 100000 ORDER BY salary DESC;");
    assert(ok);

    /* ------------------------------------------------------------- */
    /* 4. Recommendation 5: Subqueries and CTEs                      */
    /* ------------------------------------------------------------- */
    puts("\n[4] Testing Subqueries and Common Table Expressions (CTEs)...");
    /* CTE: WITH high_earners AS (SELECT ...) SELECT ... */
    ok = opendata_exec("WITH high_earners AS (SELECT name, salary FROM payroll WHERE salary >= 8000) SELECT name, salary FROM high_earners ORDER BY salary DESC;");
    assert(ok);

    /* WHERE col IN (SELECT col FROM ...) subquery */
    ok = opendata_exec("SELECT name FROM payroll WHERE id IN (SELECT cust_id FROM orders WHERE id = 101);");
    assert(ok);

    /* ------------------------------------------------------------- */
    /* 5. Recommendation 2: Prepared Statements & Parameter Binding   */
    /* ------------------------------------------------------------- */
    puts("\n[5] Testing Prepared Statements API (prepare, bind, step, column)...");
    OpendataStmt* stmt = opendata_prepare("SELECT name, salary FROM payroll WHERE salary >= ? ORDER BY salary DESC;");
    assert(stmt != NULL);

    ok = opendata_bind_int(stmt, 1, 8000);
    assert(ok);

    int rows_found = 0;
    while (opendata_step(stmt)) {
        const char* name = opendata_column_text(stmt, 0);
        const char* salary = opendata_column_text(stmt, 1);
        assert(name != NULL && salary != NULL);
        printf("  -> Row %d: %s | %s\n", rows_found + 1, name, salary);
        rows_found++;
    }
    assert(rows_found == 2 && "Should find Alice (10000) and Bob (8000)");

    /* Reset and re-bind with different parameter */
    ok = opendata_reset(stmt);
    assert(ok);
    ok = opendata_bind_int(stmt, 1, 10000);
    assert(ok);

    rows_found = 0;
    while (opendata_step(stmt)) {
        rows_found++;
    }
    assert(rows_found == 1 && "Should find only Alice (10000)");

    opendata_finalize(stmt);

    /* Commit durable state */
    ok = opendata_commit();
    assert(ok);
    opendata_close();

    /* ------------------------------------------------------------- */
    /* 6. Recommendation 6: SWMR Concurrency (Read-Only Mode)        */
    /* ------------------------------------------------------------- */
    puts("\n[6] Testing Single-Writer / Multi-Reader (SWMR) Read-Only Mode...");
    ok = opendata_open_readonly("test_rec_data.pcod");
    assert(ok && "opendata_open_readonly must succeed on committed database");

    /* Can execute SELECT queries */
    ok = opendata_exec("SELECT name, salary FROM payroll ORDER BY salary DESC;");
    assert(ok && "SELECT must succeed in read-only mode");

    /* Mutations must be blocked */
    ok = opendata_exec("INSERT INTO payroll VALUES (4, 'Dave', 7000);");
    assert(!ok && "Mutations must fail in read-only mode");

    ok = opendata_exec("UPDATE payroll SET salary = 20000 WHERE id = 1;");
    assert(!ok && "Updates must fail in read-only mode");

    ok = opendata_commit();
    assert(!ok && "Commit must fail in read-only mode");

    opendata_close();

    puts("\n===================================================================");
    puts("   All 6 Advanced Recommendations Passed Successfully!             ");
    puts("===================================================================");
    return 0;
}

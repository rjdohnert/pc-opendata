#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif
#include "opendata.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define unlink _unlink
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Testing PC/OpenData Embedded C API & Next-Gen Features ===\n");

#ifdef _WIN32
    CreateDirectoryA("test_db_dir", NULL);
    CreateDirectoryA("test_db_dir\\backups", NULL);
    const char* db = "test_db_dir\\embedded.pcod";
    const char* backups = "test_db_dir\\backups";
#else
    mkdir("test_db_dir", 0755);
    mkdir("test_db_dir/backups", 0755);
    const char* db = "test_db_dir/embedded.pcod";
    const char* backups = "test_db_dir/backups";
#endif

    unlink(db);

    /* 1. Test opendata_open */
    bool ok = opendata_open(db, backups, 60, NULL);
    assert(ok && "opendata_open must succeed");
    assert(opendata_get_generation() == 0 && "Initial generation must be 0");
    assert(!opendata_has_pending_changes() && "No pending changes initially");

    /* 2. Test DDL and DML via opendata_exec */
    ok = opendata_exec("CREATE TABLE departments (id NUMBER PRIMARY KEY, name VARCHAR2(50) NOT NULL);");
    assert(ok && "CREATE TABLE departments");
    assert(opendata_has_pending_changes() && "Changes must be pending after DDL");

    ok = opendata_exec("CREATE TABLE employees (id NUMBER PRIMARY KEY, name VARCHAR2(50) NOT NULL, dept_id NUMBER REFERENCES departments(id));");
    assert(ok && "CREATE TABLE employees");

    ok = opendata_exec("INSERT INTO departments VALUES (1, 'Engineering');");
    assert(ok && "INSERT INTO departments");

    ok = opendata_exec("INSERT INTO departments VALUES (2, 'Design');");
    assert(ok && "INSERT INTO departments 2");

    ok = opendata_exec("INSERT INTO employees VALUES (10, 'Alice', 1);");
    assert(ok && "INSERT INTO employees 10");

    ok = opendata_exec("INSERT INTO employees VALUES (20, 'Bob', 1);");
    assert(ok && "INSERT INTO employees 20");

    /* 3. Test Foreign Key enforcement */
    ok = opendata_exec("INSERT INTO employees VALUES (30, 'Charlie', 99);");
    assert(!ok && "FK constraint violation must fail");

    /* 4. Test Unique / Primary Key enforcement */
    ok = opendata_exec("INSERT INTO employees VALUES (10, 'Duplicate', 1);");
    assert(!ok && "PK constraint violation must fail");

    /* 5. Commit transaction and check generation */
    ok = opendata_commit();
    assert(ok && "opendata_commit must succeed");
    assert(opendata_get_generation() == 1 && "Generation must advance to 1 after commit");
    assert(!opendata_has_pending_changes() && "No pending changes after commit");

    /* 6. Test Query Execution with Hash Join */
    printf("Running query...\n");
    ok = opendata_exec("SELECT employees.name, departments.name FROM employees INNER JOIN departments ON employees.dept_id = departments.id;");
    printf("Query result: %d\n", ok);
    assert(ok && "SELECT query must succeed");

    /* 7. Test Rollback */
    printf("Running update...\n");
    ok = opendata_exec("UPDATE employees SET name = 'Modified' WHERE id = 10;");
    printf("Update result: %d\n", ok);
    assert(ok && "UPDATE must succeed");
    assert(opendata_has_pending_changes() && "Changes must be pending");

    ok = opendata_rollback();
    assert(ok && "opendata_rollback must succeed");
    assert(!opendata_has_pending_changes() && "No pending changes after rollback");
    assert(opendata_get_generation() == 1 && "Generation unchanged after rollback");

    /* 8. Clean Close */
    opendata_close();

    printf("=== All Embedded C API & Engine Tests Passed Successfully! ===\n");
    return 0;
}

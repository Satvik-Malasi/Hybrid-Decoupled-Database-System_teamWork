/* common.c - error message and tiny SQL helpers */
#include <stdarg.h>
#include "common.h"

static char last_error[512] = "";

void set_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_error, sizeof(last_error), format, args);
    va_end(args);
}

const char *get_error(void) {
    return last_error;
}

int db_run(sqlite3 *db, const char *sql) {
    char *message = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &message) != SQLITE_OK) {
        set_error("%s", message ? message : sqlite3_errmsg(db));
        sqlite3_free(message);
        return -1;
    }
    return 0;
}

int db_get_int(sqlite3 *db, const char *sql, int *result) {
    sqlite3_stmt *stmt;
    int status;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(db));
        return -1;
    }
    status = sqlite3_step(stmt);
    if (status == SQLITE_ROW) {
        *result = sqlite3_column_int(stmt, 0);
    } else if (status == SQLITE_DONE) {
        *result = 0;
    } else {
        set_error("%s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

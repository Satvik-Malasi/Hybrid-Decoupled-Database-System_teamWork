/* sqlconsole.c - PART 4 (DBMS side): guarded SQL access */
#include <ctype.h>
#include "sqlconsole.h"
#include "space.h"

static int table_is_editable(const char *table) {
    return table && (strcmp(table, "files") == 0 || strcmp(table, "file_tags") == 0);
}

static int column_is_protected(const char *column) {
    if (!column) return 1;
    if (strcmp(column, "id") == 0 || strcmp(column, "mode") == 0 || strcmp(column, "size") == 0) return 1;
    return strncmp(column, "block", 5) == 0;
}

/* What the statement being prepared does first. Deleting a file row makes sqlite also clear that
 * file's log rows (a foreign-key cascade), and that shows up as a second DELETE on wal_log. */
typedef struct {
    int writes_seen;
    int first_action;
    char first_table[32];
} Guard;

/* sqlite calls this for every action a statement would perform, while it is being prepared */
static int guard(void *context, int action, const char *arg1, const char *arg2, const char *database, const char *trigger) {
    Guard *state = context;
    (void)database;

    switch (action) {
    case SQLITE_SELECT:
    case SQLITE_READ:
    case SQLITE_FUNCTION:
    case SQLITE_RECURSIVE:
        return SQLITE_OK;
    case SQLITE_INSERT:
    case SQLITE_DELETE:
    case SQLITE_UPDATE:
        if (trigger) return SQLITE_OK;                  /* part of a rule the schema itself runs */
        if (state->writes_seen++ > 0) {
            int cascade = action == SQLITE_DELETE && arg1 && strcmp(arg1, "wal_log") == 0 &&
                          state->first_action == SQLITE_DELETE && strcmp(state->first_table, "files") == 0;
            if (cascade) return SQLITE_OK;
        } else {
            state->first_action = action;
            snprintf(state->first_table, sizeof(state->first_table), "%s", arg1 ? arg1 : "");
        }
        if (!table_is_editable(arg1)) return SQLITE_DENY;
        if (action == SQLITE_UPDATE && strcmp(arg1, "files") == 0 && column_is_protected(arg2)) return SQLITE_DENY;
        return SQLITE_OK;
    default:
        return SQLITE_DENY;
    }
}

static int only_blanks(const char *text) {
    while (*text) {
        if (!isspace((unsigned char)*text) && *text != ';') return 0;
        text++;
    }
    return 1;
}

static void print_value(sqlite3_stmt *stmt, int column, FILE *out) {
    switch (sqlite3_column_type(stmt, column)) {
    case SQLITE_NULL:
        fputs("NULL", out);
        break;
    case SQLITE_BLOB:
        fprintf(out, "<%d bytes>", sqlite3_column_bytes(stmt, column));
        break;
    default:
        fputs((const char *)sqlite3_column_text(stmt, column), out);
    }
}

static int show_rows(sqlite3_stmt *stmt, FILE *out) {
    int columns = sqlite3_column_count(stmt);
    int rows = 0, c, status;

    if (columns > 0) {
        for (c = 0; c < columns; c++) fprintf(out, "%s%s", c ? " | " : "", sqlite3_column_name(stmt, c));
        fputc('\n', out);
    }
    while ((status = sqlite3_step(stmt)) == SQLITE_ROW) {
        for (c = 0; c < columns; c++) {
            if (c) fputs(" | ", out);
            print_value(stmt, c, out);
        }
        fputc('\n', out);
        rows++;
    }
    if (status != SQLITE_DONE) return -1;
    fprintf(out, "(%d row%s)\n", rows, rows == 1 ? "" : "s");
    return 0;
}

int sql_console_run(Volume *v, const char *sql, FILE *out) {
    sqlite3_stmt *stmt = NULL;
    const char *rest = NULL;
    Guard state;
    int status, changed;

    if (only_blanks(sql)) {
        set_error("nothing to run - type a SQL statement");
        return -1;
    }

    memset(&state, 0, sizeof(state));
    sqlite3_set_authorizer(v->db, guard, &state);
    status = sqlite3_prepare_v2(v->db, sql, -1, &stmt, &rest);
    sqlite3_set_authorizer(v->db, NULL, NULL);
    if (status != SQLITE_OK) {
        if (status == SQLITE_AUTH) {
            set_error("not allowed here: you can SELECT, and INSERT/UPDATE/DELETE on files and file_tags "
                      "(not id, mode, size or block columns)");
        } else {
            set_error("%s", sqlite3_errmsg(v->db));
        }
        return -1;
    }
    if (!stmt) {
        set_error("nothing to run - type a SQL statement");
        return -1;
    }
    if (!only_blanks(rest)) {
        sqlite3_finalize(stmt);
        set_error("run one statement at a time");
        return -1;
    }

    if (sqlite3_stmt_readonly(stmt)) {
        status = show_rows(stmt, out);
        if (status != 0) set_error("%s", sqlite3_errmsg(v->db));
        sqlite3_finalize(stmt);
        return status;
    }

    /* a change: all or nothing */
    if (db_run(v->db, "BEGIN IMMEDIATE") != 0) {
        sqlite3_finalize(stmt);
        return -1;
    }
    status = sqlite3_step(stmt);
    if (status != SQLITE_DONE && status != SQLITE_ROW) {
        set_error("%s", sqlite3_errmsg(v->db));
        sqlite3_finalize(stmt);
        sqlite3_exec(v->db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    changed = sqlite3_changes(v->db);
    sqlite3_finalize(stmt);
    if (db_run(v->db, "COMMIT") != 0) {
        sqlite3_exec(v->db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }

    /* rows may have owned blocks: free or reclaim them so the bitmap still matches */
    if (v->disk_ready && space_reconcile(v, NULL, NULL) != 0) return -1;
    fprintf(out, "(%d row%s changed)\n", changed, changed == 1 ? "" : "s");
    return 0;
}

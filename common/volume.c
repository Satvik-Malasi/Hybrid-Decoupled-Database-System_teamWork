/* volume.c - ties the four parts together when a volume is created or opened */
#include <errno.h>
#include "volume.h"
#include "space.h"
#include "schema.h"
#include "journal.h"
#include "tags.h"

static void set_paths(Volume *v, const char *image_path) {
    snprintf(v->image_path, sizeof(v->image_path), "%s", image_path);
    snprintf(v->db_path, sizeof(v->db_path), "%s.db", v->image_path);
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int open_database(Volume *v, int create_new) {
    int flags = SQLITE_OPEN_READWRITE | (create_new ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(v->db_path, &v->db, flags, NULL) != SQLITE_OK) {
        set_error("cannot open database '%s': %s", v->db_path, sqlite3_errmsg(v->db));
        sqlite3_close(v->db);
        v->db = NULL;
        return -1;
    }
    sqlite3_busy_timeout(v->db, 2000);
    return db_run(v->db, "PRAGMA foreign_keys = ON");
}

int volume_create(Volume *v, const char *image_path, unsigned int size_mb, int force) {
    memset(v, 0, sizeof(*v));
    set_paths(v, image_path);

    if (!force && (file_exists(v->image_path) || file_exists(v->db_path))) {
        set_error("'%s' already exists - choose another name or use --force to replace it", v->image_path);
        return -1;
    }
    if (force) {
        remove(v->image_path);
        remove(v->db_path);
        if (file_exists(v->image_path) || file_exists(v->db_path)) {
            set_error("cannot replace '%s' - is it open in another program?", v->image_path);
            return -1;
        }
    }

    if (disk_create(&v->disk, v->image_path, size_mb) != 0) return -1;
    v->disk_ready = 1;

    if (open_database(v, 1) != 0) goto failed;
    if (files_schema_create(v->db) != 0) goto failed;
    if (space_schema_create(v->db) != 0) goto failed;
    if (journal_schema_create(v->db) != 0) goto failed;
    if (tags_schema_create(v->db) != 0) goto failed;
    {
        char sql[64];
        snprintf(sql, sizeof(sql), "PRAGMA user_version = %d", SCHEMA_VERSION);
        if (db_run(v->db, sql) != 0) goto failed;
    }
    return 0;

failed:
    {
        char message[512];
        snprintf(message, sizeof(message), "%s", get_error());   /* keep the reason while cleaning up */
        volume_close(v);
        remove(v->image_path);
        remove(v->db_path);
        set_error("%s", message);
    }
    return -1;
}

int volume_open(Volume *v, const char *image_path, int need_image) {
    int version = 0;

    memset(v, 0, sizeof(*v));
    set_paths(v, image_path);

    if (!file_exists(v->db_path)) {
        set_error("no volume found for '%s' (missing %s) - use: init %s <size_mb>", image_path, v->db_path, image_path);
        return -1;
    }
    if (open_database(v, 0) != 0) return -1;

    if (db_get_int(v->db, "PRAGMA user_version", &version) != 0) {
        volume_close(v);
        return -1;
    }
    if (version != SCHEMA_VERSION) {
        set_error("'%s' was made by another version of HybridFS (format %d, expected %d) - run init to make a new volume",
                  v->db_path, version, SCHEMA_VERSION);
        volume_close(v);
        return -1;
    }

    if (disk_open(&v->disk, v->image_path) == 0) {
        v->disk_ready = 1;
    } else if (need_image) {
        volume_close(v);
        return -1;
    }
    /* a crash can leave half-finished work behind; settle it before anyone uses the volume */
    if (need_image && v->disk_ready && journal_recover(v) != 0) {
        volume_close(v);
        return -1;
    }
    return 0;
}

void volume_close(Volume *v) {
    if (v->db) sqlite3_close(v->db);
    if (v->disk_ready) disk_close(&v->disk);
    v->db = NULL;
    v->disk_ready = 0;
}

int volume_is_open(const Volume *v) {
    return v->db != NULL;
}

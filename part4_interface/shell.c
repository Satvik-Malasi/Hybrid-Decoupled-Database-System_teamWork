/* shell.c - PART 4 (OS side): parsing and running the commands */
#include <ctype.h>
#include <time.h>
#include "shell.h"
#include "tree.h"
#include "fileio.h"
#include "check.h"
#include "journal.h"
#include "space.h"
#include "tags.h"
#include "sqlconsole.h"

#define MAX_WORDS 32

static const char *HELP_TEXT =
    "Volume\n"
    "  init <image> <size_mb> [--force]   create a new volume (1-2000 MB) and open it\n"
    "  use <image>                        open an existing volume\n"
    "  info | space                       show size, free space and counts\n"
    "  check [--fix]                      look for inconsistencies (and repair them)\n"
    "  recover                            finish or undo work interrupted by a crash\n"
    "Files and folders\n"
    "  ls [path]                          list a folder\n"
    "  stat <path>                        show everything stored about an entry\n"
    "  mkdir <path>                       make a folder\n"
    "  touch <path>                       make an empty file\n"
    "  write <path> <text>                replace a file's contents (up to 12288 bytes; \\n = new line)\n"
    "  cat <path>                         print a file\n"
    "  rm <path>                          delete a file\n"
    "  rm -r <path>                       delete a folder and everything in it\n"
    "  rmdir <path>                       delete an empty folder\n"
    "  mv <from> <to>                     rename or move\n"
    "  chown <path> <owner_id>            change the owner\n"
    "Tags and queries\n"
    "  tag <path> <key> [value]           add or change a tag (also: tag <path> key=value)\n"
    "  untag <path> <key>                 remove a tag\n"
    "  tags <path>                        list the tags of an entry\n"
    "  search <key>|<key=value> ...       find entries that have all the given tags\n"
    "  sql <statement>                    SELECT, or INSERT/UPDATE/DELETE on files and file_tags\n"
    "Other\n"
    "  help                               this text\n"
    "  exit                               leave\n";

/* ---------- small helpers ---------- */

static int fail(void) {
    fprintf(stderr, "error: %s\n", get_error());
    return SHELL_FAIL;
}

static int usage(const char *text) {
    fprintf(stderr, "usage: %s\n", text);
    return SHELL_FAIL;
}

static int need_volume(Shell *shell) {
    if (shell->is_open) return 1;
    fprintf(stderr, "error: no volume is open - use: init <image> <size_mb>   or   use <image>\n");
    return 0;
}

/* Splits a line into words. Quotes group words; the quote characters are removed. */
static int split_words(const char *line, char *buffer, size_t buffer_size, char **words, int max_words) {
    int count = 0;
    size_t used = 0;
    const char *p = line;

    while (*p) {
        char quote = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (count >= max_words) break;
        words[count++] = buffer + used;
        while (*p && (quote || !isspace((unsigned char)*p))) {
            if (quote) {
                if (*p == quote) quote = 0;
                else if (used + 1 < buffer_size) buffer[used++] = *p;
            } else if (*p == '"' || *p == '\'') {
                quote = *p;
            } else if (used + 1 < buffer_size) {
                buffer[used++] = *p;
            }
            p++;
        }
        if (used + 1 < buffer_size) buffer[used++] = '\0';
    }
    return count;
}

/* The part of the line after the first `skip` words, untouched (for write, sql, tag values). */
static const char *text_after_words(const char *line, int skip) {
    const char *p = line;
    while (skip-- > 0) {
        char quote = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        while (*p && (quote || !isspace((unsigned char)*p))) {      /* same quoting rules as split_words */
            if (quote) {
                if (*p == quote) quote = 0;
            } else if (*p == '"' || *p == '\'') {
                quote = *p;
            }
            p++;
        }
    }
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static void trim_end(char *text) {
    size_t length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) text[--length] = '\0';
}

static void print_time(FILE *out, long when) {
    time_t stamp = (time_t)when;
    struct tm *parts = localtime(&stamp);
    char text[32] = "?";
    if (parts) strftime(text, sizeof(text), "%Y-%m-%d %H:%M", parts);
    fputs(text, out);
}

/* Turns \n, \t and \\ typed in a write command into the real characters. */
static int unescape(const char *source, unsigned char *target) {
    int length = 0;
    while (*source) {
        if (source[0] == '\\' && source[1] == 'n') { target[length++] = '\n'; source += 2; }
        else if (source[0] == '\\' && source[1] == 't') { target[length++] = '\t'; source += 2; }
        else if (source[0] == '\\' && source[1] == '\\') { target[length++] = '\\'; source += 2; }
        else target[length++] = (unsigned char)*source++;
    }
    return length;
}

/* Do two paths point at the same file? (compares the full paths) */
static int same_file(const char *first, const char *second) {
#ifdef _WIN32
    char a[MAX_PATH_LEN], b[MAX_PATH_LEN];
    if (_fullpath(a, first, sizeof(a)) && _fullpath(b, second, sizeof(b))) return _stricmp(a, b) == 0;
#endif
    return strcmp(first, second) == 0;
}

static void remember_volume(const Volume *volume) {
    char full[MAX_PATH_LEN];
    FILE *file;
#ifdef _WIN32
    if (!_fullpath(full, volume->image_path, sizeof(full))) snprintf(full, sizeof(full), "%s", volume->image_path);
#else
    snprintf(full, sizeof(full), "%s", volume->image_path);
#endif
    file = fopen(REMEMBER_FILE, "w");
    if (file) {
        fprintf(file, "%s\n", full);
        fclose(file);
    }
}

void shell_remembered_volume(char *path, size_t capacity) {
    FILE *file = fopen(REMEMBER_FILE, "r");
    path[0] = '\0';
    if (!file) return;
    if (fgets(path, (int)capacity, file)) trim_end(path);
    else path[0] = '\0';
    fclose(file);
}

/* ---------- start, stop, open ---------- */

void shell_start(Shell *shell, FILE *out) {
    memset(shell, 0, sizeof(*shell));
    shell->out = out;
}

void shell_stop(Shell *shell) {
    if (shell->is_open) volume_close(&shell->volume);
    shell->is_open = 0;
}

static void say_recovery(Shell *shell) {
    Volume *v = &shell->volume;
    if (v->undone_rows || v->redone_blocks || v->bitmap_fixes) {
        fprintf(shell->out, "recovered after an unclean stop: %d unfinished log row(s) discarded, "
                            "%d block(s) rewritten, %d bitmap entries corrected\n",
                v->undone_rows, v->redone_blocks, v->bitmap_fixes);
    }
}

int shell_open_volume(Shell *shell, const char *image_path) {
    Volume next;
    if (volume_open(&next, image_path, 1) != 0) return -1;
    shell_stop(shell);
    shell->volume = next;
    shell->is_open = 1;
    say_recovery(shell);
    return 0;
}

/* ---------- commands ---------- */

static int cmd_init(Shell *shell, char **words, int count) {
    Volume next;
    char *end;
    long size;
    int force = 0;

    if (count == 4 && strcmp(words[3], "--force") == 0) force = 1;
    else if (count != 3) return usage("init <image> <size_mb> [--force]");

    size = strtol(words[2], &end, 10);
    if (*end != '\0' || size < MIN_SIZE_MB || size > MAX_SIZE_MB) {
        fprintf(stderr, "error: size must be a whole number between %d and %d (megabytes)\n", MIN_SIZE_MB, MAX_SIZE_MB);
        return SHELL_FAIL;
    }
    /* replacing the volume that is open right now: let go of its files first */
    if (force && shell->is_open && same_file(shell->volume.image_path, words[1])) shell_stop(shell);
    if (volume_create(&next, words[1], (unsigned int)size, force) != 0) return fail();
    shell_stop(shell);
    shell->volume = next;
    shell->is_open = 1;
    remember_volume(&shell->volume);
    fprintf(shell->out, "created %s (%ld MB, %u blocks of %d bytes) and %s\n", words[1], size,
            shell->volume.disk.total_blocks, BLOCK_SIZE, shell->volume.db_path);
    return SHELL_OK;
}

static int cmd_use(Shell *shell, char **words, int count) {
    if (count != 2) return usage("use <image>");
    if (shell_open_volume(shell, words[1]) != 0) return fail();
    remember_volume(&shell->volume);
    fprintf(shell->out, "opened %s\n", words[1]);
    return SHELL_OK;
}

static void print_listing_line(const Entry *entry, void *context) {
    FILE *out = ((Shell *)context)->out;
    fprintf(out, "%c %5d %9d  ", entry->mode == MODE_DIR ? 'd' : '-', entry->owner_id, entry->size);
    print_time(out, entry->modified_at);
    fprintf(out, "  %s%s\n", entry->name, entry->mode == MODE_DIR ? "/" : "");
}

static int cmd_ls(Shell *shell, char **words, int count) {
    if (count > 2) return usage("ls [path]");
    if (tree_list(&shell->volume, count == 2 ? words[1] : "/", print_listing_line, shell) != 0) return fail();
    return SHELL_OK;
}

static void print_tag_line(const char *key, const char *value, void *context) {
    fprintf(((Shell *)context)->out, "  %s = %s\n", key, value);
}

static int cmd_stat(Shell *shell, char **words, int count) {
    Entry entry;
    char path[MAX_PATH_LEN];
    int i;
    FILE *out = shell->out;

    if (count != 2) return usage("stat <path>");
    if (!tree_find(&shell->volume, words[1], &entry)) return fail();
    if (tree_path_of(&shell->volume, entry.id, path, sizeof(path)) != 0) return fail();

    fprintf(out, "path:      %s\n", path);
    fprintf(out, "id:        %d\n", entry.id);
    fprintf(out, "type:      %s (mode %d)\n", entry.mode == MODE_DIR ? "folder" : "file", entry.mode);
    fprintf(out, "owner:     %d\n", entry.owner_id);
    fprintf(out, "size:      %d bytes\n", entry.size);
    fprintf(out, "created:   ");
    print_time(out, entry.created_at);
    fprintf(out, "\nmodified:  ");
    print_time(out, entry.modified_at);
    fprintf(out, "\nblocks:    ");
    if (entry.mode == MODE_DIR || entry.size == 0) {
        fprintf(out, "none");
    } else {
        for (i = 0; i < MAX_FILE_BLOCKS && entry.blocks[i] > 0; i++) fprintf(out, "%s%d", i ? ", " : "", entry.blocks[i]);
    }
    fprintf(out, "\ntags:\n");
    tags_list(&shell->volume, words[1], print_tag_line, shell);
    return SHELL_OK;
}

static int cmd_make(Shell *shell, char **words, int count, int mode, const char *form) {
    if (count != 2) return usage(form);
    if (tree_make(&shell->volume, words[1], mode, 0, NULL) != 0) return fail();
    return SHELL_OK;
}

static int cmd_write(Shell *shell, const char *line, char **words, int count) {
    char *text;
    unsigned char *bytes;
    size_t length;
    int size, status;

    if (count < 2) return usage("write <path> <text>");
    text = malloc(strlen(line) + 1);
    bytes = malloc(strlen(line) + 1);
    if (!text || !bytes) {
        free(text);
        free(bytes);
        set_error("out of memory");
        return fail();
    }
    strcpy(text, text_after_words(line, 2));
    trim_end(text);
    length = strlen(text);
    if (length >= 2 && (text[0] == '"' || text[0] == '\'') && text[length - 1] == text[0]) {
        text[length - 1] = '\0';
        memmove(text, text + 1, length - 1);
    }
    size = unescape(text, bytes);
    status = file_write(&shell->volume, words[1], bytes, size);
    free(text);
    free(bytes);
    if (status != 0) return fail();
    fprintf(shell->out, "wrote %d bytes to %s\n", size, words[1]);
    return SHELL_OK;
}

static int cmd_cat(Shell *shell, char **words, int count) {
    unsigned char *data;
    int length;

    if (count != 2) return usage("cat <path>");
    if (file_read(&shell->volume, words[1], &data, &length) != 0) return fail();
    fwrite(data, 1, (size_t)length, shell->out);
    if (length > 0 && data[length - 1] != '\n') fputc('\n', shell->out);
    free(data);
    return SHELL_OK;
}

static int cmd_rm(Shell *shell, char **words, int count) {
    Entry entry;
    int recursive = 0, removed = 0;
    const char *path;

    if (count == 3 && (strcmp(words[1], "-r") == 0 || strcmp(words[1], "-rf") == 0)) {
        recursive = 1;
        path = words[2];
    } else if (count == 2) {
        path = words[1];
    } else {
        return usage("rm [-r] <path>");
    }
    if (!tree_find(&shell->volume, path, &entry)) return fail();
    if (entry.mode == MODE_DIR && !recursive) {
        fprintf(stderr, "error: '%s' is a folder - use rm -r to delete it, or rmdir if it is empty\n", path);
        return SHELL_FAIL;
    }
    if (tree_delete(&shell->volume, path, recursive, &removed) != 0) return fail();
    fprintf(shell->out, "deleted %s (%d item%s)\n", path, removed, removed == 1 ? "" : "s");
    return SHELL_OK;
}

static int cmd_rmdir(Shell *shell, char **words, int count) {
    Entry entry;

    if (count != 2) return usage("rmdir <path>");
    if (!tree_find(&shell->volume, words[1], &entry)) return fail();
    if (entry.mode != MODE_DIR) {
        fprintf(stderr, "error: '%s' is a file - use rm\n", words[1]);
        return SHELL_FAIL;
    }
    if (tree_delete(&shell->volume, words[1], 0, NULL) != 0) return fail();
    fprintf(shell->out, "deleted folder %s\n", words[1]);
    return SHELL_OK;
}

static int cmd_mv(Shell *shell, char **words, int count) {
    if (count != 3) return usage("mv <from> <to>");
    if (tree_rename(&shell->volume, words[1], words[2]) != 0) return fail();
    return SHELL_OK;
}

static int cmd_chown(Shell *shell, char **words, int count) {
    char *end;
    long owner;

    if (count != 3) return usage("chown <path> <owner_id>");
    owner = strtol(words[2], &end, 10);
    if (*end != '\0' || owner < 0 || owner > 1000000) {
        fprintf(stderr, "error: owner must be a number from 0 to 1000000\n");
        return SHELL_FAIL;
    }
    if (tree_set_owner(&shell->volume, words[1], (int)owner) != 0) return fail();
    return SHELL_OK;
}

static int cmd_tag(Shell *shell, const char *line, char **words, int count) {
    char key[MAX_TAG_KEY + 2];
    const char *value;
    char value_copy[MAX_TAG_VALUE + 2];
    char *equals;

    if (count < 3) return usage("tag <path> <key> [value]   or   tag <path> key=value");
    equals = strchr(words[2], '=');
    if (count == 3 && equals) {
        size_t key_length = (size_t)(equals - words[2]);
        if (key_length > MAX_TAG_KEY) key_length = MAX_TAG_KEY + 1;
        memcpy(key, words[2], key_length);
        key[key_length] = '\0';
        value = equals + 1;
    } else {
        snprintf(key, sizeof(key), "%s", words[2]);
        snprintf(value_copy, sizeof(value_copy), "%s", text_after_words(line, 3));
        trim_end(value_copy);
        value = value_copy;
    }
    if (tags_set(&shell->volume, words[1], key, value) != 0) return fail();
    return SHELL_OK;
}

static int cmd_untag(Shell *shell, char **words, int count) {
    if (count != 3) return usage("untag <path> <key>");
    if (tags_remove(&shell->volume, words[1], words[2]) != 0) return fail();
    return SHELL_OK;
}

static int cmd_tags(Shell *shell, char **words, int count) {
    if (count != 2) return usage("tags <path>");
    if (tags_list(&shell->volume, words[1], print_tag_line, shell) != 0) return fail();
    return SHELL_OK;
}

typedef struct {
    FILE *out;
    int found;
} SearchOutput;

static void print_match(const char *path, void *context) {
    SearchOutput *result = context;
    result->found++;
    fprintf(result->out, "%s\n", path);
}

static int cmd_search(Shell *shell, char **words, int count) {
    SearchOutput result;
    result.out = shell->out;
    result.found = 0;
    if (count < 2) return usage("search <key> | <key=value> [more terms...]");
    if (tags_search(&shell->volume, count - 1, words + 1, print_match, &result) != 0) return fail();
    fprintf(shell->out, "(%d match%s)\n", result.found, result.found == 1 ? "" : "es");
    return SHELL_OK;
}

static int cmd_sql(Shell *shell, const char *line) {
    const char *statement = text_after_words(line, 1);
    if (*statement == '\0') return usage("sql <statement>");
    if (sql_console_run(&shell->volume, statement, shell->out) != 0) return fail();
    return SHELL_OK;
}

static int cmd_recover(Shell *shell) {
    if (journal_recover(&shell->volume) != 0) return fail();
    fprintf(shell->out, "recovery done: %d unfinished log row(s) discarded, %d block(s) rewritten, "
                        "%d bitmap entries corrected\n",
            shell->volume.undone_rows, shell->volume.redone_blocks, shell->volume.bitmap_fixes);
    return SHELL_OK;
}

static int cmd_check(Shell *shell, char **words, int count) {
    int remaining = 0, fix = 0;

    if (count == 2 && strcmp(words[1], "--fix") == 0) fix = 1;
    else if (count != 1) return usage("check [--fix]");
    if (volume_check(&shell->volume, fix, shell->out, &remaining) != 0) return fail();
    return remaining == 0 ? SHELL_OK : SHELL_FAIL;
}

/* ---------- dispatcher ---------- */

int shell_run_line(Shell *shell, const char *line) {
    char buffer[LINE_LIMIT + 1];
    char *words[MAX_WORDS];
    const char *start = line;
    int count;
    const char *cmd;

    while (*start && isspace((unsigned char)*start)) start++;
    if (*start == '\0' || *start == '#') return SHELL_OK;
    if (strlen(start) > LINE_LIMIT) {
        fprintf(stderr, "error: the line is too long (limit %d characters)\n", LINE_LIMIT);
        return SHELL_FAIL;
    }

    count = split_words(start, buffer, sizeof(buffer), words, MAX_WORDS);
    if (count == 0) return SHELL_OK;
    cmd = words[0];

    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) return SHELL_QUIT;
    if (strcmp(cmd, "help") == 0) {
        fputs(HELP_TEXT, shell->out);
        return SHELL_OK;
    }
    if (strcmp(cmd, "init") == 0) return cmd_init(shell, words, count);
    if (strcmp(cmd, "use") == 0) return cmd_use(shell, words, count);

    if (!need_volume(shell)) return SHELL_FAIL;

    if (strcmp(cmd, "ls") == 0) return cmd_ls(shell, words, count);
    if (strcmp(cmd, "stat") == 0) return cmd_stat(shell, words, count);
    if (strcmp(cmd, "mkdir") == 0) return cmd_make(shell, words, count, MODE_DIR, "mkdir <path>");
    if (strcmp(cmd, "touch") == 0) return cmd_make(shell, words, count, MODE_FILE, "touch <path>");
    if (strcmp(cmd, "write") == 0) return cmd_write(shell, start, words, count);
    if (strcmp(cmd, "cat") == 0) return cmd_cat(shell, words, count);
    if (strcmp(cmd, "rm") == 0) return cmd_rm(shell, words, count);
    if (strcmp(cmd, "rmdir") == 0) return cmd_rmdir(shell, words, count);
    if (strcmp(cmd, "mv") == 0) return cmd_mv(shell, words, count);
    if (strcmp(cmd, "chown") == 0) return cmd_chown(shell, words, count);
    if (strcmp(cmd, "tag") == 0) return cmd_tag(shell, start, words, count);
    if (strcmp(cmd, "untag") == 0) return cmd_untag(shell, words, count);
    if (strcmp(cmd, "tags") == 0) return cmd_tags(shell, words, count);
    if (strcmp(cmd, "search") == 0) return cmd_search(shell, words, count);
    if (strcmp(cmd, "sql") == 0) return cmd_sql(shell, start);
    if (strcmp(cmd, "recover") == 0) return cmd_recover(shell);
    if (strcmp(cmd, "check") == 0) return cmd_check(shell, words, count);
    if (strcmp(cmd, "info") == 0 || strcmp(cmd, "space") == 0) {
        return space_report(&shell->volume, shell->out) == 0 ? SHELL_OK : fail();
    }

    fprintf(stderr, "error: unknown command '%s' - type help\n", cmd);
    return SHELL_FAIL;
}

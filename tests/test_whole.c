/* test_whole.c - the finished programs working together, run exactly as a user would run them */
#include "testkit.h"

#define HYBRIDFS "..\\..\\hybridfs.exe"
#define QUERY_DB "..\\..\\query_db.exe"
#define CRASH_SIM "..\\..\\crash_sim.exe"

static void write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_piped_session(void) {
    int code;
    const char *out;

    delete_volume_files("w1.img");
    write_text_file("script1.txt",
        "init w1.img 4\n"
        "mkdir /reports\n"
        "touch /reports/q1.txt\n"
        "write /reports/q1.txt Quarter one: revenue up\\nsecond line\n"
        "tag /reports/q1.txt type report\n"
        "tag /reports/q1.txt year=2025\n"
        "cat /reports/q1.txt\n"
        "search type=report year=2025\n"
        "sql UPDATE files SET owner_id = 5 WHERE name = 'q1.txt'\n"
        "sql SELECT name, owner_id, size FROM files WHERE name = 'q1.txt'\n"
        "rm /reports/q1.txt\n"
        "rm -r /reports\n"
        "check\n"
        "exit\n");
    code = run_program(HYBRIDFS " < script1.txt", "out1.txt");
    out = read_text_file("out1.txt");
    CHECK(code == 0);
    CHECK(contains(out, "created w1.img"));
    CHECK(contains(out, "Quarter one: revenue up\nsecond line"));
    CHECK(contains(out, "/reports/q1.txt\n(1 match)"));
    CHECK(contains(out, "q1.txt | 5 | 35"));
    CHECK(contains(out, "deleted /reports/q1.txt (1 item)"));
    CHECK(contains(out, "deleted /reports (1 item)"));
    CHECK(contains(out, "ok: no problems found"));
}

static void test_command_mode_and_memory(void) {
    int code;
    const char *out;

    delete_volume_files("w2.img");
    code = run_program(HYBRIDFS " -c \"init w2.img 4\" -c \"mkdir /kept\" -c \"touch /kept/f.txt\" "
                       "-c \"write /kept/f.txt still here after the program ended\"", "out2.txt");
    CHECK(code == 0);

    /* a brand new process: no image argument, the volume is remembered */
    code = run_program(HYBRIDFS " -c \"cat /kept/f.txt\"", "out2.txt");
    out = read_text_file("out2.txt");
    CHECK(code == 0 && contains(out, "still here after the program ended"));

    /* with an explicit image argument */
    code = run_program(HYBRIDFS " w2.img -c \"ls /kept\"", "out2.txt");
    CHECK(code == 0 && contains(read_text_file("out2.txt"), "f.txt"));

    /* a failing command gives a non-zero exit code and a message */
    code = run_program(HYBRIDFS " -c \"cat /kept/missing.txt\"", "out2.txt");
    CHECK(code == 1 && contains(read_text_file("out2.txt"), "error: no such file or folder"));
    code = run_program(HYBRIDFS " -c \"mkdir /kept\"", "out2.txt");
    CHECK(code == 1 && contains(read_text_file("out2.txt"), "already exists"));

    /* the new delete commands, from outside */
    code = run_program(HYBRIDFS " -c \"rm /kept\"", "out2.txt");
    CHECK(code == 1 && contains(read_text_file("out2.txt"), "rm -r"));
    code = run_program(HYBRIDFS " -c \"rm -r /kept\" -c \"ls /\" -c \"info\"", "out2.txt");
    out = read_text_file("out2.txt");
    CHECK(code == 0 && contains(out, "deleted /kept (2 items)") && !contains(out, "kept/") && contains(out, "0 used"));

    code = run_program(HYBRIDFS " no_such_image.img -c \"ls\"", "out2.txt");
    CHECK(code == 1 && contains(read_text_file("out2.txt"), "no volume found"));
    code = run_program(HYBRIDFS " --help", "out2.txt");
    CHECK(code == 0 && contains(read_text_file("out2.txt"), "usage:"));
    code = run_program(HYBRIDFS " -c", "out2.txt");
    CHECK(code == 2);
}

static void test_query_tool(void) {
    int code;
    const char *out;

    delete_volume_files("w3.img");
    code = run_program(HYBRIDFS " -c \"init w3.img 4\" -c \"touch /a.txt\" -c \"touch /b.txt\" "
                       "-c \"tag /a.txt color red\" -c \"tag /b.txt color blue\"", "out3.txt");
    CHECK(code == 0);

    code = run_program(QUERY_DB " -i w3.img \"SELECT name FROM files WHERE id > 1 ORDER BY name\"", "out3.txt");
    out = read_text_file("out3.txt");
    CHECK(code == 0 && contains(out, "a.txt\nb.txt") && contains(out, "(2 rows)"));

    code = run_program(QUERY_DB " -i w3.img \"INSERT INTO file_tags (file_id, tag_key, tag_value) VALUES (2, 'size', 'big')\"", "out3.txt");
    CHECK(code == 0 && contains(read_text_file("out3.txt"), "(1 row changed)"));
    code = run_program(QUERY_DB " -i w3.img \"UPDATE file_tags SET tag_value = 'green' WHERE tag_value = 'blue'\"", "out3.txt");
    CHECK(code == 0 && contains(read_text_file("out3.txt"), "(1 row changed)"));
    code = run_program(HYBRIDFS " w3.img -c \"search color=green\" -c \"search size=big\"", "out3.txt");
    out = read_text_file("out3.txt");
    CHECK(code == 0 && contains(out, "/b.txt") && contains(out, "/a.txt"));
    code = run_program(QUERY_DB " -i w3.img \"DELETE FROM file_tags WHERE tag_key = 'color'\"", "out3.txt");
    CHECK(code == 0 && contains(read_text_file("out3.txt"), "(2 rows changed)"));
    code = run_program(HYBRIDFS " w3.img -c \"search color\"", "out3.txt");
    CHECK(code == 0 && contains(read_text_file("out3.txt"), "(0 matches)"));

    code = run_program(QUERY_DB " -i w3.img \"DROP TABLE files\"", "out3.txt");
    CHECK(code == 1 && contains(read_text_file("out3.txt"), "not allowed"));
    code = run_program(QUERY_DB " -i missing.img \"SELECT 1\"", "out3.txt");
    CHECK(code == 1);

    write_text_file("sql_input.txt", "SELECT COUNT(*) AS n FROM files\nDROP TABLE files\nSELECT 1 + 1 AS two\nexit\n");
    code = run_program(QUERY_DB " -i w3.img < sql_input.txt", "out3.txt");
    out = read_text_file("out3.txt");
    CHECK(code == 1 && contains(out, "n\n3") && contains(out, "two\n2") && contains(out, "not allowed"));
}

static void test_crash_from_outside(void) {
    int code;
    const char *out;

    delete_volume_files("w4.img");
    code = run_program(HYBRIDFS " -c \"init w4.img 4\" -c \"touch /ledger.txt\" -c \"write /ledger.txt balance 100\"", "out4.txt");
    CHECK(code == 0);

    code = run_program(CRASH_SIM " w4.img /ledger.txt after_data", "out4.txt");
    CHECK(code == 86);
    CHECK(contains(read_text_file("out4.txt"), "simulated crash"));

    code = run_program(HYBRIDFS " w4.img -c \"check\" -c \"cat /ledger.txt\"", "out4.txt");
    out = read_text_file("out4.txt");
    CHECK(code == 0);
    CHECK(contains(out, "recovered after an unclean stop"));
    CHECK(contains(out, "balance 100"));                                             /* the old value, intact */
    CHECK(contains(out, "ok: no problems found"));

    code = run_program(HYBRIDFS " w4.img -c \"recover\"", "out4.txt");
    CHECK(code == 0 && contains(read_text_file("out4.txt"), "0 unfinished log row(s) discarded"));
    code = run_program(CRASH_SIM " w4.img /ledger.txt sometime", "out4.txt");
    CHECK(code == 2);
    code = run_program(CRASH_SIM " w4.img /ledger.txt after_commit", "out4.txt");
    CHECK(code == 86);
    code = run_program(HYBRIDFS " w4.img -c \"cat /ledger.txt\"", "out4.txt");
    CHECK(code == 0 && contains(read_text_file("out4.txt"), "THIS IS THE NEW CONTENT"));
}

int main(void) {
    test_begin("whole project - the programs as a user runs them");
    test_piped_session();
    test_command_mode_and_memory();
    test_query_tool();
    test_crash_from_outside();
    return test_end();
}

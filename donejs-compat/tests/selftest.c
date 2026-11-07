/* selftest.c — hosted test harness for donejs-compat.
 *
 * Runs builtin selftest plus targeted checks for language features,
 * Node modules and event loop ordering.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../include/donejs.h"

static void test_writer(void *user, const char *text, size_t length) {
    (void)user;
    fwrite(text, 1, length, stdout);
    fflush(stdout);
}

static void check_eval(djs_runtime_t *rt, const char *code, const char *desc) {
    djs_error_t err = {0};
    int rc = djs_eval(rt, code, desc, &err);
    if (rc != 0) {
        fprintf(stderr, "FAILED check: %s (err: %.*s)\n", desc,
                (int)err.length, err.message ? err.message : "");
    }
    assert(rc == 0);
    printf("PASS: %s\n", desc);
}

int main(void) {
    djs_options_t options = {
        .write_fn = test_writer,
        .write_user = NULL,
        .platform = "linux",
        .arch = "x64",
        .version = "v20.0.0",
        .exec_path = "/usr/bin/donejs",
        .cwd = "/",
    };

    printf("=== Starting donejs-compat hosted selftest ===\n");
    djs_runtime_t *rt = djs_runtime_new(&options);
    assert(rt != NULL);

    /* 1. Built-in selftest suite */
    int failed = djs_selftest(rt, test_writer, NULL);
    printf("djs_selftest finished with %d failures\n", failed);
    assert(failed == 0);

    /* 2. Language features */
    check_eval(rt, "let x = 10; let y = 20; x + y === 30;", "basic let arithmetic");
    check_eval(rt, "const arr = [1, 2, 3]; arr.map(x => x * 2).join(',') === '2,4,6';", "Array.map/join");
    check_eval(rt, "const o = { a: 1, b: 2 }; const { a, b } = o; a === 1 && b === 2;", "destructuring");
    check_eval(rt, "class Animal { constructor(name) { this.name = name; } speak() { return this.name; } } "
                   "class Dog extends Animal { speak() { return super.speak() + ' barks'; } } "
                   "new Dog('Rex').speak() === 'Rex barks';", "class inheritance & super");
    check_eval(rt, "let m = new Map(); m.set('k', 42); m.get('k') === 42 && m.size === 1;", "Map");
    check_eval(rt, "let s = new Set([1, 2, 2, 3]); s.size === 3 && s.has(2);", "Set");

    /* 3. Promises and Event Loop */
    check_eval(rt, "let settled = false; "
                   "Promise.resolve(100).then(v => { settled = (v === 100); }); "
                   "!settled;", "promise is asynchronous");
    djs_run_event_loop(rt);
    check_eval(rt, "settled === true;", "event loop drained promise");

    /* 4. Timers and ordering */
    check_eval(rt, "let timer_fired = false; "
                   "setTimeout(() => { timer_fired = true; }, 10); "
                   "!timer_fired;", "timer queued");
    djs_run_event_loop(rt);
    check_eval(rt, "timer_fired === true;", "timer fired after loop");

    /* 5. Modules */
    check_eval(rt, "const path = require('path'); path.basename('/foo/bar.txt') === 'bar.txt';", "require('path')");
    check_eval(rt, "const util = require('util'); util.format('%s:%d', 'foo', 42) === 'foo:42';", "require('util')");
    check_eval(rt, "const proc = require('process'); proc.arch === 'x64';", "require('process')");

    djs_runtime_free(rt);
    printf("=== All donejs-compat hosted tests passed ===\n");
    return 0;
}

/*
 * The MIT License (MIT)
 *
 * Copyright © 2015 Franklin "Snaipe" Mathieu <http://snai.pe/>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <csptr/smart_ptr.h>
#include "criterion/assert.h"
#include "stats.h"
#include "runner.h"
#include "report.h"
#include "event.h"
#include "process.h"

static struct criterion_test * const g_section_start = &__start_criterion_tests;
static struct criterion_test * const g_section_end   = &__stop_criterion_tests;

struct test_set {
    struct criterion_test **tests;
    size_t nb_tests;
};

static int compare_test(const void *a, const void *b) {
    struct criterion_test *first = *(struct criterion_test **) a;
    struct criterion_test *second = *(struct criterion_test **) b;

    // likely to happen
    if (first->category == second->category) {
        return strcmp(first->name, second->name);
    } else {
        return strcmp(first->category, second->category)
            ?: strcmp(first->name, second->name);
    }
}

static void destroy_test_set(void *ptr, UNUSED void *meta) {
    struct test_set *set = ptr;
    free(set->tests);
}

static struct test_set *read_all_tests(void) {
    size_t nb_tests = g_section_end - g_section_start;

    struct criterion_test **tests = malloc(nb_tests * sizeof (void *));
    if (tests == NULL)
        return NULL;

    size_t i = 0;
    for (struct criterion_test *test = g_section_start; test < g_section_end; ++test)
        tests[i++] = test;

    qsort(tests, nb_tests, sizeof (void *), compare_test);

    return unique_ptr(struct test_set, ({
                .tests = tests,
                .nb_tests = nb_tests
            }), destroy_test_set);
}

static void map_tests(struct test_set *set, struct criterion_global_stats *stats, void (*fun)(struct criterion_global_stats *, struct criterion_test *)) {
    size_t i = 0;
    for (struct criterion_test **t = set->tests; i < set->nb_tests; ++i, ++t) {
        fun(stats, *t);
        if (!is_runner())
            return;
    }
}

__attribute__ ((always_inline))
static inline void nothing() {}

static void run_test_child(struct criterion_test *test) {
    send_event(PRE_INIT, NULL, 0);
    (test->data->init ?: nothing)();
    send_event(PRE_TEST, NULL, 0);
    (test->test       ?: nothing)();
    send_event(POST_TEST, NULL, 0);
    (test->data->fini ?: nothing)();
    send_event(POST_FINI, NULL, 0);
}

static void run_test(struct criterion_global_stats *stats, struct criterion_test *test) {
    smart struct criterion_test_stats *test_stats = test_stats_init(test);

    smart struct process *proc = spawn_test_worker(test, run_test_child);
    if (proc == NULL && !is_runner())
        return;

    struct event *ev;
    while ((ev = worker_read_event(proc)) != NULL) {
        stat_push_event(stats, test_stats, ev);
        switch (ev->kind) {
            case PRE_INIT:  report(PRE_INIT, test); break;
            case PRE_TEST:  report(PRE_TEST, test); break;
            case ASSERT:    report(ASSERT, ev->data); break;
            case POST_TEST: report(POST_TEST, test_stats); break;
            case POST_FINI: report(POST_FINI, test_stats); break;
        }
        sfree(ev);
    }

    struct process_status status = wait_proc(proc);
    if (status.kind == SIGNAL) {
        test_stats->signal = status.status;
        if (test->data->signal == 0) {
            struct event ev = { .kind = TEST_CRASH };
            stat_push_event(stats, test_stats, &ev);
            report(TEST_CRASH, test_stats);
        } else {
            struct event ev = { .kind = POST_TEST };
            stat_push_event(stats, test_stats, &ev);
            report(POST_TEST, test_stats);

            ev.kind = POST_FINI;
            stat_push_event(stats, test_stats, &ev);
            report(POST_FINI, test_stats);
        }
    }
}

// TODO: disable & change tests at runtime
static int criterion_run_all_tests_impl(void) {
    report(PRE_EVERYTHING, NULL);
    set_runner_pid();

    smart struct test_set *set = read_all_tests();
    smart struct criterion_global_stats *stats = stats_init();
    if (!set)
        abort();
    map_tests(set, stats, run_test);

    if (!is_runner())
        return -1;

    report(POST_EVERYTHING, stats);
    return stats->tests_failed > 0;
}

int criterion_run_all_tests(void) {
    int res = criterion_run_all_tests_impl();
    if (res == -1) // if this is the test worker terminating
        exit(0);

    return strcmp("1", getenv("CRITERION_ALWAYS_SUCCEED") ?: "0") && res;
}

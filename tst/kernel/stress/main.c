/**
 * @section License
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014-2017, Erik Moqvist
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * This file is part of the Simba project.
 */

#include "simba.h"

static struct sem_t sem;
static int counter = 0;
static THRD_STACK(worker_0_stack, 1024);
static THRD_STACK(worker_1_stack, 1024);
static THRD_STACK(worker_2_stack, 1024);

static void *worker_main(void *arg_p)
{
    int count;
    struct time_t diff, prev, now;
    const char *name_p;

    name_p = arg_p;
    thrd_set_name(name_p);
    time_get(&prev);

    while (1) {
        sem_take(&sem, NULL);
        counter++;
        count = counter;
        thrd_yield();
        sem_give(&sem, 1);

        time_get(&now);
        time_subtract(&diff, &now, &prev);

        if (diff.seconds >= 1) {
            prev = now;
            log_object_print(NULL, LOG_ERROR, OSTR("Count: %d\r\n"), count);
        }

        thrd_yield();
    }

    return (NULL);
}

static int test_all(struct harness_t *harness_p)
{
    sem_init(&sem, 0, 1);

    thrd_spawn(worker_main,
               "worker_0",
               90,
               worker_0_stack,
               sizeof(worker_0_stack));

    thrd_spawn(worker_main,
               "worker_1",
               90,
               worker_1_stack,
               sizeof(worker_1_stack));

    thrd_spawn(worker_main,
               "worker_2",
               90,
               worker_2_stack,
               sizeof(worker_2_stack));

    thrd_sleep_ms(5500);

    return (0);
}

int main()
{
    struct harness_t harness;
    struct harness_testcase_t harness_testcases[] = {
        { test_all, "test_all" },
        { NULL, NULL }
    };

    sys_start();

    harness_init(&harness);
    harness_run(&harness, harness_testcases);

    return (0);
}

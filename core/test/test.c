/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include "test.h"
#ifdef __APPLE__
#include <execinfo.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#endif
#include <stdio.h>

int mu_tests_run;

#ifdef __APPLE__
static void mu_crash_handler(int signum)
{
    void *frames[64];
    const char prefix[] = "\nlibvmaf test caught fatal signal; backtrace:\n";

    (void)write(STDERR_FILENO, prefix, sizeof(prefix) - 1u);
    int nframes = backtrace(frames, 64);
    backtrace_symbols_fd(frames, nframes, STDERR_FILENO);

    (void)signal(signum, SIG_DFL);
    (void)raise(signum);
}

static void mu_install_crash_handler(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = mu_crash_handler;
    (void)sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    (void)sigaction(SIGABRT, &action, NULL);
    (void)sigaction(SIGBUS, &action, NULL);
    (void)sigaction(SIGSEGV, &action, NULL);
}
#endif

int main(void)
{
#ifdef __APPLE__
    mu_install_crash_handler();
#endif

    char *msg = run_tests();

    if (msg) {
        (void)fprintf(stderr, "\033[31m%s\n%d tests run, 1 failed\033[0m\n", msg, mu_tests_run);
    } else {
        (void)fprintf(stderr, "\033[32m%d tests run, %d passed\033[0m\n", mu_tests_run,
                      mu_tests_run);
    }

    return msg != 0;
}

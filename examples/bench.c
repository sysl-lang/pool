/* The C side of `bench.sysl`: the same three measurements with OpenMP (libomp), so the README's
 * table compares a work-stealing pool against the runtime C programs actually reach for.
 *
 *     clang -O2 -Xpreprocessor -fopenmp -I/opt/homebrew/opt/libomp/include \
 *         -L/opt/homebrew/opt/libomp/lib -lomp examples/bench.c -o target/bench_c
 *     target/bench_c
 *
 * `for` is `omp parallel for schedule(dynamic, 1024)`; `fib` is `omp task` with the same cutoff of 20;
 * `spawn` is one thread creating and waiting for an empty `omp task` a million times.
 */
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define N 4000000

static uint32_t collatz(uint64_t n) {
    uint32_t steps = 0;
    while (n != 1) {
        n = (n % 2 == 0) ? n / 2 : 3 * n + 1;
        steps++;
    }
    return steps;
}

static uint64_t seq_fib(uint64_t n) { return n < 2 ? n : seq_fib(n - 1) + seq_fib(n - 2); }

static uint64_t fib(uint64_t n) {
    if (n < 20) return seq_fib(n);
    uint64_t a, b;
#pragma omp task shared(a)
    a = fib(n - 1);
    b = fib(n - 2);
#pragma omp taskwait
    return a + b;
}

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t best(int64_t a, int64_t b) { return a < b ? a : b; }

int main(void) {
    static uint32_t out[N];
    static const int threads[] = {1, 2, 4, 6, 8, 12, 18};

    printf("threads  for_ms  fib_ms  spawn_ns  check\n");

    for (int t = 0; t < 7; t++) {
        int n = threads[t];
        int64_t tf = INT64_MAX, tb = INT64_MAX, ts = INT64_MAX;
        uint64_t check = 0;

        omp_set_num_threads(n);

        for (int k = 0; k < 3; k++) {
            int64_t t0 = now_us();
#pragma omp parallel for schedule(dynamic, 1024)
            for (long i = 0; i < N; i++) out[i] = collatz((uint64_t)i + 1);
            int64_t t1 = now_us();
            uint64_t sum = 0;
            for (long i = 0; i < N; i++) sum += out[i];
            tf = best(tf, t1 - t0);

            uint64_t f = 0;
            int64_t t2 = now_us();
#pragma omp parallel
#pragma omp single
            f = fib(42);
            tb = best(tb, now_us() - t2);
            check = sum + f;
        }

        if (n == 1) {
            for (int k = 0; k < 3; k++) {
                const long m = 1000000;
                long acc = 0;
                int64_t t0 = now_us();
#pragma omp parallel
#pragma omp single
                for (long i = 0; i < m; i++) {
                    long r = 0;
#pragma omp task shared(r)
                    r = 1;
#pragma omp taskwait
                    acc += r;
                }
                if (acc != m) abort();
                ts = best(ts, (now_us() - t0) * 1000 / m);
            }
        }

        printf("%7d  %6lld  %6lld  %8lld  %llu\n", n, (long long)(tf / 1000), (long long)(tb / 1000),
               (long long)(n == 1 ? ts : 0), (unsigned long long)check);
    }
    return 0;
}

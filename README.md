# pool

A work-stealing scheduler for [sysl](https://github.com/sysl-lang/sysl), in two layers: the
Chase–Lev deque alone, which needs no heap and no operating system, and a thread pool built from it.

```
sh/sysl/pool/
    deque/deque.sysl    the Chase–Lev deque: fixed capacity, atomics only, @no_alloc
    deque/tests.sysl    its semantics, and one owner against six thieves over two million items
    pool.sysl           the pool: workers, stealing, parking, spawn/join, scope, parallel_for, map_reduce
    tests.sysl          every answer against the sequential one, at 1, 2 and 8 workers
examples/
    board/              the deque as a per-core run queue on a board (build-c, no heap)
    not_pow2.sysl       a capacity the deque refuses while compiling
    bench.sysl          the scaling table below
    bench.c             the same work in C with OpenMP
    usage.sysl          the example below
package.hocon           who this package is
```

The modules are **`sh.sysl.pool.deque`** and **`sh.sysl.pool`**. It depends on nothing outside the
standard library.

## Using it

```
dependencies {
  pool { git = "github.com/sysl-lang/pool", version = "0.1.1" }
}
```

```sysl
import sh.sysl.pool.*

fib(n: u64) -> u64
    if n < 20 then return if n < 2 then n else fib(n - 1) + fib(n - 2)

    val h = spawn(() -> fib(n - 1))     // offered to the pool
    val b = fib(n - 2)                  // run here meanwhile

    h.join() + b

val p = Pool.new()                       // one worker per processor
print(p.spawn(() -> fib(30)).join())   // 832040

val squares = p.map_reduce(1..1000usize, 64, u64(0), (i) -> u64(i) * u64(i), (a, b) -> a + b)
```

| | |
|---|---|
| `Pool.new(n = 0) -> &Pool` | `n` workers, or one per processor; the destructor shuts it down |
| `pool.spawn(f) -> Handle[R]` | from outside the pool: onto the injector |
| `spawn(f) -> Handle[R]` | from inside a task: onto this worker's own deque. Outside every pool it runs `f` at once |
| `handle.join() -> R`, `is_done()` | a worker waits by running other tasks; any other thread sleeps |
| `join(a, b) -> (A, B)` | `b` offered to the pool while `a` runs here |
| `pool.scope((s) -> …)` | structured fork-join: `s.spawn((s) -> …)`, and every task has finished before `scope` returns |
| `pool.parallel_for(range, grain, f)` | `f(i)` for every index, split in halves down to `grain` |
| `pool.map_reduce(range, grain, identity, map, reduce)` | combined in index order, so `reduce` need only be associative |
| `pool.shutdown()` | lets queued and running tasks finish, including what they spawn, then joins the workers |
| `worker_index() -> Option[usize]` | which worker the calling thread is |

**A task is a `&sync Fn`**, so the compiler holds every task to the crossing rule capture by capture:
scalars, arrays, `&sync T` and raw pointers cross; a plain `&T`, a `string` and a slice do not. The
answer a task returns crosses back the same way.

**A task that panics stops the program.** A panic in sysl is a trap rather than an unwinding, so
there is nothing for a pool to catch; the process ends with the panic's message, from whichever
worker it was on. The tests pin this with `@test(should_trap: …)`, as they do spawning onto a pool
that has been shut down.

## The deque, and its orderings

`Deque[T, const N: usize]` is D. Chase and Y. Lev, "Dynamic Circular Work-Stealing Deque" (SPAA
2005), with the memory orderings N. M. Lê, A. Pop, A. Cohen and F. Zappa Nardelli prove correct for
C11 in "Correct and Efficient Work-Stealing for Weak Memory Models" (PPoPP 2013), Figure 1. Every
atomic in `deque.sysl` cites the line of that figure it is:

| operation | orderings |
|---|---|
| `push` (owner) | `bottom` relaxed, `top` **acquire**, slot relaxed, **fence(release)**, `bottom` store relaxed |
| `pop` (owner) | `bottom` relaxed, `bottom` store relaxed, **fence(seq_cst)**, `top` relaxed, slot relaxed; on the last item `cas(top)` **seq_cst** |
| `steal` (thief) | `top` **acquire**, **fence(seq_cst)**, `bottom` **acquire**, slot relaxed, `cas(top)` **seq_cst** |

Three things differ from the paper, each said where it is written:

- **The array never grows.** The paper's `resize` allocates; this layer is for machines without a
  heap. The capacity is a value parameter and must be a power of two — `Deque[u8, 3]` is refused
  while compiling, by an `@assert` settled per instantiation. `push` on a full deque answers `false`;
  the pool then runs the task on the spot.
- **The indices are machine words that wrap**, compared by their signed difference. A Cortex-M has
  no 64-bit exclusives, so the paper's 64-bit counters would be calls into a `libatomic` a board
  does not have.
- **`cas` has one ordering**, so its failure ordering is seq_cst rather than relaxed: stronger, and
  the same instructions on both boards.

**What the atomics become** (`sysl build-c examples/board`, alpha.12):

| | `push` | `pop` | `steal` |
|---|---|---|---|
| **AArch64** (`aarch64-freestanding`) | `ldar` top · `dmb ish` · plain `str` ×2 | plain `str` · `dmb ish` · `ldaxr`/`stlxr` on the last item | `ldar` · `dmb ish` · `ldar` · `ldaxr`/`stlxr` |
| **Cortex-M4** (`thumbv7em-freestanding`) | `ldr` · `dmb sy` · `str` · `dmb sy` · `str` | `str` · `dmb sy` · `ldrex`/`strex` + `dmb sy` | `ldr` · `dmb sy` ×2 · `ldr` · `dmb sy` · `ldrex`/`strex` + `dmb sy` |

### Freestanding status

The deque module is `@no_alloc`, uses no OS, and `Deque.new` is `const`, so a module `var` of one is
constant data in the image — what a kernel needs for a per-core run queue with no startup code. It
builds for both boards above **when it is the only module of the package in the build**; built
through this package it does not yet, because the compiler checks the hosted `sh.sysl.pool` module
against the board even though the program never imports it (finding 1 below). The test that builds
it is written and `ignore`d until that is fixed.

## Speed

`examples/bench.sysl` against `examples/bench.c` (clang -O2, OpenMP via Homebrew's libomp), Apple
M-series, 6 performance + 12 efficiency cores, best of three, other work running (load ~4):

| workers | `parallel_for` sysl | OpenMP `for` | fork-join fib(42) sysl | OpenMP tasks |
|---:|---:|---:|---:|---:|
| 1 | 426 ms | 423 ms | 392 ms | 385 ms |
| 2 | 213 | 211 | 197 | 193 |
| 4 | 108 | 107 | 100 | 98 |
| 6 | 75 | 74 | 70 | 68 |
| 8 | 57 | 57 | 55 | 53 |
| 12 | 39 | 39 | 38 | 37 |
| 18 | 30 | 29 | 29 | 30 |

`parallel_for` is 4M indices at grain 1024, each writing a Collatz step count into its own slot;
fib goes sequential below 20. Both scale 14× on 18 cores, level with OpenMP throughout.

**A spawn and its join cost 88 ns** on one worker (OpenMP's task and taskwait: 16 ns). The
difference is allocation: a spawn boxes the closure, the job and the result slot, three counted
heap objects, where libomp reuses task descriptors.

## Sanitizers

- **ASan**: 24 passed, 0 reports (`nm -u` shows `__asan_report_*`).
- **ThreadSanitizer runs** (`SYSL_EXTRA_CFLAGS="-fsanitize=thread -g"`, `__tsan_*` imported), and
  reports 1,519 races over 13 tests, **every one a fence TSan does not model**. Two sources,
  separated by experiment: the deque's publication by `fence(release)` — rewriting `push` with a
  release store instead removes 1,331 of them, but that rewrite is wrong under C++20 (see the comment
  on `push`); and the other 188, none of whose frames is in this package, are sysl's own `&sync`
  release: `atomicrmw sub release`, then `fence acquire` before the free. That is the correct
  pattern, and Rust's `Arc` too — which swaps the fence for an acquire load under TSan (finding 6).

## Where it fits

- **Data-parallel loops**: a ray tracer's rows, a matrix product's blocks (`raytrace`, `linalg`) —
  `parallel_for` and `map_reduce`.
- **Async runtimes**: a multi-threaded executor is a pool whose tasks are woken futures; the deque
  and the parking are the parts it needs.
- **OS per-core run queues** (`keel`): `sh.sysl.pool.deque` alone, one `Deque` per core in a
  `static var`, the core's scheduler popping its own and stealing from the others' when idle.

## Findings in sysl

Found writing this package, for the compiler (alpha.14 queue):

1. **A dependency's modules are all checked against a heap-less target even when the program
   imports none of them.** `examples/board` imports only `sh.sysl.pool.deque`; `sysl build-c
   examples/board --target thumbv7em-freestanding --lib .` (or a `path` dependency) is refused over
   `sh.sysl.pool`'s `&sync` boxes and `@thread_local` — and so is a probe that imports nothing from
   the package at all. `reference/modules.md` § "A library's own modules are exempt": *a program that
   never names the module hears nothing*.
2. **A 64-bit `Atomic` on a 32-bit Cortex-M compiles to `__atomic_load_8` / `__atomic_store_8` /
   `__atomic_compare_exchange_8`**, undefined on a freestanding image. `library/sync.md` says *a
   machine has one for 8, 16, 32 and 64 bits*; ARMv7-M and v6-M have no 64-bit exclusives, so the
   form should refuse it there as it refuses `u12`. Repro: `var a: Atomic[i64] = Atomic(0)` +
   `a.cas(0, 1)` built with `build-c --target thumbv7em-freestanding`.
3. **A module `var` of a struct holding a large array, filled by a call, hangs clang.** The
   initializer is emitted as one first-class aggregate `load` + `store` of the whole struct. Repro:
   `struct S { n: u32, big: [2000000]u8 }`, `mk() -> u32 = 7`, `var s: S = S(mk(), [0; 2000000])`
   in a non-entry module — `sysl build` sits in clang -O1 for minutes (killed at 2:30). The same
   array as its own zero `var` is `zeroinitializer` and instant.
4. **`Mutex.with((q) -> q.take())` is refused as returning the lent pointer** when `take(*self) ->
   *Job` returns a pointer *read out of* the guarded value (`self.head`), not one derived from `q`.
   `reference/memory.md` § "A pointer lent for one call" says a call is followed into the callee;
   this one keeps nothing. Repro: `struct Q { head: *Node; first(*self) -> *Node = self.head }`,
   `m.with((q) -> q.first())`. `pool.sysl` uses `lock_raw` there.
5. **A scope cannot lend what its tasks borrow.** `@lends(body)` on `Pool.scope` refuses
   `s.spawn(…)` ("passed to 'Scope.spawn', which holds on to it") although the scope outlives every
   task; and a `&sync Fn` may not capture a slice of the caller's local (`'v' … reaches a '[]ulong',
   which owns its elements through a count that is not atomic`). So a scoped task reaches the
   enclosing frame only through a raw pointer (`tests.sysl` § scope, `bench.sysl`), which nothing
   checks. Structured fork-join needs a borrow that lives exactly as long as the scope.
6. **TSan cannot see a `&sync` release.** The runtime's `fence acquire` before the free is invisible
   to ThreadSanitizer, so every `&sync` object freed on another thread is a reported race. Rust's
   `Arc` uses an acquire load under `cfg(sanitize = "thread")` for this reason.
7. **A range literal is not typed by the parameter it is passed to**: `total(r: Range[usize])` called
   as `total(0..<4)` is refused (`Range[usize]` vs `Range[int]`) where `f(x: usize)` takes `f(4)`.
   Hence `0..<4096usize` in the tests.
8. **Two allocator diagnostics land in a comment**: the board build above reports "a reference needs
   an allocator" at `pool.sysl:225:53` and "a slice with storage" at `225:58`, a `//` line.
9. **`reference/attributes.md` § "A test may call the compiler" writes `answer.status.is_ok()`**;
   `sysl.process.Status` has `ok()`. The block is not compiled, so nothing caught it.

Already queued for alpha.13 and worked in their shape: a `&sync` object cannot hold a `Buf` or a
slice (so `Pool` is a `&Pool` and tasks reach their pool through a thread-local); atomic retain and
release on every copy of a `&sync` value; `@no_alloc` not enforced on generics.

## Licence

ISC.

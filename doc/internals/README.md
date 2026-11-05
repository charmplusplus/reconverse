# Reconverse internals

This manual explains how reconverse works inside. It has one chapter per subsystem and describes the code as it currently stands. Start here if you are going to work on the runtime itself rather than on a program that uses it. Charm++ has a matching manual for its own layers, in `doc/internals/` of the charm repository.

What goes where:

- **A chapter** describes the current design: the data structures, the protocols between PEs and processes, the invariants, and where in `src/` each piece lives. A reader should be able to open the files a chapter names and recognize what it describes.
- **History** belongs elsewhere. Alternatives that were considered, measurements, and the history of decisions go in pull requests and commit messages. If a chapter and the code disagree, the chapter is wrong or stale: fix it in the same pull request that changes the code.
- **Code comments** stay short and can point to a section of a chapter.
- **User-facing behaviour** (options, APIs, how to build and run) belongs in the top-level `README.md`, not here.
- **How reconverse differs from classic Converse** is covered by [Converse and Reconverse](../converse-and-reconverse.md).

## Chapters

| Chapter | Status |
|---|---|
| [Shared-memory IPC: the `+ipc` pool](shared-memory-ipc.md) | written 2026-11-04 (#319) |
| Start-up and exit: `ConverseInit`, PE threads, `ConverseExit`, interop return mode (`CmiInteropReturnOnExit`, `ConverseInteropFinalize`) | not yet written |
| Schedulers and queues: the registered, randomized and old schedulers, per-PE and node queues, priorities, which thread delivers a network message, and the order guarantees that do and do not hold | not yet written |
| The communication backend: `comm_backend.h`, LCI devices and progress, the registered mempool, LCW/MPI | not yet written |
| Zero-copy and RDMA: `conv-rdma.cpp`, `CmiIssueRget`/`CmiIssueRput`, device memory | not yet written |
| Collectives and broadcasts: spanning trees, node broadcasts, reductions | not yet written |
| Persistent channels | not yet written |
| User-level threads and futures: `threads.cpp`, `uFcontext.h`, `futures.cpp` | not yet written |
| Seed balancing (`cldb*`) and the task queue | not yet written |

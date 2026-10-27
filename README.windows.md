# Reconverse on Windows

Reconverse builds natively on Windows with Microsoft Visual C++ (MSVC). A
Windows build runs **one process**: its PEs are threads in that process, so
`+pe N` gives you N PEs on one machine. There is no communication backend
(LCI and LCW are both off), so a Windows build cannot run across processes or
machines. This build exists mainly so that Charm++, and through it NAMD, can
run on Windows desktops.

This page covers building and testing Reconverse on its own. If you want
Charm++ on Windows, follow `README.windows.md` in Charm++ instead (target
`reconverse-win-x86_64`). That build compiles this repository as its
`contrib/reconverse` submodule, so you do not build Reconverse separately.

## Requirements

- **Visual Studio 2022**, or the Build Tools for Visual Studio 2022, with the
  **Desktop development with C++** workload. This workload provides
  everything the build needs:
  - `cl` and `link`, the compiler and linker
  - `ml64`, the assembler (Boost.Context's context-switch code is MASM)
  - the Windows SDK
  - CMake and Ninja

  Tested with Visual Studio 2022 17.12 (MSVC 19.42) and on GitHub's
  `windows-latest` runner.
- **Git**, for example Git for Windows.

You do not need Cygwin, MPI, libfabric or hwloc.

## Open a build shell

From the Start menu, open **x64 Native Tools Command Prompt for VS 2022**.
That prompt puts the 64-bit compiler, `ml64`, CMake and Ninja on `PATH`.
Don't use the plain "Developer Command Prompt": by default it targets 32-bit
x86, and Reconverse supports only x64 on Windows.

Check that the right compiler is on `PATH`:

```bat
where cl
```

The path should end in `\Hostx64\x64\cl.exe`.

## Build

```bat
git clone https://github.com/charmplusplus/reconverse.git
cd reconverse
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DRECONVERSE_TRY_ENABLE_COMM_LCI2=OFF ^
      -DRECONVERSE_TRY_ENABLE_COMM_LCW=OFF
cmake --build build
```

Until charmplusplus/reconverse#304 is merged, check out its branch after
cloning: `git checkout windows-support`.

Notes:

- **Turn both backends off.** They are on by default, and neither builds on
  Windows. CI configures exactly as shown above.
- **Build type.** Use `-DCMAKE_BUILD_TYPE=Debug` for an unoptimized build. CI
  builds Debug.
- **Generator.** Use Ninja, which is what CI uses. The Visual Studio
  generators have not been tested.
- **hwloc warning.** Configure warns that hwloc was not found and CPU affinity
  is disabled. This is expected on Windows (see [Limitations](#limitations)).
- **Library type.** Reconverse is built as a static library. The build also
  produces one executable per test under `build\tests\`.

## Run

No launcher is involved: start the executable directly.

```bat
build\tests\ring\reconverse_ring.exe +pe 4
```

Each test's executable is `build\tests\<name>\reconverse_<name>.exe`. The
runtime options in [README.md](README.md#runtime-options) apply, except those
that need more than one process or a communication backend: `+backend`, and
the `+ipc` family.

## Test

Some tests need more than one process. They run through the multi-process
launcher (`mpirun` by default), which a Windows build has no use for, so
leave them out. CI also leaves out the CCS tests: their server opens a TCP
listening port, and on a desktop Windows Defender Firewall then asks for
administrator approval.

In PowerShell (run `powershell` inside the x64 Native Tools prompt), this
runs every remaining test. It is the same filter CI uses:

```powershell
$tests = (ctest --test-dir build --show-only=json-v1 | ConvertFrom-Json).tests
$skip  = @($tests | Where-Object { -not $_.command -or ($_.command -contains 'mpirun') -or
                                   $_.name -like 'ccs*' } | ForEach-Object { $_.name })
$exclude = '^(' + (($skip | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'
ctest --test-dir build --output-on-failure --timeout 120 -E $exclude
```

When the PR was opened, 67 tests ran and passed, and 54 were skipped.

One test, `reduction_many-overflow`, runs through `sh`. GitHub's runner has
Git for Windows' `sh` on `PATH`, but a default Git for Windows install puts
only `Git\cmd` there, so on a desktop the filter usually skips that test too
(66 run, 55 skipped). To run it, add `C:\Program Files\Git\bin` to `PATH`.

## Use Reconverse from your own CMake project

Link against the `reconverse::reconverse` target, as
`tests/standalone_project` does. Set the backend options before Reconverse is
added:

```cmake
include(FetchContent)
FetchContent_Declare(reconverse
  GIT_REPOSITORY https://github.com/charmplusplus/reconverse.git
  GIT_TAG main)
set(RECONVERSE_TRY_ENABLE_COMM_LCI2 OFF CACHE BOOL "")
set(RECONVERSE_TRY_ENABLE_COMM_LCW  OFF CACHE BOOL "")
FetchContent_MakeAvailable(reconverse)   # or add_subdirectory(<checkout>)
target_link_libraries(myprog PRIVATE reconverse::reconverse)
```

Linking against `reconverse` adds two linker options to your executable:

- `/STACK:8388608` gives the main thread an 8 MB stack. PE 0 runs on the main
  thread, and Windows would otherwise give it 1 MB.
- `/DEBUG` writes a `.pdb` next to the `.exe`.

`/DEBUG` does not change the generated code. In non-Debug builds, `/OPT:REF`
and `/OPT:ICF` are added back, because `/DEBUG` would otherwise turn them off.

## What behaves differently on Windows

- **Threads and stacks.** PE threads are created with `_beginthreadex`, with
  8 MB of stack reserved for each. Pages are committed only as the stack
  grows.
- **Timers and locks.** The wall clock uses `GetSystemTimePreciseAsFileTime`,
  which has about 100 ns resolution. Locks are SRW locks, which, like a
  default pthread mutex, are not recursive.
- **Stack traces.** `CmiAbort` and fatal faults print a stack trace. With the
  `.pdb` present, frames show function names; code compiled with debug
  information (`Debug` or `RelWithDebInfo`) also shows file:line. Without the
  `.pdb`, the program's own frames show only module+offset. Keep the `.pdb`
  next to the `.exe` if you want readable traces.
- **Crashes.**
  - A fatal fault (access violation and so on) is reported with the POSIX
    signal it corresponds to, for example `SIGSEGV`, and the process exits
    with the exception code.
  - The Windows Error Reporting and `abort()` dialogs are suppressed, so a
    crashed run exits instead of waiting for someone to close a window.
  - Pass `+truecrash` to leave Windows' default handling in place, for
    example to attach the Visual Studio debugger when the program crashes.
- **Memory usage.** `CmiMemoryUsage()` returns the process's committed
  private bytes, which is what classic Converse reported on Windows.

## Limitations

- **One process only.** There is no LCI or LCW on Windows, so no
  multi-process or multi-machine runs.
- **No shared-memory IPC pool.** `+ipc` has no effect, because the job is
  always a single process. `CmiMakeIpcManager` aborts with a message saying
  so, which is what Charm++ built with `--enable-shmem` calls.
- **No CPU affinity.**
  - hwloc is not used on Windows.
  - `+setcpuaffinity`, `+pemap` and `+showcpuaffinity` are accepted but do
    nothing.
  - `CmiSetCPUAffinity` returns -1, meaning not supported.
- **CCS.** CCS (`++server`) is left out of Windows test runs, because of the
  firewall prompt described under [Test](#test). Its `ccs` test passed the
  one time it was run by hand.
- **Compiler.** Only MSVC targeting x64 is supported. clang-cl has not been
  tested; the MSVC-specific code paths are keyed to `cl`, not clang-cl.
- **Toolchain.** MinGW and MSYS2 are not supported.

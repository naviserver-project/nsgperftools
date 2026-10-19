# nsgperftools

Heap profiling and heap sampling for [NaviServer](https://github.com/naviserver-project/naviserver) with [gperftools](https://github.com/gperftools/gperftools)' tcmalloc.

The module adds one Tcl command, `ns_heapprofile`. It drives the gperftools heap profiler and tcmalloc's heap sampler from inside a running server, so you can see which code holds memory.

**Linux only.** The server process must contain gperftools' *full* tcmalloc (`libtcmalloc.so`, not `libtcmalloc_minimal.so`), normally loaded with `LD_PRELOAD`. Tested with NaviServer 5.2 on Tcl 9.1 and gperftools 2.4 (Ubuntu 16.04).

## Why a module

- **`HEAPPROFILE` breaks the server.** The usual way to heap-profile a program is `HEAPPROFILE=/tmp/x` in its environment. Every program the server runs inherits that variable and prints "Starting tracking the heap" to stderr, and Tcl's `exec` treats that as an error. On one production server this stopped the server from starting. `ns_heapprofile start` starts the profiler inside nsd only.
- **Sampling can stay on in production.** tcmalloc's heap sample costs almost nothing, so it can run for hours on a busy server, where the full profiler cannot.

This module found an unbounded leak of MySQL result sets in production ([nsdbmysql#3](https://github.com/naviserver-project/nsdbmysql/issues/3)).

## Build and install

```sh
make NAVISERVER=/usr/local/ns
make NAVISERVER=/usr/local/ns install
```

`NAVISERVER` is the NaviServer installation the module will run in. The build needs a C and a C++ compiler, because `heapsample.cpp` calls a C++-only gperftools method. It does not need the gperftools headers or libraries: the functions are looked up at run time.

## Configuration

### Launch environment

gperftools reads its settings from the environment once, when the library is loaded. Set them in the server's launch environment, i.e. its start script or systemd unit:

```sh
export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc.so.4   # the path varies by distribution
export HEAP_PROFILE_ALLOCATION_INTERVAL=0
export HEAP_PROFILE_DEALLOCATION_INTERVAL=0
export HEAP_PROFILE_INUSE_INTERVAL=0
export HEAP_PROFILE_TIME_INTERVAL=0
export TCMALLOC_SAMPLE_PARAMETER=524288     # optional: heap sampling
```

- **The four intervals** control automatic profile dumps. 0 turns a trigger off; a large value only postpones it. By default gperftools dumps every 1 GB allocated and every 100 MB of growth, and each dump stalls every allocation while it is written. `ns_heapprofile start` refuses to start unless all four are 0, unless you add `-force`.
- **Leave `HEAPPROFILE` unset.** Without it, the interval variables do nothing in the programs the server runs.
- **`TCMALLOC_SAMPLE_PARAMETER`** is the average number of bytes allocated between samples. 0, the default, means no sampling; 524288 is the usual value.

### Server configuration

```tcl
ns_section ns/server/$server/modules {
    ns_param nsgperftools nsgperftools.so
}
```

You can also load the module into one interpreter of a running server, without a restart:

```tcl
load /usr/local/ns/bin/nsgperftools.so Nsgperftools
```

Either way, the profiler and the sampler are process-wide.

## Commands

| Command | Description |
| --- | --- |
| `ns_heapprofile start ?-force? ?prefix?` | Start the heap profiler. Dumps go to `<prefix>.NNNN.heap`; the default prefix is `/tmp/nsgperftools`. Refuses unless all four intervals are 0, unless `-force` is given. |
| `ns_heapprofile dump ?reason?` | Write the next dump file. |
| `ns_heapprofile get` | Return the current heap profile as text. |
| `ns_heapprofile stop` | Stop the heap profiler. |
| `ns_heapprofile running` | 1 if the heap profiler is running, else 0. |
| `ns_heapprofile sampling ?-force? ?bytes?` | Return tcmalloc's heap-sampling period (0 = off). With `-force` and `bytes`, change it at run time; see [Caveats](#caveats-and-limitations). |
| `ns_heapprofile heapsample` | Return the sampled live heap in pprof format, including the address map. |
| `ns_heapprofile stats` | Return tcmalloc's statistics text (`MallocExtension_GetStats`). |
| `ns_heapprofile property name` | Return a tcmalloc numeric property, e.g. `generic.current_allocated_bytes`, `generic.heap_size` or `tcmalloc.pageheap_free_bytes`. |

## Usage

### Heap sampling (the one to use in production)

With `TCMALLOC_SAMPLE_PARAMETER` set, save a sample now and another one later, then compare them:

```tcl
set fh [open /tmp/s1.heap w]; puts -nonewline $fh [ns_heapprofile heapsample]; close $fh
# ... an hour later, the same to /tmp/s2.heap
```

```sh
pprof --text --base=/tmp/s1.heap /usr/local/ns/bin/nsd /tmp/s2.heap | head -30
```

A sample covers only allocations made while sampling was on. Memory allocated before then and freed later never shows as a decrease. So take the first sample after the server has warmed up.

Measured on a server with about 200 MB/s of allocations across 30 threads: sampling at 524288 did not change request latency, and a sample took about 0.5 ms.

### Full heap profile

```tcl
ns_heapprofile start /tmp/nsd
# ... let the server run ...
ns_heapprofile dump growth       ;# writes /tmp/nsd.0001.heap
ns_heapprofile stop
```

```sh
pprof --text /usr/local/ns/bin/nsd /tmp/nsd.0001.heap | head -30
pprof --text --base=/tmp/nsd.0001.heap /usr/local/ns/bin/nsd /tmp/nsd.0002.heap | head -30
```

The profiler records every allocation behind a single lock. That is fine on a test server, but on a busy one it raises latency a lot: on the server above, p50 went from 12 ms to 416 ms within a minute.

`pprof` is gperftools' analysis tool. Debian and Ubuntu install it as `google-pprof`, and there `/usr/bin/pprof` is an unrelated program. Stacks resolve only for binaries with symbols.

## Caveats and limitations

- **Linux only**, and only with gperftools' full tcmalloc in the process. Without it the module still loads, logs a warning, and the profiler subcommands return an error.
- **`heapsample` and the C++ string ABI.** It calls `MallocExtension::GetHeapSample(std::string*)` by its C++ symbol name, which depends on the C++ library's string ABI. Build the module with the same ABI as the installed gperftools; the distribution's default compiler does that.
- **`sampling -force` writes an internal setting.** It changes gperftools' variable behind `TCMALLOC_SAMPLE_PARAMETER`, which gperftools does not support doing after startup. With gperftools 2.4 the new value takes effect at once; other versions are untested. Prefer the launch environment.
- **gperftools 2.4 writes at most 1 MB per profile** (`dump` and `get`). A larger profile loses its end, including the `MAPPED_LIBRARIES` address map that pprof needs for shared-library symbols. The workaround for those versions is to append the server's `/proc/<pid>/maps` after a `MAPPED_LIBRARIES:` line. Current gperftools versions stream the profile, so neither the limit nor the workaround applies. `heapsample` output has no such limit and always includes the address map.

## License

[MPL 2.0](LICENSE).

<<<<<<< HEAD
# Substrate VM G1 Garbage Collector Sources

This branch is based on the LabsJDK CE tag `jvmci-25.2-b20` and contains the
Substrate VM integration of the G1 garbage collector.

## Maintenance Model

Most work in this repository is updating the reduced OpenJDK code base to a newer JDK version.
These updates are expensive because the downstream G1 sources must stay aligned with a large amount of upstream OpenJDK code.
For that reason, keep the diff against OpenJDK as small as possible.

## Limitations

- The supported platforms are Linux/AMD64, Linux/AArch64, Darwin/AArch64, and Windows/AMD64.
- Exactly one isolate is supported.
  It is not possible to spawn multiple isolates.
- Images with disabled multithreading (`-H:-MultiThreaded`) are not supported.
- Images without a dedicated reference handler thread (`-H:-UseReferenceHandlerThread`) are not supported.
- Images without a dedicated VM operation thread (`-H:-UseDedicatedVMOperationThread`) are not supported.

## Building

Use `mx build` to build this repository.
This creates include files and static libraries that Native Image uses when building an image with G1 GC (`-H:+UseG1GC`).
The static library used by native-image depends on whether compressed references are enabled:

- `libg1gc-cr.a` or `g1gc-cr.lib`: used if references are compressed
- `libg1gc-ur.a` or `g1gc-ur.lib`: used if references are uncompressed

## Repository Structure

The repository structure mostly matches OpenJDK.
The main differences are:

- Files that are fully Substrate VM-specific are in `src/hotspot/svm`.
- SVM-specific changes in shared files are guarded with `#ifdef SVM` or `#ifndef SVM`.
- An mx/Ninja native build replaces the OpenJDK build system.

## Development and Debugging

### Building a Debug Version

By default, `mx build` builds only the optimized static libraries.
These libraries do not contain debug information.
For G1 development or debugging, build G1 as a shared library with debug information.
The native image can then link that shared library dynamically, so C++ changes only require rebuilding G1, not the native image.

The G1 GC can be built with one of the following debug levels:

- `debug`: easy to debug, but not optimized and slow. Runs assertions and self-verification code.
- `fastdebug`: partly optimized and much faster than `debug`. Also runs assertions and self-verification code.
- `product`: fully optimized, without debug information. This is the version shipped with GraalVM.

To build a custom G1 variant, set the environment variable `SVM_GC_TARGETS`.
Generated Ninja targets use this pattern: `build_[debug|fastdebug|product]_[ur|cr]_[a|so]`, e.g.:

```shell
SVM_GC_TARGETS='build_debug_cr_so build_debug_ur_so' mx build
```

Custom builds archive only the selected static libraries. Shared libraries remain local build
outputs and are not added to the mx distributions.

The shortcut `build_all` builds both shared and static libraries for all debug levels.

After building a debug version of G1, point `-H:CLibraryPath=...` to the directory
that contains the G1 library and set
`-H:G1DebugLevel=[debug|fastdebug|product]` to match that library.

## Debugging

Debug builds already enable many assertions and self-verification checks.
The following runtime options are also useful:

- `-XX:+PrintGC`: prints some information about garbage collections
- `-XX:+VerboseGC`: prints detailed information about garbage collections
- `-H:+VerifyHeap`: verifies the heap before and after a garbage collection.
  This can be very helpful, but is also rather slow.

Helpful debugging hints:

- `gc_create(...)` in `svmToGC.cpp` is the main entry point.
  It runs when the native image initializes the garbage collector during startup.
- A breakpoint in `VMError::report_and_die(...)` is often useful.
  Most fatal errors and assertion failures on the C++ side end there.

## Major Differences to the OpenJDK Sources

Our G1 version is built from a subset of the OpenJDK sources (230k committed lines, of which 172k are actually reachable).
This includes the G1 sources, common GC infrastructure, and other OpenJDK code that G1 needs, such as OS support, threading support, and common data structures.

The list below shows the main differences from OpenJDK.
In general, we keep the diff against OpenJDK small because large diffs make JDK updates harder.

- During startup and shutdown, our G1 version runs only a small part of the code that OpenJDK usually runs.
- The address space for the image heap and the Java heap is reserved on the Native Image side.
- In our G1 version, we use additional region types to model the image heap: "closed image heap", "open image heap", and matching humongous region types for large objects.
  - Image heap regions are always alive.
  - Image heap regions are never marked.
  - Image heap regions are never compacted.
  - Image heap regions are never scrubbed.
  - Image heap regions have an empty remembered set and state "untracked" (because image heap objects are always alive).
  - Image heap regions are not part of any region set.
  - Image heap regions are always fully parsable, i.e., `pb` (parsable-bottom) is always the same as `hr->bottom()`.
  - Closed image heap regions are neither visited nor modified by the GC.
  - Some image heap regions don't need write-barriers, so they don't have a block offset table.
    - Closed image heap regions.
    - Humongous open image heap regions that contain primitive arrays.
      This assumes that primitive arrays do not contain references.
  - Open image heap regions are scanned for references (similar to the old generation).
    So, we need to set `TARS` (top-at-rebuild-start) accordingly.
- Our G1 version supports pinning any Java object.
  OpenJDK only supports pinning primitive arrays.
  Regions with pinned objects are never compacted.
  Pinned objects are not evacuated and are handled like evacuation failures.
  Primitive arrays in pinned regions are never overwritten by G1, even if they are unreachable.
  For Java code, it is therefore not necessary to keep a strong reference to primitive arrays that are pinned.
  This also means that primitive arrays must never have references to other objects, not even to a Java monitor.
  - G1 uses a thread-local cache for object pinning, so note that the number of pinned objects is only consistent at a safepoint.
- All `oop` and `Klass` related C++ classes are implemented differently because they must access data in a way that works for Native Image.
  There are also a few conceptual differences:
  - In Native Image, we always use a heap base.
  - If compressed references are used, then the mark word always has 32-bit.
  - Header-size computation is different because Native Image can configure the header size.
- Argument parsing and options (i.e., globals) are handled completely differently in our G1 version as it is necessary to distinguish between hosted and runtime options.
  OpenJDK options that are not relevant for Native Image become constants when the C++ sources are compiled.
  Only options declared with `ni_hosted` or `ni_runtime` are materialized in the SVM option table and can be parsed.
- Stack handling is different because G1 must call into Native Image to get stack information.
- Thread handling is different because Native Image owns all Java threads.
  G1 may only spawn internal worker threads (i.e., non-Java threads).
- OpenJDK uses `dlsym()` to support libc versions where some Linux and POSIX functions are unavailable.
  This does not work when the G1 library is linked into a fully statically linked Native Image (`--static`) because `dlsym()` always returns null in that case.
  So, we modify the sources in a way so that relevant libc functions are referenced directly.
  The Native Image link step resolves these references from `libc.a` for a fully static image or from `libc.so` for dynamically linked musl.
- The handling and lifecycle of JIT compiled code (`nmethods`) is completely different.
- VM operations and safepoints are implemented differently.
  This is mainly needed because the VM operation thread is a normal Java thread in Native Image.
- Performance data support is different because Native Image manages all performance-data memory and data structures.
- Our G1 version does not have any metaspace or class (un)loading support.
- There are many locking differences, e.g.:
  - Only a subset of the OpenJDK locks is used.
  - Far more locks must be lockable by Java threads.
    One of the reasons for this is that the VM operation thread is a Java thread in Native Image.
- We removed support for throwing and handling Java exceptions.
  For example, if the Java heap runs out of memory, OpenJDK would normally throw an `OutOfMemoryError`.
  In our G1 version, Native Image throws the `OutOfMemoryError` instead.
- Native Image supports hybrid objects.
  These are variable-sized objects with both instance fields and primitive array elements.
  OpenJDK does not have this concept.
  Because of that, methods like `oop::is_array()`, `oop::is_objArray()`, `oop::is_typeArray()`, `Klass::is_array_klass()`, `Klass::is_objArray_klass()`, and `Klass::is_typeArray_klass()` can be dangerous because they return false for hybrid objects.
- We added SW-CFI support.
  As of now, only the assembly code needs adjustments:
  - Using the SW-CFI pattern in front of indirect calls/jumps (no occurrences as of now) and `jmp __x86_return_thunk` instead of `ret`.
  - Using `endbr64` in indirectly called functions (no occurrences as of now, look for function address taking in `sed 's/DECLARE_FUNC(\(.*\)):/-e\1[^(]/;t;d' src/hotspot/os_cpu/linux_x86/linux_x86_64.S | xargs git grep`).

## License

This code is licensed under the GNU General Public License version 2. Some
files are subject to the Classpath exception, as stated in their headers.
=======
# Overview

This branch was originally based on the tag `25+37-jvmci-b04` of this repository and contains infrastructure that simplifies the integration of HotSpot garbage collectors into Native Image. The Shenandoah sources have meanwhile been updated on the HotSpot version on the tag `jdk-25.0.4+6`.

## Limitations

The current implementation has a few limitations:

- Only `linux/amd64` and `linux/aarch64` are supported at the moment.
  macOS and Windows support should be easy to add but the build system needs to be changed for that.
- Only a **single** Native Image isolate is supported at the moment because the C++ code has process-wide global state.

## Structure

The directory structure is modeled closely after the OpenJDK, with a few notable differences:

```
src/
└── hotspot/
    ├── cpu/
    ├── os/
    ├── os_cpu/
    ├── share/
    ├── jfrfiles/    # JFR-related files (autogenerated)
    ├── jvmtifiles/  # JVMTI-related files
    ├── mocks/       # Autogenerated mocks for C/C++ header (see below)
    └── svm/         # SVM-specific files (see below)
```

- **mocks/**: Contains autogenerated mocks for all deleted C/C++ headers.
  This ensures that `#include` directives remain valid, which reduces the need for conditional compilation (`#ifdefs`) around header inclusion.
- **svm/**: Contains files that either entirely new or significantly different from their OpenJDK counterparts.
  The structure of the subdirectories mirrors the OpenJDK's `hotspot` directory.
  - *Example*: `src/hotspot/svm/share/oops/oop.hpp` corresponds to `src/hotspot/share/oops/oop.hpp` in OpenJDK.

## Differences to OpenJDK

All files that were not strictly required were removed from the code base.
This includes major components such as class loading, metaspace handling, JIT compilation, code generation, garbage collection, and other VM-internal infrastructure.

Outside the `svm/` directory, SVM-specific changes are typically guarded with directives such as `#ifdef SVM`.
In general, we try to keep the diff against OpenJDK minimal so that updating to new OpenJDK versions is reasonably easy.

In addition, there are several large conceptual differences between Native Image and HotSpot, such as:

- **Heap Management:** Native Image always uses a heap base.
  So, `UseCompressedOops` is always `true`, regardless of the actual reference size.
- **Argument & Option Handling:** Argument parsing and option handling are different as Native Image needs to distinguish between hosted and runtime options.
  HotSpot options that are not relevant for Native Image are reduced to constants when the C++ sources are compiled.
- **Stack Handling:** The C++ code does not understand the Native Image stack layout.
  Therefore, the GC must call into Native Image to retrieve stack information.
- **Thread Management:** All Java threads are managed by Native Image.
  The GC is restricted to spawning only native (non-Java) worker threads.
- **Metaspace & Class Loading:** Metaspace handling or class (un)loading are not supported at the moment.
  Project Crema recently added a metaspace to Native Image, but this C++ GC infrastructure is not yet aware of that.
- **VM Operations & Safepoints:** The VM operation thread in Native Image is a Java thread.
  So, the handling of VM operations and safepoints is implemented completely differently than on HotSpot.
- **Locking:** Only a subset of the HotSpot locks is used.
  However, more locks must be accessible to Java threads because the VM operation thread is a Java thread.
- **Exception Handling:** Java exception support was entirely removed from the C++ code.
  For example, when the Java heap runs out of memory, the `OutOfMemoryError` must be thrown on the Native Image side (and not by the C++ code).
- **JIT-Compiled Code:** The lifecycle and handling of JIT compiled code (`nmethods`) are fundamentally different between Native Image and HotSpot.
- **Hybrid Objects:** Native Image supports hybrid objects (with both instance fields and primitive array elements).
  Methods like `oop::is_array()`, `oop::is_objArray()`, `oop::is_typeArray()`, `Klass::is_array_klass()`, `Klass::is_objArray_klass()`, and `Klass::is_typeArray_klass()` may return `false` for hybrid objects, so caution is required.
- **Performance Data:** Code related to `UsePerfData` is different as Native Image is responsible for managing all related memory and data structures (the C++ may only update data in already existing data structures).

## Current Status & Placeholders

In some code parts, `Unimplemented()` serves as a placeholder for GC-specific functionality:

- Core Functionality
  - `src/hotspot/svm/svmImageHeap.hpp`
  - `src/hotspot/svm/svmToGC.cpp`
  - `src/hotspot/share/gc/shared/gcConfig.cpp`
- Only relevant if `UsePerfData` is enabled in Native Image
  - `src/hotspot/share/runtime/cpuTimeCounters.cpp`
  - `src/hotspot/share/gc/shared/gcPolicyCounters.cpp`
  - `src/hotspot/share/gc/shared/collectorCounters.cpp`
  - `src/hotspot/share/gc/shared/collectedHeap.cpp`
  - `src/hotspot/share/gc/shared/generationCounters.cpp`
  - `src/hotspot/share/gc/shared/hSpaceCounters.cpp`
  - `src/hotspot/share/gc/shared/ageTable.cpp`

Besides that, there are a few places where values are hardcoded at the moment.
Those need to be replaced with the correct GC-specific values:
- the length of `GCThreadLocalData` in `src/hotspot/share/gc/shared/gcThreadLocalData.hpp` is hardcoded to 0
- `log_of_heap_region_grain_bytes` in `src/hotspot/svm/svmToGC.cpp` is hardcoded to 20

## Build

This branch uses a simple [Makefile](src/hotspot/Makefile) instead of the OpenJDK build system.
The following build configurations are supported:

- **Output formats**: Static (`.a`) or shared (`.so`) libraries
- **Debug level**: `debug`, `fastdebug`, `notproduct`, `product`
- **Reference size**: Compressed references (`cr`) or uncompressed references (`ur`)

- **`BUILD_ROOT`**: Set alternative output root (defaults to `./`)
- **`QUIETLY=`**: To get verbose build output
- **`VSCODE=yes`**: To generate a VS Code project file (with ccls as indexer) in the output directory.

**To build all configurations:**
```shell
cd src/hotspot
make -j16 build_all
```

**To build a shared library for local debugging:**
```shell
cd src/hotspot
make -j16 build_debug_ur_so
```

## Run

In order to run Native Image with Shenandoah, you need to use the [simonis/GR-70306](https://github.com/simonis/graal/tree/simonis/GR-70306) Graal branch (which is based on the Graal PR [[GR-70306] Add infrastructure for Shenandoah](https://github.com/oracle/graal/pull/12365)) and do the following:

- Clone [https://github.com/simonis/graal](https://github.com/simonis/graal).
- Change into the `graal/` directory and checkout the [simonis/GR-70306](https://github.com/simonis/graal/tree/simonis/GR-70306) branch.
- Set `JAVA_HOME` to a compatible JDK ([labs-openjdk](https://github.com/graalvm/labs-openjdk) at tag [`25+37-jvmci-b06`](https://github.com/graalvm/labs-openjdk/tree/25%2B37-jvmci-b06) is known to work)
- Make sure to have a version of `mx` on the `PATH` which corresponds to the version specified by the `mx_version` key in the `graal/common.json` file.
- Build the project with: `mx --primary-suite=substratevm --components=ni,nju build` (the `nju` (native unit tests) component is only required if you want to run the native unit tests).
  This will create a complete Native Image distribution under `./sdk/latest_graalvm_home` with the `native-image`
  executable under `./sdk/latest_graalvm_home/bin/native-image`.
- Build a native executable for a simple `HelloWorld` program (with `BUILD_ROOT` from the previous [build](#build) step which built `libshenandoah.so`):
  ```shell
  ./sdk/latest_graalvm_home/bin/native-image \
    -esa -g -O0 -H:+SourceLevelDebug -H:-DeleteLocalSymbols -H:+IncludeDebugHelperMethods \
    --native-compiler-options=-L$BUILD_ROOT \
    --gc=shenandoah -H:ShenandoahDebugLevel=product \
    -o HelloWorld.exe HelloWorld
  ```
- Run the native executable with: ` LD_LIBRARY_PATH=$BUILD_ROOT ./HelloWorld.exe`
- It should run fine without any unexpected exceptions or crashes. If you detect any problems, please report :)

`-H:ShenandoahDebugLevel=` can be one of `product`, `debug` or `fastdebug` and will search for the corresponding version of Shenandoah (i.e. `libshenandoahgc-ur.so`, `libshenandoahgc-debug-ur.so` or `libshenandoahgc-fastdebug-ur.so`) in `$BUILD_ROOT`.

## Status

As of September 2026, Shenandoah is known to correctly execute in all `ShenandoahGCMode` modes (i.e. `passive`, `satb` and `generational` with `satb` being the default) the [Renaissance Benchmark Suite](https://github.com/simonis/RenaissanceNI) and SpecJBB. If you encounter eny issues, please report here. Concurrent Shenandoah (i.e. `satb` and `generational`) are currently only implemented on x86_64.

We are working on supporting concurrent Shenandoah on aarch64 as well.

Notice that with the latest changes, `ShenandoahGCMode` has been converted into a build time (i.e. 'hosted' option). You can only set it at native image build time with `-H:ShenandoahGCMode=<passive|satb|generational>`. If not set explicitely, it defaults to `satb`.

## Executing the Native JUnit tests

- The Native Image JUnit tests can be run by executing: `mx --primary-suite=substratevm native-unittest`
  This will run with SubstrateVM's default Serial GC.
- The Native Image JUnit tests with the new Shenandoah GC from the `labs-openjdk/` repository and the `BUILD_ROOT` from the previous build step can be run by executing (make sure that `BUILD_ROOT`, i.e. the location of libshenandoah.so` is an absolute directory path!):
  ```
  LD_LIBRARY_PATH=$BUILD_ROOT \
  NATIVE_IMAGE_OPTIONS="-H:-TraceVMOperations -R:-UsePerfData -H:+UseShenandoahGC
  mx --primary-suite=substratevm native-unittest
  ```
- With Shenandoah, 9 of the 189 native unit tests are known to fail:
  ```
  Tests run: 189,  Failures: 9
  ```
  These are the `com.oracle.svm.test.jfr.*` and `com.oracle.svm.test.nmt.*` because this functionality is not yet implemented for Shenandoah.

## Importing the Shenandoah implementation

This branch was originally based on `25+37-jvmci-b04` but the existing Shenandoah sources have meanwhile been upgraded to tag `jdk-25.0.4+6`, so we must be careful when importing missing Shenandoah files and their dependencies (which were removed by [Remove unnecessary files](https://github.com/graalvm/labs-openjdk/commit/35c85302eb6)) at that specific version such that things don't get out of sync. This can be achieved with `git checkout jdk-25.0.4+6 -- <file>`, e.g.:

```shell
git checkout jdk-25.0.4+6 -- src/hotspot/share/gc/shenandoah/shenandoahHeapRegion.hpp
```
>>>>>>> simonis/GR-70066

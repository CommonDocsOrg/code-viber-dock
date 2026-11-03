# QuickJS-ng (vendored)

- Version: 0.18.0 (`QJS_VERSION_MAJOR/MINOR/PATCH` in `quickjs.h`)
- Source: https://github.com/quickjs-ng/quickjs/releases (tag `v0.18.0`, source tarball
  `quickjs-ng-v0.18.0.tar.gz`, SHA-256
  `28bcd89aa3683fd2a946537e5ce3f11cc46b5cbf3383c7561aaef3df5ff9f1dd`)
- License: MIT (`LICENSE`, unchanged)
- Local modifications: none. Files are copied byte for byte from the tarball.

Only what the engine needs is kept: the four translation units `quickjs.c`, `dtoa.c`,
`libregexp.c`, `libunicode.c` and the headers they include (`quickjs.h`, `quickjs-atom.h`,
`quickjs-opcode.h`, `quickjs-c-atomics.h`, `cutils.h`, `list.h`, `dtoa.h`, `libregexp.h`,
`libregexp-opcode.h`, `libunicode.h`, `libunicode-table.h`, `builtin-array-fromasync.h`,
`builtin-iterator-zip.h`, `builtin-iterator-zip-keyed.h`). Not vendored: `quickjs-libc.c` (the
std / os modules: the dock provides its own host API), the CLI (`qjs.c`, `qjsc.c`), tests,
test262, docs, examples, fuzzers, the generators and upstream's build files.

Build: WinDock's `CMakeLists.txt` compiles these as its own static library `quickjs` (C11 with
`/experimental:c11atomics`, warnings off with `/W0`, upstream's defines `QUICKJS_NG_BUILD`,
`WIN32_LEAN_AND_MEAN`, `_GNU_SOURCE`, `_WIN32_WINNT=0x0601`), with the same static runtime
library (`/MT`) as the dock. Upstream's CMake is deliberately not used: its
`cmake_minimum_required(3.10)` leaves policy CMP0091 off, so the runtime library would silently
stay `/MD` and fail to link with the dock.

To update: download the new release tarball, copy the same files over, check the include list
(`grep '#include "' quickjs.c dtoa.c libregexp.c libunicode.c`), update this file.

Utilities overview (src/include/util)

This directory holds small, focused utility headers used across unroot. They are header-only and kept minimal to avoid dependencies and reduce coupling.

- app_exit.hpp
  Normalized application exit codes (app::Exit) plus helpers to convert to/from ints.

- error_map.hpp
  Map library/util errors to normalized app exits.

- errors.hpp
  Library error taxonomy (util::Error) for non-throwing flows and CLI boundary.

- fd.hpp
  UniqueFd — tiny RAII wrapper for POSIX file descriptors.

- io.hpp
  Robust I/O helpers: write_all, read_n, read_exact.

- log.hpp
  Pluggable logging sink interface used by library code (decouples from stderr).

- log_host.hpp
  Host-side step logging helpers for CLI/logging contexts; formats text or JSON based on UNROOT_VERBOSE/UNROOT_LOG.

- path.hpp
  Small path/process helpers: pathExists, isExecutableFile, findOnPath, resolveSudoPath (honors UNROOT_SUDO).

- proc.hpp
  Direct fork/exec/wait helper for privileged ID-map utilities.

- result.hpp
  Tiny Result<T,E> type for non-exception error propagation.

- str.hpp
  String helpers shared across TUs: jsonEscape, q (quote), joinArgs.

Notes
- Prefer these helpers over ad-hoc reimplementations in .cpp files.
- Keep headers self-contained and dependency-light. If adding new helpers, document them here.

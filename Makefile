## Top-level Makefile
# Goals:
# - Default `make` builds the new C++ code in src/ into bin/unroot
# - `make legacy` builds the legacy C implementation into bin/unroot-legacy
# - `make nim` retains the Nim demo build
# - `make test` builds and runs the doctest-based C++ tests (no Catch2 dependency)

# Compilers
CC      ?= gcc
CXX     ?= g++
PYTHON  ?= python3
PYTEST  ?= $(PYTHON) -m pytest

# --- Auto-detect musl vs glibc -------------------------------------------------
# We no longer bootstrap a toolchain; we rely on whatever $(CXX) points to.
# Detection heuristic: compiler triple contains "musl".
CXX_TARGET := $(shell $(CXX) -dumpmachine 2>/dev/null)
DETECTED_MUSL := $(findstring musl,$(CXX_TARGET))
MUSL ?= $(if $(DETECTED_MUSL),1,0)

# Allow user override of size tuning (adds aggressive flags only if explicitly enabled)
SIZE_TUNE ?= 0

# Flags (default: size-optimized release build)
# NOTE: We intentionally omit -g and enable section GC & visibility reductions for size.
CFLAGS   ?= -Wall -Wextra -Os -std=c99 -ffunction-sections -fdata-sections
CXXFLAGS ?= -Wall -Wextra -Os -std=c++17 -pipe -ffunction-sections -fdata-sections -fvisibility=hidden -MMD -MP
LDFLAGS  ?=
LIBS     ?=

## (Removed default --gc-sections to favor static stability.)

# Link Time Optimization (can disable with LTO=0)
# Link Time Optimization (can disable with LTO=0). Some GCC versions emit an
# unavoidable informational note about serial LTRANS. Set LTO=quiet to disable LTO.
LTO ?= 1
ifeq ($(LTO),quiet)
	LTO := 0
endif
ifeq ($(LTO),1)
	CFLAGS   += -flto=auto -fno-fat-lto-objects
	CXXFLAGS += -flto=auto -fno-fat-lto-objects
	LDFLAGS  += -flto=auto
endif

# Strip final binary by default (disable with STRIP_BINARY=0 or via `make debug`).
STRIP ?= strip
STRIP_BINARY ?= 1

# Paths
BIN_DIR := bin
SRC_DIR := src
INC_DIR := $(SRC_DIR)
INC_DIR2 := $(SRC_DIR)/include

# Version detection for release artifacts (used by docs); falls back to dev string
VERSION ?= $(shell if [ -f VERSION ]; then cat VERSION; elif git describe --tags --match '[0-9]*' --abbrev=0 >/dev/null 2>&1; then git describe --tags --match '[0-9]*' --abbrev=0; else echo 0.0.0+dev; fi)
# Primary binary name (define early so 'all' always builds it)
TARGET_BIN ?= $(BIN_DIR)/unroot
UTIL_BIN ?= $(BIN_DIR)/unroot-util
UNROOT_UTIL_LIBSUBID ?= auto
UTIL_SOURCES := $(SRC_DIR)/unroot_util.cpp $(SRC_DIR)/util/subid_backend.cpp

# Default target: C++ build + generated docs
.PHONY: all
all: $(TARGET_BIN) $(UTIL_BIN) docs

# Convenience full clean (remove objects, deps, binaries, and generated headers)
.PHONY: clean
clean:
	@echo "[clean] removing objects and dependency files" ; \
	find $(SRC_DIR) -type f \( -name '*.o' -o -name '*.d' \) -delete 2>/dev/null || true; \
	find legacy -type f \( -name '*.o' -o -name '*.d' \) -delete 2>/dev/null || true; \
	rm -f *.o *.d legacy/*.o legacy/sds/*.o $(BIN_DIR)/unroot $(UTIL_BIN) $(BIN_DIR)/unroot-tests $(BIN_DIR)/unroot-legacy $(BIN_DIR)/unroot-d 2>/dev/null || true; \
	rm -f $(BUILD_DIR)/libsubid-probe 2>/dev/null || true; \
	rm -f $(BUILD_DIR)/version.hpp 2>/dev/null || true

## C++ (primary) build
CPP_SOURCES := \
	$(SRC_DIR)/unroot.cpp \
	$(SRC_DIR)/app_core.cpp \
	$(SRC_DIR)/debug_setup.cpp \
	$(SRC_DIR)/program_context.cpp \
	$(SRC_DIR)/diagnostics.cpp \
	$(SRC_DIR)/linuxns.cpp \
	$(SRC_DIR)/binfmt.cpp \
	$(SRC_DIR)/meta.cpp \
	$(SRC_DIR)/shebang.cpp \
	$(SRC_DIR)/emulation.cpp \
	$(SRC_DIR)/compat_blacklist.cpp \
	$(SRC_DIR)/hostcaps.cpp \
	$(SRC_DIR)/wrapper.cpp \
	$(SRC_DIR)/arch.cpp \
	$(SRC_DIR)/actions/unified_action_registry.cpp \
	$(SRC_DIR)/actions/config_base.cpp \
	$(SRC_DIR)/actions/archive_config.cpp \
	$(SRC_DIR)/actions/archive_action.cpp \
	$(SRC_DIR)/actions/enter_config.cpp \
	$(SRC_DIR)/actions/enter_action.cpp \
	$(SRC_DIR)/actions/parsed_args.cpp \
	$(SRC_DIR)/util/path.cpp \
	$(SRC_DIR)/util/idmap.cpp \
	$(SRC_DIR)/util/rootfs.cpp \
	$(SRC_DIR)/util/subid.cpp \
	$(SRC_DIR)/util/exception_handler.cpp

CPP_OBJECTS := $(CPP_SOURCES:.cpp=.o)
CPP_DEPS := $(CPP_OBJECTS:.o=.d)

# Generated build/version header (git SHA + parsed semantic version components)
BUILD_DIR := build
VERSION_HDR := $(BUILD_DIR)/version.hpp

.PHONY: FORCE
FORCE:

$(VERSION_HDR): FORCE VERSION
	@mkdir -p $(BUILD_DIR)
	@MAJOR=0; MINOR=0; PATCH=0; \
	CORE=$$(printf '%s' "$(VERSION)" | sed 's/[_-].*//'); \
	if echo "$$CORE" | grep -Eq '^[0-9]+\.[0-9]+(\.[0-9]+)?$$'; then \
	  MAJOR=$$(echo "$$CORE" | cut -d. -f1); \
	  MINOR=$$(echo "$$CORE" | cut -d. -f2); \
	  PATCH=$$(echo "$$CORE" | cut -s -d. -f3); \
	  PATCH=$${PATCH:-0}; \
	fi; \
	GIT_SHA=$$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown); \
	echo "#pragma once" > $(VERSION_HDR).tmp; \
	echo "#define UNROOT_VERSION_MAJOR $$MAJOR" >> $(VERSION_HDR).tmp; \
	echo "#define UNROOT_VERSION_MINOR $$MINOR" >> $(VERSION_HDR).tmp; \
	echo "#define UNROOT_VERSION_PATCH $$PATCH" >> $(VERSION_HDR).tmp; \
	echo "#define UNROOT_VERSION_STRING \"$(VERSION)\"" >> $(VERSION_HDR).tmp; \
	echo "#define UNROOT_GIT_SHA \"$$GIT_SHA\"" >> $(VERSION_HDR).tmp; \
	if ! cmp -s $(VERSION_HDR).tmp $(VERSION_HDR); then \
	  mv $(VERSION_HDR).tmp $(VERSION_HDR); \
	else \
	  rm $(VERSION_HDR).tmp; \
	fi

$(SRC_DIR)/app_core.o $(SRC_DIR)/emulation.o: $(VERSION_HDR)

CPP_INCLUDES := -I$(INC_DIR) -I$(INC_DIR2)

# Build objects per source to allow parallel builds

$(SRC_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(CPP_INCLUDES) -c $< -o $@

# Always build static (project intent). Keep existing -Os default.
STATIC := 1
ifeq ($(STATIC),1)
  CXXFLAGS += -static
  LDFLAGS  += -static
endif

# Strict warning-clean build. Define these target-specific flags after static
# policy is applied so this path cannot silently produce a dynamic core.
.PHONY: warnclean
warnclean: CXXFLAGS := $(filter-out -flto -flto=auto -fno-fat-lto-objects,$(CXXFLAGS)) -Werror
warnclean: CFLAGS := $(filter-out -flto -flto=auto -fno-fat-lto-objects,$(CFLAGS)) -Werror
warnclean: LDFLAGS := $(filter-out -flto -flto=auto,$(LDFLAGS))
warnclean: LTO=0
warnclean: clean all

# Optional size tuning (explicit opt-in): keeps exceptions unless user also sets NO_EXCEPTIONS=1
NO_EXCEPTIONS ?= 0
ifeq ($(SIZE_TUNE),1)
  CXXFLAGS += -ffunction-sections -fdata-sections -fvisibility=hidden
  LDFLAGS  += -Wl,--gc-sections
  ifeq ($(NO_EXCEPTIONS),1)
    CXXFLAGS += -fno-exceptions -fno-rtti
  endif
endif

## (TARGET_BIN defined near top)

# Include header dependency files (optional; ignore missing on first run)
-include $(CPP_DEPS)

$(TARGET_BIN): $(CPP_OBJECTS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(CPP_INCLUDES) $^ -o $@ $(LDFLAGS) $(LIBS)
	@if [ "$(STRIP_BINARY)" = "1" ]; then \
	  if command -v $(STRIP) >/dev/null 2>&1; then \
	    echo "Stripping $@"; $(STRIP) -s $@; \
	  else \
	    echo "strip tool not found; skipping strip"; \
	  fi; \
	fi

# Host integration is deliberately kept outside the static namespace engine.
# Build unroot-util dynamically, using libsubid when its development interface
# is available and the portable local-file backend otherwise.
$(UTIL_BIN): $(UTIL_SOURCES) $(SRC_DIR)/util/subid.hpp $(SRC_DIR)/util/subid_backend.hpp $(VERSION_HDR)
	@mkdir -p $(BIN_DIR) $(BUILD_DIR)
	@set -e; \
	use_libsubid="$(UNROOT_UTIL_LIBSUBID)"; \
	if [ "$$use_libsubid" = auto ]; then \
	  if printf '%s\n' '#include <cstdlib>' '#if __has_include(<shadow/subid.h>)' '#include <shadow/subid.h>' '#elif __has_include(<subid.h>)' '#include <subid.h>' '#else' '#error libsubid header unavailable' '#endif' 'int main(){struct subid_range *r=0; if(!subid_init("probe", 0)) return 1; int n=subid_get_uid_ranges("0", &r); std::free(r); return n<0;}' | \
	       $(CXX) -x c++ - -o $(BUILD_DIR)/libsubid-probe -lsubid >/dev/null 2>&1; then \
	    use_libsubid=1; \
	  else \
	    use_libsubid=0; \
	  fi; \
	fi; \
	case "$$use_libsubid" in 0|1) ;; *) echo "UNROOT_UTIL_LIBSUBID must be auto, 0, or 1" >&2; exit 2;; esac; \
	flags=""; libs=""; \
	if [ "$$use_libsubid" = 1 ]; then \
	  flags="-DUNROOT_HAVE_LIBSUBID=1"; libs="-lsubid"; backend=libsubid; \
	else \
	  backend=files; \
	fi; \
	echo "Building unroot-util (backend: $$backend)"; \
	$(CXX) $(filter-out -static -MMD -MP,$(CXXFLAGS)) $(CPP_INCLUDES) $$flags \
	  $(UTIL_SOURCES) -o $@ $(filter-out -static,$(LDFLAGS)) $$libs; \
	if [ "$(STRIP_BINARY)" = 1 ] && command -v $(STRIP) >/dev/null 2>&1; then \
	  $(STRIP) -s $@; \
	fi

# Convenience target to just build CLI (no docs)
.PHONY: cli util
cli: $(TARGET_BIN) $(UTIL_BIN)
util: $(UTIL_BIN)
# (Removed accidental embedded test invocation which caused build failures.)
.PHONY: debug
debug: CXXFLAGS := $(filter-out -Os,$(CXXFLAGS)) -O0 -g
debug: CFLAGS := $(filter-out -Os,$(CFLAGS)) -O0 -g
debug: STRIP_BINARY=0
debug: LTO=0
debug: all

## Documentation compiled from docs/unroot.docatoms
.PHONY: docs docs-check
docs:
	$(PYTHON) scripts/build_docs.py --version "$(VERSION)"

docs-check:
	$(PYTHON) scripts/build_docs.py --version "$(VERSION)" --check

## ---------------------------------------------------------------------------
## Doctest coverage (pilot) - produces coverage/doctest/ reports for the
## currently ported doctest suite (compat blacklist). Independent of Catch2.
## ---------------------------------------------------------------------------

.PHONY: doctest-coverage
doctest-coverage:
	@set -e; \
	mkdir -p coverage/doctest; \
	rm -f coverage/doctest/coverage-*.profraw coverage/doctest/coverage.profdata 2>/dev/null || true; \
	if command -v clang++ >/dev/null 2>&1 && command -v $(LLVM_COV) >/dev/null 2>&1 && command -v $(LLVM_PROFDATA) >/dev/null 2>&1 \
	   && echo '#include <stddef.h>' | clang++ -E -xc++ - >/dev/null 2>&1; then \
	  echo "[doctest-cov] Using Clang + llvm-cov"; \
	  echo "[doctest-cov] Building instrumented doctest harness"; \
	  clang++ -Wall -Wextra -O0 -g -std=c++17 -fprofile-instr-generate -fcoverage-mapping -DUNROOT_ENABLE_DOCTEST \
	    -Isrc -Isrc/include -Ithird_party/doctest \
	    tests/doctest_main.cpp tests/dt_compat_blacklist.cpp tests/dt_option_parser.cpp tests/dt_option_parser_trailing.cpp tests/dt_action_trailing_negative.cpp tests/dt_enter_config_trailing.cpp tests/dt_enter_config_integration.cpp tests/dt_enter_config_idmap.cpp tests/dt_util_path.cpp tests/dt_util_subid.cpp tests/dt_error_map.cpp tests/dt_app_exit.cpp tests/dt_arch.cpp tests/dt_arch_errors.cpp tests/dt_exception_handler.cpp tests/enter_action_stub.cpp \
	    src/compat_blacklist.cpp src/actions/config_base.cpp src/actions/enter_config.cpp src/actions/unified_action_registry.cpp src/actions/parsed_args.cpp src/program_context.cpp src/util/path.cpp src/util/subid.cpp src/util/subid_backend.cpp src/arch.cpp src/util/exception_handler.cpp \
	    -o bin/unroot-tests-doctest-cov -pthread || { echo "[doctest-cov] clang build failed; falling back to GCC"; false; }; \
	  echo "[doctest-cov] Running doctest harness"; \
	  LLVM_PROFILE_FILE=coverage/doctest/coverage-%p.profraw bin/unroot-tests-doctest-cov || true; \
	  $(LLVM_PROFDATA) merge -sparse coverage/doctest/coverage-*.profraw -o coverage/doctest/coverage.profdata; \
	  $(LLVM_COV) report bin/unroot-tests-doctest-cov -instr-profile=coverage/doctest/coverage.profdata \
	    -ignore-filename-regex='(third_party|tests/|/usr/include/)' > coverage/doctest/coverage.txt; \
	  $(LLVM_COV) show bin/unroot-tests-doctest-cov -instr-profile=coverage/doctest/coverage.profdata \
	    -format=html -output-dir=coverage/doctest/html -show-branches=count -show-expansions \
	    -ignore-filename-regex='(third_party|tests/|/usr/include/)' src || true; \
	  $(LLVM_COV) export bin/unroot-tests-doctest-cov -instr-profile=coverage/doctest/coverage.profdata -format=lcov > coverage/doctest/coverage.lcov || true; \
	  echo "[doctest-cov] Coverage (LLVM) written to coverage/doctest/coverage.txt"; \
	else \
	  echo "[doctest-cov] Falling back to GCC + gcovr"; \
	  if ! command -v gcovr >/dev/null 2>&1; then echo "gcovr not installed; please install gcovr for GCC coverage (e.g., 'pip install gcovr')"; exit 1; fi; \
	  echo "[doctest-cov] Cleaning stale GCC coverage artifacts"; \
	  find . -name '*.gcda' -o -name '*.gcno' -o -name '*.gcov' -delete 2>/dev/null || true; \
	  g++ -Wall -Wextra -O0 -g -std=c++17 --coverage -DUNROOT_ENABLE_DOCTEST -Isrc -Isrc/include -Ithird_party/doctest \
	    tests/doctest_main.cpp tests/dt_compat_blacklist.cpp tests/dt_option_parser.cpp tests/dt_option_parser_trailing.cpp tests/dt_action_trailing_negative.cpp tests/dt_enter_config_trailing.cpp tests/dt_enter_config_integration.cpp tests/dt_enter_config_idmap.cpp tests/dt_util_path.cpp tests/dt_util_subid.cpp tests/dt_error_map.cpp tests/dt_app_exit.cpp tests/dt_arch.cpp tests/dt_arch_errors.cpp tests/dt_exception_handler.cpp tests/enter_action_stub.cpp \
	    src/compat_blacklist.cpp src/actions/config_base.cpp src/actions/enter_config.cpp src/actions/unified_action_registry.cpp src/actions/parsed_args.cpp src/program_context.cpp src/util/path.cpp src/util/subid.cpp src/util/subid_backend.cpp src/arch.cpp src/util/exception_handler.cpp \
	    -o bin/unroot-tests-doctest-cov -pthread; \
	  echo "[doctest-cov] Running doctest harness (GCC)"; \
	  ./bin/unroot-tests-doctest-cov || true; \
	  echo "[doctest-cov] Invoking gcovr"; \
	  unset GCOV; \
	  # Suppress known GCC gcov negative branch hit parse issue (GCC Bug 68080) so report generation proceeds. \
	  gcovr -r . --gcov-executable gcov --merge-mode-functions=separate --gcov-ignore-parse-errors=negative_hits.warn_once_per_file --filter src/compat_blacklist.cpp --filter src/actions/config_base.cpp --filter src/util/path.cpp --filter src/util/subid.cpp --filter src/util/subid_backend.cpp --filter src/arch.cpp --xml-pretty -o coverage/doctest/coverage.xml \
	    --html-details coverage/doctest/coverage.html --html-title "Doctest Coverage" \
	    --txt coverage/doctest/coverage.txt --exclude 'tests/.*' --exclude 'third_party/.*' --lcov coverage/doctest/coverage.lcov || true; \
	  echo "[doctest-cov] Coverage (GCC) written to coverage/doctest/coverage.txt"; \
	fi
	@echo "[doctest-cov] Done"

# Remove stale GCC coverage artifacts (gcno/gcda/gcov) to avoid mismatched checksum warnings
.PHONY: coverage-clean
coverage-clean:
	@echo "[coverage-clean] Removing stale gcov artifacts"; \
	find . -name '*.gcda' -o -name '*.gcno' -o -name '*.gcov' -delete 2>/dev/null || true; \
	rm -f coverage/doctest/coverage.* 2>/dev/null || true; \
	echo "[coverage-clean] Done"

## ---------------------------------------------------------------------------
## Aggregate coverage (all src/*.cpp) to show global low % including untested
## files. Builds a large instrumented test binary that links every translation
## unit (except the production main in unroot.cpp) plus all doctest tests.
## Output lives under coverage/doctest/all/.
## Usage: make doctest-coverage-all [AGG_JOBS=N]
## ---------------------------------------------------------------------------
AGG_COV_DIR := coverage/doctest/all
AGG_COV_OBJ_DIR := $(AGG_COV_DIR)/obj

.PHONY: doctest-coverage-all
doctest-coverage-all:
	@set -e; \
	# Clean stray coverage artifacts in bin that could confuse gcov/gcovr
	[ -d bin ] && find bin -maxdepth 1 \( -name '*.gcda' -o -name '*.gcno' -o -name '*.gcov' \) -delete 2>/dev/null || true; \
	# Ensure version header (some sources include it)
	if [ ! -f $(VERSION_HDR) ]; then echo "[agg-cov] Generating version header"; $(MAKE) $(VERSION_HDR); fi; \
	mkdir -p $(AGG_COV_DIR); \
	rm -rf $(AGG_COV_OBJ_DIR); mkdir -p $(AGG_COV_OBJ_DIR)/tests; \
	echo "[agg-cov] Auto-discovering source files under src/ (excluding unroot.cpp + enter_action.cpp + linuxns.cpp Option A)"; \
	# Option A: omit real enter_action.cpp and linuxns.cpp; rely on enter_action_stub.cpp to avoid heavy namespace/filesystem linkage & duplicate symbols
	ALL_CPP_SRC_NO_MAIN=$$(find src -type f -name '*.cpp' \
	  ! -name 'unroot.cpp' ! -name 'enter_action.cpp' ! -name 'linuxns.cpp' \
	  ! -path 'third_party/*' | sort); \
	echo "$$ALL_CPP_SRC_NO_MAIN" > $(AGG_COV_DIR)/source_files.txt; \
	JOBS=$${AGG_JOBS:-$$(nproc 2>/dev/null || echo 1)}; \
	echo "[agg-cov] Total sources (excl main): $$(echo "$$ALL_CPP_SRC_NO_MAIN" | wc -l)"; \
	pending_pids=""; launched=0; \
	for f in $$ALL_CPP_SRC_NO_MAIN; do \
	  rel=$${f#src/}; out=$(AGG_COV_OBJ_DIR)/$$rel.o; mkdir -p $$(dirname $$out); \
	  if [ $$JOBS -gt 1 ]; then \
	    ( $(CXX) -O0 -g --coverage -DUNROOT_ENABLE_DOCTEST -Isrc -Isrc/include -Ithird_party/doctest -c $$f -o $$out || { echo "[agg-cov] Compile failed for $$f"; rm -f $$out; } ) & \
	    pending_pids="$$pending_pids $$!"; launched=$$((launched+1)); \
	    if [ $$((launched % JOBS)) -eq 0 ]; then wait $${pending_pids}; pending_pids=""; fi; \
	  else \
	    $(CXX) -O0 -g --coverage -DUNROOT_ENABLE_DOCTEST -Isrc -Isrc/include -Ithird_party/doctest -c $$f -o $$out || { echo "[agg-cov] Compile failed for $$f"; rm -f $$out; }; \
	  fi; \
	done; \
	if [ $$JOBS -gt 1 ] && [ -n "$$pending_pids" ]; then wait $${pending_pids}; fi; \
	# Compile all doctest test sources (wildcard for flexibility)
	for t in tests/doctest_main.cpp $$(ls tests/dt_*.cpp 2>/dev/null) tests/enter_action_stub.cpp tests/coverage_feature_stub.cpp; do \
	  [ -f $$t ] || continue; \
	  base=$$(basename $$t .cpp); \
	  $(CXX) -O0 -g --coverage -DUNROOT_ENABLE_DOCTEST -Isrc -Isrc/include -Ithird_party/doctest -c $$t -o $(AGG_COV_OBJ_DIR)/tests/$$base.o || { echo "[agg-cov] Test compile failed for $$t"; exit 1; }; \
	done; \
	# Link: include every object we just built
	ALL_OBJS=$$(find $(AGG_COV_OBJ_DIR) -type f -name '*.o' | sort); \
	[ -z "$$ALL_OBJS" ] && { echo "[agg-cov] No objects built"; exit 1; } || true; \
	AGG_BIN=$(AGG_COV_DIR)/unroot-tests-doctest-all-cov; \
	$(CXX) -O0 -g --coverage $$ALL_OBJS -o $$AGG_BIN -pthread; \
	$$AGG_BIN || true; \
	if ! command -v gcovr >/dev/null 2>&1; then echo "gcovr not installed (pip install gcovr)"; exit 1; fi; \
	# Add parse error suppression for negative branch hits (GCC Bug 68080). We do not enable branch coverage anyway, but gcovr parses the lines. \
	gcovr -r . --object-directory $(AGG_COV_OBJ_DIR) --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
	  --filter 'src/' \
	  --exclude '.*tests/.*' --exclude '.*third_party/.*' --exclude 'coverage/obj/.*' --exclude '.*bin/.*' \
	  --merge-mode-functions=separate --xml-pretty -o $(AGG_COV_DIR)/coverage.xml --html-details $(AGG_COV_DIR)/coverage.html --html-title "Aggregate Doctest Coverage" \
	  --txt $(AGG_COV_DIR)/coverage.txt --lcov $(AGG_COV_DIR)/coverage.lcov || true; \
	echo "[agg-cov] Global aggregate coverage at $(AGG_COV_DIR)/coverage.txt"; \
	PCT=$$(awk '/^TOTAL/ {print $$NF}' $(AGG_COV_DIR)/coverage.txt 2>/dev/null || echo '?'); \
	echo "[agg-cov] Overall line coverage: $$PCT"; \
	echo "[agg-cov] Done"

## Diff (changed-line) coverage for doctest sources
.PHONY: doctest-diff-coverage
doctest-diff-coverage: doctest-coverage
	@set -e; \
	if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
	  echo "[diff-cov] Not a git repository; cannot compute diff coverage"; exit 0; \
	fi; \
	REF=$${REF:-origin/main}; \
	if ! git rev-parse --verify $$REF >/dev/null 2>&1; then \
	  echo "[diff-cov] Ref '$$REF' not found; falling back to HEAD~1"; \
	  REF=HEAD~1; \
	fi; \
	echo "[diff-cov] Comparing against $$REF"; \
	DIFF_FILE=coverage/doctest/git_diff.patch; \
	git diff --unified=0 $$REF -- src/ > $$DIFF_FILE || true; \
	CHANGED=coverage/doctest/changed_lines.txt; \
	LCOV=coverage/doctest/coverage.lcov; \
	RESULT=coverage/doctest/diff_coverage.txt; \
	> $$CHANGED; \
	awk 'BEGIN{file=""} \
	/^\+\+\+ b\//{file=substr($$0,7);} \
	/@@/ { if (file=="") next; split($$0,a," "); for(i in a) if(a[i] ~ /^\+/){match(a[i],/\+([0-9]+)(,([0-9]+))?/,m); if(m[1]!=""){ start=m[1]; count=(m[3]!=""?m[3]:1); for(l=start;l<start+count;l++) print file":"l; } } }' $$DIFF_FILE > $$CHANGED; \
	TOTAL_CHANGED=$$(wc -l < $$CHANGED | tr -d ' '); \
	if [ $$TOTAL_CHANGED -eq 0 ]; then echo "[diff-cov] No changed lines under src/ relative to $$REF"; echo "No changed lines" > $$RESULT; exit 0; fi; \
	awk -F':' 'BEGIN{coveredFileLine=""} \
	/^SF:/{gsub(/\r/,"",$$0); file=substr($$0,4);} \
	/^DA:/ { split($$0,da,":"); split(da[2],v,","); line=v[1]; cnt=v[2]; key=file":"line; if(cnt>0) covered[key]=1;} \
	END{ while((getline cl < ARGV[1])>0){gsub(/\r/,"",cl); if(cl in covered){cov++;} total++; changed[cl]= (cl in covered ? 1:0);} \
	  pct = (total? (100.0*cov/total):100); printf("Changed lines: %d\nCovered changed lines: %d\nDiff coverage: %.2f%%\n", total, cov, pct) > ARGV[2]; \
	  close(ARGV[2]); \
	  for (c in changed) { if(!changed[c]) misses++; } \
	  printf("[diff-cov] %d/%d (%.2f%%) changed lines covered. Misses: %d\n", cov,total,pct,misses); \
	  thr=ENVIRON["DIFF_COV_THRESHOLD"]; if(thr!="" && pct+1e-9 < thr){ printf("[diff-cov] FAIL: below threshold %.2f%% < %.2f%%\n", pct, thr); exit 2;} }' $$CHANGED $$LCOV $$RESULT; \
	echo "[diff-cov] Detailed report: $$RESULT"; cat $$RESULT

## Install (optional)
.PHONY: install uninstall
PREFIX ?= /usr/local
DESTDIR ?=
BINDIR := $(DESTDIR)$(PREFIX)/bin
MANDIR := $(DESTDIR)$(PREFIX)/share/man/man1
DOCDIR := $(DESTDIR)$(PREFIX)/share/doc/unroot

install: $(BIN_DIR)/unroot $(UTIL_BIN) docs
	@echo "Installing to $(PREFIX) (DESTDIR=$(DESTDIR))"; \
	install -d "$(BINDIR)" "$(MANDIR)" "$(DOCDIR)"; \
	install -m 0755 "$(BIN_DIR)/unroot" "$(BINDIR)/unroot"; \
	install -m 0755 "$(UTIL_BIN)" "$(BINDIR)/unroot-util"; \
	install -m 0644 docs/unroot.1 "$(MANDIR)/unroot.1"; \
	install -m 0644 docs/unroot.md "$(DOCDIR)/unroot.md"

uninstall:
	@echo "Uninstalling from $(PREFIX) (DESTDIR=$(DESTDIR))"; \
	rm -f "$(BINDIR)/unroot" "$(BINDIR)/unroot-util" "$(MANDIR)/unroot.1" "$(DOCDIR)/unroot.md" 2>/dev/null || true

## Distribution tarball (source)
.PHONY: dist dist-clean
DIST_DIR := dist
DIST_NAME := unroot-$(VERSION)
DIST_TAR := $(DIST_DIR)/$(DIST_NAME).tar.xz

dist: docs
	@set -e; \
	mkdir -p "$(DIST_DIR)" "$(DIST_DIR)/.stage"; \
	rm -rf "$(DIST_DIR)/.stage/$(DIST_NAME)"; \
	if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
	  echo "Staging source from git archive"; \
	  git archive --format=tar --prefix="$(DIST_NAME)/" HEAD | tar -C "$(DIST_DIR)/.stage" -xf -; \
	else \
	  echo "git not available; staging from working tree"; \
	  mkdir -p "$(DIST_DIR)/.stage/$(DIST_NAME)"; \
	  rsync -a --delete --exclude '.git/' --exclude '$(DIST_DIR)/' --exclude 'bin/' --exclude 'coverage/' --exclude '*.o' --exclude '*.gcno' --exclude '*.gcda' ./ "$(DIST_DIR)/.stage/$(DIST_NAME)/"; \
	fi; \
	# Overlay freshly generated docs in case the source tree was dirty.
	mkdir -p "$(DIST_DIR)/.stage/$(DIST_NAME)/docs"; \
	cp docs/unroot.1 docs/unroot.md docs/unroot.docs.json "$(DIST_DIR)/.stage/$(DIST_NAME)/docs/"; \
	# Pack
	TAR_TMP="$(DIST_DIR)/$(DIST_NAME).tar"; \
	[ -f "$$TAR_TMP" ] && rm -f "$$TAR_TMP" || true; \
	tar -C "$(DIST_DIR)/.stage" -cf "$$TAR_TMP" "$(DIST_NAME)"; \
	xz -f -T0 -9e "$$TAR_TMP"; \
	echo "Built $(DIST_TAR)"

dist-clean:
	rm -rf "$(DIST_DIR)" 2>/dev/null || true

## Release helpers
.PHONY: release release-refresh
TAG := $(VERSION)

release: ## Create annotated tag $(VERSION), build dist, and push tag; fails if tag exists
	@set -e; \
	git rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo "Not a git repo"; exit 1; }; \
	git update-index -q --refresh; \
	if ! git diff --quiet || ! git diff --cached --quiet; then \
	  echo "Refusing to release: working tree has uncommitted changes"; exit 1; \
	fi; \
	if git rev-parse -q --verify "refs/tags/$(TAG)" >/dev/null; then \
	  echo "Tag $(TAG) already exists. Use 'make release-refresh' if you intend to rewrite it."; exit 1; \
	fi; \
	git tag -a "$(TAG)" -m "unroot $(VERSION)"; \
	$(MAKE) dist; \
	git push origin "$(TAG)"; \
	echo "Released $(TAG)"

release-refresh: ## Force rewrite annotated tag $(VERSION), rebuild dist, and force-push tag
	@set -e; \
	git rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo "Not a git repo"; exit 1; }; \
	git update-index -q --refresh; \
	if ! git diff --quiet || ! git diff --cached --quiet; then \
	  echo "Refusing to release: working tree has uncommitted changes"; exit 1; \
	fi; \
	git tag -d "$(TAG)" >/dev/null 2>&1 || true; \
	git push origin :refs/tags/"$(TAG)" >/dev/null 2>&1 || true; \
	git tag -a "$(TAG)" -m "unroot $(VERSION)"; \
	$(MAKE) dist; \
	git push --force origin "$(TAG)"; \
	echo "Refreshed release $(TAG)"

## Legacy C build
.PHONY: legacy
legacy: $(BIN_DIR)/unroot-legacy

LEGACY_OBJS := \
	legacy/unroot.o \
	legacy/arch_db.o \
	legacy/subarch_db.o \
	legacy/unroot_util.o \
	legacy/sds/sds.o

$(BIN_DIR)/unroot-legacy: $(LEGACY_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)

# Generic C object rule
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Legacy nested pattern rules (so we don't pollute root directory)
legacy/%.o: legacy/%.c
	$(CC) $(CFLAGS) -c $< -o $@

legacy/sds/%.o: legacy/sds/%.c
	$(CC) $(CFLAGS) -c $< -o $@

## Test / Coverage tool binaries (used by doctest coverage targets)
LLVM_COV      ?= llvm-cov
LLVM_PROFDATA ?= llvm-profdata

# (Removed legacy Catch2 harness variables and targets.)

## Doctest harness build (all doctest-based unit tests)
DOCTEST_TEST_SRCS := tests/doctest_main.cpp $(wildcard tests/dt_*.cpp) tests/enter_action_stub.cpp tests/archive_action_stub.cpp
DOCTEST_HARNESS_SRCS := \
	src/compat_blacklist.cpp \
	src/meta.cpp \
	src/binfmt.cpp \
	src/hostcaps.cpp \
	src/actions/config_base.cpp \
	src/actions/archive_config.cpp \
	src/actions/enter_config.cpp \
	src/actions/unified_action_registry.cpp \
	src/actions/parsed_args.cpp \
	src/program_context.cpp \
	src/util/path.cpp \
	src/util/idmap.cpp \
	src/util/rootfs.cpp \
	src/util/subid.cpp \
	src/util/subid_backend.cpp \
	src/arch.cpp \
	src/shebang.cpp \
	src/util/exception_handler.cpp

DOCTEST_TEST_SRCS += tests/coverage_feature_stub.cpp

.PHONY: doctest
doctest: $(DOCTEST_TEST_SRCS) $(DOCTEST_HARNESS_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(filter-out -static,$(CXXFLAGS)) -DUNROOT_ENABLE_DOCTEST -I$(SRC_DIR) -I$(INC_DIR2) -Ithird_party/doctest \
	  $(DOCTEST_TEST_SRCS) $(DOCTEST_HARNESS_SRCS) \
	  -o $(BIN_DIR)/unroot-tests-doctest -pthread
	@echo "[doctest] built bin/unroot-tests-doctest (sources: $$(echo $(DOCTEST_TEST_SRCS) | wc -w))"

.PHONY: test
test: $(UTIL_BIN) doctest
	@echo "--- Running doctest suite ---"
	@$(BIN_DIR)/unroot-tests-doctest

.PHONY: e2e
e2e: $(TARGET_BIN) $(UTIL_BIN)
	@$(PYTEST) -q tests/e2e/test_native_enter.py tests/e2e/test_archive.py tests/e2e/test_rich_idmap.py tests/e2e/test_rootfs_journey.py

.PHONY: e2e-cross
e2e-cross: $(TARGET_BIN) $(UTIL_BIN)
	@$(PYTEST) -q tests/e2e/test_cross_arch.py

.PHONY: e2e-all
e2e-all: e2e e2e-cross

.PHONY: check
check: test e2e

## Nim build (optional/fun)
.PHONY: nim nim-clean nim-test
nim:
	nim c -d:release --mm:arc --opt:size --passc:-fno-exceptions --passl:-s -o:$(BIN_DIR)/unroot-nim src/unroot.nim || true

nim-clean:
	rm -f $(BIN_DIR)/unroot-nim

nim-test:
	nim c -d:release --mm:arc -o:$(BIN_DIR)/unroot-nim src/unroot.nim || true
	nim r --mm:arc tests/test_cli.nim || true

# (Removed duplicate clean target at bottom; primary clean defined near top.)

## D build (experimental stub retained)
.PHONY: d d-clean
D_COMPILER := $(shell command -v gdc || command -v ldc2 || command -v dmd)

d:
	@if [ -z "$(D_COMPILER)" ]; then \
		echo "No D compiler found (install gdc or ldc2)"; exit 1; \
	fi
	@mkdir -p $(BIN_DIR)
	@if echo $(D_COMPILER) | grep -q gdc; then \
		$(D_COMPILER) -O2 -pipe -o $(BIN_DIR)/unroot-d dsrc/unroot.d dsrc/linuxns.d; \
	elif echo $(D_COMPILER) | grep -q ldc2; then \
		$(D_COMPILER) -O2 -release -of=$(BIN_DIR)/unroot-d dsrc/unroot.d dsrc/linuxns.d; \
	else \
		$(D_COMPILER) -O -release -of=$(BIN_DIR)/unroot-d dsrc/unroot.d dsrc/linuxns.d; \
	fi

d-clean:
	rm -f $(BIN_DIR)/unroot-d

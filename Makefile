# cdin Backend: Kernel-independent services plus a small Kernel-hosting library.
CC ?= cc
AR ?= ar
RANLIB ?= ranlib
PKG_CONFIG ?= pkg-config

BUILD_DIR ?= build
KERNEL_PREFIX ?= $(CURDIR)/.deps/kernel
export KERNEL_PREFIX
KERNEL_MIN_VERSION ?= 1.2.1
LUA_PKG ?= lua5.4
WITH_LUA ?= auto
HOST_OS := $(shell uname -s 2>/dev/null || printf unknown)

BASE_CFLAGS := -std=c11 -Wall -Wextra -Wshadow -Wconversion -Wstrict-prototypes -Wformat=2 -Wpedantic -Werror
CFLAGS ?= -O2
CFLAGS += $(BASE_CFLAGS)
BASE_CPPFLAGS := -Iinclude -Isrc

# pkg-config is used only by the Backend/Kernel integration library. The
# services archive is intentionally buildable without Kernel installed.
ifneq ($(findstring MINGW,$(HOST_OS)),)
  KERNEL_PKG_CONFIG_DIR := $(shell cygpath -m "$(KERNEL_PREFIX)/lib/pkgconfig")
  KERNEL_PKG_CONFIG_WINDOWS_PATH := yes
  THREAD_LIBS :=
else ifneq ($(findstring MSYS,$(HOST_OS)),)
  KERNEL_PKG_CONFIG_DIR := $(shell cygpath -m "$(KERNEL_PREFIX)/lib/pkgconfig")
  KERNEL_PKG_CONFIG_WINDOWS_PATH := yes
  THREAD_LIBS :=
else ifneq ($(findstring CYGWIN,$(HOST_OS)),)
  KERNEL_PKG_CONFIG_DIR := $(shell cygpath -m "$(KERNEL_PREFIX)/lib/pkgconfig")
  KERNEL_PKG_CONFIG_WINDOWS_PATH := yes
  THREAD_LIBS :=
else
  KERNEL_PKG_CONFIG_DIR := $(KERNEL_PREFIX)/lib/pkgconfig
  THREAD_LIBS := -pthread
endif

ifeq ($(KERNEL_PKG_CONFIG_WINDOWS_PATH),yes)
  KERNEL_PKG_CONFIG_PATH := $(KERNEL_PKG_CONFIG_DIR)
else
  KERNEL_PKG_CONFIG_PATH := $(KERNEL_PKG_CONFIG_DIR)$(if $(strip $(PKG_CONFIG_PATH)),:$(PKG_CONFIG_PATH))
endif
export PKG_CONFIG_PATH := $(KERNEL_PKG_CONFIG_PATH)

# Pass the package search path explicitly to make's parse-time shell calls.
# This avoids depending on how a caller exported PKG_CONFIG_PATH between steps.
KERNEL_PKG_OK := $(shell PKG_CONFIG_PATH="$(KERNEL_PKG_CONFIG_PATH)" $(PKG_CONFIG) --atleast-version=$(KERNEL_MIN_VERSION) cdin-kernel >/dev/null 2>&1 && echo yes)
KERNEL_CFLAGS := $(shell PKG_CONFIG_PATH="$(KERNEL_PKG_CONFIG_PATH)" $(PKG_CONFIG) --cflags cdin-kernel 2>/dev/null)
KERNEL_LIBS := $(shell PKG_CONFIG_PATH="$(KERNEL_PKG_CONFIG_PATH)" $(PKG_CONFIG) --static --libs cdin-kernel 2>/dev/null)

LUA_AVAILABLE := $(shell $(PKG_CONFIG) --exists $(LUA_PKG) 2>/dev/null && echo yes)
ifeq ($(WITH_LUA),1)
  ifneq ($(LUA_AVAILABLE),yes)
    $(error Lua 5.4 development package '$(LUA_PKG)' not found. Install it or use WITH_LUA=0.)
  endif
  LUA_CPPFLAGS := -DCDIN_WITH_LUA $(shell $(PKG_CONFIG) --cflags $(LUA_PKG))
  LUA_LIBS := $(shell $(PKG_CONFIG) --libs $(LUA_PKG))
else ifeq ($(WITH_LUA),auto)
  ifeq ($(LUA_AVAILABLE),yes)
    LUA_CPPFLAGS := -DCDIN_WITH_LUA $(shell $(PKG_CONFIG) --cflags $(LUA_PKG))
    LUA_LIBS := $(shell $(PKG_CONFIG) --libs $(LUA_PKG))
  endif
endif

ifeq ($(HOST_OS),Linux)
  PLATFORM_CPPFLAGS := -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
else ifeq ($(HOST_OS),Darwin)
  PLATFORM_CPPFLAGS := -D_DARWIN_C_SOURCE -D_POSIX_C_SOURCE=200809L
endif

SERVICE_SRC := $(sort $(wildcard src/jobs/*.c) $(wildcard src/os/*.c) $(wildcard src/platform/*.c))
BACKEND_SRC := src/backend.c src/core/status.c src/diagnostics/log.c $(sort $(wildcard src/kernel/*.c) $(wildcard src/protocol/*.c) $(wildcard src/runtime/*.c) $(wildcard src/session/*.c))
SERVICE_OBJ := $(patsubst src/%.c,$(BUILD_DIR)/obj/services/%.o,$(SERVICE_SRC))
BACKEND_OBJ := $(patsubst src/%.c,$(BUILD_DIR)/obj/backend/%.o,$(BACKEND_SRC))
SERVICE_LIB := $(BUILD_DIR)/libcdin_backend_services.a
BACKEND_LIB := $(BUILD_DIR)/libcdin_backend.a

SCENARIO_OBJ := $(BUILD_DIR)/e2e/scenario.o
CHILD_BIN := $(BUILD_DIR)/e2e/test_child_process
E2E_JOB := $(BUILD_DIR)/e2e/job_pipeline
E2E_ABORT := $(BUILD_DIR)/e2e/job_abort_shutdown
E2E_FS := $(BUILD_DIR)/e2e/platform_file_workflow
E2E_PROCESS := $(BUILD_DIR)/e2e/process_supervision
INTEGRATION_BIN := $(BUILD_DIR)/integration/backend_session
INTEGRATION_KERNEL_BIN := $(BUILD_DIR)/integration/kernel_document_workflow
EXAMPLE_BACKUP_BIN := $(BUILD_DIR)/examples/atomic-backup

.PHONY: all services library require-kernel deps build e2e integration runtime-integration stress benchmark examples example-atomic-backup check test lint boundary info clean
all: services library
build: all
services: $(SERVICE_LIB)
library: require-kernel $(BACKEND_LIB)

require-kernel:
	@if [ "$(KERNEL_PKG_OK)" != yes ]; then \
	  echo "cdin-kernel >= $(KERNEL_MIN_VERSION) is not installed or not visible to pkg-config." >&2; \
	  echo "Run 'make deps' or set KERNEL_PREFIX / PKG_CONFIG_PATH to an installed Kernel package." >&2; \
	  exit 2; \
	fi

deps:
	bash scripts/install-deps.sh --prefix "$(KERNEL_PREFIX)"

$(SERVICE_LIB): $(SERVICE_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(BACKEND_LIB): $(BACKEND_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(BUILD_DIR)/obj/services/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(BASE_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/obj/backend/%.o: src/%.c | require-kernel
	@mkdir -p $(@D)
	$(CC) $(BASE_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(KERNEL_CFLAGS) $(LUA_CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(SCENARIO_OBJ): tests/e2e/support/scenario.c tests/e2e/support/scenario.h
	@mkdir -p $(@D)
	$(CC) $(BASE_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(CHILD_BIN): tests/e2e/support/test_child_process.c
	@mkdir -p $(@D)
	$(CC) $(PLATFORM_CPPFLAGS) $(CFLAGS) $< -o $@

$(E2E_JOB): tests/e2e/scenarios/job_pipeline.c $(SCENARIO_OBJ) $(SERVICE_LIB)
	$(CC) $(BASE_CPPFLAGS) -Itests/e2e/support $(CFLAGS) $< $(SCENARIO_OBJ) $(SERVICE_LIB) -o $@ $(THREAD_LIBS)

$(E2E_ABORT): tests/e2e/scenarios/job_abort_shutdown.c $(SCENARIO_OBJ) $(SERVICE_LIB)
	$(CC) $(BASE_CPPFLAGS) -Isrc -Itests/e2e/support $(CFLAGS) $< $(SCENARIO_OBJ) $(SERVICE_LIB) -o $@ $(THREAD_LIBS)

$(E2E_FS): tests/e2e/scenarios/platform_file_workflow.c $(SCENARIO_OBJ) $(SERVICE_LIB)
	$(CC) $(BASE_CPPFLAGS) -Itests/e2e/support $(CFLAGS) $< $(SCENARIO_OBJ) $(SERVICE_LIB) -o $@ $(THREAD_LIBS)

$(E2E_PROCESS): tests/e2e/scenarios/process_supervision.c $(SCENARIO_OBJ) $(SERVICE_LIB) $(CHILD_BIN)
	$(CC) $(BASE_CPPFLAGS) -Itests/e2e/support $(CFLAGS) $< $(SCENARIO_OBJ) $(SERVICE_LIB) -o $@ $(THREAD_LIBS)

$(INTEGRATION_BIN): tests/integration/backend_session.c $(BACKEND_LIB) $(SERVICE_LIB) | require-kernel
	@mkdir -p $(@D)
	$(CC) $(BASE_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(KERNEL_CFLAGS) $(CFLAGS) $< \
	  $(BACKEND_LIB) $(SERVICE_LIB) $(KERNEL_LIBS) $(LUA_LIBS) $(THREAD_LIBS) -o $@

$(INTEGRATION_KERNEL_BIN): tests/integration/kernel_document_workflow.c $(BACKEND_LIB) $(SERVICE_LIB) | require-kernel
	@mkdir -p $(@D)
	$(CC) $(BASE_CPPFLAGS) -Isrc $(PLATFORM_CPPFLAGS) $(KERNEL_CFLAGS) $(CFLAGS) $< \
	  $(BACKEND_LIB) $(SERVICE_LIB) $(KERNEL_LIBS) $(LUA_LIBS) $(THREAD_LIBS) -o $@

$(EXAMPLE_BACKUP_BIN): examples/atomic-backup/main.c $(SERVICE_LIB)
	@mkdir -p $(@D)
	$(CC) $(BASE_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CFLAGS) $< $(SERVICE_LIB) -o $@ $(THREAD_LIBS)

example-atomic-backup: $(EXAMPLE_BACKUP_BIN)
examples: example-atomic-backup

e2e: $(E2E_JOB) $(E2E_ABORT) $(E2E_FS) $(E2E_PROCESS)
	$(E2E_JOB)
	$(E2E_ABORT)
	$(E2E_FS)
	$(E2E_PROCESS) "$(abspath $(CHILD_BIN))"

integration: $(INTEGRATION_BIN) $(INTEGRATION_KERNEL_BIN)
	bash scripts/run-with-kernel-libs.sh "$(INTEGRATION_BIN)"
	bash scripts/run-with-kernel-libs.sh "$(INTEGRATION_KERNEL_BIN)"

# Additional release-readiness checks; not part of the default fast E2E gate.
stress:
	bash scripts/run-resource-stress.sh --seconds 15 --workers 4 --capacity 128 --max-rss-mib 512

benchmark:
	mkdir -p build/stress
	bash scripts/run-resource-stress.sh --seconds 120 --workers 4 --capacity 128 --output build/stress/resource-benchmark.json

runtime-integration:
	bash scripts/run-runtime-integration.sh

test: e2e integration

lint:
	bash scripts/lint-docs.sh

boundary:
	bash scripts/check-boundaries.sh

check: e2e library integration lint boundary
	@if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then git diff --check; else echo "Git worktree not initialized; whitespace diff check skipped"; fi

info:
	@echo "Kernel pkg-config: $(if $(filter yes,$(KERNEL_PKG_OK)),cdin-kernel available,not found)"
	@echo "Kernel prefix: $(KERNEL_PREFIX)"
	@echo "Kernel minimum package version: $(KERNEL_MIN_VERSION)"
	@echo "Lua bridge: $(if $(filter yes,$(LUA_AVAILABLE)),Lua 5.4 development package found,stub build)"
	@echo "Reusable service sources: $(words $(SERVICE_SRC))"
	@echo "Backend host sources: $(words $(BACKEND_SRC))"

clean:
	rm -rf $(BUILD_DIR)

-include $(SERVICE_OBJ:.o=.d) $(BACKEND_OBJ:.o=.d) $(SCENARIO_OBJ:.o=.d)

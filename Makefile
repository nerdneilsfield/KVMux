SHELL := /bin/bash

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifeq ($(UNAME_S),Darwin)
  PLATFORM := macos
else ifeq ($(UNAME_S),Linux)
  PLATFORM := linux
else ifneq (,$(filter MINGW% MSYS% CYGWIN% Windows%,$(UNAME_S)))
  PLATFORM := windows
else
  $(error Unsupported host operating system: $(UNAME_S))
endif

HEADLESS_SUFFIX := $(if $(filter 1,$(HEADLESS)),-headless,)
DEV_PRESET := $(PLATFORM)-debug$(HEADLESS_SUFFIX)
RELEASE_PRESET := $(PLATFORM)-release$(HEADLESS_SUFFIX)
ASAN_PRESET := $(PLATFORM)-sanitizers$(HEADLESS_SUFFIX)
DEV_BUILD_DIR := build/$(DEV_PRESET)

BUILD_JOBS := $(if $(JOBS),--parallel $(JOBS),--parallel)
FIRST_PARTY_DIRS := src tests
SOURCES := $(shell find $(FIRST_PARTY_DIRS) -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' -o -name '*.m' -o -name '*.mm' -o -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' \) 2>/dev/null)

ifeq ($(PLATFORM),macos)
  LLVM_BREW_BIN := $(firstword $(wildcard /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin))
endif
CLANG_FORMAT := $(or $(shell command -v clang-format 2>/dev/null),$(wildcard $(LLVM_BREW_BIN)/clang-format),clang-format)
RUN_CLANG_TIDY := $(or $(shell command -v run-clang-tidy 2>/dev/null),$(wildcard $(LLVM_BREW_BIN)/run-clang-tidy),run-clang-tidy)
CLANG_TIDY := $(or $(shell command -v clang-tidy 2>/dev/null),$(wildcard $(LLVM_BREW_BIN)/clang-tidy),clang-tidy)
CPPCHECK := $(or $(shell command -v cppcheck 2>/dev/null),cppcheck)

CLANG_TIDY_CHECKS := clang-analyzer-*,bugprone-*,performance-*,portability-*,-bugprone-easily-swappable-parameters,-performance-enum-size,-portability-avoid-pragma-once
FIRST_PARTY_SOURCE_FILTER := ^$(CURDIR)/(src|tests)/.*\.(c|cc|cpp|cxx|m|mm)$$
HEADER_FILTER := ^$(CURDIR)/(src|tests)/
EXCLUDE_HEADER_FILTER := ^$(CURDIR)/third_party/

.PHONY: help config-dev build-dev dev config-release build-release release \
        config-asan build-asan asan test format format-check cppcheck clang-tidy lint clean

.DEFAULT_GOAL := help

help:
	@printf '%s\n' \
	  'KVMux developer commands:' \
	  '  make dev             Configure and build Debug' \
	  '  make release         Configure and build Release' \
	  '  make asan            Configure and build with sanitizers (macOS/Linux)' \
	  '  make test            Build Debug and run its tests' \
	  '  make format          Format first-party C/C++/Objective-C++ files' \
	  '  make format-check    Check formatting without changing files' \
	  '  make cppcheck        Run cppcheck using the Debug compile database' \
	  '  make clang-tidy      Run clang-tidy on first-party sources and tests' \
	  '  make lint            Run all static checks' \
	  '  make clean           Remove build outputs' \
	  '' \
	  'Options: HEADLESS=1 selects a headless preset; JOBS=N sets build parallelism.' \
	  'Config-only and build-only forms are also available (config-dev, build-dev, etc.).'

config-dev:
	cmake --preset $(DEV_PRESET)

build-dev:
	cmake --build --preset $(DEV_PRESET) $(BUILD_JOBS)

dev: config-dev build-dev

config-release:
	cmake --preset $(RELEASE_PRESET)

build-release:
	cmake --build --preset $(RELEASE_PRESET) $(BUILD_JOBS)

release: config-release build-release

config-asan:
ifeq ($(PLATFORM),windows)
	@echo 'Error: sanitizer presets are not supported on Windows.' >&2; exit 2
else
	cmake --preset $(ASAN_PRESET)
endif

build-asan:
ifeq ($(PLATFORM),windows)
	@echo 'Error: sanitizer presets are not supported on Windows.' >&2; exit 2
else
	cmake --build --preset $(ASAN_PRESET) $(BUILD_JOBS)
endif

asan: config-asan build-asan

test: dev
	ctest --preset $(DEV_PRESET)

format:
	@command -v "$(CLANG_FORMAT)" >/dev/null || { echo 'Error: clang-format not found.' >&2; exit 127; }
	@$(CLANG_FORMAT) -i $(SOURCES)

format-check:
	@command -v "$(CLANG_FORMAT)" >/dev/null || { echo 'Error: clang-format not found.' >&2; exit 127; }
	@$(CLANG_FORMAT) --dry-run --Werror $(SOURCES)

cppcheck: config-dev
	@command -v "$(CPPCHECK)" >/dev/null || { echo 'Error: cppcheck not found.' >&2; exit 127; }
	$(CPPCHECK) --project=$(DEV_BUILD_DIR)/compile_commands.json -ithird_party \
	  --enable=warning,performance,portability --error-exitcode=1 \
	  --suppressions-list=cppcheck-suppressions.txt --quiet

clang-tidy: config-dev
	@command -v "$(RUN_CLANG_TIDY)" >/dev/null || { echo 'Error: run-clang-tidy not found.' >&2; exit 127; }
	$(RUN_CLANG_TIDY) -clang-tidy-binary '$(CLANG_TIDY)' -p $(DEV_BUILD_DIR) -checks='$(CLANG_TIDY_CHECKS)' \
	  -source-filter='$(FIRST_PARTY_SOURCE_FILTER)' \
	  -header-filter='$(HEADER_FILTER)' -exclude-header-filter='$(EXCLUDE_HEADER_FILTER)'

lint: format-check cppcheck clang-tidy

clean:
	cmake -E rm -rf build

# Routine build and test entry points. Agents with a command allowlist (Antigravity)
# run these as single `make <target>` commands instead of env-prefixed or chained shells.
PY ?= /tmp/orb-w2-tests/bin/python
HUBPATH := hub:hub/packages/orb_agents/src:hub/packages/orb_mcp/src

.PHONY: help sim sim-test emu fw-build hub-test agents-test mcp-test py-test lint host-tests all-tests
EMU_ARGS ?=
help:
	@echo "make sim | sim-test | fw-build | hub-test | agents-test | mcp-test | py-test | lint | host-tests | all-tests"
	@echo "make host-test C=<component>   # firmware/components/<component>/tests/host"
	@echo "make emu [EMU_ARGS='--scale 2']   # interactive firmware shell"

sim:
	cmake -S sim -B sim/build -G Ninja
	cmake --build sim/build

sim-test: sim
	ctest --test-dir sim/build --output-on-failure

emu:
	cmake -S sim -B sim/build -G Ninja -DCMAKE_PREFIX_PATH=/opt/homebrew
	cmake --build sim/build --target orb_emu -j4
	sim/build/orb_emu $(EMU_ARGS)

fw-build:
	scripts/idf.sh build

hub-test:
	PYTHONPATH=$(HUBPATH) $(PY) -m pytest -c hub/pyproject.toml hub/tests -q

agents-test:
	PYTHONPATH=$(HUBPATH) $(PY) -m pytest hub/packages/orb_agents/tests -q

mcp-test:
	PYTHONPATH=$(HUBPATH) $(PY) -m pytest hub/packages/orb_mcp/tests -q

py-test:
	$(PY) -m pytest tests -q

lint:
	python3 scripts/guardrails.py
	/opt/homebrew/bin/ruff check hub scripts devtools tests
	/opt/homebrew/bin/ruff format --check hub scripts devtools tests
	shellcheck scripts/*.sh

host-test:
	$(MAKE) -C firmware/components/$(C)/tests/host test

host-tests:
	@for d in firmware/components/*/tests/host; do echo "== $$d"; $(MAKE) -C $$d test || exit 1; done

all-tests: sim-test host-tests hub-test agents-test mcp-test py-test lint fw-build

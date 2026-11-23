# SPDX-License-Identifier: GPL-3.0-only
# melodee-modules.clap: the plug-in for this machine (make), for the MPC Sample (make mpc: Linux aarch64 in Docker),
# and the host test (make test: the plug-in with tests/fixtures/*.mmod in its Modules folder). make dist puts the MPC
# build in dist/ as "Melodee Modules/Melodee Modules.clap" beside empty Modules and SysEx folders. Synth modules are the
# firmware's build outputs, never in this repository: make fixtures builds the test modules from a Melodee firmware
# checkout (FIRMWARE) with mod_build.py --native, into tests/fixtures (ignored).
CC ?= cc
OUT ?= build/$(shell uname -s)-$(shell uname -m)
NAME = melodee-modules.clap
CFLAGS ?= -O2
MM_CFLAGS = -std=gnu11 -fPIC -fvisibility=hidden -Wall -Wextra -Werror -Isdk -Isrc -Ithird_party/clap
LIBS = -lm
ifeq ($(shell uname -s),Darwin)
SHARED = -dynamiclib
else
SHARED = -shared -Wl,--no-undefined -Wl,-z,defs
endif

FIRMWARE ?= ../Felucca
SRC = src/mm_engine.c src/mm_file.c src/mm_image.c src/mm_plugin.c src/mm_resample.c src/mm_schema.c src/mm_sign.c
OBJ = $(SRC:%.c=$(OUT)/obj/%.o)
FIXTURES = $(wildcard tests/fixtures/*.mmod)

all: $(OUT)/$(NAME)

$(OUT)/obj/src/%.o: src/%.c $(wildcard src/*.h sdk/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MM_CFLAGS) -c $< -o $@

$(OUT)/$(NAME): $(OBJ)
	$(CC) $(SHARED) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OUT)/test_host: tests/test_host.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) $(LDFLAGS) -std=gnu11 -g -Wall -Wextra -Werror -Isdk -Isrc -Ithird_party/clap $< -o $@ -ldl -lm

# a PPG Wave 2.2 factory bank (PPG's, never in this repository): the test compares the PPG module's reading of it with
# its factory programs when it is there
PPG_BANK ?= ../saadet/private/ppg/ppg22-factory.syx

NO_FIXTURES = { echo "no module files in tests/fixtures: make fixtures (FIRMWARE=path of a Melodee firmware checkout)"; exit 1; }

test: $(OUT)/$(NAME) $(OUT)/test_host
	@test -n "$(FIXTURES)" || $(NO_FIXTURES)
	rm -rf $(OUT)/test && mkdir -p $(OUT)/test/Modules
	cp $(OUT)/$(NAME) $(OUT)/test/
	cp $(FIXTURES) $(OUT)/test/Modules/
	MM_PPG_BANK="$(PPG_BANK)" $(OUT)/test_host $(OUT)/test/$(NAME)

# each module file alone in a Modules folder; two staged copies of the plug-in beside a linked Modules folder
test-each: $(OUT)/$(NAME) $(OUT)/test_host
	@test -n "$(FIXTURES)" || $(NO_FIXTURES)
	@for f in $(FIXTURES); do \
		rm -rf $(OUT)/one && mkdir -p $(OUT)/one/Modules && cp $(OUT)/$(NAME) $(OUT)/one/ && cp $$f $(OUT)/one/Modules/ && \
		$(OUT)/test_host --one $(OUT)/one/$(NAME) || exit 1; done

test-staged: test
	rm -rf $(OUT)/stage1 $(OUT)/stage2 && mkdir -p $(OUT)/stage1 $(OUT)/stage2
	cp $(OUT)/$(NAME) $(OUT)/stage1/ && cp $(OUT)/$(NAME) $(OUT)/stage2/
	ln -s $(abspath $(OUT))/test/Modules $(OUT)/stage1/Modules && ln -s $(abspath $(OUT))/test/Modules $(OUT)/stage2/Modules
	$(OUT)/test_host --staged $(OUT)/stage1/$(NAME) $(OUT)/stage2/$(NAME)

# the test modules: the FM-1's files with their AArch64 code (LLVM clang and ld.lld), built outputs kept out of the
# repository (.gitignore); PPG's from the firmware checkout's own PPG data
fixtures:
	cd $(FIRMWARE) && for m in trio wheel; do \
		python3 firmware/modules/sdk/mod_build.py --native build/modules/$$m.mmod -o $(CURDIR)/tests/fixtures/$$m.mmod || exit 1; done
	cd $(FIRMWARE) && for m in hydra ppg; do \
		python3 firmware/modules/sdk/mod_build.py --native build/modules/outside/$$m.mmod --src outside/$$m/$$m.c \
		-o $(CURDIR)/tests/fixtures/$$m.mmod || exit 1; done
	rm -rf tests/fixtures/.mod_native

# Linux aarch64 for the MPC Sample (glibc 2.36, GCC 12: older than the MPC's 2.39 / 13), tested there as on the Mac
mpc:
	docker run --rm --platform linux/arm64 -v "$(CURDIR):/work" -w /work arm64v8/gcc:12-bookworm \
		make OUT=build/mpc-aarch64 all test test-each test-staged

dist: 
	@test -f build/mpc-aarch64/$(NAME) || { echo "make mpc first"; exit 1; }
	rm -rf dist && mkdir -p "dist/Melodee Modules/Modules" "dist/Melodee Modules/SysEx"
	cp build/mpc-aarch64/$(NAME) "dist/Melodee Modules/Melodee Modules.clap"
	cd dist && zip -qr "Melodee Modules.zip" "Melodee Modules"

clean:
	rm -rf build

.PHONY: all test test-each test-staged fixtures mpc dist clean

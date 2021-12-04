# Please see the README for setting up a valid build environment.

# The top-level binary that you wish to produce.
all: beamfrenzy.bin

# All of the source files (.c and .s) that you want to compile.
# You can use relative directories here as well. Note that if
# one of these files is not found, make will complain about a
# missing missing `build/naomi.bin' target, so make sure all of
# these files exist.
SRCS += main.c

# We want a different serial for whatever reason.
SERIAL = BBF0

# Pick up base makefile rules common to all examples.
include ${NAOMI_BASE}/tools/Makefile.base

# Provide a rule to build our ROM FS.
build/romfs.bin: romfs/ ${ROMFSGEN_FILE} ${IMG2BIN_FILE}
	mkdir -p romfs/
	mkdir -p romfs/sprites/
	${IMG2BIN} romfs/sprites/purpleblock assets/sprites/purpleblock.png --depth 16
	${IMG2BIN} romfs/sprites/straightpipe assets/sprites/straightpipe.png --depth 16
	${IMG2BIN} romfs/sprites/cornerpipe assets/sprites/cornerpipe.png --depth 16
	${IMG2BIN} romfs/sprites/source assets/sprites/source.png --depth 16
	${IMG2BIN} romfs/sprites/red assets/sprites/red.png --depth 16
	${IMG2BIN} romfs/sprites/green assets/sprites/green.png --depth 16
	${IMG2BIN} romfs/sprites/blue assets/sprites/blue.png --depth 16
	${IMG2BIN} romfs/sprites/cursor assets/sprites/cursor.png --depth 16
	${ROMFSGEN} $@ romfs/

# Provide the top-level ROM creation target for this binary.
# See scripts/makerom.py for details about what is customizable.
beamfrenzy.bin: ${MAKEROM_FILE} ${NAOMI_BIN_FILE} build/romfs.bin
	${MAKEROM} $@ \
		--title "Beam Frenzy" \
		--publisher "DragonMinded" \
		--serial "${SERIAL}" \
		--section ${NAOMI_BIN_FILE},${START_ADDR} \
		--entrypoint ${MAIN_ADDR} \
		--main-binary-includes-test-binary \
		--test-entrypoint ${TEST_ADDR} \
		--align-before-data 4 \
		--filedata build/romfs.bin

# Include a simple clean target which wipes the build directory
# and kills any binary built.
.PHONY: clean
clean:
	rm -rf build
	rm -rf beamfrenzy.bin

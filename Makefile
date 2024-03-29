# Please see the README for setting up a valid build environment.

# The top-level binary that you wish to produce.
all: beamfrenzy.bin

# All of the source files (.c and .s) that you want to compile.
# You can use relative directories here as well. Note that if
# one of these files is not found, make will complain about a
# missing missing `build/naomi.bin' target, so make sure all of
# these files exist.
SRCS += main.c

# Make sure to link with libxmp for music.
LIBS += -lxmp

# We want a different serial for whatever reason.
SERIAL = BBF9

# Pick up base makefile rules common to all examples.
include ${NAOMI_BASE}/tools/Makefile.base

# Provide a rule to build our ROM FS.
build/romfs.bin: romfs/ ${ROMFSGEN_FILE} ${IMG2BIN_FILE}
	mkdir -p romfs/
	mkdir -p romfs/sprites/
	${IMG2BIN} romfs/sprites/purpleblock assets/sprites/purpleblock.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/brownblock assets/sprites/brownblock.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/blueblock assets/sprites/blueblock.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/greenblock assets/sprites/greenblock.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/orangeblock assets/sprites/orangeblock.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/grayblock assets/sprites/grayblock.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/impossible assets/sprites/impossible.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/source assets/sprites/source.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/red assets/sprites/red.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/green assets/sprites/green.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/blue assets/sprites/blue.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/cyan assets/sprites/cyan.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/magenta assets/sprites/magenta.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/yellow assets/sprites/yellow.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/white assets/sprites/white.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/cursor assets/sprites/cursor.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/straightpipe assets/sprites/straightpipe.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/straightred assets/sprites/straightred.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/straightgreen assets/sprites/straightgreen.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/straightblue assets/sprites/straightblue.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/straightcyan assets/sprites/straightcyan.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/straightmagenta assets/sprites/straightmagenta.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/straightyellow assets/sprites/straightyellow.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/straightwhite assets/sprites/straightwhite.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/cornerpipe assets/sprites/cornerpipe.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/cornerred assets/sprites/cornerred.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/cornergreen assets/sprites/cornergreen.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/cornerblue assets/sprites/cornerblue.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/cornercyan assets/sprites/cornercyan.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/cornermagenta assets/sprites/cornermagenta.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/corneryellow assets/sprites/corneryellow.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/cornerwhite assets/sprites/cornerwhite.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/endred assets/sprites/endred.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/endgreen assets/sprites/endgreen.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/endblue assets/sprites/endblue.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/endcyan assets/sprites/endcyan.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/endmagenta assets/sprites/endmagenta.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/endyellow assets/sprites/endyellow.png --mode RGBA1555
	${IMG2BIN} romfs/sprites/endwhite assets/sprites/endwhite.png --mode RGBA1555
	mkdir -p romfs/sounds/
	cp assets/sounds/activate.raw romfs/sounds/activate
	cp assets/sounds/bad.raw romfs/sounds/bad
	cp assets/sounds/clear.raw romfs/sounds/clear
	cp assets/sounds/drop.raw romfs/sounds/drop
	cp assets/sounds/scroll.raw romfs/sounds/scroll
	mkdir -p romfs/music/
	cp assets/music/ts*.xm romfs/music/
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

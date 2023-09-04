Beam Frenzy
===========

A quick weekend game jam block puzzle game where you connect light between various light sources to clear blocks. Light can be connected between any primary color (red, green, blue) and a matching secondary light source which will net you 5 points per block cleared in this manner. You have the option of connecting two dissimilar light sources as long as one of them contains only a subset of colors of the other. For instance, yellow light is made up of red and green light, so it can be connected to another yellow light, a red light or a green light, or even a white light which contains all three colors. Clearing blocks in this manner gets you 10 points for every block cleared that is a mix of two colors, and 20 points for every block cleared that is a mix of all 3 colors. If you drop blocks that can never be cleared, they will be marked with an X and removed from the playfield, netting you -5 points each. You have five seconds to drop a block at which point it will be placed under the cursor automatically. If there is no room there, it will be dropped randomly on the board. You reach game over when there is no valid place to put a block.

Note that this runs on the SEGA Naomi arcade platform. To play this, download a recent version of Demul which has Naomi support and run `beamfrenzy.bin` or net boot it onto your Naomi and play on target. It supports only one player and uses only one button and one digital joystick. It contains layouts for both horizontal and vertical orientiations so feel free to play it on a cabinet running in either configuration.

To compile, make sure you have a build environment set up as detailed in https://github.com/DragonMinded/libnaomi. Contains no external dependencies aside from what 3rd party libraries are compiled with libnaomi.

Credits
=======

Original game idea and all code done by DragonMinded. Toolchain work built and distributed by DragonMinded. Sound effects found on various free sites and chopped up. Graphics made in mspaint by DragonMinded. Music is the excellent Toilet Story series by Ghidora playing using libxmp. You are free to download, compile, play, remix or redistribute the binary or source code for non-commercial purposes only! No warranty is expressed or implied by this repo or any of the code or binaries within it.

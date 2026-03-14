# libdualsense

an open source, cross-platform library for using Dualsense controller features.

there is currently very little documentation, this project is very early in development.

This code is based off of dualsensectl, check it out here: https://github.com/nowrep/dualsensectl

# compiling

note that all compilation has only been tested on Linux, so UNIX-like commands will be used below.

linux:

`gcc -c -O2 libdualsense.c` `ar -crs libdualsense.a libdualsense.o`

windows:

(get hidapi headers and place in the repo root)

`x86-64-w64-mingw32-gcc -c -02 libdualsense.c -I.` `x86-64-w64-mingw32-ar -crs libdualsense.lib libdualsense.o`

The resulting file works just the same as any other old windows static library, also note you'll have to link your app with hidapi.dll and include it with the distribution of your executable.

in theory, the code should work on macOS, i just don't have a mac.

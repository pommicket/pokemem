# pokemem

A (Linux-only) tool for locating and modifying values in memory.

## Installation

You can get a .deb file from https://github.com/pommicket/pokemem/releases

## How to use

Disclaimer: This might not always work! If it doesn't, you might have gotten the wrong
data type, or it could be stored in a special way, or it could have
moved around in memory during the search. It's possible that there's nothing
you can do (with this tool at least) to pin down the location of the value.

1. Find the PID of the process you're interested in with a task manager,
or `pidof <process_name>`. This should be some number, like 18035.
2. Select a data type. If you're not sure what data type to pick:
For websites, it's usually (but not always)
64-bit floating-point that you want, and for non-web applications,
it's probably 32-bit signed integer.
3. Make sure that the value is in memory (i.e. have whatever
tab you need open, etc.).
4. Click "Begin search"
5. Enter the current value, and click update/press enter. Repeat until the number of
candidates isn't going down any further (it should be probably between 1 and 20).
6. (Optional) Turn on auto-refresh, change stuff around,
and watch to see if you've got the right value.
7. Either double click on a value to change it, or use the box
at the bottom to change all candidates at once.
8. If you want to do another search, click "Stop", then "Begin search" again.

## Compiling from source

Run `make` for a debug build, and `make release` for a release build,
or `make pokemem.deb` to build the .deb file.

## Report a bug

Bugs can be reported to pommicket at pommicket.com

## License

```
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org/>
```

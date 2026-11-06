# SRL

Tweak a C program's globals without recompiling. Mark variables with the macros in `include/srl.h`, then open the binary in `srl_editor`: edit values, run it inside the editor, and change them live while it runs.

```c
#include "srl.h"

SRL_INT_RANGE(g_speed, 5, 1, 10);
SRL_FLOAT(g_height, 3.5f);
SRL_BOOL(g_god_mode, false);
SRL_STR(g_name, "Pilot");
```

## Build and run

Needs gcc and ncurses (`sudo apt install libncurses-dev`).

```sh
make
./srl_editor examples/dodger
```

Keys: `Enter` edit, `r` run, `Ctrl-]` back from the program, `u` undo, `w` save a patched copy (`*.pat`), `q` quit.

## How it works

- Each macro adds a `NAME_SRL_<TYPE>` pointer; the editor finds it in the ELF symbol table and follows it to the variable. `NAME_SRL_RANGE_MIN/MAX` set limits.
- Before a run, the patched binary is built in memory (`memfd`) and run from there; the file on disk is never changed.
- While it runs, values are read and written in the process's memory (`process_vm_readv/writev`).
- Build programs with `-O0` (or mark variables `volatile`) so live edits are seen.

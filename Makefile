CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra

CORE = src/srl_core.c src/srl_core.h

all: srl_editor examples

# terminal UI (default) - needs only ncursesw
srl_editor: src/srl_tui.c src/vt.c src/vt.h $(CORE)
	$(CC) $(CFLAGS) -o $@ src/srl_tui.c src/vt.c src/srl_core.c -lncursesw -lutil -lm

# optional GTK3 + VTE front end (unmaintained)
srl_editor_gtk: src/srl_gui.c $(CORE)
	$(CC) $(CFLAGS) $$(pkg-config --cflags gtk+-3.0 vte-2.91) -o $@ src/srl_gui.c src/srl_core.c \
		$$(pkg-config --libs gtk+-3.0 vte-2.91) -lutil -lm

# examples: -O0 so the globals stay real memory reads (needed for live edits)
examples: examples/main examples/dodger

examples/main: examples/main.c include/srl.h
	$(CC) -O0 -g -Wall -Wextra -Iinclude -o $@ $<

examples/dodger: examples/dodger.c include/srl.h
	$(CC) -O0 -g -Wall -Wextra -Iinclude -o $@ $< -lncurses

clean:
	rm -f srl_editor srl_editor_gtk examples/main examples/dodger

.PHONY: all examples clean

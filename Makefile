MAIN = dwmpoll

THERMAL_ZONE_GREP = \
  grep -l x86_pkg_temp /sys/class/thermal/thermal_zone*/type \
  | sed 's,/type$$,/temp,'
THERMAL_ZONE = $(shell $(THERMAL_ZONE_GREP))

.PHONY: all clean install

all: $(MAIN)

$(MAIN): %: %.c
	$(CC) -Wall -Os -DTHERMAL_ZONE='$(THERMAL_ZONE)' $< -o $@ -lX11 -lxkbfile -lasound

clean:
	@rm -fv $(MAIN)

install: $(MAIN)
	@cp -v $(MAIN) $(HOME)/.local/bin

uninstall:
	@rm -fv $(HOME)/.local/bin/$(MAIN)


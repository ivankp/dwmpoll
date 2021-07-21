MAIN = dwmpoll

THERMAL_ZONE_GREP = \
  grep -l x86_pkg_temp /sys/class/thermal/thermal_zone*/type \
  | sed 's,/type$$,/temp,'
THERMAL_ZONE = $(shell $(THERMAL_ZONE_GREP))

all: $(MAIN)

$(MAIN): %: %.c
	gcc -Wall -g -pthread -DTHERMAL_ZONE='$(THERMAL_ZONE)' $< -o $@ -lX11 -lxkbfile -lasound

clean:
	@rm -fv $(MAIN)

.PHONY: all clean

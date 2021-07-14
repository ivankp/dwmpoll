MAIN = dwmpoll

all: $(MAIN)

$(MAIN): %: %.c
	gcc -Wall -O3 $< -o $@ -lX11

clean:
	@rm -fv $(MAIN)

.PHONY: all clean

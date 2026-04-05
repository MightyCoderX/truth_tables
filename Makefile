CC := gcc
CFLAGS := -std=c99 -Wall -Wextra -pedantic
LIBS := -ldl -lreadline -lm

main: main.c
	${CC} ${CFLAGS} ${LIBS} $^ -o $@

run: main
	./main

debug: main
	gdb ./main

clean: main
	rm main


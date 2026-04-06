CC := gcc
CFLAGS := -std=c99 -Wall -Wextra -fsanitize=undefined

LIBS := -ldl -lm
EXE_NAME := ttable

${EXE_NAME}: main.c
	${CC} ${CFLAGS} ${LIBS} $^ -o $@

run: ${EXE_NAME}
	./${EXE_NAME}

debug:
	${CC} ${CFLAGS} ${LIBS} $^ -o ${EXE_NAME} -ggdb
	gdb ./ttable

clean: main
	rm main


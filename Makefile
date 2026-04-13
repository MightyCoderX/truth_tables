CC := gcc
# TODO: Remove -fsanitize in release builds
CFLAGS := -std=c99 -Wall -Wextra -fsanitize=undefined

LIBS := -ldl -lm
EXE_NAME := ttable

${EXE_NAME}: main.c
	${CC} ${CFLAGS} ${LIBS} $^ -o $@

run: ${EXE_NAME}
	./${EXE_NAME}

debug:
	${CC} ${CFLAGS} ${LIBS} main.c -o ${EXE_NAME} -ggdb -DKEEP_FILES=1
	gdb ./ttable

clean: main
	rm main


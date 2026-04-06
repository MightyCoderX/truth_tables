#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

// TODO:Consider using arenas for allocation

#define ERROR(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define PARSE_ERROR(lineno, fmt, ...) ERROR("ERROR: line %d: " fmt, (lineno), ##__VA_ARGS__)

typedef int (*bool_func_t)(int* inputs);

int* bits_from_int(int num, int n_bits) {
    int* bits = malloc(sizeof(*bits) * n_bits);
    for(int i = 0; i < n_bits; i++) {
        bits[i] = (num >> (n_bits - 1 - i)) & 1;
    }

    return bits;
}
typedef struct {
    size_t size;
    size_t capacity;
    char* chars;
} ChrVec;

ChrVec* chrvec_new() {
    ChrVec* vec = malloc(sizeof(*vec));
    vec->size = 0;
    vec->capacity = 1;
    vec->chars = malloc(vec->capacity);
    assert(vec->chars != NULL && "Buy more RAM");

    return vec;
}

void chrvec_extend(ChrVec* vec) {
    vec->capacity = ceil((float)vec->capacity * 1.5);
    vec->chars = realloc(vec->chars, vec->capacity);
    assert(vec->chars != NULL && "Buy more RAM");
}

void chrvec_append(ChrVec* vec, char c) {
    if(vec->size >= vec->capacity) {
        chrvec_extend(vec);
    }

    vec->chars[vec->size] = c;
    vec->size++;
}

void chrvec_cat(ChrVec* vec, const char* str) {
    int len = strlen(str);
    if(vec->size + len >= vec->capacity) {
        chrvec_extend(vec);
    }
    strcat(vec->chars, str);
    vec->size += len;
}

bool chrvec_contains(ChrVec* vec, char needle) {
    for(size_t i = 0; i < vec->size; i++) {
        if(vec->chars[i] == needle) {
            return true;
        }
    }
    return false;
}

typedef struct {
    size_t size;
    size_t capacity;
    char** strings;
} StringVec;

StringVec* strvec_new() {
    StringVec* vec = malloc(sizeof(*vec));
    vec->size = 0;
    vec->capacity = 10;
    vec->strings = malloc(sizeof(char*) * vec->capacity);
    assert(vec->strings != NULL && "Buy more RAM");

    return vec;
}

void strvec_append(StringVec* vec, const char* str) {
    if(vec->size >= vec->capacity) {
        vec->capacity *= 1.5;
        vec->strings = realloc(vec->strings, vec->capacity * sizeof(char*));
        assert(vec->strings != NULL && "Buy more RAM");
    }

    vec->strings[vec->size] = malloc(strlen(str));
    strcpy(vec->strings[vec->size], str);
    vec->size++;
}

// TODO: Fix lexing and parsing to reduce branching and allow for sorrounding with parentheses
// the current sub-expression when needed (see ANDs added automatically when two variables
// are unpacked from same token (eg. `ab` -> `a && b` should be `(a && b)` instead))
ssize_t parse_expr_file(FILE* expr_file, ChrVec* out_inputs, StringVec* out_exprs, StringVec* out_outputs) {
    ChrVec* buf = chrvec_new();
    int stmt_num = 0;
    int lineno = 1;
    char c;
    while((c = fgetc(expr_file)) != EOF) {
        if(c == '\n') {
            lineno++;
            continue;
        }

        if(c == '#') {
            char nc;
            while((nc = fgetc(expr_file)) != '\n')
                ;
            fseek(expr_file, -1, SEEK_CUR);
        }

        if(stmt_num == 0) { // inputs
            if(isalpha(c)) {
                chrvec_append(out_inputs, c);
            } else if(c == ',' || c == ';' || c == ' ') {
                if(c == ';') stmt_num++;
                continue;
            } else {
                PARSE_ERROR(lineno, "invalid character '%c' expected comma separated input names\n", c);
                return -1;
            }
        } else { // expressions
            if(isalpha(c)) {
                chrvec_append(buf, c);
            } else if(isalnum(c)) {
                if(buf->size > 0 && isalpha(buf->chars[buf->size - 1])) {
                    chrvec_append(buf, c);
                } else {
                    PARSE_ERROR(lineno, "invalid character '%c' identifier must begin with a letter\n", c);
                    return -1;
                }
            } else if(c == '=') {
                strvec_append(out_outputs, buf->chars);
                buf = chrvec_new();
                ChrVec* c_expr = chrvec_new();
                while((c = fgetc(expr_file)) != ';') {
                    if(c == EOF) {
                        PARSE_ERROR(lineno, "missing ';'\n");
                        return -1;
                    }

                    // parse expression a
                    if(isalpha(c)) {
                        if(!chrvec_contains(out_inputs, c)) {
                            PARSE_ERROR(lineno, "undefined input %c\n", c);
                            return -1;
                        }
                        chrvec_append(c_expr, c);
                        char nc = fgetc(expr_file);
                        if(isalpha(nc) || nc == '!' || nc == '(') {
                            if(nc == '(') {
                                fseek(expr_file, -1, SEEK_CUR);
                            }
                            chrvec_cat(c_expr, " && ");
                            if(nc == '!') {
                                nc = fgetc(expr_file);
                                chrvec_append(c_expr, '!');
                            }
                            chrvec_append(c_expr, nc);
                        } else {
                            fseek(expr_file, -1, SEEK_CUR);
                        }
                    } else if(c
                        == '+') { // TODO:fix this, someone is eating this char or other logic problem only happens when reading from stdin or pipe
                        chrvec_cat(c_expr, " || ");
                    } else if(c
                        == '*') { // TODO:fix this, someone is eating this char or other logic problem only happens when reading from stdin or pipe
                        chrvec_cat(c_expr, " && ");
                    } else if(c == '(' || c == ')' || c == '!') {
                        if(c == ')') {
                            char nc = fgetc(expr_file);
                            if(nc == '(') {
                                chrvec_cat(c_expr, ") && (");
                            } else if(nc == '!') {
                                ChrVec* tmp = chrvec_new();
                                chrvec_cat(tmp, ") && ");
                                do {
                                    chrvec_append(tmp, nc);
                                } while((nc = fgetc(expr_file)) == '!');

                                if(nc == '(') {
                                    chrvec_append(tmp, nc);
                                    chrvec_cat(c_expr, tmp->chars);
                                    free(tmp->chars);
                                } else {
                                    fseek(expr_file, -1, SEEK_CUR);
                                }
                            } else if(isalpha(nc)) {
                                chrvec_cat(c_expr, ") && ");
                                fseek(expr_file, -1, SEEK_CUR);
                            } else {
                                fseek(expr_file, -1, SEEK_CUR);
                                chrvec_append(c_expr, c);
                            }
                        } else {
                            chrvec_append(c_expr, c);
                        }
                    }
                }
                stmt_num++;

                strvec_append(out_exprs, c_expr->chars);
            }
        }
    }

    return 0;
}

int main(int argc, char** argv) {
    if(argc > 2) {
        ERROR("usage: %s [expr_file]\n", argv[0]);
        return 1;
    }

    // TODO: Map whole file to memory before parsing, to allow seeking
    // (change signature of `parse_expr_file` function to accept pointer to bytes)
    FILE* expr_file;

    if(argc == 2) {
        expr_file = fopen(argv[1], "r");
    } else {
        expr_file = stdin;
    }

    if(expr_file == NULL) {
        perror("fopen");
        return 1;
    }

    ChrVec* inputs = chrvec_new();
    StringVec* exprs = strvec_new();
    StringVec* outputs = strvec_new();

    ssize_t err = parse_expr_file(expr_file, inputs, exprs, outputs);
    if(err == -1) {
        return 1;
    }

    printf("Inputs:\n    ");
    for(size_t i = 0; i < inputs->size; i++) {
        printf("%c ", inputs->chars[i]);
    }
    printf("\n");

    printf("Functions:\n");
    for(size_t i = 0; i < exprs->size; i++) {
        printf("    %s = %s\n", outputs->strings[i], exprs->strings[i]);
    }
    printf("\n");

    FILE* funcs_file = fopen("funcs.c", "w");
    fprintf(funcs_file, "#include <stddef.h>\n");
    fprintf(funcs_file, "\n");

    StringVec* func_names = strvec_new();

    for(size_t i = 0; i < exprs->size; i++) {
        char* func = malloc(22);

        sprintf(func, "f%zu", i);

        strvec_append(func_names, func);

        fprintf(funcs_file, "int %s(int* inputs, size_t len) {\n", func);
        for(size_t j = 0; j < inputs->size; j++) {
            fprintf(funcs_file, "    int %c = inputs[%zu];\n", inputs->chars[j], j);
        }
        fprintf(funcs_file, "    return %s;\n}\n", exprs->strings[i]);
        fprintf(funcs_file, "\n");
    }
    fclose(funcs_file);

    // TODO:Compile functions all in memory using `gcc - -o -` and mmap to load the output
    // into an executable memory page

    // Compile .c file with functions to .so
    pid_t child = fork();
    if(child == -1) {
        perror("fork");
        return 1;
    }
    if(child == 0) {
        int err = execlp("gcc", "gcc", //
            //"-Wall",
            "-o", //
            "funcs.so", //
            "funcs.c", //
            "-shared", //
            NULL //
        );
        if(err == -1) {
            perror("execlp");
        }
        return 1;
    }
    err = waitpid(child, NULL, 0);
    if(err == -1) {
        perror("wait");
        return 1;
    }

    // Open file with dlopen
    void* handle = dlopen("./funcs.so", RTLD_NOW);
    if(handle == NULL) {
        ERROR("dlopen failed: %s\n", dlerror());
        return 1;
    }

    // Load all functions
    bool_func_t* funcs = malloc(exprs->size * sizeof(*funcs));
    for(size_t i = 0; i < exprs->size; i++) {
        funcs[i] = dlsym(handle, func_names->strings[i]);
        if(*funcs[i] == NULL) {
            ERROR("error when getting function %s: %s\n", func_names->strings[i], dlerror());
            return 1;
        }
    }

    /* Print table */

    // print header
    for(size_t i = 0; i < inputs->size; i++) {
        printf("%c ", inputs->chars[i]);
    }
    printf("|");
    for(size_t i = 0; i < exprs->size; i++) {
        printf(" %s", outputs->strings[i]);
    }
    printf("\n");

    // print values
    for(size_t i = 0; i < pow(2, inputs->size); i++) {
        int* bits = bits_from_int(i, inputs->size);
        for(size_t j = 0; j < inputs->size; j++) {
            printf("%d ", bits[j]);
        }
        printf("|");
        for(size_t j = 0; j < exprs->size; j++) {
            printf(" %d ", (funcs[j])(bits));
        }
        printf("\n");
    }
    return 0;
}

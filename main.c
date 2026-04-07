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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>

// TODO: Consider using arenas for allocation

#define ERROR(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define PARSE_ERROR(loc, fmt, ...)                                                                                     \
    ERROR("PARSING ERROR: at %s:%zu:%zu: " fmt, (loc.filename), (loc.lineno), (loc.colno), ##__VA_ARGS__)
#define PANIC(fmt, ...)                                                                                                \
    ERROR("PANIC: %s():%d: " fmt, __func__, __LINE__, ##__VA_ARGS__);                                                  \
    abort();

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
    vec->capacity = 10;
    vec->chars = malloc(vec->capacity);
    assert(vec->chars != NULL && "Buy more RAM");

    return vec;
}

void chrvec_extend(ChrVec* vec, size_t amount) {
    vec->capacity += amount;
    vec->capacity = ceil((float)vec->capacity * 1.5);
    vec->chars = realloc(vec->chars, vec->capacity);
    assert(vec->chars != NULL && "Buy more RAM");
}

void chrvec_append(ChrVec* vec, char c) {
    if(vec->size >= vec->capacity) {
        chrvec_extend(vec, 0);
    }

    vec->chars[vec->size] = c;
    vec->size++;
}

void chrvec_cat(ChrVec* vec, const char* str) {
    int len = strlen(str);
    if(vec->size + len >= vec->capacity) {
        chrvec_extend(vec, len);
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
    vec->strings[vec->size] = malloc(strlen(str) + 1);
    strcpy(vec->strings[vec->size], str);
    vec->size++;
}

typedef struct {
    char* filename;
    char* bytes;
    size_t size;
    size_t cur;
} FileBuf;

FileBuf* fbuf_new(const char* filename, char* bytes, size_t size) {
    FileBuf* fbuf = malloc(sizeof(*fbuf));
    assert(fbuf != NULL && "Buy more RAM");
    fbuf->filename = malloc(strlen(filename) + 1);
    assert(fbuf->filename != NULL && "Buy more RAM");
    strcpy(fbuf->filename, filename);
    fbuf->bytes = bytes;
    fbuf->size = size;
    fbuf->cur = 0;

    return fbuf;
}

void fbuf_rseek(FileBuf* fbuf, long amount) {
    long idx = fbuf->cur + amount;
    if(idx < 0 || idx >= (long)fbuf->size) {
        PANIC("index %ld out of bounds [0,%zu]\n", idx, fbuf->size);
    }
    fbuf->cur += amount;
}

char fbuf_getc(FileBuf* fbuf, long idx) {
    if(idx < 0 || idx >= (long)fbuf->size) {
        PANIC("index %ld out of bounds [0,%zu]\n", idx, fbuf->size);
    }
    return fbuf->bytes[idx];
}

char fbuf_nextc(FileBuf* fbuf) {
    if(fbuf->cur >= fbuf->size) {
        return EOF;
    }

    return fbuf->bytes[fbuf->cur++];
}

typedef struct {
    const char* filename;
    size_t lineno;
    size_t colno;
} Location;

// TODO: Fix lexing and parsing to reduce branching and allow for sorrounding with parentheses
// the current sub-expression when needed (see ANDs added automatically when two variables
// are unpacked from same token (eg. `ab` -> `a && b` should be `(a && b)` instead))
void parse_expr_file(FileBuf* fbuf, ChrVec** out_inputs, StringVec** out_exprs, StringVec** out_outputs) {
    *out_inputs = chrvec_new();
    *out_exprs = strvec_new();
    *out_outputs = strvec_new();

    ChrVec* tokbuf = chrvec_new();
    int stmt_num = 0;
    Location loc = {
        .filename = fbuf->filename,
        .lineno = 1,
        .colno = 0,
    };
    char c;
    while((c = fbuf_nextc(fbuf)) != EOF) {
        if(c == '\n') {
            loc.colno = 0;
            loc.lineno++;
            continue;
        }

        loc.colno++;

        if(c == '#') {
            char nc;
            while((nc = fbuf_nextc(fbuf)) != '\n')
                ;
            fbuf_rseek(fbuf, -1); // don't eat newline
            continue;
        }

        if(stmt_num == 0) { // inputs
            if(isalpha(c)) {
                chrvec_append(*out_inputs, c);
            } else if(c == ',' || c == ';' || c == ' ') {
                if(c == ';') stmt_num++;
                continue;
            } else {
                PARSE_ERROR(loc, "invalid character '%c' expected comma separated input names\n", c);
                exit(1);
            }
        } else { // expressions
            if(isalpha(c)) {
                chrvec_append(tokbuf, c);
            } else if(isalnum(c)) {
                if(tokbuf->size > 0 && isalpha(tokbuf->chars[tokbuf->size - 1])) {
                    chrvec_append(tokbuf, c);
                } else {
                    PARSE_ERROR(loc, "invalid character '%c' identifier must begin with a letter\n", c);
                    exit(1);
                }
            } else if(c == '=') {
                strvec_append(*out_outputs, tokbuf->chars);
                tokbuf = chrvec_new();
                ChrVec* c_expr = chrvec_new();
                while((c = fbuf_nextc(fbuf)) != ';') {
                    if(c == EOF) {
                        PARSE_ERROR(loc, "missing ';'\n");
                        exit(1);
                    }

                    // parse expression a
                    if(isalpha(c)) {
                        if(!chrvec_contains(*out_inputs, c)) {
                            PARSE_ERROR(loc, "undefined input %c\n", c);
                            exit(1);
                        }
                        chrvec_append(c_expr, c);
                        char nc = fbuf_nextc(fbuf);
                        if(isalpha(nc) || nc == '!' || nc == '(') {
                            chrvec_cat(c_expr, " && ");
                            if(nc == '!') {
                                nc = fbuf_nextc(fbuf);
                                chrvec_append(c_expr, '!');
                            }
                            chrvec_append(c_expr, nc);
                        } else if(nc == ' ') {
                            // do nothing
                        } else {
                            fbuf_rseek(fbuf, -1);
                        }
                    } else if(c == '+') {
                        chrvec_cat(c_expr, " || ");
                    } else if(c == '*') {
                        chrvec_cat(c_expr, " && ");
                    } else if(c == '(' || c == ')' || c == '!') {
                        if(c == ')') {
                            char nc = fbuf_nextc(fbuf);
                            if(nc == '(') {
                                chrvec_cat(c_expr, ") && (");
                            } else if(nc == '!') {
                                ChrVec* tmp = chrvec_new();
                                chrvec_cat(tmp, ") && ");
                                do {
                                    chrvec_append(tmp, nc);
                                } while((nc = fbuf_nextc(fbuf)) == '!');

                                if(nc == '(') {
                                    chrvec_append(tmp, nc);
                                    chrvec_cat(c_expr, tmp->chars);
                                    free(tmp->chars);
                                } else {
                                    fbuf_rseek(fbuf, -1);
                                }
                            } else if(isalpha(nc)) {
                                chrvec_cat(c_expr, ") && ");
                                fbuf_rseek(fbuf, -1);
                            } else {
                                fbuf_rseek(fbuf, -1);
                                chrvec_append(c_expr, c);
                            }
                        } else {
                            chrvec_append(c_expr, c);
                        }
                    }
                }
                stmt_num++;

                strvec_append(*out_exprs, c_expr->chars);
            }
        }
    }

    printf("Inputs:\n    ");
    for(size_t i = 0; i < (*out_inputs)->size; i++) {
        printf("%c ", (*out_inputs)->chars[i]);
    }
    printf("\n");

    printf("Functions:\n");
    for(size_t i = 0; i < (*out_exprs)->size; i++) {
        printf("    %s = %s\n", (*out_outputs)->strings[i], (*out_exprs)->strings[i]);
    }
    printf("\n");
}

void read_expr_file(const char* filename, FILE* file, FileBuf** fbuf) {
    ChrVec* bytes = chrvec_new();

    size_t size = 0;
    char c;
    while((c = fgetc(file)) != EOF) {
        chrvec_append(bytes, c);
        size++;
    }

    printf("size: %zu\n", size);

    *fbuf = fbuf_new(filename, bytes->chars, size);
    if(fbuf == NULL) {
        exit(1);
    }
}

typedef struct {
    char* expr_filename;
    FILE* expr_file;
} Args;

void parse_args(int argc, char** argv, Args* out_args) {
    if(argc > 2) {
        ERROR("usage: %s [expr_file]\n", argv[0]);
        exit(1);
    }

    // TODO: Map whole file to memory before parsing, to allow seeking
    // (change signature of `parse_expr_file` function to accept pointer to bytes)

    FILE* expr_file = stdin;
    char* filename = "stdin";

    if(argc == 2) {
        filename = argv[1];
        expr_file = fopen(argv[1], "r");
    }

    if(expr_file == NULL) {
        perror("fopen");
        exit(1);
    }

    out_args->expr_filename = filename;
    out_args->expr_file = expr_file;
}

void generate_c_file(ChrVec* inputs, StringVec* exprs, StringVec** func_names) {
    *func_names = strvec_new();
    FILE* funcs_file = fopen("funcs.c", "w");

    if(funcs_file == NULL) {
        perror("fopen");
        exit(1);
    }

    fprintf(funcs_file, "#include <stddef.h>\n");
    fprintf(funcs_file, "\n");

    for(size_t i = 0; i < exprs->size; i++) {
        ChrVec* func = chrvec_new();

        sprintf(func->chars, "f%zu", i);

        strvec_append(*func_names, func->chars);

        fprintf(funcs_file, "int %s(int* inputs) {\n", func->chars);
        for(size_t j = 0; j < inputs->size; j++) {
            fprintf(funcs_file, "    int %c = inputs[%zu];\n", inputs->chars[j], j);
        }
        fprintf(funcs_file, "    return %s;\n}\n", exprs->strings[i]);
        fprintf(funcs_file, "\n");

        free(func->chars);
    }
    fclose(funcs_file);
}

void compile_c_to_so() {
    // TODO: Compile functions all in memory using `gcc - -o -` and mmap to load the output
    // into an executable memory page

    pid_t child = fork();
    if(child == -1) {
        perror("fork");
        exit(1);
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
        exit(1);
    }
    int err = waitpid(child, NULL, 0);
    if(err == -1) {
        perror("wait");
        exit(1);
    }
    unlink("./funcs.c");
}

void load_funcs_from_so(StringVec* exprs, StringVec* func_names, bool_func_t** funcs) {
    *funcs = malloc(exprs->size * sizeof(**funcs));
    // Open file with dlopen
    void* handle = dlopen("./funcs.so", RTLD_NOW);
    if(handle == NULL) {
        ERROR("dlopen failed: %s\n", dlerror());
        exit(1);
    }

    // Load all functions
    for(size_t i = 0; i < exprs->size; i++) {
        (*funcs)[i] = dlsym(handle, func_names->strings[i]);
        if(funcs[i] == NULL) {
            ERROR("error when getting function %s: %s\n", func_names->strings[i], dlerror());
            exit(1);
        }
    }
    unlink("./funcs.so");
}

void generate_truth_table(ChrVec* inputs, StringVec* outputs, bool_func_t* funcs) {
    // print header
    for(size_t i = 0; i < inputs->size; i++) {
        printf("%c ", inputs->chars[i]);
    }
    printf("|");
    for(size_t i = 0; i < outputs->size; i++) {
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
        for(size_t j = 0; j < outputs->size; j++) {
            printf(" %d ", (funcs[j])(bits));
        }
        printf("\n");
    }
}

int main(int argc, char** argv) {
    Args args = { 0 };
    FileBuf* fbuf;
    ChrVec* inputs;
    StringVec *exprs, *outputs;
    StringVec* func_names;
    bool_func_t* funcs;
    // TODO: DON'T exit() in functions, handle errors outside using macros for error codes
    // TODO: Pull allocations out of functions

    parse_args(argc, argv, &args);

    read_expr_file(args.expr_filename, args.expr_file, &fbuf);
    parse_expr_file(fbuf, &inputs, &exprs, &outputs);

    generate_c_file(inputs, exprs, &func_names);
    compile_c_to_so();

    load_funcs_from_so(exprs, func_names, &funcs);
    generate_truth_table(inputs, outputs, funcs);

    return 0;
}

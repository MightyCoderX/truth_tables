#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

// TODO: Consider using arenas for allocation

#define ERROR(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define PARSE_ERROR(loc, fmt, ...)                                                                                     \
    ERROR("PARSING ERROR: at %s:%zu:%zu: " fmt, ((loc)->filename), ((loc)->lineno), ((loc)->colno), ##__VA_ARGS__)
#define PANIC(fmt, ...)                                                                                                \
    do {                                                                                                               \
        ERROR("PANIC: %s():%d: " fmt, __func__, __LINE__, ##__VA_ARGS__);                                              \
        abort();                                                                                                       \
    } while(0);

typedef int(bool_func_t)(int* inputs);

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

ChrVec chrvec_new() {
    ChrVec vec = {
        .size = 0,
        .capacity = 10,
        .chars = NULL,
    };
    vec.chars = malloc(vec.capacity);
    assert(vec.chars != NULL && "Buy more RAM");

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
        chrvec_extend(vec, 1);
    }

    vec->chars[vec->size++] = c;
    vec->chars[vec->size + 1] = '\0';
}

void chrvec_cat(ChrVec* vec, const char* str) {
    int len = strlen(str);
    if(vec->size + len >= vec->capacity) {
        chrvec_extend(vec, len + 1);
    }
    memcpy(vec->chars + vec->size, str, len);
    vec->size += len;
    vec->chars[vec->size + 1] = '\0';
}

bool chrvec_contains(ChrVec* vec, char needle) {
    for(size_t i = 0; i < vec->size; i++) {
        if(vec->chars[i] == needle) {
            return true;
        }
    }
    return false;
}

char chrvec_get(ChrVec* vec, long idx) {
    size_t index = idx;

    if(idx < 0L) {
        index = vec->size + idx;
    }

    if(index >= vec->size) {
        PANIC("index %ld out of bounds [0,%zu]\n", idx, vec->size);
    }

    return vec->chars[index];
}

typedef struct {
    size_t size;
    size_t capacity;
    char** strings;
} StringVec;

StringVec strvec_new() {
    StringVec vec = {
        .size = 0,
        .capacity = 10,
    };
    vec.strings = malloc(sizeof(char*) * vec.capacity);
    assert(vec.strings != NULL && "Buy more RAM");

    return vec;
}

void strvec_append(StringVec* vec, const char* str) {
    if(vec->size >= vec->capacity) {
        vec->capacity *= 1.5;
        vec->strings = realloc(vec->strings, vec->capacity * sizeof(char*));
        assert(vec->strings != NULL && "Buy more RAM");
    }
    size_t len = strlen(str);
    vec->strings[vec->size] = malloc(len + 1);
    memcpy(vec->strings[vec->size], str, len);
    vec->strings[vec->size][len] = '\0';
    vec->size++;
}

typedef struct {
    char* filename;
    char* bytes;
    size_t len;
    size_t cur;
} FileBuf;

FileBuf fbuf_new(const char* filename, char* bytes, size_t len) {
    FileBuf fbuf;

    fbuf.filename = malloc(strlen(filename) + 1);
    assert(fbuf.filename != NULL && "Buy more RAM");
    strcpy(fbuf.filename, filename);
    fbuf.bytes = bytes;
    fbuf.len = len;
    fbuf.cur = 0;

    return fbuf;
}

void fbuf_rseek(FileBuf* fbuf, long amount) {
    long idx = fbuf->cur + amount;
    if(idx < 0 || idx >= (long)fbuf->len) {
        PANIC("index %ld out of bounds [0,%zu]\n", idx, fbuf->len);
    }
    fbuf->cur += amount;
}

char fbuf_getc(FileBuf* fbuf, long idx) {
    if(idx < 0 || idx >= (long)fbuf->len) {
        PANIC("index %ld out of bounds [0,%zu]\n", idx, fbuf->len);
    }
    return fbuf->bytes[idx];
}

char fbuf_nextc(FileBuf* fbuf) {
    if(fbuf->cur >= fbuf->len) {
        return EOF;
    }

    return fbuf->bytes[fbuf->cur++];
}

typedef struct {
    const char* filename;
    size_t lineno;
    size_t colno;
} Location;

typedef enum {
    INPUTS,
    OUTPUTS,
} ParseState;

void parse_expression(
    FileBuf* fbuf, ChrVec* tokbuf, Location* loc, ChrVec* inputs, StringVec* out_outputs, StringVec* out_exprs) {

    char c = fbuf->bytes[fbuf->cur - 1];

    if(isalpha(c)) {
        chrvec_append(tokbuf, c);
    } else if(isalnum(c) || c == '_') {
        if(tokbuf->size > 0 && isalpha(chrvec_get(tokbuf, -1))) {
            chrvec_append(tokbuf, c);
        } else {
            PARSE_ERROR(loc, "invalid character '%c' identifier must begin with a letter\n", c);
            exit(1);
        }
    } else if(c == '=') {
        strvec_append(out_outputs, tokbuf->chars);
        *tokbuf = chrvec_new();
        ChrVec c_expr = chrvec_new();
        while((c = fbuf_nextc(fbuf)) != ';') {
            if(c == EOF) {
                PARSE_ERROR(loc, "missing ';'\n");
                exit(1);
            }

            // parse expression like: ab + c
            if(isalpha(c)) {
                if(!chrvec_contains(inputs, c)) {
                    PARSE_ERROR(loc, "undefined input %c\n", c);
                    exit(1);
                }
                chrvec_append(&c_expr, c);
                char nextc = fbuf_nextc(fbuf);
                if(isalpha(nextc) || nextc == '!' || nextc == '(') {
                    chrvec_cat(&c_expr, " && ");
                    if(nextc == '!') {
                        chrvec_append(&c_expr, '!');
                    } else {
                        fbuf_rseek(fbuf, -1);
                    }
                } else if(nextc == ' ') {
                    // do nothing
                } else {
                    fbuf_rseek(fbuf, -1);
                }
            } else if(c == '+') {
                chrvec_cat(&c_expr, " || ");
            } else if(c == '*') {
                chrvec_cat(&c_expr, " && ");
            } else if(c == '(' || c == ')' || c == '!') {
                if(c == ')') {
                    char nc = fbuf_nextc(fbuf);
                    if(nc == '(') {
                        chrvec_cat(&c_expr, ") && (");
                    } else if(nc == '!') {
                        ChrVec tmp = chrvec_new();
                        chrvec_cat(&tmp, ") && ");
                        do {
                            chrvec_append(&tmp, nc);
                        } while((nc = fbuf_nextc(fbuf)) == '!');

                        if(nc == '(') {
                            chrvec_append(&tmp, nc);
                            chrvec_cat(&c_expr, tmp.chars);
                            free(tmp.chars);
                        } else {
                            fbuf_rseek(fbuf, -1);
                        }
                    } else if(isalpha(nc)) {
                        chrvec_cat(&c_expr, ") && ");
                        fbuf_rseek(fbuf, -1);
                    } else {
                        fbuf_rseek(fbuf, -1);
                        chrvec_append(&c_expr, c);
                    }
                } else {
                    chrvec_append(&c_expr, c);
                }
            }
        }

        strvec_append(out_exprs, c_expr.chars);
    } else if(c == ' ') {
        // ignore
    } else {
        PARSE_ERROR(loc, "unexpected character '%c'\n", c);
        exit(1);
    }
}

// TODO: Fix lexing and parsing to reduce branching and allow for sorrounding with parentheses
// the current sub-expression when needed (see ANDs added automatically when two variables
// are unpacked from same token (eg. `ab` -> `a && b` should be `(a && b)` instead))
void parse_expr_file(FileBuf* fbuf, ChrVec* out_inputs, StringVec* out_exprs, StringVec* out_outputs) {
    ParseState parse_state = INPUTS;
    Location loc = {
        .filename = fbuf->filename,
        .lineno = 1,
        .colno = 0,
    };

    char c;
    ChrVec tokbuf = chrvec_new();
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

        switch(parse_state) {
        case INPUTS:
            if(isalpha(c)) {
                chrvec_append(out_inputs, c);
            } else if(c == ',' || c == ';' || c == ' ') {
                if(c == ';') parse_state = OUTPUTS;
            } else {
                PARSE_ERROR(&loc, "invalid character '%c' expected comma separated alphabetic input names\n", c);
                exit(1);
            }
            break;

        case OUTPUTS:
            parse_expression(fbuf, &tokbuf, &loc, out_inputs, out_outputs, out_exprs);
            break;
        }
    }

    printf("Inputs:\n    ");
    for(size_t i = 0; i < (*out_inputs).size; i++) {
        printf("%c ", (*out_inputs).chars[i]);
    }
    printf("\n");

    printf("Functions:\n");
    for(size_t i = 0; i < (*out_exprs).size; i++) {
        printf("    %s = %s\n", (*out_outputs).strings[i], (*out_exprs).strings[i]);
    }
    printf("\n");
}

void read_expr_file(FILE* file, FileBuf* fbuf) {
    ChrVec bytes = chrvec_new();

    size_t size = 0;
    char c;
    while((c = fgetc(file)) != EOF) {
        chrvec_append(&bytes, c);
        size++;
    }

    fbuf->bytes = bytes.chars;
    fbuf->len = size;
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

    FILE* expr_file = stdin;
    char* filename = "stdin";

    if(argc == 2) {
        filename = argv[1];
        expr_file = fopen(filename, "r");
    }

    if(expr_file == NULL) {
        perror("fopen");
        exit(1);
    }

    out_args->expr_filename = filename;
    out_args->expr_file = expr_file;
}

void generate_c_file(const ChrVec* inputs, const StringVec* exprs, StringVec* func_names) {
    FILE* funcs_file = fopen("funcs.c", "w");

    if(funcs_file == NULL) {
        perror("fopen");
        exit(1);
    }

    fprintf(funcs_file, "#include <stddef.h>\n");
    fprintf(funcs_file, "\n");

    for(size_t i = 0; i < exprs->size; i++) {
        ChrVec func = chrvec_new();

        sprintf(func.chars, "f%zu", i);

        strvec_append(func_names, func.chars);

        fprintf(funcs_file, "int %s(int* inputs) {\n", func.chars);
        for(size_t j = 0; j < inputs->size; j++) {
            fprintf(funcs_file, "    int %c = inputs[%zu];\n", inputs->chars[j], j);
        }
        fprintf(funcs_file, "    return %s;\n}\n", exprs->strings[i]);
        fprintf(funcs_file, "\n");

        free(func.chars);
    }
    fclose(funcs_file);
}

void compile_c_to_so() {
    // TODO: Compile functions all in memory using `gcc - -o -` and mmap to load the output into an executable memory page

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

void load_funcs_from_so(StringVec* func_names, bool_func_t** funcs) {
    void* handle = dlopen("./funcs.so", RTLD_NOW);
    if(handle == NULL) {
        ERROR("dlopen failed: %s\n", dlerror());
        exit(1);
    }

    for(size_t i = 0; i < func_names->size; i++) {
        funcs[i] = dlsym(handle, func_names->strings[i]);
        if(funcs[i] == NULL) {
            ERROR("error when loading function %s: %s\n", func_names->strings[i], dlerror());
            exit(1);
        }
    }
    unlink("./funcs.so");
}

void generate_truth_table(const ChrVec* inputs, const StringVec* outputs, bool_func_t** funcs) {
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
    FileBuf fbuf;
    ChrVec inputs;
    StringVec exprs, outputs;
    StringVec func_names;
    bool_func_t** funcs;

    // TODO: DON'T exit() in functions, handle errors outside using macros for error codes

    parse_args(argc, argv, &args);

    fbuf = fbuf_new(args.expr_filename, NULL, 0);
    read_expr_file(args.expr_file, &fbuf);

    inputs = chrvec_new();
    exprs = strvec_new();
    outputs = strvec_new();
    parse_expr_file(&fbuf, &inputs, &exprs, &outputs);

    func_names = strvec_new();
    generate_c_file(&inputs, &exprs, &func_names);
    compile_c_to_so();

    funcs = malloc(func_names.size * sizeof(*funcs));
    load_funcs_from_so(&func_names, funcs);
    generate_truth_table(&inputs, &outputs, funcs);

    return 0;
}

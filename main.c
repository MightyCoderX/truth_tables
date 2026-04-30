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

#ifndef KEEP_FILES
#define KEEP_FILES 0
#endif

#define EXIT_ERR_ARGS 1
#define EXIT_ERR_FILE 2
#define EXIT_ERR_PARSE 3
#define EXIT_ERR_COMPILE 4
#define EXIT_ERR_LOADSO 5

#define ERROR(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define PARSE_ERROR(lex, fmt, ...)                            \
    lex_print_diagnostic(lex);                                \
    ERROR("PARSING ERROR: %d " fmt, __LINE__, ##__VA_ARGS__); \
    exit(EXIT_ERR_PARSE);
#define PANIC(fmt, ...)                                                   \
    do                                                                    \
    {                                                                     \
        ERROR("PANIC: %s():%d: " fmt, __func__, __LINE__, ##__VA_ARGS__); \
        abort();                                                          \
    } while (0);

typedef int(bool_func_t)(int* inputs);

int* bits_from_int(int num, int n_bits)
{
    int* bits = malloc(sizeof(*bits) * n_bits);
    for (int i = 0; i < n_bits; i++)
    {
        bits[i] = (num >> (n_bits - 1 - i)) & 1;
    }

    return bits;
}

typedef struct {
    size_t len;
    size_t capacity;
    char* chars;
} ChrVec;

ChrVec chrvec_new()
{
    ChrVec vec = {
        .len = 0,
        .capacity = 10,
        .chars = NULL,
    };
    vec.chars = malloc(vec.capacity);
    assert(vec.chars != NULL && "Buy more RAM");

    return vec;
}

void chrvec_extend(ChrVec* vec, size_t amount)
{
    vec->capacity += amount;
    vec->capacity = ceil((float)vec->capacity * 1.5);
    vec->chars = realloc(vec->chars, vec->capacity);
    assert(vec->chars != NULL && "Buy more RAM");
}

void chrvec_append(ChrVec* vec, char c)
{
    if (vec->len >= vec->capacity)
    {
        chrvec_extend(vec, 1);
    }

    vec->chars[vec->len++] = c;
    vec->chars[vec->len + 1] = '\0';
}

void chrvec_cat(ChrVec* vec, const char* str)
{
    int len = strlen(str);
    if (vec->len + len >= vec->capacity)
    {
        chrvec_extend(vec, len + 1);
    }
    memcpy(vec->chars + vec->len, str, len);
    vec->len += len;
    vec->chars[vec->len + 1] = '\0';
}

bool chrvec_contains(ChrVec* vec, char needle)
{
    for (size_t i = 0; i < vec->len; i++)
    {
        if (vec->chars[i] == needle)
        {
            return true;
        }
    }
    return false;
}

char chrvec_get(ChrVec* vec, long idx)
{
    size_t index = idx;

    if (idx < 0L)
    {
        index = vec->len + idx;
    }

    if (index >= vec->len)
    {
        PANIC("index %ld out of bounds [0,%zu]\n", idx, vec->len);
    }

    return vec->chars[index];
}

void chrvec_print(ChrVec* vec)
{
    printf("[");
    for (size_t i = 0; i < vec->len; i++)
    {
        printf("'%c'", vec->chars[i]);
        if (i < vec->len - 1) printf(", ");
    }
    printf("]\n");
}

typedef struct {
    size_t len;
    size_t capacity;
    char** strings;
} StrVec;

StrVec strvec_new()
{
    StrVec vec = {
        .len = 0,
        .capacity = 10,
    };
    vec.strings = malloc(sizeof(char*) * vec.capacity);
    assert(vec.strings != NULL && "Buy more RAM");

    return vec;
}

void strvec_append(StrVec* vec, const char* str)
{
    if (vec->len >= vec->capacity)
    {
        vec->capacity *= 1.5;
        vec->strings = realloc(vec->strings, vec->capacity * sizeof(char*));
        assert(vec->strings != NULL && "Buy more RAM");
    }
    size_t len = strlen(str);
    vec->strings[vec->len] = malloc(len + 1);
    memcpy(vec->strings[vec->len], str, len);
    vec->strings[vec->len][len] = '\0';
    vec->len++;
}

void strvec_print(StrVec* vec)
{
    printf("[");
    for (size_t i = 0; i < vec->len; i++)
    {
        printf("\"%s\"", vec->strings[i]);
        if (i < vec->len - 1) printf(", ");
    }
    printf("]\n");
}

typedef struct {
    char* filename;
    char* bytes;
    size_t len;
    size_t cur;
} FileBuf;

FileBuf fbuf_new(const char* filename, char* bytes, size_t len)
{
    FileBuf fbuf;

    fbuf.filename = malloc(strlen(filename) + 1);
    assert(fbuf.filename != NULL && "Buy more RAM");
    strcpy(fbuf.filename, filename);
    fbuf.bytes = bytes;
    fbuf.len = len;
    fbuf.cur = 0;

    return fbuf;
}

void fbuf_rseek(FileBuf* fbuf, long amount)
{
    long idx = fbuf->cur + amount;
    if (idx < 0 || idx >= (long)fbuf->len)
    {
        PANIC("index %ld out of bounds [0,%zu]\n", idx, fbuf->len);
    }
    fbuf->cur += amount;
}

char fbuf_getc(FileBuf* fbuf, long idx)
{
    if (idx < 0 || idx >= (long)fbuf->len)
    {
        PANIC("index %ld out of bounds [0,%zu]\n", idx, fbuf->len);
    }
    return fbuf->bytes[idx];
}

char fbuf_nextc(FileBuf* fbuf)
{
    if (fbuf->cur >= fbuf->len)
    {
        return EOF;
    }

    return fbuf->bytes[fbuf->cur++];
}

char fbuf_peekc(FileBuf* fbuf)
{
    if (fbuf->cur >= fbuf->len)
    {
        return EOF;
    }

    return fbuf->bytes[fbuf->cur];
}

typedef struct {
    size_t lineno;
    size_t colno;
} Location;

typedef struct {
    FileBuf* fbuf;
    Location loc;
} Lexer;

Lexer lex_new(FileBuf* fbuf)
{
    return (Lexer) {
        fbuf,
        {
          .lineno = 1,
          .colno = 0,
          },
    };
}

void lex_print_diagnostic(Lexer* lex)
{
    char* fmt = "%s:%zu:%zu:\n    ";
    int fmt_len = strlen(fmt) + 1;
    char* str = malloc(fmt_len);
    int len = snprintf(str, fmt_len, fmt, lex->fbuf->filename, lex->loc.lineno,
        lex->loc.colno);
    len++;
    str = realloc(str, len);
    snprintf(str, len, fmt, lex->fbuf->filename, lex->loc.lineno,
        lex->loc.colno);
    printf("%s", str);

    char c;
    size_t line_start = 0L;
    if (lex->fbuf->cur > 0L)
    {
        line_start = lex->fbuf->cur;
        while (line_start > 0)
        {
            c = fbuf_getc(lex->fbuf, line_start);
            if (c == '\n') break;
            line_start--;
        }
    }
    size_t line_end = line_start;
    while ((c = fbuf_getc(lex->fbuf, line_end)) != '\n' &&
        line_end < lex->fbuf->len)
    {
        putchar(c);
        line_end++;
    }

    printf("\n");
    for (size_t i = 0; i < (lex->loc.colno - 1 + 4); i++)
    {
        putchar(' ');
    }
    putchar('^');
    putchar('\n');
}

char lex_getcur(Lexer* lex)
{
    return lex->fbuf->bytes[lex->fbuf->cur];
}

char lex_skip_char(Lexer* lex)
{
    char c = fbuf_nextc(lex->fbuf);
    if (c == '\n')
    {
        while (c == '\n')
        {
            lex->loc.lineno++;
            lex->loc.colno = 0;
            c = fbuf_nextc(lex->fbuf);
        }
        fbuf_rseek(lex->fbuf, -1);
    }

    if (c == '#')
    {
        while (fbuf_peekc(lex->fbuf) != '\n')
        {
            fbuf_nextc(lex->fbuf);
        }
        c = lex_skip_char(lex);
    }
    else
    {
        lex->loc.colno++;
    }
    return c;
}

char lex_expect_char(Lexer* lex, char expected)
{
    char c = lex_skip_char(lex);
    if (c != expected)
    {
        PARSE_ERROR(lex, "expected '%c' got '%c' (%d) instead\n", expected, c,
            c);
    }

    return c;
}

char lex_expect_alpha(Lexer* lex)
{
    char c = lex_skip_char(lex);
    if (!isalpha(c))
    {
        PARSE_ERROR(lex, "expected [a-zA-Z] got '%c' (%d) instead\n", c, c);
    }

    return c;
}

char lex_expect_alnum(Lexer* lex)
{
    char c = lex_skip_char(lex);
    if (!isalnum(c))
    {
        PARSE_ERROR(lex, "expected [a-zA-Z0-9] got '%c' (%d) instead\n", c, c);
    }

    return c;
}

char lex_peek_char(Lexer* lex)
{
    return fbuf_peekc(lex->fbuf);
}

void lex_skip_space(Lexer* lex)
{
    while (isspace(lex_peek_char(lex)))
        lex_skip_char(lex);
}

void lex_seekto(Lexer* lex, char target)
{
    char c;
    while ((c = lex_peek_char(lex)) != target)
    {
        lex_skip_char(lex);
    }
}

typedef enum {
    INPUTS,
    OUTPUTS,
    END,
} ParserState;

typedef struct {
    ChrVec inputs;
    StrVec outputs;
    StrVec expressions;
    ParserState state;
} Parser;

Parser parser_new()
{
    return (Parser) {
        .inputs = chrvec_new(),
        .outputs = strvec_new(),
        .expressions = strvec_new(),
        .state = INPUTS,
    };
}

void parse_inputs(Lexer* lex, Parser* parser)
{
    bool stop = false;
    while (!stop)
    {
        char input = lex_expect_alpha(lex);
        if (chrvec_contains(&parser->inputs, input))
        {
            PARSE_ERROR(lex, "duplicate variable %c\n", input);
        }
        chrvec_append(&parser->inputs, input);
        char c = lex_peek_char(lex);
        switch (c)
        {
        case ';':
            lex_skip_char(lex);
            parser->state = OUTPUTS;
            stop = true;
            break;
        case ',':
            lex_skip_char(lex);
            c = lex_peek_char(lex);
            if (c == ' ')
            {
                lex_skip_char(lex);
            }
            break;
        default:
            PARSE_ERROR(lex, "expected ',' or ';' got '%c'\n", c);
        }
    }
    lex_skip_space(lex);
}

void parse_expression(Lexer* lex, Parser* parser, ChrVec* c_expr)
{
    bool stop = false;
    while (!stop)
    {
        lex_skip_space(lex);
        char c = lex_peek_char(lex);
        printf("'%c' (%d)\n", c, c);
        printf("%s\n", c_expr->chars);
        if (isalpha(c))
        {
            lex_skip_char(lex);
            chrvec_append(c_expr, c);
        }
        else if (c == '(')
        {
            lex_skip_char(lex);
            chrvec_append(c_expr, c);
            parse_expression(lex, parser, c_expr);
        }
        else if (c == ')')
        {
            lex_skip_char(lex);
            chrvec_append(c_expr, c);
            stop = true;
        }
        else if (c == '+')
        {
            lex_skip_char(lex);
            chrvec_cat(c_expr, " || ");
        }
        else if (c == ';' || c == '\n' || c == EOF)
        {
            lex_skip_char(lex);
            stop = true;
        }
        else
        {
            PARSE_ERROR(lex, "expected expression got '%c'\n", c);
        }
    }
}

void parse_outputs(Lexer* lex, Parser* parser)
{
    bool stop = false;
    ChrVec tokbuf = chrvec_new();
    while (!stop)
    {
        strvec_print(&parser->outputs);
        tokbuf = chrvec_new();
        if (lex_peek_char(lex) == EOF)
        {
            parser->state = END;
            stop = true;
            continue;
        }

        if (lex_peek_char(lex) == '\n')
        {
            lex_skip_char(lex);
            continue;
        }

        char c = lex_expect_alpha(lex);
        chrvec_append(&tokbuf, c);

        while (true)
        {
            c = lex_peek_char(lex);
            if (isalnum(c))
            {
                lex_skip_char(lex);
                chrvec_append(&tokbuf, c);
            }
            else if (c == ' ')
            {
                lex_skip_char(lex);
            }
            else if (c == '=')
            {
                printf("tokbuf: ");
                chrvec_print(&tokbuf);
                strvec_append(&parser->outputs, tokbuf.chars);
                lex_skip_char(lex);
                ChrVec c_expr = chrvec_new();
                parse_expression(lex, parser, &c_expr);
                strvec_append(&parser->expressions, c_expr.chars);
                break;
            }
            else
            {
                PARSE_ERROR(lex,
                    "expected [a-zA-Z0-9], '=' or ' ' got '%c' (%d) instead\n",
                    c, c);
            }
        }
    }
}

// TODO: Fix lexing and parsing to reduce branching and allow for sorrounding with parentheses
// the current sub-expression when needed (see ANDs added automatically when two variables
// are unpacked from same token (eg. `ab` -> `a && b` should be `(a && b)` instead))
void parse_expr_file(FileBuf* fbuf, Parser* parser)
{
    Lexer lex = lex_new(fbuf);

    bool stop = false;
    while (!stop)
    {
        switch (parser->state)
        {
        case INPUTS:
            printf("parsing inputs:\n");
            parse_inputs(&lex, parser);
            chrvec_print(&parser->inputs);
            break;
        case OUTPUTS:
            printf("parsing outputs:\n");
            parse_outputs(&lex, parser);
            break;
        case END:
            stop = true;
        }
    }
}

void read_expr_file(FILE* file, FileBuf* fbuf)
{
    ChrVec bytes = chrvec_new();

    size_t size = 0;
    char c;
    while ((c = fgetc(file)) != EOF)
    {
        chrvec_append(&bytes, c);
        size++;
    }

    fbuf->bytes = bytes.chars;
    fbuf->len = size;
    if (fbuf == NULL)
    {
        ERROR("file '%s' is empty\n", fbuf->filename);
        exit(EXIT_ERR_FILE);
    }
}

typedef struct {
    char* expr_filename;
    FILE* expr_file;
} Args;

void parse_args(int argc, char** argv, Args* out_args)
{
    if (argc > 2)
    {
        ERROR("usage: %s [expr_file]\n", argv[0]);
        exit(EXIT_ERR_ARGS);
    }

    FILE* expr_file = stdin;
    char* filename = "stdin";

    if (argc == 2)
    {
        filename = argv[1];
        expr_file = fopen(filename, "r");
    }

    if (expr_file == NULL)
    {
        perror("fopen");
        exit(EXIT_ERR_FILE);
    }

    out_args->expr_filename = filename;
    out_args->expr_file = expr_file;
}

void generate_c_file(const Parser* parser, StrVec* func_names)
{
    FILE* funcs_file = fopen("funcs.c", "w");

    if (funcs_file == NULL)
    {
        perror("fopen");
        exit(EXIT_ERR_FILE);
    }

    fprintf(funcs_file, "#include <stddef.h>\n");
    fprintf(funcs_file, "\n");

    for (size_t i = 0; i < parser->expressions.len; i++)
    {
        ChrVec func = chrvec_new();

        sprintf(func.chars, "f%zu", i);

        strvec_append(func_names, func.chars);

        fprintf(funcs_file, "int %s(int* inputs) {\n", func.chars);
        for (size_t j = 0; j < parser->inputs.len; j++)
        {
            fprintf(funcs_file, "    int %c = inputs[%zu];\n",
                parser->inputs.chars[j], j);
        }
        fprintf(funcs_file, "    return %s;\n}\n",
            parser->expressions.strings[i]);
        fprintf(funcs_file, "\n");

        free(func.chars);
    }
    fclose(funcs_file);
}

void compile_c_to_so()
{
    // TODO: Compile functions all in memory using `gcc - -o -` and mmap to load the output into an executable memory page

    pid_t child = fork();
    if (child == -1)
    {
        perror("fork");
        exit(EXIT_ERR_COMPILE);
    }
    if (child == 0)
    {
        int err = execlp("gcc",
            "gcc", //
            //"-Wall",
            "-o", //
            "funcs.so", //
            "funcs.c", //
            "-shared", //
            NULL //
        );
        if (err == -1)
        {
            perror("execlp");
        }
        exit(EXIT_ERR_COMPILE);
    }
    int err = waitpid(child, NULL, 0);
    if (err == -1)
    {
        perror("wait");
        exit(EXIT_ERR_COMPILE);
    }
#if !defined(KEEP_FILES) || KEEP_FILES == 0
    unlink("./funcs.c");
#endif
}

void load_funcs_from_so(StrVec* func_names, bool_func_t** funcs)
{
    void* handle = dlopen("./funcs.so", RTLD_NOW);
    if (handle == NULL)
    {
        ERROR("dlopen failed: %s\n", dlerror());
        exit(EXIT_ERR_LOADSO);
    }

    for (size_t i = 0; i < func_names->len; i++)
    {
        funcs[i] = dlsym(handle, func_names->strings[i]);
        if (funcs[i] == NULL)
        {
            ERROR("error when loading function %s: %s\n",
                func_names->strings[i], dlerror());
            exit(EXIT_ERR_LOADSO);
        }
    }
#if !defined(KEEP_FILES) || KEEP_FILES == 0
    unlink("./funcs.so");
#endif
}

void generate_truth_table(const ChrVec* inputs, const StrVec* outputs,
    bool_func_t** funcs)
{
    // print header
    for (size_t i = 0; i < inputs->len; i++)
    {
        printf("%c ", inputs->chars[i]);
    }
    printf("|");
    for (size_t i = 0; i < outputs->len; i++)
    {
        printf(" %s", outputs->strings[i]);
    }
    printf("\n");

    // print values
    for (size_t i = 0; i < pow(2, inputs->len); i++)
    {
        int* bits = bits_from_int(i, inputs->len);
        for (size_t j = 0; j < inputs->len; j++)
        {
            printf("%d ", bits[j]);
        }
        printf("|");
        for (size_t j = 0; j < outputs->len; j++)
        {
            printf(" %d ", (funcs[j])(bits));
        }
        printf("\n");
    }
}

int main(int argc, char** argv)
{
    Args args = { 0 };
    FileBuf fbuf;
    Parser parser;
    StrVec func_names;
    bool_func_t** funcs;

    // TODO: DON'T exit() in functions, handle errors outside

    parse_args(argc, argv, &args);

    fbuf = fbuf_new(args.expr_filename, NULL, 0);
    read_expr_file(args.expr_file, &fbuf);

    parser = parser_new();
    parse_expr_file(&fbuf, &parser);

    func_names = strvec_new();
    generate_c_file(&parser, &func_names);
    compile_c_to_so();

    funcs = malloc(func_names.len * sizeof(*funcs));
    load_funcs_from_so(&func_names, funcs);
    generate_truth_table(&parser.inputs, &parser.outputs, funcs);

    return 0;
}

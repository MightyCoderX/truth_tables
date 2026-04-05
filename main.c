#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <readline/readline.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

typedef int (*bool_func_t)(int* inputs, size_t len);

int* bits_from_int(int num, int n_bits) {
    int* bits = malloc(sizeof(*bits) * n_bits);
    for(int i = 0; i < n_bits; i++) {
        bits[i] = (num >> (n_bits - 1 - i)) & 1;
    }

    return bits;
}

int main(void) {
    // TODO: Take input names directly instead of count
    int input_count = 0;
    printf("input count: ");
    int res = scanf("%d", &input_count);
    if(res == EOF) {
        perror("scanf");
        return 1;
    }

    char* input_names = malloc(input_count);

    for(int i = 0; i < input_count; i++) {
        printf("input #%d name: ", i);
        scanf(" %c", &input_names[i]);
    }

    int function_count = 0;
    printf("function count: ");
    res = scanf("%d", &function_count);
    if(res == EOF) {
        perror("scanf");
        return 1;
    }

    char** func_names = malloc(function_count * sizeof(*func_names));

    FILE* funcs_file = fopen("funcs.c", "w");
    fprintf(funcs_file, "#include <stddef.h>\n");
    fprintf(funcs_file, "\n");

    for(int i = 0; i < function_count; i++) {
        printf("func #%d expression", i);
        char* expr = readline("> ");
        char* func = malloc(strlen(expr) + 1);

        sprintf(func, "f%d", i);

        func_names[i] = func;

        fprintf(funcs_file, "int %s(int* inputs, size_t len) {\n", func);
        for(int j = 0; j < input_count; j++) {
            fprintf(funcs_file, "    int %c = inputs[%d];\n", input_names[j], j);
        }
        fprintf(funcs_file, "    return %s;\n}\n", expr);
    }
    fclose(funcs_file);

    // Compile .c file with functions to .so
    pid_t child = fork();
    if(child == -1) {
        perror("fork");
        return 1;
    }
    if(child == 0) {
        int err = execlp("gcc", "gcc", "-Wall", "-o", "funcs.so", "funcs.c", "-shared", NULL);
        if(err == -1) {
            perror("execlp");
        }
        return 1;
    }
    int err = waitpid(child, NULL, 0);
    if(err == -1) {
        perror("wait");
        return 1;
    }

    // Open file with dlopen
    void* handle = dlopen("./funcs.so", RTLD_NOW);
    if(handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    // Load all functions
    bool_func_t* funcs = malloc(function_count * sizeof(*funcs));
    for(int i = 0; i < function_count; i++) {
        funcs[i] = dlsym(handle, func_names[i]);
        if(*funcs[i] == NULL) {
            fprintf(stderr, "error when getting function %s: %s\n", func_names[i], dlerror());
            return 1;
        }
    }

    // print header
    for(int i = 0; i < input_count; i++) {
        printf("%c ", input_names[i]);
    }
    printf("|");
    for(int i = 0; i < function_count; i++) {
        printf(" Y%d", i);
    }
    printf("\n--------------\n");

    // print values
    for(int i = 0; i < pow(2, input_count); i++) {
        int* bits = bits_from_int(i, input_count);
        for(int j = 0; j < input_count; j++) {
            printf("%d ", bits[j]);
        }
        printf("|");
        for(int j = 0; j < function_count; j++) {
            printf(" %d ", (funcs[j])(bits, input_count));
        }
        printf("\n");
    }
    return 0;
}

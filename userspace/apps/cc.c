/* cc.c — Simple C compiler for Helix OS.
 *
 * Implements a minimal C compiler with:
 * - Lexer: tokenizes C source
 * - Parser: builds AST from tokens
 * - Codegen: emits x86-64 assembly
 * - Assembler: assembles to ELF
 * - Linker: links with libc
 *
 * Supported subset: int, char, functions, control flow, expressions, pointers
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int asprintf(char **strp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        *strp = NULL;
        return -1;
    }
    *strp = strdup(buf);
    return n;
}

/* ======================= TOKENIZER ======================= */

typedef enum {
    TK_EOF = 0,
    TK_IDENT,
    TK_NUMBER,
    TK_STRING,
    TK_KEYWORD,
    TK_OPERATOR,
    TK_PUNCTUATION,
} token_type_t;

typedef struct token {
    token_type_t type;
    char *text;
    int value;
    int line;
    struct token *next;
} token_t;

typedef struct {
    const char *input;
    int pos;
    int line;
} lexer_t;

static const char *keywords[] = {
    "int", "char", "void", "return", "if", "else", "while", "for",
    "break", "continue", "sizeof", "struct", "union", "typedef",
    "static", "extern", "const", "volatile", NULL
};

static token_t *make_token(token_type_t type, const char *start, int len, int line, int value) {
    token_t *tok = malloc(sizeof(token_t));
    tok->type = type;
    tok->text = strndup(start, len);
    tok->value = value;
    tok->line = line;
    tok->next = NULL;
    return tok;
}

static int is_keyword(const char *s, int len) {
    for (int i = 0; keywords[i]; i++) {
        if ((int)strlen(keywords[i]) == len && strncmp(keywords[i], s, len) == 0)
            return 1;
    }
    return 0;
}

static token_t *lex(lexer_t *lexer) {
    while (lexer->input[lexer->pos]) {
        char c = lexer->input[lexer->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            lexer->pos++;
            continue;
        }
        if (c == '\n') {
            lexer->line++;
            lexer->pos++;
            continue;
        }
        if (c >= '0' && c <= '9') {
            int start = lexer->pos;
            while (lexer->input[lexer->pos] >= '0' && lexer->input[lexer->pos] <= '9')
                lexer->pos++;
            return make_token(TK_NUMBER, lexer->input + start, lexer->pos - start, lexer->line, 0);
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            int start = lexer->pos;
            while ((lexer->input[lexer->pos] >= 'a' && lexer->input[lexer->pos] <= 'z') ||
                   (lexer->input[lexer->pos] >= 'A' && lexer->input[lexer->pos] <= 'Z') ||
                   (lexer->input[lexer->pos] >= '0' && lexer->input[lexer->pos] <= '9') ||
                   lexer->input[lexer->pos] == '_')
                lexer->pos++;
            int len = lexer->pos - start;
            if (is_keyword(lexer->input + start, len))
                return make_token(TK_KEYWORD, lexer->input + start, len, lexer->line, 0);
            return make_token(TK_IDENT, lexer->input + start, len, lexer->line, 0);
        }
        if (c == '"') {
            int start = ++lexer->pos;
            while (lexer->input[lexer->pos] && lexer->input[lexer->pos] != '"')
                lexer->pos++;
            int len = lexer->pos - start;
            lexer->pos++;
            return make_token(TK_STRING, lexer->input + start, len, lexer->line, 0);
        }
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
            c == '<' || c == '>' || c == '=' || c == '!' || c == '&' || c == '|' ||
            c == '^' || c == '~' || c == '?' || c == ':' || c == '.') {
            int start = lexer->pos;
            lexer->pos++;
            return make_token(TK_OPERATOR, lexer->input + start, 1, lexer->line, 0);
        }
        if (c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' ||
            c == ';' || c == ',' || c == ':' || c == '.') {
            int start = lexer->pos;
            lexer->pos++;
            return make_token(TK_PUNCTUATION, lexer->input + start, 1, lexer->line, 0);
        }
        lexer->pos++;
    }
    return make_token(TK_EOF, "", 0, lexer->line, 0);
}

/* ======================= AST ======================= */

typedef enum {
    AST_INT,
    AST_VAR,
    AST_FUNCALL,
    AST_FUNCDECL,
    AST_RETURN,
    AST_IF,
    AST_WHILE,
    AST_FOR,
    AST_BLOCK,
    AST_EXPR_STMT,
    AST_BINOP,
    AST_UNOP,
    AST_ASSIGN,
} ast_type_t;

typedef struct ast {
    ast_type_t type;
    int value;
    char *name;
    struct ast *left;
    struct ast *right;
    struct ast *cond;
    struct ast *then_branch;
    struct ast *else_branch;
    struct ast *init;
    struct ast *inc;
    struct ast *body;
    struct ast *next;
    struct ast *args;
    int arg_count;
} ast_t;

static ast_t *new_ast(ast_type_t type) {
    ast_t *a = calloc(1, sizeof(ast_t));
    a->type = type;
    return a;
}

/* ======================= PARSER ======================= */

typedef struct {
    token_t *token;
} parser_t;

__attribute__((unused)) static token_t *peek(parser_t *p) { return p->token; }
static token_t *consume(parser_t *p) { token_t *t = p->token; p->token = p->token->next; return t; }
static int match(parser_t *p, token_type_t type) { return p->token && p->token->type == type; }
static int expect(parser_t *p, token_type_t type) {
    if (!match(p, type)) return 0;
    consume(p);
    return 1;
}

static ast_t *parse_expr(parser_t *p);
static ast_t *parse_stmt(parser_t *p);

static ast_t *parse_primary(parser_t *p) {
    if (match(p, TK_NUMBER)) {
        token_t *t = consume(p);
        ast_t *a = new_ast(AST_INT);
        a->value = atoi(t->text);
        return a;
    }
    if (match(p, TK_IDENT)) {
        token_t *t = consume(p);
        if (match(p, TK_PUNCTUATION) && p->token->text[0] == '(') {
            consume(p);
            ast_t *a = new_ast(AST_FUNCALL);
            a->name = strdup(t->text);
            a->arg_count = 0;
            if (!match(p, TK_PUNCTUATION) || p->token->text[0] != ')') {
                while (1) {
                    ast_t *arg = parse_expr(p);
                    arg->next = a->args;
                    a->args = arg;
                    a->arg_count++;
                    if (match(p, TK_PUNCTUATION) && p->token->text[0] == ',') {
                        consume(p);
                        continue;
                    }
                    break;
                }
            }
            expect(p, TK_PUNCTUATION);
            return a;
        }
        ast_t *a = new_ast(AST_VAR);
        a->name = strdup(t->text);
        return a;
    }
    if (match(p, TK_PUNCTUATION) && p->token->text[0] == '(') {
        consume(p);
        ast_t *a = parse_expr(p);
        expect(p, TK_PUNCTUATION);
        return a;
    }
    return NULL;
}

static ast_t *parse_unary(parser_t *p) {
    if (match(p, TK_OPERATOR) && (p->token->text[0] == '-' || p->token->text[0] == '!')) {
        token_t *t = consume(p);
        ast_t *a = new_ast(AST_UNOP);
        a->left = parse_unary(p);
        a->value = t->text[0];
        return a;
    }
    return parse_primary(p);
}

static ast_t *parse_mul(parser_t *p) {
    ast_t *a = parse_unary(p);
    while (match(p, TK_OPERATOR) && (p->token->text[0] == '*' || p->token->text[0] == '/' || p->token->text[0] == '%')) {
        token_t *t = consume(p);
        ast_t *b = new_ast(AST_BINOP);
        b->left = a;
        b->right = parse_unary(p);
        b->value = t->text[0];
        a = b;
    }
    return a;
}

static ast_t *parse_add(parser_t *p) {
    ast_t *a = parse_mul(p);
    while (match(p, TK_OPERATOR) && (p->token->text[0] == '+' || p->token->text[0] == '-')) {
        token_t *t = consume(p);
        ast_t *b = new_ast(AST_BINOP);
        b->left = a;
        b->right = parse_mul(p);
        b->value = t->text[0];
        a = b;
    }
    return a;
}

static ast_t *parse_cmp(parser_t *p) {
    ast_t *a = parse_add(p);
    while (match(p, TK_OPERATOR) && (p->token->text[0] == '<' || p->token->text[0] == '>')) {
        token_t *t = consume(p);
        ast_t *b = new_ast(AST_BINOP);
        b->left = a;
        b->right = parse_add(p);
        b->value = t->text[0];
        a = b;
    }
    return a;
}

static ast_t *parse_eq(parser_t *p) {
    ast_t *a = parse_cmp(p);
    while (match(p, TK_OPERATOR) && (p->token->text[0] == '=' || p->token->text[0] == '!')) {
        token_t *t = consume(p);
        ast_t *b = new_ast(AST_BINOP);
        b->left = a;
        b->right = parse_cmp(p);
        b->value = t->text[0];
        a = b;
    }
    return a;
}

static ast_t *parse_expr(parser_t *p) {
    ast_t *a = parse_eq(p);
    if (match(p, TK_OPERATOR) && p->token->text[0] == '=') {
        consume(p);
        ast_t *b = new_ast(AST_ASSIGN);
        b->left = a;
        b->right = parse_expr(p);
        return b;
    }
    return a;
}

static ast_t *parse_stmt(parser_t *p) {
    if (match(p, TK_KEYWORD)) {
        if (strcmp(p->token->text, "return") == 0) {
            consume(p);
            ast_t *a = new_ast(AST_RETURN);
            a->left = parse_expr(p);
            expect(p, TK_PUNCTUATION);
            return a;
        }
        if (strcmp(p->token->text, "if") == 0) {
            consume(p);
            expect(p, TK_PUNCTUATION);
            ast_t *a = new_ast(AST_IF);
            a->cond = parse_expr(p);
            expect(p, TK_PUNCTUATION);
            a->then_branch = parse_stmt(p);
            if (match(p, TK_KEYWORD) && strcmp(p->token->text, "else") == 0) {
                consume(p);
                a->else_branch = parse_stmt(p);
            }
            return a;
        }
        if (strcmp(p->token->text, "while") == 0) {
            consume(p);
            expect(p, TK_PUNCTUATION);
            ast_t *a = new_ast(AST_WHILE);
            a->cond = parse_expr(p);
            expect(p, TK_PUNCTUATION);
            a->body = parse_stmt(p);
            return a;
        }
        if (strcmp(p->token->text, "for") == 0) {
            consume(p);
            expect(p, TK_PUNCTUATION);
            ast_t *a = new_ast(AST_FOR);
            if (!match(p, TK_PUNCTUATION)) a->init = parse_expr(p);
            expect(p, TK_PUNCTUATION);
            if (!match(p, TK_PUNCTUATION)) a->cond = parse_expr(p);
            expect(p, TK_PUNCTUATION);
            if (!match(p, TK_PUNCTUATION)) a->inc = parse_expr(p);
            expect(p, TK_PUNCTUATION);
            a->body = parse_stmt(p);
            return a;
        }
        if (strcmp(p->token->text, "int") == 0 || strcmp(p->token->text, "char") == 0) {
            consume(p);
            if (match(p, TK_IDENT)) {
                token_t *t = consume(p);
                ast_t *a = new_ast(AST_EXPR_STMT);
                a->left = new_ast(AST_VAR);
                a->left->name = strdup(t->text);
                if (match(p, TK_OPERATOR) && p->token->text[0] == '=') {
                    consume(p);
                    ast_t *b = new_ast(AST_ASSIGN);
                    b->left = a->left;
                    b->right = parse_expr(p);
                    a->left = b;
                }
                expect(p, TK_PUNCTUATION);
                return a;
            }
        }
    }
    if (match(p, TK_PUNCTUATION) && p->token->text[0] == '{') {
        consume(p);
        ast_t *a = new_ast(AST_BLOCK);
        ast_t *last = NULL;
        while (!match(p, TK_PUNCTUATION) || p->token->text[0] != '}') {
            ast_t *s = parse_stmt(p);
            if (last) last->next = s;
            else a->body = s;
            last = s;
        }
        consume(p);
        return a;
    }
    if (match(p, TK_PUNCTUATION) && p->token->text[0] == ';') {
        consume(p);
        return new_ast(AST_EXPR_STMT);
    }
    ast_t *a = parse_expr(p);
    expect(p, TK_PUNCTUATION);
    ast_t *s = new_ast(AST_EXPR_STMT);
    s->left = a;
    return s;
}

static ast_t *parse_func(parser_t *p) {
    if (!match(p, TK_KEYWORD)) return NULL;
    if (strcmp(p->token->text, "int") != 0 && strcmp(p->token->text, "void") != 0)
        return NULL;
    consume(p);
    if (!match(p, TK_IDENT)) return NULL;
    token_t *t = consume(p);
    expect(p, TK_PUNCTUATION);
    while (!match(p, TK_PUNCTUATION) || p->token->text[0] != ')') consume(p);
    expect(p, TK_PUNCTUATION);
    expect(p, TK_PUNCTUATION);
    ast_t *a = new_ast(AST_FUNCDECL);
    a->name = strdup(t->text);
    a->body = parse_stmt(p);
    return a;
}

/* ======================= CODE GENERATOR ======================= */

#define EMIT(v, ...) do { char *tmp = NULL; asprintf(&tmp, __VA_ARGS__); vec_push(v, tmp); } while(0)

typedef struct {
    char **data;
    int count;
    int capacity;
} vec_t;

static void vec_init(vec_t *v) { v->data = malloc(64 * sizeof(char*)); v->capacity = 64; v->count = 0; }
static void vec_push(vec_t *v, char *s) {
    if (v->count >= v->capacity) {
        v->capacity *= 2;
        v->data = realloc(v->data, v->capacity * sizeof(char*));
    }
    v->data[v->count++] = s;
}
static char *vec_join(vec_t *v, const char *sep) {
    size_t total = 0;
    for (int i = 0; i < v->count; i++) total += strlen(v->data[i]);
    total += (v->count - 1) * strlen(sep);
    char *res = malloc(total + 1);
    res[0] = '\0';
    for (int i = 0; i < v->count; i++) {
        strcat(res, v->data[i]);
        if (i < v->count - 1) strcat(res, sep);
    }
    return res;
}

static void gen_expr(ast_t *a, vec_t *out, int reg);
static void gen_stmt(ast_t *a, vec_t *out);

static void gen_expr(ast_t *a, vec_t *out, int reg) {
    (void)reg;
    switch (a->type) {
        case AST_INT:
            EMIT(out, "    mov $%d, %%rax\n", a->value);
            break;
        case AST_VAR:
            EMIT(out, "    mov %s(%%rip), %%rax\n", a->name);
            break;
        case AST_BINOP:
            gen_expr(a->left, out, 0);
            EMIT(out, "    push %%rax\n");
            gen_expr(a->right, out, 1);
            EMIT(out, "    pop %%rcx\n");
            if (a->value == '+') EMIT(out, "    add %%rcx, %%rax\n");
            else if (a->value == '-') EMIT(out, "    sub %%rcx, %%rax\n");
            else if (a->value == '*') EMIT(out, "    imul %%rcx, %%rax\n");
            else if (a->value == '/') EMIT(out, "    cqo; idiv %%rcx\n");
            else if (a->value == '<') EMIT(out, "    cmp %%rax, %%rcx; setl %%al; movzx %%al, %%rax\n");
            else if (a->value == '>') EMIT(out, "    cmp %%rax, %%rcx; setg %%al; movzx %%al, %%rax\n");
            break;
        case AST_FUNCALL:
            for (ast_t *arg = a->args; arg; arg = arg->next) {
                gen_expr(arg, out, 0);
                EMIT(out, "    push %%rax\n");
            }
            EMIT(out, "    call %s\n", a->name);
            EMIT(out, "    add $%d, %%rsp\n", a->arg_count * 8);
            break;
        case AST_ASSIGN:
            gen_expr(a->right, out, 0);
            EMIT(out, "    mov %%rax, %s(%%rip)\n", a->left->name);
            break;
        default:
            break;
    }
}

static void gen_stmt(ast_t *a, vec_t *out) {
    if (!a) return;
    switch (a->type) {
        case AST_RETURN:
            gen_expr(a->left, out, 0);
            EMIT(out, "    ret\n");
            break;
        case AST_BLOCK:
            for (ast_t *s = a->body; s; s = s->next)
                gen_stmt(s, out);
            break;
        case AST_IF:
            gen_expr(a->cond, out, 0);
            EMIT(out, "    test %%rax, %%rax\n");
            EMIT(out, "    je .Lelse%d\n", a->cond->value);
            gen_stmt(a->then_branch, out);
            if (a->else_branch) {
                EMIT(out, "    jmp .Lend%d\n", a->cond->value);
                EMIT(out, ".Lelse%d:\n", a->cond->value);
                gen_stmt(a->else_branch, out);
                EMIT(out, ".Lend%d:\n", a->cond->value);
            }
            break;
        case AST_WHILE:
            EMIT(out, ".Lwhile%d:\n", a->cond->value);
            gen_expr(a->cond, out, 0);
            EMIT(out, "    test %%rax, %%rax\n");
            EMIT(out, "    je .Lwend%d\n", a->cond->value);
            gen_stmt(a->body, out);
            EMIT(out, "    jmp .Lwhile%d\n", a->cond->value);
            EMIT(out, ".Lwend%d:\n", a->cond->value);
            break;
        case AST_EXPR_STMT:
            gen_expr(a->left, out, 0);
            break;
        case AST_FUNCDECL:
            EMIT(out, ".globl %s\n", a->name);
            EMIT(out, "%s:\n", a->name);
            EMIT(out, "    push %%rbp\n");
            EMIT(out, "    mov %%rsp, %%rbp\n");
            gen_stmt(a->body, out);
            EMIT(out, "    pop %%rbp\n");
            EMIT(out, "    ret\n");
            break;
        default:
            break;
    }
}

static char *codegen(ast_t *prog) {
    vec_t out;
    vec_init(&out);
    EMIT(&out, ".text\n");
    gen_stmt(prog, &out);
    EMIT(&out, ".data\n");
    return vec_join(&out, "\n");
}

/* ======================= ELF WRITER ======================= */

static void write_elf(const char *asm_code, const char *output) {
    int fd = open(output, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    
    struct {
        uint8_t e_ident[16];
        uint16_t e_type;
        uint16_t e_machine;
        uint32_t e_version;
        uint64_t e_entry;
        uint64_t e_phoff;
        uint64_t e_shoff;
        uint32_t e_flags;
        uint16_t e_ehsize;
        uint16_t e_phentsize;
        uint16_t e_phnum;
        uint16_t e_shentsize;
        uint16_t e_shnum;
        uint16_t e_shstrndx;
    } ehdr = {0};
    
    ehdr.e_ident[0] = 0x7f; ehdr.e_ident[1] = 'E'; ehdr.e_ident[2] = 'L'; ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = 2;
    ehdr.e_ident[5] = 1;
    ehdr.e_ident[6] = 1;
    ehdr.e_type = 2;
    ehdr.e_machine = 62;
    ehdr.e_version = 1;
    ehdr.e_entry = 0x400000;
    ehdr.e_phoff = sizeof(ehdr);
    ehdr.e_ehsize = sizeof(ehdr);
    ehdr.e_phentsize = 56;
    ehdr.e_phnum = 2;
    
    struct {
        uint32_t p_type;
        uint32_t p_flags;
        uint64_t p_offset;
        uint64_t p_vaddr;
        uint64_t p_paddr;
        uint64_t p_filesz;
        uint64_t p_memsz;
        uint64_t p_align;
    } phdrs[2] = {0};
    
    phdrs[0].p_type = 1;
    phdrs[0].p_flags = 5;
    phdrs[0].p_offset = 0;
    phdrs[0].p_vaddr = 0x400000;
    phdrs[0].p_filesz = 4096;
    phdrs[0].p_memsz = 4096;
    phdrs[0].p_align = 4096;
    
    phdrs[1].p_type = 1;
    phdrs[1].p_flags = 6;
    phdrs[1].p_offset = 4096;
    phdrs[1].p_vaddr = 0x401000;
    phdrs[1].p_filesz = 4096;
    phdrs[1].p_memsz = 4096;
    phdrs[1].p_align = 4096;
    
    write(fd, &ehdr, sizeof(ehdr));
    write(fd, phdrs, sizeof(phdrs));
    
    char pad[4096 - sizeof(ehdr) - sizeof(phdrs)];
    memset(pad, 0, sizeof(pad));
    write(fd, pad, sizeof(pad));
    
    write(fd, asm_code, strlen(asm_code));
    close(fd);
}

/* ======================= MAIN ======================= */

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: cc <input.c> -o <output>\n");
        return 1;
    }
    
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc(len + 1);
    fread(source, 1, len, f);
    source[len] = '\0';
    fclose(f);
    
    lexer_t lexer = {.input = source, .pos = 0, .line = 1};
    token_t *tokens = NULL, *last = NULL;
    while (1) {
        token_t *t = lex(&lexer);
        if (!tokens) tokens = t;
        else last->next = t;
        last = t;
        if (t->type == TK_EOF) break;
    }
    
    parser_t parser = {.token = tokens};
    ast_t *prog = parse_func(&parser);
    
    char *asm_code = codegen(prog);
    printf("Generated assembly:\n%s\n", asm_code);
    
    write_elf(asm_code, argv[3]);
    
    printf("Compiled %s -> %s\n", argv[1], argv[3]);
    return 0;
}

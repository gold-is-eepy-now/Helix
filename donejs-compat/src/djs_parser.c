/* djs_parser.c — tokenizer, parser plumbing and statement parsing for the
 * donejs JavaScript dialect. Expression parsing lives in djs_expr.c.
 *
 * The parser produces an arena-allocated AST: statements and expressions both
 * use the generic djs_node_t with kind-specific fields.
 */

#include "djs_parser.h"

static djs_node_t *djs_parse_statement(djs_parser_t *p);

/* ------------------------------------------------------------------ basics */

djs_node_t *djs_node_new(djs_parser_t *p, int kind) {
    djs_node_t *node = (djs_node_t *)djs_arena_calloc(p->rt, 1, sizeof(*node));
    if (node == NULL) return NULL;
    node->kind = (uint8_t)kind;
    return node;
}

void djs_list_push(djs_parser_t *p, djs_node_t *node, djs_node_t *item) {
    if (node->list_count == node->list_capacity) {
        size_t capacity = node->list_capacity == 0 ? 4 : node->list_capacity * 2;
        djs_node_t **grown = (djs_node_t **)djs_arena_alloc(p->rt, capacity * sizeof(*grown));
        if (grown == NULL) return;
        if (node->list_count > 0) {
            djs_memcpy(grown, node->list, node->list_count * sizeof(*grown));
        }
        node->list = grown;
        node->list_capacity = capacity;
    }
    node->list[node->list_count++] = item;
}

djs_token_t *djs_cur(djs_parser_t *p) {
    if (p->count == 0) return NULL;
    if (p->index >= p->count) return &p->tokens[p->count - 1];
    return &p->tokens[p->index];
}

djs_token_t *djs_peek_at(djs_parser_t *p, size_t offset) {
    if (p->count == 0) return NULL;
    size_t index = p->index + offset;
    if (index >= p->count) index = p->count - 1;
    return &p->tokens[index];
}

void djs_advance(djs_parser_t *p) {
    if (p->index + 1 < p->count) p->index++;
}

bool djs_at_punct(djs_parser_t *p, const char *text) {
    djs_token_t *token = djs_cur(p);
    return token != NULL && token->type == DJS_TOK_PUNCT && djs_strcmp(token->text, text) == 0;
}

bool djs_at_ident(djs_parser_t *p, const char *text) {
    djs_token_t *token = djs_cur(p);
    return token != NULL && token->type == DJS_TOK_IDENTIFIER &&
           djs_strcmp(token->text, text) == 0;
}

bool djs_eat_punct(djs_parser_t *p, const char *text) {
    if (!djs_at_punct(p, text)) return false;
    djs_advance(p);
    return true;
}

bool djs_eat_ident(djs_parser_t *p, const char *text) {
    if (!djs_at_ident(p, text)) return false;
    djs_advance(p);
    return true;
}

void djs_set_error(djs_parser_t *p, const char *message) {
    if (p->error != NULL) return;
    char buffer[320];
    size_t length = 0;
    const char *prefix = "parse error: ";
    for (size_t i = 0; prefix[i] != '\0' && length + 1 < sizeof(buffer); i++) {
        buffer[length++] = prefix[i];
    }
    for (size_t i = 0; message[i] != '\0' && length + 1 < sizeof(buffer); i++) {
        buffer[length++] = message[i];
    }
    buffer[length] = '\0';
    p->error = djs_arena_strdup(p->rt, buffer);
}

bool djs_expect_punct(djs_parser_t *p, const char *text) {
    if (djs_eat_punct(p, text)) return true;
    char message[128];
    size_t length = 0;
    const char *prefix = "expected '";
    for (size_t i = 0; prefix[i] != '\0' && length + 1 < sizeof(message); i++) {
        message[length++] = prefix[i];
    }
    for (size_t i = 0; text[i] != '\0' && length + 2 < sizeof(message); i++) {
        message[length++] = text[i];
    }
    message[length++] = '\'';
    message[length] = '\0';
    djs_set_error(p, message);
    return false;
}

/* ------------------------------------------------------------- entry point */

static int tokenize(djs_parser_t *p, const char *source, char **error_out) {
    djs_lexer_t lexer;
    djs_lexer_init(&lexer, p->rt, source);
    size_t capacity = 64;
    size_t count = 0;
    djs_token_t *tokens = (djs_token_t *)djs_arena_alloc(p->rt, capacity * sizeof(*tokens));
    if (tokens == NULL) return 0;
    while (true) {
        djs_token_t token;
        char *error = NULL;
        if (!djs_lexer_next(&lexer, &token, &error)) {
            *error_out = error ? error : djs_arena_strdup(p->rt, "lex error");
            return 0;
        }
        if (count == capacity) {
            capacity *= 2;
            djs_token_t *grown = (djs_token_t *)djs_arena_alloc(p->rt, capacity * sizeof(*tokens));
            if (grown == NULL) return 0;
            djs_memcpy(grown, tokens, count * sizeof(*tokens));
            tokens = grown;
        }
        tokens[count++] = token;
        if (token.type == DJS_TOK_EOF) break;
    }
    p->tokens = tokens;
    p->count = count;
    p->index = 0;
    return 1;
}

djs_node_t *djs_parse(djs_runtime_t *rt, const char *source, const char *filename,
                      char **error_out) {
    djs_parser_t parser;
    parser.rt = rt;
    parser.tokens = NULL;
    parser.count = 0;
    parser.index = 0;
    parser.filename = filename ? filename : "(eval)";
    parser.error = NULL;
    parser.no_in = 0;

    char *error = NULL;
    if (!tokenize(&parser, source, &error)) {
        *error_out = error ? error : djs_arena_strdup(rt, "lex error");
        return NULL;
    }

    djs_node_t *program = djs_node_new(&parser, DJS_N_PROGRAM);
    while (djs_cur(&parser)->type != DJS_TOK_EOF) {
        djs_node_t *statement = djs_parse_statement(&parser);
        if (statement == NULL) {
            *error_out = parser.error ? parser.error : djs_arena_strdup(rt, "parse error");
            return NULL;
        }
        djs_list_push(&parser, program, statement);
    }
    if (parser.error != NULL) {
        *error_out = parser.error;
        return NULL;
    }
    return program;
}

/* -------------------------------------------------------------- statements */

djs_node_t *djs_parse_block(djs_parser_t *p) {
    djs_node_t *block = djs_node_new(p, DJS_N_BLOCK);
    if (block == NULL) return NULL;
    if (!djs_expect_punct(p, "{")) return NULL;
    while (!djs_at_punct(p, "}")) {
        if (djs_cur(p)->type == DJS_TOK_EOF) {
            djs_set_error(p, "unexpected end of input in block");
            return NULL;
        }
        djs_node_t *statement = djs_parse_statement(p);
        if (statement == NULL) return NULL;
        djs_list_push(p, block, statement);
    }
    if (!djs_expect_punct(p, "}")) return NULL;
    return block;
}

/* var / let / const declarations. */
static djs_node_t *parse_var_statement(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_VAR);
    if (node == NULL) return NULL;
    if (djs_eat_ident(p, "let")) {
        node->flags |= DJS_NODE_LET;
    } else if (djs_eat_ident(p, "const")) {
        node->flags |= DJS_NODE_CONST;
    } else {
        djs_advance(p); /* var */
    }
    while (true) {
        djs_node_t *declarator = djs_node_new(p, DJS_N_DECLARATOR);
        if (declarator == NULL) return NULL;
        declarator->a = djs_parse_binding_target(p);
        if (declarator->a == NULL) return NULL;
        if (djs_eat_punct(p, "=")) {
            declarator->b = djs_parse_assignment(p);
            if (declarator->b == NULL) return NULL;
        }
        djs_list_push(p, node, declarator);
        if (!djs_at_punct(p, ",")) break;
        djs_advance(p);
    }
    djs_eat_punct(p, ";");
    return node;
}

static djs_node_t *parse_if(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_IF);
    if (node == NULL) return NULL;
    djs_advance(p); /* if */
    if (!djs_expect_punct(p, "(")) return NULL;
    node->a = djs_parse_expression(p);
    if (node->a == NULL) return NULL;
    if (!djs_expect_punct(p, ")")) return NULL;
    node->b = djs_parse_statement(p);
    if (node->b == NULL) return NULL;
    if (djs_eat_ident(p, "else")) {
        node->c = djs_parse_statement(p);
        if (node->c == NULL) return NULL;
    }
    return node;
}

static djs_node_t *parse_while(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_WHILE);
    if (node == NULL) return NULL;
    djs_advance(p); /* while */
    if (!djs_expect_punct(p, "(")) return NULL;
    node->a = djs_parse_expression(p);
    if (node->a == NULL) return NULL;
    if (!djs_expect_punct(p, ")")) return NULL;
    node->b = djs_parse_statement(p);
    if (node->b == NULL) return NULL;
    return node;
}

static djs_node_t *parse_do_while(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_DO_WHILE);
    if (node == NULL) return NULL;
    djs_advance(p); /* do */
    node->a = djs_parse_statement(p);
    if (node->a == NULL) return NULL;
    if (!djs_eat_ident(p, "while")) {
        djs_set_error(p, "expected 'while'");
        return NULL;
    }
    if (!djs_expect_punct(p, "(")) return NULL;
    node->b = djs_parse_expression(p);
    if (node->b == NULL) return NULL;
    if (!djs_expect_punct(p, ")")) return NULL;
    djs_eat_punct(p, ";");
    return node;
}

/* for (init; test; update) body  |  for (left of/in right) body */
static djs_node_t *parse_for(djs_parser_t *p) {
    djs_advance(p); /* for */
    if (!djs_expect_punct(p, "(")) return NULL;

    djs_node_t *init = NULL;
    if (djs_at_punct(p, ";")) {
        /* no initialiser */
    } else if (djs_at_ident(p, "var") || djs_at_ident(p, "let") || djs_at_ident(p, "const")) {
        init = parse_var_statement(p);
        if (init == NULL) return NULL;
    } else {
        p->no_in++;
        init = djs_parse_expression(p);
        p->no_in--;
        if (init == NULL) return NULL;
    }

    if (djs_at_ident(p, "of") || djs_at_ident(p, "in")) {
        bool is_of = djs_at_ident(p, "of");
        djs_advance(p);
        djs_node_t *node = djs_node_new(p, is_of ? DJS_N_FOR_OF : DJS_N_FOR_IN);
        if (node == NULL) return NULL;
        if (init != NULL && init->kind == DJS_N_EXPR_STMT) init = init->a;
        if (init != NULL && init->kind == DJS_N_ARRAY) init = djs_node_to_pattern(p, init);
        if (init != NULL && init->kind == DJS_N_OBJECT) init = djs_node_to_pattern(p, init);
        node->a = init;
        node->b = djs_parse_assignment(p);
        if (node->b == NULL) return NULL;
        if (!djs_expect_punct(p, ")")) return NULL;
        node->c = djs_parse_statement(p);
        if (node->c == NULL) return NULL;
        return node;
    }

    djs_node_t *node = djs_node_new(p, DJS_N_FOR);
    if (node == NULL) return NULL;
    node->a = init;
    if (!djs_expect_punct(p, ";")) return NULL;
    if (!djs_at_punct(p, ";")) {
        node->b = djs_parse_expression(p);
        if (node->b == NULL) return NULL;
    }
    if (!djs_expect_punct(p, ";")) return NULL;
    if (!djs_at_punct(p, ")")) {
        node->c = djs_parse_expression(p);
        if (node->c == NULL) return NULL;
    }
    if (!djs_expect_punct(p, ")")) return NULL;
    node->d = djs_parse_statement(p);
    if (node->d == NULL) return NULL;
    return node;
}

static djs_node_t *parse_switch(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_SWITCH);
    if (node == NULL) return NULL;
    djs_advance(p); /* switch */
    if (!djs_expect_punct(p, "(")) return NULL;
    node->a = djs_parse_expression(p);
    if (node->a == NULL) return NULL;
    if (!djs_expect_punct(p, ")")) return NULL;
    if (!djs_expect_punct(p, "{")) return NULL;
    while (!djs_at_punct(p, "}")) {
        djs_node_t *entry = djs_node_new(p, DJS_N_CASE);
        if (entry == NULL) return NULL;
        if (djs_eat_ident(p, "case")) {
            entry->a = djs_parse_expression(p);
            if (entry->a == NULL) return NULL;
        } else if (djs_eat_ident(p, "default")) {
            entry->a = NULL;
        } else {
            djs_set_error(p, "expected 'case' or 'default'");
            return NULL;
        }
        if (!djs_expect_punct(p, ":")) return NULL;
        while (!djs_at_punct(p, "}") && !djs_at_ident(p, "case") && !djs_at_ident(p, "default")) {
            djs_node_t *statement = djs_parse_statement(p);
            if (statement == NULL) return NULL;
            djs_list_push(p, entry, statement);
        }
        djs_list_push(p, node, entry);
    }
    if (!djs_expect_punct(p, "}")) return NULL;
    return node;
}

static djs_node_t *parse_try(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_TRY);
    if (node == NULL) return NULL;
    djs_advance(p); /* try */
    node->a = djs_parse_block(p);
    if (node->a == NULL) return NULL;
    if (djs_eat_ident(p, "catch")) {
        djs_node_t *parameter = NULL;
        if (djs_eat_punct(p, "(")) {
            parameter = djs_parse_binding_target(p);
            if (parameter == NULL) return NULL;
            if (!djs_expect_punct(p, ")")) return NULL;
        }
        node->b = parameter;
        node->c = djs_parse_block(p);
        if (node->c == NULL) return NULL;
    }
    if (djs_eat_ident(p, "finally")) {
        node->d = djs_parse_block(p);
        if (node->d == NULL) return NULL;
    }
    if (node->c == NULL && node->d == NULL) {
        djs_set_error(p, "try without catch or finally");
        return NULL;
    }
    return node;
}

static djs_node_t *parse_break_continue(djs_parser_t *p, bool is_break) {
    djs_node_t *node = djs_node_new(p, is_break ? DJS_N_BREAK : DJS_N_CONTINUE);
    if (node == NULL) return NULL;
    djs_advance(p);
    djs_token_t *token = djs_cur(p);
    if (token->type == DJS_TOK_IDENTIFIER && !token->newline_before &&
        !djs_at_punct(p, ";")) {
        node->text = token->text;
        djs_advance(p);
    }
    djs_eat_punct(p, ";");
    return node;
}

static djs_node_t *parse_return(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_RETURN);
    if (node == NULL) return NULL;
    djs_advance(p); /* return */
    djs_token_t *token = djs_cur(p);
    bool same_line = !token->newline_before;
    if (same_line && !djs_at_punct(p, ";") && !djs_at_punct(p, "}") &&
        token->type != DJS_TOK_EOF) {
        node->a = djs_parse_expression(p);
        if (node->a == NULL) return NULL;
    }
    djs_eat_punct(p, ";");
    return node;
}

static djs_node_t *parse_throw(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_THROW);
    if (node == NULL) return NULL;
    djs_advance(p); /* throw */
    node->a = djs_parse_expression(p);
    if (node->a == NULL) return NULL;
    djs_eat_punct(p, ";");
    return node;
}

/* A labelled statement: `name: statement`. */
static djs_node_t *parse_labeled(djs_parser_t *p, const char *name) {
    djs_node_t *node = djs_node_new(p, DJS_N_LABEL);
    if (node == NULL) return NULL;
    node->text = (char *)name;
    djs_advance(p); /* the label */
    if (!djs_expect_punct(p, ":")) return NULL;
    node->a = djs_parse_statement(p);
    if (node->a == NULL) return NULL;
    return node;
}

static djs_node_t *djs_parse_statement(djs_parser_t *p) {
    djs_token_t *token = djs_cur(p);
    if (token == NULL) return NULL;

    if (token->type == DJS_TOK_PUNCT) {
        if (djs_strcmp(token->text, "{") == 0) return djs_parse_block(p);
        if (djs_strcmp(token->text, ";") == 0) {
            djs_advance(p);
            return djs_node_new(p, DJS_N_EMPTY);
        }
    }

    if (token->type == DJS_TOK_IDENTIFIER) {
        const char *name = token->text;
        if (djs_strcmp(name, "var") == 0 || djs_strcmp(name, "let") == 0 ||
            djs_strcmp(name, "const") == 0) {
            /* `let` is only a declaration when followed by a binding. */
            if (djs_strcmp(name, "let") != 0 || djs_peek_at(p, 1)->type == DJS_TOK_IDENTIFIER ||
                djs_peek_at(p, 1)->type == DJS_TOK_PUNCT) {
                djs_token_t *next = djs_peek_at(p, 1);
                bool is_declaration = next->type == DJS_TOK_IDENTIFIER ||
                                     (next->type == DJS_TOK_PUNCT &&
                                      (djs_strcmp(next->text, "[") == 0 ||
                                       djs_strcmp(next->text, "{") == 0));
                if (is_declaration) return parse_var_statement(p);
            }
        }
        if (djs_strcmp(name, "if") == 0) return parse_if(p);
        if (djs_strcmp(name, "while") == 0) return parse_while(p);
        if (djs_strcmp(name, "do") == 0) return parse_do_while(p);
        if (djs_strcmp(name, "for") == 0) return parse_for(p);
        if (djs_strcmp(name, "switch") == 0) return parse_switch(p);
        if (djs_strcmp(name, "try") == 0) return parse_try(p);
        if (djs_strcmp(name, "throw") == 0) return parse_throw(p);
        if (djs_strcmp(name, "return") == 0) return parse_return(p);
        if (djs_strcmp(name, "break") == 0) return parse_break_continue(p, true);
        if (djs_strcmp(name, "continue") == 0) return parse_break_continue(p, false);
        if (djs_strcmp(name, "function") == 0) {
            djs_node_t *fn = djs_parse_function(p, true);
            return fn;
        }
        if (djs_strcmp(name, "class") == 0) return djs_parse_class(p);
        if (djs_strcmp(name, "async") == 0 &&
            djs_peek_at(p, 1)->type == DJS_TOK_IDENTIFIER &&
            djs_strcmp(djs_peek_at(p, 1)->text, "function") == 0) {
            djs_advance(p);
            return djs_parse_function(p, true);
        }
        /* Labelled statement. */
        djs_token_t *next = djs_peek_at(p, 1);
        if (next->type == DJS_TOK_PUNCT && djs_strcmp(next->text, ":") == 0) {
            return parse_labeled(p, token->text);
        }
    }

    djs_node_t *node = djs_node_new(p, DJS_N_EXPR_STMT);
    if (node == NULL) return NULL;
    node->a = djs_parse_expression(p);
    if (node->a == NULL) return NULL;
    djs_eat_punct(p, ";");
    return node;
}

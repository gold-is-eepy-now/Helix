/* djs_operators.c — operator and arrow function parsing. */

#include "djs_parser.h"

/* Find the token index just past the ')' that matches the '(' at `start`. */
static bool matching_paren(djs_parser_t *p, size_t start, size_t *end) {
    int depth = 0;
    for (size_t i = start; i < p->count; i++) {
        djs_token_t *token = &p->tokens[i];
        if (token->type != DJS_TOK_PUNCT) continue;
        if (djs_strcmp(token->text, "(") == 0 || djs_strcmp(token->text, "[") == 0 ||
            djs_strcmp(token->text, "{") == 0) {
            depth++;
        } else if (djs_strcmp(token->text, ")") == 0 || djs_strcmp(token->text, "]") == 0 ||
                   djs_strcmp(token->text, "}") == 0) {
            depth--;
            if (depth == 0) {
                *end = i;
                return true;
            }
        }
    }
    return false;
}

/* True when the '(' at the current position starts an arrow parameter list. */
static bool arrow_ahead(djs_parser_t *p) {
    size_t end = 0;
    if (!matching_paren(p, p->index, &end)) return false;
    djs_token_t *next = djs_peek_at(p, (end - p->index) + 1);
    return next->type == DJS_TOK_PUNCT && djs_strcmp(next->text, "=>") == 0;
}

/* True when `async` at the current position starts an async arrow function. */
static bool async_arrow_ahead(djs_parser_t *p) {
    djs_token_t *second = djs_peek_at(p, 1);
    if (second->type != DJS_TOK_IDENTIFIER) return false;
    if (djs_strcmp(second->text, "function") == 0) return false;
    djs_token_t *third = djs_peek_at(p, 2);
    if (third->type == DJS_TOK_IDENTIFIER) {
        djs_token_t *fourth = djs_peek_at(p, 3);
        return fourth->type == DJS_TOK_PUNCT && djs_strcmp(fourth->text, "=>") == 0;
    }
    if (third->type == DJS_TOK_PUNCT && djs_strcmp(third->text, "(") == 0) {
        size_t end = 0;
        if (!matching_paren(p, p->index + 2, &end)) return false;
        djs_token_t *next = djs_peek_at(p, (end - p->index) + 1);
        return next->type == DJS_TOK_PUNCT && djs_strcmp(next->text, "=>") == 0;
    }
    return false;
}

/* ------------------------------------------------------------------- unary */

djs_node_t *djs_parse_unary(djs_parser_t *p) {
    djs_token_t *token = djs_cur(p);
    if (token == NULL) return NULL;

    if (token->type == DJS_TOK_PUNCT) {
        const char *op = token->text;
        if (djs_strcmp(op, "!") == 0 || djs_strcmp(op, "~") == 0 ||
            djs_strcmp(op, "+") == 0 || djs_strcmp(op, "-") == 0) {
            djs_advance(p);
            djs_node_t *node = djs_node_new(p, DJS_N_UNARY);
            if (node == NULL) return NULL;
            node->text = (char *)op;
            node->a = djs_parse_unary(p);
            if (node->a == NULL) return NULL;
            return node;
        }
        if (djs_strcmp(op, "++") == 0 || djs_strcmp(op, "--") == 0) {
            djs_advance(p);
            djs_node_t *node = djs_node_new(p, DJS_N_UPDATE);
            if (node == NULL) return NULL;
            node->text = (char *)op;
            node->flags |= DJS_NODE_PREFIX;
            node->a = djs_parse_unary(p);
            if (node->a == NULL) return NULL;
            return node;
        }
    }
    if (token->type == DJS_TOK_IDENTIFIER) {
        const char *op = token->text;
        if (djs_strcmp(op, "typeof") == 0 || djs_strcmp(op, "void") == 0 ||
            djs_strcmp(op, "delete") == 0) {
            djs_advance(p);
            djs_node_t *node = djs_node_new(p, DJS_N_UNARY);
            if (node == NULL) return NULL;
            node->text = (char *)op;
            node->a = djs_parse_unary(p);
            if (node->a == NULL) return NULL;
            return node;
        }
        if (djs_strcmp(op, "await") == 0) {
            djs_advance(p);
            djs_node_t *node = djs_node_new(p, DJS_N_AWAIT);
            if (node == NULL) return NULL;
            node->a = djs_parse_unary(p);
            if (node->a == NULL) return NULL;
            return node;
        }
        if (djs_strcmp(op, "yield") == 0) {
            djs_advance(p);
            djs_node_t *node = djs_node_new(p, DJS_N_YIELD);
            if (node == NULL) return NULL;
            if (djs_at_punct(p, "*")) {
                djs_advance(p);
                node->flags |= DJS_NODE_DELEGATE;
            }
            djs_token_t *next = djs_cur(p);
            bool has_value = next != NULL && next->type != DJS_TOK_EOF && !next->newline_before &&
                             !(next->type == DJS_TOK_PUNCT &&
                               (djs_strcmp(next->text, ";") == 0 || djs_strcmp(next->text, "}") == 0 ||
                                djs_strcmp(next->text, ")") == 0 || djs_strcmp(next->text, "]") == 0 ||
                                djs_strcmp(next->text, ",") == 0));
            if (has_value) {
                node->a = djs_parse_assignment(p);
                if (node->a == NULL) return NULL;
            }
            return node;
        }
    }
    return djs_parse_postfix(p);
}

djs_node_t *djs_parse_postfix(djs_parser_t *p) {
    djs_node_t *expression = djs_parse_left_hand_side(p);
    if (expression == NULL) return NULL;
    djs_token_t *token = djs_cur(p);
    if (token != NULL && token->type == DJS_TOK_PUNCT && !token->newline_before &&
        (djs_strcmp(token->text, "++") == 0 || djs_strcmp(token->text, "--") == 0)) {
        djs_node_t *node = djs_node_new(p, DJS_N_UPDATE);
        if (node == NULL) return NULL;
        node->text = token->text;
        node->a = expression;
        djs_advance(p);
        return node;
    }
    return expression;
}

/* ------------------------------------------------------------------ binary */

static int binary_precedence(djs_parser_t *p, const char **op_out) {
    djs_token_t *token = djs_cur(p);
    if (token == NULL) return 0;
    if (token->type == DJS_TOK_PUNCT) {
        const char *text = token->text;
        if (djs_strcmp(text, "||") == 0 || djs_strcmp(text, "??") == 0) {
            *op_out = text;
            return 1;
        }
        if (djs_strcmp(text, "&&") == 0) { *op_out = text; return 2; }
        if (djs_strcmp(text, "|") == 0) { *op_out = text; return 3; }
        if (djs_strcmp(text, "^") == 0) { *op_out = text; return 4; }
        if (djs_strcmp(text, "&") == 0) { *op_out = text; return 5; }
        if (djs_strcmp(text, "==") == 0 || djs_strcmp(text, "!=") == 0 ||
            djs_strcmp(text, "===") == 0 || djs_strcmp(text, "!==") == 0) {
            *op_out = text;
            return 6;
        }
        if (djs_strcmp(text, "<") == 0 || djs_strcmp(text, ">") == 0 ||
            djs_strcmp(text, "<=") == 0 || djs_strcmp(text, ">=") == 0) {
            *op_out = text;
            return 7;
        }
        if (djs_strcmp(text, "<<") == 0 || djs_strcmp(text, ">>") == 0 ||
            djs_strcmp(text, ">>>") == 0) {
            *op_out = text;
            return 8;
        }
        if (djs_strcmp(text, "+") == 0 || djs_strcmp(text, "-") == 0) {
            *op_out = text;
            return 9;
        }
        if (djs_strcmp(text, "*") == 0 || djs_strcmp(text, "/") == 0 ||
            djs_strcmp(text, "%") == 0) {
            *op_out = text;
            return 10;
        }
        if (djs_strcmp(text, "**") == 0) { *op_out = text; return 11; }
        return 0;
    }
    if (token->type == DJS_TOK_IDENTIFIER) {
        if (djs_strcmp(token->text, "instanceof") == 0) {
            *op_out = "instanceof";
            return 7;
        }
        if (djs_strcmp(token->text, "in") == 0) {
            if (p->no_in > 0) return 0;
            *op_out = "in";
            return 7;
        }
    }
    return 0;
}

djs_node_t *djs_parse_binary(djs_parser_t *p, int min_precedence) {
    djs_node_t *left = djs_parse_unary(p);
    if (left == NULL) return NULL;
    while (true) {
        const char *op = NULL;
        int precedence = binary_precedence(p, &op);
        if (precedence == 0 || precedence < min_precedence) break;
        djs_advance(p);
        int next_minimum = djs_strcmp(op, "**") == 0 ? precedence : precedence + 1;
        djs_node_t *right = djs_parse_binary(p, next_minimum);
        if (right == NULL) return NULL;
        bool logical = djs_strcmp(op, "&&") == 0 || djs_strcmp(op, "||") == 0 ||
                       djs_strcmp(op, "??") == 0;
        djs_node_t *node = djs_node_new(p, logical ? DJS_N_LOGICAL : DJS_N_BINARY);
        if (node == NULL) return NULL;
        node->text = (char *)op;
        node->a = left;
        node->b = right;
        left = node;
    }
    return left;
}

djs_node_t *djs_parse_conditional(djs_parser_t *p) {
    djs_node_t *test = djs_parse_binary(p, 1);
    if (test == NULL) return NULL;
    if (!djs_at_punct(p, "?")) return test;
    djs_advance(p);
    djs_node_t *node = djs_node_new(p, DJS_N_CONDITIONAL);
    if (node == NULL) return NULL;
    node->a = test;
    node->b = djs_parse_assignment(p);
    if (node->b == NULL) return NULL;
    if (!djs_expect_punct(p, ":")) return NULL;
    node->c = djs_parse_assignment(p);
    if (node->c == NULL) return NULL;
    return node;
}

/* -------------------------------------------------------------------- arrow */

djs_node_t *djs_parse_arrow(djs_parser_t *p, bool is_async) {
    djs_node_t *fn = djs_node_new(p, DJS_N_FUNCTION);
    if (fn == NULL) return NULL;
    djs_function_def_t *def = (djs_function_def_t *)djs_arena_calloc(p->rt, 1, sizeof(*def));
    if (def == NULL) return NULL;
    fn->aux = def;
    def->flags = DJS_FN_ARROW;
    if (is_async) def->flags |= DJS_FN_ASYNC;

    if (is_async) djs_advance(p); /* async */
    if (djs_at_punct(p, "(")) {
        if (!djs_parse_params_into(p, fn)) return NULL;
    } else {
        djs_node_t *param = djs_node_new(p, DJS_N_PARAM);
        if (param == NULL) return NULL;
        param->a = djs_parse_binding_target(p);
        if (param->a == NULL) return NULL;
        djs_list_push(p, fn, param);
    }
    if (!djs_expect_punct(p, "=>")) return NULL;

    if (djs_at_punct(p, "{")) {
        def->body = djs_parse_block(p);
        if (def->body == NULL) return NULL;
    } else {
        def->expression_body = true;
        def->body = djs_parse_assignment(p);
        if (def->body == NULL) return NULL;
    }
    return fn;
}

/* --------------------------------------------------------------- assignment */

djs_node_t *djs_parse_assignment(djs_parser_t *p) {
    /* Arrow functions need to be recognised before the expression parser
     * consumes the parameter list. */
    if (djs_at_punct(p, "(") && arrow_ahead(p)) {
        return djs_parse_arrow(p, false);
    }
    if (djs_at_ident(p, "async") && async_arrow_ahead(p)) {
        return djs_parse_arrow(p, true);
    }
    if (djs_cur(p)->type == DJS_TOK_IDENTIFIER &&
        djs_peek_at(p, 1)->type == DJS_TOK_PUNCT &&
        djs_strcmp(djs_peek_at(p, 1)->text, "=>") == 0) {
        return djs_parse_arrow(p, false);
    }

    djs_node_t *left = djs_parse_conditional(p);
    if (left == NULL) return NULL;

    djs_token_t *token = djs_cur(p);
    if (token == NULL || token->type != DJS_TOK_PUNCT) return left;
    const char *op = token->text;
    static const char *const operators[] = {
        "=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=",
        "&=", "|=", "^=", "&&=", "||=", "?\?=", NULL
    };
    for (size_t i = 0; operators[i] != NULL; i++) {
        if (djs_strcmp(op, operators[i]) != 0) continue;
        djs_advance(p);
        djs_node_t *right = djs_parse_assignment(p);
        if (right == NULL) return NULL;
        djs_node_t *node = djs_node_new(p, DJS_N_ASSIGN);
        if (node == NULL) return NULL;
        node->text = (char *)op;
        if (djs_strcmp(op, "=") == 0 &&
            (left->kind == DJS_N_ARRAY || left->kind == DJS_N_OBJECT)) {
            left = djs_node_to_pattern(p, left);
        }
        node->a = left;
        node->b = right;
        return node;
    }
    return left;
}

/* --------------------------------------------------------------- expression */

djs_node_t *djs_parse_expression(djs_parser_t *p) {
    djs_node_t *first = djs_parse_assignment(p);
    if (first == NULL) return NULL;
    if (!djs_at_punct(p, ",")) return first;
    djs_node_t *node = djs_node_new(p, DJS_N_SEQUENCE);
    if (node == NULL) return NULL;
    djs_list_push(p, node, first);
    while (djs_eat_punct(p, ",")) {
        djs_node_t *item = djs_parse_assignment(p);
        if (item == NULL) return NULL;
        djs_list_push(p, node, item);
    }
    return node;
}

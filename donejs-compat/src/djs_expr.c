/* djs_expr.c — expression parsing for the donejs JavaScript dialect. */

#include "djs_parser.h"

/* djs_parse_block lives in djs_parser.c. */

/* ------------------------------------------------------------------ helpers */

djs_node_t *djs_string_node(djs_parser_t *p, const char *text) {
    djs_node_t *node = djs_node_new(p, DJS_N_STRING);
    if (node == NULL) return NULL;
    node->text = (char *)text;
    return node;
}

djs_node_t *djs_ident_node(djs_parser_t *p, const char *text) {
    djs_node_t *node = djs_node_new(p, DJS_N_IDENT);
    if (node == NULL) return NULL;
    node->text = (char *)text;
    return node;
}

/* -------------------------------------------------------------- expressions */

djs_node_t *djs_parse_expression_source(djs_parser_t *p, const char *source) {
    char *error = NULL;
    djs_node_t *program = djs_parse(p->rt, source, p->filename, &error);
    if (program == NULL) {
        djs_set_error(p, error ? error : "invalid template expression");
        return NULL;
    }
    if (program->list_count == 1 && program->list[0]->kind == DJS_N_EXPR_STMT) {
        return program->list[0]->a;
    }
    return program;
}

/* Convert an array or object literal into the matching destructuring
 * pattern, so that `[a, b] = value` works. */
djs_node_t *djs_node_to_pattern(djs_parser_t *p, djs_node_t *node) {
    if (node == NULL) return NULL;
    if (node->kind == DJS_N_ARRAY) {
        djs_node_t *pattern = djs_node_new(p, DJS_N_ARRAY_PATTERN);
        if (pattern == NULL) return NULL;
        for (size_t i = 0; i < node->list_count; i++) {
            djs_node_t *item = node->list[i];
            if (item->kind == DJS_N_SPREAD) {
                djs_node_t *rest = djs_node_new(p, DJS_N_REST);
                rest->a = djs_node_to_pattern(p, item->a);
                djs_list_push(p, pattern, rest);
            } else if (item->kind == DJS_N_ASSIGN) {
                djs_node_t *assign = djs_node_new(p, DJS_N_ASSIGN_PATTERN);
                assign->a = djs_node_to_pattern(p, item->a);
                assign->b = item->b;
                djs_list_push(p, pattern, assign);
            } else {
                djs_list_push(p, pattern, djs_node_to_pattern(p, item));
            }
        }
        return pattern;
    }
    if (node->kind == DJS_N_OBJECT) {
        djs_node_t *pattern = djs_node_new(p, DJS_N_OBJECT_PATTERN);
        if (pattern == NULL) return NULL;
        for (size_t i = 0; i < node->list_count; i++) {
            djs_node_t *property = node->list[i];
            djs_node_t *entry = djs_node_new(p, DJS_N_PROPERTY);
            entry->flags |= property->flags & (DJS_NODE_COMPUTED | DJS_NODE_SPREAD);
            if (property->flags & DJS_NODE_SPREAD) {
                entry->a = djs_node_to_pattern(p, property->a);
            } else {
                entry->a = property->a;
                if (property->flags & DJS_NODE_SHORTHAND) {
                    entry->b = djs_ident_node(p, property->a->text);
                } else {
                    entry->b = djs_node_to_pattern(p, property->b);
                }
            }
            djs_list_push(p, pattern, entry);
        }
        return pattern;
    }
    return node;
}

/* --------------------------------------------------------------- patterns */

static djs_node_t *parse_array_pattern(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_ARRAY_PATTERN);
    if (node == NULL) return NULL;
    djs_advance(p); /* [ */
    while (!djs_at_punct(p, "]")) {
        if (djs_at_punct(p, ",")) {
            djs_advance(p);
            djs_list_push(p, node, djs_node_new(p, DJS_N_EMPTY));
            continue;
        }
        djs_node_t *item = djs_node_new(p, DJS_N_PARAM);
        if (item == NULL) return NULL;
        if (djs_eat_punct(p, "...")) {
            djs_node_t *rest = djs_node_new(p, DJS_N_REST);
            rest->a = djs_parse_binding_target(p);
            if (rest->a == NULL) return NULL;
            item = rest;
        } else {
            item->a = djs_parse_binding_target(p);
            if (item->a == NULL) return NULL;
            if (djs_eat_punct(p, "=")) {
                item->b = djs_parse_assignment(p);
                if (item->b == NULL) return NULL;
            }
        }
        djs_list_push(p, node, item);
        if (!djs_at_punct(p, ",")) break;
        djs_advance(p);
    }
    if (!djs_expect_punct(p, "]")) return NULL;
    return node;
}

static djs_node_t *parse_object_pattern(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_OBJECT_PATTERN);
    if (node == NULL) return NULL;
    djs_advance(p); /* { */
    while (!djs_at_punct(p, "}")) {
        djs_node_t *property = djs_node_new(p, DJS_N_PROPERTY);
        if (property == NULL) return NULL;
        if (djs_eat_punct(p, "...")) {
            property->flags |= DJS_NODE_SPREAD;
            property->a = djs_parse_binding_target(p);
            if (property->a == NULL) return NULL;
        } else {
            if (djs_at_punct(p, "[")) {
                djs_advance(p);
                property->a = djs_parse_assignment(p);
                if (property->a == NULL) return NULL;
                if (!djs_expect_punct(p, "]")) return NULL;
                property->flags |= DJS_NODE_COMPUTED;
            } else {
                djs_token_t *token = djs_cur(p);
                if (token->type != DJS_TOK_STRING && token->type != DJS_TOK_IDENTIFIER) {
                    djs_set_error(p, "expected property name in pattern");
                    return NULL;
                }
                property->a = djs_string_node(p, token->text);
                djs_advance(p);
            }
            if (djs_eat_punct(p, ":")) {
                property->b = djs_parse_binding_target(p);
                if (property->b == NULL) return NULL;
                if (djs_eat_punct(p, "=")) {
                    djs_node_t *assign = djs_node_new(p, DJS_N_ASSIGN_PATTERN);
                    assign->a = property->b;
                    assign->b = djs_parse_assignment(p);
                    if (assign->b == NULL) return NULL;
                    property->b = assign;
                }
            } else {
                property->flags |= DJS_NODE_SHORTHAND;
                property->b = djs_ident_node(p, property->a->text);
                if (djs_eat_punct(p, "=")) {
                    djs_node_t *assign = djs_node_new(p, DJS_N_ASSIGN_PATTERN);
                    assign->a = property->b;
                    assign->b = djs_parse_assignment(p);
                    if (assign->b == NULL) return NULL;
                    property->b = assign;
                }
            }
        }
        djs_list_push(p, node, property);
        if (!djs_at_punct(p, ",")) break;
        djs_advance(p);
    }
    if (!djs_expect_punct(p, "}")) return NULL;
    return node;
}

djs_node_t *djs_parse_binding_target(djs_parser_t *p) {
    if (djs_at_punct(p, "[")) return parse_array_pattern(p);
    if (djs_at_punct(p, "{")) return parse_object_pattern(p);
    djs_token_t *token = djs_cur(p);
    if (token->type == DJS_TOK_IDENTIFIER) {
        djs_advance(p);
        return djs_ident_node(p, token->text);
    }
    djs_set_error(p, "expected a binding name");
    return NULL;
}

/* -------------------------------------------------------- parameters, args */

bool djs_parse_params_into(djs_parser_t *p, djs_node_t *fn) {
    if (!djs_expect_punct(p, "(")) return false;
    while (!djs_at_punct(p, ")")) {
        djs_node_t *param = djs_node_new(p, DJS_N_PARAM);
        if (param == NULL) return false;
        if (djs_eat_punct(p, "...")) {
            djs_node_t *rest = djs_node_new(p, DJS_N_REST);
            if (rest == NULL) return false;
            rest->a = djs_parse_binding_target(p);
            if (rest->a == NULL) return false;
            param = rest;
        } else {
            param->a = djs_parse_binding_target(p);
            if (param->a == NULL) return false;
            if (djs_eat_punct(p, "=")) {
                param->b = djs_parse_assignment(p);
                if (param->b == NULL) return false;
            }
        }
        djs_list_push(p, fn, param);
        if (!djs_at_punct(p, ",")) break;
        djs_advance(p);
    }
    return djs_expect_punct(p, ")");
}

bool djs_parse_arguments_into(djs_parser_t *p, djs_node_t *call) {
    if (!djs_expect_punct(p, "(")) return false;
    while (!djs_at_punct(p, ")")) {
        djs_node_t *argument;
        if (djs_eat_punct(p, "...")) {
            argument = djs_node_new(p, DJS_N_SPREAD);
            if (argument == NULL) return false;
            argument->a = djs_parse_assignment(p);
        } else {
            argument = djs_parse_assignment(p);
        }
        if (argument == NULL) return false;
        djs_list_push(p, call, argument);
        if (!djs_at_punct(p, ",")) break;
        djs_advance(p);
    }
    return djs_expect_punct(p, ")");
}

/* --------------------------------------------------------------- functions */

djs_node_t *djs_parse_function(djs_parser_t *p, bool declaration) {
    djs_node_t *fn = djs_node_new(p, DJS_N_FUNCTION);
    if (fn == NULL) return NULL;
    djs_function_def_t *def = (djs_function_def_t *)djs_arena_calloc(p->rt, 1, sizeof(*def));
    if (def == NULL) return NULL;
    fn->aux = def;

    /* The "function" keyword; "async" was consumed by the caller. */
    if (djs_at_ident(p, "function")) djs_advance(p);
    bool generator = djs_eat_punct(p, "*");
    djs_token_t *token = djs_cur(p);
    if (token->type == DJS_TOK_IDENTIFIER && !djs_at_punct(p, "(")) {
        def->name = token->text;
        djs_advance(p);
    }
    if (!djs_parse_params_into(p, fn)) return NULL;
    def->flags = generator ? DJS_FN_GENERATOR : 0u;
    if (p->pending_async) {
        def->flags |= DJS_FN_ASYNC;
        p->pending_async = false;
    }
    def->body = djs_parse_block(p);
    if (def->body == NULL) return NULL;
    (void)declaration;
    return fn;
}

/* ------------------------------------------------------------------ class */

static djs_node_t *parse_class_member(djs_parser_t *p) {
    djs_node_t *property = djs_node_new(p, DJS_N_PROPERTY);
    if (property == NULL) return NULL;

    bool is_static = false;
    if (djs_at_ident(p, "static")) {
        djs_token_t *next = djs_peek_at(p, 1);
        bool is_member_name = next->type == DJS_TOK_IDENTIFIER || next->type == DJS_TOK_STRING ||
                              (next->type == DJS_TOK_PUNCT &&
                               (djs_strcmp(next->text, "[") == 0 ||
                                djs_strcmp(next->text, "*") == 0));
        if (!is_member_name) {
            is_static = true;
            djs_advance(p);
        }
    }

    bool is_async = false;
    bool is_generator = false;
    int accessor = 0;
    if (djs_at_ident(p, "get") || djs_at_ident(p, "set")) {
        djs_token_t *next = djs_peek_at(p, 1);
        bool name_follows = next->type == DJS_TOK_IDENTIFIER || next->type == DJS_TOK_STRING ||
                            (next->type == DJS_TOK_PUNCT && djs_strcmp(next->text, "[") == 0);
        if (name_follows) {
            accessor = djs_at_ident(p, "get") ? 1 : 2;
            djs_advance(p);
        }
    }
    if (accessor == 0 && djs_at_ident(p, "async")) {
        djs_token_t *next = djs_peek_at(p, 1);
        bool name_follows = next->type == DJS_TOK_IDENTIFIER || next->type == DJS_TOK_STRING ||
                            (next->type == DJS_TOK_PUNCT &&
                             (djs_strcmp(next->text, "[") == 0 ||
                              djs_strcmp(next->text, "*") == 0));
        if (name_follows) {
            is_async = true;
            djs_advance(p);
        }
    }
    if (djs_eat_punct(p, "*")) is_generator = true;

    /* The key. */
    if (djs_at_punct(p, "[")) {
        djs_advance(p);
        property->a = djs_parse_assignment(p);
        if (property->a == NULL) return NULL;
        if (!djs_expect_punct(p, "]")) return NULL;
        property->flags |= DJS_NODE_COMPUTED;
    } else if (djs_at_punct(p, "#")) {
        djs_advance(p);
        djs_token_t *token = djs_cur(p);
        if (token->type != DJS_TOK_IDENTIFIER) {
            djs_set_error(p, "expected a private name");
            return NULL;
        }
        char buffer[256];
        size_t length = 0;
        buffer[length++] = '#';
        for (size_t i = 0; token->text[i] != '\0' && length + 1 < sizeof(buffer); i++) {
            buffer[length++] = token->text[i];
        }
        buffer[length] = '\0';
        property->a = djs_string_node(p, djs_arena_strdup(p->rt, buffer));
        djs_advance(p);
    } else {
        djs_token_t *token = djs_cur(p);
        if (token->type != DJS_TOK_IDENTIFIER && token->type != DJS_TOK_STRING) {
            djs_set_error(p, "expected a class member name");
            return NULL;
        }
        property->a = djs_string_node(p, token->text);
        djs_advance(p);
    }

    if (djs_at_punct(p, "(")) {
        djs_node_t *fn = djs_node_new(p, DJS_N_FUNCTION);
        if (fn == NULL) return NULL;
        djs_function_def_t *def = (djs_function_def_t *)djs_arena_calloc(p->rt, 1, sizeof(*def));
        if (def == NULL) return NULL;
        fn->aux = def;
        def->flags = DJS_FN_METHOD;
        if (is_generator) def->flags |= DJS_FN_GENERATOR;
        if (is_async) def->flags |= DJS_FN_ASYNC;
        if (accessor == 1) def->flags |= DJS_FN_GETTER;
        if (accessor == 2) def->flags |= DJS_FN_SETTER;
        if (!djs_parse_params_into(p, fn)) return NULL;
        def->body = djs_parse_block(p);
        if (def->body == NULL) return NULL;
        property->b = fn;
        property->flags |= DJS_NODE_METHOD;
    } else {
        if (djs_eat_punct(p, "=")) {
            property->b = djs_parse_assignment(p);
            if (property->b == NULL) return NULL;
        }
        djs_eat_punct(p, ";");
    }

    if (is_static) property->flags |= DJS_NODE_STATIC;
    if (accessor == 1) property->flags |= DJS_NODE_GETTER;
    if (accessor == 2) property->flags |= DJS_NODE_SETTER;
    return property;
}

djs_node_t *djs_parse_class(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_CLASS);
    if (node == NULL) return NULL;
    djs_class_def_t *def = (djs_class_def_t *)djs_arena_calloc(p->rt, 1, sizeof(*def));
    if (def == NULL) return NULL;
    node->aux = def;

    djs_advance(p); /* class */
    djs_token_t *token = djs_cur(p);
    if (token->type == DJS_TOK_IDENTIFIER && !djs_at_ident(p, "extends")) {
        def->name = token->text;
        djs_advance(p);
    }
    if (djs_eat_ident(p, "extends")) {
        def->superclass = djs_parse_primary(p);
        if (def->superclass == NULL) return NULL;
    }
    if (!djs_expect_punct(p, "{")) return NULL;
    while (!djs_at_punct(p, "}")) {
        if (djs_cur(p)->type == DJS_TOK_EOF) {
            djs_set_error(p, "unexpected end of input in class body");
            return NULL;
        }
        if (djs_eat_punct(p, ";")) continue;
        djs_node_t *member = parse_class_member(p);
        if (member == NULL) return NULL;
        if (!(member->flags & DJS_NODE_STATIC) && !(member->flags & DJS_NODE_METHOD) &&
            member->b != NULL && djs_at_punct(p, "}")) {
            /* A field named "constructor" is not a constructor. */
        }
        if (!(member->flags & DJS_NODE_STATIC) && member->b != NULL &&
            member->b->kind == DJS_N_FUNCTION && member->a != NULL &&
            djs_strcmp(member->a->text, "constructor") == 0) {
            def->constructor = member;
        } else {
            djs_list_push(p, node, member);
        }
    }
    if (!djs_expect_punct(p, "}")) return NULL;
    return node;
}

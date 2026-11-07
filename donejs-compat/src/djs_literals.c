/* djs_literals.c — literal and member/call expression parsing. */

#include "djs_parser.h"

/* ---------------------------------------------------------------- template */

djs_node_t *djs_parse_template(djs_parser_t *p) {
    djs_token_t *token = djs_cur(p);
    djs_template_t *tmpl = (djs_template_t *)token->extra;
    djs_node_t *node = djs_node_new(p, DJS_N_TEMPLATE);
    if (node == NULL) return NULL;
    djs_advance(p);
    if (tmpl == NULL) return node;
    for (size_t i = 0; i < tmpl->count; i++) {
        djs_template_part_t *part = &tmpl->parts[i];
        if (part->is_expression) {
            djs_node_t *expression = djs_parse_expression_source(p, part->text);
            if (expression == NULL) return NULL;
            djs_list_push(p, node, expression);
        } else {
            djs_node_t *text = djs_node_new(p, DJS_N_STRING);
            if (text == NULL) return NULL;
            text->text = part->text;
            text->text2 = part->raw;
            djs_list_push(p, node, text);
        }
    }
    return node;
}

/* ------------------------------------------------------------------ object */

djs_node_t *djs_parse_object_literal(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_OBJECT);
    if (node == NULL) return NULL;
    djs_advance(p); /* { */
    while (!djs_at_punct(p, "}")) {
        djs_node_t *property = djs_node_new(p, DJS_N_PROPERTY);
        if (property == NULL) return NULL;

        if (djs_eat_punct(p, "...")) {
            property->flags |= DJS_NODE_SPREAD;
            property->a = djs_parse_assignment(p);
            if (property->a == NULL) return NULL;
        } else {
            bool is_async = false;
            bool is_generator = false;
            int accessor = 0;

            if (djs_at_ident(p, "get") || djs_at_ident(p, "set")) {
                djs_token_t *next = djs_peek_at(p, 1);
                bool name_follows = next->type == DJS_TOK_IDENTIFIER ||
                                    next->type == DJS_TOK_STRING ||
                                    (next->type == DJS_TOK_PUNCT &&
                                     djs_strcmp(next->text, "[") == 0);
                if (name_follows) {
                    accessor = djs_at_ident(p, "get") ? 1 : 2;
                    djs_advance(p);
                }
            }
            if (accessor == 0 && djs_at_ident(p, "async")) {
                djs_token_t *next = djs_peek_at(p, 1);
                bool name_follows = !(next->type == DJS_TOK_PUNCT &&
                                      (djs_strcmp(next->text, "(") == 0 ||
                                       djs_strcmp(next->text, ":") == 0 ||
                                       djs_strcmp(next->text, ",") == 0 ||
                                       djs_strcmp(next->text, "}") == 0 ||
                                       djs_strcmp(next->text, "=") == 0));
                if (name_follows) {
                    is_async = true;
                    djs_advance(p);
                }
            }
            if (djs_eat_punct(p, "*")) is_generator = true;

            if (djs_at_punct(p, "[")) {
                djs_advance(p);
                property->a = djs_parse_assignment(p);
                if (property->a == NULL) return NULL;
                if (!djs_expect_punct(p, "]")) return NULL;
                property->flags |= DJS_NODE_COMPUTED;
            } else {
                djs_token_t *token = djs_cur(p);
                if (token->type != DJS_TOK_STRING && token->type != DJS_TOK_IDENTIFIER) {
                    djs_set_error(p, "expected a property name");
                    return NULL;
                }
                property->a = djs_string_node(p, token->text);
                djs_advance(p);
            }

            if (djs_at_punct(p, "(")) {
                djs_node_t *fn = djs_node_new(p, DJS_N_FUNCTION);
                if (fn == NULL) return NULL;
                djs_function_def_t *def =
                    (djs_function_def_t *)djs_arena_calloc(p->rt, 1, sizeof(*def));
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
            } else if (djs_eat_punct(p, ":")) {
                property->b = djs_parse_assignment(p);
                if (property->b == NULL) return NULL;
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
            if (accessor == 1) property->flags |= DJS_NODE_GETTER;
            if (accessor == 2) property->flags |= DJS_NODE_SETTER;
        }
        djs_list_push(p, node, property);
        if (!djs_at_punct(p, ",")) break;
        djs_advance(p);
    }
    if (!djs_expect_punct(p, "}")) return NULL;
    return node;
}

/* ------------------------------------------------------------------- array */

djs_node_t *djs_parse_array_literal(djs_parser_t *p) {
    djs_node_t *node = djs_node_new(p, DJS_N_ARRAY);
    if (node == NULL) return NULL;
    djs_advance(p); /* [ */
    while (!djs_at_punct(p, "]")) {
        if (djs_at_punct(p, ",")) {
            djs_advance(p);
            djs_list_push(p, node, djs_node_new(p, DJS_N_EMPTY));
            continue;
        }
        djs_node_t *item;
        if (djs_eat_punct(p, "...")) {
            item = djs_node_new(p, DJS_N_SPREAD);
            if (item == NULL) return NULL;
            item->a = djs_parse_assignment(p);
        } else {
            item = djs_parse_assignment(p);
        }
        if (item == NULL) return NULL;
        djs_list_push(p, node, item);
        if (!djs_at_punct(p, ",")) break;
        djs_advance(p);
    }
    if (!djs_expect_punct(p, "]")) return NULL;
    return node;
}

/* ----------------------------------------------------------------- primary */

static djs_node_t *parse_new(djs_parser_t *p) {
    djs_advance(p); /* new */
    if (djs_at_punct(p, ".")) {
        djs_advance(p);
        djs_token_t *token = djs_cur(p);
        if (token->type != DJS_TOK_IDENTIFIER || djs_strcmp(token->text, "target") != 0) {
            djs_set_error(p, "expected 'new.target'");
            return NULL;
        }
        djs_advance(p);
        return djs_node_new(p, DJS_N_NEWTARGET);
    }
    djs_node_t *callee = djs_parse_primary(p);
    if (callee == NULL) return NULL;
    while (djs_at_punct(p, ".") || djs_at_punct(p, "[")) {
        djs_node_t *member = djs_node_new(p, DJS_N_MEMBER);
        if (member == NULL) return NULL;
        member->a = callee;
        if (djs_eat_punct(p, ".")) {
            djs_token_t *token = djs_cur(p);
            if (token->type != DJS_TOK_IDENTIFIER) {
                djs_set_error(p, "expected a property name");
                return NULL;
            }
            member->b = djs_string_node(p, token->text);
            djs_advance(p);
        } else {
            djs_advance(p); /* [ */
            member->b = djs_parse_expression(p);
            if (member->b == NULL) return NULL;
            if (!djs_expect_punct(p, "]")) return NULL;
            member->flags |= DJS_NODE_COMPUTED;
        }
        callee = member;
    }
    djs_node_t *node = djs_node_new(p, DJS_N_NEW);
    if (node == NULL) return NULL;
    node->a = callee;
    if (djs_at_punct(p, "(")) {
        if (!djs_parse_arguments_into(p, node)) return NULL;
    }
    return node;
}

djs_node_t *djs_parse_primary(djs_parser_t *p) {
    djs_token_t *token = djs_cur(p);
    if (token == NULL) {
        djs_set_error(p, "unexpected end of input");
        return NULL;
    }

    if (token->type == DJS_TOK_NUMBER) {
        djs_node_t *node = djs_node_new(p, DJS_N_NUMBER);
        if (node == NULL) return NULL;
        node->number = token->number;
        djs_advance(p);
        return node;
    }
    if (token->type == DJS_TOK_STRING) {
        djs_node_t *node = djs_node_new(p, DJS_N_STRING);
        if (node == NULL) return NULL;
        node->text = token->text;
        djs_advance(p);
        return node;
    }
    if (token->type == DJS_TOK_REGEXP) {
        djs_node_t *node = djs_node_new(p, DJS_N_REGEXP);
        if (node == NULL) return NULL;
        node->text = token->text;
        node->text2 = token->text2;
        djs_advance(p);
        return node;
    }
    if (token->type == DJS_TOK_TEMPLATE) {
        return djs_parse_template(p);
    }

    if (token->type == DJS_TOK_IDENTIFIER) {
        const char *name = token->text;
        if (djs_strcmp(name, "true") == 0 || djs_strcmp(name, "false") == 0) {
            djs_node_t *node = djs_node_new(p, DJS_N_BOOLEAN);
            if (node == NULL) return NULL;
            node->number = djs_strcmp(name, "true") == 0 ? 1.0 : 0.0;
            djs_advance(p);
            return node;
        }
        if (djs_strcmp(name, "null") == 0) {
            djs_advance(p);
            return djs_node_new(p, DJS_N_NULL);
        }
        if (djs_strcmp(name, "undefined") == 0) {
            djs_advance(p);
            return djs_node_new(p, DJS_N_UNDEFINED);
        }
        if (djs_strcmp(name, "this") == 0) {
            djs_advance(p);
            return djs_node_new(p, DJS_N_THIS);
        }
        if (djs_strcmp(name, "super") == 0) {
            djs_advance(p);
            return djs_node_new(p, DJS_N_SUPER);
        }
        if (djs_strcmp(name, "new") == 0) {
            return parse_new(p);
        }
        if (djs_strcmp(name, "function") == 0) {
            return djs_parse_function(p, false);
        }
        if (djs_strcmp(name, "class") == 0) {
            return djs_parse_class(p);
        }
        if (djs_strcmp(name, "async") == 0 &&
            djs_peek_at(p, 1)->type == DJS_TOK_IDENTIFIER &&
            djs_strcmp(djs_peek_at(p, 1)->text, "function") == 0) {
            djs_advance(p);
            p->pending_async = true;
            return djs_parse_function(p, false);
        }
        if (djs_strcmp(name, "import") == 0) {
            djs_set_error(p, "ES modules are not supported by this runtime");
            return NULL;
        }
        djs_advance(p);
        return djs_ident_node(p, name);
    }

    if (token->type == DJS_TOK_PUNCT) {
        const char *text = token->text;
        if (djs_strcmp(text, "(") == 0) {
            djs_advance(p);
            djs_node_t *expression = djs_parse_expression(p);
            if (expression == NULL) return NULL;
            if (!djs_expect_punct(p, ")")) return NULL;
            return expression;
        }
        if (djs_strcmp(text, "[") == 0) return djs_parse_array_literal(p);
        if (djs_strcmp(text, "{") == 0) return djs_parse_object_literal(p);
        if (djs_strcmp(text, "#") == 0) {
            djs_set_error(p, "private names are only allowed inside class bodies");
            return NULL;
        }
    }

    djs_set_error(p, "unexpected token");
    return NULL;
}

/* --------------------------------------------------------- member and call */

djs_node_t *djs_parse_left_hand_side(djs_parser_t *p) {
    djs_node_t *expression = djs_parse_primary(p);
    if (expression == NULL) return NULL;
    while (true) {
        if (djs_at_punct(p, ".")) {
            djs_advance(p);
            djs_node_t *member = djs_node_new(p, DJS_N_MEMBER);
            if (member == NULL) return NULL;
            member->a = expression;
            if (djs_at_punct(p, "#")) {
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
                member->b = djs_string_node(p, djs_arena_strdup(p->rt, buffer));
            } else {
                djs_token_t *token = djs_cur(p);
                if (token->type != DJS_TOK_IDENTIFIER) {
                    djs_set_error(p, "expected a property name");
                    return NULL;
                }
                member->b = djs_string_node(p, token->text);
            }
            djs_advance(p);
            expression = member;
            continue;
        }
        if (djs_at_punct(p, "?.")) {
            djs_advance(p);
            if (djs_at_punct(p, "(")) {
                djs_node_t *call = djs_node_new(p, DJS_N_CALL);
                if (call == NULL) return NULL;
                call->a = expression;
                call->flags |= DJS_NODE_OPTIONAL;
                if (!djs_parse_arguments_into(p, call)) return NULL;
                expression = call;
                continue;
            }
            if (djs_at_punct(p, "[")) {
                djs_advance(p);
                djs_node_t *member = djs_node_new(p, DJS_N_MEMBER);
                if (member == NULL) return NULL;
                member->a = expression;
                member->b = djs_parse_expression(p);
                if (member->b == NULL) return NULL;
                if (!djs_expect_punct(p, "]")) return NULL;
                member->flags |= DJS_NODE_COMPUTED | DJS_NODE_OPTIONAL;
                expression = member;
                continue;
            }
            djs_token_t *token = djs_cur(p);
            if (token->type != DJS_TOK_IDENTIFIER) {
                djs_set_error(p, "expected a property name");
                return NULL;
            }
            djs_node_t *member = djs_node_new(p, DJS_N_MEMBER);
            if (member == NULL) return NULL;
            member->a = expression;
            member->b = djs_string_node(p, token->text);
            member->flags |= DJS_NODE_OPTIONAL;
            djs_advance(p);
            expression = member;
            continue;
        }
        if (djs_at_punct(p, "[")) {
            djs_advance(p);
            djs_node_t *member = djs_node_new(p, DJS_N_MEMBER);
            if (member == NULL) return NULL;
            member->a = expression;
            member->b = djs_parse_expression(p);
            if (member->b == NULL) return NULL;
            if (!djs_expect_punct(p, "]")) return NULL;
            member->flags |= DJS_NODE_COMPUTED;
            expression = member;
            continue;
        }
        if (djs_at_punct(p, "(")) {
            djs_node_t *call = djs_node_new(p, DJS_N_CALL);
            if (call == NULL) return NULL;
            call->a = expression;
            if (!djs_parse_arguments_into(p, call)) return NULL;
            expression = call;
            continue;
        }
        if (djs_cur(p)->type == DJS_TOK_TEMPLATE) {
            djs_node_t *tagged = djs_node_new(p, DJS_N_TAGGED);
            if (tagged == NULL) return NULL;
            tagged->a = expression;
            tagged->b = djs_parse_template(p);
            if (tagged->b == NULL) return NULL;
            expression = tagged;
            continue;
        }
        break;
    }
    return expression;
}

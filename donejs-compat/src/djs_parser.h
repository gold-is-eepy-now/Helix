/* djs_parser.h — parser state shared by the statement and expression
 * parsers (djs_parser.c and djs_expr.c). */

#ifndef DJS_PARSER_H
#define DJS_PARSER_H

#include "djs_internal.h"

typedef struct djs_parser {
    djs_runtime_t *rt;
    djs_token_t *tokens;
    size_t count;
    size_t index;
    const char *filename;
    char *error;
    int no_in;                /* depth of "in" operator suppression */
    bool pending_async;       /* the next function is an async function */
} djs_parser_t;

/* Shared helpers (djs_parser.c). */
djs_node_t *djs_node_new(djs_parser_t *p, int kind);
void djs_list_push(djs_parser_t *p, djs_node_t *node, djs_node_t *item);
djs_token_t *djs_cur(djs_parser_t *p);
djs_token_t *djs_peek_at(djs_parser_t *p, size_t offset);
void djs_advance(djs_parser_t *p);
bool djs_at_punct(djs_parser_t *p, const char *text);
bool djs_at_ident(djs_parser_t *p, const char *text);
bool djs_eat_punct(djs_parser_t *p, const char *text);
bool djs_eat_ident(djs_parser_t *p, const char *text);
void djs_set_error(djs_parser_t *p, const char *message);
bool djs_expect_punct(djs_parser_t *p, const char *text);

/* Shared expression helpers (djs_expr.c). */
djs_node_t *djs_parse_expression(djs_parser_t *p);
djs_node_t *djs_parse_assignment(djs_parser_t *p);
djs_node_t *djs_parse_binding_target(djs_parser_t *p);
bool djs_parse_params_into(djs_parser_t *p, djs_node_t *fn);
bool djs_parse_arguments_into(djs_parser_t *p, djs_node_t *call);
djs_node_t *djs_parse_expression_source(djs_parser_t *p, const char *source);
djs_node_t *djs_node_to_pattern(djs_parser_t *p, djs_node_t *node);
djs_node_t *djs_parse_function(djs_parser_t *p, bool declaration);
djs_node_t *djs_parse_class(djs_parser_t *p);
djs_node_t *djs_parse_block(djs_parser_t *p);

/* Forward declarations shared by the expression parser files. */
djs_node_t *djs_parse_primary(djs_parser_t *p);
djs_node_t *djs_parse_left_hand_side(djs_parser_t *p);
djs_node_t *djs_parse_unary(djs_parser_t *p);
djs_node_t *djs_parse_postfix(djs_parser_t *p);
djs_node_t *djs_parse_binary(djs_parser_t *p, int min_precedence);
djs_node_t *djs_parse_conditional(djs_parser_t *p);
djs_node_t *djs_parse_arrow(djs_parser_t *p, bool is_async);
djs_node_t *djs_parse_template(djs_parser_t *p);
djs_node_t *djs_parse_object_literal(djs_parser_t *p);
djs_node_t *djs_parse_array_literal(djs_parser_t *p);
djs_node_t *djs_string_node(djs_parser_t *p, const char *text);
djs_node_t *djs_ident_node(djs_parser_t *p, const char *text);

#endif /* DJS_PARSER_H */

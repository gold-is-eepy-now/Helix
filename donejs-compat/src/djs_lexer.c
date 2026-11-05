/* djs_lexer.c — tokenizer for the donejs JavaScript dialect.
 *
 * The lexer is pull-based: the parser asks for the next token and decides
 * whether a '/' starts a regular expression or is a division, using the
 * `regex_allowed` flag that the lexer maintains from the previous token.
 */

#include "djs_internal.h"

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
           (unsigned char)c >= 0x80;
}

static bool is_ident_part(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

void djs_lexer_init(djs_lexer_t *lexer, djs_runtime_t *rt, const char *source) {
    lexer->rt = rt;
    lexer->source = source;
    lexer->length = djs_strlen(source);
    lexer->position = 0;
    lexer->line = 1;
    lexer->regex_allowed = true;
    lexer->saw_newline = false;
}

/* Append a Unicode code point as UTF-8. */
static void append_utf8(char *buffer, size_t *length, unsigned int code) {
    if (code < 0x80) {
        buffer[(*length)++] = (char)code;
    } else if (code < 0x800) {
        buffer[(*length)++] = (char)(0xC0 | (code >> 6));
        buffer[(*length)++] = (char)(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        buffer[(*length)++] = (char)(0xE0 | (code >> 12));
        buffer[(*length)++] = (char)(0x80 | ((code >> 6) & 0x3F));
        buffer[(*length)++] = (char)(0x80 | (code & 0x3F));
    } else {
        buffer[(*length)++] = (char)(0xF0 | (code >> 18));
        buffer[(*length)++] = (char)(0x80 | ((code >> 12) & 0x3F));
        buffer[(*length)++] = (char)(0x80 | ((code >> 6) & 0x3F));
        buffer[(*length)++] = (char)(0x80 | (code & 0x3F));
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Skip whitespace and comments. */
static void skip_trivia(djs_lexer_t *lexer) {
    lexer->saw_newline = false;
    while (lexer->position < lexer->length) {
        char c = lexer->source[lexer->position];
        if (c == '\n') {
            lexer->line++;
            lexer->saw_newline = true;
            lexer->position++;
        } else if (is_space(c)) {
            lexer->position++;
        } else if (c == '/' && lexer->position + 1 < lexer->length &&
                   lexer->source[lexer->position + 1] == '/') {
            while (lexer->position < lexer->length && lexer->source[lexer->position] != '\n') {
                lexer->position++;
            }
        } else if (c == '/' && lexer->position + 1 < lexer->length &&
                   lexer->source[lexer->position + 1] == '*') {
            lexer->position += 2;
            while (lexer->position < lexer->length) {
                if (lexer->source[lexer->position] == '*' &&
                    lexer->position + 1 < lexer->length &&
                    lexer->source[lexer->position + 1] == '/') {
                    lexer->position += 2;
                    break;
                }
                if (lexer->source[lexer->position] == '\n') {
                    lexer->line++;
                    lexer->saw_newline = true;
                }
                lexer->position++;
            }
        } else {
            return;
        }
    }
}

/* Read a quoted string literal. The opening quote must be current. */
static bool read_string(djs_lexer_t *lexer, djs_token_t *token, char **error_out) {
    char quote = lexer->source[lexer->position++];
    size_t capacity = 32;
    size_t length = 0;
    char *buffer = (char *)djs_arena_alloc(lexer->rt, capacity);
    if (buffer == NULL) return false;

    while (lexer->position < lexer->length) {
        char c = lexer->source[lexer->position];
        if (c == quote) {
            lexer->position++;
            buffer[length] = '\0';
            token->type = DJS_TOK_STRING;
            token->text = buffer;
            return true;
        }
        if (c == '\n') {
            *error_out = djs_arena_strdup(lexer->rt, "unterminated string literal");
            return false;
        }
        if (c == '\\') {
            lexer->position++;
            if (lexer->position >= lexer->length) break;
            char escape = lexer->source[lexer->position++];
            char decoded = 0;
            switch (escape) {
            case 'n': decoded = '\n'; break;
            case 't': decoded = '\t'; break;
            case 'r': decoded = '\r'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'v': decoded = '\v'; break;
            case '0': decoded = '\0'; break;
            case '\n': continue;
            case 'x': {
                int high = hex_value(lexer->source[lexer->position]);
                int low = lexer->position + 1 < lexer->length
                              ? hex_value(lexer->source[lexer->position + 1]) : -1;
                if (high < 0 || low < 0) {
                    *error_out = djs_arena_strdup(lexer->rt, "invalid hex escape");
                    return false;
                }
                lexer->position += 2;
                decoded = (char)((high << 4) | low);
                break;
            }
            case 'u': {
                unsigned int code = 0;
                if (lexer->position < lexer->length && lexer->source[lexer->position] == '{') {
                    lexer->position++;
                    while (lexer->position < lexer->length &&
                           lexer->source[lexer->position] != '}') {
                        int digit = hex_value(lexer->source[lexer->position]);
                        if (digit < 0) break;
                        code = code * 16 + (unsigned int)digit;
                        lexer->position++;
                    }
                    if (lexer->position < lexer->length) lexer->position++;
                } else {
                    for (int i = 0; i < 4 && lexer->position < lexer->length; i++) {
                        int digit = hex_value(lexer->source[lexer->position]);
                        if (digit < 0) {
                            *error_out = djs_arena_strdup(lexer->rt, "invalid unicode escape");
                            return false;
                        }
                        code = code * 16 + (unsigned int)digit;
                        lexer->position++;
                    }
                }
                if (length + 4 >= capacity) {
                    capacity *= 2;
                    char *grown = (char *)djs_arena_alloc(lexer->rt, capacity);
                    if (grown == NULL) return false;
                    djs_memcpy(grown, buffer, length);
                    buffer = grown;
                }
                append_utf8(buffer, &length, code);
                continue;
            }
            default: decoded = escape; break;
            }
            if (length + 1 >= capacity) {
                capacity *= 2;
                char *grown = (char *)djs_arena_alloc(lexer->rt, capacity);
                if (grown == NULL) return false;
                djs_memcpy(grown, buffer, length);
                buffer = grown;
            }
            buffer[length++] = decoded;
            continue;
        }
        if (length + 1 >= capacity) {
            capacity *= 2;
            char *grown = (char *)djs_arena_alloc(lexer->rt, capacity);
            if (grown == NULL) return false;
            djs_memcpy(grown, buffer, length);
            buffer = grown;
        }
        buffer[length++] = c;
        lexer->position++;
    }
    *error_out = djs_arena_strdup(lexer->rt, "unterminated string literal");
    return false;
}

/* Read a numeric literal. */
static bool read_number(djs_lexer_t *lexer, djs_token_t *token, char **error_out) {
    size_t start = lexer->position;

    if (lexer->source[lexer->position] == '0' && lexer->position + 1 < lexer->length) {
        char marker = lexer->source[lexer->position + 1];
        int base = 0;
        if (marker == 'x' || marker == 'X') base = 16;
        else if (marker == 'o' || marker == 'O') base = 8;
        else if (marker == 'b' || marker == 'B') base = 2;
        if (base != 0) {
            lexer->position += 2;
            double result = 0.0;
            bool any = false;
            while (lexer->position < lexer->length) {
                char c = lexer->source[lexer->position];
                int digit;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                else break;
                if (digit >= base) {
                    *error_out = djs_arena_strdup(lexer->rt, "invalid digits in numeric literal");
                    return false;
                }
                result = result * (double)base + (double)digit;
                any = true;
                lexer->position++;
            }
            if (!any) {
                *error_out = djs_arena_strdup(lexer->rt, "invalid numeric literal");
                return false;
            }
            token->type = DJS_TOK_NUMBER;
            token->number = result;
            return true;
        }
    }

    while (lexer->position < lexer->length && is_digit(lexer->source[lexer->position])) {
        lexer->position++;
    }
    if (lexer->position < lexer->length && lexer->source[lexer->position] == '.') {
        lexer->position++;
        while (lexer->position < lexer->length && is_digit(lexer->source[lexer->position])) {
            lexer->position++;
        }
    }
    if (lexer->position < lexer->length &&
        (lexer->source[lexer->position] == 'e' || lexer->source[lexer->position] == 'E')) {
        size_t save = lexer->position;
        lexer->position++;
        if (lexer->position < lexer->length &&
            (lexer->source[lexer->position] == '+' || lexer->source[lexer->position] == '-')) {
            lexer->position++;
        }
        if (lexer->position < lexer->length && is_digit(lexer->source[lexer->position])) {
            while (lexer->position < lexer->length && is_digit(lexer->source[lexer->position])) {
                lexer->position++;
            }
        } else {
            lexer->position = save;
        }
    }

    size_t text_length = lexer->position - start;
    char *text = djs_arena_strndup(lexer->rt, lexer->source + start, text_length);
    if (text == NULL) return false;
    double parsed = 0.0;
    if (!djs_string_to_number(text, text_length, &parsed)) {
        *error_out = djs_arena_strdup(lexer->rt, "invalid numeric literal");
        return false;
    }
    token->type = DJS_TOK_NUMBER;
    token->number = parsed;
    return true;
}

/* Read a regular expression literal. */
static bool read_regexp(djs_lexer_t *lexer, djs_token_t *token, char **error_out) {
    size_t start = lexer->position;
    lexer->position++; /* the opening slash */
    bool in_class = false;
    while (lexer->position < lexer->length) {
        char c = lexer->source[lexer->position];
        if (c == '\\' && lexer->position + 1 < lexer->length) {
            lexer->position += 2;
            continue;
        }
        if (c == '[') in_class = true;
        else if (c == ']') in_class = false;
        else if (c == '/' && !in_class) break;
        else if (c == '\n') {
            *error_out = djs_arena_strdup(lexer->rt, "unterminated regular expression");
            return false;
        }
        lexer->position++;
    }
    if (lexer->position >= lexer->length) {
        *error_out = djs_arena_strdup(lexer->rt, "unterminated regular expression");
        return false;
    }
    size_t pattern_length = lexer->position - start - 1;
    char *pattern = djs_arena_strndup(lexer->rt, lexer->source + start + 1, pattern_length);
    lexer->position++; /* closing slash */

    size_t flags_start = lexer->position;
    while (lexer->position < lexer->length && is_ident_part(lexer->source[lexer->position])) {
        lexer->position++;
    }
    char *flags = djs_arena_strndup(lexer->rt, lexer->source + flags_start,
                                    lexer->position - flags_start);
    token->type = DJS_TOK_REGEXP;
    token->text = pattern;
    token->text2 = flags;
    return true;
}

/* Scan a template literal starting at the current backtick. Produces an array
 * of parts: string chunks (cooked and raw) and substitution sources. */
static bool read_template(djs_lexer_t *lexer, djs_token_t *token, char **error_out) {
    size_t capacity = 8;
    size_t count = 0;
    djs_template_t *template = (djs_template_t *)djs_arena_alloc(lexer->rt, sizeof(*template));
    if (template == NULL) return false;
    djs_template_part_t *parts =
        (djs_template_part_t *)djs_arena_alloc(lexer->rt, capacity * sizeof(*parts));
    if (parts == NULL) return false;

    size_t cooked_capacity = 32;
    size_t cooked_length = 0;
    size_t raw_length = 0;
    char *cooked = (char *)djs_arena_alloc(lexer->rt, cooked_capacity);
    char *raw = (char *)djs_arena_alloc(lexer->rt, cooked_capacity);
    if (cooked == NULL || raw == NULL) return false;

    lexer->position++; /* the opening backtick */
    while (true) {
        if (lexer->position >= lexer->length) {
            *error_out = djs_arena_strdup(lexer->rt, "unterminated template literal");
            return false;
        }
        char c = lexer->source[lexer->position];
        if (c == '`') {
            lexer->position++;
            break;
        }
        if (c == '$' && lexer->position + 1 < lexer->length &&
            lexer->source[lexer->position + 1] == '{') {
            /* Close the current chunk and record the substitution. */
            if (count + 2 > capacity) {
                capacity *= 2;
                djs_template_part_t *grown = (djs_template_part_t *)djs_arena_alloc(
                    lexer->rt, capacity * sizeof(*grown));
                if (grown == NULL) return false;
                djs_memcpy(grown, parts, count * sizeof(*grown));
                parts = grown;
            }
            parts[count].is_expression = false;
            parts[count].text = djs_arena_strndup(lexer->rt, cooked, cooked_length);
            parts[count].raw = djs_arena_strndup(lexer->rt, raw, raw_length);
            count++;

            lexer->position += 2;
            size_t expression_start = lexer->position;
            int depth = 1;
            while (lexer->position < lexer->length) {
                char e = lexer->source[lexer->position];
                if (e == '{') {
                    depth++;
                    lexer->position++;
                } else if (e == '}') {
                    depth--;
                    lexer->position++;
                    if (depth == 0) break;
                } else if (e == '\'' || e == '"') {
                    char quote = e;
                    lexer->position++;
                    while (lexer->position < lexer->length &&
                           lexer->source[lexer->position] != quote) {
                        if (lexer->source[lexer->position] == '\\') lexer->position++;
                        if (lexer->position < lexer->length &&
                            lexer->source[lexer->position] == '\n') {
                            lexer->line++;
                        }
                        lexer->position++;
                    }
                    if (lexer->position < lexer->length) lexer->position++;
                } else if (e == '`') {
                    /* Nested template: skip it wholesale. */
                    lexer->position++;
                    int nested = 1;
                    while (lexer->position < lexer->length && nested > 0) {
                        char n = lexer->source[lexer->position];
                        if (n == '\\') {
                            lexer->position += 2;
                            continue;
                        }
                        if (n == '`') nested--;
                        else if (n == '$' && lexer->position + 1 < lexer->length &&
                                 lexer->source[lexer->position + 1] == '{') {
                            int inner = 1;
                            lexer->position += 2;
                            while (lexer->position < lexer->length && inner > 0) {
                                char i = lexer->source[lexer->position];
                                if (i == '{') inner++;
                                else if (i == '}') inner--;
                                lexer->position++;
                            }
                            continue;
                        }
                        lexer->position++;
                    }
                } else if (e == '/' && lexer->position + 1 < lexer->length &&
                           lexer->source[lexer->position + 1] == '/') {
                    while (lexer->position < lexer->length &&
                           lexer->source[lexer->position] != '\n') {
                        lexer->position++;
                    }
                } else if (e == '/' && lexer->position + 1 < lexer->length &&
                           lexer->source[lexer->position + 1] == '*') {
                    lexer->position += 2;
                    while (lexer->position + 1 < lexer->length &&
                           !(lexer->source[lexer->position] == '*' &&
                             lexer->source[lexer->position + 1] == '/')) {
                        if (lexer->source[lexer->position] == '\n') lexer->line++;
                        lexer->position++;
                    }
                    lexer->position += 2;
                } else {
                    if (e == '\n') lexer->line++;
                    lexer->position++;
                }
            }
            if (depth != 0) {
                *error_out = djs_arena_strdup(lexer->rt, "unterminated template substitution");
                return false;
            }
            size_t expression_length = lexer->position - expression_start - 1;
            parts[count].is_expression = true;
            parts[count].text = djs_arena_strndup(lexer->rt, lexer->source + expression_start,
                                                  expression_length);
            parts[count].raw = NULL;
            count++;

            cooked_length = 0;
            raw_length = 0;
            continue;
        }
        if (c == '\\') {
            /* Keep the raw text, decode the cooked text. */
            lexer->position++;
            if (lexer->position >= lexer->length) break;
            char escape = lexer->source[lexer->position++];
            char decoded = escape;
            switch (escape) {
            case 'n': decoded = '\n'; break;
            case 't': decoded = '\t'; break;
            case 'r': decoded = '\r'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'v': decoded = '\v'; break;
            case '`': decoded = '`'; break;
            case '$': decoded = '$'; break;
            case '\\': decoded = '\\'; break;
            case 'x': {
                int high = hex_value(lexer->source[lexer->position]);
                int low = lexer->position + 1 < lexer->length
                              ? hex_value(lexer->source[lexer->position + 1]) : -1;
                if (high < 0 || low < 0) {
                    *error_out = djs_arena_strdup(lexer->rt, "invalid hex escape");
                    return false;
                }
                lexer->position += 2;
                decoded = (char)((high << 4) | low);
                break;
            }
            case 'u': {
                unsigned int code = 0;
                if (lexer->position < lexer->length && lexer->source[lexer->position] == '{') {
                    lexer->position++;
                    while (lexer->position < lexer->length &&
                           lexer->source[lexer->position] != '}') {
                        int digit = hex_value(lexer->source[lexer->position]);
                        if (digit < 0) break;
                        code = code * 16 + (unsigned int)digit;
                        lexer->position++;
                    }
                    if (lexer->position < lexer->length) lexer->position++;
                } else {
                    for (int i = 0; i < 4 && lexer->position < lexer->length; i++) {
                        int digit = hex_value(lexer->source[lexer->position]);
                        if (digit < 0) {
                            *error_out = djs_arena_strdup(lexer->rt, "invalid unicode escape");
                            return false;
                        }
                        code = code * 16 + (unsigned int)digit;
                        lexer->position++;
                    }
                }
                if (cooked_length + 4 >= cooked_capacity) {
                    cooked_capacity *= 2;
                    char *grown = (char *)djs_arena_alloc(lexer->rt, cooked_capacity);
                    if (grown == NULL) return false;
                    djs_memcpy(grown, cooked, cooked_length);
                    cooked = grown;
                }
                append_utf8(cooked, &cooked_length, code);
                raw[raw_length++] = '\\';
                raw[raw_length++] = 'u';
                continue;
            }
            default: break;
            }
            if (cooked_length + 1 >= cooked_capacity) {
                cooked_capacity *= 2;
                char *grown = (char *)djs_arena_alloc(lexer->rt, cooked_capacity);
                if (grown == NULL) return false;
                djs_memcpy(grown, cooked, cooked_length);
                cooked = grown;
            }
            cooked[cooked_length++] = decoded;
            raw[raw_length++] = '\\';
            raw[raw_length++] = escape;
            continue;
        }
        if (cooked_length + 1 >= cooked_capacity) {
            cooked_capacity *= 2;
            char *grown = (char *)djs_arena_alloc(lexer->rt, cooked_capacity);
            if (grown == NULL) return false;
            djs_memcpy(grown, cooked, cooked_length);
            cooked = grown;
        }
        cooked[cooked_length++] = c;
        raw[raw_length++] = c;
        lexer->position++;
    }

    if (count + 1 > capacity) {
        capacity = count + 1;
        djs_template_part_t *grown = (djs_template_part_t *)djs_arena_alloc(
            lexer->rt, capacity * sizeof(*grown));
        if (grown == NULL) return false;
        djs_memcpy(grown, parts, count * sizeof(*grown));
        parts = grown;
    }
    parts[count].is_expression = false;
    parts[count].text = djs_arena_strndup(lexer->rt, cooked, cooked_length);
    parts[count].raw = djs_arena_strndup(lexer->rt, raw, raw_length);
    count++;

    template->parts = parts;
    template->count = count;
    token->type = DJS_TOK_TEMPLATE;
    token->extra = template;
    return true;
}

/* Punctuators, longest first. */
static const char *const punctuators[] = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=",
    "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=",
    "*=", "/=", "%=", "&=", "|=", "^=", "**", "<<", ">>",
    "{", "}", "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/", "%",
    "&", "|", "^", "!", "~", "?", ":", "=", ".", "#", "@",
    NULL
};

/* Decide whether a '/' after this token starts a regular expression. */
static bool punct_allows_regex(const char *text) {
    if (djs_strcmp(text, ")") == 0 || djs_strcmp(text, "]") == 0) return false;
    if (djs_strcmp(text, "}") == 0) return true;
    if (djs_strcmp(text, "++") == 0 || djs_strcmp(text, "--") == 0) return false;
    return true;
}

int djs_lexer_next(djs_lexer_t *lexer, djs_token_t *token, char **error_out) {
    skip_trivia(lexer);
    token->type = DJS_TOK_EOF;
    token->text = NULL;
    token->text2 = NULL;
    token->extra = NULL;
    token->number = 0.0;
    token->position = (int)lexer->position;
    token->length = 0;
    token->newline_before = lexer->saw_newline;

    if (lexer->position >= lexer->length) {
        lexer->regex_allowed = false;
        return 1;
    }

    char c = lexer->source[lexer->position];

    if (is_ident_start(c)) {
        size_t start = lexer->position;
        while (lexer->position < lexer->length && is_ident_part(lexer->source[lexer->position])) {
            lexer->position++;
        }
        token->type = DJS_TOK_IDENTIFIER;
        token->text = djs_arena_strndup(lexer->rt, lexer->source + start, lexer->position - start);
        token->length = (int)(lexer->position - start);
        lexer->regex_allowed = false;
        return 1;
    }

    if (is_digit(c) || (c == '.' && lexer->position + 1 < lexer->length &&
                        is_digit(lexer->source[lexer->position + 1]))) {
        if (!read_number(lexer, token, error_out)) return 0;
        lexer->regex_allowed = false;
        return 1;
    }

    if (c == '"' || c == '\'') {
        if (!read_string(lexer, token, error_out)) return 0;
        lexer->regex_allowed = false;
        return 1;
    }

    if (c == '`') {
        if (!read_template(lexer, token, error_out)) return 0;
        lexer->regex_allowed = false;
        return 1;
    }

    if (c == '/' && lexer->regex_allowed) {
        if (!read_regexp(lexer, token, error_out)) return 0;
        lexer->regex_allowed = false;
        return 1;
    }

    for (int i = 0; punctuators[i] != NULL; i++) {
        size_t length = djs_strlen(punctuators[i]);
        if (lexer->position + length <= lexer->length &&
            djs_memcmp_helper(lexer->source + lexer->position, punctuators[i], length) == 0) {
            lexer->position += length;
            token->type = DJS_TOK_PUNCT;
            token->text = djs_arena_strdup(lexer->rt, punctuators[i]);
            token->length = (int)length;
            lexer->regex_allowed = punct_allows_regex(punctuators[i]);
            return 1;
        }
    }

    *error_out = djs_arena_strdup(lexer->rt, "unexpected character");
    return 0;
}

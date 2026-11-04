/* plist.c — Apple Property List (XML) parser and serializer.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include "../include/plist.h"
#include <string.h>

#define MAX_PLIST_POOL 256
static plist_node_t g_plist_pool[MAX_PLIST_POOL];
static uint32_t     g_pool_used = 0;

static plist_node_t *alloc_plist_node(plist_type_t type) {
    if (g_pool_used >= MAX_PLIST_POOL) return NULL;
    plist_node_t *node = &g_plist_pool[g_pool_used++];
    memset(node, 0, sizeof(*node));
    node->type = type;
    return node;
}

void plist_free(plist_node_t *node) {
    (void)node;
    /* Static pool resets or individual mark */
}

plist_node_t *plist_create_dict(void) {
    return alloc_plist_node(PLIST_TYPE_DICT);
}

plist_node_t *plist_create_array(void) {
    return alloc_plist_node(PLIST_TYPE_ARRAY);
}

plist_node_t *plist_create_string(const char *str) {
    plist_node_t *node = alloc_plist_node(PLIST_TYPE_STRING);
    if (node && str) {
        size_t slen = strlen(str);
        if (slen >= sizeof(node->val_str)) slen = sizeof(node->val_str) - 1;
        memcpy(node->val_str, str, slen);
        node->val_str[slen] = '\0';
    }
    return node;
}

plist_node_t *plist_create_int(int64_t val) {
    plist_node_t *node = alloc_plist_node(PLIST_TYPE_INTEGER);
    if (node) node->val_int = val;
    return node;
}

plist_node_t *plist_create_bool(bool val) {
    plist_node_t *node = alloc_plist_node(PLIST_TYPE_BOOLEAN);
    if (node) node->val_bool = val;
    return node;
}

plist_node_t *plist_dict_get(const plist_node_t *dict, const char *key) {
    if (!dict || dict->type != PLIST_TYPE_DICT || !key) return NULL;
    plist_node_t *curr = dict->children;
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

const char *plist_dict_get_string(const plist_node_t *dict, const char *key) {
    plist_node_t *node = plist_dict_get(dict, key);
    if (node && node->type == PLIST_TYPE_STRING) {
        return node->val_str;
    }
    return NULL;
}

int64_t plist_dict_get_int(const plist_node_t *dict, const char *key, int64_t def) {
    plist_node_t *node = plist_dict_get(dict, key);
    if (node && node->type == PLIST_TYPE_INTEGER) {
        return node->val_int;
    }
    return def;
}

bool plist_dict_get_bool(const plist_node_t *dict, const char *key, bool def) {
    plist_node_t *node = plist_dict_get(dict, key);
    if (node && node->type == PLIST_TYPE_BOOLEAN) {
        return node->val_bool;
    }
    return def;
}

int plist_dict_set(plist_node_t *dict, const char *key, plist_node_t *val) {
    if (!dict || dict->type != PLIST_TYPE_DICT || !key || !val) return -1;

    size_t klen = strlen(key);
    if (klen >= sizeof(val->key)) klen = sizeof(val->key) - 1;
    memcpy(val->key, key, klen);
    val->key[klen] = '\0';

    val->next = dict->children;
    dict->children = val;
    return 0;
}

int plist_dict_set_string(plist_node_t *dict, const char *key, const char *str) {
    plist_node_t *node = plist_create_string(str);
    return plist_dict_set(dict, key, node);
}

int plist_dict_set_int(plist_node_t *dict, const char *key, int64_t val) {
    plist_node_t *node = plist_create_int(val);
    return plist_dict_set(dict, key, node);
}

int plist_dict_set_bool(plist_node_t *dict, const char *key, bool val) {
    plist_node_t *node = plist_create_bool(val);
    return plist_dict_set(dict, key, node);
}

int plist_array_append(plist_node_t *array, plist_node_t *item) {
    if (!array || array->type != PLIST_TYPE_ARRAY || !item) return -1;
    if (!array->children) {
        array->children = item;
    } else {
        plist_node_t *curr = array->children;
        while (curr->next) curr = curr->next;
        curr->next = item;
    }
    return 0;
}

/* Helper to skip whitespace */
static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

/* Extract content between <tag> and </tag> */
static const char *find_tag_content(const char *p, const char *end, const char *open_tag,
                                   const char *close_tag, const char **out_val_start, size_t *out_val_len) {
    size_t open_len = strlen(open_tag);
    size_t close_len = strlen(close_tag);

    while (p + open_len <= end) {
        if (strncmp(p, open_tag, open_len) == 0) {
            const char *val_start = p + open_len;
            const char *q = val_start;
            while (q + close_len <= end) {
                if (strncmp(q, close_tag, close_len) == 0) {
                    *out_val_start = val_start;
                    *out_val_len = (size_t)(q - val_start);
                    return q + close_len;
                }
                q++;
            }
            return NULL;
        }
        p++;
    }
    return NULL;
}

/* Recursive XML parser */
plist_node_t *plist_parse_xml(const char *xml, size_t len) {
    if (!xml || len == 0) return NULL;

    const char *p = xml;
    const char *end = xml + len;

    /* Find start of root dictionary or array */
    const char *dict_start = NULL;
    while (p + 6 <= end) {
        if (strncmp(p, "<dict>", 6) == 0) {
            dict_start = p + 6;
            break;
        }
        p++;
    }

    if (!dict_start) return NULL;

    plist_node_t *root_dict = plist_create_dict();
    p = dict_start;

    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end || strncmp(p, "</dict>", 7) == 0) {
            break;
        }

        /* Parse <key>...</key> */
        const char *key_start = NULL;
        size_t key_len = 0;
        const char *next_p = find_tag_content(p, end, "<key>", "</key>", &key_start, &key_len);
        if (!next_p) break;

        char key_buf[64];
        if (key_len >= sizeof(key_buf)) key_len = sizeof(key_buf) - 1;
        memcpy(key_buf, key_start, key_len);
        key_buf[key_len] = '\0';

        p = skip_ws(next_p, end);
        if (p >= end) break;

        /* Parse value based on tag */
        if (strncmp(p, "<string>", 8) == 0) {
            const char *str_start = NULL;
            size_t str_len = 0;
            p = find_tag_content(p, end, "<string>", "</string>", &str_start, &str_len);
            if (!p) break;

            char val_buf[256];
            if (str_len >= sizeof(val_buf)) str_len = sizeof(val_buf) - 1;
            memcpy(val_buf, str_start, str_len);
            val_buf[str_len] = '\0';
            plist_dict_set_string(root_dict, key_buf, val_buf);
        } else if (strncmp(p, "<integer>", 9) == 0) {
            const char *int_start = NULL;
            size_t int_len = 0;
            p = find_tag_content(p, end, "<integer>", "</integer>", &int_start, &int_len);
            if (!p) break;

            int64_t val = 0;
            for (size_t k = 0; k < int_len; k++) {
                if (int_start[k] >= '0' && int_start[k] <= '9') {
                    val = val * 10 + (int_start[k] - '0');
                }
            }
            plist_dict_set_int(root_dict, key_buf, val);
        } else if (strncmp(p, "<true/>", 7) == 0) {
            plist_dict_set_bool(root_dict, key_buf, true);
            p += 7;
        } else if (strncmp(p, "<false/>", 8) == 0) {
            plist_dict_set_bool(root_dict, key_buf, false);
            p += 8;
        } else {
            /* Skip unknown element */
            while (p < end && *p != '>') p++;
            if (p < end) p++;
        }
    }

    return root_dict;
}

/* Simple serialization */
size_t plist_to_xml(const plist_node_t *node, char *buf, size_t maxlen) {
    if (!node || !buf || maxlen < 128) return 0;

    size_t offset = 0;
    const char *header = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n";
    size_t hlen = strlen(header);
    if (offset + hlen < maxlen) {
        memcpy(buf + offset, header, hlen);
        offset += hlen;
    }

    if (node->type == PLIST_TYPE_DICT) {
        const plist_node_t *curr = node->children;
        while (curr && offset + 128 < maxlen) {
            /* <key> */
            const char *k1 = "  <key>";
            size_t k1len = strlen(k1);
            memcpy(buf + offset, k1, k1len);
            offset += k1len;

            size_t keylen = strlen(curr->key);
            memcpy(buf + offset, curr->key, keylen);
            offset += keylen;

            const char *k2 = "</key>\n";
            size_t k2len = strlen(k2);
            memcpy(buf + offset, k2, k2len);
            offset += k2len;

            /* Value */
            if (curr->type == PLIST_TYPE_STRING) {
                const char *s1 = "  <string>";
                size_t s1len = strlen(s1);
                memcpy(buf + offset, s1, s1len);
                offset += s1len;

                size_t vlen = strlen(curr->val_str);
                memcpy(buf + offset, curr->val_str, vlen);
                offset += vlen;

                const char *s2 = "</string>\n";
                size_t s2len = strlen(s2);
                memcpy(buf + offset, s2, s2len);
                offset += s2len;
            } else if (curr->type == PLIST_TYPE_BOOLEAN) {
                const char *b = curr->val_bool ? "  <true/>\n" : "  <false/>\n";
                size_t blen = strlen(b);
                memcpy(buf + offset, b, blen);
                offset += blen;
            }

            curr = curr->next;
        }
    }

    const char *footer = "</dict>\n</plist>\n";
    size_t flen = strlen(footer);
    if (offset + flen < maxlen) {
        memcpy(buf + offset, footer, flen);
        offset += flen;
    }
    buf[offset] = '\0';
    return offset;
}

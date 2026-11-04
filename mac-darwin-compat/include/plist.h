/* plist.h — Apple Property List (XML/ASCII) parser and DOM tree.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#ifndef MAC_DARWIN_PLIST_H
#define MAC_DARWIN_PLIST_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLIST_TYPE_UNKNOWN = 0,
    PLIST_TYPE_DICT,
    PLIST_TYPE_ARRAY,
    PLIST_TYPE_STRING,
    PLIST_TYPE_INTEGER,
    PLIST_TYPE_BOOLEAN,
    PLIST_TYPE_DATA
} plist_type_t;

typedef struct plist_node {
    plist_type_t        type;
    char                key[64];    /* Dictionary key, if child of dict */
    char                val_str[256];
    int64_t             val_int;
    bool                val_bool;
    struct plist_node  *children;   /* First child */
    struct plist_node  *next;       /* Sibling */
} plist_node_t;

/* API */
plist_node_t *plist_create_dict(void);
plist_node_t *plist_create_array(void);
plist_node_t *plist_create_string(const char *str);
plist_node_t *plist_create_int(int64_t val);
plist_node_t *plist_create_bool(bool val);

plist_node_t *plist_dict_get(const plist_node_t *dict, const char *key);
const char   *plist_dict_get_string(const plist_node_t *dict, const char *key);
int64_t       plist_dict_get_int(const plist_node_t *dict, const char *key, int64_t def);
bool          plist_dict_get_bool(const plist_node_t *dict, const char *key, bool def);

int           plist_dict_set(plist_node_t *dict, const char *key, plist_node_t *val);
int           plist_dict_set_string(plist_node_t *dict, const char *key, const char *str);
int           plist_dict_set_int(plist_node_t *dict, const char *key, int64_t val);
int           plist_dict_set_bool(plist_node_t *dict, const char *key, bool val);

int           plist_array_append(plist_node_t *array, plist_node_t *item);

plist_node_t *plist_parse_xml(const char *xml, size_t len);
size_t        plist_to_xml(const plist_node_t *node, char *buf, size_t maxlen);
void          plist_free(plist_node_t *node);

#ifdef __cplusplus
}
#endif

#endif /* MAC_DARWIN_PLIST_H */

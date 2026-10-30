/* capability.c — revocable rights grants layered on object handles.
 *
 * Like the M5 object registry this initial version is single-CPU and is only
 * called from kernel context. M7 must add synchronization before concurrency.
 */
#include "capability.h"

#include "heap.h"

typedef struct {
    uint32_t references;
    bool revoked;
} revocation_domain_t;

typedef struct {
    object_t *target;
    uint32_t rights;
    revocation_domain_t *domain;
} capability_payload_t;

static void domain_release(revocation_domain_t *domain) {
    if (domain == NULL || domain->references == 0) return;
    if (--domain->references == 0) kfree(domain);
}

static bool domain_retain(revocation_domain_t *domain) {
    if (domain == NULL || domain->revoked || domain->references == UINT32_MAX)
        return false;
    domain->references++;
    return true;
}

static void capability_destroy(object_t *object, void *payload_ptr) {
    (void)object;
    capability_payload_t *payload = payload_ptr;
    if (payload == NULL) return;
    object_release(payload->target);
    domain_release(payload->domain);
}

static handle_t capability_install(handle_table_t *table, object_t *target,
                                   uint32_t rights,
                                   revocation_domain_t *domain,
                                   bool create_domain, uint32_t handle_rights) {
    if (table == NULL || target == NULL || rights == 0 ||
        (rights & ~OBJECT_RIGHT_ALL) != 0) return 0;

    if (create_domain) {
        domain = kzalloc(sizeof(*domain));
        if (domain == NULL) return 0;
        domain->references = 1;
    } else if (!domain_retain(domain)) {
        return 0;
    }

    if (!object_retain(target)) {
        domain_release(domain);
        return 0;
    }
    object_t *cap_object = object_create(OBJECT_TYPE_CAPABILITY,
                                        sizeof(capability_payload_t),
                                        capability_destroy);
    if (cap_object == NULL) {
        object_release(target);
        domain_release(domain);
        return 0;
    }
    capability_payload_t *payload = object_payload(cap_object);
    payload->target = target;
    payload->rights = rights;
    payload->domain = domain;

    handle_t result = handle_create(table, cap_object, handle_rights);
    object_release(cap_object);
    return result;
}

handle_t capability_create(handle_table_t *table, handle_t source,
                           uint32_t granted_rights) {
    handle_info_t info;
    if (!handle_inspect(table, source, &info) ||
        !(info.rights & OBJECT_RIGHT_DUPLICATE) || granted_rights == 0 ||
        (granted_rights & ~info.rights) != 0) return 0;
    object_t *target = handle_get(table, source, OBJECT_RIGHT_DUPLICATE,
                                  OBJECT_TYPE_NONE);
    if (target == NULL) return 0;
    const uint32_t cap_handle_rights = OBJECT_RIGHT_READ |
        OBJECT_RIGHT_DUPLICATE | OBJECT_RIGHT_CONTROL | OBJECT_RIGHT_INSPECT;
    handle_t result = capability_install(table, target, granted_rights, NULL,
                                         true, cap_handle_rights);
    object_release(target);
    return result;
}

handle_t capability_derive(handle_table_t *table, handle_t source,
                           uint32_t reduced_rights) {
    if (reduced_rights == 0 || (reduced_rights & ~OBJECT_RIGHT_ALL) != 0)
        return 0;
    object_t *cap_object = handle_get(table, source, OBJECT_RIGHT_DUPLICATE,
                                      OBJECT_TYPE_CAPABILITY);
    if (cap_object == NULL) return 0;
    capability_payload_t *parent = object_payload(cap_object);
    handle_t result = 0;
    if (parent != NULL && !parent->domain->revoked &&
        (reduced_rights & ~parent->rights) == 0) {
        result = capability_install(table, parent->target, reduced_rights,
                                    parent->domain, false,
                                    OBJECT_RIGHT_READ | OBJECT_RIGHT_DUPLICATE);
    }
    object_release(cap_object);
    return result;
}

bool capability_revoke(handle_table_t *table, handle_t capability) {
    object_t *cap_object = handle_get(table, capability, OBJECT_RIGHT_CONTROL,
                                      OBJECT_TYPE_CAPABILITY);
    if (cap_object == NULL) return false;
    capability_payload_t *payload = object_payload(cap_object);
    bool result = payload != NULL && payload->domain != NULL &&
                  !payload->domain->revoked;
    if (result) payload->domain->revoked = true;
    object_release(cap_object);
    return result;
}

bool capability_allows(handle_table_t *table, handle_t capability,
                       uint32_t required_rights, object_type_t expected_type) {
    if (required_rights == 0 || (required_rights & ~OBJECT_RIGHT_ALL) != 0)
        return false;
    object_t *cap_object = handle_get(table, capability, OBJECT_RIGHT_READ,
                                      OBJECT_TYPE_CAPABILITY);
    if (cap_object == NULL) return false;
    capability_payload_t *payload = object_payload(cap_object);
    bool allowed = payload != NULL && payload->domain != NULL &&
                   !payload->domain->revoked &&
                   (payload->rights & required_rights) == required_rights &&
                   (expected_type == OBJECT_TYPE_NONE ||
                    object_type(payload->target) == expected_type);
    object_release(cap_object);
    return allowed;
}

object_t *capability_resolve(handle_table_t *table, handle_t capability,
                             uint32_t required_rights,
                             object_type_t expected_type) {
    if (required_rights == 0 || (required_rights & ~OBJECT_RIGHT_ALL) != 0)
        return NULL;
    if (!capability_allows(table, capability, required_rights, expected_type))
        return NULL;
    object_t *cap_object = handle_get(table, capability, OBJECT_RIGHT_READ,
                                      OBJECT_TYPE_CAPABILITY);
    object_t *target = NULL;
    if (cap_object != NULL) {
        capability_payload_t *payload = object_payload(cap_object);
        if (payload != NULL && object_retain(payload->target)) target = payload->target;
        object_release(cap_object);
    }
    return target;
}

bool capability_is_revoked(handle_table_t *table, handle_t capability) {
    object_t *cap_object = handle_get(table, capability, OBJECT_RIGHT_READ,
                                      OBJECT_TYPE_CAPABILITY);
    if (cap_object == NULL) return false;
    capability_payload_t *payload = object_payload(cap_object);
    bool revoked = payload == NULL || payload->domain == NULL ||
                   payload->domain->revoked;
    object_release(cap_object);
    return revoked;
}

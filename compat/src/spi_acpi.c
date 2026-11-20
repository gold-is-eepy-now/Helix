/* spi_acpi.c — works out an ACPI SPI slave's link to a Helix controller. See spi_acpi.h.
 *
 * Each step either succeeds or names the reason it could not, and the reason is what the
 * spi command shows. Nothing is guessed: with no controller at the parent's address the slave
 * is not bound, even when some other controller exists. */
#include <stddef.h>
#include "kstring.h"
#include "spi_acpi.h"

const char *spi_link_status_text(spi_link_status_t s) {
    switch (s) {
    case SPI_LINK_OK:            return "linked";
    case SPI_LINK_NO_NODE:       return "device not in the ACPI namespace";
    case SPI_LINK_NO_CRS:        return "device has no _CRS buffer";
    case SPI_LINK_CRS_NO_SPI:    return "_CRS has no SpiSerialBus descriptor";
    case SPI_LINK_CRS_BAD:       return "_CRS has a malformed SpiSerialBus descriptor";
    case SPI_LINK_NO_PARENT_ADR: return "parent has no _ADR";
    case SPI_LINK_NO_CONTROLLER: return "no SPI controller at the parent's _ADR";
    case SPI_LINK_AMBIGUOUS:     return "more than one SPI controller at the parent's _ADR";
    case SPI_LINK_WIRE_3:        return "3-wire SPI is not supported";
    case SPI_LINK_BITS:          return "word size is not 8 bits";
    }
    return "unknown link status";
}

void spi_link_node(aml_node_t *slave, spi_link_t *out) {
    memset(out, 0, sizeof *out);
    if (slave == NULL) { out->status = SPI_LINK_NO_NODE; return; }
    out->slave = slave;

    /* _CRS: the device's resources. It must be a buffer for the descriptor parser. */
    aml_value_t crs;
    memset(&crs, 0, sizeof crs);
    if (aml_eval_at(slave, "_CRS", NULL, 0, &crs) != AML_OK || crs.type != AML_T_BUF) {
        aml_value_free(&crs);
        out->status = SPI_LINK_NO_CRS;
        return;
    }
    acpi_spi_res_status_t rs = acpi_spi_res_parse(crs.data, crs.len, &out->res);
    aml_value_free(&crs);
    if (rs == ACPI_SPI_RES_NONE) { out->status = SPI_LINK_CRS_NO_SPI; return; }
    if (rs != ACPI_SPI_RES_OK)   { out->status = SPI_LINK_CRS_BAD; return; }
    out->have_res = true;

    /* The parent's _ADR names the controller that the device hangs off. */
    aml_node_t *parent = aml_parent(slave);
    aml_value_t adr;
    memset(&adr, 0, sizeof adr);
    if (parent == NULL || aml_eval_at(parent, "_ADR", NULL, 0, &adr) != AML_OK ||
        adr.type != AML_T_INT) {
        aml_value_free(&adr);
        out->status = SPI_LINK_NO_PARENT_ADR;
        return;
    }
    out->parent = parent;
    out->parent_adr = (uint32_t)adr.integer;
    out->have_parent_adr = true;
    aml_value_free(&adr);

    /* Exactly one registered controller must have that address. */
    spi_controller_t *found = NULL;
    unsigned matches = 0;
    for (unsigned i = 0; i < spi_controller_count(); i++) {
        spi_controller_t *c = spi_controller_at(i);
        if (c != NULL && c->has_acpi_adr && c->acpi_adr == out->parent_adr) {
            found = c;
            matches++;
        }
    }
    if (matches == 0) { out->status = SPI_LINK_NO_CONTROLLER; return; }
    if (matches > 1)  { out->status = SPI_LINK_AMBIGUOUS; return; }

    /* The controller can only do 4-wire, 8-bit SPI. Anything else is refused, not adjusted. */
    if (out->res.three_wire)            { out->status = SPI_LINK_WIRE_3; return; }
    if (out->res.data_bit_length != 8u) { out->status = SPI_LINK_BITS; return; }

    out->ctrl = found;
    out->status = SPI_LINK_OK;
}

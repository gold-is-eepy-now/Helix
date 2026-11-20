/* spi_acpi.h — how an ACPI-described SPI slave is wired to a Helix SPI controller.
 *
 * A slave such as APP000D sits in the AML namespace under its controller: the controller's
 * node has an _ADR (the PCI address, for an LPSS block) and the slave's _CRS names its chip
 * select and clock settings. spi_link works both out, without sending any transfer. */
#ifndef HELIX_COMPAT_SPI_ACPI_H
#define HELIX_COMPAT_SPI_ACPI_H

#include <stdbool.h>
#include <stdint.h>
#include "acpi_spi_res.h"
#include "spi.h"
#include "aml.h"

typedef enum {
    SPI_LINK_OK = 0,
    SPI_LINK_NO_NODE,          /* the device is not in the ACPI namespace */
    SPI_LINK_NO_CRS,           /* the device has no _CRS, or it is not a buffer */
    SPI_LINK_CRS_NO_SPI,       /* _CRS has no SpiSerialBus descriptor */
    SPI_LINK_CRS_BAD,          /* _CRS has a malformed SpiSerialBus descriptor */
    SPI_LINK_NO_PARENT_ADR,    /* the parent has no integer _ADR */
    SPI_LINK_NO_CONTROLLER,    /* no registered SPI controller has the parent's _ADR */
    SPI_LINK_AMBIGUOUS,        /* more than one controller has the parent's _ADR */
    SPI_LINK_WIRE_3,           /* 3-wire SPI: the controller does not support it */
    SPI_LINK_BITS,             /* the word size is not 8 bits */
} spi_link_status_t;

typedef struct {
    spi_link_status_t status;
    aml_node_t *slave;         /* the device node, when found */
    aml_node_t *parent;        /* the node the device sits under, when found */
    uint32_t parent_adr;       /* the parent's _ADR, valid when have_parent_adr */
    bool have_parent_adr;
    acpi_spi_res_t res;        /* the SpiSerialBus settings, valid when have_res */
    bool have_res;
    spi_controller_t *ctrl;    /* the controller at parent_adr, valid when status is OK */
} spi_link_t;

/* Works out the link for a slave node. Sends no transfer and changes no state. */
void spi_link_node(aml_node_t *slave, spi_link_t *out);

/* Text for a status, for the log and the spi command. */
const char *spi_link_status_text(spi_link_status_t s);

#endif /* HELIX_COMPAT_SPI_ACPI_H */

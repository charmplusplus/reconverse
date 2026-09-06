/**
 * Set up variables that Reconverse doesn't need
 * but Charm++ does, but only if they are not
 * architecture-specific.
 */

#ifndef RECONVERSE_INCLUDE_CHARM_CONFIG_H
#define RECONVERSE_INCLUDE_CHARM_CONFIG_H

/* Universal source discriminator for code shared with classic charm:
 * reconverse's charm-config.h defines it 1; classic's conv-autoconfig
 * defines it 0 (and classic legacy builds leave it undefined, which
 * preprocesses as 0). Charm's two-way tree guards on it. */
#define CMK_RECONVERSE 1

#define CMK_LBDB_ON 1

// Size of the machine-layer buffer in CmiNcpyBuffer::layerInfo.
// Reconverse uses 8 bytes for mr_t + 16 bytes for rmr + 8 bytes spare = 32.
// This must match CMK_NOCOPY_DIRECT_BYTES in reconverse/include/conv-rdma.h.
#define CMK_NOCOPY_DIRECT_BYTES 32

#endif
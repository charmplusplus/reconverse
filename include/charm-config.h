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

/* CCS (Converse Client-Server) is part of libreconverse: conv-ccs.h,
 * ccs-server.h, sockRoutines.h and the ccs-client library. */
#define CMK_CCS_AVAILABLE 1

// Size of the machine-layer buffer in CmiNcpyBuffer::layerInfo: 8 bytes for
// mr_t + 32 for the remote region (device index and lci::rmr_t) = 40.
// This must match CMK_NOCOPY_DIRECT_BYTES in reconverse/include/conv-rdma.h.
#define CMK_NOCOPY_DIRECT_BYTES 40

#endif
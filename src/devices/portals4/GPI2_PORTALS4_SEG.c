/*
Copyright (c) Goethe University Frankfurt MSQC - Niklas
Bartelheimer<bartelheimer@em.uni-frankfurt.de>, 2023-2026

This file is part of GPI-2.

GPI-2 is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License
version 3 as published by the Free Software Foundation.

GPI-2 is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GPI-2. If not, see <http://www.gnu.org/licenses/>.
*/

#include "GASPI.h"
#include "GPI2.h"
#include "GPI2_SEG.h"
#include "GPI2_PORTALS4.h"

int
pgaspi_dev_register_mem(gaspi_context_t const* const GASPI_UNUSED(gctx),
                        gaspi_rc_mseg_t* GASPI_UNUSED(seg))
{
  return GASPI_SUCCESS;
}

int
pgaspi_dev_unregister_mem(gaspi_context_t const* const GASPI_UNUSED(gctx),
                          gaspi_rc_mseg_t* GASPI_UNUSED(seg))
{
  return GASPI_SUCCESS;
}

#ifdef GPI2_PORTALS4_XPMEM
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include "GPI2_Sys.h"

static void
pgaspi_portals4_xpmem_lock(portals4_xpmem_peer *peer)
{
  while(__atomic_test_and_set(&peer->lock, __ATOMIC_ACQUIRE))
    GASPI_DELAY();
}

static void
pgaspi_portals4_xpmem_unlock(portals4_xpmem_peer *peer)
{
  __atomic_clear(&peer->lock, __ATOMIC_RELEASE);
}

/* create_lock serializes creation of the process-wide export. The export
   remains alive across peer disconnects and GASPI segment recreation. */
int
pgaspi_portals4_xpmem_endpoint(const gaspi_context_t *gctx, int rank)
{
  gaspi_portals4_ctx *dev = gctx->device->ctx;
  struct portals4_ctx_info *info = &dev->local_info[rank];
  info->xpmem_enabled = pgaspi_ranks_are_local(gctx->rank, rank);
  info->xpmem_segid = -1;
  if(!info->xpmem_enabled || rank == gctx->rank)
    return GASPI_SUCCESS;

  if(dev->xpmem_segid == -1)
  {
    dev->xpmem_segid = xpmem_make(NULL, XPMEM_MAXADDR_SIZE,
                                 XPMEM_PERMIT_MODE, (void *)(uintptr_t)0600);
    if(dev->xpmem_segid == -1)
    {
      GASPI_DEBUG_PRINT_ERROR("xpmem_make failed: %s", strerror(errno));
      return GASPI_ERROR;
    }
  }
  info->xpmem_segid = dev->xpmem_segid;
  return GASPI_SUCCESS;
}

/* Called under ccontext_lock, after the connection metadata exchange. No
   transfer may subsequently change this choice, including after errors. */
int
pgaspi_portals4_xpmem_connect(const gaspi_context_t *gctx, int rank)
{
  gaspi_portals4_ctx *dev = gctx->device->ctx;
  portals4_xpmem_peer *peer = &dev->xpmem_peers[rank];
  const struct portals4_ctx_info *info = &dev->remote_info[rank];
  if(!pgaspi_ranks_are_local(gctx->rank, rank))
    return info->xpmem_enabled ? GASPI_ERROR : GASPI_SUCCESS;
  if(!info->xpmem_enabled)
    return GASPI_ERROR;
  if(rank == gctx->rank)
  {
    peer->local = 1;
    return GASPI_SUCCESS;
  }
  if(info->xpmem_segid == -1)
    return GASPI_ERROR;

  peer->mappings = calloc(2 * (size_t)gctx->config->segment_max,
                          sizeof(*peer->mappings));
  if(peer->mappings == NULL)
    return GASPI_ERROR;
  peer->apid = xpmem_get(info->xpmem_segid, XPMEM_RDWR,
                         XPMEM_PERMIT_MODE, NULL);
  if(peer->apid == -1)
  {
    GASPI_DEBUG_PRINT_ERROR("xpmem_get for rank %d failed: %s", rank,
                            strerror(errno));
    free(peer->mappings);
    peer->mappings = NULL;
    return GASPI_ERROR;
  }
  peer->local = 1;
  return GASPI_SUCCESS;
}

int
pgaspi_portals4_xpmem_disconnect(const gaspi_context_t *gctx, int rank)
{
  gaspi_portals4_ctx *dev = gctx->device->ctx;
  portals4_xpmem_peer *peer = &dev->xpmem_peers[rank];
  pgaspi_portals4_xpmem_lock(peer);
  /* release also detaches every mapping associated with this access permit.
     On failure retain the handles so disconnect/cleanup can be retried. */
  if(peer->apid != -1 && xpmem_release(peer->apid) != 0)
  {
    GASPI_DEBUG_PRINT_ERROR("xpmem_release for rank %d failed: %s", rank,
                            strerror(errno));
    pgaspi_portals4_xpmem_unlock(peer);
    return GASPI_ERROR;
  }
  peer->apid = -1;
  peer->local = 0;
  free(peer->mappings);
  peer->mappings = NULL;
  pgaspi_portals4_xpmem_unlock(peer);
  return GASPI_SUCCESS;
}

int
pgaspi_portals4_xpmem_cleanup(const gaspi_context_t *gctx)
{
  gaspi_portals4_ctx *dev = gctx->device->ctx;
  int result = GASPI_SUCCESS;
  if(dev->xpmem_peers != NULL)
  {
    for(int rank = 0; rank < gctx->tnc; ++rank)
      if(pgaspi_portals4_xpmem_disconnect(gctx, rank) != GASPI_SUCCESS)
        result = GASPI_ERROR;
    if(result != GASPI_SUCCESS)
      return result;
    free(dev->xpmem_peers);
    dev->xpmem_peers = NULL;
  }
  if(dev->xpmem_segid != -1)
  {
    if(xpmem_remove(dev->xpmem_segid) != 0)
    {
      GASPI_DEBUG_PRINT_ERROR("xpmem_remove failed: %s", strerror(errno));
      return GASPI_ERROR;
    }
    dev->xpmem_segid = -1;
  }
  return GASPI_SUCCESS;
}

/* Caller holds the peer lock through the copy: another queue cannot replace
   or detach its mapping while it is in use. Cache by segment and region,
   checking the exchanged address/size on every use. XPMEM's MMU notifier
   invalidates imported PTEs when the exporter unmaps/replaces backing pages;
   attachments refer to virtual addresses, not to a GASPI segment generation.
   Applications must quiesce communication before deleting/re-registering. */
static void *
pgaspi_portals4_xpmem_map(gaspi_context_t *gctx, gaspi_rank_t rank,
                          gaspi_segment_id_t segment, int notification)
{
  gaspi_portals4_ctx *dev = gctx->device->ctx;
  portals4_xpmem_peer *peer = &dev->xpmem_peers[rank];
  gaspi_rc_mseg_t *seg = &gctx->rrmd[segment][rank];
  uintptr_t address = notification ? seg->notif_spc.addr : seg->data.addr;
  /* The SN segment-registration message carries notif_spc.addr but not
     notif_spc_size. As in the Portals path, use the configured space size. */
  size_t size = notification ? pgaspi_notifications_space_size() : seg->size;
  if(rank == gctx->rank)
    return (void *)address;
  portals4_xpmem_mapping *entry = &peer->mappings[2 * (size_t)segment + notification];
  if(entry->mapping && entry->address == address && entry->size == size)
    return (char *)entry->mapping + entry->delta;
  if(entry->mapping)
  {
    if(xpmem_detach(entry->mapping) != 0)
    {
      GASPI_DEBUG_PRINT_ERROR("xpmem_detach for rank %u failed: %s", rank,
                              strerror(errno));
      return NULL;
    }
    memset(entry, 0, sizeof(*entry));
  }
  size_t page = dev->xpmem_page_size;
  size_t delta = address % page;
  uintptr_t base = address - delta;
  if(!size || base > LONG_MAX || size > SIZE_MAX - delta - (page - 1))
    return NULL;
  size_t length = ((size + delta + page - 1) / page) * page;
  if(length > (uintptr_t)LONG_MAX - base)
    return NULL;
  struct xpmem_addr location = { .apid = peer->apid, .offset = (off_t)base };
  void *mapping = xpmem_attach(location, length, NULL);
  if(mapping == (void *)-1)
  {
    GASPI_DEBUG_PRINT_ERROR("xpmem_attach for rank %u failed: %s", rank,
                            strerror(errno));
    return NULL;
  }
  entry->mapping = mapping;
  entry->address = address;
  entry->size = size;
  entry->delta = delta;
  return (char *)mapping + delta;
}

gaspi_return_t
pgaspi_portals4_xpmem_copy(gaspi_context_t *gctx, gaspi_rank_t rank,
    gaspi_segment_id_t segment, gaspi_offset_t offset, void *buffer,
    gaspi_size_t size, int read)
{
  if(!size)
    return GASPI_SUCCESS;
  gaspi_portals4_ctx *dev = gctx->device->ctx;
  portals4_xpmem_peer *peer = &dev->xpmem_peers[rank];
  pgaspi_portals4_xpmem_lock(peer);
  void *mapped = peer->local ? pgaspi_portals4_xpmem_map(gctx, rank, segment, 0) : NULL;
  if(mapped != NULL)
  {
    if(read)
      memmove(buffer, (char *)mapped + offset, size);
    else
      memmove((char *)mapped + offset, buffer, size);
  }
  pgaspi_portals4_xpmem_unlock(peer);
  return mapped ? GASPI_SUCCESS : GASPI_ERROR;
}

gaspi_return_t
pgaspi_portals4_xpmem_notify(gaspi_context_t *gctx, gaspi_rank_t rank,
    gaspi_segment_id_t segment, gaspi_notification_id_t id,
    gaspi_notification_t value)
{
  gaspi_portals4_ctx *dev = gctx->device->ctx;
  portals4_xpmem_peer *peer = &dev->xpmem_peers[rank];
  pgaspi_portals4_xpmem_lock(peer);
  gaspi_notification_t *mapped = peer->local ?
      pgaspi_portals4_xpmem_map(gctx, rank, segment, 1) : NULL;
  if(mapped != NULL)
  {
    /* libc completes any streaming stores before returning from memmove.
       Publish only after the payload; never copy a notification bytewise. */
    __atomic_store_n(&mapped[id], value, __ATOMIC_RELEASE);
  }
  pgaspi_portals4_xpmem_unlock(peer);
  return mapped ? GASPI_SUCCESS : GASPI_ERROR;
}
#endif

/*
Copyright (c) Goethe University Frankfurt MSQC - Niklas Bartelheimer
<bartelheimer@em.uni-frankfurt.de>, 2023-2024
Copyright (c) Johannes Gutenberg-University Mainz HPCA - Niklas Bartelheimer
<bartelheimer@uni-mainz.de>, 2024-2026
This file is part of GPI-2.

GPI4PTL is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License
version 3 as published by the Free Software Foundation.

GPI4PTL is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GPI4PTL. If not, see <http://www.gnu.org/licenses/>.
*/
#include "GPI2.h"
#include "GPI2_PORTALS4.h"
#include "GPI2_Sys.h"
#include <limits.h>
#include <time.h>

/* Zero-byte controls: RTS carries address in hdr_data and size in
   remote_offset; DONE carries success/error in remote_offset. One send per
   destination may be outstanding, so the DONE initiator identifies it. */
enum { PASSIVE_IDLE, PASSIVE_GET_PENDING, PASSIVE_DONE_READY,
       PASSIVE_DONE_PENDING, PASSIVE_FATAL };

static ptl_time_t
passive_remaining(const gaspi_context_t* gctx, gaspi_timeout_t timeout,
                   gaspi_cycles_t start)
{
  if(timeout == GASPI_BLOCK)
    return PTL_TIME_FOREVER;
  if(timeout == GASPI_TEST)
    return 0;
  const gaspi_cycles_t elapsed = gaspi_get_cycles() - start;
  const float ms = (float)elapsed * gctx->cycles_to_msecs;
  if(ms >= timeout)
    return 0;
  const double remaining = (double)timeout - ms;
  if(remaining >= UINT_MAX - 1)
    return UINT_MAX - 1;
  /* Portals accepts whole milliseconds; round up the remaining budget. */
  const ptl_time_t wait = (ptl_time_t)remaining;
  return wait + (remaining > wait);
}

static int
passive_peer(gaspi_context_t* gctx, ptl_process_t initiator)
{
  gaspi_portals4_ctx* dev = gctx->device->ctx;
  for(int i = 0; i < gctx->tnc; ++i)
    if(dev->remote_info[i].phys_address.phys.nid == initiator.phys.nid &&
       dev->remote_info[i].phys_address.phys.pid == initiator.phys.pid)
      return i;
  return -1;
}

void
pgaspi_portals4_passive_reset(gaspi_portals4_ctx* dev)
{
  portals4_passive* p = &dev->passive;
  p->rts_le = p->done_le = PTL_INVALID_HANDLE;
  p->rts_md = p->recv_md = p->recv_ct = PTL_INVALID_HANDLE;
  p->rts_eq = p->done_eq = PTL_INVALID_HANDLE;
  p->send_error_eq = p->recv_error_eq = PTL_INVALID_HANDLE;
  p->rts_pt = p->done_pt = PTL_PT_ANY;
}

int
pgaspi_portals4_passive_init(gaspi_context_t* gctx)
{
  gaspi_portals4_ctx* dev = gctx->device->ctx;
  portals4_passive* p = &dev->passive;
  p->send = calloc(gctx->tnc, sizeof(*p->send));
  if(!p->send)
    return GASPI_ERROR;

  /* One outstanding RTS/DONE per peer, including self; no LINK/UNLINK
     events. Send and receive own separate error EQs. */
  ptl_size_t slots = gctx->tnc;
  if(slots < gctx->config->passive_queue_size_max)
    slots = gctx->config->passive_queue_size_max;
  if(PtlEQAlloc(dev->ni_h, slots, &p->rts_eq) != PTL_OK ||
     PtlEQAlloc(dev->ni_h, slots, &p->done_eq) != PTL_OK ||
     PtlEQAlloc(dev->ni_h, 2 * slots, &p->send_error_eq) != PTL_OK ||
     PtlEQAlloc(dev->ni_h, 4, &p->recv_error_eq) != PTL_OK ||
     PtlPTAlloc(dev->ni_h, 0, p->rts_eq,
                PORTALS4_PASSIVE_PT_INDEX, &p->rts_pt) != PTL_OK ||
     PtlPTAlloc(dev->ni_h, 0, p->done_eq, PORTALS4_PASSIVE_DONE_PT_INDEX,
                &p->done_pt) != PTL_OK)
    return GASPI_ERROR;

  ptl_le_t le = {
      .start = NULL, .length = 0, .uid = PTL_UID_ANY,
      .options = PTL_LE_OP_PUT | PTL_LE_EVENT_LINK_DISABLE |
                 PTL_LE_EVENT_UNLINK_DISABLE,
      .ct_handle = PTL_CT_NONE,
  };
  if(PtlLEAppend(dev->ni_h, p->rts_pt, &le,
                 PTL_PRIORITY_LIST, NULL, &p->rts_le) != PTL_OK ||
     PtlLEAppend(dev->ni_h, p->done_pt, &le,
                 PTL_PRIORITY_LIST, NULL, &p->done_le) != PTL_OK)
    return GASPI_ERROR;

  /* RTS uses full ACKs: successful ACK events are suppressed, but errors
     retain user_ptr = rank + 1. Receiver operations use user_ptr = NULL. */
  ptl_md_t md = {
      .start = NULL, .length = PTL_SIZE_MAX,
      .options = PTL_MD_EVENT_SUCCESS_DISABLE | PTL_MD_EVENT_SEND_DISABLE,
      .eq_handle = p->send_error_eq, .ct_handle = PTL_CT_NONE,
  };
  if(PtlMDBind(dev->ni_h, &md, &p->rts_md) != PTL_OK ||
     PtlCTAlloc(dev->ni_h, &p->recv_ct) != PTL_OK)
    return GASPI_ERROR;
  md.eq_handle = p->recv_error_eq;
  md.ct_handle = p->recv_ct;
  md.options |= PTL_MD_EVENT_CT_REPLY | PTL_MD_EVENT_CT_ACK;
  return PtlMDBind(dev->ni_h, &md, &p->recv_md) == PTL_OK
             ? GASPI_SUCCESS : GASPI_ERROR;
}

int
pgaspi_portals4_passive_cleanup(gaspi_context_t* gctx)
{
  gaspi_portals4_ctx* dev = gctx->device->ctx;
  portals4_passive* p = &dev->passive;
  int status = GASPI_SUCCESS;
#define PASSIVE_RELEASE(fn, handle)                         \
  do {                                                     \
    if(!PtlHandleIsEqual(handle, PTL_INVALID_HANDLE))        \
    {                                                      \
      if(fn(handle) != PTL_OK)                              \
        status = GASPI_ERROR;                              \
      else                                                 \
        handle = PTL_INVALID_HANDLE;                       \
    }                                                      \
  } while(0)
  PASSIVE_RELEASE(PtlMDRelease, p->rts_md);
  PASSIVE_RELEASE(PtlMDRelease, p->recv_md);
  PASSIVE_RELEASE(PtlLEUnlink, p->rts_le);
  PASSIVE_RELEASE(PtlLEUnlink, p->done_le);
  if(p->done_pt != (ptl_pt_index_t)PTL_PT_ANY)
  {
    if(PtlPTFree(dev->ni_h, p->done_pt) != PTL_OK)
      status = GASPI_ERROR;
    else
      p->done_pt = PTL_PT_ANY;
  }
  if(p->rts_pt != (ptl_pt_index_t)PTL_PT_ANY)
  {
    if(PtlPTFree(dev->ni_h, p->rts_pt) != PTL_OK)
      status = GASPI_ERROR;
    else
      p->rts_pt = PTL_PT_ANY;
  }
  PASSIVE_RELEASE(PtlCTFree, p->recv_ct);
  PASSIVE_RELEASE(PtlEQFree, p->rts_eq);
  PASSIVE_RELEASE(PtlEQFree, p->done_eq);
  PASSIVE_RELEASE(PtlEQFree, p->send_error_eq);
  PASSIVE_RELEASE(PtlEQFree, p->recv_error_eq);
#undef PASSIVE_RELEASE
  free(p->send);
  p->send = NULL;
  return status;
}

/* Called only under lockPS. Retain failures for other pending peers. */
static int
passive_send_errors(gaspi_context_t* gctx)
{
  portals4_passive* p = &((gaspi_portals4_ctx*)gctx->device->ctx)->passive;
  ptl_event_t event;
  int ret;
  while((ret = PtlEQGet(p->send_error_eq, &event)) == PTL_OK)
  {
    uintptr_t tag = (uintptr_t)event.user_ptr;
    GASPI_DEBUG_PRINT_ERROR("Passive RTS failure %d", event.ni_fail_type);
    if(event.ni_fail_type == PTL_NI_OK || !tag || tag > (uintptr_t)gctx->tnc)
      p->send_broken = 1;
    else
    {
      const gaspi_rank_t peer = tag - 1;
      const unsigned char bit = 1u << (peer & 7);
      p->send[peer].failed = 1;
      if(gctx->ne_count_p[peer >> 3] & bit)
      {
        gctx->ne_count_p[peer >> 3] &= ~bit;
        --p->pending_sends;
      }
    }
  }
  if(ret != PTL_EQ_EMPTY)
    p->send_broken = 1;
  return p->send_broken ? GASPI_ERROR : GASPI_SUCCESS;
}

/* Called only under lockPR. Per-operation failures are checked in recv_ct;
   losing error events makes the receive path unusable. */
static int
passive_recv_errors(portals4_passive* p)
{
  ptl_event_t event;
  int ret;
  while((ret = PtlEQGet(p->recv_error_eq, &event)) == PTL_OK)
  {
    GASPI_DEBUG_PRINT_ERROR("Passive GET/DONE failure %d", event.ni_fail_type);
    if(event.ni_fail_type == PTL_NI_OK)
      p->recv_broken = 1;
  }
  if(ret != PTL_EQ_EMPTY)
    p->recv_broken = 1;
  return p->recv_broken ? GASPI_ERROR : GASPI_SUCCESS;
}

/* Only lockPS consumes DONE. Other peers' completions stay in send[]. */
static int
passive_done(gaspi_context_t* gctx, const ptl_event_t* event)
{
  portals4_passive* p = &((gaspi_portals4_ctx*)gctx->device->ctx)->passive;
  int peer = passive_peer(gctx, event->initiator);
  if(event->type != PTL_EVENT_PUT || event->ni_fail_type != PTL_NI_OK ||
     event->rlength != 0 || peer < 0 || event->remote_offset > 1)
  {
    p->send_broken = 1;
    return GASPI_ERROR;
  }
  /* A failed peer is never reused. A late DONE cannot consume a new slot. */
  if(p->send[peer].failed)
    return GASPI_SUCCESS;
  if(!(gctx->ne_count_p[peer >> 3] & (1u << (peer & 7))) || p->send[peer].done)
  {
    p->send_broken = 1;
    return GASPI_ERROR;
  }
  p->send[peer].done = event->remote_offset ? 2 : 1;
  return GASPI_SUCCESS;
}

gaspi_return_t
pgaspi_dev_passive_send(gaspi_context_t* const gctx,
                        const gaspi_segment_id_t segment_id_local,
                        const gaspi_offset_t offset_local,
                        const gaspi_rank_t rank, const gaspi_size_t size,
                        const gaspi_timeout_t timeout_ms)
{
  gaspi_portals4_ctx* dev = gctx->device->ctx;
  portals4_passive* p = &dev->passive;
  portals4_passive_send* send = &p->send[rank];
  const unsigned char bit = 1u << (rank & 7);
  const ptl_size_t address =
      gctx->rrmd[segment_id_local][gctx->rank].data.addr + offset_local;
  const gaspi_cycles_t start = gaspi_get_cycles();
  if(passive_send_errors(gctx) != GASPI_SUCCESS ||
     send->failed)
    return GASPI_ERROR;
  if(gctx->ne_count_p[rank >> 3] & bit)
  {
    if(send->address != address || send->size != size)
      return GASPI_ERR_INV_COMMSIZE;
  }
  else
  {
    if(p->pending_sends >= gctx->config->passive_queue_size_max)
      return GASPI_ERROR;
    if(PtlAtomicSync() != PTL_OK)
      return GASPI_ERROR;
    send->address = address;
    send->size = size;
    send->done = 0;
    gctx->ne_count_p[rank >> 3] |= bit;
    int ret = PtlPut(p->rts_md, 0, 0, PTL_ACK_REQ,
                     dev->remote_info[rank].phys_address,
                     PORTALS4_PASSIVE_PT_INDEX, 0, size,
                     (void*)(uintptr_t)(rank + 1), address);
    if(ret != PTL_OK)
    {
      gctx->ne_count_p[rank >> 3] &= ~bit;
      return GASPI_ERROR;
    }
    ++p->pending_sends;
  }
  for(;;)
  {
    if(passive_send_errors(gctx) != GASPI_SUCCESS ||
       send->failed)
      return GASPI_ERROR;
    if(send->done)
    {
      /* BXI needs visibility synchronization before source-buffer reuse. */
      if(PtlAtomicSync() != PTL_OK)
        return GASPI_ERROR;
      gctx->ne_count_p[rank >> 3] &= ~bit;
      --p->pending_sends;
      return send->done == 1 ? GASPI_SUCCESS : GASPI_ERROR;
    }
    /* Bound the wait so RTS errors are checked even if no DONE arrives. */
    ptl_time_t wait = passive_remaining(gctx, timeout_ms, start);
    if(wait == PTL_TIME_FOREVER || wait > 10)
      wait = 10;
    ptl_event_t event;
    unsigned int which;
    int ret = PtlEQPoll(&p->done_eq, 1, wait, &event, &which);
    if(ret == PTL_OK)
    {
      if(passive_done(gctx, &event) != GASPI_SUCCESS)
        return GASPI_ERROR;
    }
    else if(ret != PTL_EQ_EMPTY)
    {
      p->send_broken = 1;
      return GASPI_ERROR;
    }
    else if(!passive_remaining(gctx, timeout_ms, start))
      return GASPI_TIMEOUT;
  }
}

/* Portals CT waits test successes, and wake on ANY nonzero failure count.
   After a previous failure use CTGet, comparing cumulative totals explicitly. */
static int
passive_wait(gaspi_context_t* gctx, gaspi_timeout_t timeout,
             gaspi_cycles_t start, ptl_size_t* failures)
{
  portals4_passive* p = &((gaspi_portals4_ctx*)gctx->device->ctx)->passive;
  ptl_ct_event_t count;
  unsigned int which;
  int ret;
  for(;;)
  {
    if(passive_recv_errors(p) != GASPI_SUCCESS)
      return GASPI_ERROR;
    if(*failures)
      ret = PtlCTGet(p->recv_ct, &count);
    else
      ret = PtlCTPoll(&p->recv_ct, &p->recv_count, 1,
                      passive_remaining(gctx, timeout, start), &count, &which);
    if(ret == PTL_CT_NONE_REACHED)
      return GASPI_TIMEOUT;
    if(ret != PTL_OK)
    {
      /* Completion unknown: keep the buffer reserved. */
      p->recv_broken = 1;
      return GASPI_ERROR;
    }
    if(count.failure != *failures)
    {
      *failures = count.failure;
      passive_recv_errors(p);
      return GASPI_ERROR;
    }
    if(count.success + count.failure >= p->recv_count)
      return passive_recv_errors(p);
    if(!passive_remaining(gctx, timeout, start))
      return GASPI_TIMEOUT;
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
    nanosleep(&pause, NULL);
  }
}

gaspi_return_t
pgaspi_dev_passive_receive(gaspi_context_t* const gctx,
                           const gaspi_segment_id_t segment_id_local,
                           const gaspi_offset_t offset_local,
                           gaspi_rank_t* const rank, const gaspi_size_t size,
                           const gaspi_timeout_t timeout_ms)
{
  gaspi_portals4_ctx* dev = gctx->device->ctx;
  portals4_passive* p = &dev->passive;
  const gaspi_cycles_t start = gaspi_get_cycles();
  const ptl_size_t destination =
      gctx->rrmd[segment_id_local][gctx->rank].data.addr + offset_local;
  int ret;
  if(passive_recv_errors(p) != GASPI_SUCCESS || p->stage == PASSIVE_FATAL)
    return GASPI_ERROR;
  if(p->stage != PASSIVE_IDLE &&
     (p->segment != segment_id_local || p->offset != offset_local ||
      p->capacity != size || p->destination != destination))
    return GASPI_ERR_INV_COMMSIZE;
  if(p->stage == PASSIVE_IDLE)
  {
    ptl_event_t event;
    unsigned int which;
    ret = PtlEQPoll(&p->rts_eq, 1,
                    passive_remaining(gctx, timeout_ms, start), &event, &which);
    if(ret == PTL_EQ_EMPTY)
      return GASPI_TIMEOUT;
    if(ret != PTL_OK || event.type != PTL_EVENT_PUT ||
       event.ni_fail_type != PTL_NI_OK || event.rlength != 0)
    {
      p->recv_broken = 1;
      return GASPI_ERROR;
    }
    int peer = passive_peer(gctx, event.initiator);
    if(peer < 0)
    {
      p->recv_broken = 1;
      return GASPI_ERROR;
    }
    p->peer = peer;
    p->source = event.hdr_data;
    p->size = event.remote_offset;
    p->segment = segment_id_local;
    p->offset = offset_local;
    p->destination = destination;
    p->capacity = size;
    p->result = GASPI_SUCCESS;
    p->stage = PASSIVE_DONE_READY;
    if(!p->size || p->size > size ||
       p->size > gctx->config->passive_transfer_size_max ||
       p->source > PTL_SIZE_MAX - p->size)
    {
      p->result = GASPI_ERROR;
    }
    else
    {
      ret = PtlGet(p->recv_md, p->destination, p->size,
                   dev->remote_info[p->peer].phys_address,
                   PORTALS4_DATA_PT_INDEX, 0, p->source, NULL);
      if(ret != PTL_OK)
        p->result = GASPI_ERROR;
      else
      {
        ++p->recv_count;
        p->stage = PASSIVE_GET_PENDING;
      }
    }
  }
  ptl_size_t failures = p->recv_failures;
  if(p->stage == PASSIVE_GET_PENDING)
  {
    ret = passive_wait(gctx, timeout_ms, start, &failures);
    p->recv_failures = failures;
    if(ret == GASPI_TIMEOUT)
      return ret;
    if(p->recv_broken)
      return GASPI_ERROR;
    p->result = ret;
    p->stage = PASSIVE_DONE_READY;
  }
  if(p->stage == PASSIVE_DONE_READY)
  {
    if(PtlAtomicSync() != PTL_OK)
      return GASPI_ERROR;
    ret = PtlPut(p->recv_md, 0, 0, PORTALS4_PASSIVE_ACK_TYPE,
                 dev->remote_info[p->peer].phys_address,
                 PORTALS4_PASSIVE_DONE_PT_INDEX, 0,
                 p->result == GASPI_SUCCESS ? 0 : 1,
                 NULL, 0);
    if(ret != PTL_OK)
      return GASPI_ERROR; /* Retry DONE, never repeat the GET. */
    ++p->recv_count;
    p->stage = PASSIVE_DONE_PENDING;
  }
  ret = passive_wait(gctx, timeout_ms, start, &failures);
  p->recv_failures = failures;
  if(ret == GASPI_TIMEOUT)
    return ret;
  if(ret != GASPI_SUCCESS)
  {
    p->stage = PASSIVE_FATAL;
    gctx->state_vec[GASPI_PASSIVE_QP][p->peer] = GASPI_STATE_CORRUPT;
    return GASPI_ERROR;
  }
  *rank = p->peer;
  ret = p->result;
  p->stage = PASSIVE_IDLE;
  return ret;
}

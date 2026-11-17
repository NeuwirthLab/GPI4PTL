/*
Copyright (c) Goethe University Frankfurt MSQC - Niklas Bartelheimer
<bartelheimer@em.uni-frankfurt.de>, 2023-2026

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

#ifndef _GPI2_PORTALS4_H_
#define _GPI2_PORTALS4_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "GPI2_Dev.h"
#include "GPI2_Utility.h"
#include <string.h>
#include <portals4.h>
#include <stdint.h>

#ifdef GPI2_PORTALS4_XPMEM
#include <xpmem.h>
/* The common notification poller uses volatile loads. Its external-call
   boundary and x86 load ordering supply the receive-side ordering here.
   Other architectures require an acquire in that common poller first. */
#if !defined(__x86_64__)
#error "Portals4 XPMEM currently requires x86-64"
#endif
typedef struct
{
  void *mapping;
  uintptr_t address;
  size_t size, delta;
} portals4_xpmem_mapping;

typedef struct
{
  int local;
  xpmem_apid_t apid;
  volatile unsigned char lock;
  portals4_xpmem_mapping *mappings;
} portals4_xpmem_peer;
#endif

#define PORTALS4_DATA_PT_INDEX 0
#define PORTALS4_PASSIVE_PT_INDEX 1
#define PORTALS4_PASSIVE_DONE_PT_INDEX 2

#define PORTALS4_DATA_PT_EVENT_SLOTS (1)
#define PORTALS4_ACK_TYPE PTL_ACK_REQ
#define PORTALS4_PASSIVE_ACK_TYPE PTL_CT_ACK_REQ

struct portals4_ctx_info
{
  ptl_process_t phys_address;
  /* Keep the exchange layout identical in enabled and disabled builds. */
  int64_t xpmem_segid;
  uint32_t xpmem_enabled;
};

typedef struct
{
  ptl_size_t address, size;
  int done, failed;
} portals4_passive_send;

typedef struct
{
  ptl_handle_le_t rts_le, done_le;
  ptl_pt_index_t rts_pt, done_pt;
  ptl_handle_eq_t rts_eq, done_eq, send_error_eq, recv_error_eq;
  ptl_handle_md_t rts_md, recv_md;
  ptl_handle_ct_t recv_ct;
  ptl_size_t recv_count, recv_failures;
  portals4_passive_send *send;
  gaspi_number_t pending_sends; 
  int send_broken, recv_broken;
  int stage, result;
  gaspi_rank_t peer;
  gaspi_segment_id_t segment;
  gaspi_offset_t offset;
  ptl_size_t source, destination, size, capacity;
} portals4_passive;

typedef struct
{
  // Interface
  ptl_handle_ni_t ni_h;
  int initialized;
  volatile unsigned char group_lock;
  ptl_handle_md_t atomic_md_h;
  ptl_handle_ct_t atomic_ct_h;
  ptl_handle_eq_t atomic_eq_h;
  ptl_size_t atomic_ct_cnt;
  portals4_passive passive;

  // Memory Descriptors
  ptl_handle_md_t group_atomic_md_h;
  ptl_handle_md_t comm_notif_md_h[GASPI_MAX_QP];

  // Data Space Portal
  ptl_handle_le_t data_le_h;
  ptl_pt_index_t data_pt_idx;

  // Counting Event Queues
  ptl_handle_ct_t group_atomic_ct_h;
  ptl_handle_ct_t comm_notif_ct_h[GASPI_MAX_QP];

  // Event Queues
  ptl_handle_eq_t data_pt_eq_h;
  ptl_handle_eq_t comm_notif_err_eq_h[GASPI_MAX_QP];
  ptl_handle_eq_t group_atomic_err_eq_h;

  // CT Event Counter
  ptl_size_t group_atomic_ct_cnt;
  ptl_size_t comm_notif_ct_cnt[GASPI_MAX_QP];

  // Exchange
  ptl_size_t ordered_size;
  struct portals4_ctx_info* local_info;
  struct portals4_ctx_info* remote_info;
#ifdef GPI2_PORTALS4_XPMEM
  xpmem_segid_t xpmem_segid;
  size_t xpmem_page_size;
  portals4_xpmem_peer *xpmem_peers;
#endif
} gaspi_portals4_ctx;

#ifdef GPI2_PORTALS4_XPMEM
int pgaspi_portals4_xpmem_endpoint(const gaspi_context_t *gctx, int rank);
int pgaspi_portals4_xpmem_connect(const gaspi_context_t *gctx, int rank);
int pgaspi_portals4_xpmem_disconnect(const gaspi_context_t *gctx, int rank);
int pgaspi_portals4_xpmem_cleanup(const gaspi_context_t *gctx);
gaspi_return_t pgaspi_portals4_xpmem_copy(gaspi_context_t *gctx,
    gaspi_rank_t rank, gaspi_segment_id_t segment, gaspi_offset_t offset,
    void *buffer, gaspi_size_t size, int read);
gaspi_return_t pgaspi_portals4_xpmem_notify(gaspi_context_t *gctx,
    gaspi_rank_t rank, gaspi_segment_id_t segment,
    gaspi_notification_id_t id, gaspi_notification_t value);
#endif

static inline int
pgaspi_portals4_is_local(const gaspi_portals4_ctx *dev, gaspi_rank_t rank)
{
#ifdef GPI2_PORTALS4_XPMEM
  return dev->xpmem_peers[rank].local;
#else
  (void)dev;
  (void)rank;
  return 0;
#endif
}

void pgaspi_portals4_passive_reset(gaspi_portals4_ctx* dev);
int pgaspi_portals4_passive_init(gaspi_context_t* gctx);
int pgaspi_portals4_passive_cleanup(gaspi_context_t* gctx);
int pgaspi_portals4_queue_reset(gaspi_context_t* gctx, unsigned int queue);
int pgaspi_portals4_errors(gaspi_context_t* gctx, ptl_handle_eq_t eq,
                          unsigned int queue);

#endif

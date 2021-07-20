/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "mpidimpl.h"
#include "ofi_impl.h"
#include "ofi_noinline.h"

#define PORT_NAME_TAG_KEY "tag"
#define CONNENTR_TAG_KEY "connentry"

#define DYNPROC_RECEIVER 0
#define DYNPROC_SENDER 1

/* NOTE: dynamics process support is limited to single VCI for now.
 * TODO: assert MPIDI_global.n_vcis == 1.
 */

static void free_port_name_tag(int tag);
static int get_port_name_tag(int *port_name_tag);
static int get_tag_from_port(const char *port_name, int *port_name_tag);
static int get_conn_name_from_port(const char *port_name, char *connname);
static int dynproc_create_intercomm(const char *port_name, int remote_size, int *remote_gpids,
                                    MPIR_Comm * comm_ptr, MPIR_Comm ** newcomm, int is_low_group,
                                    int get_tag, char *api);
static int dynproc_handshake(int root, int phase, int timeout, int port_id, fi_addr_t * conn,
                             MPIR_Comm * comm_ptr);
static int dynproc_exchange_map(int root, int phase, int port_id, fi_addr_t * conn, char *conname,
                                MPIR_Comm * comm_ptr, int *out_root, int *remote_size,
                                size_t ** remote_upid_size, char **remote_upids);

/* NOTE: port_name_tag, context_id_offset, and port_id all refer to the same context_id used during
 * establishing dynamic connections */
static void free_port_name_tag(int tag)
{
    int idx, rem_tag;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_FREE_PORT_NAME_TAG);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_FREE_PORT_NAME_TAG);
    MPID_THREAD_CS_ENTER(VCI, MPIDIU_THREAD_DYNPROC_MUTEX);

    idx = tag / (sizeof(int) * 8);
    rem_tag = tag - (idx * sizeof(int) * 8);

    MPIDI_OFI_global.port_name_tag_mask[idx] &= ~(1u << ((8 * sizeof(int)) - 1 - rem_tag));

    MPID_THREAD_CS_EXIT(VCI, MPIDIU_THREAD_DYNPROC_MUTEX);
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_FREE_PORT_NAME_TAG);
}

static int get_port_name_tag(int *port_name_tag)
{
    unsigned i, j;
    int mpi_errno = MPI_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_GET_PORT_NAME_TAG);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_GET_PORT_NAME_TAG);
    MPID_THREAD_CS_ENTER(VCI, MPIDIU_THREAD_DYNPROC_MUTEX);

    for (i = 0; i < MPIR_MAX_CONTEXT_MASK; i++)
        if (MPIDI_OFI_global.port_name_tag_mask[i] != ~0)
            break;

    if (i < MPIR_MAX_CONTEXT_MASK)
        for (j = 0; j < (8 * sizeof(int)); j++) {
            if ((MPIDI_OFI_global.port_name_tag_mask[i] | (1u << ((8 * sizeof(int)) - j - 1))) !=
                MPIDI_OFI_global.port_name_tag_mask[i]) {
                MPIDI_OFI_global.port_name_tag_mask[i] |= (1u << ((8 * sizeof(int)) - j - 1));
                *port_name_tag = ((i * 8 * sizeof(int)) + j);
                goto fn_exit;
            }
    } else
        goto fn_fail;

  fn_exit:
    MPID_THREAD_CS_EXIT(VCI, MPIDIU_THREAD_DYNPROC_MUTEX);
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_GET_PORT_NAME_TAG);
    return mpi_errno;

  fn_fail:
    *port_name_tag = -1;
    mpi_errno = MPI_ERR_OTHER;
    goto fn_exit;
}

static int get_tag_from_port(const char *port_name, int *port_name_tag)
{
    int mpi_errno = MPI_SUCCESS;
    int str_errno = MPL_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_GET_TAG_FROM_PORT);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_GET_TAG_FROM_PORT);

    if (strlen(port_name) == 0)
        goto fn_exit;

    str_errno = MPL_str_get_int_arg(port_name, PORT_NAME_TAG_KEY, port_name_tag);
    MPIR_ERR_CHKANDJUMP(str_errno, mpi_errno, MPI_ERR_OTHER, "**argstr_no_port_name_tag");
  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_GET_TAG_FROM_PORT);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

static int get_conn_name_from_port(const char *port_name, char *connname)
{
    int mpi_errno = MPI_SUCCESS;
    int maxlen = MPIDI_KVSAPPSTRLEN;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_GET_CONN_NAME_FROM_PORT);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_GET_CONN_NAME_FROM_PORT);

    /* WB TODO - Only setting up nic 0 for spawn right now. */
    MPL_str_get_binary_arg(port_name, CONNENTR_TAG_KEY, connname, MPIDI_OFI_global.addrnamelen,
                           &maxlen);

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_GET_CONN_NAME_FROM_PORT);
    return mpi_errno;
}

static int dynproc_create_intercomm(const char *port_name, int remote_size, int *remote_gpids,
                                    MPIR_Comm * comm_ptr, MPIR_Comm ** newcomm, int is_low_group,
                                    int context_id_offset, char *api)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Comm *tmp_comm_ptr = NULL;
    int i = 0;
    MPIDI_rank_map_mlut_t *mlut = NULL;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_DYNPROC_CREATE_INTERCOMM);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_DYNPROC_CREATE_INTERCOMM);

    mpi_errno = MPIR_Comm_create(&tmp_comm_ptr);
    MPIR_ERR_CHECK(mpi_errno);

    tmp_comm_ptr->context_id = MPIR_CONTEXT_SET_FIELD(DYNAMIC_PROC, context_id_offset, 1);
    tmp_comm_ptr->recvcontext_id = tmp_comm_ptr->context_id;
    tmp_comm_ptr->remote_size = remote_size;
    tmp_comm_ptr->local_size = comm_ptr->local_size;
    tmp_comm_ptr->rank = comm_ptr->rank;
    tmp_comm_ptr->comm_kind = MPIR_COMM_KIND__INTERCOMM;
    tmp_comm_ptr->local_comm = comm_ptr;
    tmp_comm_ptr->is_low_group = is_low_group;

    /* handle local group */
    /* No ref changes to LUT/MLUT in this step because the localcomm will not
     * be released in the normal way */
    MPIDI_COMM(tmp_comm_ptr, local_map).mode = MPIDI_COMM(comm_ptr, map).mode;
    MPIDI_COMM(tmp_comm_ptr, local_map).size = MPIDI_COMM(comm_ptr, map).size;
    MPIDI_COMM(tmp_comm_ptr, local_map).avtid = MPIDI_COMM(comm_ptr, map).avtid;
    switch (MPIDI_COMM(comm_ptr, map).mode) {
        case MPIDI_RANK_MAP_DIRECT:
        case MPIDI_RANK_MAP_DIRECT_INTRA:
            break;
        case MPIDI_RANK_MAP_OFFSET:
        case MPIDI_RANK_MAP_OFFSET_INTRA:
            MPIDI_COMM(tmp_comm_ptr, local_map).reg.offset = MPIDI_COMM(comm_ptr, map).reg.offset;
            break;
        case MPIDI_RANK_MAP_STRIDE:
        case MPIDI_RANK_MAP_STRIDE_INTRA:
        case MPIDI_RANK_MAP_STRIDE_BLOCK:
        case MPIDI_RANK_MAP_STRIDE_BLOCK_INTRA:
            MPIDI_COMM(tmp_comm_ptr, local_map).reg.stride.stride =
                MPIDI_COMM(comm_ptr, map).reg.stride.stride;
            MPIDI_COMM(tmp_comm_ptr, local_map).reg.stride.blocksize =
                MPIDI_COMM(comm_ptr, map).reg.stride.blocksize;
            MPIDI_COMM(tmp_comm_ptr, local_map).reg.stride.offset =
                MPIDI_COMM(comm_ptr, map).reg.stride.offset;
            break;
        case MPIDI_RANK_MAP_LUT:
        case MPIDI_RANK_MAP_LUT_INTRA:
            MPIDI_COMM(tmp_comm_ptr, local_map).irreg.lut.t = MPIDI_COMM(comm_ptr, map).irreg.lut.t;
            MPIDI_COMM(tmp_comm_ptr, local_map).irreg.lut.lpid =
                MPIDI_COMM(comm_ptr, map).irreg.lut.lpid;
            break;
        case MPIDI_RANK_MAP_MLUT:
            MPIDI_COMM(tmp_comm_ptr, local_map).irreg.mlut.t =
                MPIDI_COMM(comm_ptr, map).irreg.mlut.t;
            MPIDI_COMM(tmp_comm_ptr, local_map).irreg.mlut.gpid =
                MPIDI_COMM(comm_ptr, map).irreg.mlut.gpid;
            break;
        case MPIDI_RANK_MAP_NONE:
            MPIR_Assert(0);
            break;
    }

    /* set mapping for remote group */
    mpi_errno = MPIDIU_alloc_mlut(&mlut, remote_size);
    MPIR_ERR_CHECK(mpi_errno);
    MPIDI_COMM(tmp_comm_ptr, map).mode = MPIDI_RANK_MAP_MLUT;
    MPIDI_COMM(tmp_comm_ptr, map).size = remote_size;
    MPIDI_COMM(tmp_comm_ptr, map).avtid = -1;
    MPIDI_COMM(tmp_comm_ptr, map).irreg.mlut.t = mlut;
    MPIDI_COMM(tmp_comm_ptr, map).irreg.mlut.gpid = mlut->gpid;
    for (i = 0; i < remote_size; ++i) {
        MPIDI_COMM(tmp_comm_ptr, map).irreg.mlut.gpid[i].avtid =
            MPIDIU_GPID_GET_AVTID(remote_gpids[i]);
        MPIDI_COMM(tmp_comm_ptr, map).irreg.mlut.gpid[i].lpid =
            MPIDIU_GPID_GET_LPID(remote_gpids[i]);
    }

    MPIR_Comm_commit(tmp_comm_ptr);

    mpi_errno = MPIR_Comm_dup_impl(tmp_comm_ptr, newcomm);
    MPIR_ERR_CHECK(mpi_errno);

    tmp_comm_ptr->local_comm = NULL;    /* avoid freeing local comm with comm_release */
    MPIR_Comm_release(tmp_comm_ptr);

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_DYNPROC_CREATE_INTERCOMM);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

static int dynproc_handshake(int root, int phase, int timeout, int port_id, fi_addr_t * conn,
                             MPIR_Comm * comm_ptr)
{
    int mpi_errno = MPI_SUCCESS;
    MPIDI_OFI_dynamic_process_request_t req;
    uint64_t match_bits = 0;
    uint64_t mask_bits = 0;
    struct fi_msg_tagged msg;
    int buf = 0;
    MPL_time_t time_sta, time_now;
    double time_gap;
    int nic = 0;
    int ctx_idx = MPIDI_OFI_get_ctx_index(comm_ptr, 0, nic);

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_DYNPROC_HANDSHAKE);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_DYNPROC_HANDSHAKE);

    /* connector */
    if (phase == 0) {
        req.done = MPIDI_OFI_PEEK_START;
        req.event_id = MPIDI_OFI_EVENT_ACCEPT_PROBE;
        match_bits = MPIDI_OFI_init_recvtag(&mask_bits, port_id, MPI_ANY_TAG);
        match_bits |= MPIDI_OFI_DYNPROC_SEND;

        msg.msg_iov = NULL;
        msg.desc = NULL;
        msg.iov_count = 0;
        msg.addr = FI_ADDR_UNSPEC;
        msg.tag = match_bits;
        msg.ignore = mask_bits;
        msg.context = (void *) &req.context;
        msg.data = 0;

        MPL_DBG_MSG_FMT(MPIDI_CH4_DBG_GENERAL, VERBOSE,
                        (MPL_DBG_FDEST,
                         "connecting port_id %d, conn %" PRIu64
                         ", waiting for dynproc_handshake", port_id, *conn));
        time_gap = 0.0;
        MPL_wtime(&time_sta);
        while (req.done != MPIDI_OFI_PEEK_FOUND) {
            req.done = MPIDI_OFI_PEEK_START;
            MPIDI_OFI_VCI_CALL(fi_trecvmsg
                               (MPIDI_OFI_global.ctx[ctx_idx].rx, &msg,
                                FI_PEEK | FI_COMPLETION | FI_REMOTE_CQ_DATA), 0, trecv);
            do {
                mpi_errno = MPID_Progress_test(NULL);
                MPIR_ERR_CHECK(mpi_errno);

                MPL_wtime(&time_now);
                MPL_wtime_diff(&time_sta, &time_now, &time_gap);
            } while (req.done == MPIDI_OFI_PEEK_START && (int) time_gap < timeout);
            if ((int) time_gap >= timeout) {
                /* connection is timed out */
                MPL_DBG_MSG_FMT(MPIDI_CH4_DBG_GENERAL, VERBOSE,
                                (MPL_DBG_FDEST,
                                 "connection to port_id %d, conn %" PRIu64 " ack timed out",
                                 port_id, *conn));
                mpi_errno = MPI_ERR_PORT;
                goto fn_fail;
            }
        }

        req.done = 0;
        req.event_id = MPIDI_OFI_EVENT_DYNPROC_DONE;

        MPIDI_OFI_VCI_CALL_RETRY(fi_trecv(MPIDI_OFI_global.ctx[ctx_idx].rx,
                                          &buf,
                                          sizeof(int),
                                          NULL,
                                          *conn, match_bits, mask_bits, &req.context), 0, trecv,
                                 FALSE);
        time_gap = 0.0;
        MPL_wtime(&time_sta);
        do {
            mpi_errno = MPID_Progress_test(NULL);
            MPIR_ERR_CHECK(mpi_errno);

            MPL_wtime(&time_now);
            MPL_wtime_diff(&time_sta, &time_now, &time_gap);
        } while (!req.done && (int) time_gap < timeout);
        if ((int) time_gap >= timeout) {
            /* connection is mismatched */
            MPL_DBG_MSG_FMT(MPIDI_CH4_DBG_GENERAL, VERBOSE,
                            (MPL_DBG_FDEST,
                             "connection to port_id %d, conn %" PRIu64 " ack mismatched", port_id,
                             *conn));
            mpi_errno = MPI_ERR_PORT;
            goto fn_fail;
        }
    }

    /* acceptor */
    if (phase == 1) {
        int tag = root;

        match_bits = MPIDI_OFI_init_sendtag(port_id, tag, MPIDI_OFI_DYNPROC_SEND);

        req.done = 0;
        req.event_id = MPIDI_OFI_EVENT_DYNPROC_DONE;
        MPIDI_OFI_VCI_CALL_RETRY(fi_tsenddata(MPIDI_OFI_global.ctx[ctx_idx].tx,
                                              &buf, sizeof(int), NULL /* desc */ ,
                                              comm_ptr->rank,
                                              *conn,
                                              match_bits,
                                              (void *) &req.context), 0, tsenddata,
                                 FALSE /* eagain */);

        MPIDI_OFI_VCI_PROGRESS_WHILE(0, !req.done);
    }

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_DYNPROC_HANDSHAKE);

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

static int dynproc_exchange_map(int root, int phase, int port_id, fi_addr_t * conn, char *conname,
                                MPIR_Comm * comm_ptr, int *out_root, int *remote_size,
                                size_t ** remote_upid_size, char **remote_upids)
{
    int i, mpi_errno = MPI_SUCCESS;

    MPIDI_OFI_dynamic_process_request_t req[3];
    uint64_t match_bits = 0;
    uint64_t mask_bits = 0;
    struct fi_msg_tagged msg;
    size_t *local_upid_size = NULL;
    char *local_upids = NULL;
    int nic = 0;
    int ctx_idx = MPIDI_OFI_get_ctx_index(comm_ptr, 0, nic);

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_DYNPROC_EXCHANGE_MAP);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_DYNPROC_EXCHANGE_MAP);

    MPIR_CHKPMEM_DECL(3);

    req[0].done = MPIDI_OFI_PEEK_START;
    req[0].event_id = MPIDI_OFI_EVENT_ACCEPT_PROBE;
    match_bits = MPIDI_OFI_init_recvtag(&mask_bits, port_id, MPI_ANY_TAG);
    match_bits |= MPIDI_OFI_DYNPROC_SEND;

    if (phase == 0) {
        size_t remote_upid_recvsize = 0;

        /* Receive the addresses                           */
        /* We don't know the size, so probe for table size */
        /* Receive phase updates the connection            */
        /* With the probed address                         */
        msg.msg_iov = NULL;
        msg.desc = NULL;
        msg.iov_count = 0;
        msg.addr = FI_ADDR_UNSPEC;
        msg.tag = match_bits;
        msg.ignore = mask_bits;
        msg.context = (void *) &req[0].context;
        msg.data = 0;

        while (req[0].done != MPIDI_OFI_PEEK_FOUND) {
            req[0].done = MPIDI_OFI_PEEK_START;
            MPIDI_OFI_VCI_CALL(fi_trecvmsg
                               (MPIDI_OFI_global.ctx[ctx_idx].rx, &msg,
                                FI_PEEK | FI_COMPLETION | FI_REMOTE_CQ_DATA), 0, trecv);
            MPIDI_OFI_VCI_PROGRESS_WHILE(0, req[0].done == MPIDI_OFI_PEEK_START);
        }

        *remote_size = req[0].msglen / sizeof(size_t);
        *out_root = req[0].tag;
        MPIR_CHKPMEM_MALLOC((*remote_upid_size), size_t *,
                            (*remote_size) * sizeof(size_t), mpi_errno, "remote_upid_size",
                            MPL_MEM_ADDRESS);
        req[0].done = 0;
        req[0].event_id = MPIDI_OFI_EVENT_DYNPROC_DONE;
        req[1].done = 0;
        req[1].event_id = MPIDI_OFI_EVENT_DYNPROC_DONE;

        MPIDI_OFI_VCI_CALL_RETRY(fi_trecv(MPIDI_OFI_global.ctx[ctx_idx].rx,
                                          *remote_upid_size,
                                          (*remote_size) * sizeof(size_t),
                                          NULL,
                                          FI_ADDR_UNSPEC,
                                          match_bits, mask_bits, &req[0].context), 0, trecv, FALSE);
        MPIDI_OFI_VCI_PROGRESS_WHILE(0, !req[0].done);

        for (i = 0; i < (*remote_size); i++)
            remote_upid_recvsize += (*remote_upid_size)[i];
        MPIR_CHKPMEM_MALLOC((*remote_upids), char *, remote_upid_recvsize,
                            mpi_errno, "remote_upids", MPL_MEM_ADDRESS);

        MPIDI_OFI_VCI_CALL_RETRY(fi_trecv(MPIDI_OFI_global.ctx[ctx_idx].rx,
                                          *remote_upids,
                                          remote_upid_recvsize,
                                          NULL,
                                          FI_ADDR_UNSPEC,
                                          match_bits, mask_bits, &req[1].context), 0, trecv, FALSE);

        MPIDI_OFI_VCI_PROGRESS_WHILE(0, !req[1].done);
        size_t disp = 0;
        for (i = 0; i < req[0].source; i++)
            disp += (*remote_upid_size)[i];
        memcpy(conname, *remote_upids + disp, (*remote_upid_size)[req[0].source]);
        MPIR_CHKPMEM_COMMIT();
    }

    if (phase == 1) {
        /* Send our table to the child */
        /* Send phase maps the entry   */
        int tag = root;
        int local_size = comm_ptr->local_size;
        size_t local_upid_sendsize = 0;

        /* Step 1: get local upids (with size) and node ids for sending */
        MPIDI_NM_get_local_upids(comm_ptr, &local_upid_size, &local_upids);
        for (i = 0; i < local_size; i++)
            local_upid_sendsize += local_upid_size[i];

        match_bits = MPIDI_OFI_init_sendtag(port_id, tag, MPIDI_OFI_DYNPROC_SEND);

        /* fi_av_map here is not quite right for some providers */
        /* we need to get this connection from the sockname     */
        req[0].done = 0;
        req[0].event_id = MPIDI_OFI_EVENT_DYNPROC_DONE;
        req[1].done = 0;
        req[1].event_id = MPIDI_OFI_EVENT_DYNPROC_DONE;
        req[2].done = 0;
        req[2].event_id = MPIDI_OFI_EVENT_DYNPROC_DONE;
        MPIDI_OFI_VCI_CALL_RETRY(fi_tsenddata(MPIDI_OFI_global.ctx[ctx_idx].tx,
                                              local_upid_size,
                                              local_size * sizeof(size_t), NULL /* desc */ ,
                                              comm_ptr->rank,
                                              *conn,
                                              match_bits,
                                              (void *) &req[0].context),
                                 0, tsenddata, FALSE /* eagain */);
        MPIDI_OFI_VCI_CALL_RETRY(fi_tsenddata(MPIDI_OFI_global.ctx[ctx_idx].tx,
                                              local_upids, local_upid_sendsize, NULL /* desc */ ,
                                              comm_ptr->rank,
                                              *conn,
                                              match_bits,
                                              (void *) &req[1].context),
                                 0, tsenddata, FALSE /* eagain */);

        MPIDI_OFI_VCI_PROGRESS_WHILE(0, !req[0].done || !req[1].done);

    }

  fn_exit:
    MPL_free(local_upid_size);
    MPL_free(local_upids);
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_DYNPROC_EXCHANGE_MAP);
    return mpi_errno;
  fn_fail:
    MPIR_CHKPMEM_REAP();
    goto fn_exit;
}

int MPIDI_OFI_mpi_comm_connect(const char *port_name, MPIR_Info * info, int root, int timeout,
                               MPIR_Comm * comm_ptr, MPIR_Comm ** newcomm)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Errflag_t errflag = MPIR_ERR_NONE;
    int remote_size = 0;
    size_t *remote_upid_size = NULL;
    char *remote_upids = NULL;
    int *remote_gpids = NULL;
    int is_low_group = -1;
    int parent_root = -1;
    int rank = comm_ptr->rank;
    fi_addr_t conn;
    int nic = 0;
    int ctx_idx = MPIDI_OFI_get_ctx_index(comm_ptr, 0, nic);

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDI_OFI_MPI_COMM_CONNECT);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDI_OFI_MPI_COMM_CONNECT);

    if (!MPIDI_OFI_ENABLE_TAGGED) {
        MPIR_Assert(0);
        goto fn_exit;
    }

    int port_id;
    int bcast_ints[2];          /* used to bcast port_id and errno */
    if (rank == root) {
        /* NOTE: do not goto fn_fail on error, or it will leave children hanging */
        mpi_errno = get_tag_from_port(port_name, &port_id);
        if (mpi_errno)
            goto bcast_errno_and_port_id;

        char conname[FI_NAME_MAX];
        mpi_errno = get_conn_name_from_port(port_name, conname);
        if (mpi_errno)
            goto bcast_errno_and_port_id;

        MPIDI_OFI_VCI_CALL(fi_av_insert
                           (MPIDI_OFI_global.ctx[ctx_idx].av, conname, 1, &conn, 0ULL, NULL), 0,
                           avmap);
        MPIR_Assert(conn != FI_ADDR_NOTAVAIL);
        mpi_errno =
            dynproc_exchange_map(root, DYNPROC_SENDER, port_id, &conn, conname, comm_ptr,
                                 &parent_root, &remote_size, &remote_upid_size, &remote_upids);
        if (mpi_errno)
            goto bcast_errno_and_port_id;

        mpi_errno = dynproc_handshake(root, DYNPROC_RECEIVER, timeout, port_id, &conn, comm_ptr);
        if (mpi_errno)
            goto bcast_errno_and_port_id;


        mpi_errno =
            dynproc_exchange_map(root, DYNPROC_RECEIVER, port_id, &conn, conname, comm_ptr,
                                 &parent_root, &remote_size, &remote_upid_size, &remote_upids);
        if (mpi_errno)
            goto bcast_errno_and_port_id;

        remote_gpids = MPL_malloc(remote_size * sizeof(int), MPL_MEM_ADDRESS);
        if (!remote_gpids) {
            MPIR_CHKMEM_SETERR(mpi_errno, remote_size * sizeof(int), "remote_gpids");
            goto bcast_errno_and_port_id;
        }

        MPIDIU_upids_to_gpids(remote_size, remote_upid_size, remote_upids, &remote_gpids);
        /* the child comm group is alawys the low group */
        is_low_group = 0;

      bcast_errno_and_port_id:
        bcast_ints[0] = port_id;
        bcast_ints[1] = mpi_errno;
        mpi_errno = MPIR_Bcast_allcomm_auto(bcast_ints, 2, MPI_INT, root, comm_ptr, &errflag);
        MPIR_ERR_CHECK(mpi_errno);
        mpi_errno = bcast_ints[1];
        MPIR_ERR_CHECK(mpi_errno);
    } else {
        mpi_errno = MPIR_Bcast_allcomm_auto(bcast_ints, 2, MPI_INT, root, comm_ptr, &errflag);
        MPIR_ERR_CHECK(mpi_errno);
        port_id = bcast_ints[0];
        if (bcast_ints[1]) {
            /* errno from root cannot be directly returned */
            MPIR_ERR_SET(mpi_errno, MPI_ERR_PORT, "**comm_connect_fail");
            goto fn_fail;
        }
    }

    /* broadcast the upids to local groups */
    mpi_errno =
        MPIDIU_Intercomm_map_bcast_intra(comm_ptr, root, &remote_size, &is_low_group, 0,
                                         remote_upid_size, remote_upids, &remote_gpids);
    MPIR_ERR_CHECK(mpi_errno);
    if (rank == root) {
        MPL_free(remote_upid_size);
        MPL_free(remote_upids);
    }

    /* Now Create the New Intercomm */
    mpi_errno =
        dynproc_create_intercomm(port_name, remote_size, remote_gpids, comm_ptr, newcomm,
                                 is_low_group, port_id, (char *) "Connect");
    MPIR_ERR_CHECK(mpi_errno);
    if (rank == root) {
        mpi_errno = MPIDI_OFI_dynproc_insert_conn(conn, (*newcomm)->rank,
                                                  MPIDI_OFI_DYNPROC_CONNECTED_CHILD,
                                                  &MPIDI_OFI_COMM(*newcomm).conn_id);
        MPIR_Assert(mpi_errno == MPI_SUCCESS);
    }
    mpi_errno = MPIR_Barrier_allcomm_auto(comm_ptr, &errflag);
    MPIR_ERR_CHECK(mpi_errno);
  fn_exit:
    /* Note: remote_gpids on children process is allocated in MPIDIU_Intercomm_map_bcast_intra */
    MPL_free(remote_gpids);

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDI_OFI_MPI_COMM_CONNECT);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

int MPIDI_OFI_mpi_comm_disconnect(MPIR_Comm * comm_ptr)
{
    int mpi_errno = MPI_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDI_OFI_MPI_COMM_DISCONNECT);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDI_OFI_MPI_COMM_DISCONNECT);

    mpi_errno = MPIR_Comm_free_impl(comm_ptr);
    MPIR_ERR_CHECK(mpi_errno);

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDI_OFI_MPI_COMM_DISCONNECT);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

int MPIDI_OFI_mpi_open_port(MPIR_Info * info_ptr, char *port_name)
{
    int mpi_errno = MPI_SUCCESS;
    int str_errno = MPL_SUCCESS;
    int port_name_tag = 0;
    int len = MPI_MAX_PORT_NAME;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDI_OFI_MPI_OPEN_PORT);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDI_OFI_MPI_OPEN_PORT);

    if (!MPIDI_OFI_ENABLE_TAGGED) {
        MPIR_Assert(0);
        goto fn_exit;
    }

    mpi_errno = get_port_name_tag(&port_name_tag);
    MPIR_ERR_CHECK(mpi_errno);
    MPIDI_OFI_STR_CALL(MPL_str_add_int_arg(&port_name, &len, PORT_NAME_TAG_KEY, port_name_tag),
                       port_str);
    /* WB TODO - Only setting up nic 0 for spawn right now. */
    MPIDI_OFI_STR_CALL(MPL_str_add_binary_arg(&port_name, &len, CONNENTR_TAG_KEY,
                                              MPIDI_OFI_global.addrname[0],
                                              MPIDI_OFI_global.addrnamelen), port_str);
  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDI_OFI_MPI_OPEN_PORT);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

int MPIDI_OFI_mpi_close_port(const char *port_name)
{
    int mpi_errno = MPI_SUCCESS;
    int port_name_tag;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDI_OFI_MPI_CLOSE_PORT);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDI_OFI_MPI_CLOSE_PORT);

    if (!MPIDI_OFI_ENABLE_TAGGED) {
        MPIR_Assert(0);
        goto fn_exit;
    }

    mpi_errno = get_tag_from_port(port_name, &port_name_tag);
    free_port_name_tag(port_name_tag);

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDI_OFI_MPI_CLOSE_PORT);
    return mpi_errno;
}

int MPIDI_OFI_mpi_comm_accept(const char *port_name, MPIR_Info * info, int root,
                              MPIR_Comm * comm_ptr, MPIR_Comm ** newcomm)
{
    int mpi_errno = MPI_SUCCESS;
    int root_errno;
    MPIR_Errflag_t errflag = MPIR_ERR_NONE;
    int remote_size = 0;
    size_t *remote_upid_size = 0;
    char *remote_upids = NULL;
    int *remote_gpids = NULL;
    int child_root = -1;
    int is_low_group = -1;
    fi_addr_t conn = 0;
    int rank = comm_ptr->rank;
    int port_id;
    int nic = 0;
    int ctx_idx = MPIDI_OFI_get_ctx_index(comm_ptr, 0, nic);

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_MPIDI_OFI_MPI_COMM_ACCEPT);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_MPIDI_OFI_MPI_COMM_ACCEPT);

    if (!MPIDI_OFI_ENABLE_TAGGED) {
        MPIR_Assert(0);
        goto fn_exit;
    }

    if (rank == root) {
        char conname[FI_NAME_MAX];

        mpi_errno = get_tag_from_port(port_name, &port_id);
        if (mpi_errno)
            goto bcast_errno;

        /* note: conn is a dummy for DYNPROC_RECEIVER (phase 0). */
        mpi_errno =
            dynproc_exchange_map(root, DYNPROC_RECEIVER, port_id, &conn, conname, comm_ptr,
                                 &child_root, &remote_size, &remote_upid_size, &remote_upids);
        if (mpi_errno)
            goto bcast_errno;

        MPIDI_OFI_VCI_CALL(fi_av_insert
                           (MPIDI_OFI_global.ctx[ctx_idx].av, conname, 1, &conn, 0ULL, NULL), 0,
                           avmap);
        MPIR_Assert(conn != FI_ADDR_NOTAVAIL);
        mpi_errno = dynproc_handshake(root, DYNPROC_SENDER, 0, port_id, &conn, comm_ptr);
        if (mpi_errno)
            goto bcast_errno;

        mpi_errno =
            dynproc_exchange_map(root, DYNPROC_SENDER, port_id, &conn, conname, comm_ptr,
                                 &child_root, &remote_size, &remote_upid_size, &remote_upids);
        if (mpi_errno)
            goto bcast_errno;

        remote_gpids = MPL_malloc(remote_size * sizeof(int), MPL_MEM_ADDRESS);
        if (!remote_gpids) {
            MPIR_CHKMEM_SETERR(mpi_errno, remote_size * sizeof(int), "remote_gpids");
            goto bcast_errno;
        }
        MPIDIU_upids_to_gpids(remote_size, remote_upid_size, remote_upids, &remote_gpids);
        /* the parent comm group is alawys the low group */
        is_low_group = 1;

      bcast_errno:
        root_errno = mpi_errno;
    }

    mpi_errno = MPIR_Bcast_allcomm_auto(&root_errno, 1, MPI_INT, root, comm_ptr, &errflag);
    MPIR_ERR_CHECK(mpi_errno);
    if (root_errno) {
        if (rank == root) {
            mpi_errno = root_errno;
        } else {
            /* errno from root cannot be directly returned */
            MPIR_ERR_SET(mpi_errno, MPI_ERR_PORT, "**comm_accept_fail");
        }
        goto fn_fail;
    }

    /* broadcast the upids to local groups */
    mpi_errno =
        MPIDIU_Intercomm_map_bcast_intra(comm_ptr, root, &remote_size, &is_low_group, 0,
                                         remote_upid_size, remote_upids, &remote_gpids);
    MPIR_ERR_CHECK(mpi_errno);
    if (rank == root) {
        MPL_free(remote_upid_size);
        MPL_free(remote_upids);
    }

    mpi_errno = MPIR_Bcast_impl(&port_id, 1, MPI_INT, root, comm_ptr, &errflag);
    MPIR_ERR_CHECK(mpi_errno);

    /* Now Create the New Intercomm */
    mpi_errno =
        dynproc_create_intercomm(port_name, remote_size, remote_gpids, comm_ptr, newcomm,
                                 is_low_group, port_id, (char *) "Accept");
    MPIR_ERR_CHECK(mpi_errno);
    if (rank == root) {
        mpi_errno = MPIDI_OFI_dynproc_insert_conn(conn, (*newcomm)->rank,
                                                  MPIDI_OFI_DYNPROC_CONNECTED_PARENT,
                                                  &MPIDI_OFI_COMM(*newcomm).conn_id);
        MPIR_Assert(mpi_errno == MPI_SUCCESS);
    }
    mpi_errno = MPIR_Barrier_allcomm_auto(comm_ptr, &errflag);
    MPIR_ERR_CHECK(mpi_errno);
  fn_exit:
    /* Note: remote_gpids on children process is allocated in MPIDIU_Intercomm_map_bcast_intra */
    MPL_free(remote_gpids);

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_MPIDI_OFI_MPI_COMM_ACCEPT);
    return mpi_errno;

  fn_fail:
    goto fn_exit;
}

/* the following functions are "proc" functions, but because they are only used during dynamic
 * process spawning, having them here provides better context */

int MPIDI_OFI_upids_to_gpids(int size, size_t * remote_upid_size, char *remote_upids,
                             int **remote_gpids)
{
    int i, mpi_errno = MPI_SUCCESS;
    int *new_avt_procs;
    char **new_upids;
    int n_new_procs = 0;
    int n_avts;
    char *curr_upid;
    int nic = 0;
    int ctx_idx = MPIDI_OFI_get_ctx_index(NULL, 0, nic);

    MPIR_CHKLMEM_DECL(2);

    MPIR_CHKLMEM_MALLOC(new_avt_procs, int *, sizeof(int) * size, mpi_errno, "new_avt_procs",
                        MPL_MEM_ADDRESS);
    MPIR_CHKLMEM_MALLOC(new_upids, char **, sizeof(char *) * size, mpi_errno, "new_upids",
                        MPL_MEM_ADDRESS);

    n_avts = MPIDIU_get_n_avts();

    curr_upid = remote_upids;
    for (i = 0; i < size; i++) {
        int j, k;
        char tbladdr[FI_NAME_MAX];
        int found = 0;
        size_t sz = 0;

        for (k = 0; k < n_avts; k++) {
            if (MPIDIU_get_av_table(k) == NULL) {
                continue;
            }
            for (j = 0; j < MPIDIU_get_av_table(k)->size; j++) {
                sz = MPIDI_OFI_global.addrnamelen;
                MPIDI_OFI_VCI_CALL(fi_av_lookup(MPIDI_OFI_global.ctx[ctx_idx].av,
                                                MPIDI_OFI_TO_PHYS(k, j, nic), &tbladdr, &sz), 0,
                                   avlookup);
                if (sz == remote_upid_size[i]
                    && !memcmp(tbladdr, curr_upid, remote_upid_size[i])) {
                    (*remote_gpids)[i] = MPIDIU_GPID_CREATE(k, j);
                    found = 1;
                    break;
                }
            }
            if (found) {
                break;
            }
        }

        if (!found) {
            new_avt_procs[n_new_procs] = i;
            new_upids[n_new_procs] = curr_upid;
            n_new_procs++;
        }
        curr_upid += remote_upid_size[i];
    }

    /* create new av_table, insert processes */
    if (n_new_procs > 0) {
        int avtid;
        mpi_errno = MPIDIU_new_avt(n_new_procs, &avtid);
        MPIR_ERR_CHECK(mpi_errno);

        for (i = 0; i < n_new_procs; i++) {
            fi_addr_t addr;
            MPIDI_OFI_VCI_CALL(fi_av_insert(MPIDI_OFI_global.ctx[ctx_idx].av, new_upids[i],
                                            1, &addr, 0ULL, NULL), 0, avmap);
            MPIR_Assert(addr != FI_ADDR_NOTAVAIL);
            MPIDI_OFI_AV(&MPIDIU_get_av(avtid, i)).dest[nic][0] = addr;
            /* highest bit is marked as 1 to indicate this is a new process */
            (*remote_gpids)[new_avt_procs[i]] = MPIDIU_GPID_CREATE(avtid, i);
        }
    }

  fn_exit:
    MPIR_CHKLMEM_FREEALL();
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

int MPIDI_OFI_get_local_upids(MPIR_Comm * comm, size_t ** local_upid_size, char **local_upids)
{
    int mpi_errno = MPI_SUCCESS;
    int i, total_size = 0;
    char *temp_buf = NULL, *curr_ptr = NULL;
    int nic = 0;
    int ctx_idx = MPIDI_OFI_get_ctx_index(comm, 0, nic);

    MPIR_CHKPMEM_DECL(2);
    MPIR_CHKLMEM_DECL(1);

    MPIR_CHKPMEM_MALLOC((*local_upid_size), size_t *, comm->local_size * sizeof(size_t),
                        mpi_errno, "local_upid_size", MPL_MEM_ADDRESS);
    MPIR_CHKLMEM_MALLOC(temp_buf, char *, comm->local_size * MPIDI_OFI_global.addrnamelen,
                        mpi_errno, "temp_buf", MPL_MEM_BUFFER);

    for (i = 0; i < comm->local_size; i++) {
        (*local_upid_size)[i] = MPIDI_OFI_global.addrnamelen;
        MPIDI_OFI_addr_t *av = &MPIDI_OFI_AV(MPIDIU_comm_rank_to_av(comm, i));
        MPIDI_OFI_VCI_CALL(fi_av_lookup(MPIDI_OFI_global.ctx[ctx_idx].av, av->dest[nic][0],
                                        &temp_buf[i * MPIDI_OFI_global.addrnamelen],
                                        &(*local_upid_size)[i]), 0, avlookup);
        total_size += (*local_upid_size)[i];
    }

    MPIR_CHKPMEM_MALLOC((*local_upids), char *, total_size * sizeof(char),
                        mpi_errno, "local_upids", MPL_MEM_BUFFER);
    curr_ptr = (*local_upids);
    for (i = 0; i < comm->local_size; i++) {
        memcpy(curr_ptr, &temp_buf[i * MPIDI_OFI_global.addrnamelen], (*local_upid_size)[i]);
        curr_ptr += (*local_upid_size)[i];
    }

    MPIR_CHKPMEM_COMMIT();
  fn_exit:
    MPIR_CHKLMEM_FREEALL();
    return mpi_errno;
  fn_fail:
    MPIR_CHKPMEM_REAP();
    goto fn_exit;
}

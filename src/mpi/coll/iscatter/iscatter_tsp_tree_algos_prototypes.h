/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *  (C) 2006 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 *
 *  Portions of this code were written by Intel Corporation.
 *  Copyright (C) 2011-2017 Intel Corporation.  Intel provides this material
 *  to Argonne National Laboratory subject to Software Grant and Corporate
 *  Contributor License Agreement dated February 8, 2012.
 */

/* Header protection (i.e., ISCATTER_TSP_TREE_ALGOS_PROTOTYPES_H_INCLUDED) is
 * intentionally omitted since this header might get included multiple
 * times within the same .c file. */

#include "tsp_namespace_def.h"

#undef MPIR_TSP_Iscatter_intra_tree
#define MPIR_TSP_Iscatter_intra_tree                      MPIR_TSP_NAMESPACE(Iscatter_intra_tree)
#undef MPIR_TSP_Iscatter_sched_intra_tree
#define MPIR_TSP_Iscatter_sched_intra_tree                MPIR_TSP_NAMESPACE(Iscatter_sched_intra_tree)

int MPIR_TSP_Iscatter_sched_intra_tree(const void *sendbuf, int sendcount,
                                       MPI_Datatype sendtype, void *recvbuf, int recvcount,
                                       MPI_Datatype recvtype, int root, MPIR_Comm * comm_ptr,
                                       int k, MPIR_TSP_sched_t * sched);
int MPIR_TSP_Iscatter_intra_tree(const void *sendbuf, int sendcount,
                                 MPI_Datatype sendtype, void *recvbuf, int recvcount,
                                 MPI_Datatype recvtype, int root, MPIR_Comm * comm_ptr,
                                 MPIR_Request ** request, int k);

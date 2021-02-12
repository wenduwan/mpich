/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "mpiimpl.h"

int MPIR_Gatherv_allcomm_nb(const void *sendbuf, MPI_Aint sendcount, MPI_Datatype sendtype,
                            void *recvbuf, const int *recvcounts, const int *displs,
                            MPI_Datatype recvtype, int root, MPIR_Comm * comm_ptr,
                            MPIR_Errflag_t * errflag)
{
    int mpi_errno = MPI_SUCCESS;
    MPI_Request req = MPI_REQUEST_NULL;
    MPIR_Request *req_ptr = NULL;

    /* just call the nonblocking version and wait on it */
    mpi_errno =
        MPIR_Igatherv(sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs, recvtype, root,
                      comm_ptr, &req_ptr);
    MPIR_ERR_CHECK(mpi_errno);
    if (req_ptr)
        req = req_ptr->handle;

    mpi_errno = MPIR_Wait(&req, MPI_STATUS_IGNORE);
    MPIR_ERR_CHECK(mpi_errno);

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

/*
  This file is part of MADNESS.

  Copyright (C) 2007,2010 Oak Ridge National Laboratory

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

  For more information please contact:

  Robert J. Harrison
  Oak Ridge National Laboratory
  One Bethel Valley Road
  P.O. Box 2008, MS-6367

  email: harrisonrj@ornl.gov
  tel:   865-241-3937
  fax:   865-572-0680

  $Id$
*/

#include <world/worldrmi.h>
#include <world/posixmem.h>
#include <world/worldtime.h>
#include <iostream>
#include <algorithm>
#include <utility>
#include <sstream>

namespace madness {

    RMI* RMI::instance_ptr = 0;

#if HAVE_INTEL_TBB
    tbb::empty_task* RMI::tbb_rmi_parent_task = 0;
#endif

    bool RMI::is_ordered(attrT attr) {
        return attr & ATTR_ORDERED;
    }

    void RMI::run() {
        ThreadBase::set_affinity(1); // The RMI thread is logical thread 1
        if (debugging)
            std::cerr << rank << ":RMI: server thread is running" << std::endl;
        // The RMI server thread spends its life in here

        ScopedArray<SafeMPI::Status> status(new SafeMPI::Status[maxq_]);
        ScopedArray<int> ind(new int[maxq_]);
        ScopedArray<qmsg> q(new qmsg[maxq_]);
        int n_in_q = 0;

        while (1) {

            if (debugging && n_in_q)
                std::cerr << rank << ":RMI: about to call Waitsome with "
                          << n_in_q << " messages in the queue" << std::endl;

            // If MPI is not safe for simultaneous entry by multiple threads we
            // cannot call Waitsome ... have to poll via Testsome
            int narrived;

            MutexWaiter waiter;
            while (!(narrived = SafeMPI::Request::Testsome(maxq_, recv_req.get(), ind.get(), status.get()))) {
                if (finished) return;
#if defined(HAVE_CRAYXT) || defined(HAVE_IBMBGP)
                myusleep(1);
#else
                waiter.wait();
#endif
            }

#ifndef HAVE_CRAYXT
            waiter.reset();
#endif

            if (debugging)
                std::cerr << rank << ":RMI: " << narrived
                          << " messages just arrived" << std::endl;

            if (narrived) {
                for (int m=0; m<narrived; ++m) {
                    int src = status[m].Get_source();
                    size_t len = status[m].Get_count(MPI_BYTE);
                    int i = ind[m];

                    ++(stats.nmsg_recv);
                    stats.nbyte_recv += len;

                    const header* h = (const header*)(recv_buf[i]);
                    rmi_handlerT func = h->func;
                    attrT attr = h->attr;
                    counterT count = (attr>>16); //&&0xffff;

                    if (!is_ordered(attr) || count==recv_counters[src]) {
                        // Unordered and in order messages should be digested as soon as possible.
                        if (debugging)
                            std::cerr << rank
                                      << ":RMI: invoking from=" << src
                                      << " nbyte=" << len
                                      << " func=" << func
                                      << " ordered=" << is_ordered(attr)
                                      << " count=" << count
                                      << std::endl;

                        if (is_ordered(attr)) ++(recv_counters[src]);
                        func(recv_buf[i], len);
                        post_recv_buf(i);
                    }
                    else {
                        if (debugging)
                            std::cerr << rank
                                      << ":RMI: enqueing from=" << src
                                      << " nbyte=" << len
                                      << " func=" << func
                                      << " ordered=" << is_ordered(attr)
                                      << " fromcount=" << count
                                      << " herecount=" << int(recv_counters[src])
                                      << std::endl;
                        // Shove it in the queue
                        int n = n_in_q++;
                        if (n >= maxq_) MADNESS_EXCEPTION("RMI:server: overflowed out-of-order message q\n", n);
                        q[n] = qmsg(len, func, i, src, attr, count);
                    }
                }

                // Only ordered messages can end up in the queue due to
                // out-of-order receipt or order of recv buffer processing.

                // Sort queued messages by ascending recv count
                std::sort(q.get(),q.get()+n_in_q);

                // Loop thru messages ... since we have sorted only one pass
                // is necessary and if we cannot process a message we
                // save it at the beginning of the queue
                int nleftover = 0;
                for (int m=0; m<n_in_q; ++m) {
                    int src = q[m].src;
                    if (q[m].count == recv_counters[src]) {
                        if (debugging)
                            std::cerr << rank
                                      << ":RMI: queue invoking from=" << src
                                      << " nbyte=" << q[m].len
                                      << " func=" << q[m].func
                                      << " ordered=" << is_ordered(q[m].attr)
                                      << " count=" << q[m].count
                                      << std::endl;

                        ++(recv_counters[src]);
                        q[m].func(recv_buf[q[m].i], q[m].len);
                        post_recv_buf(q[m].i);
                    }
                    else {
                        q[nleftover++] = q[m];
                        if (debugging)
                            std::cerr << rank
                                      << ":RMI: queue pending out of order from=" << src
                                      << " nbyte=" << q[m].len
                                      << " func=" << q[m].func
                                      << " ordered=" << is_ordered(q[m].attr)
                                      << " count=" << q[m].count
                                      << std::endl;
                    }
                }
                n_in_q = nleftover;

                post_pending_huge_msg();
            }
        }
    }

    void RMI::post_pending_huge_msg() {
        if (recv_buf[nrecv_]) return;      // Message already pending
        if (!hugeq.empty()) {
            int src = hugeq.front().first;
            size_t nbyte = hugeq.front().second;
            hugeq.pop_front();
            if (posix_memalign(&recv_buf[nrecv_], ALIGNMENT, nbyte))
                MADNESS_EXCEPTION("RMI: failed allocating huge message", 1);
            recv_req[nrecv_] = comm.Irecv(recv_buf[nrecv_], nbyte, MPI_BYTE, src, SafeMPI::RMI_HUGE_DAT_TAG);
            int nada=0;
#ifdef MADNESS_USE_BSEND_ACKS
            comm.Bsend(&nada, sizeof(nada), MPI_BYTE, src, SafeMPI::RMI_HUGE_ACK_TAG);
#else
            comm.Send(&nada, sizeof(nada), MPI_BYTE, src, SafeMPI::RMI_HUGE_ACK_TAG);
#endif // MADNESS_USE_BSEND_ACKS
        }
    }

    void RMI::post_recv_buf(int i) {
        if (i < nrecv_) {
            recv_req[i] = comm.Irecv(recv_buf[i], max_msg_len_, MPI_BYTE, MPI_ANY_SOURCE, SafeMPI::RMI_TAG);
        }
        else if (i == nrecv_) {
            free(recv_buf[i]);
            recv_buf[i] = 0;
            post_pending_huge_msg();
        }
        else {
            MADNESS_EXCEPTION("RMI::post_recv_buf: confusion", i);
        }
    }

    RMI::~RMI() {
        //         if (!SafeMPI::Is_finalized()) {
        //             for (int i=0; i<nrecv_; ++i) {
        //                 if (!recv_req[i].Test())
        //                     recv_req[i].Cancel();
        //             }
        //         }
        //for (int i=0; i<nrecv_; ++i) free(recv_buf[i]);
    }

    RMI::RMI()
            : comm(SafeMPI::COMM_WORLD)
            , nproc(comm.Get_size())
            , rank(comm.Get_rank())
            , debugging(false)
            , finished(false)
            , send_counters(new unsigned short[nproc])
            , recv_counters(new unsigned short[nproc])
            , max_msg_len_(DEFAULT_MAX_MSG_LEN)
            , nrecv_(DEFAULT_NRECV)
            , maxq_(DEFAULT_NRECV + 1)
            , recv_buf()
            , recv_req()
    {
        // Get the maximum buffer size from the MAD_BUFFER_SIZE environment
        // variable.
        const char* mad_buffer_size = getenv("MAD_BUFFER_SIZE");
        if(mad_buffer_size) {
            // Convert the string into bytes
            std::stringstream ss(mad_buffer_size);
            double memory = 0.0;
            if(ss >> memory) {
                if(memory > 0.0) {
                    std::string unit;
                    if(ss >> unit) { // Failure == assume bytes
                        if(unit == "KB" || unit == "kB") {
                            memory *= 1024.0;
                        } else if(unit == "MB") {
                            memory *= 1048576.0;
                        } else if(unit == "GB") {
                            memory *= 1073741824.0;
                        }
                    }
                }
            }

            max_msg_len_ = memory;
            // Check that the size of the receive buffers is reasonable.
            if(max_msg_len_ < 1024) {
                max_msg_len_ = DEFAULT_MAX_MSG_LEN; // = 3*512*1024
                std::cerr << "!!! WARNING: MAD_BUFFER_SIZE must be at least 1024 bytes.\n"
                          << "!!! WARNING: Increasing MAD_BUFFER_SIZE to the default size, " <<  max_msg_len_ << " bytes.\n";
            }
            // Check that the buffer has the correct alignment
            const std::size_t unaligned = max_msg_len_ % ALIGNMENT;
            if(unaligned != 0)
                max_msg_len_ += ALIGNMENT - unaligned;
        }

        // Get the number of receive buffers from the MAD_RECV_BUFFERS
        // environment variable.
        const char* mad_recv_buffs = getenv("MAD_RECV_BUFFERS");
        if(mad_recv_buffs) {
            std::stringstream ss(mad_recv_buffs);
            ss >> nrecv_;
            // Check that the number of receive buffers is reasonable.
            if(nrecv_ < DEFAULT_NRECV) {
                nrecv_ = DEFAULT_NRECV;
                std::cerr << "!!! WARNING: MAD_RECV_BUFFERS must be at least " << DEFAULT_NRECV << ".\n"
                          << "!!! WARNING: Increasing MAD_RECV_BUFFERS to " << nrecv_ << ".\n";
            }
            maxq_ = nrecv_ + 1;
        }

        // Allocate memory for receive buffer and requests
        recv_buf.reset(new void*[maxq_]);
        recv_req.reset(new Request[maxq_]);

        // Initialize the send/recv counts
        std::fill_n(send_counters.get(), nproc, 0);
        std::fill_n(recv_counters.get(), nproc, 0);

        // Allocate recive buffers
        if(nproc > 1) {
            for(int i = 0; i < nrecv_; ++i) {
                if(posix_memalign(&recv_buf[i], ALIGNMENT, max_msg_len_))
                    MADNESS_EXCEPTION("RMI:initialize:failed allocating aligned recv buffer", 1);
                post_recv_buf(i);
            }
            recv_buf[nrecv_] = 0;
#if HAVE_INTEL_TBB
            tbb_rmi_parent_task = new(tbb::task::allocate_root()) tbb::empty_task;
            tbb_rmi_parent_task->set_ref_count(2);

            RMI_TBB_TASK* rmi_task = new (tbb_rmi_parent_task->allocate_child()) RMI_TBB_TASK(this);

            tbb_rmi_parent_task->enqueue(*rmi_task);
#else
            start();
#endif
        }
    }

    RMI* RMI::instance() {
        if (!instance_ptr) {
            instance_ptr = new RMI();
        }
        return instance_ptr;
    }

    void RMI::huge_msg_handler(void *buf, size_t /*nbytein*/) {
        const size_t* info = (size_t *)(buf);
        int nword = HEADER_LEN/sizeof(size_t);
        int src = info[nword];
        size_t nbyte = info[nword+1];

        instance()->hugeq.push_back(std::make_pair(src,nbyte));
        instance()->post_pending_huge_msg();
    }

    RMI::Request RMI::private_isend(const void* buf, size_t nbyte, ProcessID dest, rmi_handlerT func, attrT attr) {
        int tag = SafeMPI::RMI_TAG;

        if (nbyte > max_msg_len_) {
            // Huge message protocol ... send message to dest indicating size and origin of huge message.
            // Remote end posts a buffer then acks the request.  This end can then send.
            const int nword = HEADER_LEN/sizeof(size_t);
            size_t info[nword+2];
            info[nword  ] = rank;
            info[nword+1] = nbyte;

            int ack;
            Request req_ack = comm.Irecv(&ack, sizeof(ack), MPI_BYTE, dest, SafeMPI::RMI_HUGE_ACK_TAG);
            Request req_send = private_isend(info, sizeof(info), dest, RMI::huge_msg_handler, ATTR_UNORDERED);

            MutexWaiter waiter;
            while (!req_send.Test()) waiter.wait();
            waiter.reset();
            while (!req_ack.Test()) waiter.wait();

            tag = SafeMPI::RMI_HUGE_DAT_TAG;
        }
        else if (nbyte < HEADER_LEN) {
            MADNESS_EXCEPTION("RMI::isend --- your buffer is too small to hold the header", static_cast<int>(nbyte));
        }

        if (debugging)
            std::cerr << instance_ptr->rank
                      << ":RMI: sending buf=" << buf
                      << " nbyte=" << nbyte
                      << " dest=" << dest
                      << " func=" << func
                      << " ordered=" << is_ordered(attr)
                      << " count=" << int(send_counters[dest])
                      << std::endl;

        // Since most uses are ordered and we need the mutex to accumulate stats
        // we presently always get the lock
        lock();

        // If ordering need the mutex to enclose sending the message
        // otherwise there is a livelock scenario due to a starved thread
        // holding an early counter.
        if (is_ordered(attr)) {
            //lock();
            attr |= ((send_counters[dest]++)<<16);
        }

        header* h = (header*)(buf);
        h->func = func;
        h->attr = attr;

        ++(stats.nmsg_sent);
        stats.nbyte_sent += nbyte;

        Request result = comm.Isend(buf, nbyte, MPI_BYTE, dest, tag);

        //if (is_ordered(attr)) unlock();
        unlock();

        return result;
    }

    void RMI::private_exit() {
        if (debugging)
            std::cerr << instance_ptr->rank << ":RMI: sending exit request to server thread" << std::endl;

        finished = true;
        myusleep(10000);

        //delete this;
    }

    RMI::Request RMI::isend(const void* buf, size_t nbyte, ProcessID dest, rmi_handlerT func, unsigned int attr) {
        return instance()->private_isend(buf, nbyte, dest, func, attr);
    }

    void RMI::end() {
        if (instance_ptr) instance_ptr->private_exit();
    }

    void RMI::begin() {
        instance();
    }

    void RMI::set_debug(bool status) {
        instance()->debugging = status;
    }

    bool RMI::get_debug() {
        return instance()->debugging;
    }

    const RMIStats& RMI::get_stats() {
        return instance()->stats;
    }

} // namespace madness

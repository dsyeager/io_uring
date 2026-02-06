#pragma once

/*
io_uring(7) — Linux manual page
#include <linux/io_uring.h>

After you add one or more SQEs, you need to call
    io_uring_enter(2) (if polling mode is not being used)

io_uring supports a polling mode that lets you
    avoid the call to io_uring_enter(2), which you use to inform the
    kernel that you have queued SQEs on to the SQ.  With SQ Polling,
    io_uring starts a kernel thread that polls the submission queue
    for any I/O requests you submit by adding SQEs.  With SQ Polling
    enabled, there is no need for you to call io_uring_enter(2),
    letting you avoid the overhead of system calls.

IORING_ENTER_SQ_WAKEUP
    If the ring has been created with IORING_SETUP_SQPOLL, then
    this flag asks the kernel to wakeup the SQ kernel thread to submit IO.



Questions:
- How does does a process using IO uring effect that process's priority in the system? Does the kernel's IO scheduler treat them as individual requests?

*/

// NOTE: this sample code started as the example given in man page for io_uring
//       made it pretty to my eyes then converted it to C++ with some guess work

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <iostream>
#include <tuple>

#include "log.h"
#include "misc.h"

template<typename SRC_TYPE, typename OFF_TYPE, typename DEST_TYPE>
void from_offset(SRC_TYPE base, OFF_TYPE bytes, DEST_TYPE &destination)
{
    destination = reinterpret_cast<DEST_TYPE>(static_cast<char*>(base) + bytes);
}

/*
   Some good reading: https://github.com/axboe/liburing/wiki/io_uring-and-networking-in-2023
   Good examples on buffer rings

Requirements:
  - open a file for reading or writing asynch
      - ::open(filename, flags, request_pointer);
      - might not be needed, if the normal file system is not being used
  - close a file asynch
  - potentially have multiple client requests reading from the same file, so would need pread functionality
  - reads and writes should accept a pointer that is returned after the operation completes, like a pointer to the client request
Questions:
  - Need to connect the completion of an operation to the actual request for further action
     - should the apps main event loop poll this class for completed operations then loop through them
     - or should the app pass in a call back function that is call as each operation completes
     - either way still requires the apps main loop to start the logic ::get_complete or ::process_complete
     - probably cleaner to use a ::process_complete(io_complete_function*(request*)) 

to open a file asynch:
    Get a Submission Queue Entry (SQE): Request a free SQE from the io_uring instance using io_uring_get_sqe().
    Prepare the Request: Initialize the SQE for the desired operation.
        Use io_uring_prep_openat() or io_uring_prep_openat2() to set up the parameters for opening 
        the file (directory file descriptor dfd, path, flags, mode, etc.).

    Submit the Request: Tell the kernel that the request is ready for processing using io_uring_submit().
        The kernel will then execute the open operation asynchronously.
*/

template<class EVENT_CLASS>
class io_uring_wrapper
{
public:
    io_uring_wrapper(uint32_t queue_depth)
        : m_queue_depth(queue_depth)
    {
        struct io_uring_params params;

        memset(&params, 0, sizeof(params));
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 5000; // Set idle timeout to 5 seconds, not handling the timeout for now

        //int ret = io_uring_queue_init_params(queue_depth, &m_ring, &params);
        // enabling SQPOLL increased both CPU and test run times by 30%
        // maybe there were additional settings needed?

        int ret = io_uring_queue_init(queue_depth, &m_ring, 0);
        if (ret < 0)
        {
            ERROR << "io_uring_queue_init: " << ::strerror(-ret) << ENDL;
            m_valid = false;
            return;
        }
    }

    ~io_uring_wrapper()
    {
        if (m_valid)
        {
            io_uring_queue_exit(&m_ring);
        }
    }

    int submit()
    {
        if (!m_valid)
            return -1;
        int ret = io_uring_submit(&m_ring);
        if (ret < 0)
        {
            ERROR << "io_uring_submit failed: " << ::strerror(-ret) << ENDL;
        }
        return ret;
    }

    bool prep_open_at(int dir_fd, const char *path, int flags, mode_t mode, void *data)
    {
        io_uring_sqe *sqe = get_sqe();

        if (!sqe)
            return false;

        io_uring_sqe_set_data(sqe, data);

        io_uring_prep_openat(sqe, dir_fd, path, flags, mode);

        if (!m_multishot)
            m_pending++;

        return true;
    }

    bool prep_write(int fd, const char *buffer, size_t len, off_t offset, void *data)
    {
        if (!m_valid)
            return false;

        io_uring_sqe *sqe = get_sqe();

        if (!sqe)
        {
            ERROR << "io_uring_get_sqe: failed to get SQE" << ENDL;
            return false;
        }

        io_uring_sqe_set_data(sqe, data);

        io_uring_prep_write(sqe, fd, buffer, len, offset);

        if (!m_multishot)
            m_pending++;

        return true;
    }

    bool prep_read(int fd, char *buffer, size_t sz, off_t offset, void *data)
    {
        if (!m_valid)
            return false;

        io_uring_sqe *sqe = get_sqe();

        if (!sqe)
        {
            ERROR << "io_uring_get_sqe: failed to get SQE" << ENDL;
            return false;
        }

        io_uring_sqe_set_data(sqe, data);

        io_uring_prep_read(sqe, fd, buffer, sz, offset);

        if (!m_multishot)
            m_pending++;

        return true;
    }
    /*
       example code: https://git.kernel.dk/cgit/liburing/tree/examples/proxy.c
       I have not found any examples using a buffer ring to hold per accept addrinfo_in data, maybe not needed.
    */

    bool prep_multishot_accept(int fd, void *data)
    {
        if (!m_valid)
            return false;

        io_uring_sqe *sqe = get_sqe();

        if (!sqe)
        {
            ERROR << "get_sqe: failed to get SQE" << ENDL;
            return false;
        }

        io_uring_sqe_set_data(sqe, data);

        // void io_uring_prep_multishot_accept(struct io_uring_sqe *sqe, int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
        // pass nullptr for addr and addrlen to use the buffer rings
        // flags is zero for now

        io_uring_prep_multishot_accept(sqe, fd, nullptr, nullptr, 0); 
        m_multishot = true;
        return true;
    }

    bool prep_connect(int fd, const sockaddr *addr, socklen_t addrlen, void *data)
    {
        io_uring_sqe *sqe = get_sqe();

        if (!sqe)
            return false;

        io_uring_sqe_set_data(sqe, data);

        io_uring_prep_connect(sqe, fd, addr, addrlen); 

        if (!m_multishot)
            m_pending++;
        return true;
    }

    bool prep_close(int fd, void *data)
    {
        io_uring_sqe *sqe = get_sqe();

        if (!sqe)
            return false;

        io_uring_sqe_set_data(sqe, data);

        io_uring_prep_close(sqe, fd);

        if (!m_multishot)
            m_pending++;

        return true;
    }

    uint32_t process_events()
    {
        if (!m_valid)
        {
            ERROR << "m_valid == false" << ENDL;
            return 0;
        }

        if (!m_multishot && !m_pending)
        {
            DEBUG(5) << "m_pending: " << m_pending << ENDL;
            return 0;
        }

        io_uring_cqe *cqe = nullptr;
        uint32_t i = 0;
        unsigned head;
        uint32_t new_events = 0;

        io_uring_for_each_cqe(&m_ring, head, cqe)
        {
             if (!m_multishot)
                 m_pending--; // decrement prior to ::process potentially incrementing
             EVENT_CLASS *req = reinterpret_cast<EVENT_CLASS*>(io_uring_cqe_get_data(cqe));
             uint32_t events = req->process_io_uring(cqe->res);
             DEBUG(3) << "called process_io_uring, events: " << events << ENDL;
             new_events += events;
             i++;
        }

        DEBUG(2) << "batch events: " << i << ", new events: " << new_events << ENDL;

        if (new_events)
            this->submit();

        if (i > 0)
            io_uring_cq_advance(&m_ring, i);

        return i;
    }

    bool is_valid() const { return m_valid; }

    uint32_t pending() const { return m_pending; }

private:
    io_uring m_ring;
    uint32_t m_queue_depth = 10;
    uint32_t m_pending = 0;
    bool m_valid = true;
    bool m_multishot = false;

    // io_uring allows you to supply multiple buffer rings (rings of buffers), each can have a diff/uniq size if desired
    // then io_uring chooses which buffer to use based on the incoming event
    // might use that if we were doing accepts and reads from the same io_uring ring
    // for now we won't use it but want to plumb in the storage to handle it, kind of

    //std::vector<io_uring_buf_ring*> m_buff_rings; // not used yet, maybe never

    // what is best way to store multiple arrays of buffers (array of arrays)
    // tracking the number of arrays and the length of each buffer
    // allow user to call ->add_ring_buffer(number_of_buffers, size_of_buffers)

private:
    io_uring_sqe* get_sqe()
    {
        if (!m_valid)
            return nullptr;

        io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);

        if (!sqe)
        {
            // MAN: the  SQ  ring is currently full and entries must be submitted for processing before new ones can get allocated
            WARN << "io_uring_get_sqe: failed to get SQE, calling submit and retrying" << ENDL;
            this->submit();
            sqe = io_uring_get_sqe(&m_ring);
            if (!sqe)
            {
                ERROR << "io_uring_get_sqe: failed to get SQE on 2nd try, failing" << ENDL;
            }
        }
        return sqe;
    }

    /**
      Add buffers for use by io_uring.
      Initial use case is for multishot accept but could be used by most of the io_uring apis

      should be able to pass in an array of objects or and array of char*
      element size is redundant with actual types that you can run sizeof() on but 
      when an array of char buffers is passed we need to know how big each buffer is

      sockaddr_in *addrs = new sockaddr_in[1000];
      setup_buffer_ring(1000, addrs) // pass in an array of 1000 sockaddr_in objects, the pointers are alligned properly


      char **buffers = new char*[1000];
      char *buff = new char[1000 * 1024]; // single large buffer to avoid allocation overhead
      for (uint32_t i = 0; i < 1000; i++)
      {
          buffers[i] = buff + (i * 1024);
      }
      setup_buffer_ring(1000, buffers, 1024) // pass in an array of 1000 char buffers of 1024 bytes each

    */
/*
    template<typename DTYPE>
    io_uring_buf_ring* setup_buffer_ring(uint32_t buff_cnt, DTYPE *array_ptr, size_t element_size = sizeof(DTYPE), int32_t buff_ring_id = -1)
    {
	    io_uring_buf_reg reg = { };
	    io_uring_buf_ring *br = nullptr;

	    // allocate mem for sharing buffer ring
	    if (posix_memalign((void **) &br, 4096, buff_cnt * sizeof(struct io_uring_buf_ring)))
		    return nullptr;

	    // assign and register buffer ring
	    reg.ring_addr = (unsigned long) br;
	    reg.ring_entries = buff_cnt;
	    reg.bgid = buff_ring_id;

	    if (io_uring_register_buf_ring(&m_ring, &reg, 0))
		    return nullptr;

	    // add initial buffers to the ring
	    io_uring_buf_ring_init(br);
	    for (uint32_t i = 0; i < buff_cnt; i++)
        {
		    // add each buffer, we'll use i as the buffer ID
		    io_uring_buf_ring_add(br, bufs[i], BUF_SIZE, i, io_uring_buf_ring_mask(buff_cnt), i);
	    }

	    // we've supplied buffers, make them visible to the kernel
	    io_uring_buf_ring_advance(br, buff_cnt);
	    return br;
    }
*/

};

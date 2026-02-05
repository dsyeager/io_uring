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

#include "misc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/io_uring.h>

#include <atomic>
#include <charconv>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using std::cout, std::cerr, std::endl, std::string, std::string_view, std::vector;
using namespace std::literals;

// Macros for barriers needed by io_uring
// The C example used #defines, casting to _Atomic with typeof, 
// for C++ you need std::atomic and decltype which was treating the p* as a p*& creating compile errors
// So I rewrote the defines as template functions for hopefully better type handling

//#define io_uring_smp_store_release(p, v) atomic_store_explicit((std::atomic<typeof(*(p)) *>)(p), (v), std::memory_order_release)
//#define io_uring_smp_load_acquire(p) atomic_load_explicit((std::atomic<typeof(*(p)) *>)(p), std::memory_order_acquire)

template<typename P_TYPE>
void io_uring_smp_store_release(P_TYPE *p, P_TYPE &v)
{
    std::atomic_store_explicit((std::atomic<P_TYPE>*)p, v, std::memory_order_release);
}

template<typename P_TYPE>
P_TYPE io_uring_smp_load_acquire(P_TYPE *p)
{
    return std::atomic_load_explicit((std::atomic<P_TYPE>*)p, std::memory_order_acquire);
}

template<typename SRC_TYPE, typename OFF_TYPE, typename DEST_TYPE>
void from_offset(SRC_TYPE base, OFF_TYPE bytes, DEST_TYPE &destination)
{
    destination = reinterpret_cast<DEST_TYPE>(static_cast<char*>(base) + bytes);
}

/*
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

template<class REQUEST>
class io_uring_wrapper
{
private:
    int m_ring_fd = -1;

    unsigned *m_sring_tail = nullptr;
    unsigned *m_sring_mask = nullptr;
    unsigned *m_sring_array = nullptr;
    unsigned *m_cring_head = nullptr;
    unsigned *m_cring_tail = nullptr;
    unsigned *m_cring_mask = nullptr;
    struct io_uring_sqe *m_sqes = nullptr;
    struct io_uring_cqe *m_cqes = nullptr;
    bool m_is_setup = false;
    uint64_t m_counter = 0;

    // System call wrappers provided since glibc does not yet
    // provide wrappers for io_uring system calls.

    // NOTE: man suggests these are actual functions in liburing.h
    // with some additional args, should try the actual syscalls

    int32_t io_uring_setup(unsigned entries, struct io_uring_params *p)
    {
        int32_t ret;
        ret = syscall(__NR_io_uring_setup, entries, p);
        return (ret < 0) ? -errno : ret;
    }

    int32_t io_uring_enter(int32_t ring_fd, uint32_t to_submit, uint32_t min_complete, uint32_t flags)
    {
        int32_t ret;
        ret = syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
        return (ret < 0) ? -errno : ret;
    }

    int32_t app_setup_uring(uint32_t queue_depth)
    {

        // See io_uring_setup(2) for io_uring_params.flags you can set
        // Consider using IORING_SETUP_SQPOLL to avoid some system calls. 
        // Looks likes there are some caveats to consider.

        struct io_uring_params p;
        memset(&p, 0, sizeof(p));
        m_ring_fd = io_uring_setup(queue_depth, &p);
        if (m_ring_fd < 0)
        {
            perror("io_uring_setup");
            return 1;
        }

        // io_uring communication happens via 2 shared kernel-user space ring
        // buffers, which can be jointly mapped with a single mmap() call in
        // kernels >= 5.4.

        int sring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
        int cring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

        // Rather than check for kernel version, the recommended way is to
        // check the features field of the io_uring_params structure, which is a
        // bitmask. If IORING_FEAT_SINGLE_MMAP is set, we can do away with the
        // second mmap() call to map in the completion ring separately.

        if (p.features & IORING_FEAT_SINGLE_MMAP)
        {
            if (cring_sz > sring_sz)
                sring_sz = cring_sz;
            cring_sz = sring_sz;
        }

        // Map in the submission and completion queue ring buffers.
        // Kernels < 5.4 only map in the submission queue, though.

        char *sq_ptr = static_cast<char*>(mmap(0,
                                               sring_sz,
                                               PROT_READ | PROT_WRITE,
                                               MAP_SHARED | MAP_POPULATE,
                                               m_ring_fd,
                                               IORING_OFF_SQ_RING));

        if (sq_ptr == MAP_FAILED)
        {
            perror("mmap");
            return 1;
        }

        char *cq_ptr = nullptr;

        if (p.features & IORING_FEAT_SINGLE_MMAP)
        {
            cq_ptr = sq_ptr;
        }
        else
        {
            // Map in the completion queue ring buffer in older kernels separately
            cq_ptr = (char*) mmap(0,
                                  cring_sz,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE,
                                  m_ring_fd,
                                  IORING_OFF_CQ_RING);

            if (cq_ptr == MAP_FAILED)
            {
                perror("mmap");
                return 1;
            }
        }

        // Save useful fields for later easy reference
        from_offset(sq_ptr, p.sq_off.tail, m_sring_tail);
        from_offset(sq_ptr, p.sq_off.ring_mask, m_sring_mask);
        from_offset(sq_ptr, p.sq_off.array, m_sring_array);

        // Map in the submission queue entries array
        m_sqes = static_cast<io_uring_sqe*>(mmap(0,
                                                 p.sq_entries * sizeof(struct io_uring_sqe),
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_SHARED | MAP_POPULATE,
                                                 m_ring_fd,
                                                 IORING_OFF_SQES));

        if (m_sqes == MAP_FAILED)
        {
            perror("mmap");
            return 1;
        }

        // Save useful fields for later easy reference
        from_offset(cq_ptr, p.cq_off.head, m_cring_head);
        from_offset(cq_ptr, p.cq_off.tail, m_cring_tail);
        from_offset(cq_ptr, p.cq_off.ring_mask, m_cring_mask);
        from_offset(cq_ptr, p.cq_off.cqes, m_cqes);
    
        cout << "io_uring setup completed" << endl;
        m_is_setup = true;
        return 0;
    }

public:
    io_uring_wrapper(uint32_t queue_depth)
    {
        app_setup_uring(queue_depth); 
    }

    ~io_uring_wrapper()
    {
        // TODO: probably need to clean up the mmaps, will be fine initially as long as this object lives for the life of the app
    }

    bool is_setup() const { return m_is_setup; }

    // Read from completion queue.
    // In this function, we read completion events from the completion queue.
    // We dequeue the CQE, update and head and return the result of the operation.

    int read_from_cq()
    {
        if (!m_counter)
            return -1;

        // Read barrier
        uint32_t head = io_uring_smp_load_acquire(m_cring_head);

        // Remember, this is a ring buffer. If head == tail, it means that the
        // buffer is empty.
        if (head == *m_cring_tail)
            return -1;

        // Get the entry
        struct io_uring_cqe *cqe = &m_cqes[head & (*m_cring_mask)];
        int res = cqe->res;
        if (res < 0)
            cerr << "Error: " << strerror(abs(cqe->res)) << endl;

        head++;
        m_counter--;

        // Write barrier so that update to the head are made visible
        io_uring_smp_store_release(m_cring_head, head);

        if (res >= 0)
        {
            REQUEST *req = reinterpret_cast<REQUEST*>(cqe->user_data);
            req->process(res);
        }
    
        return res;
    }

    // Submit a read or a write request to the submission queue.

    int submit_to_sq(int fd, int op, off_t offset, char* buff, size_t buff_len, REQUEST *req)
    {
        // Add our submission queue entry to the tail of the SQE ring buffer
        uint32_t tail = *m_sring_tail;
        uint32_t index = tail & *m_sring_mask;

        struct io_uring_sqe *sqe = &m_sqes[index];

        // Fill in the parameters required for the read or write operation
        sqe->opcode = op;
        sqe->fd = fd;
        sqe->addr = (unsigned long) buff;
        sqe->user_data = (__u64) req;

        if (op == IORING_OP_READ)
        {
            memset(buff, 0, buff_len);
            sqe->len = buff_len;
        }
        else
        {
            sqe->len = buff_len;
        }
        sqe->off = offset;

        m_sring_array[index] = index;
        tail++;

        // Update the tail
        io_uring_smp_store_release(m_sring_tail, tail);

        // Tell the kernel we have submitted events with the io_uring_enter()
        // system call. We also pass in the IORING_ENTER_GETEVENTS flag which
        // causes the io_uring_enter() call to wait until min_complete
        // (the 3rd param) events complete.

        int ret =  io_uring_enter(m_ring_fd, 1, 1, IORING_ENTER_GETEVENTS);
        if (ret < 0)
        {
            perror("io_uring_enter");
            return -1;
        }

        m_counter++;
    
        return ret;
    }

    uint64_t count() const { return m_counter; }
};

#define BUFFER_SZ 1024

class client_request
{
private:
    enum STATE {READING_CLIENT_INPUT, WRITING_TO_FILE, COMPLETED, FAILED};
    STATE m_state = READING_CLIENT_INPUT;
    char m_buffer[BUFFER_SZ];
    off_t m_offset = 0;
    off_t m_output_offset = 0;
    int  m_input_fd = -1;
    int  m_output_fd = -1;
    uint32_t m_index = 0;
    io_uring_wrapper<client_request> *m_io_uring = nullptr;

public:
    client_request(int input_fd,
                   std::string_view output_path,
                   uint32_t index,
                   io_uring_wrapper<client_request> *iou,
                   int output_fd = -1, uint64_t output_offset = 0)
    : m_input_fd(dup(input_fd)), m_index(index), m_io_uring(iou), m_output_fd(output_fd), m_output_offset(output_offset)
    {
        if (m_output_fd == -1)
        {
            // build the full output filepath with index on the end
            // open the output fd
            std::string opath(output_path);
            opath += ".";
            opath += std::to_string(m_index);
            m_output_fd = ::open(opath.data(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (m_output_fd < 0)
            {
                cerr << "Failed to open output file, " << opath << ", " << strerror(errno) << endl;
            }
        }
    }

    void start_io_uring()
    {
        m_io_uring->submit_to_sq(m_input_fd, IORING_OP_READ, m_offset, m_buffer, BUFFER_SZ, this);
    }

    void process(int res)
    {
        cerr << __func__ << ", index: " << m_index << ", res: " << res << endl;
        if (res > 0)
        {
            switch (m_state) {
            case READING_CLIENT_INPUT:
                // Read successful. Write to stdout.
                m_io_uring->submit_to_sq(m_output_fd, IORING_OP_WRITE, m_output_offset + m_offset, m_buffer, res, this);
                m_state = WRITING_TO_FILE;
                m_offset += res;
                break;
            case WRITING_TO_FILE:
                m_io_uring->submit_to_sq(m_input_fd, IORING_OP_READ, m_offset, m_buffer, BUFFER_SZ, this);
                m_state = READING_CLIENT_INPUT;
                break;
            };
        }
        else if (res == 0)
        {
            // reached EOF
            m_state = COMPLETED;
            return;
        }
        else if (res < 0)
        {
            // Error reading file
            cerr << "Error: " << strerror(abs(res)) << endl;
            m_state = FAILED;
            return;
        }
    }

    char* buffer() { return m_buffer; }
};

int32_t main (int argc, char **argv)
{
    // process cmd line args
    std::string input;
    std::string output;
    uint32_t cnt = 1;
    bool spool_it = false;

    for (int i = 1; i < argc; i++)
    {
        std::string_view arg(argv[i]);
        auto[key, val] = split(arg, '=');
        if (key == "--input"sv)
        {
            input = val;
        }
        else if (key == "--output"sv)
        {
            output = val;
        }
        else if (key == "--cnt"sv)
        {
            cnt = aton(val);
        } 
        else if (key == "--spool"sv)
        {
            spool_it = true;
        }
    }

    int input_fd = ::open(input.data(), O_RDONLY);
    if (input_fd < 0)
    {
        cerr << "Failed to open " << input << ", " << strerror(errno) << endl;
        return 0;
    }

    io_uring_wrapper<client_request> iuw(cnt); // queue_depth of <cnt> 

    if (!iuw.is_setup())
    {
        return 0;
    }

    off_t offset = 0;

// currently the reads and writes are using m_buff, a single buffer
// will need multiple buffers, one per client request
// and maybe the use of iovec
// if we want simultaneous operations on the socket and file fd we'd need multiple buffers, maybe just a buffer pool at the app level

// TODO NEXT: simulate client requests, maybe duplicate a file n times "go <file copies> <input filepath> <output filepath>" each request appends it's index to the output filepath
    
    vector<client_request*> requests;

    for (uint32_t i = 0; i < cnt; i++)
    {
        requests.push_back(new client_request(input_fd, output, i, &iuw));
        requests.back()->start_io_uring();
    }

    // we now have <cnt> open fd's for input and <cnt> open fd's for output
    cerr << "iuw pending requests: " << iuw.count() << endl;

    while (iuw.count())
    {
        int res = iuw.read_from_cq();
    }

    cerr << "iuw pending requests: " << iuw.count() << endl;
}

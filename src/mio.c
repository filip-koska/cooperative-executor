#include "mio.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>

#include "debug.h"
#include "executor.h"
#include "waker.h"
#include "err.h"

// Maximum number of events to handle per epoll_wait call.
#define MAX_EVENTS 64

#define MAX_DESCRIPTORS 1048577

struct Mio {
    // TODO: add required fields
    struct epoll_event dummy;
    Executor *executor;
    int epfd;
    // Waker wakers[MAX_DESCRIPTORS];
    // struct epoll_event fd_data[MAX_DESCRIPTORS];
    struct epoll_event events[MAX_EVENTS];
    int n_descriptors;
    // Waker to_wake[MAX_EVENTS];
    // int errcodes[MAX_EVENTS];
};

// TODO: delete this once not needed.
// #define UNIMPLEMENTED (exit(42))

Mio* mio_create(Executor* executor) {
    Mio *ret = (Mio*)malloc(sizeof(Mio));
    if (!ret)
        fatal("Allocation failed\n");
    ret->executor = executor;
    ret->epfd = epoll_create(MAX_DESCRIPTORS);
    ASSERT_SYS_OK(ret->epfd);
    ret->n_descriptors = 0;
    return ret;
}

void mio_destroy(Mio* mio) {
    close(mio->epfd);
    free(mio);
}

int mio_register(Mio* mio, int fd, uint32_t events, Waker waker)
{
    debug("Registering (in Mio = %p) fd = %d with\n", mio, fd);

    ++mio->n_descriptors;
    // mio->fd_data[fd].events = events;
    // mio->fd_data[fd].data.fd = fd;
    // mio->wakers[fd] = waker;
    // int create_res = epoll_ctl(mio->epfd, EPOLL_CTL_ADD, fd, &mio->fd_data[fd]);
    // if (create_res == -1 && errno == EEXIST) { // attempt to change events associated with descriptor
    //     --mio->n_descriptors;
    //     create_res = epoll_ctl(mio->epfd, EPOLL_CTL_MOD, fd, &mio->fd_data[fd]);
    // }
    struct epoll_event ee;
    ee.events = events;
    ee.data.ptr = (void*)waker.future;
    int create_res = epoll_ctl(mio->epfd, EPOLL_CTL_ADD, fd, &ee);
    if (create_res == -1 && errno == EEXIST) { // attempt to change events associated with descriptor
        --mio->n_descriptors;
        create_res = epoll_ctl(mio->epfd, EPOLL_CTL_MOD, fd, &ee);
    }
    return create_res;
}

int mio_unregister(Mio* mio, int fd)
{
    debug("Unregistering (from Mio = %p) fd = %d\n", mio, fd);

    int ret = epoll_ctl(mio->epfd, EPOLL_CTL_DEL, fd, &mio->dummy);
    if (ret == 0)
        --mio->n_descriptors;
    return ret;
}

void mio_poll(Mio* mio)
{
    debug("Mio (%p) polling\n", mio);

    if (mio->n_descriptors == 0)
        return;

    int n_ready = epoll_wait(mio->epfd, mio->events, MAX_EVENTS, -1);

    // for (int i = 0; i < n_ready; ++i) {
    //     waker_wake(&mio->wakers[mio->events[i].data.fd]);
    // }
    Waker waker;
    waker.executor = (void*)mio->executor;
    for (int i = 0; i < n_ready; ++i) {
        waker.future = (Future*)mio->events[i].data.ptr;
        waker_wake(&waker);
    }
}

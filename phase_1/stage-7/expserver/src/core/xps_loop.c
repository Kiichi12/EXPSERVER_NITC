#include "xps_loop.h"

/**
 * Creates a new event loop instance associated with the given core.
 *
 * This function creates an epoll file descriptor, allocates memory for the xps_loop instance,
 * and initializes its values.
*
 * @param core : The core instance to which the loop belongs
 * @return A pointer to the newly created loop instance, or NULL on failure.
 */
xps_loop_t *xps_loop_create(xps_core_t *core) {
    assert(core != NULL);
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        printf("[ERROR] Failed to create epoll file descriptor\n");
        return NULL;
    }
    xps_loop_t *loop = malloc(sizeof(xps_loop_t));
    loop->epoll_fd = epoll_fd;
    loop->core = core;
    loop->n_null_events = 0;

    vec_init(&loop->events);
    // loop->epoll_events = malloc(MAX_EPOLL_EVENTS * sizeof(struct epoll_event));
    // if (loop->epoll_events == NULL) {
    //     free(loop);
    //     close(epoll_fd);
    //     return NULL;
    // }

    logger(LOG_INFO, "xps_loop_create()", "epoll created, epoll fd: %d", epoll_fd);
    return loop;
}

/**
 * Destroys the given loop instance and releases associated resources.
 *
 * This function destroys all loop_event_t instances present in loop->events list,
 * closes the epoll file descriptor and releases memory allocated for the loop instance,
 *
 * @param loop The loop instance to be destroyed.
 */
void xps_loop_destroy(xps_loop_t *loop) {
    assert(loop != NULL);
    logger(LOG_DEBUG, "xps_loop_destroy()", "destroying loop: %p, core: %p, epoll_fd: %d", loop, loop->core, loop->epoll_fd);
    for (int i = 0; i < loop->events.length; i++) {
        loop_event_t *event = loop->events.data[i];

        if (event != NULL){
            logger(LOG_DEBUG, "xps_loop_destroy()", "freeing event: %p", event);
            free(event);
        }
    }

    vec_deinit(&loop->events);
    logger(LOG_DEBUG, "xps_loop_destroy()", "events list deinitialized");

    // free(loop->epoll_events);

    close(loop->epoll_fd);
    free(loop);
    logger(LOG_DEBUG, "xps_loop_destroy()", "epoll_fd closed and loop freed");

    logger(LOG_INFO, "xps_loop_destroy()", "loop destroyed successfully");
}

/**
 * Attaches a FD to be monitored using epoll
 *
 * The function creates an instance of loop_event_t and attaches it to epoll.
 * Add the pointer to loop_event_t to the events list in loop
 *
 * @param loop : loop to which FD should be attached
 * @param fd : FD to be attached to epoll
 * @param event_flags : epoll event flags
 * @param ptr : Pointer to instance of xps_listener_t or xps_connection_t
 * @param read_cb : Callback function to be called on a read event
 * @return : OK on success and E_FAIL on error
 */
int xps_loop_attach(xps_loop_t *loop, u_int fd, int event_flags, void *ptr, xps_handler_t read_cb) {
    assert(loop != NULL);
    assert(ptr != NULL);
    logger(LOG_DEBUG, "xps_loop_attach()", "Attaching FD %d to loop %p", fd, loop);
    
    // Check if fd is valid
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        logger(LOG_ERROR, "xps_loop_attach()", "Invalid file descriptor %d: %s", 
               fd, strerror(errno));
        return E_FAIL;
    }

    loop_event_t *event = malloc(sizeof(loop_event_t));
    if (event == NULL) {
        logger(LOG_ERROR, "xps_loop_attach()", "Failed to allocate memory for loop_event_t");
        return E_FAIL;
    }
    event->fd = fd;
    event->ptr = ptr;
    event->read_cb = read_cb;
    
    struct epoll_event epoll_event;
    epoll_event.events = event_flags;
    epoll_event.data.ptr = event;

    logger(LOG_DEBUG, "xps_loop_attach()", "Adding event to epoll");
 
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &epoll_event) < 0) {
        logger(LOG_ERROR, "xps_loop_attach()", "epoll_ctl ADD failed");
        free(event);
        return E_FAIL;
    }

    vec_push(&loop->events, event); 

    logger(LOG_INFO, "xps_loop_attach()", "Event added to epoll successfully");

    return OK;
}

/**
 * Remove FD from epoll
 *
 * Find the instance of loop_event_t from loop->events that matches fd param
 * and detach FD from epoll. Destroy the loop_event_t instance and set the pointer
 * to NULL in loop->events list. Increment loop->n_null_events.
 *
 * @param loop : loop instnace from which to detach fd
 * @param fd : FD to be detached
 * @return : OK on success and E_FAIL on error
 */
int xps_loop_detach(xps_loop_t *loop, u_int fd) {
    assert(loop != NULL);
    logger(LOG_DEBUG, "xps_loop_detach()", "Detaching FD %d", fd);
    for(int i = 0; i < (loop->events).length; i++)
    {
        loop_event_t *event = loop->events.data[i];
        
        if (event != NULL && event->fd == fd) {
            // Detach from epoll
            if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
                logger(LOG_ERROR, "xps_loop_detach()", "epoll_ctl DEL failed");
                return E_FAIL;
            }
            // Free the event
            free(event);

            logger(LOG_DEBUG, "xps_loop_detach()", "FD %d detached from epoll and memory for event freed", fd);
            
            // Set pointer to NULL and increment null count
            loop->events.data[i] = NULL;
            loop->n_null_events++;
            logger(LOG_DEBUG, "xps_loop_detach()", "Event pointer set to NULL and null count incremented: %d", loop->n_null_events);
            
            break;
        }
    }
    logger(LOG_INFO, "xps_loop_detach()", "FD %d detached from epoll successfully", fd);

    return OK;

}



void xps_loop_run(xps_loop_t *loop) {
    /* Validate params */
    assert(loop!=NULL);

    while (1) {
        logger(LOG_DEBUG, "xps_loop_run()", "epoll wait");
        int n_events = epoll_wait(loop->epoll_fd, loop->epoll_events, MAX_EPOLL_EVENTS, -1);
        logger(LOG_DEBUG, "xps_loop_run()", "epoll wait over");

        logger(LOG_DEBUG, "xps_loop_run()", "handling %d events", n_events);

        if (n_events < 0) {
            logger(LOG_ERROR, "xps_loop_run()", "epoll_wait failed: %s (errno: %d)", 
                   strerror(errno), errno);
            
            // If interrupted by signal, continue
            if (errno == EINTR) {
                logger(LOG_DEBUG, "xps_loop_run()", "Interrupted by signal, continuing");
                continue;
            }
            
            // For other errors, we might need to break or handle differently
            // If epoll_fd is invalid, we can't recover
            if (errno == EBADF) {
                logger(LOG_ERROR, "xps_loop_run()", "Invalid epoll file descriptor, exiting");
                break;
            }
            
            // For other errors, sleep briefly to avoid busy loop
            usleep(1000); // 1ms
            continue;
        }

        // Handle events
        for (int i = 0; i < n_events; i++) {
            logger(LOG_DEBUG, "xps_loop_run()", "handling event no. %d", i + 1);

            struct epoll_event curr_epoll_event = loop->epoll_events[i];
            loop_event_t *curr_event = curr_epoll_event.data.ptr;

            // Check if event still exists. Could have been destroyed due to prev event
            assert(curr_event != NULL);


            int curr_event_idx;
            vec_find(&loop->events, curr_event, curr_event_idx);
            // for(int i = 0; i < (loop->events).length; i++)    /* search through loop->events and get index of curr_event, set it to -1 if not found */
            // {
            // if (loop->events.data[i] == curr_event) {
            //     curr_event_idx = i;
            //     break;
            // }
            // }

            // 🟡 Above can be optimized using an RB tree
            if (curr_event_idx == -1) {
                logger(LOG_DEBUG, "handle_epoll_events()", "event not found. skipping");
                continue;
            }

            // Read event
            if (curr_epoll_event.events & EPOLLIN) {
                logger(LOG_DEBUG, "handle_epoll_events()", "EVENT / read");
                if (curr_event->read_cb != NULL)
                    // Pass the ptr from loop_event_t as a parameter to the callback
                    curr_event->read_cb(curr_event->ptr);
            }
        }
    }
}
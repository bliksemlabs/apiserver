/* Copyright 2013 Bliksem Labs. See the LICENSE file at the top-level directory of this distribution and at https://github.com/bliksemlabs/rrrr/. */

/* apiserver.c */

/*
  A single-purpose "HTTP server" library for building minimal web services.
  Adapted from code originally developed for the RRRR OTP compatibility layer.
  It ignores everything but lines matching the pattern 'GET *?querystring'.

  All incoming IO is multiplexed via a portable polling mechanism (POSIX poll).
  Once a full request has been received, it is passed off to a waiting handler thread.
  The handler thread is given a socket descriptor and can stream results back as it pleases.
*/

// $ time for i in {1..2000}; do curl localhost:9393/plan?0; done

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <pwd.h>

// HTTP requires CR-LF style newlines. Headers are followed by two newlines.
#define CRLF          "\r\n"
#define HEADERS       CRLF
#define END_HEADERS   CRLF CRLF
#define TEXT_PLAIN    "Content-Type:text/plain"
#define APPLICATION_JSON    "Content-Type:application/json"
#define ALLOW_ORIGIN    "Access-Control-Allow-Origin:*"
#define ALLOW_HEADERS    "Access-Control-Allow-Headers:Requested-With,Content-Type"
#define OK_TEXT_PLAIN "HTTP/1.0 200 OK" HEADERS APPLICATION_JSON CRLF ALLOW_ORIGIN CRLF ALLOW_HEADERS CRLF
#define ERROR_404     "HTTP/1.0 404 Not Found" HEADERS "Content-Length: 16" CRLF "Connection: close" CRLF TEXT_PLAIN END_HEADERS "FOUR ZERO FOUR" CRLF

#define BUFLEN     1024 // buffer length for each open connection
#define PORT       9393 // port on which to listen for new connections
#define QUEUE_CONN  500 // connection queue length to _request_ from the OS
#define MAX_CONN    100 // maximum number of simultaneous incoming HTTP connections
#define N_THREADS     4 // number of request handling threads to spawn

void die(const char *msg) {
    fprintf (stderr, "%s\n", msg);
    //syslog (LOG_ERR, "%s\n", msg);
    exit (EXIT_FAILURE);
}

/* Buffers used to assemble and parse incoming HTTP requests. One per open connection. */
struct buffer {
    char  *buf;  // A pointer to the buffer, to allow efficient swapping
    int    size; // Number of bytes used in the buffer
//    int    sd;   // Socket descriptor for this connection (is this redundant since it's in the pollfds?)
//    time_t time; // Time at which last activity occurred, for timeouts
};

/* Poll items. One listens for incoming connections, the rest are HTTP client communication sockets. */
struct pollfd poll_items [1 + MAX_CONN];

/* Open HTTP connections. */
struct pollfd *conn_items;        // Simply the tail of the poll_items array, without the first item.
uint32_t       n_conn;            // Number of open connections.
struct buffer  buffers[MAX_CONN]; // We can swap these structs directly, including the char pointers they contain.

// A queue of connections to be removed at the end of the current polling iteration.
// FIXME how does this handle removing multiple connections with positional arguments?
uint32_t conn_remove_queue[MAX_CONN];
uint32_t conn_remove_n = 0;

/*
  Schedule a connection for removal from the poll_items / open connections. It will be removed at 
  the end of the current polling iteration to avoid reordering other poll_items in the middle of an 
  iteration.
  Note that scheduling the same connection for removal more than once will have unpredictable effects.
  Returns the number of connections enqueued for removal.
*/
static uint32_t remove_conn_later (uint32_t nc) {
    printf ("connection %02d [fd=%02d] enqueued for removal.\n", nc, conn_items[nc].fd);
    conn_remove_queue[conn_remove_n] = nc;
    conn_remove_n += 1;
    return conn_remove_n;
}

/* Debug function: print out all open connections. */
static void conn_dump_all () {
    printf ("number of active connections is %d\n", n_conn);
    for (int i = 0; i < n_conn; ++i) {
        struct pollfd *pi = poll_items + 1 + i;
        printf ("[%02d] fd=%02d buf='%s'\n", i, pi->fd, buffers[i].buf);
    }
}

/* Add a connection with socket descriptor sd to the end of the list of open connections. */
static void add_conn (uint32_t sd) {
    if (n_conn < MAX_CONN) {
        conn_items[n_conn].fd = sd;
        conn_items[n_conn].events = POLLIN;
        printf ("connection %02d [fd=%02d] has been added.\n", n_conn, sd);
        n_conn++;
        conn_dump_all ();
    } else {
        // This should not happen since we suspend listen socket polling when the connection limit is reached.
        printf ("Accepted too many incoming connections, dropping one on the floor. \n");
    }
}

/*
  Remove the HTTP connection with index nc from the list of open connections.
  The last open connection in the list is swapped into the hole created.
  Returns true if the poll item was removed, false if the operation failed.
  If several connections are to be removed in succession, they must be removed in reverse order.
  Otherwise higher-numbered connections may move out of position before they can be removed.
*/
static bool remove_conn (uint32_t nc) {
    if (nc >= n_conn) return false; // trying to remove an inactive connection
    uint32_t last_index = n_conn - 1;
    struct pollfd *item = conn_items + nc;
    struct pollfd *last = conn_items + last_index;
    printf ("connection %02d [fd=%02d] being removed.\n", nc, item->fd);
    memcpy (item, last, sizeof(*item));
    /* Swap in the buffer struct for the last active connection (retain char *buf). */
    struct buffer temp;
    temp = buffers[nc];
    buffers[nc] = buffers[last_index];
    buffers[last_index] = temp;
    buffers[last_index].size = 0;
    n_conn--;
    conn_dump_all ();
    return true;
}

// OS X doesn't have MSG_NOSIGNAL
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#define setsockopt_no_sigpipe(conn_sd) setsockopt(conn_sd, SOL_SOCKET, SO_NOSIGPIPE, &(int){1}, sizeof(int));
#else
#define setsockopt_no_sigpipe(conn_sd)
#endif

#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK O_NONBLOCK
#endif

// Removing a connection from pollitems and closing its SD are separate, because we have no connection number in the ZMQ response. Connection numbers are always changing.

/* 
  Remove all connections that have been enqueued for removal in a single operation.
  They must be removed in reverse order in case multiple connections are removed in a single 
  iteration. We don't want to touch any higher-numbered connection that might have been moved in
  the same iteration. This function assumes that they were added to the queue in increasing order.
  TODO or should we just continue to the next poll loop iteration after removing any connection?
*/
static void remove_conn_enqueued () {
    while (conn_remove_n > 0) {
        conn_remove_n -= 1;
        remove_conn (conn_remove_queue[conn_remove_n]);
    }
}

/*
  Read input from the socket associated with connection index nc into the corresponding buffer.
  Implementation note: POLLIN tells us that "data is available", which actually means 
  "you can call read on this socket without blocking".
  If read/recv then returns 0 bytes, that indicates that the socket has been closed.
  Return true if the buffer contains a single complete line of the request, false otherwise.
*/
static bool read_input (uint32_t nc) {
    struct buffer *b = &(buffers[nc]);
    int conn_sd = conn_items[nc].fd;
    char *c = b->buf + b->size; // pointer to the first available character in the buffer
    int remaining = BUFLEN - b->size;
    size_t received = recv (conn_sd, c, remaining, 0);
    printf ("connection %02d [fd=%02d] recevied %zd bytes.\n", nc, conn_sd, received);
    // If recv returns zero, that means the connection has been closed.
    // Don't remove it immediately, since we are in the middle of a poll loop.
    if (received == 0) {
        printf ("connection %02d [fd=%02d] closed. closing socket descriptor locally.\n", nc, conn_sd);
        remove_conn_later (nc);
        close (conn_sd); // client actively closed. necessary to close SD on server side as well.
        return false;
    }
    b->size += received;
    // TODO use 'remaining' 
    if (b->size >= BUFLEN) {
        printf ("HTTP request does not fit in buffer.\n");
        return false;
    }
    //printf ("received: %s \n", c);
    //printf ("buffer is now: %s \n", b->buf);
    bool eol = false;
    for (char *end = c + received; c <= end; ++c) {
        if (*c == '\n' || *c == '\r') {
            *c = '\0';
            eol = true;
            break;
        }
    }
    return eol;
}

static void send_request (int nc, void *broker_socket) {
    struct buffer *b = &(buffers[nc]);
    uint32_t conn_sd = conn_items[nc].fd;
    char *token = strtok (b->buf, " ");
    if (token == NULL) {
        printf ("request contained no verb \n");
        goto cleanup;
    }
    if (strcmp(token, "GET") != 0) {
        printf ("request was %s not GET \n", token);
        goto cleanup;
    }
    char *resource = strtok (NULL, " ");
    if (resource == NULL) {
        printf ("request contained no filename \n");
        goto cleanup;
    }
    char *qstring = index (resource, '?');
    if (qstring == NULL || qstring[1] == '\0') {
        printf ("request contained no query string \n");
        goto cleanup;
    }
    qstring += 1; // skip question mark

    // TODO parse query string and pass it off to one handler thread.

    printf ("connection %02d [fd=%02d] sent request to broker.\n", nc, conn_sd);
    // TODO: Do not remove_conn_later here. Continue polling so we detect client closing,
    // avoiding TIME_WAIT on server side and supporting persistent connections.
    remove_conn_later (nc); // remove from poll list. stops accepting input from this connection.
    return;

    cleanup:
    setsockopt_no_sigpipe(conn_sd);
    send (conn_sd, ERROR_404, sizeof(ERROR_404) - 1, MSG_NOSIGNAL);
    remove_conn_later (nc); // could this lead to double-remove?
    return;
}

/*
  This can be called by the client of the apiserver library to provide a simple fixed-length response.
*/
void respond (int sd, char *response) {
    char buf[512];
    sprintf (buf, OK_TEXT_PLAIN "Content-Length: %zu" CRLF "Connection: close" END_HEADERS, strlen (response));
    // MSG_NOSIGNAL: Do not generate SIGPIPE if client has closed connection.
    // Send will return EPIPE if client already closed connection.
    setsockopt_no_sigpipe(sd);
    send (sd, buf, strlen(buf), MSG_NOSIGNAL);
    if (send (sd, response, strlen(response), MSG_NOSIGNAL) == -1 && errno == EPIPE)
        printf ("              [fd=%02d] socket is closed, response dropped.\n", sd);
    else
        printf ("              [fd=%02d] sent response to client.\n", sd);
}

typedef struct {
    char *buffer; // storage for lines of incoming request
    int sd; // socket descriptor
} Connection;

typedef struct request_s {
    int argc;
    char **argv;
    char *buf;
    struct request_s *next;
} Request;

Request *request_queue_head = NULL;
Request *request_queue_tail = NULL;

static bool request_queue_available () {
    return request_queue_head != NULL;
}

static void request_queue_enqueue (Request *req) {
    req->next = NULL;
    if (request_queue_head == NULL) {
        request_queue_head = req;
        request_queue_tail = req;
    } else {
        request_queue_tail->next = req;
    }
}

static Request *request_queue_dequeue () {
    if (request_queue_head == NULL) {
        return NULL;
    }
    Request *ret = request_queue_head;
    request_queue_head = request_queue_head->next;
    /* Only head is used to signal an empty queue. Don't worry about the tail. */
    return ret;
}

/* This condition variable is used to signal a thread when a complete request is available. */
pthread_cond_t request_avail_cond = PTHREAD_COND_INITIALIZER;

/* This mutex ensures that only one thread at a time is waiting to grab a complete request. */
pthread_mutex_t request_avail_lock = PTHREAD_MUTEX_INITIALIZER;

void *request_handling_callback (Request *req);
void *request_handling_callback (Request *req) {
    printf ("Handling a request. It contains: %s\n", req->buf);
    free(req);
    return NULL;
}

/*
  This is the function executed by the request handler threads.
  It continually tries to pull requests off a queue and hand them off to a callback function.
*/
void *thread_spin (void *params) {
    while (true) {
        /* Attempt to grab the lock. This will happen when the thread is awakened. */
        int rc = pthread_mutex_lock (&request_avail_lock);
        if (rc != 0) die ("fail on mutex lock");
        /*
          Spurious wakeups are possible. 
          Returning from wait does not imply anything about the value of the Boolean expression.
          The waiting thread must check whether it should return to waiting or act on the condition.
          Mutexes are intended to be locked for only a few instructions. The wait call releases the
          associated mutex atomically when it begins waiting, and re-acquires it atomically when it returns.
        */
        while (!request_queue_available()) {
            rc = pthread_cond_wait (&request_avail_cond, &request_avail_lock);
        }
        /* While holding the lock, consume one waiting request. */
        Request *req = request_queue_dequeue ();
        printf ("I got one request. It contains: %s\n", req->buf);
        /* Now unlock, giving all threads the possibility to do more work when awakened. */
        rc = pthread_mutex_unlock (&request_avail_lock);
        /* Handle the request while no longer holding the lock. */
        request_handling_callback (req);
    }
    return NULL;
}

/* 
  Main function: 
  Listen for connections on a socket. Within a single thread, multiplex any input from all connected 
  clients. Once a complete request has been assembled, place the request on a queue and wake up a
  handler thread to actually compute the response. The handler thread is handed a socket descriptor
  and can write whatever it wants as a response.
*/
int main (int argc, char **argv) {

    /* Set up TCP/IP stream socket to listen for incoming HTTP requests. */
    struct sockaddr_in server_in_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    /* Make listening socket is nonblocking: connections or bytes may not be waiting. */
    uint32_t server_socket = socket (AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    
    /* Bind even when still in the TIME_WAIT state due to a recently closed socket. */
    int one = 1; 
    setsockopt (server_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    socklen_t in_addr_length = sizeof (server_in_addr);
    if (bind(server_socket, (struct sockaddr *) &server_in_addr, sizeof(server_in_addr)))
        die ("Failed to bind socket.\n");

    /* Drop root privileges if we are running as root. */
    if (getuid() == 0  || geteuid() == 0) {
        struct passwd *pw;
        uid_t puid = 65534; /* just use the traditional value */
        gid_t pgid = 65534;
        if ((pw = getpwnam("nobody"))) {
            puid = pw->pw_uid;
            pgid = pw->pw_gid;
        }
        /* Now we chroot to this directory, preventing any write access outside it */
        chroot("/var/empty");
        /* After we bind to the socket and chrooted, we drop privileges to nobody */
        setuid(puid);
        setgid(pgid);
    }

    /* Liten for incoming connections. */
    listen(server_socket, QUEUE_CONN);

    /* Set up the poll_items for the main polling loop. */
    struct pollfd *http_item = &(poll_items[0]);

    /* First poll item is a standard socket for incoming HTTP requests. */
    http_item->fd = server_socket;

    /* The remaining poll_items are incoming HTTP connections. */
    conn_items = &(poll_items[1]);
    n_conn = 0;

    /* Allocate buffers for incoming HTTP requests. */
    for (int i = 0; i < MAX_CONN; ++i) buffers[i].buf = malloc (BUFLEN); // could be statically allocated or done on demand.
    
    /* Spawn request handler threads. */
    pthread_t threads[N_THREADS];
    for (int t = 0; t < N_THREADS; t++) {
        if (pthread_create(&(threads[t]), NULL, &thread_spin, NULL)) {
            die ("Could not create thread\n");
        }
    }
    
    /* Main event loop that multiplexes IO. */
    for (;;) {
        /* Suspend polling (ignore enqueued incoming HTTP connections) when we already have too many. */
        http_item->events = n_conn < MAX_CONN ? POLLIN : 0;
        /* Blocking poll for queued incoming TCP connections or traffic on open TCP connections. */
        int n_waiting = poll (poll_items, 1 + n_conn, -1);
        if (n_waiting < 1) {
            printf ("Poll call interrupted.\n");
            break; // Really, we should stop accepting incoming connections and break only when all connections closed.
        }
        /* Check if the listening TCP/IP socket has a queued connection. */
        if (http_item->revents & POLLIN) {
            struct sockaddr_in client_in_addr;
            socklen_t in_addr_length;
            // Adding a connection will increase the total connection count, but in the loop over 
            // open connections n_waiting should hit zero before the new one is encountered.
            // Checking open connections before adding the new one would be less efficient since 
            // each incoming connection would trigger an iteration through the whole list of 
            // (possibly inactive) existing connections.
            // Will these client sockets necessarily be nonblocking because the listening socket is?
            int client_socket = accept (server_socket, (struct sockaddr *) &client_in_addr, &in_addr_length);
            if (client_socket < 0) printf ("Error on TCP socket accept.\n");
            else add_conn (client_socket);
            n_waiting--;
        }
        /* Read from any open HTTP connections that have available input. */
        for (uint32_t c = 0; c < n_conn && n_waiting > 0; ++c) {
            if (conn_items[c].revents & POLLIN) {
                bool eol = read_input (c);
                conn_dump_all ();
                n_waiting--;
                if (eol) {
                    // Buffer contains at least one full line.
                    // Parse and enqueue a request, then wake a handler thread.
                    write (conn_items[c].fd, buffers[c].buf, buffers[c].size); 
                    write (conn_items[c].fd, "\n", 0); 
                    Request *req = malloc(sizeof(Request)); // TODO these should be statically allocated, one per potential connection
                    req->buf = buffers[c].buf;
                    request_queue_enqueue(req);
                    pthread_cond_signal(&request_avail_cond);
                }
            }
        }
        /* Remove all connections found to be closed during this poll iteration. */
        remove_conn_enqueued ();
    }
    close (server_socket);
    return (0);
}



#include <ccan/io/io.h>
#include <ccan/err/err.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define SOCKET_PATH "/tmp/strfry.sock"
#define BUFFER_SIZE 4096

struct buffer {
    bool finished;
    size_t start, end, rlen, wlen;
    char buf[BUFFER_SIZE];
};

/* Called when finished reading */
static void finish(struct io_conn *c, struct buffer *b) {
    b->finished = true;
    io_wake(b); // Wake up waiting writers
}

/* Reads input from the client */
static struct io_plan *read_in(struct io_conn *c, struct buffer *b) {
    b->end += b->rlen;

    if (b->rlen != 0)
        io_wake(b);

    if (b->start == b->end)
        b->start = b->end = 0;

    if (b->end == sizeof(b->buf))
        return io_wait(c, b, read_in, b);

    return io_read_partial(c, b->buf + b->end, sizeof(b->buf) - b->end,
                           &b->rlen, read_in, b);
}

/* Writes output to the client */
static struct io_plan *write_out(struct io_conn *c, struct buffer *b) {
    b->start += b->wlen;
    b->wlen = 0;

    if (b->wlen != 0)
        io_wake(b);

    if (b->end == b->start) {
        if (b->finished)
            return io_close(c);
        return io_wait(c, b, write_out, b);
    }

    return io_write_partial(c, b->buf + b->start, b->end - b->start,
                            &b->wlen, write_out, b);
}

/* Reads output from the child process and sends it to the client */
static struct io_plan *read_from_child(struct io_conn *conn, struct buffer *child_buf) {
    return io_read_partial(conn, child_buf->buf + child_buf->end,
                           sizeof(child_buf->buf) - child_buf->end,
                           &child_buf->rlen, write_out, child_buf);
}

/* Processes the query received from the client */
static void process_query(const char *query, int tochild[2], int fromchild[2]) {
    if (!fork()) {
        // Child process
        close(tochild[1]);      // Close unused write end
        close(fromchild[0]);    // Close unused read end

        dup2(tochild[0], STDIN_FILENO);   // Child reads from the pipe
        dup2(fromchild[1], STDOUT_FILENO); // Child writes to the pipe

        // Execute the strfry command
        execl("/bin/sh", "sh", "-c", query, (char *)NULL);
        err(1, "execl");
    }

    // Parent closes unused ends
    close(tochild[0]);
    close(fromchild[1]);
}

static struct io_plan *new_connection(struct io_conn *conn, void *arg) {
    int *pipes = (int *)arg; // pipes[0]: tochild, pipes[1]: fromchild
    struct buffer *child_buf = tal(conn, struct buffer);
    struct buffer *client_buf = tal(conn, struct buffer);

    memset(child_buf, 0, sizeof(*child_buf));
    memset(client_buf, 0, sizeof(*client_buf));

    // Read from client and send to child
    io_new_conn(NULL, pipes[0], read_in, client_buf);

    // Read from child and send to client
    return io_duplex(conn,
                     io_read_partial(conn, client_buf->buf, sizeof(client_buf->buf),
                                     &client_buf->rlen, read_in, client_buf),
                     io_write_partial(conn, child_buf->buf, sizeof(child_buf->buf),
                                      &child_buf->wlen, write_out, child_buf));
}

int main(void) {
    int fd, tochild[2], fromchild[2];
    struct sockaddr_un addr;

    // Create socket
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        err(1, "socket");

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCKET_PATH);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        err(1, "bind");
    if (listen(fd, 5) < 0)
        err(1, "listen");

    printf("[INFO] Listening on %s\n", SOCKET_PATH);

    // Create pipes for communication with the strfry process
    if (pipe(tochild) < 0 || pipe(fromchild) < 0)
        err(1, "pipe");

    // Accept new connections
    int pipes[] = {tochild[1], fromchild[0]};
    io_new_listener(NULL, fd, new_connection, pipes);
    io_loop(NULL, NULL);

    close(fd);
    unlink(SOCKET_PATH);

    return 0;
}

#include <ccan/io/io.h>
#include <ccan/tal/tal.h>
#include <ccan/tal/str/str.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOCKET_PATH "/tmp/strfry.sock"
#define BUFFER_SIZE 4096

/* Represents a client connection */
struct client {
    int pipe_fd;               // File descriptor for subprocess
    char *buffer;              // Buffer for reading/writing
    size_t used;               // Number of bytes used in the buffer
    size_t write_offset;       // Offset for writing data to the client
    bool finished;              // Whether the command execution is finished
    enum { READING, PROCESSING, WRITING } state; // Connection state
};

const char *state_to_string(int state) {
    switch (state) {
    case READING:
        return "READING";
    case PROCESSING:
        return "PROCESSING";
    case WRITING:
        return "WRITING";
    default:
        return "UNKNOWN";
    }
}

/* Function prototypes */
static struct io_plan *read_query(struct io_conn *conn, struct client *c);
static struct io_plan *execute_command(struct io_conn *conn, struct client *c);
static struct io_plan *read_results(struct io_conn *conn, struct client *c);
static struct io_plan *write_results(struct io_conn *conn, struct client *c);
static void close_client(struct io_conn *conn, struct client *c);

/* Executes the `strfry scan` command in a subprocess */
static int run_command(const char *command) {
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        perror("pipe");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    } else if (pid == 0) {
        close(pipe_fds[0]);  // Close read end in child
        dup2(pipe_fds[1], STDOUT_FILENO);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        perror("execl");
        exit(1);
    }
    close(pipe_fds[1]);  // Close write end in parent
    return pipe_fds[0];
}

/* Reads the query from the client */
static struct io_plan *read_query(struct io_conn *conn, struct client *c) {
    fprintf(stderr, "[DEBUG] State: %s\n", state_to_string(c->state));
    if (c->state != READING) {
        fprintf(stderr, "[ERROR] Invalid state transition: %s\n", state_to_string(c->state));
        return io_close(conn);  // Close the connection if state is invalid
    }
    c->state = PROCESSING;  // Update state to processing
    return io_read_partial(conn, c->buffer + c->used, BUFFER_SIZE - c->used,
                           &c->used, execute_command, c);
}

/* Executes the command and prepares to read results */
static struct io_plan *execute_command(struct io_conn *conn, struct client *c) {
    fprintf(stderr, "[DEBUG] Executing command: %s\n", c->buffer);
    c->pipe_fd = run_command(c->buffer);
    if (c->pipe_fd < 0) {
        fprintf(stderr, "[ERROR] Failed to execute command\n");
        return io_close(conn);
    }
    c->state = WRITING;  // Update state to writing results
    io_new_conn(NULL, c->pipe_fd, read_results, c);  // Handle subprocess output
    return io_wait(conn, c, read_query, c);          // Wait for more input
}

/* Reads results from the subprocess */
static struct io_plan *read_results(struct io_conn *conn, struct client *c) {
    ssize_t n = read(c->pipe_fd, c->buffer, BUFFER_SIZE);
    if (n < 0) {
        perror("[ERROR] Failed to read from subprocess");
        return io_close(conn);
    } else if (n == 0) {
        c->finished = true;  // Subprocess is done
        close(c->pipe_fd);
        return write_results(conn, c);
    }

    c->used = n;
    c->write_offset = 0;
    return write_results(conn, c);
}

/* Writes results back to the client */
static struct io_plan *write_results(struct io_conn *conn, struct client *c) {
    if (c->write_offset < c->used) {
        return io_write_partial(conn, c->buffer + c->write_offset,
                                c->used - c->write_offset, &c->write_offset,
                                write_results, c);
    }

    // If the subprocess is finished, close the connection
    if (c->finished) {
        printf("[INFO] Finished sending results to client\n");
        return io_close(conn);
    }

    // Wait for more results from the subprocess
    return io_wait(conn, c, read_results, c);
}

/* Cleans up a client connection */
static void close_client(struct io_conn *conn, struct client *c) {
    if (c->pipe_fd >= 0)
        close(c->pipe_fd);
    tal_free(c);
}

/* Handles a new client connection */
static struct io_plan *new_client(struct io_conn *conn, void *arg) {
    struct client *c = tal(conn, struct client);
    c->buffer = tal_arr(c, char, BUFFER_SIZE);
    c->used = 0;
    c->pipe_fd = -1;
    c->state = READING;  // Initial state for the connection
    return io_duplex(conn,
                     read_query(conn, c),  // Start reading from the client
                     io_never(conn, c));   // No writes initially
}

/* Main server setup */
int main(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
    };
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCKET_PATH);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(fd, 5) < 0) {
        perror("listen");
        return 1;
    }

    printf("[INFO] Listening on %s\n", SOCKET_PATH);

    io_new_listener(NULL, fd, new_client, NULL);
    io_loop(NULL, NULL);

    close(fd);
    return 0;
}

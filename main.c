// Given "tr A-Z a-z" outputs tr a-z a-z
#include <ccan/io/io.h>
#include <ccan/err/err.h>
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>

struct buffer {
     bool finished;
     size_t start, end, rlen, wlen;
     char buf[4096];
};

static void finish(struct io_conn *c, struct buffer *b)
{
     // Mark us finished.
     b->finished = true;
     // Wake writer just in case it's asleep.
     io_wake(b);
}

static struct io_plan *read_in(struct io_conn *c, struct buffer *b)
{
     // Add what we just read.
     b->end += b->rlen;
     assert(b->end <= sizeof(b->buf));

     // If we just read something, wake writer.
     if (b->rlen != 0)
             io_wake(b);

     // If buffer is empty, return to start.
     if (b->start == b->end)
             b->start = b->end = 0;

     // No room?  Wait for writer
     if (b->end == sizeof(b->buf))
             return io_wait(c, b, read_in, b);

     return io_read_partial(c, b->buf + b->end, sizeof(b->buf) - b->end,
                            &b->rlen, read_in, b);
}

static struct io_plan *write_out(struct io_conn *c, struct buffer *b)
{
     // Remove what we just wrote.
     b->start += b->wlen;
     b->wlen = 0;
     assert(b->start <= sizeof(b->buf));

     // If we wrote something, wake reader.
     if (b->wlen != 0)
             io_wake(b);

     // Nothing to write?  Wait for reader.
     if (b->end == b->start) {
             if (b->finished)
                     return io_close(c);
             return io_wait(c, b, write_out, b);
     }

     return io_write_partial(c, b->buf + b->start, b->end - b->start,
                             &b->wlen, write_out, b);
}

// Feed a program our stdin, gather its stdout, print that at end.
int main(int argc, char *argv[])
{
     int tochild[2], fromchild[2];
     struct buffer to, from;
     int status;
     struct io_conn *reader;

     if (argc == 1)
             errx(1, "Usage: runner <cmdline>...");

     if (pipe(tochild) != 0 || pipe(fromchild) != 0)
             err(1, "Creating pipes");

     if (!fork()) {
             // Child runs command.
             close(tochild[1]);
             close(fromchild[0]);

             dup2(tochild[0], STDIN_FILENO);
             dup2(fromchild[1], STDOUT_FILENO);
             execvp(argv[1], argv + 1);
             exit(127);
     }

     close(tochild[0]);
     close(fromchild[1]);
     signal(SIGPIPE, SIG_IGN);

     // Read from stdin, write to child.
     memset(&to, 0, sizeof(to));
     reader = io_new_conn(NULL, STDIN_FILENO, read_in, &to);
     io_set_finish(reader, finish, &to);
     io_new_conn(NULL, tochild[1], write_out, &to);

     // Read from child, write to stdout.
     memset(&from, 0, sizeof(from));
     reader = io_new_conn(NULL, fromchild[0], read_in, &from);
     io_set_finish(reader, finish, &from);
     io_new_conn(NULL, STDOUT_FILENO, write_out, &from);

     io_loop(NULL, NULL);
     wait(&status);

     return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
}

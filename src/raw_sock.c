/*
 * Functions used to send/receive data using SOCK_STREAM sockets.
 *
 * Copyright 2000-2012 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <netinet/tcp.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/standard.h>
#include <common/ticks.h>
#include <common/time.h>

#include <proto/buffers.h>
#include <proto/connection.h>
#include <proto/fd.h>
#include <proto/freq_ctr.h>
#include <proto/log.h>
#include <proto/pipe.h>
#include <proto/protocols.h>
#include <proto/raw_sock.h>
#include <proto/stream_interface.h>
#include <proto/task.h>

#include <types/global.h>

/* main event functions used to move data between sockets and buffers */
static void sock_raw_read(struct connection *conn);


#if 0 && defined(CONFIG_HAP_LINUX_SPLICE)
#include <common/splice.h>

/* A pipe contains 16 segments max, and it's common to see segments of 1448 bytes
 * because of timestamps. Use this as a hint for not looping on splice().
 */
#define SPLICE_FULL_HINT	16*1448

/* how many data we attempt to splice at once when the buffer is configured for
 * infinite forwarding */
#define MAX_SPLICE_AT_ONCE	(1<<30)

/* Returns :
 *   -1 if splice is not possible or not possible anymore and we must switch to
 *      user-land copy (eg: to_forward reached)
 *    0 otherwise, including errors and close.
 * Sets :
 *   BF_READ_NULL
 *   BF_READ_PARTIAL
 *   BF_WRITE_PARTIAL (during copy)
 *   BF_OUT_EMPTY (during copy)
 *   SI_FL_ERR
 *   SI_FL_WAIT_ROOM
 *   (SI_FL_WAIT_RECV)
 *
 * This function automatically allocates a pipe from the pipe pool. It also
 * carefully ensures to clear b->pipe whenever it leaves the pipe empty.
 */
static int sock_raw_splice_in(struct channel *b, struct stream_interface *si)
{
	static int splice_detects_close;
	int fd = si_fd(si);
	int ret;
	unsigned long max;
	int retval = 0;

	if (!b->to_forward)
		return -1;

	if (!(b->flags & BF_KERN_SPLICING))
		return -1;

	if (buffer_not_empty(&b->buf)) {
		/* We're embarrassed, there are already data pending in
		 * the buffer and we don't want to have them at two
		 * locations at a time. Let's indicate we need some
		 * place and ask the consumer to hurry.
		 */
		si->flags |= SI_FL_WAIT_ROOM;
		conn_data_stop_recv(&si->conn);
		b->rex = TICK_ETERNITY;
		si_chk_snd(b->cons);
		return 0;
	}

	if (unlikely(b->pipe == NULL)) {
		if (pipes_used >= global.maxpipes || !(b->pipe = get_pipe())) {
			b->flags &= ~BF_KERN_SPLICING;
			return -1;
		}
	}

	/* At this point, b->pipe is valid */

	while (1) {
		if (b->to_forward == BUF_INFINITE_FORWARD)
			max = MAX_SPLICE_AT_ONCE;
		else
			max = b->to_forward;

		if (!max) {
			/* It looks like the buffer + the pipe already contain
			 * the maximum amount of data to be transferred. Try to
			 * send those data immediately on the other side if it
			 * is currently waiting.
			 */
			retval = -1; /* end of forwarding */
			break;
		}

		ret = splice(fd, NULL, b->pipe->prod, NULL, max,
			     SPLICE_F_MOVE|SPLICE_F_NONBLOCK);

		if (ret <= 0) {
			if (ret == 0) {
				/* connection closed. This is only detected by
				 * recent kernels (>= 2.6.27.13). If we notice
				 * it works, we store the info for later use.
				 */
				splice_detects_close = 1;
				b->flags |= BF_READ_NULL;
				break;
			}

			if (errno == EAGAIN) {
				/* there are two reasons for EAGAIN :
				 *   - nothing in the socket buffer (standard)
				 *   - pipe is full
				 *   - the connection is closed (kernel < 2.6.27.13)
				 * Since we don't know if pipe is full, we'll
				 * stop if the pipe is not empty. Anyway, we
				 * will almost always fill/empty the pipe.
				 */

				if (b->pipe->data) {
					si->flags |= SI_FL_WAIT_ROOM;
					break;
				}

				/* We don't know if the connection was closed,
				 * but if we know splice detects close, then we
				 * know it for sure.
				 * But if we're called upon POLLIN with an empty
				 * pipe and get EAGAIN, it is suspect enough to
				 * try to fall back to the normal recv scheme
				 * which will be able to deal with the situation.
				 */
				if (splice_detects_close)
					conn_data_poll_recv(&si->conn); /* we know for sure that it's EAGAIN */
				else
					retval = -1;
				break;
			}

			if (errno == ENOSYS || errno == EINVAL) {
				/* splice not supported on this end, disable it */
				b->flags &= ~BF_KERN_SPLICING;
				si->flags &= ~SI_FL_CAP_SPLICE;
				put_pipe(b->pipe);
				b->pipe = NULL;
				return -1;
			}

			/* here we have another error */
			si->flags |= SI_FL_ERR;
			break;
		} /* ret <= 0 */

		if (b->to_forward != BUF_INFINITE_FORWARD)
			b->to_forward -= ret;
		b->total += ret;
		b->pipe->data += ret;
		b->flags |= BF_READ_PARTIAL;
		b->flags &= ~BF_OUT_EMPTY;

		if (b->pipe->data >= SPLICE_FULL_HINT ||
		    ret >= global.tune.recv_enough) {
			/* We've read enough of it for this time. */
			break;
		}
	} /* while */

	if (unlikely(!b->pipe->data)) {
		put_pipe(b->pipe);
		b->pipe = NULL;
	}

	return retval;
}

#endif /* CONFIG_HAP_LINUX_SPLICE */


/* Receive up to <count> bytes from connection <conn>'s socket and store them
 * into buffer <buf>. The caller must ensure that <count> is always smaller
 * than the buffer's size. Only one call to recv() is performed, unless the
 * buffer wraps, in which case a second call may be performed. The connection's
 * flags are updated with whatever special event is detected (error, read0,
 * empty). The caller is responsible for taking care of those events and
 * avoiding the call if inappropriate. The function does not call the
 * connection's polling update function, so the caller is responsible for this.
 */
static int raw_sock_to_buf(struct connection *conn, struct buffer *buf, int count)
{
	int ret, done = 0;
	int try = count;

	/* stop here if we reached the end of data */
	if ((fdtab[conn->t.sock.fd].ev & (FD_POLL_IN|FD_POLL_HUP)) == FD_POLL_HUP)
		goto read0;

	/* compute the maximum block size we can read at once. */
	if (buffer_empty(buf)) {
		/* let's realign the buffer to optimize I/O */
		buf->p = buf->data;
	}
	else if (buf->data + buf->o < buf->p &&
		 buf->p + buf->i < buf->data + buf->size) {
		/* remaining space wraps at the end, with a moving limit */
		if (try > buf->data + buf->size - (buf->p + buf->i))
			try = buf->data + buf->size - (buf->p + buf->i);
	}

	/* read the largest possible block. For this, we perform only one call
	 * to recv() unless the buffer wraps and we exactly fill the first hunk,
	 * in which case we accept to do it once again. A new attempt is made on
	 * EINTR too.
	 */
	while (try) {
		ret = recv(conn->t.sock.fd, bi_end(buf), try, 0);

		if (ret > 0) {
			buf->i += ret;
			done += ret;
			if (ret < try) {
				/* unfortunately, on level-triggered events, POLL_HUP
				 * is generally delivered AFTER the system buffer is
				 * empty, so this one might never match.
				 */
				if (fdtab[conn->t.sock.fd].ev & FD_POLL_HUP)
					goto read0;
				break;
			}
			count -= ret;
			try = count;
		}
		else if (ret == 0) {
			goto read0;
		}
		else if (errno == EAGAIN) {
			conn->flags |= CO_FL_WAIT_DATA;
			break;
		}
		else if (errno != EINTR) {
			conn->flags |= CO_FL_ERROR;
			break;
		}
	}
	return done;

 read0:
	conn_sock_read0(conn);
	return done;
}


/*
 * this function is called on a read event from a stream socket.
 */
static void sock_raw_read(struct connection *conn)
{
	struct stream_interface *si = container_of(conn, struct stream_interface, conn);
	struct channel *b = si->ib;
	int ret, max, cur_read;
	int read_poll = MAX_READ_POLL_LOOPS;

#ifdef DEBUG_FULL
	fprintf(stderr,"sock_raw_read : fd=%d, ev=0x%02x, owner=%p\n", conn->t.sock.fd, fdtab[conn->t.sock.fd].ev, fdtab[conn->t.sock.fd].owner);
#endif
	/* stop immediately on errors. Note that we DON'T want to stop on
	 * POLL_ERR, as the poller might report a write error while there
	 * are still data available in the recv buffer. This typically
	 * happens when we send too large a request to a backend server
	 * which rejects it before reading it all.
	 */
	if (conn->flags & CO_FL_ERROR)
		goto out_error;

	/* stop here if we reached the end of data */
	if (conn_data_read0_pending(conn))
		goto out_shutdown_r;

	/* maybe we were called immediately after an asynchronous shutr */
	if (b->flags & BF_SHUTR)
		return;

#if 0 && defined(CONFIG_HAP_LINUX_SPLICE)
	if (b->to_forward >= MIN_SPLICE_FORWARD && b->flags & BF_KERN_SPLICING) {

		/* Under Linux, if FD_POLL_HUP is set, we have reached the end.
		 * Since older splice() implementations were buggy and returned
		 * EAGAIN on end of read, let's bypass the call to splice() now.
		 */
		if (fdtab[conn->t.sock.fd].ev & FD_POLL_HUP)
			goto out_shutdown_r;

		if (sock_raw_splice_in(b, si) >= 0) {
			if (si->flags & SI_FL_ERR)
				goto out_error;
			if (b->flags & BF_READ_NULL)
				goto out_shutdown_r;
			return;
		}
		/* splice not possible (anymore), let's go on on standard copy */
	}
#endif
	cur_read = 0;
	conn->flags &= ~(CO_FL_WAIT_DATA | CO_FL_WAIT_ROOM);
	while (!(conn->flags & (CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_DATA_RD_SH | CO_FL_WAIT_DATA | CO_FL_WAIT_ROOM | CO_FL_HANDSHAKE))) {
		max = bi_avail(b);

		if (!max) {
			b->flags |= BF_FULL;
			si->flags |= SI_FL_WAIT_ROOM;
			break;
		}

		ret = conn->data->rcv_buf(conn, &b->buf, max);
		if (ret <= 0)
			break;

		cur_read += ret;

		/* if we're allowed to directly forward data, we must update ->o */
		if (b->to_forward && !(b->flags & (BF_SHUTW|BF_SHUTW_NOW))) {
			unsigned long fwd = ret;
			if (b->to_forward != BUF_INFINITE_FORWARD) {
				if (fwd > b->to_forward)
					fwd = b->to_forward;
				b->to_forward -= fwd;
			}
			b_adv(b, fwd);
		}

		if (conn->flags & CO_FL_WAIT_L4_CONN) {
			conn->flags &= ~CO_FL_WAIT_L4_CONN;
			si->exp = TICK_ETERNITY;
		}

		b->flags |= BF_READ_PARTIAL;
		b->total += ret;

		if (bi_full(b)) {
			/* The buffer is now full, there's no point in going through
			 * the loop again.
			 */
			if (!(b->flags & BF_STREAMER_FAST) && (cur_read == buffer_len(&b->buf))) {
				b->xfer_small = 0;
				b->xfer_large++;
				if (b->xfer_large >= 3) {
					/* we call this buffer a fast streamer if it manages
					 * to be filled in one call 3 consecutive times.
					 */
					b->flags |= (BF_STREAMER | BF_STREAMER_FAST);
					//fputc('+', stderr);
				}
			}
			else if ((b->flags & (BF_STREAMER | BF_STREAMER_FAST)) &&
				 (cur_read <= b->buf.size / 2)) {
				b->xfer_large = 0;
				b->xfer_small++;
				if (b->xfer_small >= 2) {
					/* if the buffer has been at least half full twice,
					 * we receive faster than we send, so at least it
					 * is not a "fast streamer".
					 */
					b->flags &= ~BF_STREAMER_FAST;
					//fputc('-', stderr);
				}
			}
			else {
				b->xfer_small = 0;
				b->xfer_large = 0;
			}

			b->flags |= BF_FULL;
			si->flags |= SI_FL_WAIT_ROOM;
			break;
		}

		if ((b->flags & BF_READ_DONTWAIT) || --read_poll <= 0)
			break;

		/* if too many bytes were missing from last read, it means that
		 * it's pointless trying to read again because the system does
		 * not have them in buffers.
		 */
		if (ret < max) {
			if ((b->flags & (BF_STREAMER | BF_STREAMER_FAST)) &&
			    (cur_read <= b->buf.size / 2)) {
				b->xfer_large = 0;
				b->xfer_small++;
				if (b->xfer_small >= 3) {
					/* we have read less than half of the buffer in
					 * one pass, and this happened at least 3 times.
					 * This is definitely not a streamer.
					 */
					b->flags &= ~(BF_STREAMER | BF_STREAMER_FAST);
					//fputc('!', stderr);
				}
			}

			/* if a streamer has read few data, it may be because we
			 * have exhausted system buffers. It's not worth trying
			 * again.
			 */
			if (b->flags & BF_STREAMER)
				break;

			/* if we read a large block smaller than what we requested,
			 * it's almost certain we'll never get anything more.
			 */
			if (ret >= global.tune.recv_enough)
				break;
		}
	} /* while !flags */

	if (conn->flags & CO_FL_ERROR)
		goto out_error;

	if (conn->flags & CO_FL_WAIT_DATA) {
		/* we don't automatically ask for polling if we have
		 * read enough data, as it saves some syscalls with
		 * speculative pollers.
		 */
		if (cur_read < MIN_RET_FOR_READ_LOOP)
			__conn_data_poll_recv(conn);
		else
			__conn_data_want_recv(conn);
	}

	if (conn_data_read0_pending(conn))
		/* connection closed */
		goto out_shutdown_r;

	return;

 out_shutdown_r:
	/* we received a shutdown */
	b->flags |= BF_READ_NULL;
	if (b->flags & BF_AUTO_CLOSE)
		buffer_shutw_now(b);
	stream_sock_read0(si);
	conn_data_read0(conn);
	return;

 out_error:
	/* Read error on the connection, report the error and stop I/O */
	conn->flags |= CO_FL_ERROR;
	conn_data_stop_both(conn);
}


/*
 * This function is called to send buffer data to a stream socket.
 * It returns -1 in case of unrecoverable error, otherwise zero.
 */
static int sock_raw_write_loop(struct connection *conn)
{
	struct stream_interface *si = container_of(conn, struct stream_interface, conn);
	struct channel *b = si->ob;
	int write_poll = MAX_WRITE_POLL_LOOPS;
	int ret, max;

#if 0 && defined(CONFIG_HAP_LINUX_SPLICE)
	while (b->pipe) {
		ret = splice(b->pipe->cons, NULL, si_fd(si), NULL, b->pipe->data,
			     SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
		if (ret <= 0) {
			if (ret == 0 || errno == EAGAIN) {
				conn_data_poll_send(&si->conn);
				return 0;
			}
			/* here we have another error */
			return -1;
		}

		b->flags |= BF_WRITE_PARTIAL;
		b->pipe->data -= ret;

		if (!b->pipe->data) {
			put_pipe(b->pipe);
			b->pipe = NULL;
			break;
		}

		if (--write_poll <= 0)
			return 0;

		/* The only reason we did not empty the pipe is that the output
		 * buffer is full.
		 */
		conn_data_poll_send(&si->conn);
		return 0;
	}

	/* At this point, the pipe is empty, but we may still have data pending
	 * in the normal buffer.
	 */
#endif
	if (!b->buf.o) {
		b->flags |= BF_OUT_EMPTY;
		return 0;
	}

	/* when we're in this loop, we already know that there is no spliced
	 * data left, and that there are sendable buffered data.
	 */
	while (1) {
		max = b->buf.o;

		/* outgoing data may wrap at the end */
		if (b->buf.data + max > b->buf.p)
			max = b->buf.data + max - b->buf.p;

		/* check if we want to inform the kernel that we're interested in
		 * sending more data after this call. We want this if :
		 *  - we're about to close after this last send and want to merge
		 *    the ongoing FIN with the last segment.
		 *  - we know we can't send everything at once and must get back
		 *    here because of unaligned data
		 *  - there is still a finite amount of data to forward
		 * The test is arranged so that the most common case does only 2
		 * tests.
		 */

		if (MSG_NOSIGNAL && MSG_MORE) {
			unsigned int send_flag = MSG_DONTWAIT | MSG_NOSIGNAL;

			if ((!(b->flags & BF_NEVER_WAIT) &&
			    ((b->to_forward && b->to_forward != BUF_INFINITE_FORWARD) ||
			     (b->flags & BF_EXPECT_MORE))) ||
			    ((b->flags & (BF_SHUTW|BF_SHUTW_NOW|BF_HIJACK)) == BF_SHUTW_NOW && (max == b->buf.o)) ||
			    (max != b->buf.o)) {
				send_flag |= MSG_MORE;
			}

			/* this flag has precedence over the rest */
			if (b->flags & BF_SEND_DONTWAIT)
				send_flag &= ~MSG_MORE;

			ret = send(si_fd(si), bo_ptr(&b->buf), max, send_flag);
		} else {
			int skerr;
			socklen_t lskerr = sizeof(skerr);

			ret = getsockopt(si_fd(si), SOL_SOCKET, SO_ERROR, &skerr, &lskerr);
			if (ret == -1 || skerr)
				ret = -1;
			else
				ret = send(si_fd(si), bo_ptr(&b->buf), max, MSG_DONTWAIT);
		}

		if (ret > 0) {
			if (si->conn.flags & CO_FL_WAIT_L4_CONN) {
				si->conn.flags &= ~CO_FL_WAIT_L4_CONN;
				si->exp = TICK_ETERNITY;
			}

			b->flags |= BF_WRITE_PARTIAL;

			b->buf.o -= ret;
			if (likely(!buffer_len(&b->buf)))
				/* optimize data alignment in the buffer */
				b->buf.p = b->buf.data;

			if (likely(!bi_full(b)))
				b->flags &= ~BF_FULL;

			if (!b->buf.o) {
				/* Always clear both flags once everything has been sent, they're one-shot */
				b->flags &= ~(BF_EXPECT_MORE | BF_SEND_DONTWAIT);
				if (likely(!b->pipe))
					b->flags |= BF_OUT_EMPTY;
				break;
			}

			/* if the system buffer is full, don't insist */
			if (ret < max)
				break;

			if (--write_poll <= 0)
				break;
		}
		else if (ret == 0 || errno == EAGAIN) {
			/* nothing written, we need to poll for write first */
			conn_data_poll_send(&si->conn);
			return 0;
		}
		else {
			/* bad, we got an error */
			return -1;
		}
	} /* while (1) */
	return 0;
}


/* stream sock operations */
struct sock_ops raw_sock = {
	.update  = stream_int_update_conn,
	.shutr   = NULL,
	.shutw   = NULL,
	.chk_rcv = stream_int_chk_rcv_conn,
	.chk_snd = stream_int_chk_snd_conn,
	.read    = sock_raw_read,
	.write   = si_conn_send_cb,
	.snd_buf = sock_raw_write_loop,
	.rcv_buf = raw_sock_to_buf,
	.close   = NULL,
};

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */

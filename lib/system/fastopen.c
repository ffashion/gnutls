/*
 * Copyright (C) 2016 Free Software Foundation, Inc.
 *
 * Author: Nikos Mavrogiannopoulos
 *
 * This file is part of GnuTLS.
 *
 * The GnuTLS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 *
 */

#include <config.h>
#include <system.h>
#include "gnutls_int.h"
#include "errors.h"

#include <sys/socket.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <c-ctype.h>

/* Get TCP_FASTOPEN */
#ifdef HAVE_NETINET_TCP_H
# include <netinet/tcp.h>
#endif

/* Do not use the gnulib functions for sending and receiving data.
 * Using them makes gnutls only working with gnulib applications.
 */
#undef send
#undef recv
#undef select
#undef connect

#ifdef _WIN32
static ssize_t
tfo_send(gnutls_transport_ptr_t ptr, const void *buf, size_t len)
{
	tfo_st *p = ptr;
	int fd = p->fd;

	if (unlikely(p->connect_addrlen != 0)) {
		int ret;

		ret = connect(fd, (struct sockaddr*)&p->connect_addr, p->connect_addrlen);
		if (errno == ENOTCONN || errno == EINPROGRESS) {
			gnutls_assert();
			errno = EAGAIN;
		}

		if (ret == 0 || errno != EAGAIN) {
			p->connect_only = 0;
			p->connect_addrlen = 0;
		}

		return ret;
	}

	return send(fd, buf, len, 0);
}
#else /* sendmsg */
static ssize_t
tfo_writev(gnutls_transport_ptr_t ptr, const giovec_t * iovec, int iovec_cnt)
{
	tfo_st *p = ptr;
	int fd = p->fd;
	struct msghdr hdr;
	int ret, on = 1;

	memset(&hdr, 0, sizeof(hdr));
	hdr.msg_iov = (struct iovec *)iovec;
	hdr.msg_iovlen = iovec_cnt;

	if (likely(!p->connect_addrlen))
		return sendmsg(fd, &hdr, p->flags);

#ifdef MSG_FASTOPEN
	if (!p->connect_only) {
		if (setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &on, sizeof(on)) == -1)
			_gnutls_debug_log("Failed to set socket option FASTOPEN\n");

		hdr.msg_name = &p->connect_addr;
		hdr.msg_namelen = p->connect_addrlen;

		ret = sendmsg(fd, &hdr, p->flags | MSG_FASTOPEN);

		if (ret < 0) {
			if (errno == EINPROGRESS) {
				gnutls_assert();
				errno = EAGAIN; // GnuTLS does not handle EINPROGRESS
			} else if (errno == EOPNOTSUPP) {
				// fallback from fastopen, e.g. when fastopen is disabled in system
				_gnutls_debug_log("Fallback from TCP Fast Open... TFO is not enabled at system level\n");
				p->connect_only = 1;
				goto connect_only;
			}
		}
	} else
#endif
	{
 connect_only:
		ret = connect(fd, (struct sockaddr*)&p->connect_addr, p->connect_addrlen);
		if (errno == ENOTCONN || errno == EINPROGRESS) {
			gnutls_assert();
			errno = EAGAIN;
		}

		if (ret == 0)
			p->connect_only = 0;
	}

	if (ret == 0 || errno != EAGAIN) {
		/* This has to be called just once, connect info not needed any more */
		p->connect_addrlen = 0;
	}

	return ret;
}
#endif

static
int tfo_recv_timeout(gnutls_transport_ptr_t ptr, unsigned int ms)
{
	tfo_st *p = ptr;

	return gnutls_system_recv_timeout((gnutls_transport_ptr_t)(long)p->fd, ms);
}

static ssize_t
tfo_read(gnutls_transport_ptr_t ptr, void *data, size_t data_size)
{
	tfo_st *p = ptr;

	return recv(p->fd, data, data_size, 0);
}

/**
 * gnutls_transport_set_fastopen:
 * @session: is a #gnutls_session_t type.
 * @fd: is the session's socket descriptor
 * @connect_addr: is the address we want to connect to
 * @connect_addrlen: is the length of @connect_addr
 *
 * Enables TCP Fast Open (TFO) when @connect_addr and @connect_addrlen are set
 * before the transport socket has been connected.
 *
 * TFO only works for TCP sockets of type AF_INET and AF_INET6.
 * If the OS doesn't support TCP fast open this function will use
 * connect() transparently during the first write.
 *
 * Note: This function overrides all transport callback functions.
 * If this is undesirable, TCP Fast Open must be implemented on the user
 * callback functions without calling this function. When using
 * this function gnutls_transport_set_ptr() or gnutls_transport_set_int()
 * must not be used.
 *
 * On GNU/Linux TFO has to be enabled at the system layer, that is
 *   in /proc/sys/net/ipv4/tcp_fastopen, bit 0 has to be set.
 *
 * Since: 3.5.3
 **/
void
gnutls_transport_set_fastopen(gnutls_session_t session,
			      int fd, struct sockaddr *connect_addr, socklen_t connect_addrlen)
{
	if (connect_addrlen > (socklen_t)sizeof(session->internals.tfo.connect_addr)) {
		gnutls_assert();
		abort();
	}

	memcpy(&session->internals.tfo.connect_addr, connect_addr, connect_addrlen);
	session->internals.tfo.connect_addrlen = connect_addrlen;
	session->internals.tfo.fd = fd;

	gnutls_transport_set_pull_function(session, tfo_read);
	gnutls_transport_set_pull_timeout_function(session, tfo_recv_timeout);
	gnutls_transport_set_ptr(session, &session->internals.tfo);

	session->internals.tfo.flags = 0;
#ifdef MSG_NOSIGNAL
	if (session->internals.flags & GNUTLS_NO_SIGNAL)
		session->internals.tfo.flags |= MSG_NOSIGNAL;
#endif

#ifdef _WIN32
	gnutls_transport_set_vec_push_function(session, NULL);
	gnutls_transport_set_push_function(session, tfo_send);
#else
	gnutls_transport_set_vec_push_function(session, tfo_writev);
#endif
}

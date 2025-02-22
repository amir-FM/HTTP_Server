// SPDX-License-Identifier: BSD-3-Clause

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libaio.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aws.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

void
connection_clean_send_buffer(struct connection *conn)
{
	conn->send_len = conn->send_pos = 0;
}

size_t
get_file_size(struct connection *conn)
{
	struct stat st;

	fstat(conn->fd, &st);
	return st.st_size;
}

static int
aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

void
sending_header(struct connection *conn)
{
	if (conn->send_len == conn->send_pos) {
		if (conn->state == STATE_SENDING_HEADER)
			conn->state = STATE_HEADER_SENT;
		else if (conn->state == STATE_SENDING_404)
			conn->state = STATE_404_SENT;
		return;
	}

	char *send_buffer_with_offset = conn->send_buffer + conn->send_pos;
	int remaining_len = conn->send_len - conn->send_pos;

	conn->send_pos +=
	  send(conn->sockfd, send_buffer_with_offset, remaining_len, 0);
}

static void
connection_prepare_send_reply_header(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the reply header. */
	char reply_header[] = "HTTP/1.1 200 OK\r\n"
						  "Date: Sun, 08 May 2011 09:26:16 GMT\r\n"
						  "Server: Apache/2.2.9\r\n"
						  "Last-Modified: Mon, 02 Aug 2010 17:55:28 GMT\r\n"
						  "Accept-Ranges: bytes\r\n"
						  "Content-Length: %d\r\n"
						  "Connection: close\r\n"
						  "Content-Type: text/html\r\n"
						  "\r\n";

	sprintf(conn->send_buffer, reply_header, get_file_size(conn));
	conn->send_len = strlen(conn->send_buffer);
	conn->state = STATE_SENDING_HEADER;
}

static void
connection_prepare_send_404(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the 404 header. */
	char reply_header[] = "HTTP/1.1 404 Not Found\r\n"
						  "Date: Sun, 08 May 2011 09:26:16 GMT\r\n"
						  "Server: Apache/2.2.9\r\n"
						  "Content-Length: %d\r\n"
						  "Connection: close\r\n"
						  "\r\n";

	int send_len = strlen(reply_header);

	sprintf(conn->send_buffer, "%s", reply_header);
	conn->send_len = send_len;
	conn->state = STATE_SENDING_404;
}

static enum resource_type
connection_get_resource_type(struct connection *conn)
{
	/* TODO: Get resource type depending on request path/filename. Filename
	 * should point to the static or dynamic folder.
	 */
	int rc;

	if (strstr(conn->request_path, AWS_REL_STATIC_FOLDER) ==
		conn->request_path + 1)
		rc = conn->res_type = RESOURCE_TYPE_STATIC;
	else if (strstr(conn->request_path, AWS_REL_DYNAMIC_FOLDER) ==
			 conn->request_path + 1)
		rc = conn->res_type = RESOURCE_TYPE_DYNAMIC;
	else
		rc = conn->res_type = RESOURCE_TYPE_NONE;

	return rc;
}

struct connection *
connection_create(int sockfd)
{
	/* TODO: Initialize connection structure on given socket. */
	struct connection *conn = calloc(1, sizeof(struct connection));

	DIE(conn == NULL, "calloc");
	conn->sockfd = sockfd;
	conn->state = STATE_INITIAL;

	return conn;
}

void
connection_start_async_io(struct connection *conn)
{
	/* TODO: Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
	if (conn->file_pos == conn->file_size) {
		conn->state = STATE_DATA_SENT;
		return;
	}

	int rc;

	conn->ctx = ctx;
	conn->iocb.data = conn;
	conn->iocb.aio_fildes = conn->fd;
	conn->iocb.aio_lio_opcode = IO_CMD_PREAD;
	conn->iocb.u.c.buf = conn->send_buffer;
	conn->iocb.u.c.nbytes = BUFSIZ;
	conn->iocb.u.c.offset = conn->file_pos;
	conn->piocb[0] = &conn->iocb;

	rc = io_submit(conn->ctx, 1, conn->piocb);
	DIE(rc < 0, "io_submit");
	conn->state = STATE_ASYNC_WAITING;
}

void
connection_remove(struct connection *conn)
{
	/* TODO: Remove connection handler. */
	w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

void
handle_new_connection(void)
{
	/* TODO: Handle a new connection request on the server socket. */
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;
	int flags;

	/* TODO: Accept new connection. */
	sockfd = accept(listenfd, (SSA *)&addr, &addrlen);

	/* TODO: Set socket to be non-blocking. */
	flags = fcntl(sockfd, F_GETFL, 0);
	DIE(flags < 0, "fcntl");
	flags = flags | O_NONBLOCK;
	rc = fcntl(sockfd, F_SETFL, flags);
	DIE(rc < 0, "fcntl");

	/* TODO: Instantiate new connection handler. */
	conn = connection_create(sockfd);

	/* TODO: Add socket to epoll. */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);

	/* TODO: Initialize HTTP_REQUEST parser. */
	http_parser_init(&(conn->request_parser), HTTP_REQUEST);
	conn->request_parser.data = conn;
}

void
receive_data(struct connection *conn)
{
	/* TODO: Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
	int rc = 0;

	rc = recv(conn->sockfd, conn->recv_buffer, BUFSIZ, 0);
	if (rc < 0) {
		conn->state = STATE_CONNECTION_CLOSED;
		return;
	}
	conn->recv_len += rc;

	while (1) {
		rc = recv(conn->sockfd,
				  conn->recv_buffer + conn->recv_len,
				  BUFSIZ - conn->recv_len,
				  0);
		if (rc <= 0)
			break;
		conn->recv_len += rc;
	}

	conn->state = STATE_REQUEST_RECEIVED;
}

int
connection_open_file(struct connection *conn)
{
	/* TODO: Open file and update connection fields. */
	int aws_root_len = strlen(AWS_DOCUMENT_ROOT);
	char *path = malloc(aws_root_len + strlen(conn->request_path) + 1);

	DIE(path == NULL, "malloc");

	path[0] = '\0';
	strcat(path, AWS_DOCUMENT_ROOT);
	path[aws_root_len - 1] = 0;
	strcat(path, conn->request_path);

	int fd = open(path, O_RDONLY);

	conn->fd = fd;

	if (fd >= 0)
		conn->file_size = get_file_size(conn);

	free(path);

	return fd;
}

void
connection_complete_async_io(struct connection *conn)
{
	/* TODO: Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 */
	int rc;
	struct io_event rev;

	rc = io_getevents(ctx, 0, 1, &rev, NULL);
	if (rc <= 0)
		return;
	struct connection *conn2 = (struct connection *)rev.obj->data;

	conn->state = STATE_ASYNC_NOTIFICATION;
	conn->async_read_len = rev.res; // bytes read
	handle_output(conn2);
}

int
parse_header(struct connection *conn)
{
	/* TODO: Parse the HTTP header and extract the file path. */
	/* Use mostly null settings except for on_path callback. */
	int rc;

	http_parser_settings settings_on_path = { .on_message_begin = 0,
											  .on_header_field = 0,
											  .on_header_value = 0,
											  .on_path = aws_on_path_cb,
											  .on_url = 0,
											  .on_fragment = 0,
											  .on_query_string = 0,
											  .on_body = 0,
											  .on_headers_complete = 0,
											  .on_message_complete = 0 };

	rc = http_parser_execute(&(conn->request_parser),
							 &settings_on_path,
							 conn->recv_buffer,
							 conn->recv_len);

	return rc;
}

enum connection_state
connection_send_static(struct connection *conn)
{
	/* TODO: Send static data using sendfile(2). */
	size_t file_size = get_file_size(conn);

	if (conn->file_pos == file_size) {
		conn->state = STATE_DATA_SENT;
		return STATE_DATA_SENT;
	}

	sendfile(
	  conn->sockfd, conn->fd, (off_t *)&(conn->file_pos), file_size - conn->file_pos);

	return STATE_SENDING_DATA;
}

void
connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* TODO: Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	if (conn->send_len == conn->send_pos) {
		conn->send_len = conn->send_pos = 0;
		conn->state = STATE_DYNAMIC_SENT;
		return;
	}

	char *send_buffer_with_offset = conn->send_buffer + conn->send_pos;
	size_t remaining_len = conn->send_len - conn->send_pos;

	conn->send_pos +=
	  send(conn->sockfd, send_buffer_with_offset, remaining_len, 0);
}

void
connection_send_dynamic(struct connection *conn)
{
	/* TODO: Read data asynchronously.
	 * Returns 0 on success and -1 on error.
	 */
	conn->send_len = conn->async_read_len;
	conn->file_pos += conn->async_read_len;
	conn->state = STATE_SENDING_DYNAMIC;
}

void
handle_input(struct connection *conn)
{
	/* TODO: Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */
	int rc;

	switch (conn->state) {
	case STATE_INITIAL:
		receive_data(conn);
		parse_header(conn);
		connection_get_resource_type(conn);
		rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_add_ptr_inout");
		break;
	default:
		printf("(input)shouldn't get here %d\n", conn->state);
	}
}

void
handle_output(struct connection *conn)
{
	/* TODO: Handle output information: may be a new valid requests or
	 * notification of completion of an asynchronous I/O operation or invalid
	 * requests.
	 */
	int rc;

	switch (conn->state) {
	case STATE_REQUEST_RECEIVED:
		rc = connection_open_file(conn);
		if (rc < 0) {
			connection_prepare_send_404(conn);
			break;
		}

		switch (conn->res_type) {
		case RESOURCE_TYPE_STATIC:
			connection_prepare_send_reply_header(conn);
			break;
		case RESOURCE_TYPE_DYNAMIC:
			connection_prepare_send_reply_header(conn);
			break;
		case RESOURCE_TYPE_NONE:
			connection_prepare_send_404(conn);
			break;
		}
		break;
	case STATE_SENDING_HEADER:
	case STATE_SENDING_404:
		sending_header(conn);
		break;
	case STATE_HEADER_SENT:
		switch (conn->res_type) {
		case RESOURCE_TYPE_STATIC:
			conn->state = STATE_SENDING_DATA;
			break;
		case RESOURCE_TYPE_DYNAMIC:
			connection_clean_send_buffer(conn);
			conn->state = STATE_ASYNC_ONGOING;
			break;
		case RESOURCE_TYPE_NONE:
			printf("How did you get here?\n");
			exit(1);
		}
		break;
	case STATE_SENDING_DATA:
		connection_send_static(conn);
		break;
	case STATE_ASYNC_ONGOING:
		connection_start_async_io(conn);
		break;
	case STATE_ASYNC_NOTIFICATION:
		connection_send_dynamic(conn);
		break;
	case STATE_SENDING_DYNAMIC:
		connection_send_data(conn);
		break;
	case STATE_DYNAMIC_SENT:
		conn->state = STATE_ASYNC_ONGOING;
		break;
	case STATE_ASYNC_WAITING:
		connection_complete_async_io(conn);
		break;
	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

void
handle_client(uint32_t event, struct connection *conn)
{
	/* TODO: Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.
	 */
	if (event & EPOLLIN)
		handle_input(conn);
	if (event & EPOLLOUT) {
		handle_output(conn);
		if (CLOSE_STATE(conn->state))
			connection_remove(conn);
	}
}

int
main(void)
{
	int rc;

	/* TODO: Initialize asynchronous operations. */
	rc = io_setup(10, &ctx);
	DIE(rc < 0, "io_setup");

	/* TODO: Initialize multiplexing. */
	epollfd = epoll_create1(0);
	DIE(epollfd < 0, "epoll_create1");

	/* TODO: Create server socket. */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, 10);

	/* TODO: Add server socket to epoll object*/
	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	/* Uncomment the following line for debugging. */
	/* dlog(LOG_INFO, "Server waiting for connections on port %d\n",
	 * AWS_LISTEN_PORT);
	 **/

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* TODO: Wait for events. */
		rc = epoll_wait(epollfd, &rev, 1, -1);

		// new connection
		if (rev.data.fd == listenfd) {
			handle_new_connection();
		} else { // socket communication
			handle_client(rev.events, rev.data.ptr);
		}
	}

	return 0;
}

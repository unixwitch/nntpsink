/* nntpsink: dummy NNTP server */
/* 
 * Copyright (c) 2013 River Tarnell.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#include	<sys/types.h>
#include	<sys/socket.h>
#include	<sys/resource.h>

#include	<netinet/in.h>
#include	<netinet/tcp.h>

#include	<stdlib.h>
#include	<stdio.h>
#include	<unistd.h>
#include	<string.h>
#include	<netdb.h>
#include	<errno.h>
#include	<fcntl.h>
#include	<ctype.h>
#include	<assert.h>
#include	<time.h>
#include	<stdarg.h>

#include	<ev.h>

#include	"nntpsink.h"
#include	"charq.h"

char	*listen_host;
char	*port;
int	 debug;

int	 do_ihave = 1;
int	 do_streaming = 1;

#define		ignore_errno(e) ((e) == EAGAIN || (e) == EINPROGRESS || (e) == EWOULDBLOCK)

typedef enum client_state {
	CL_NORMAL,
	CL_TAKETHIS,
	CL_IHAVE
} client_state_t;

#define	CL_DEAD		0x1

typedef struct client {
	int		 cl_fd;
	ev_io		 cl_readable;
	ev_io		 cl_writable;
	charq_t		*cl_wrbuf;
	charq_t		*cl_rdbuf;
	client_state_t	 cl_state;
	int		 cl_flags;
	char		*cl_msgid;
	struct client	*cl_next;
} client_t;

client_t	*deadlist;

void	client_read(struct ev_loop *, ev_io *, int);
void	client_write(struct ev_loop *, ev_io *, int);
void	client_flush(client_t *);
void	client_close(client_t *);
void	client_printf(client_t *, char const *, ...);
void	client_vprintf(client_t *, char const *, va_list);

struct ev_prepare	housekeeping_ev;
void	do_housekeeping(struct ev_loop *, ev_prepare *w, int revents);

typedef struct listener {
	int	ln_fd;
	ev_io	ln_readable;
} listener_t;

void	listener_accept(struct ev_loop *, ev_io *, int);

struct ev_loop	*loop;
ev_timer	 stats_timer;
time_t		 start_time;

void	 usage(char const *);

int	nsend, naccept, ndefer, nreject, nrefuse;
void	do_stats(struct ev_loop *, ev_timer *w, int);

void
usage(p)
	char const	*p;
{
	fprintf(stderr,
"usage: %s [-VDhIS] [-l <host>] [-p <port>]\n"
"\n"
"    -V                   print version and exit\n"
"    -h                   print this text\n"
"    -D                   show data sent/received\n"
"    -I                   support IHAVE only (not streaming)\n"
"    -S                   support streaming only (not IHAVE)\n"
"    -l <host>            address to listen on (default: localhost)\n"
"    -p <port>            port to listen on (default: 119)\n"
, p);
}

int
main(ac, av)
	char	**av;
{
int	 c, i;
char	*progname = av[0];
struct addrinfo	*res, *r, hints;

	while ((c = getopt(ac, av, "VDSIh:p:")) != -1) {
		switch (c) {
		case 'V':
			printf("nntpgen %s\n", PACKAGE_VERSION);
			return 0;

		case 'D':
			debug++;
			break;

		case 'I':
			do_streaming = 0;
			break;

		case 'S':
			do_ihave = 0;
			break;

		case 'l':
			free(listen_host);
			listen_host = strdup(optarg);
			break;

		case 'p':
			free(port);
			port = strdup(optarg);
			break;

		case 'h':
			usage(av[0]);
			return 0;

		default:
			usage(av[0]);
			return 1;
		}
	}
	ac -= optind;
	av += optind;

	if (!do_ihave && !do_streaming) {
		fprintf(stderr, "%s: -I and -S may not both be specified\n", progname);
		return 1;
	}

	if (!listen_host)
		listen_host = strdup("localhost");

	if (!port)
		port = strdup("119");

	if (av[0]) {
		usage(progname);
		return 1;
	}

	loop = ev_loop_new(ev_supported_backends());

	bzero(&hints, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	i = getaddrinfo(listen_host, port, &hints, &res);

	if (i) {
		fprintf(stderr, "%s: %s:%s: %s\n",
			progname, listen_host, port, gai_strerror(i));
		return 1;
	}

	for (r = res; r; r = r->ai_next) {
	listener_t	*lsn = xcalloc(1, sizeof(*lsn));
	int		 fl, one = 1;
	char		 sname[NI_MAXHOST];

		if ((lsn->ln_fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol)) == -1) {
			fprintf(stderr, "%s:%s: socket: %s\n",
				listen_host, port, strerror(errno));
			return 1;
		}

		if ((fl = fcntl(lsn->ln_fd, F_GETFL, 0)) == -1) {
			fprintf(stderr, "%s:%s: fgetfl: %s\n",
				listen_host, port, strerror(errno));
			return 1;
		}

		if (fcntl(lsn->ln_fd, F_SETFL, fl | O_NONBLOCK) == -1) {
			fprintf(stderr, "%s:%s: fsetfl: %s\n",
				listen_host, port, strerror(errno));
			return 1;
		}

		if (setsockopt(lsn->ln_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == -1) {
			fprintf(stderr, "%s:%s: setsockopt(TCP_NODELAY): %s\n",
				listen_host, port, strerror(errno));
			return 1;
		}

		if (setsockopt(lsn->ln_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1) {
			fprintf(stderr, "%s:%s: setsockopt(SO_REUSEADDR): %s\n",
				listen_host, port, strerror(errno));
			return 1;
		}

		if (bind(lsn->ln_fd, r->ai_addr, r->ai_addrlen) == -1) {
			getnameinfo(r->ai_addr, r->ai_addrlen, sname, sizeof(sname),
					NULL, 0, NI_NUMERICHOST);
			fprintf(stderr, "%s[%s]:%s: bind: %s\n",
				listen_host, sname, port, strerror(errno));
			return 1;
		}

		if (listen(lsn->ln_fd, 128) == -1) {
			fprintf(stderr, "%s:%s: listen: %s\n",
				listen_host, port, strerror(errno));
			return 1;
		}

		ev_io_init(&lsn->ln_readable, listener_accept, lsn->ln_fd, EV_READ);
		lsn->ln_readable.data = lsn;

		ev_io_start(loop, &lsn->ln_readable);
	}
		
	freeaddrinfo(res);

	ev_timer_init(&stats_timer, do_stats, 1., 1.);
	ev_timer_start(loop, &stats_timer);

        ev_prepare_init(&housekeeping_ev, do_housekeeping);
	ev_prepare_start(loop, &housekeeping_ev);

	time(&start_time);
	ev_run(loop, 0);

	return 0;
}

void
listener_accept(loop, w, revents)
	struct ev_loop	*loop;
	ev_io		*w;
{
int			 fd;
listener_t		*lsn = w->data;
struct sockaddr_storage	 addr;
socklen_t		 addrlen;

	while ((fd = accept(lsn->ln_fd, (struct sockaddr *) &addr, &addrlen)) >= 0) {
	client_t	*client = xcalloc(1, sizeof(*client));

		client->cl_fd = fd;

		client->cl_rdbuf = cq_new();
		client->cl_wrbuf = cq_new();

		ev_io_init(&client->cl_readable, client_read, client->cl_fd, EV_READ);
		client->cl_readable.data = client;

		ev_io_init(&client->cl_writable, client_write, client->cl_fd, EV_WRITE);
		client->cl_writable.data = client;

		ev_io_start(loop, &client->cl_readable);
		client_printf(client, "200 nntpsink ready.\r\n");
	}

	if (!ignore_errno(errno)) {
		fprintf(stderr, "accept: %s", strerror(errno));
		exit(1);
	}
}

void
client_write(loop, w, revents)
	struct ev_loop	*loop;
	ev_io		*w;
{
client_t	*cl = w->data;

	client_flush(cl);
}

void
client_destroy(cl)
	client_t	*cl;
{
	close(cl->cl_fd);
	cq_free(cl->cl_rdbuf);
	cq_free(cl->cl_wrbuf);
	free(cl->cl_msgid);
	free(cl);
}

void
client_flush(cl)
	client_t	*cl;
{
	if (cl->cl_flags & CL_DEAD)
		return;

	if (cq_write(cl->cl_wrbuf, cl->cl_fd) < 0) {
		if (ignore_errno(errno)) {
			ev_io_start(loop, &cl->cl_writable);
			return;
		}

		printf("[%d] write error: %s\n",
			cl->cl_fd, strerror(errno));
		client_close(cl);
		return;
	}

	ev_io_stop(loop, &cl->cl_writable);
}

void
client_close(cl)
	client_t	*cl;
{
	ev_io_stop(loop, &cl->cl_writable);
	ev_io_stop(loop, &cl->cl_readable);
	cl->cl_flags |= CL_DEAD;
	cl->cl_next = deadlist;
	deadlist = cl;
}

void
client_vprintf(client_t *cl, char const *fmt, va_list ap)
{
char	line[1024];
int	n;
	n = vsnprintf(line, sizeof(line), fmt, ap);
	cq_append(cl->cl_wrbuf, line, n);
	client_flush(cl);
}

void
client_printf(client_t *cl, char const *fmt, ...)
{
va_list	ap;
	va_start(ap, fmt);
	client_vprintf(cl, fmt, ap);
	va_end(ap);
}

void
client_read(loop, w, revents)
	struct ev_loop	*loop;
	ev_io		*w;
{
client_t	*cl = w->data;
	for (;;) {
	char	*ln;
	ssize_t	 n;

		if ((n = cq_read(cl->cl_rdbuf, cl->cl_fd)) == -1) {
			if (ignore_errno(errno))
				return;
			printf("[%d] read error: %s\n",
				cl->cl_fd, strerror(errno));
		}

		while (ln = cq_read_line(cl->cl_rdbuf)) {
		char	*cmd, *data;

			if (debug)
				printf("[%d] <- [%s]\n", cl->cl_fd, ln);

			/*
			 * 238 <msg-id> -- CHECK, send the article
			 * 431 <msg-id> -- CHECK, defer the article
			 * 438 <msg-id> -- CHECK, never send the article
			 * 239 <msg-id> -- TAKETHIS, accepted
			 * 439 <msg-id> -- TAKETHIS, rejected
			 * 335 <msg-id> -- IHAVE, send the article
			 * 435 <msg-id> -- IHAVE, never send the article
			 * 436 <msg-id> -- IHAVE, defer the article
			 */

			if (cl->cl_state == CL_NORMAL) {
				cmd = ln;
				if ((data = index(cmd, ' ')) != NULL) {
					*data++ = 0;
					while (isspace(*data))
						data++;
					if (!*data)
						data = NULL;
				}

				if (strcasecmp(cmd, "CAPABILITIES") == 0) {
					client_printf(cl,
						"101 Capability list:\r\n"
						"VERSION 2\r\n"
						"IMPLEMENTATION nntpsink %s\r\n", PACKAGE_VERSION);
					if (do_ihave)
						client_printf(cl, "IHAVE\r\n");
					if (do_streaming)
						client_printf(cl, "STREAMING\r\n");
					client_printf(cl, ".\r\n");
				} else if (strcasecmp(cmd, "QUIT") == 0) {
					client_close(cl);
				} else if (strcasecmp(cmd, "MODE") == 0) {
					if (!data || strcasecmp(data, "STREAM"))
						client_printf(cl, "501 Unknown MODE.\r\n");
					else if (!do_streaming)
						client_printf(cl, "501 Unknown MODE.\r\n");
					else
						client_printf(cl, "203 Streaming OK.\r\n");
				} else if (strcasecmp(cmd, "CHECK") == 0) {
					if (!do_streaming)
						client_printf(cl, "500 Unknown command.\r\n");
					else if (!data)
						client_printf(cl, "501 Missing message-id.\r\n");
					else
						client_printf(cl, "238 %s\r\n", data);
				} else if (strcasecmp(cmd, "TAKETHIS") == 0) {
					if (!do_streaming)
						client_printf(cl, "500 Unknown command.\r\n");
					else if (!data)
						client_printf(cl, "501 Missing message-id.\r\n");
					else {
						cl->cl_msgid = strdup(data);
						cl->cl_state = CL_TAKETHIS;
					}
				} else if (strcasecmp(cmd, "IHAVE") == 0) {
					if (!do_ihave)
						client_printf(cl, "500 Unknown command.\r\n");
					else if (!data)
						client_printf(cl, "501 Missing message-id.\r\n");
					else {
						client_printf(cl, "335 %s\r\n", data);
						cl->cl_msgid = strdup(data);
						cl->cl_state = CL_IHAVE;
					}
				} else {
					client_printf(cl, "500 Unknown command.\r\n");
				}
			} else if (cl->cl_state == CL_TAKETHIS || cl->cl_state == CL_IHAVE) {
				if (strcmp(ln, ".") == 0) {
					client_printf(cl, "%d %s\r\n",
						cl->cl_state == CL_IHAVE ? 235 : 239,
						cl->cl_msgid);
					free(cl->cl_msgid);
					cl->cl_msgid = NULL;
					cl->cl_state = CL_NORMAL;
				}
			}

			free(ln);
			if (cl->cl_flags & CL_DEAD)
				return;
		}
	}
}

void *
xmalloc(sz)
	size_t	sz;
{
void	*ret = malloc(sz);
	if (!ret) {
		fprintf(stderr, "out of memory\n");
		_exit(1);
	}

	return ret;
}

void *
xcalloc(n, sz)
	size_t	n, sz;
{
void	*ret = calloc(n, sz);
	if (!ret) {
		fprintf(stderr, "out of memory\n");
		_exit(1);
	}

	return ret;
}

void
do_stats(loop, w, revents)
	struct ev_loop	*loop;
	ev_timer	*w;
{
struct rusage	rus;
uint64_t	ct;
time_t		upt = time(NULL) - start_time;

	getrusage(RUSAGE_SELF, &rus);
	ct = (rus.ru_utime.tv_sec * 1000) + (rus.ru_utime.tv_usec / 1000)
	   + (rus.ru_stime.tv_sec * 1000) + (rus.ru_stime.tv_usec / 1000);

	printf("send it: %d/s, refused: %d/s, rejected: %d/s, deferred: %d/s, accepted: %d/s, cpu %.2f%%\n",
		nsend, nrefuse, nreject, ndefer, naccept, (((double)ct / 1000) / upt) * 100);
	nsend = nrefuse = nreject = ndefer = naccept = 0;
}

void
do_housekeeping(loop, w, revents)
	struct ev_loop	*loop;
	ev_prepare	*w;
{
client_t	*cl, *next;
	cl = deadlist;
	while (cl) {
		next = cl->cl_next;
		client_destroy(cl);
		cl = next;
	}
	deadlist = NULL;
}

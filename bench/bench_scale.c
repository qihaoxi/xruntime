#define _GNU_SOURCE
#include "xr/xr_env.h"
#include "xr/xr_log.h"
#include "xr/xr_time.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * U12:多 loop accept 分发原型(无迁移)。
 *  --dist=reuseport:每 worker 自持 SO_REUSEPORT listener,内核分发 accept;
 *  --dist=dispatch :单 acceptor(worker0)accept 后 round-robin 投给其他
 *                   worker(事件面跨 loop 交接,测 handoff 延迟)。
 * 连接从 accept 起固定在同一 worker(连接=loop 归属,不迁移)。
 * 客户端用 PEL loadgen(echo,原样回显),可选内嵌 fork/exec。
 *
 *   bench_scale --workers K --dist reuseport|dispatch --port P
 *               [--duration S] [--basecpu C] [--loadgen PATH]
 *               [--conns C] [--threads T] [--payload N] [--warmup S]
 */

#define SC_MAX_FD 65536
#define SC_BUF 4096
#define SC_QCAP 8192

typedef struct
{
	int fd;
	int rlen; /* 待回显字节(0..SC_BUF) */
	int woff;
	char buf[SC_BUF];
} sc_conn_t;

typedef struct
{
	int fd;
	uint64_t t0;
} sc_qe_t;

typedef struct sc_scale sc_scale_t;

typedef struct sc_worker
{
	sc_scale_t *s;
	int id;
	int epfd;
	int listener; /* -1 = 无 */
	int wakefd;
	int tag_listener;
	int tag_wake;
	pthread_t thread;
	sc_conn_t *conns;
	pthread_mutex_t qlock;
	sc_qe_t q[SC_QCAP];
	int qh, qt, qn;
	_Atomic int stop;
	_Atomic uint64_t accepted;
	_Atomic uint64_t reqs;
	_Atomic uint64_t bytes;
	_Atomic uint64_t dsp_ns;
	_Atomic uint64_t dsp_n;
	_Atomic uint64_t dsp_max;
} sc_worker_t;

struct sc_scale
{
	int n;
	int reuseport;
	int port;
	int duration_s;
	int basecpu;
	const char *loadgen;
	int conns;
	int threads;
	int payload;
	int warmup;
	sc_worker_t *ws;
	_Atomic int rr;
};

static int sc_listen(int port, int reuseport)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct sockaddr_in sa;
	int one = 1;

	if (fd < 0)
	{
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (reuseport != 0)
	{
		setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    listen(fd, 4096) != 0)
	{
		close(fd);
		return -1;
	}
	return fd;
}

static void sc_conn_close(sc_worker_t *w, sc_conn_t *c)
{
	if (c->fd < 0)
	{
		return;
	}
	epoll_ctl(w->epfd, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	c->fd = -1;
}

static void sc_add_conn(sc_worker_t *w, int fd)
{
	sc_conn_t *c = &w->conns[fd];
	struct epoll_event ev;

	if (fd >= SC_MAX_FD)
	{
		close(fd);
		return;
	}
	if (c->fd >= 0)
	{
		close(fd); /* 复用竞态防御 */
		return;
	}
	c->fd = fd;
	c->rlen = 0;
	c->woff = 0;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLRDHUP;
	ev.data.ptr = c;
	if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &ev) != 0)
	{
		close(fd);
		c->fd = -1;
		return;
	}
	atomic_fetch_add_explicit(&w->accepted, 1, memory_order_relaxed);
}

static void sc_flush(sc_worker_t *w, sc_conn_t *c)
{
	while (c->rlen > 0)
	{
		ssize_t k = write(c->fd, c->buf + c->woff, (size_t)c->rlen);

		if (k > 0)
		{
			c->woff += (int)k;
			c->rlen -= (int)k;
			continue;
		}
		if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		{
			struct epoll_event ev;

			memset(&ev, 0, sizeof(ev));
			ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
			ev.data.ptr = c;
			epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &ev);
			return;
		}
		sc_conn_close(w, c);
		return;
	}
	c->woff = 0;
	atomic_fetch_add_explicit(&w->reqs, 1, memory_order_relaxed);
	{
		struct epoll_event ev;

		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN | EPOLLRDHUP;
		ev.data.ptr = c;
		epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &ev);
	}
}

static void sc_conn_readable(sc_worker_t *w, sc_conn_t *c)
{
	char tmp[SC_BUF];

	for (;;)
	{
		ssize_t n = read(c->fd, tmp, sizeof(tmp));

		if (n > 0)
		{
			if (c->rlen + (int)n > SC_BUF)
			{
				sc_conn_close(w, c);
				return;
			}
			memcpy(c->buf + c->rlen, tmp, (size_t)n);
			c->rlen += (int)n;
			atomic_fetch_add_explicit(&w->bytes, (uint64_t)n,
						  memory_order_relaxed);
			sc_flush(w, c);
			if (c->fd < 0)
			{
				return;
			}
			continue;
		}
		if (n == 0)
		{
			sc_conn_close(w, c);
			return;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK)
		{
			return;
		}
		sc_conn_close(w, c);
		return;
	}
}

static void sc_accept_all(sc_worker_t *w, sc_scale_t *s)
{
	for (;;)
	{
		int fd = accept4(w->listener, NULL, NULL,
				 SOCK_NONBLOCK | SOCK_CLOEXEC);

		if (fd < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				return;
			}
			return;
		}
		{
			int one = 1;

			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one,
				   sizeof(one));
		}
		if (s->reuseport != 0)
		{
			sc_add_conn(w, fd);
		}
		else
		{
			/* 单 acceptor:round-robin 投给目标 worker */
			int tgt = atomic_fetch_add_explicit(&s->rr, 1,
							    memory_order_relaxed) %
				  s->n;
			sc_worker_t *tw = &s->ws[tgt];
			sc_qe_t qe;

			if (tw == w)
			{
				sc_add_conn(w, fd);
				continue;
			}
			qe.fd = fd;
			qe.t0 = xr_tsc();
			pthread_mutex_lock(&tw->qlock);
			if (tw->qn < SC_QCAP)
			{
				tw->q[tw->qt] = qe;
				tw->qt = (tw->qt + 1) % SC_QCAP;
				tw->qn++;
				pthread_mutex_unlock(&tw->qlock);
			}
			else
			{
				pthread_mutex_unlock(&tw->qlock);
				close(fd);
				continue;
			}
			{
				uint64_t one = 1;

				ssize_t r = write(tw->wakefd, &one,
						  sizeof(one));
				(void)r;
			}
		}
	}
}

static void *sc_worker_main(void *arg)
{
	sc_worker_t *w = arg;
	sc_scale_t *s = w->s;
	struct epoll_event evs[256];

	while (atomic_load_explicit(&w->stop, memory_order_acquire) == 0)
	{
		int nev = epoll_wait(w->epfd, evs, 256, -1);

		for (int e = 0; e < nev; e++)
		{
			void *p = evs[e].data.ptr;

			if (p == &w->tag_listener)
			{
				sc_accept_all(w, s);
			}
			else if (p == &w->tag_wake)
			{
				uint64_t v;
				ssize_t r = read(w->wakefd, &v, sizeof(v));
				(void)r;
				for (;;)
				{
					sc_qe_t qe;
					int got = 0;

					pthread_mutex_lock(&w->qlock);
					if (w->qn > 0)
					{
						qe = w->q[w->qh];
						w->qh = (w->qh + 1) % SC_QCAP;
						w->qn--;
						got = 1;
					}
					pthread_mutex_unlock(&w->qlock);
					if (got == 0)
					{
						break;
					}
					sc_add_conn(w, qe.fd);
					{
						uint64_t d = xr_tsc() - qe.t0;

						atomic_fetch_add_explicit(
							&w->dsp_ns, d,
							memory_order_relaxed);
						atomic_fetch_add_explicit(
							&w->dsp_n, 1,
							memory_order_relaxed);
						if (d > atomic_load_explicit(
								&w->dsp_max,
								memory_order_relaxed))
						{
							atomic_store_explicit(
								&w->dsp_max, d,
								memory_order_relaxed);
						}
					}
				}
			}
			else
			{
				sc_conn_t *c = p;

				if ((evs[e].events & EPOLLOUT) != 0)
				{
					sc_flush(w, c);
				}
				if (c->fd >= 0 &&
				    (evs[e].events & (EPOLLIN | EPOLLRDHUP)) != 0)
				{
					sc_conn_readable(w, c);
				}
			}
		}
	}
	/* cleanup */
	for (int fd = 0; fd < SC_MAX_FD; fd++)
	{
		if (w->conns[fd].fd >= 0)
		{
			sc_conn_close(w, &w->conns[fd]);
		}
	}
	return NULL;
}

static void *sc_worker_trampoline(void *arg)
{
	sc_worker_t *w = arg;

	if (w->s->basecpu >= 0)
	{
		int cpu = w->s->basecpu + w->id;

		if (cpu < xr_cpu_count())
		{
			(void)xr_pin_to_cpu(cpu);
		}
	}
	return sc_worker_main(w);
}

static void sc_spawn_loadgen(sc_scale_t *s)
{
	pid_t pid;
	char port[16];
	char conns[16];
	char threads[16];
	char payload[16];
	char warmup[16];
	char duration[16];

	if (s->loadgen == NULL)
	{
		return;
	}
	snprintf(port, sizeof(port), "%d", s->port);
	snprintf(conns, sizeof(conns), "%d", s->conns);
	snprintf(threads, sizeof(threads), "%d", s->threads);
	snprintf(payload, sizeof(payload), "%d", s->payload);
	snprintf(warmup, sizeof(warmup), "%d", s->warmup);
	snprintf(duration, sizeof(duration), "%d", s->duration_s);
	pid = fork();
	if (pid == 0)
	{
		execl(s->loadgen, s->loadgen, "echo", "--host", "127.0.0.1",
		      "--port", port, "--concurrency", conns, "--threads",
		      threads, "--payload", payload, "--warmup", warmup,
		      "--duration", duration, "--subject", "xr-scale",
		      (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
	{
		int st;

		(void)waitpid(pid, &st, 0);
	}
}

int main(int argc, char **argv)
{
	sc_scale_t s;
	uint64_t t0;

	memset(&s, 0, sizeof(s));
	s.n = 1;
	s.reuseport = 1;
	s.port = 23100;
	s.duration_s = 5;
	s.basecpu = 2;
	s.conns = 256;
	s.threads = 4;
	s.payload = 1024;
	s.warmup = 1;
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc)
		{
			s.n = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--dist") == 0 && i + 1 < argc)
		{
			s.reuseport = strcmp(argv[++i], "dispatch") != 0;
		}
		else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
		{
			s.port = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
		{
			s.duration_s = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--basecpu") == 0 && i + 1 < argc)
		{
			s.basecpu = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--loadgen") == 0 && i + 1 < argc)
		{
			s.loadgen = argv[++i];
		}
		else if (strcmp(argv[i], "--conns") == 0 && i + 1 < argc)
		{
			s.conns = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
		{
			s.threads = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc)
		{
			s.payload = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc)
		{
			s.warmup = atoi(argv[++i]);
		}
	}
	xr_time_init();
	s.ws = calloc((size_t)s.n, sizeof(*s.ws));
	if (s.ws == NULL)
	{
		return 1;
	}
	for (int i = 0; i < s.n; i++)
	{
		sc_worker_t *w = &s.ws[i];
		struct epoll_event ev;

		w->s = &s;
		w->id = i;
		w->listener = -1;
		pthread_mutex_init(&w->qlock, NULL);
		w->conns = calloc(SC_MAX_FD, sizeof(*w->conns));
		if (w->conns == NULL)
		{
			XR_LOGE("conns oom");
			return 1;
		}
		for (int fd = 0; fd < SC_MAX_FD; fd++)
		{
			w->conns[fd].fd = -1;
		}
		w->epfd = epoll_create1(EPOLL_CLOEXEC);
		w->wakefd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (w->epfd < 0 || w->wakefd < 0)
		{
			XR_LOGE("epoll/eventfd failed");
			return 1;
		}
		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN;
		ev.data.ptr = &w->tag_wake;
		epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->wakefd, &ev);

		if (s.reuseport != 0 || i == 0)
		{
			w->listener = sc_listen(s.port, s.reuseport);
			if (w->listener < 0)
			{
				XR_LOGE("listen %d failed: %s", s.port,
					strerror(errno));
				return 1;
			}
			memset(&ev, 0, sizeof(ev));
			ev.events = EPOLLIN;
			ev.data.ptr = &w->tag_listener;
			epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->listener, &ev);
		}
	}
	for (int i = 0; i < s.n; i++)
	{
		if (pthread_create(&s.ws[i].thread, NULL, sc_worker_trampoline,
				   &s.ws[i]) != 0)
		{
			XR_LOGE("pthread_create failed");
			return 1;
		}
	}
	printf("bench_scale: workers=%d dist=%s port=%d conns=%d payload=%d\n",
	       s.n, s.reuseport != 0 ? "reuseport" : "dispatch", s.port,
	       s.conns, s.payload);
	fflush(stdout);
	t0 = xr_now_ns();
	sc_spawn_loadgen(&s);
	if (s.loadgen == NULL)
	{
		usleep((useconds_t)s.duration_s * 1000000u);
	}
	for (int i = 0; i < s.n; i++)
	{
		atomic_store_explicit(&s.ws[i].stop, 1, memory_order_release);
		{
			uint64_t one = 1;

			ssize_t r = write(s.ws[i].wakefd, &one, sizeof(one));
			(void)r;
		}
	}
	for (int i = 0; i < s.n; i++)
	{
		pthread_join(s.ws[i].thread, NULL);
	}
	{
		uint64_t wall_ns = xr_now_ns() - t0;
		uint64_t acc = 0;
		uint64_t reqs = 0;
		uint64_t bytes = 0;
		uint64_t dsp_ns = 0;
		uint64_t dsp_n = 0;
		uint64_t dsp_max = 0;

		for (int i = 0; i < s.n; i++)
		{
			acc += atomic_load(&s.ws[i].accepted);
			reqs += atomic_load(&s.ws[i].reqs);
			bytes += atomic_load(&s.ws[i].bytes);
			dsp_ns += atomic_load(&s.ws[i].dsp_ns);
			dsp_n += atomic_load(&s.ws[i].dsp_n);
			if (atomic_load(&s.ws[i].dsp_max) > dsp_max)
			{
				dsp_max = atomic_load(&s.ws[i].dsp_max);
			}
		}
		printf("  server: accepted=%" PRIu64 " reqs=%" PRIu64
		       " bytes=%" PRIu64 " wall=%.2fs rps=%.0f\n",
		       acc, reqs, bytes, (double)wall_ns / 1e9,
		       (double)reqs / ((double)wall_ns / 1e9));
		printf("  per-worker accepted/reqs:");
		for (int i = 0; i < s.n; i++)
		{
			printf(" w%d=%" PRIu64 "/%" PRIu64, i,
			       atomic_load(&s.ws[i].accepted),
			       atomic_load(&s.ws[i].reqs));
		}
		printf("\n");
		if (dsp_n > 0)
		{
			printf("  dispatch handoff: avg=%.1fns max=%.1fns n=%" PRIu64
			       "\n",
			       (double)xr_tsc_to_ns(dsp_ns) / (double)dsp_n,
			       (double)xr_tsc_to_ns(dsp_max), dsp_n);
		}
	}
	for (int i = 0; i < s.n; i++)
	{
		if (s.ws[i].listener >= 0)
		{
			close(s.ws[i].listener);
		}
		close(s.ws[i].wakefd);
		close(s.ws[i].epfd);
		pthread_mutex_destroy(&s.ws[i].qlock);
		free(s.ws[i].conns);
	}
	free(s.ws);
	return 0;
}

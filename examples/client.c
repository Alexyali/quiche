// Copyright (C) 2018-2019, Cloudflare, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#define _GNU_SOURCE

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include <fcntl.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <ev.h>

#include <quiche.h>

#include <termios.h>

#define LOCAL_CONN_ID_LEN 16

#define MAX_DATAGRAM_SIZE 1350

struct conn_io {
    ev_timer timer;
    ev_timer keyboard_timer;

    int sock;

    quiche_conn *conn;
};

static int vpipe_fd, apipe_fd;

static void debug_log(const char *line, void *argp) {
    fprintf(stderr, "%s\n", line);
}

static struct termios initial_settings, new_settings;
static int peek_character = -1;

void init_keyboard() {
    tcgetattr(0,&initial_settings);
    new_settings = initial_settings;
    new_settings.c_lflag &= ~ICANON;
    new_settings.c_lflag &= ~ECHO;
    new_settings.c_lflag &= ~ISIG;
    new_settings.c_cc[VMIN] = 1;
    new_settings.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &new_settings);
}

void close_keyboard() {
    tcsetattr(0, TCSANOW, &initial_settings);
}

int kbhit() {
    unsigned char ch;
    int nread;

    if (peek_character != -1) return 1;
    new_settings.c_cc[VMIN]=0;
    tcsetattr(0, TCSANOW, &new_settings);
    nread = read(0,&ch,1);
    new_settings.c_cc[VMIN]=1;
    tcsetattr(0, TCSANOW, &new_settings);
    if(nread == 1)
    {
        peek_character = ch;
        return 1;
    }
    return 0;
}

int readch() {
    char ch;

    if(peek_character != -1)
    {
        ch = peek_character;
        peek_character = -1;
        return ch;
    }
    read(0,&ch,1);
    return ch;
}

static void flush_egress(struct ev_loop *loop, struct conn_io *conn_io) {
    static uint8_t out[MAX_DATAGRAM_SIZE];

    while (1) {
        ssize_t written = quiche_conn_send(conn_io->conn, out, sizeof(out));

        if (written == QUICHE_ERR_DONE) {
            fprintf(stderr, "done writing\n");
            break;
        }

        if (written < 0) {
            fprintf(stderr, "failed to create packet: %zd\n", written);
            return;
        }

        ssize_t sent = send(conn_io->sock, out, written, 0);
        if (sent != written) {
            perror("failed to send");
            return;
        }

        fprintf(stderr, "sent %zd bytes\n", sent);
    }

    // double t = quiche_conn_timeout_as_nanos(conn_io->conn) / 1e9f;
    conn_io->timer.repeat = 5;//t;
    ev_timer_again(loop, &conn_io->timer);
}

static void keyboard_cb(EV_P_ ev_timer* w, int revents) {
    struct conn_io *conn_io = w->data;
    char ch;
    if (quiche_conn_is_established(conn_io->conn) && kbhit()) {
        ch = readch();
        fprintf(stderr, "********%c********\n", ch);
        if (ch >= 'a' && ch <= 'z') {
            char r[100];
            sprintf(r,  "*message %c from client to server*", ch);
            if (quiche_conn_stream_send(conn_io->conn, 12, (uint8_t *)r, sizeof(r), false) < 0) {
                fprintf(stderr, "Failed to send keyboard event message.\n");
                return;
            }
            flush_egress(loop, conn_io);
        }
    }
}

static void recv_cb(EV_P_ ev_io *w, int revents) {
    static bool req_sent = false;

    struct conn_io *conn_io = w->data;

    static uint8_t buf[65535];

    while (1) {
        ssize_t read = recv(conn_io->sock, buf, sizeof(buf), 0);

        if (read < 0) {
            if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
                fprintf(stderr, "recv would block\n");
                break;
            }

            perror("failed to read");
            return;
        }

        ssize_t done = quiche_conn_recv(conn_io->conn, buf, read);

        if (done == QUICHE_ERR_DONE) {
            fprintf(stderr, "done reading\n");
            break;
        }

        if (done < 0) {
            fprintf(stderr, "failed to process packet\n");
            return;
        }

        fprintf(stderr, "recv %zd bytes\n", done);
    }

    if (quiche_conn_is_closed(conn_io->conn)) {
        fprintf(stderr, "connection closed\n");

        ev_break(EV_A_ EVBREAK_ONE);
        return;
    }

    if (quiche_conn_is_established(conn_io->conn) && !req_sent) {
        const uint8_t *app_proto;
        size_t app_proto_len;

        quiche_conn_application_proto(conn_io->conn, &app_proto, &app_proto_len);

        fprintf(stderr, "connection established: %.*s\n",
                (int) app_proto_len, app_proto);

        const static uint8_t r[] = "GET /index.html\r\n";
        if (quiche_conn_stream_send(conn_io->conn, 4, r, sizeof(r), true) < 0) {
            fprintf(stderr, "failed to send HTTP request\n");
            return;
        }
        if (quiche_conn_stream_send(conn_io->conn, 8, r, sizeof(r), true) < 0) {
            fprintf(stderr, "failed to send HTTP request\n");
            return;
        }

        fprintf(stderr, "sent HTTP request\n");

        req_sent = true;

        conn_io->keyboard_timer.repeat = 0.1;
        ev_timer_again(loop, &conn_io->keyboard_timer);
    }

    if (quiche_conn_is_established(conn_io->conn)) {
        uint64_t s = 0;

        quiche_stream_iter *readable = quiche_conn_readable(conn_io->conn);

        while (quiche_stream_iter_next(readable, &s)) {
            fprintf(stderr, "stream %" PRIu64 " is readable\n", s);

            bool fin = false;
            ssize_t recv_len = quiche_conn_stream_recv(conn_io->conn, s,
                                                       buf, sizeof(buf),
                                                       &fin);
            if (recv_len < 0) {
                break;
            }

            if (s == 4) {
                write(vpipe_fd, buf, recv_len);
            } else if (s == 8) {
                write(apipe_fd, buf, recv_len);
            }

            if (fin) {
                if (quiche_conn_close(conn_io->conn, true, 0, NULL, 0) < 0) {
                    fprintf(stderr, "failed to close connection\n");
                }
            }
        }

        quiche_stream_iter_free(readable);
    }

    flush_egress(loop, conn_io);
}

static void timeout_cb(EV_P_ ev_timer *w, int revents) {
    struct conn_io *conn_io = w->data;
    quiche_conn_on_timeout(conn_io->conn);

    fprintf(stderr, "timeout\n");

    flush_egress(loop, conn_io);

    if (quiche_conn_is_closed(conn_io->conn)) {
        quiche_stats stats;

        quiche_conn_stats(conn_io->conn, &stats);

        fprintf(stderr, "connection closed, recv=%zu sent=%zu lost=%zu rtt=%" PRIu64 "ns\n",
                stats.recv, stats.sent, stats.lost, stats.rtt);

        ev_break(EV_A_ EVBREAK_ONE);
        return;
    }
}

int main(int argc, char *argv[]) {
    const char *host = argv[1];
    const char *port = argv[2];

    init_keyboard();
    vpipe_fd = open("cvideopipe", O_WRONLY | O_NONBLOCK);
    apipe_fd = open("caudiopipe", O_WRONLY | O_NONBLOCK);
    fcntl(vpipe_fd, F_SETPIPE_SZ, 12582912);
    fcntl(apipe_fd, F_SETPIPE_SZ, 12582912);

    const struct addrinfo hints = {
        .ai_family = PF_UNSPEC,
        .ai_socktype = SOCK_DGRAM,
        .ai_protocol = IPPROTO_UDP
    };

    quiche_enable_debug_logging(debug_log, NULL);

    struct addrinfo *peer;
    if (getaddrinfo(host, port, &hints, &peer) != 0) {
        perror("failed to resolve host");
        return -1;
    }

    int sock = socket(peer->ai_family, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("failed to create socket");
        return -1;
    }

    if (fcntl(sock, F_SETFL, O_NONBLOCK) != 0) {
        perror("failed to make socket non-blocking");
        return -1;
    }

    if (connect(sock, peer->ai_addr, peer->ai_addrlen) < 0) {
        perror("failed to connect socket");
        return -1;
    }

    quiche_config *config = quiche_config_new(0xbabababa);
    if (config == NULL) {
        fprintf(stderr, "failed to create config\n");
        return -1;
    }

    quiche_config_set_application_protos(config,
        (uint8_t *) "\x05hq-27\x05hq-25\x05hq-24\x05hq-23\x08http/0.9", 15);

    quiche_config_load_cert_chain_from_pem_file(config, "./cert.crt");
    quiche_config_set_max_idle_timeout(config, 5000);
    quiche_config_set_max_packet_size(config, MAX_DATAGRAM_SIZE);
    quiche_config_set_initial_max_data(config, 10000000);
    quiche_config_set_initial_max_stream_data_bidi_local(config, 1000000);
    quiche_config_set_initial_max_stream_data_uni(config, 1000000);
    quiche_config_set_initial_max_streams_bidi(config, 100);
    quiche_config_set_initial_max_streams_uni(config, 100);
    quiche_config_set_disable_active_migration(config, true);
    quiche_config_set_cc_algorithm(config, QUICHE_CC_RENO);

    if (getenv("SSLKEYLOGFILE")) {
      quiche_config_log_keys(config);
    }

    uint8_t scid[LOCAL_CONN_ID_LEN];
    int rng = open("/dev/urandom", O_RDONLY);
    if (rng < 0) {
        perror("failed to open /dev/urandom");
        return -1;
    }

    ssize_t rand_len = read(rng, &scid, sizeof(scid));
    if (rand_len < 0) {
        perror("failed to create connection ID");
        return -1;
    }

    quiche_conn *conn = quiche_connect(host, (const uint8_t *) scid,
                                       sizeof(scid), config);
    if (conn == NULL) {
        fprintf(stderr, "failed to create connection\n");
        return -1;
    }

    struct conn_io *conn_io = malloc(sizeof(*conn_io));
    if (conn_io == NULL) {
        fprintf(stderr, "failed to allocate connection IO\n");
        return -1;
    }

    conn_io->sock = sock;
    conn_io->conn = conn;

    ev_io watcher;

    struct ev_loop *loop = ev_default_loop(0);

    ev_io_init(&watcher, recv_cb, conn_io->sock, EV_READ);
    ev_io_start(loop, &watcher);
    watcher.data = conn_io;

    ev_init(&conn_io->timer, timeout_cb);
    conn_io->timer.data = conn_io;

    ev_init(&conn_io->keyboard_timer, keyboard_cb);
	conn_io->keyboard_timer.data = conn_io;

    flush_egress(loop, conn_io);

    ev_loop(loop, 0);

    freeaddrinfo(peer);

    quiche_conn_free(conn);

    quiche_config_free(config);

    close(vpipe_fd);
    close(apipe_fd);
    close_keyboard();
    return 0;
}

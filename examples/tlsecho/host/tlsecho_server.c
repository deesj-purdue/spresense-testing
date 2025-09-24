/****************************************************************************
 * examples/tlsecho/host/tlsecho_server.c
 *
 *   Copyright 2025 Sony Semiconductor Solutions Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of Sony Semiconductor Solutions Corporation nor
 *    the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MAX_EVENTS 64
#define BUF_SIZE 1024

/****************************************************************************
 * Private Data Type
 ****************************************************************************/

struct client_s {
  int fd;
  SSL *ssl;
  int handshake_done;
};

typedef struct client_s client_t;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char **argv)
{
  int i;
  int ret;
  int err;
  int port;
  const char *cert_file;
  const char *key_file;
  SSL_CTX *ctx;
  int server_fd, epfd;
  int sockopt_one = 1;
  struct sockaddr_in addr;
  struct epoll_event ev, events[MAX_EVENTS];
  int nfds;
  int client_fd;
  SSL *ssl;
  client_t *newone;
  char buf[BUF_SIZE];

  if (argc < 4)
    {
      fprintf(stderr, "Usage: $ %s <port> <cert.pem> <key.pem>\n", argv[0]);
      return 1;
    }

  port      = atoi(argv[1]);
  cert_file = argv[2];
  key_file  = argv[3];

  /* Initialize OpenSSL */

  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms();

  ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx)
    {
      ERR_print_errors_fp(stderr);
      return -1;
    }

  /* Setup Server certificate file and private key */

  if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) <= 0 ||
      SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0)
    {
      ERR_print_errors_fp(stderr);
      return -1;
    }

  /* Create socket */

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &sockopt_one, sizeof(sockopt_one));

  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
      perror("bind");
      return -1;
    }

  if (listen(server_fd, SOMAXCONN) < 0)
    {
      perror("listen");
      return -1;
    }

  /* Create epoll */

  epfd = epoll_create1(0);
  if (epfd < 0)
    {
      perror("epoll_create1");
      return -1;
    }

  ev.events = EPOLLIN;
  ev.data.fd = server_fd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

  printf("TLS Echo Server listening on port %d\n", port);

  while (1)
    {
      /* Wait an event */

      nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
      if (nfds < 0)
        {
          perror("epoll_wait");
          continue;
        }

      for (i = 0; i < nfds; i++)
        {
          if (events[i].data.fd == server_fd)
            {
              /* New connection from a client */

              client_fd = accept(server_fd, NULL, NULL);
              if (client_fd < 0)
                {
                  continue;
                }

              ssl = SSL_new(ctx);
              SSL_set_fd(ssl, client_fd);

              newone = malloc(sizeof(client_t));
              newone->fd = client_fd;
              newone->ssl = ssl;
              newone->handshake_done = 0;

              ev.events = EPOLLIN | EPOLLET;
              ev.data.ptr = newone;
              epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

              printf("New client fd=%d\n", client_fd);
            }
          else /* of if (events[i].data.fd == server_fd) */
            {
              newone = (client_t*)events[i].data.ptr;

              if (!newone->handshake_done)
                {
                  /* TLS Handshake */

                  ret = SSL_accept(newone->ssl);
                  if (ret <= 0)
                    {
                      err = SSL_get_error(newone->ssl, ret);
                      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                        {
                          continue;
                        }
                      ERR_print_errors_fp(stderr);
                      goto cleanup;
                    }
                  newone->handshake_done = 1;
                  printf("Handshake done fd=%d cipher=%s\n",
                         newone->fd, SSL_get_cipher(newone->ssl));
                }
              else  /* of if (!newone->handshake_done) */
                {
                  /* Receive data from the client and echo back it */

                  ret = SSL_read(newone->ssl, buf, sizeof(buf));
                  if (ret > 0)
                    {
                      buf[ret] = '\0';
                      printf("fd=%d Received: %s\n", newone->fd, buf);
                      SSL_write(newone->ssl, buf, ret);
                    }
                  else
                    {
                      goto cleanup;
                    }
                } /* end of else of if (!newone->handshake_done) */
              continue;

              cleanup:
                epoll_ctl(epfd, EPOLL_CTL_DEL, newone->fd, NULL);
                SSL_shutdown(newone->ssl);
                SSL_free(newone->ssl);
                close(newone->fd);
                free(newone);
                printf("Client closed\n");
            } /* "else" of if (events[i].data.fd == server_fd) */
        } /* for (int i = 0; i < nfds; i++) */
    } /* while (1) */

  close(server_fd);
  close(epfd);
  SSL_CTX_free(ctx);
  EVP_cleanup();
  return 0;
}

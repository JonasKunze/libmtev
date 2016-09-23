/*
 * Copyright (c) 2016, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name Circonus, Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "mtev_cluster.h"
#include "eventer/eventer.h"
#include "mtev_cluster_messaging.h"
#include "mtev_listener.h"

#include <errno.h>

static int
async_read_next_message(eventer_t e, mtev_cluster_messaging_received_func_t callback);

typedef struct {
  uint32_t length_including_header;
} msg_hdr_t;

typedef struct {
  char* outbuff;
  uint send_size;
  uint write_sofar;
  data_free_fn *data_free;
} request_ctx_t;

typedef struct {
  msg_hdr_t msg_hdr;
  char *payload;
  uint32_t read_so_far;
} response_ctx_t;

typedef struct {
  mtev_cluster_node_t node;
  request_ctx_t request;
  response_ctx_t response;
  mtev_cluster_messaging_received_func_t response_callback;
  void* closure;
} connection_ctx_t;

//static void
//response_ctx_free(void* data) {
//  response_ctx_t *ctx = data;
//  if(ctx->payload) free(ctx->payload);
//  free(ctx);
//}

static msg_hdr_t
msg_hdr_create(uint32_t payload_length) {
  msg_hdr_t hdr;
  hdr.length_including_header = htonl(payload_length + sizeof(hdr));
  return hdr;
}

static void
ntoh_msg_hdr(msg_hdr_t *hdr) {
  hdr->length_including_header = ntohl(hdr->length_including_header);
}

MTEV_HOOK_IMPL(mtev_cluster_messaging_request,
  (eventer_t e, const char *data, uint data_len),
  void *, closure,
  (void *closure, eventer_t e, const char *data, uint data_len),
  (closure,e,data,data_len))

eventer_t
mtev_cluster_messaging_connect(const mtev_cluster_node_t *node) {
  int fd, rv;
  eventer_t e;
  union {
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
  } addr;
  addr.addr6 = node->addr.addr6;
  addr.addr4.sin_port = htons(node->data_port);
  fd = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
  rv = connect(fd, (struct sockaddr*)&addr, node->address_len);
  if(rv == -1) return NULL;

  e = eventer_alloc();
  e->mask = EVENTER_READ | EVENTER_WRITE | EVENTER_EXCEPTION;
  e->fd = fd;
  eventer_add(e);

  return e;
}

void
mtev_cluster_messaging_disconnect(eventer_t connection) {
  int mask;
  if(connection->closure) {
    free(connection->closure);
  }
  eventer_remove_fd(connection->fd);
  connection->opset->close(connection->fd, &mask, connection);
  eventer_free(connection);
}

static int
keep_reading(eventer_t e, int mask, void *closure,
    struct timeval *now) {
  int read, bytes_expected, inbuff_offset;
  char* inbuff;
  connection_ctx_t *ctx = closure;
  response_ctx_t *response = &ctx->response;
  mtev_hook_return_t callback_result;

  bytes_expected = sizeof(msg_hdr_t);
  inbuff_offset = response->read_so_far;
  inbuff = (char*)&response->msg_hdr;
  if(response->read_so_far >= bytes_expected) {
    bytes_expected = response->msg_hdr.length_including_header;
    inbuff_offset -= sizeof(msg_hdr_t);
    inbuff = response->payload;
  }

  while(1) {
    read = e->opset->read(e->fd, inbuff + inbuff_offset, bytes_expected - response->read_so_far, &mask, e);
    if(read == -1 && errno == EAGAIN) {
      return mask;
    }

    if(read <= 0) {
      mtev_cluster_messaging_disconnect(e);
      return 0;
    }
    if(read > 0) {
      response->read_so_far += read;
      inbuff_offset += read;
      if(response->read_so_far == sizeof(msg_hdr_t)) {
        // header is complete
        ntoh_msg_hdr(&response->msg_hdr);
        response->payload = malloc(response->msg_hdr.length_including_header - sizeof(msg_hdr_t));
        bytes_expected = response->msg_hdr.length_including_header;
        inbuff = response->payload;
        inbuff_offset = 0;
      } else if(response->read_so_far == bytes_expected) {
        // message is complete
        callback_result = ctx->response_callback(ctx->closure, e, response->payload, response->msg_hdr.length_including_header - sizeof(msg_hdr_t));

        response->read_so_far = 0;
        free(response->payload);
        response->payload = NULL;

        if(callback_result == MTEV_HOOK_CONTINUE) {
          return e->mask;
        }

        return EVENTER_CANCEL;
      }
    }
  }
}

static int
async_read_next_message(eventer_t e, mtev_cluster_messaging_received_func_t callback) {
  connection_ctx_t *ctx = e->closure;
  if(ctx == NULL) {
    ctx = calloc(1, sizeof(connection_ctx_t));
    e->closure = ctx;
  }
  ctx->response_callback = callback;

  e->mask = EVENTER_READ | EVENTER_EXCEPTION;
  e->callback = keep_reading;
  return e->mask;
}

static mtev_hook_return_t
on_request_received(void *closure, eventer_t e, const void *data, uint data_len) {
  return mtev_cluster_messaging_request_hook_invoke(e, data, data_len);
}

static int
on_connection_established(eventer_t e, int mask, void *closure,
    struct timeval *now) {
  if(mask & EVENTER_EXCEPTION) {
    /* This removes the log feed which is important to do before calling close */
    eventer_remove_fd(e->fd);

    e->opset->close(e->fd, &mask, e);
    return 0;
  }

  free(e->closure);
  e->closure = NULL;

  return async_read_next_message(e, on_request_received);
}

static int
mtev_cluster_messaging_send(eventer_t e, int mask, void *closure,
    struct timeval *now) {
  int rv;
  int write_mask = EVENTER_EXCEPTION;
  connection_ctx_t *ctx = closure;
  request_ctx_t *request = &ctx->request;
  msg_hdr_t hdr;
  hdr = msg_hdr_create(request->send_size);

  request->send_size += sizeof(hdr);
  while((rv = e->opset->write(e->fd,
     ((char*)&hdr) + request->write_sofar, sizeof(hdr) - request->write_sofar, &write_mask, e)) > 0) {
    request->write_sofar += rv;
    if(request->write_sofar == sizeof(hdr)) break;
  }
  while((rv = e->opset->write(e->fd,
      request->outbuff + request->write_sofar - sizeof(hdr), request->send_size - request->write_sofar, &write_mask, e)) > 0) {
    request->write_sofar += rv;
    if(request->write_sofar == request->send_size) break;
  }
  if(request->data_free) {
    request->data_free(request->outbuff);
  }
  request->outbuff = NULL;

  return async_read_next_message(e, ctx->response_callback);
}

static int
mtev_cluster_messaging_start_sending(eventer_t e, char *data,
    uint data_len, data_free_fn *data_free, mtev_cluster_messaging_received_func_t response_callback, void* closure) {
  connection_ctx_t *ctx = e->closure;

  if(ctx == NULL) {
    ctx = calloc(1, sizeof(connection_ctx_t));
    e->closure = ctx;
  }

  assert(ctx->request.outbuff == NULL && "Trying to send via a connection that's still active. Please remember that send/receive are in lockstep");

  ctx->request.outbuff = data;
  ctx->request.send_size = data_len;
  ctx->request.write_sofar = 0;
  ctx->request.data_free = data_free;
  ctx->closure = closure;
  e->mask = EVENTER_WRITE | EVENTER_EXCEPTION;
  if(response_callback)
    ctx->response_callback = response_callback;

  e->callback = mtev_cluster_messaging_send;

  return 1;
}

int
mtev_cluster_messaging_send_request(eventer_t connection, char *data,
  uint data_len, data_free_fn *data_free,
  mtev_cluster_messaging_received_func_t response_callback, void* closure) {
  return mtev_cluster_messaging_start_sending(connection, data, data_len, data_free, response_callback, closure);
}

int
mtev_cluster_messaging_send_response(eventer_t e, char *data,
    uint data_len, data_free_fn *data_free) {
  return mtev_cluster_messaging_start_sending(e, data, data_len, data_free, NULL, NULL);
}

void
mtev_cluster_messaging_init(char* cluster_name) {
  mtev_cluster_t *cluster;
  if(mtev_cluster_enabled() == mtev_true) {
    cluster = mtev_cluster_by_name(cluster_name);
    if(cluster == NULL) {
      mtevL(mtev_error, "Unable to find cluster %s in the config files\n",
          cluster_name);
      exit(1);
    }
    eventer_name_callback("noit_cluster_network", on_connection_established);
    mtev_listener("*", mtev_cluster_get_data_port(cluster), SOCK_STREAM, 5, NULL, NULL, on_connection_established, NULL);
  } else {
    mtevL(mtev_notice, "Didn't find any cluster in the config files\n");
  }
}

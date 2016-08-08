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

static int newmask = EVENTER_READ | EVENTER_EXCEPTION;

typedef struct {
  uint32_t length_including_header;
} msg_hdr_t;

typedef struct {
  mtev_cluster_node_t node;
  char* outbuff;
  uint send_size;
  uint write_sofar;
  data_free_fn *data_free;
} send_job_t;

typedef struct {
  msg_hdr_t msg_hdr;
  char *payload;
  uint8_t read_so_far;
} request_ctx_t;

static request_ctx_t*
request_ctx_alloc() {
  return calloc(1, sizeof(request_ctx_t));
}

static void
request_ctx_free(void* data) {
  request_ctx_t *ctx = data;
  if(ctx->payload) free(ctx->payload);
  free(ctx);
}

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

MTEV_HOOK_IMPL(mtev_cluster_messaging_received,
  (eventer_t e, char *data, int data_len),
  void *, closure,
  (void *closure, eventer_t e, char *data, int data_len),
  (closure,e,data,data_len))

static int
read_next_message(eventer_t e, request_ctx_t *ctx) {
  int read, bytes_expected, inbuff_offset, mask;
  char* inbuff;
  bytes_expected = sizeof(msg_hdr_t);
  inbuff_offset = ctx->read_so_far;
  inbuff = (char*)&ctx->msg_hdr;
  if(ctx->read_so_far >= bytes_expected) {
    bytes_expected = ctx->msg_hdr.length_including_header;
    inbuff_offset -= sizeof(msg_hdr_t);
    inbuff = ctx->payload;
  }

  while(1) {
    read = e->opset->read(e->fd, inbuff + inbuff_offset, bytes_expected - ctx->read_so_far, &mask, e);
    if(read == -1 && errno == EAGAIN) {
      return mask;
    }

    if(read <= 0) {
      eventer_remove_fd(e->fd);
      e->opset->close(e->fd, &mask, e);
      return 0;
    }
    if(read > 0) {
      ctx->read_so_far += read;
      inbuff_offset += read;
      if(ctx->read_so_far == sizeof(msg_hdr_t)) {
        // header is complete
        ntoh_msg_hdr(&ctx->msg_hdr);
        ctx->payload = malloc(ctx->msg_hdr.length_including_header - sizeof(msg_hdr_t));
        bytes_expected = ctx->msg_hdr.length_including_header;
        inbuff = ctx->payload;
        inbuff_offset = 0;
      } else if(ctx->read_so_far == bytes_expected) {
        // message is complete
        mtev_cluster_messaging_received_hook_invoke(e, ctx->payload, ctx->msg_hdr.length_including_header - sizeof(msg_hdr_t));

        ctx->read_so_far = 0;
        free(ctx->payload);
        ctx->payload = NULL;
        return 0;
      }
    }
  }
}

static int
on_msg_received(eventer_t e, int mask, void *closure,
    struct timeval *now) {
  acceptor_closure_t *ac = closure;
  request_ctx_t *ctx = ac->service_ctx;

  if(mask & EVENTER_EXCEPTION) {
    /* Exceptions cause us to simply snip the connection */

    /* This removes the log feed which is important to do before calling close */
    eventer_remove_fd(e->fd);
    e->opset->close(e->fd, &newmask, e);
    return 0;
  }

  if(!ac->service_ctx) {
    ctx = ac->service_ctx = request_ctx_alloc();
    ac->service_ctx_free = request_ctx_free;
  }

  return read_next_message(e, ctx);
}


static int
mtev_cluster_send_connection_complete(eventer_t e, int mask, void *closure,
    struct timeval *now) {
  int rv;
  int write_mask = EVENTER_EXCEPTION;
  send_job_t *job = closure;
  msg_hdr_t hdr;
  hdr = msg_hdr_create(job->send_size);

  job->send_size += sizeof(hdr);
  while((rv = e->opset->write(e->fd,
     ((char*)&hdr) + job->write_sofar, sizeof(hdr) - job->write_sofar, &write_mask, e)) > 0) {
    job->write_sofar += rv;
    if(job->write_sofar == sizeof(hdr)) break;
  }
  while((rv = e->opset->write(e->fd,
      job->outbuff + job->write_sofar - sizeof(hdr), job->send_size - job->write_sofar, &write_mask, e)) > 0) {
    job->write_sofar += rv;
    if(job->write_sofar == job->send_size) break;
  }
  if(job->data_free) {
    job->data_free(job->outbuff);
  }
  eventer_remove_fd(e->fd);
  e->opset->close(e->fd, &newmask, e);
  eventer_free(e);
  free(job);
  return 0;
}

int
mtev_cluster_messaging_send(const mtev_cluster_node_t *node, char *data, uint data_len, data_free_fn *data_free) {
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
  if(rv == -1) return -1;

  send_job_t *job = malloc(sizeof(send_job_t));
  job->node = *node;
  job->outbuff = data;
  job->send_size = data_len;
  job->data_free = data_free;

  /* Register a handler for connection completion */
  e = eventer_alloc();
  e->fd = fd;
  e->mask = EVENTER_READ | EVENTER_WRITE | EVENTER_EXCEPTION;
  e->callback = mtev_cluster_send_connection_complete;
  e->closure = job;
  eventer_add(e);

  return 1;
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
    eventer_name_callback("noit_cluster_network", on_msg_received);
  } else {
    mtevL(mtev_notice, "Didn't find any cluster in the config files\n");
  }
}

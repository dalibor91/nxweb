/*
 * Copyright (c) 2011-2012 Yaroslav Stavnichiy <yarosla@gmail.com>
 *
 * This file is part of NXWEB.
 *
 * NXWEB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * NXWEB is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with NXWEB. If not, see <http://www.gnu.org/licenses/>.
 */

#include "nxweb.h"

#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>

static void request_complete(nxe_loop* loop, nxd_http_server_proto* hsp) {
  if (hsp->req_finalize) {
    hsp->req_finalize(hsp, hsp->req_data);
    hsp->req_finalize=0;
    hsp->req_data=0;
  }
  //nxd_fbuffer_finalize(&hsp->fb);
  if (hsp->resp && hsp->resp->sendfile_fd) {
    close(hsp->resp->sendfile_fd);
  }
  nxb_empty(hsp->nxb);
  nxp_free(hsp->nxb_pool, hsp->nxb);
  hsp->nxb=0;
  if (hsp->resp_body_in.pair) nxe_disconnect_streams(hsp->resp_body_in.pair, &hsp->resp_body_in);
  if (hsp->req_body_out.pair) nxe_disconnect_streams(&hsp->req_body_out, hsp->req_body_out.pair);
  hsp->request_count++;
  hsp->state=HSP_WAITING_FOR_REQUEST;
  hsp->headers_bytes_received=0;
  nxe_publish(&hsp->events_pub, (nxe_data)NXD_HSP_REQUEST_COMPLETE);
  memset(&hsp->req, 0, sizeof(nxweb_http_request));
  memset(&hsp->_resp, 0, sizeof(nxweb_http_response));
  hsp->resp=0;
}

static void subrequest_finalize(nxd_http_server_proto* hsp) {
  if (hsp->req_finalize) hsp->req_finalize(hsp, hsp->req_data);
  nxe_loop* loop=hsp->events_pub.super.loop;
  while (hsp->events_pub.sub) nxe_unsubscribe(&hsp->events_pub, hsp->events_pub.sub);
  nxd_fbuffer_finalize(&hsp->fb);
  if (hsp->resp && hsp->resp->sendfile_fd) close(hsp->resp->sendfile_fd);
  if (hsp->nxb) {
    nxb_empty(hsp->nxb);
    nxp_free(hsp->nxb_pool, hsp->nxb);
  }
}

static void subrequest_start_sending_response(nxd_http_server_proto* hsp, nxweb_http_response* resp) {
  if (hsp->state!=HSP_RECEIVING_HEADERS && hsp->state!=HSP_RECEIVING_BODY && hsp->state!=HSP_HANDLING) {
    nxweb_log_error("illegal state for start_sending_response()");
    return;
  }

  nxweb_http_request* req=&hsp->req;
  hsp->resp=resp;
  nxe_loop* loop=hsp->events_pub.super.loop;
  if (!resp->nxb) resp->nxb=hsp->nxb;

  if (!resp->content && !resp->sendfile_path && !resp->sendfile_fd && !resp->content_out) {
    int size;
    nxb_get_unfinished(resp->nxb, &size);
    if (size) {
      resp->content=nxb_finish_stream(resp->nxb, &size);
      resp->content_length=size;
    }
  }

  if (resp->content && resp->content_length>0) {
    nxd_obuffer_init(&hsp->ob, resp->content, resp->content_length);
    nxe_connect_streams(loop, &hsp->ob.data_out, &hsp->resp_body_in);
  }
  else if (resp->sendfile_fd && resp->content_length>0) {
    assert(resp->sendfile_end - resp->sendfile_offset == resp->content_length);
    nxd_fbuffer_init(&hsp->fb, resp->sendfile_fd, resp->sendfile_offset, resp->sendfile_end);
    nxe_connect_streams(loop, &hsp->fb.data_out, &hsp->resp_body_in);
  }
  else if (resp->sendfile_path && resp->content_length>0) {
    resp->sendfile_fd=open(resp->sendfile_path, O_RDONLY|O_NONBLOCK);
    if (resp->sendfile_fd!=-1) {
      assert(resp->sendfile_end - resp->sendfile_offset == resp->content_length);
      nxd_fbuffer_init(&hsp->fb, resp->sendfile_fd, resp->sendfile_offset, resp->sendfile_end);
      nxe_connect_streams(loop, &hsp->fb.data_out, &hsp->resp_body_in);
    }
    else {
      nxweb_log_error("nxd_http_server_proto_start_sending_response(): can't open %s", resp->sendfile_path);
    }
  }
  else if (resp->content_out) {
    nxe_connect_streams(loop, resp->content_out, &hsp->resp_body_in);
  }

  if (!resp->raw_headers) _nxweb_prepare_response_headers(loop, resp);

  hsp->state=HSP_SENDING_HEADERS;
}

static void subrequest_connect_request_body_out(nxd_http_server_proto* hsp, nxe_ostream* is) {
  nxe_connect_streams(hsp->events_pub.super.loop, &hsp->ob.data_out, is);
}

static nxe_ostream* subrequest_get_request_body_out_pair(nxd_http_server_proto* hsp) {
  return hsp->ob.data_out.pair;
}

static void subrequest_start_receiving_request_body(nxd_http_server_proto* hsp) {

}

static const nxd_http_server_proto_class subrequest_class={
  .finalize=subrequest_finalize,
  .start_sending_response=subrequest_start_sending_response,
  .start_receiving_request_body=subrequest_start_receiving_request_body,
  .connect_request_body_out=subrequest_connect_request_body_out,
  .get_request_body_out_pair=subrequest_get_request_body_out_pair
};

void nxd_http_server_proto_subrequest_init(nxd_http_server_proto* hsp, nxp_pool* nxb_pool) {
  memset(hsp, 0, sizeof(nxd_http_server_proto));
  hsp->cls=&subrequest_class;
  hsp->nxb_pool=nxb_pool;
  hsp->events_pub.super.cls.pub_cls=NXE_PUB_DEFAULT;
  hsp->state=HSP_WAITING_FOR_REQUEST;
}

void nxweb_subrequest_execute(nxweb_http_server_connection* conn) {
  nxd_http_server_proto* hsp=&conn->hsp;
  hsp->nxb=nxp_alloc(hsp->nxb_pool);
  nxb_init(hsp->nxb, NXWEB_CONN_NXB_SIZE);
  hsp->state=HSP_RECEIVING_HEADERS;
  hsp->headers_bytes_received=1;
}
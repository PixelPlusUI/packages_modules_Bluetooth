
// Copyright (C) 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "net/posix/posix_async_socket_connector.h"

#include <arpa/inet.h>   // for inet_addr, inet_ntoa
#include <errno.h>       // for errno, EAGAIN, EINPROGRESS
#include <netdb.h>       // for gethostbyname, addrinfo
#include <netinet/in.h>  // for sockaddr_in, in_addr
#include <poll.h>        // for poll, POLLHUP, POLLIN, POL...
#include <string.h>      // for strerror, NULL
#include <sys/socket.h>  // for connect, getpeername, gets...
#include <type_traits>   // for remove_extent_t

#include "net/posix/posix_async_socket.h"  // for PosixAsyncSocket
#include "os/log.h"                        // for LOG_INFO
#include "osi/include/osi.h"               // for OSI_NO_INTR

namespace android {
namespace net {

PosixAsyncSocketConnector::PosixAsyncSocketConnector(AsyncManager* am)
    : am_(am) {}

std::shared_ptr<AsyncDataChannel>
PosixAsyncSocketConnector::ConnectToRemoteServer(
    const std::string& server, int port,
    const std::chrono::milliseconds timeout) {
  LOG_INFO("Connecting to %s:%d in %d ms", server.c_str(), port,
           (int)timeout.count());
  int socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  std::shared_ptr<PosixAsyncSocket> pas =
      std::make_shared<PosixAsyncSocket>(socket_fd, am_);

  if (socket_fd < 1) {
    LOG_INFO("socket() call failed: %s", strerror(errno));
    return pas;
  }

  struct hostent* host;
  host = gethostbyname(server.c_str());
  if (host == NULL) {
    LOG_INFO("gethostbyname() failed for %s: %s", server.c_str(),
             strerror(errno));
    pas->Close();
    return pas;
  }

  struct in_addr** addr_list = (struct in_addr**)host->h_addr_list;
  struct sockaddr_in serv_addr {};
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = inet_addr(inet_ntoa(*addr_list[0]));
  serv_addr.sin_port = htons(port);

  int result =
      connect(socket_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

  if (result != 0 &&
      !(errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS)) {
    LOG_INFO("Failed to connect to %s:%d, error:  %s", server.c_str(), port,
             strerror(errno));
    pas->Close();
    return pas;
  }

  // wait for the connection.
  struct pollfd fds[] = {
      {
          .fd = socket_fd,
          .events = POLLIN | POLLOUT | POLLHUP,
          .revents = 0,
      },
  };
  int numFdsReady = 0;
  OSI_NO_INTR(numFdsReady = ::poll(fds, 1, timeout.count()));
  if (numFdsReady <= 0) {
    LOG_INFO("Failed to connect to %s:%d, error:  %s", server.c_str(), port,
             strerror(errno));
    pas->Close();
    return pas;
  }

  // As per https://cr.yp.to/docs/connect.html, we should get the peername
  // for validating if a connection was established.
  struct sockaddr_storage ss;
  socklen_t sslen = sizeof(ss);

  if (getpeername(socket_fd, (struct sockaddr*)&ss, &sslen) < 0) {
    LOG_INFO("Failed to connect to %s:%d, error:  %s", server.c_str(), port,
             strerror(errno));
    pas->Close();
    return pas;
  }

  int err = 0;
  socklen_t optLen = sizeof(err);
  if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err),
                 &optLen) ||
      err) {
    // Either getsockopt failed or there was an error associated
    // with the socket. The connection did not succeed.
    LOG_INFO("Failed to connect to %s:%d, error:  %s", server.c_str(), port,
             strerror(err));
    pas->Close();
    return pas;
  }

  LOG_INFO("Connected to %s:%d (%d)", server.c_str(), port, socket_fd);
  return pas;
}

}  // namespace net
}  // namespace android

/*
  Copyright (c) 2015, 2020, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "mysqlrouter/routing.h"

#include <array>
#include <chrono>
#include <string>

#ifndef _WIN32
#include <netdb.h>        // addrinfo
#include <netinet/tcp.h>  // TCP_NODELAY
#include <sys/socket.h>   // SOCK_NONBLOCK, ...
#else
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "common.h"  // serial_comma
#include "mysql/harness/logging/logging.h"
#include "mysql/harness/net_ts/impl/resolver.h"
#include "mysql/harness/net_ts/impl/socket.h"
#include "mysql/harness/net_ts/impl/socket_error.h"

IMPORT_LOG_FUNCTIONS()

namespace routing {

const int kDefaultWaitTimeout = 0;  // 0 = no timeout used
const int kDefaultMaxConnections = 512;
const std::chrono::seconds kDefaultDestinationConnectionTimeout{1};
const std::string kDefaultBindAddress = "127.0.0.1";
const unsigned int kDefaultNetBufferLength =
    16384;  // Default defined in latest MySQL Server
const unsigned long long kDefaultMaxConnectErrors =
    100;  // Similar to MySQL Server
const std::chrono::seconds kDefaultClientConnectTimeout{
    9};  // Default connect_timeout MySQL Server minus 1

// unused constant
// const int kMaxConnectTimeout = INT_MAX / 1000;

// keep in-sync with enum AccessMode
static const std::array<const char *, 3> kAccessModeNames{{
    nullptr,
    "read-write",
    "read-only",
}};

AccessMode get_access_mode(const std::string &value) {
  for (unsigned int i = 1; i < kAccessModeNames.size(); ++i)
    if (kAccessModeNames[i] == value) return static_cast<AccessMode>(i);
  return AccessMode::kUndefined;
}

std::string get_access_mode_names() {
  // +1 to skip undefined
  return mysql_harness::serial_comma(kAccessModeNames.begin() + 1,
                                     kAccessModeNames.end());
}

std::string get_access_mode_name(AccessMode access_mode) noexcept {
  if (access_mode == AccessMode::kUndefined)
    return "<not-set>";
  else
    return kAccessModeNames[static_cast<int>(access_mode)];
}

// keep in-sync with enum RoutingStrategy
static const std::array<const char *, 5> kRoutingStrategyNames{{
    nullptr,
    "first-available",
    "next-available",
    "round-robin",
    "round-robin-with-fallback",
}};

RoutingStrategy get_routing_strategy(const std::string &value) {
  for (unsigned int i = 1; i < kRoutingStrategyNames.size(); ++i)
    if (kRoutingStrategyNames[i] == value)
      return static_cast<RoutingStrategy>(i);
  return RoutingStrategy::kUndefined;
}

std::string get_routing_strategy_names(bool metadata_cache) {
  // round-robin-with-fallback is not supported for static routing
  const std::array<const char *, 3> kRoutingStrategyNamesStatic{{
      "first-available",
      "next-available",
      "round-robin",
  }};

  // next-available is not supported for metadata-cache routing
  const std::array<const char *, 3> kRoutingStrategyNamesMetadataCache{{
      "first-available",
      "round-robin",
      "round-robin-with-fallback",
  }};

  const auto &v = metadata_cache ? kRoutingStrategyNamesMetadataCache
                                 : kRoutingStrategyNamesStatic;
  return mysql_harness::serial_comma(v.begin(), v.end());
}

std::string get_routing_strategy_name(
    RoutingStrategy routing_strategy) noexcept {
  if (routing_strategy == RoutingStrategy::kUndefined)
    return "<not set>";
  else
    return kRoutingStrategyNames[static_cast<int>(routing_strategy)];
}

RoutingSockOps *RoutingSockOps::instance(
    mysql_harness::SocketOperationsBase *sock_ops) {
  static RoutingSockOps routing_sock_ops(sock_ops);
  return &routing_sock_ops;
}

stdx::expected<routing::native_handle_type, std::error_code>
RoutingSockOps::get_mysql_socket(mysql_harness::TCPAddress addr,
                                 std::chrono::milliseconds connect_timeout_ms,
                                 bool log) noexcept {
  struct addrinfo hints;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  const auto addrinfo_res = net::impl::resolver::getaddrinfo(
      addr.addr.c_str(), std::to_string(addr.port).c_str(), &hints);
  if (!addrinfo_res) {
    if (log) {
      log_debug("Failed getting address information for '%s' (%s)",
                addr.addr.c_str(), addrinfo_res.error().message().c_str());
    }
    return stdx::make_unexpected(addrinfo_res.error());
  }

  routing::native_handle_type sock{routing::kInvalidSocket};
  bool timeout_expired = false;

  struct addrinfo *info = addrinfo_res.value().get();
  for (; info != nullptr; info = info->ai_next) {
    auto sock_type = info->ai_socktype;
#if defined(__linux__) || defined(__FreeBSD__)
    // linux|freebsd allows to set NONBLOCK as part of the socket() call to safe
    // the extra syscall
    sock_type |= SOCK_NONBLOCK;
#endif
    const auto socket_res = net::impl::socket::socket(
        info->ai_family, sock_type, info->ai_protocol);
    if (!socket_res) {
      log_error("Failed opening socket: %s",
                socket_res.error().message().c_str());
    } else {
      sock = socket_res.value();

      so_->set_socket_blocking(sock, false);

      const auto connect_res =
          net::impl::socket::connect(sock, info->ai_addr, info->ai_addrlen);
      if (!connect_res) {
        if (connect_res.error() ==
                make_error_condition(std::errc::operation_in_progress) ||
            connect_res.error() ==
                make_error_condition(std::errc::operation_would_block)) {
          const auto wait_res =
              so_->connect_non_blocking_wait(sock, connect_timeout_ms);

          if (!wait_res) {
            log_warning(
                "Timeout reached trying to connect to MySQL Server %s: %s",
                addr.str().c_str(), wait_res.error().message().c_str());
            timeout_expired =
                (wait_res.error() == make_error_code(std::errc::timed_out));
          } else {
            const auto status_res = so_->connect_non_blocking_status(sock);
            if (status_res) {
              // success, we can continue
              break;
            }
          }
        } else {
          log_debug("Failed connect() to %s: %s", addr.str().c_str(),
                    connect_res.error().message().c_str());
        }
      } else {
        // everything is fine, we are connected
        break;
      }

      // some error, close the socket again and try the next one
      so_->close(sock);
      sock = kInvalidSocket;
    }
  }

  if (info == nullptr) {
    // all connects failed.
    return stdx::make_unexpected(
        make_error_code(timeout_expired ? std::errc::timed_out
                                        : std::errc::connection_refused));
  }

  // set blocking; MySQL protocol is blocking and we do not take advantage of
  // any non-blocking possibilities
  so_->set_socket_blocking(sock, true);

  int opt_nodelay = 1;
  const auto sockopt_res = so_->setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                                           &opt_nodelay, sizeof opt_nodelay);
  if (!sockopt_res) {
    log_debug("Failed setting TCP_NODELAY on client socket: %s",
              sockopt_res.error().message().c_str());
    so_->close(sock);

    return stdx::make_unexpected(sockopt_res.error());
  }

  return sock;
}

}  // namespace routing

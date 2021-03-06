/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/transport/tcp/pair.h"

#include <array>
#include <algorithm>
#include <sstream>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "gloo/common/error.h"
#include "gloo/common/logging.h"
#include "gloo/transport/tcp/buffer.h"
#include "gloo/transport/tcp/context.h"
#include "gloo/transport/tcp/unbound_buffer.h"

#define FD_INVALID (-1)

namespace gloo {
namespace transport {
namespace tcp {

namespace {

// This reflects an approximation of /proc/sys/net/core/{r,w}mem_max.
// It is hard coded because making buffers larger than this would not
// have much impact. Also see socket(7).
constexpr size_t kMaxSendBufferSize = 32 * 1024 * 1024;
constexpr size_t kMaxRecvBufferSize = 32 * 1024 * 1024;

} // namespace

Pair::Pair(
    Context* context,
    Device* device,
    int rank,
    std::chrono::milliseconds timeout)
    : context_(context),
      device_(device),
      rank_(rank),
      state_(INITIALIZING),
      sync_(false),
      timeout_(timeout),
      busyPoll_(false),
      fd_(FD_INVALID),
      sendBufferSize_(0),
      ex_(nullptr) {
  listen();
}

// Destructor performs a "soft" close.
Pair::~Pair() {
  // Needs lock so that this doesn't race with read/write of the
  // underlying file descriptor on the device thread.
  std::lock_guard<std::mutex> lock(m_);
  if (state_ != CLOSED) {
    changeState(CLOSED);
  }
}

// The close function performs a "hard" close.
// It sets SO_LINGER to reset the connection on close,
// in order to avoid sockets hanging around in TIME_WAIT.
void Pair::close() {
  // Needs lock so that this doesn't race with read/write of the
  // underlying file descriptor on the device thread.
  std::lock_guard<std::mutex> lock(m_);
  if (state_ != CLOSED) {
    if (fd_ != FD_INVALID) {
      struct linger sl;
      sl.l_onoff = 1;
      sl.l_linger = 0;
      setsockopt(fd_, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    }
    changeState(CLOSED);
  }
}

const Address& Pair::address() const {
  return self_;
}

void Pair::connect(const std::vector<char>& bytes) {
  auto peer = Address(bytes);
  connect(peer);
}

static void setSocketBlocking(int fd, bool enable) {
  auto rv = fcntl(fd, F_GETFL);
  GLOO_ENFORCE_NE(rv, -1);
  if (enable) {
    rv &= ~O_NONBLOCK;
  } else {
    rv |= O_NONBLOCK;
  }
  rv = fcntl(fd, F_SETFL, rv);
  GLOO_ENFORCE_NE(rv, -1);
}

void Pair::setSync(bool sync, bool busyPoll) {
  std::unique_lock<std::mutex> lock(m_);

  if (!sync) {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION("Can only switch to sync mode");
  }

  // Wait for pair to be connected. No need to wait for timeout here. If
  // necessary, the connect path will timeout and signal this thread.
  waitUntilConnected(lock, false);
  if (state_ == CLOSED) {
    signalAndThrowException(
        GLOO_ERROR_MSG("Socket unexpectedly closed ", peer_.str()));
  }

  if (!sync_) {
    // If async, unregister from loop and switch socket to blocking mode
    device_->unregisterDescriptor(fd_);
    setSocketBlocking(fd_, true);

    // If the pair was still flushing writes, finish them.
    for (auto& op : tx_) {
      auto rv = write(op);
      if (!rv) {
        GLOO_ENFORCE(
            ex_ != nullptr,
            "write() returned false in sync mode; ex_ must be set");
        std::rethrow_exception(ex_);
      }
    }
    tx_.clear();
  }

  sync_ = true;
  busyPoll_ = busyPoll;
}

void Pair::listen() {
  std::lock_guard<std::mutex> lock(m_);
  int rv;

  const auto& attr = device_->attr_;
  auto fd = socket(attr.ai_family, attr.ai_socktype, attr.ai_protocol);
  if (fd == -1) {
    signalAndThrowException(GLOO_ERROR_MSG("socket: ", strerror(errno)));
  }

  // Set SO_REUSEADDR to signal that reuse of the listening port is OK.
  int on = 1;
  rv = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (rv == -1) {
    ::close(fd);
    signalAndThrowException(GLOO_ERROR_MSG("setsockopt: ", strerror(errno)));
  }

  rv = bind(fd, (const sockaddr*)&attr.ai_addr, attr.ai_addrlen);
  if (rv == -1) {
    ::close(fd);
    signalAndThrowException(GLOO_ERROR_MSG("bind: ", strerror(errno)));
  }

  // listen(2) on socket
  fd_ = fd;
  rv = ::listen(fd_, 1);
  if (rv == -1) {
    ::close(fd_);
    fd_ = FD_INVALID;
    signalAndThrowException(GLOO_ERROR_MSG("listen: ", strerror(errno)));
  }

  // Keep copy of address
  self_ = Address::fromSockName(fd);

  // Register with device so we're called when peer connects
  changeState(LISTENING);
  device_->registerDescriptor(fd_, EPOLLIN, this);

  return;
}

void Pair::connect(const Address& peer) {
  std::unique_lock<std::mutex> lock(m_);
  int rv;
  socklen_t addrlen;
  throwIfException();

  peer_ = peer;

  // Addresses have to have same family
  if (self_.ss_.ss_family != peer_.ss_.ss_family) {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION("address family mismatch");
  }

  if (self_.ss_.ss_family == AF_INET) {
    struct sockaddr_in* sa = (struct sockaddr_in*)&self_.ss_;
    struct sockaddr_in* sb = (struct sockaddr_in*)&peer_.ss_;
    addrlen = sizeof(struct sockaddr_in);
    rv = memcmp(&sa->sin_addr, &sb->sin_addr, sizeof(struct in_addr));
    if (rv == 0) {
      rv = sa->sin_port - sb->sin_port;
    }
  } else if (peer_.ss_.ss_family == AF_INET6) {
    struct sockaddr_in6* sa = (struct sockaddr_in6*)&self_.ss_;
    struct sockaddr_in6* sb = (struct sockaddr_in6*)&peer_.ss_;
    addrlen = sizeof(struct sockaddr_in6);
    rv = memcmp(&sa->sin6_addr, &sb->sin6_addr, sizeof(struct in6_addr));
    if (rv == 0) {
      rv = sa->sin6_port - sb->sin6_port;
    }
  } else {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION("unknown sa_family");
  }

  if (rv == 0) {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION("cannot connect to self");
  }

  // self_ < peer_; we are listening side.
  if (rv < 0) {
    waitUntilConnected(lock, true);
    return;
  }

  // self_ > peer_; we are connecting side.
  // First destroy listening socket.
  device_->unregisterDescriptor(fd_);
  ::close(fd_);

  // Create new socket to connect to peer.
  fd_ = socket(peer_.ss_.ss_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd_ == -1) {
    signalAndThrowException(GLOO_ERROR_MSG("socket: ", strerror(errno)));
  }

  // Set SO_REUSEADDR to signal that reuse of the source port is OK.
  int on = 1;
  rv = setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (rv == -1) {
    ::close(fd_);
    fd_ = FD_INVALID;
    signalAndThrowException(GLOO_ERROR_MSG("setsockopt: ", strerror(errno)));
  }

  // Connect to peer
  rv = ::connect(fd_, (struct sockaddr*)&peer_.ss_, addrlen);
  if (rv == -1 && errno != EINPROGRESS) {
    ::close(fd_);
    fd_ = FD_INVALID;
    signalAndThrowException(GLOO_ERROR_MSG("connect: ", strerror(errno)));
  }

  // Register with device so we're called when connection completes.
  changeState(CONNECTING);
  device_->registerDescriptor(fd_, EPOLLIN | EPOLLOUT, this);

  // Wait for connection to complete
  waitUntilConnected(lock, true);
}

ssize_t Pair::prepareWrite(
    Op& op,
    const NonOwningPtr<UnboundBuffer>& buf,
    struct iovec* iov,
    int& ioc) {
  ssize_t len = 0;
  ioc = 0;

  // Include preamble if necessary
  if (op.nwritten < sizeof(op.preamble)) {
    iov[ioc].iov_base = ((char*)&op.preamble) + op.nwritten;
    iov[ioc].iov_len = sizeof(op.preamble) - op.nwritten;
    len += iov[ioc].iov_len;
    ioc++;
  }

  auto opcode = op.getOpcode();

  // Send data to a remote buffer
  if (opcode == Op::SEND_BUFFER) {
    char* ptr = (char*)op.buf->ptr_;
    size_t offset = op.preamble.offset;
    size_t nbytes = op.preamble.length;
    if (op.nwritten > sizeof(op.preamble)) {
      offset += op.nwritten - sizeof(op.preamble);
      nbytes -= op.nwritten - sizeof(op.preamble);
    }
    iov[ioc].iov_base = ptr + offset;
    iov[ioc].iov_len = nbytes;
    len += iov[ioc].iov_len;
    ioc++;
    return len;
  }

  // Send data to a remote unbound buffer
  if (opcode == Op::SEND_UNBOUND_BUFFER) {
    char* ptr = (char*)buf->ptr;
    size_t offset = op.offset;
    size_t nbytes = op.nbytes;
    if (op.nwritten > sizeof(op.preamble)) {
      offset += op.nwritten - sizeof(op.preamble);
      nbytes -= op.nwritten - sizeof(op.preamble);
    }
    iov[ioc].iov_base = ptr + offset;
    iov[ioc].iov_len = nbytes;
    len += iov[ioc].iov_len;
    ioc++;
    return len;
  }

  return len;
}

// write is called from:
// 1) the device thread (the handleEvents function)
// 2) a user thread (the send function)
//
// In either case, the lock is held and the write function
// below inherits it.
//
bool Pair::write(Op& op) {
  NonOwningPtr<UnboundBuffer> buf;
  std::array<struct iovec, 2> iov;
  int ioc;
  ssize_t rv;

  const auto opcode = op.getOpcode();

  // Acquire pointer to unbound buffer if applicable.
  if (opcode == Op::SEND_UNBOUND_BUFFER) {
    buf = NonOwningPtr<UnboundBuffer>(op.ubuf);
    if (!buf) {
      return false;
    }
  }

  for (;;) {
    const auto nbytes = prepareWrite(op, buf, iov.data(), ioc);

    // Write
    rv = writev(fd_, iov.data(), ioc);
    if (rv == -1) {
      if (errno == EAGAIN) {
        if (sync_) {
          // Sync mode: blocking call returning with EAGAIN indicates timeout.
          signalException(GLOO_ERROR_MSG("Write timeout ", peer_.str()));
        } else {
          // Async mode: can't write more than this.
        }
        return false;
      }

      // Retry on EINTR
      if (errno == EINTR) {
        continue;
      }

      // Unexpected error
      signalException(
          GLOO_ERROR_MSG("writev ", peer_.str(), ": ", strerror(errno)));
      return false;
    }

    // From write(2) man page (NOTES section):
    //
    //  If a write() is interrupted by a signal handler before any
    //  bytes are written, then the call fails with the error EINTR;
    //  if it is interrupted after at least one byte has been written,
    //  the call succeeds, and returns the number of bytes written.
    //
    // If rv < nbytes we ALWAYS retry, regardless of sync/async mode,
    // since an EINTR may or may not have happened. If this was not
    // the case, and the kernel buffer is full, the next call to
    // write(2) will return EAGAIN, which is handled appropriately.
    op.nwritten += rv;
    if (rv < nbytes) {
      continue;
    }

    GLOO_ENFORCE_EQ(rv, nbytes);
    GLOO_ENFORCE_EQ(op.nwritten, op.preamble.nbytes);
    break;
  }

  // Write completed
  switch (opcode) {
    case Op::SEND_BUFFER:
      op.buf->handleSendCompletion();
      break;
    case Op::SEND_UNBOUND_BUFFER:
      buf->handleSendCompletion(rank_);
      break;
    case Op::NOTIFY_SEND_READY:
      break;
    case Op::NOTIFY_RECV_READY:
      break;
  }

  return true;
}

// Populates the iovec struct. May populate the 'buf' or 'ubuf' member field
// in the op if the preamble indicates the operation is one of type SEND_BUFFER
// or SEND_UNBOUND_BUFFER.
//
// Returns a boolean indicating whether or not the caller (the read function)
// should continue trying to read from the socket. This is not the case if the
// buffer this message is intended for has not yet been registered (this can
// only be the case for unbound buffers).
//
ssize_t Pair::prepareRead(
    Op& op,
    NonOwningPtr<UnboundBuffer>& buf,
    struct iovec& iov) {
  iov.iov_base = nullptr;
  iov.iov_len = 0;

  // Read preamble
  if (op.nread < sizeof(op.preamble)) {
    iov.iov_base = ((char*)&op.preamble) + op.nread;
    iov.iov_len = sizeof(op.preamble) - op.nread;
    return iov.iov_len;
  }

  auto opcode = op.getOpcode();
  auto offset = op.nread - sizeof(op.preamble);

  // Remote side is sending data to a buffer; read payload
  if (opcode == Op::SEND_BUFFER) {
    if (op.buf == nullptr) {
      op.buf = getBuffer(op.preamble.slot);
      // Buffer not (yet) registered, leave it for next loop iteration
      if (op.buf == nullptr) {
        return -1;
      }
    }

    iov.iov_base = ((char*)op.buf->ptr_) + offset + op.preamble.roffset;
    iov.iov_len = op.preamble.length - offset;

    // Bytes read must be in bounds for target buffer
    GLOO_ENFORCE_LE(op.preamble.roffset + op.preamble.length, op.buf->size_);
    return iov.iov_len;
  }

  // Remote side is sending data to an unbound buffer; read payload
  if (opcode == Op::SEND_UNBOUND_BUFFER) {
    if (!op.ubuf) {
      auto it = localPendingRecv_.find(op.preamble.slot);
      GLOO_ENFORCE(it != localPendingRecv_.end());
      std::deque<UnboundBufferOp>& queue = it->second;
      GLOO_ENFORCE(!queue.empty());
      std::tie(op.ubuf, op.offset, op.nbytes) = queue.front();
      queue.pop_front();
      if (queue.empty()) {
        localPendingRecv_.erase(it);
      }
    }

    // Acquire short lived pointer to unbound buffer.
    // This is a stack allocated variable in the read function
    // which is destructed upon that function returning.
    buf = NonOwningPtr<UnboundBuffer>(op.ubuf);
    if (!buf) {
      return -1;
    }

    iov.iov_base = ((char*)buf->ptr) + op.offset + offset;
    iov.iov_len = op.preamble.length - offset;

    // Bytes read must be in bounds for target buffer
    GLOO_ENFORCE_LE(op.preamble.length, op.nbytes);
    return iov.iov_len;
  }

  return 0;
}

// read is called from:
// 1) the device thread (the handleEvents function).
// 2) a user thread (the recv function) IFF the pair is in sync mode.
//
// In either case, the lock is held and the read function
// below inherits it.
//
bool Pair::read() {
  NonOwningPtr<UnboundBuffer> buf;
  auto start = std::chrono::steady_clock::now();

  for (;;) {
    struct iovec iov = {
        .iov_base = nullptr,
        .iov_len = 0,
    };
    const auto nbytes = prepareRead(rx_, buf, iov);
    if (nbytes < 0) {
      return false;
    }

    // Break from loop if the op is complete.
    // Note that this means that the buffer pointer has been
    // set, per the call to prepareRead.
    if (nbytes == 0) {
      break;
    }

    // If busy-poll has been requested AND sync mode has been enabled for pair
    // we'll keep spinning calling recv() on socket by supplying MSG_DONTWAIT
    // flag. This is more efficient in terms of latency than allowing the kernel
    // to de-schedule this thread waiting for IO event to happen. The tradeoff
    // is stealing the CPU core just for busy polling.
    ssize_t rv = 0;
    for (;;) {
      // Alas, readv does not support flags, so we need to use recv
      rv = ::recv(fd_, iov.iov_base, iov.iov_len, busyPoll_ ? MSG_DONTWAIT : 0);
      if (rv == -1) {
        // EAGAIN happens when (1) non-blocking and there are no more bytes left
        // to read or (2) blocking and timeout occurs.
        if (errno == EAGAIN) {
          if (sync_) {
            // Sync mode: EAGAIN indicates nothing to read right now.
            auto hasTimedOut = [&] {
              return (timeout_ != kNoTimeout) &&
                  ((std::chrono::steady_clock::now() - start) >= timeout_);
            };
            if (busyPoll_ && !hasTimedOut()) {
              // Keep looping on EAGAIN if busy-poll flag has been set and the
              // timeout (if set) hasn't been reached
              continue;
            } else {
              // Either timeout on poll or blocking call returning with EAGAIN
              // indicates timeout
              signalException(GLOO_ERROR_MSG("Read timeout ", peer_.str()));
            }
          } else {
            // Async mode: can't read more than this.
          }
          return false;
        }

        // Retry on EINTR
        if (errno == EINTR) {
          continue;
        }

        // Unexpected error
        signalException(
            GLOO_ERROR_MSG("Read error ", peer_.str(), ": ", strerror(errno)));
        return false;
      }
      break;
    }

    // Transition to CLOSED on EOF
    if (rv == 0) {
      signalException(
          GLOO_ERROR_MSG("Connection closed by peer ", peer_.str()));
      return false;
    }

    rx_.nread += rv;
  }

  // Read completed
  const auto opcode = rx_.getOpcode();
  switch (opcode) {
    case Op::SEND_BUFFER:
      // Done sending data to pinned buffer; trigger completion.
      rx_.buf->handleRecvCompletion();
      break;
    case Op::SEND_UNBOUND_BUFFER:
      // Remote side is sending data to unbound buffer; trigger completion
      buf->handleRecvCompletion(rank_);
      break;
    case Op::NOTIFY_SEND_READY:
      // Remote side has pending send operation
      handleRemotePendingSend(rx_);
      break;
    case Op::NOTIFY_RECV_READY:
      // Remote side has pending recv operation
      handleRemotePendingRecv(rx_);
      break;
  }

  // Reset read operation state.
  rx_ = Op();
  return true;
}

// This function is called upon receiving a message from the peer
// indicating it has a pending send operation.
void Pair::handleRemotePendingSend(const Op& op) {
  const auto& slot = op.preamble.slot;

  // Note that the current value may be negative if the
  // corresponding recv's have already been posted.
  ContextMutator mutator(*context_, slot, rank_);
  auto remotePendingSend = mutator.getRemotePendingSend();

  // If the balance of remote pending sends is not negative, check
  // with the context whether or not there are any recv-from-any
  // operations that this send can fulfill.
  if (remotePendingSend >= 0) {
    WeakNonOwningPtr<UnboundBuffer> buf;
    size_t offset;
    size_t nbytes;
    if (mutator.findRecvFromAny(&buf, &offset, &nbytes)) {
      localPendingRecv_[slot].push_back(std::make_tuple(buf, offset, nbytes));
      sendNotifyRecvReady(slot, nbytes);
      return;
    }
  }

  // Increase balance of remote pending sends.
  mutator.updateRemotePendingSend(1);
}

// This function is called upon receiving a message from the peer
// indicating it has a pending receive operation.
void Pair::handleRemotePendingRecv(const Op& op) {
  const auto& slot = op.preamble.slot;

  // Find local pending send and execute it.
  // Nothing to do if there are none.
  auto it = localPendingSend_.find(slot);
  if (it != localPendingSend_.end()) {
    std::deque<UnboundBufferOp>& queue = it->second;
    GLOO_ENFORCE(!queue.empty());
    WeakNonOwningPtr<UnboundBuffer> buf;
    size_t offset;
    size_t nbytes;
    std::tie(buf, offset, nbytes) = queue.front();
    queue.pop_front();
    if (queue.empty()) {
      localPendingSend_.erase(it);
    }
    sendUnboundBuffer(std::move(buf), slot, offset, nbytes);
    return;
  }

  // Increase balance of remote pending recv.
  // Note that the current value CANNOT be negative, as sends
  // cannot execute until the remote side is ready to receive.
  ContextMutator mutator(*context_, slot, rank_);
  mutator.updateRemotePendingRecv(1);
}

void Pair::handleEvents(int events) {
  // Try to acquire the pair's lock so the device thread (the thread
  // that ends up calling handleEvents) can mutate the tx and rx op
  // fields of this instance. If the lock cannot be acquired that
  // means some other thread is trying to mutate this pair's state,
  // which in turn might require calling into (and locking) the
  // underlying device (for example, when the pair transitions to the
  // CLOSED state). To avoid deadlocks, attempt to lock the pair and
  // skip handling the events until the next tick if the lock cannot
  // be acquired.
  std::unique_lock<std::mutex> lock(m_, std::try_to_lock);
  if (!lock) {
    return;
  }

  // State must be <= CONNECTED.
  // If state is CLOSED; this function will NOT be called. Refer to
  // Pair::changeState and Device::unregisterDescriptor for more info.
  GLOO_ENFORCE_LE(state_, CONNECTED);

  // Exception must not be set.
  // If exception is set, state must advance to CLOSED state.
  GLOO_ENFORCE(ex_ == nullptr);

  if (state_ == CONNECTED) {
    if (state_ == CONNECTED && (events & EPOLLOUT)) {
      GLOO_ENFORCE(
          !tx_.empty(), "tx_ cannot be empty because EPOLLOUT happened");
      while (!tx_.empty()) {
        auto& op = tx_.front();
        if (!write(op)) {
          // Write did not complete; wait for epoll.
          break;
        }
        // Write completed; remove from queue.
        tx_.pop_front();
      }
      // If there is nothing to transmit; remove EPOLLOUT.
      if (tx_.empty()) {
        device_->registerDescriptor(fd_, EPOLLIN, this);
      }
    }
    if (state_ == CONNECTED && (events & EPOLLIN)) {
      while (read()) {
        // Keep going
      }
    }
    return;
  }

  if (state_ == LISTENING) {
    handleListening();
    return;
  }

  if (state_ == CONNECTING) {
    handleConnecting();
    return;
  }

  GLOO_ENFORCE(false, "Unexpected state: ", state_);
}

void Pair::handleListening() {
  struct sockaddr_storage addr;
  socklen_t addrlen = sizeof(addr);
  int rv;

  rv = accept(fd_, (struct sockaddr*)&addr, &addrlen);

  // Close the listening file descriptor whether we've successfully connected
  // or run into an error and will throw an exception.
  device_->unregisterDescriptor(fd_);
  ::close(fd_);
  fd_ = FD_INVALID;

  if (rv == -1) {
    signalException(GLOO_ERROR_MSG("accept: ", strerror(errno)));
    return;
  }

  // Connected, replace file descriptor
  fd_ = rv;

  // Common connection-made code
  handleConnected();
}

void Pair::handleConnecting() {
  int optval;
  socklen_t optlen = sizeof(optval);
  int rv;

  // Verify that connecting was successful
  rv = getsockopt(fd_, SOL_SOCKET, SO_ERROR, &optval, &optlen);
  GLOO_ENFORCE_NE(rv, -1);
  if (optval != 0) {
    signalException(
        GLOO_ERROR_MSG("connect ", peer_.str(), ": ", strerror(optval)));
    return;
  }

  // Common connection-made code
  handleConnected();
}

void Pair::handleConnected() {
  int rv;

  // Reset addresses
  self_ = Address::fromSockName(fd_);
  peer_ = Address::fromPeerName(fd_);

  // Make sure socket is non-blocking
  setSocketBlocking(fd_, false);

  int flag = 1;
  socklen_t optlen = sizeof(flag);
  rv = setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, optlen);
  GLOO_ENFORCE_NE(rv, -1);

  // Set timeout
  struct timeval tv = {};
  tv.tv_sec = timeout_.count() / 1000;
  tv.tv_usec = (timeout_.count() % 1000) * 1000;
  rv = setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  GLOO_ENFORCE_NE(rv, -1);
  rv = setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  GLOO_ENFORCE_NE(rv, -1);

  device_->registerDescriptor(fd_, EPOLLIN, this);
  changeState(CONNECTED);
}

// getBuffer must only be called when holding lock.
Buffer* Pair::getBuffer(int slot) {
  for (;;) {
    auto it = buffers_.find(slot);
    if (it == buffers_.end()) {
      // The remote peer already sent some bytes destined for the
      // buffer at this slot, but this side of the pair hasn't
      // registed it yet.
      //
      // The current strategy is to return and let the the device loop
      // repeatedly call us again until the buffer has been
      // registered. This essentially means busy waiting while
      // yielding to other pairs. This is not a problem as this only
      // happens at initialization time.
      //
      return nullptr;
    }

    return it->second;
  }
}

void Pair::registerBuffer(Buffer* buf) {
  std::lock_guard<std::mutex> lock(m_);
  GLOO_ENFORCE(
      buffers_.find(buf->slot_) == buffers_.end(),
      "duplicate buffer for slot ",
      buf->slot_);
  buffers_[buf->slot_] = buf;
  cv_.notify_all();
}

void Pair::unregisterBuffer(Buffer* buf) {
  std::lock_guard<std::mutex> lock(m_);
  buffers_.erase(buf->slot_);
}

// changeState must only be called when holding lock.
void Pair::changeState(state nextState) {
  // Ignore nops
  if (nextState == state_) {
    return;
  }

  // State can only move forward
  GLOO_ENFORCE_GT(nextState, state_);

  // Clean up file descriptor when transitioning to CLOSED.
  if (nextState == CLOSED) {
    if (state_ == CONNECTED) {
      if (!sync_) {
        device_->unregisterDescriptor(fd_);
      }
      ::close(fd_);
      fd_ = FD_INVALID;
    } else if (state_ == LISTENING) {
      // The pair may be in the LISTENING state when it is destructed.
      if (fd_ != FD_INVALID) {
        device_->unregisterDescriptor(fd_);
        ::close(fd_);
        fd_ = FD_INVALID;
      }
    } else if (state_ == CONNECTING) {
      // The pair may be in the CONNECTING state when it is destructed.
      if (fd_ != FD_INVALID) {
        device_->unregisterDescriptor(fd_);
        ::close(fd_);
        fd_ = FD_INVALID;
      }
    } else {
      GLOO_ENFORCE(false, "Invalid state: ", state_);
    }
  }

  state_ = nextState;
  cv_.notify_all();
}

void Pair::waitUntilConnected(
    std::unique_lock<std::mutex>& lock,
    bool useTimeout) {
  auto pred = [&] {
    throwIfException();
    return state_ >= CONNECTED;
  };
  auto timeoutSet = timeout_ != kNoTimeout;
  if (useTimeout && timeoutSet) {
    // Use a longer timeout when waiting for initial connect
    auto done = cv_.wait_for(lock, timeout_ * 5, pred);
    if (!done) {
      signalAndThrowException(GLOO_ERROR_MSG("Connect timeout ", peer_.str()));
    }
  } else {
    cv_.wait(lock, pred);
  }
}

void Pair::verifyConnected() {
  // This code path should only be called after reaching the connected state
  GLOO_ENFORCE_GE(
      state_,
      CONNECTED,
      "Pair is not connected (",
      self_.str(),
      " <--> ",
      peer_.str(),
      ")");
  // Check if the socket has been closed. We were unable to tell if this was an
  // error or normal tear down, but now throw since we are trying to do IO.
  if (state_ == CLOSED) {
    signalAndThrowException(GLOO_ERROR_MSG("Socket closed ", peer_.str()));
  }
}

// Sends contents of operation to the remote side of the pair.
// The pair's mutex is held when this function is called.
// Only applicable to synchronous mode. May block.
void Pair::sendSyncMode(Op& op) {
  GLOO_ENFORCE(sync_);
  auto rv = write(op);
  if (!rv) {
    GLOO_ENFORCE(ex_ != nullptr);
    std::rethrow_exception(ex_);
  }
}

// Sends contents of operation to the remote side of the pair.
// The pair's mutex is held when this function is called.
// Only applicable to asynchronous mode. Never blocks.
void Pair::sendAsyncMode(Op& op) {
  GLOO_ENFORCE(!sync_);

  // If an earlier operation hasn't finished transmitting,
  // add this operation to the transmit queue.
  if (!tx_.empty()) {
    tx_.push_back(std::move(op));
    return;
  }

  // Write in place without checking socket for writeability.
  // This is the fast path.
  if (write(op)) {
    return;
  }

  // Write may have resulted in an error.
  throwIfException();

  // Write didn't complete; pass to event loop
  tx_.push_back(std::move(op));
  device_->registerDescriptor(fd_, EPOLLIN | EPOLLOUT, this);
}

void Pair::send(Op& op) {
  std::unique_lock<std::mutex> lock(m_);
  throwIfException();
  verifyConnected();

  // Try to size the send buffer such that the write below completes
  // synchronously and we don't need to finish the write later.
  size_t size = std::min(op.preamble.nbytes, kMaxSendBufferSize);
  if (sendBufferSize_ < size) {
    int rv;
    size_t optval = size;
    socklen_t optlen = sizeof(optval);
    rv = setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &optval, optlen);
    GLOO_ENFORCE_NE(rv, -1);
    rv = getsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &optval, &optlen);
    GLOO_ENFORCE_NE(rv, -1);
    sendBufferSize_ = optval;
  }

  // Write to socket
  if (sync_) {
    sendSyncMode(op);
  } else {
    sendAsyncMode(op);
  }
}

void Pair::recv() {
  std::unique_lock<std::mutex> lock(m_);
  throwIfException();
  verifyConnected();

  auto rv = read();
  if (!rv) {
    GLOO_ENFORCE(
        ex_ != nullptr, "read() returned false in sync mode; ex_ must be set");
    std::rethrow_exception(ex_);
  }
}

std::unique_ptr<::gloo::transport::Buffer> Pair::createSendBuffer(
    int slot,
    void* ptr,
    size_t size) {
  auto buffer = new Buffer(this, slot, ptr, size);
  return std::unique_ptr<::gloo::transport::Buffer>(buffer);
}

std::unique_ptr<::gloo::transport::Buffer> Pair::createRecvBuffer(
    int slot,
    void* ptr,
    size_t size) {
  auto buffer = new Buffer(this, slot, ptr, size);
  registerBuffer(buffer);
  return std::unique_ptr<::gloo::transport::Buffer>(buffer);
}

// Send from the specified buffer to remote side of pair.
void Pair::send(
    transport::UnboundBuffer* tbuf,
    uint64_t slot,
    size_t offset,
    size_t nbytes) {
  auto buf = static_cast<tcp::UnboundBuffer*>(tbuf)->getWeakNonOwningPtr();

  GLOO_ENFORCE_GE(offset, 0);
  GLOO_ENFORCE_LE(offset, tbuf->size);
  GLOO_ENFORCE_GE(nbytes, 0);
  GLOO_ENFORCE_LE(nbytes, tbuf->size - offset);

  std::unique_lock<std::mutex> lock(m_);
  throwIfException();

  // Execute this send if there is a remote pending receive.
  ContextMutator mutator(*context_, slot, rank_);
  if (mutator.getRemotePendingRecv() > 0) {
    // We keep a count of remote pending send and receive operations.
    // In this code path the remote side hasn't seen a notification
    // for this send operation yet so we need to take special care
    // their count is updated regardless.
    sendNotifySendReady(slot, nbytes);
    sendUnboundBuffer(std::move(buf), slot, offset, nbytes);
    mutator.updateRemotePendingRecv(-1);
    return;
  }

  // Notify peer of this pending send.
  localPendingSend_[slot].push_back(std::make_tuple(buf, offset, nbytes));
  sendNotifySendReady(slot, nbytes);
}

// Receive into the specified buffer from the remote side of pair.
void Pair::recv(
    transport::UnboundBuffer* tbuf,
    uint64_t slot,
    size_t offset,
    size_t nbytes) {
  auto buf = static_cast<tcp::UnboundBuffer*>(tbuf)->getWeakNonOwningPtr();

  GLOO_ENFORCE_GE(offset, 0);
  GLOO_ENFORCE_LE(offset, tbuf->size);
  GLOO_ENFORCE_GE(nbytes, 0);
  GLOO_ENFORCE_LE(nbytes, tbuf->size - offset);

  std::unique_lock<std::mutex> lock(m_);
  throwIfException();

  // Notify peer of this pending recv.
  localPendingRecv_[slot].push_back(std::make_tuple(buf, offset, nbytes));
  sendNotifyRecvReady(slot, nbytes);

  // Decrease balance of remote pending sends.
  // This results in a negative count if executed before
  // receiving the send notification from our peer.
  ContextMutator mutator(*context_, slot, rank_);
  mutator.updateRemotePendingSend(-1);
}

bool Pair::tryRecv(
    transport::UnboundBuffer* tbuf,
    uint64_t slot,
    size_t offset,
    size_t nbytes) {
  auto buf = static_cast<tcp::UnboundBuffer*>(tbuf)->getWeakNonOwningPtr();

  GLOO_ENFORCE_GE(offset, 0);
  GLOO_ENFORCE_LT(offset, tbuf->size);
  GLOO_ENFORCE_GT(nbytes, 0);
  GLOO_ENFORCE_LE(nbytes, tbuf->size - offset);

  std::unique_lock<std::mutex> lock(m_);
  throwIfException();

  // Return early if there is no remote pending send.
  ContextMutator mutator(*context_, slot, rank_);
  if (mutator.getRemotePendingSend() <= 0) {
    return false;
  }

  // Notify peer of this pending recv.
  localPendingRecv_[slot].push_back(std::make_tuple(buf, offset, nbytes));
  sendNotifyRecvReady(slot, nbytes);

  // Decrease balance of remote pending sends.
  mutator.updateRemotePendingSend(-1);
  return true;
}

void Pair::sendUnboundBuffer(
    WeakNonOwningPtr<UnboundBuffer> buf,
    uint64_t slot,
    size_t offset,
    size_t nbytes) {
  Op op;
  op.preamble.nbytes = sizeof(op.preamble) + nbytes;
  op.preamble.opcode = Op::SEND_UNBOUND_BUFFER;
  op.preamble.slot = slot;
  op.preamble.length = nbytes;
  op.ubuf = std::move(buf);
  op.offset = offset;
  op.nbytes = nbytes;
  sendAsyncMode(op);
}

void Pair::sendNotifyRecvReady(uint64_t slot, size_t nbytes) {
  Op op;
  op.preamble.nbytes = sizeof(op.preamble);
  op.preamble.opcode = Op::NOTIFY_RECV_READY;
  op.preamble.slot = slot;
  op.preamble.length = nbytes;
  sendAsyncMode(op);
}

void Pair::sendNotifySendReady(uint64_t slot, size_t nbytes) {
  Op op;
  op.preamble.nbytes = sizeof(op.preamble);
  op.preamble.opcode = Op::NOTIFY_SEND_READY;
  op.preamble.slot = slot;
  op.preamble.length = nbytes;
  sendAsyncMode(op);
}

void Pair::throwIfException() {
  // If we previously encountered an error, rethrow here.
  if (ex_ != nullptr) {
    std::rethrow_exception(ex_);
  }
}

std::exception_ptr Pair::signalExceptionExternal(const std::string& msg) {
  std::lock_guard<std::mutex> lock(m_);

  // This function may be called by a buffer upon timing out waiting
  // for some operation to complete. It may race with the device
  // thread performing read/write operations, detecting a failure, and
  // setting an exception as well. Therefore, check if an exception is
  // already set, and ignore this call if it is.
  if (ex_ == nullptr) {
    signalException(msg);
  }
  return ex_;
}

void Pair::signalException(const std::string& msg) {
  signalException(std::make_exception_ptr(::gloo::IoException(msg)));
}

void Pair::signalException(std::exception_ptr ex) {
  GLOO_ENFORCE(ex_ == nullptr);

  // Loop through the buffers and signal that an error has occurred.
  for (auto it = buffers_.begin(); it != buffers_.end(); it++) {
    it->second->signalException(ex);
  }

  // Loop through posted send operations.
  for (auto& op : tx_) {
    if (op.buf != nullptr) {
      op.buf->signalException(ex);
    }
  }

  // Loop through pending send operations.
  for (auto& it : localPendingSend_) {
    for (auto& op : it.second) {
      NonOwningPtr<UnboundBuffer> buf(std::get<0>(op));
      if (buf) {
        buf->signalException(ex);
      }
    }
  }

  // Loop through pending recv operations.
  for (auto& it : localPendingRecv_) {
    for (auto& op : it.second) {
      NonOwningPtr<UnboundBuffer> buf(std::get<0>(op));
      if (buf) {
        buf->signalException(ex);
      }
    }
  }

  // Store exception_ptr and signal any threads in the async path.
  ex_ = ex;
  cv_.notify_all();

  // Move to closed state.
  // Either this error is an underlying socket error and the socket
  // must be closed, or this error is an application side timeout, and
  // we are no longer guaranteed that buffer pointers will be valid.
  changeState(CLOSED);
}

void Pair::signalAndThrowException(const std::string& msg) {
  signalAndThrowException(std::make_exception_ptr(::gloo::IoException(msg)));
}

void Pair::signalAndThrowException(std::exception_ptr ex) {
  signalException(ex);
  std::rethrow_exception(ex);
}

} // namespace tcp
} // namespace transport
} // namespace gloo

/*
 *  Copyright (c) 2017, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "AsyncMcClientImpl.h"

#include <netinet/tcp.h>

#include <folly/EvictingCacheMap.h>
#include <folly/Memory.h>
#include <folly/SingletonThreadLocal.h>
#include <folly/io/async/AsyncSSLSocket.h>
#include <folly/io/async/EventBase.h>

#include "mcrouter/lib/debug/FifoManager.h"
#include "mcrouter/lib/fbi/cpp/LogFailure.h"

namespace facebook {
namespace memcache {

constexpr size_t kReadBufferSizeMin = 256;
constexpr size_t kReadBufferSizeMax = 4096;

namespace {
class OnEventBaseDestructionCallback : public folly::EventBase::LoopCallback {
 public:
  explicit OnEventBaseDestructionCallback(AsyncMcClientImpl& client)
      : client_(client) {}
  ~OnEventBaseDestructionCallback() {}
  void runLoopCallback() noexcept override final {
    client_.closeNow();
  }

 private:
  AsyncMcClientImpl& client_;
};

struct GoAwayContext : public folly::AsyncTransportWrapper::WriteCallback {
  GoAwayAcknowledgement message;
  McSerializedRequest data;
  std::unique_ptr<GoAwayContext> selfPtr;

  explicit GoAwayContext(const CodecIdRange& supportedCodecs)
      : data(message, 0, mc_caret_protocol, supportedCodecs) {}

  void writeSuccess() noexcept override final {
    auto self = std::move(selfPtr);
    self.reset();
  }
  void writeErr(size_t, const folly::AsyncSocketException&) noexcept override
      final {
    auto self = std::move(selfPtr);
    self.reset();
  }
};
} // anonymous

/**
 * A callback class for network writing.
 *
 * We use it instead of simple std::function, because it will safely cancel
 * callback event when destructed.
 */
class AsyncMcClientImpl::WriterLoop : public folly::EventBase::LoopCallback {
 public:
  explicit WriterLoop(AsyncMcClientImpl& client) : client_(client) {}
  ~WriterLoop() {}
  void runLoopCallback() noexcept override final {
    // Delay this write until the end of current loop (e.g. after
    // runActiveFibers() callback). That way we achieve better batching without
    // affecting latency.
    if (!rescheduled_) {
      rescheduled_ = true;
      client_.eventBase_.runInLoop(this, /* thisIteration */ true);
      return;
    }
    rescheduled_ = false;
    client_.pushMessages();
  }

 private:
  bool rescheduled_{false};
  AsyncMcClientImpl& client_;
};

AsyncMcClientImpl::AsyncMcClientImpl(
    folly::VirtualEventBase& eventBase,
    ConnectionOptions options)
    : eventBase_(eventBase.getEventBase()),
      connectionOptions_(std::move(options)),
      outOfOrder_(
          connectionOptions_.accessPoint->getProtocol() != mc_ascii_protocol),
      queue_(outOfOrder_),
      writer_(std::make_unique<WriterLoop>(*this)),
      eventBaseDestructionCallback_(
          std::make_unique<OnEventBaseDestructionCallback>(*this)) {
  eventBase.runOnDestruction(eventBaseDestructionCallback_.get());
  if (connectionOptions_.compressionCodecMap) {
    supportedCompressionCodecs_ =
        connectionOptions_.compressionCodecMap->getIdRange();
  }
}

std::shared_ptr<AsyncMcClientImpl> AsyncMcClientImpl::create(
    folly::VirtualEventBase& eventBase,
    ConnectionOptions options) {
  auto client = std::shared_ptr<AsyncMcClientImpl>(
      new AsyncMcClientImpl(eventBase, std::move(options)), Destructor());
  client->selfPtr_ = client;
  return client;
}

void AsyncMcClientImpl::closeNow() {
  DestructorGuard dg(this);

  if (socket_) {
    isAborting_ = true;
    // We need to destroy it immediately.
    socket_->closeNow();
    socket_.reset();
    isAborting_ = false;
  }
}

void AsyncMcClientImpl::setStatusCallbacks(
    std::function<void()> onUp,
    std::function<void(ConnectionDownReason)> onDown) {
  DestructorGuard dg(this);

  statusCallbacks_ =
      ConnectionStatusCallbacks{std::move(onUp), std::move(onDown)};

  if (connectionState_ == ConnectionState::UP && statusCallbacks_.onUp) {
    statusCallbacks_.onUp();
  }
}

void AsyncMcClientImpl::setRequestStatusCallbacks(
    std::function<void(int pendingDiff, int inflightDiff)> onStateChange,
    std::function<void(int numToSend)> onWrite) {
  DestructorGuard dg(this);

  requestStatusCallbacks_ =
      RequestStatusCallbacks{std::move(onStateChange), std::move(onWrite)};
}

AsyncMcClientImpl::~AsyncMcClientImpl() {
  assert(getPendingRequestCount() == 0);
  assert(getInflightRequestCount() == 0);
  if (socket_) {
    // Close the socket immediately. We need to process all callbacks, such as
    // readEOF and connectError, before we exit destructor.
    socket_->closeNow();
  }
  eventBaseDestructionCallback_.reset();
}

size_t AsyncMcClientImpl::getPendingRequestCount() const {
  return queue_.getPendingRequestCount();
}

size_t AsyncMcClientImpl::getInflightRequestCount() const {
  return queue_.getInflightRequestCount();
}

void AsyncMcClientImpl::setThrottle(size_t maxInflight, size_t maxPending) {
  maxInflight_ = maxInflight;
  maxPending_ = maxPending;
}

void AsyncMcClientImpl::sendCommon(McClientRequestContextBase& req) {
  switch (req.reqContext.serializationResult()) {
    case McSerializedRequest::Result::OK:
      incMsgId(nextMsgId_);

      queue_.markAsPending(req);
      scheduleNextWriterLoop();
      if (connectionState_ == ConnectionState::DOWN) {
        attemptConnection();
      }
      return;
    case McSerializedRequest::Result::BAD_KEY:
      req.replyError(mc_res_bad_key, "The key provided is invalid");
      return;
    case McSerializedRequest::Result::ERROR:
      req.replyError(mc_res_local_error, "Error when serializing the request.");
      return;
  }
}

size_t AsyncMcClientImpl::getNumToSend() const {
  size_t numToSend = queue_.getPendingRequestCount();
  if (maxInflight_ != 0) {
    if (maxInflight_ <= getInflightRequestCount()) {
      numToSend = 0;
    } else {
      numToSend = std::min(numToSend, maxInflight_ - getInflightRequestCount());
    }
  }
  return numToSend;
}

void AsyncMcClientImpl::scheduleNextWriterLoop() {
  if (connectionState_ == ConnectionState::UP && !writeScheduled_ &&
      (getNumToSend() > 0 || pendingGoAwayReply_)) {
    writeScheduled_ = true;
    eventBase_.runInLoop(writer_.get());
  }
}

void AsyncMcClientImpl::cancelWriterCallback() {
  writeScheduled_ = false;
  writer_->cancelLoopCallback();
}

void AsyncMcClientImpl::pushMessages() {
  DestructorGuard dg(this);

  assert(connectionState_ == ConnectionState::UP);
  auto numToSend = getNumToSend();
  // Call batch status callback
  if (requestStatusCallbacks_.onWrite && numToSend > 0) {
    requestStatusCallbacks_.onWrite(numToSend);
  }

  while (getPendingRequestCount() != 0 && numToSend > 0 &&
         /* we might be already not UP, because of failed writev */
         connectionState_ == ConnectionState::UP) {
    auto& req = queue_.markNextAsSending();

    auto iov = req.reqContext.getIovs();
    auto iovcnt = req.reqContext.getIovsCount();
    if (debugFifo_.isConnected()) {
      debugFifo_.startMessage(MessageDirection::Sent, req.reqContext.typeId());
      debugFifo_.writeData(iov, iovcnt);
    }
    socket_->writev(
        this,
        iov,
        iovcnt,
        numToSend == 1 ? folly::WriteFlags::NONE : folly::WriteFlags::CORK);
    --numToSend;
  }
  if (connectionState_ == ConnectionState::UP && pendingGoAwayReply_) {
    // Note: we're not waiting for all requests to be sent, since that may take
    // a while and if we didn't succeed in one loop, this means that we're
    // already backlogged.
    sendGoAwayReply();
  }
  pendingGoAwayReply_ = false;
  writeScheduled_ = false;
  scheduleNextWriterLoop();
}

void AsyncMcClientImpl::sendGoAwayReply() {
  auto ctxPtr = std::make_unique<GoAwayContext>(supportedCompressionCodecs_);
  auto& ctx = *ctxPtr;
  switch (ctx.data.serializationResult()) {
    case McSerializedRequest::Result::OK: {
      auto iov = ctx.data.getIovs();
      auto iovcnt = ctx.data.getIovsCount();
      // Pass context ownership of itself, writev will call a callback that
      // will destroy the context.
      ctx.selfPtr = std::move(ctxPtr);
      socket_->writev(&ctx, iov, iovcnt);
      break;
    }
    default:
      // Ignore errors on GoAway.
      break;
  }
}

namespace {

void createTCPKeepAliveOptions(
    folly::AsyncSocket::OptionMap& options,
    int cnt,
    int idle,
    int interval) {
  // 0 means KeepAlive is disabled.
  if (cnt != 0) {
#ifdef SO_KEEPALIVE
    folly::AsyncSocket::OptionMap::key_type key;
    key.level = SOL_SOCKET;
    key.optname = SO_KEEPALIVE;
    options[key] = 1;

    key.level = IPPROTO_TCP;

#ifdef TCP_KEEPCNT
    key.optname = TCP_KEEPCNT;
    options[key] = cnt;
#endif // TCP_KEEPCNT

#ifdef TCP_KEEPIDLE
    key.optname = TCP_KEEPIDLE;
    options[key] = idle;
#endif // TCP_KEEPIDLE

#ifdef TCP_KEEPINTVL
    key.optname = TCP_KEEPINTVL;
    options[key] = interval;
#endif // TCP_KEEPINTVL

#endif // SO_KEEPALIVE
  }
}

const folly::AsyncSocket::OptionKey getQoSOptionKey(sa_family_t addressFamily) {
  static const folly::AsyncSocket::OptionKey kIpv4OptKey = {IPPROTO_IP, IP_TOS};
  static const folly::AsyncSocket::OptionKey kIpv6OptKey = {IPPROTO_IPV6,
                                                            IPV6_TCLASS};
  return (addressFamily == AF_INET) ? kIpv4OptKey : kIpv6OptKey;
}

uint64_t getQoS(uint64_t qosClassLvl, uint64_t qosPathLvl) {
  // class
  static const uint64_t kDefaultClass = 0x00;
  static const uint64_t kLowestClass = 0x20;
  static const uint64_t kMediumClass = 0x40;
  static const uint64_t kHighClass = 0x60;
  static const uint64_t kHighestClass = 0x80;
  static const uint64_t kQoSClasses[] = {
      kDefaultClass, kLowestClass, kMediumClass, kHighClass, kHighestClass};

  // path
  static const uint64_t kAnyPathNoProtection = 0x00;
  static const uint64_t kAnyPathProtection = 0x04;
  static const uint64_t kShortestPathNoProtection = 0x08;
  static const uint64_t kShortestPathProtection = 0x0c;
  static const uint64_t kQoSPaths[] = {kAnyPathNoProtection,
                                       kAnyPathProtection,
                                       kShortestPathNoProtection,
                                       kShortestPathProtection};

  if (qosClassLvl > 4) {
    qosClassLvl = 0;
    LOG_FAILURE(
        "AsyncMcClient",
        failure::Category::kSystemError,
        "Invalid QoS class value in AsyncMcClient");
  }

  if (qosPathLvl > 3) {
    qosPathLvl = 0;
    LOG_FAILURE(
        "AsyncMcClient",
        failure::Category::kSystemError,
        "Invalid QoS path value in AsyncMcClient");
  }

  return kQoSClasses[qosClassLvl] | kQoSPaths[qosPathLvl];
}

void createQoSClassOption(
    folly::AsyncSocket::OptionMap& options,
    const sa_family_t addressFamily,
    uint64_t qosClass,
    uint64_t qosPath) {
  const auto& optkey = getQoSOptionKey(addressFamily);
  options[optkey] = getQoS(qosClass, qosPath);
}

void checkWhetherQoSIsApplied(
    const folly::SocketAddress& address,
    int socketFd,
    const ConnectionOptions& connectionOptions) {
  const auto& optkey = getQoSOptionKey(address.getFamily());

  const uint64_t expectedValue =
      getQoS(connectionOptions.qosClass, connectionOptions.qosPath);

  uint64_t val = 0;
  socklen_t len = sizeof(expectedValue);
  int rv = getsockopt(socketFd, optkey.level, optkey.optname, &val, &len);
  // Zero out last 2 bits as they are not used for the QOS value
  constexpr uint64_t kMaskTwoLeastSignificantBits = 0xFFFFFFFc;
  val = val & kMaskTwoLeastSignificantBits;
  if (rv != 0 || val != expectedValue) {
    LOG_FAILURE(
        "AsyncMcClient",
        failure::Category::kSystemError,
        "Failed to apply QoS! "
        "Return Value: {} (expected: {}). "
        "QoS Value: {} (expected: {}).",
        rv,
        0,
        val,
        expectedValue);
  }
}

folly::AsyncSocket::OptionMap createSocketOptions(
    const folly::SocketAddress& address,
    const ConnectionOptions& connectionOptions) {
  folly::AsyncSocket::OptionMap options;

  createTCPKeepAliveOptions(
      options,
      connectionOptions.tcpKeepAliveCount,
      connectionOptions.tcpKeepAliveIdle,
      connectionOptions.tcpKeepAliveInterval);
  if (connectionOptions.enableQoS) {
    createQoSClassOption(
        options,
        address.getFamily(),
        connectionOptions.qosClass,
        connectionOptions.qosPath);
  }

  return options;
}

/////////////////////////////  SslSessionCache //////////////////////////////

class SslSessionDestructor {
 public:
  void operator()(SSL_SESSION* session) {
    if (session != nullptr) {
      SSL_SESSION_free(session);
    }
  }
};

using SslSessionUniquePtr = std::unique_ptr<SSL_SESSION, SslSessionDestructor>;

using SslSessionCache =
    folly::EvictingCacheMap<std::string, SslSessionUniquePtr>;

SslSessionCache& sslSessionCache() {
  constexpr size_t kCacheSize = 10000;
  static folly::SingletonThreadLocal<SslSessionCache> cache(
      []() { return new SslSessionCache(kCacheSize); });
  return cache.get();
}

std::string getSessionCacheKey(const AccessPoint& ap) {
  return ap.toHostPortString();
}

void storeSslSession(const AccessPoint& ap, SslSessionUniquePtr session) {
  const auto& key = getSessionCacheKey(ap);
  if (session != nullptr) {
    sslSessionCache().set(key, std::move(session));
  }
}

void removeSslSession(const AccessPoint& ap) {
  const auto& key = getSessionCacheKey(ap);
  sslSessionCache().erase(key);
}

SSL_SESSION* getSslSession(const AccessPoint& ap) {
  const auto& key = getSessionCacheKey(ap);
  auto it = sslSessionCache().find(key);
  return it != sslSessionCache().end() ? it->second.get() : nullptr;
}

///////////////////////////////////////////////////////////////////////////

} // anonymous namespace

void AsyncMcClientImpl::attemptConnection() {
  // We may use a lot of stack memory (e.g. hostname resolution) or some
  // expensive SSL code. This should be always executed on main context.
  folly::fibers::runInMainContext([this] {
    assert(connectionState_ == ConnectionState::DOWN);

    connectionState_ = ConnectionState::CONNECTING;
    pendingGoAwayReply_ = false;

    if (connectionOptions_.sslContextProvider) {
      auto sslContext = connectionOptions_.sslContextProvider();
      if (!sslContext) {
        connectErr(folly::AsyncSocketException(
            folly::AsyncSocketException::SSL_ERROR,
            "SSLContext provider returned nullptr, "
            "check SSL certificates"));
        return;
      }

      auto sslSocket = new folly::AsyncSSLSocket(sslContext, &eventBase_);
      if (connectionOptions_.sessionCachingEnabled) {
        /* If we have an existing session try to re-use */
        auto* session = getSslSession(*connectionOptions_.accessPoint);
        sslSocket->setSSLSession(session);
      }
      socket_.reset(sslSocket);
    } else {
      socket_.reset(new folly::AsyncSocket(&eventBase_));
    }

    folly::SocketAddress address;
    try {
      address = folly::SocketAddress(
          connectionOptions_.accessPoint->getHost(),
          connectionOptions_.accessPoint->getPort(),
          /* allowNameLookup */ true);
    } catch (const std::system_error& e) {
      LOG_FAILURE(
          "AsyncMcClient", failure::Category::kBadEnvironment, "{}", e.what());
      connectErr(folly::AsyncSocketException(
          folly::AsyncSocketException::NOT_OPEN, ""));
      return;
    }

    auto socketOptions = createSocketOptions(address, connectionOptions_);

    socket_->setSendTimeout(connectionOptions_.writeTimeout.count());
    socket_->connect(
        this, address, connectionOptions_.writeTimeout.count(), socketOptions);

    // If AsyncSocket::connect() fails, socket_ may have been reset
    if (socket_ && connectionOptions_.enableQoS) {
      checkWhetherQoSIsApplied(address, socket_->getFd(), connectionOptions_);
    }
  });
}

void AsyncMcClientImpl::connectSuccess() noexcept {
  assert(connectionState_ == ConnectionState::CONNECTING);
  DestructorGuard dg(this);
  connectionState_ = ConnectionState::UP;

  if (statusCallbacks_.onUp) {
    statusCallbacks_.onUp();
  }

  if (!connectionOptions_.debugFifoPath.empty()) {
    if (auto fifoManager = FifoManager::getInstance()) {
      if (auto fifo =
              fifoManager->fetchThreadLocal(connectionOptions_.debugFifoPath)) {
        debugFifo_ = ConnectionFifo(
            std::move(fifo), socket_.get(), connectionOptions_.routerInfoName);
      }
    }
  }

  if (connectionOptions_.sslContextProvider &&
      connectionOptions_.sessionCachingEnabled) {
    auto* sslSocket = socket_->getUnderlyingTransport<folly::AsyncSSLSocket>();
    assert(sslSocket != nullptr);
    if (!sslSocket->getSSLSessionReused()) {
      /* store the new ssl session for future re-use */
      auto session = SslSessionUniquePtr(sslSocket->getSSLSession());
      storeSslSession(*connectionOptions_.accessPoint, std::move(session));
    }
  }

  assert(getInflightRequestCount() == 0);
  assert(queue_.getParserInitializer() == nullptr);

  scheduleNextWriterLoop();
  parser_ = std::make_unique<ParserT>(
      *this,
      kReadBufferSizeMin,
      kReadBufferSizeMax,
      connectionOptions_.useJemallocNodumpAllocator,
      connectionOptions_.compressionCodecMap,
      &debugFifo_);
  socket_->setReadCB(this);
}

void AsyncMcClientImpl::connectErr(
    const folly::AsyncSocketException& ex) noexcept {
  assert(connectionState_ == ConnectionState::CONNECTING);
  DestructorGuard dg(this);

  if (connectionOptions_.sslContextProvider &&
      connectionOptions_.sessionCachingEnabled) {
    /* clear the ssl session from cache */
    removeSslSession(*connectionOptions_.accessPoint);
  }

  std::string errorMessage;
  if (ex.getType() == folly::AsyncSocketException::SSL_ERROR) {
    errorMessage = folly::sformat(
        "SSLError: {}. Connect to {} failed.",
        ex.what(),
        connectionOptions_.accessPoint->toHostPortString());
    LOG_FAILURE(
        "AsyncMcClient", failure::Category::kBadEnvironment, errorMessage);
  }

  mc_res_t error = mc_res_connect_error;
  ConnectionDownReason reason = ConnectionDownReason::CONNECT_ERROR;
  if (ex.getType() == folly::AsyncSocketException::TIMED_OUT) {
    error = mc_res_connect_timeout;
    reason = ConnectionDownReason::CONNECT_TIMEOUT;
    errorMessage = "Timed out when trying to connect to server";
  } else if (isAborting_) {
    error = mc_res_aborted;
    reason = ConnectionDownReason::ABORTED;
    errorMessage = "Connection aborted";
  }

  assert(getInflightRequestCount() == 0);
  queue_.failAllPending(error, errorMessage);
  connectionState_ = ConnectionState::DOWN;
  // We don't need it anymore, so let it perform complete cleanup.
  socket_.reset();

  if (statusCallbacks_.onDown) {
    statusCallbacks_.onDown(reason);
  }
}

void AsyncMcClientImpl::processShutdown(folly::StringPiece errorMessage) {
  DestructorGuard dg(this);
  switch (connectionState_) {
    case ConnectionState::UP: // on error, UP always transitions to ERROR state
      if (writeScheduled_) {
        // Cancel loop callback, or otherwise we might attempt to write
        // something while processing error state.
        cancelWriterCallback();
      }
      connectionState_ = ConnectionState::ERROR;
      // We're already in ERROR state, no need to listen for reads.
      socket_->setReadCB(nullptr);
      // We can safely close connection, it will stop all writes.
      socket_->close();

    /* fallthrough */

    case ConnectionState::ERROR:
      queue_.failAllSent(
          isAborting_ ? mc_res_aborted : mc_res_remote_error, errorMessage);
      if (queue_.getInflightRequestCount() == 0) {
        // No need to send any of remaining requests if we're aborting.
        if (isAborting_) {
          queue_.failAllPending(mc_res_aborted, errorMessage);
        }

        // This is a last processShutdown() for this error and it is safe
        // to go DOWN.
        if (statusCallbacks_.onDown) {
          statusCallbacks_.onDown(
              isAborting_ ? ConnectionDownReason::ABORTED
                          : ConnectionDownReason::ERROR);
        }

        connectionState_ = ConnectionState::DOWN;
        // We don't need it anymore, so let it perform complete cleanup.
        socket_.reset();

        // In case we still have some pending requests, then try reconnecting
        // immediately.
        if (getPendingRequestCount() != 0) {
          attemptConnection();
        }
      }
      return;
    case ConnectionState::CONNECTING:
    // connectError is not a remote error, it's processed in connectError.
    case ConnectionState::DOWN:
      // We shouldn't have any errors while not connected.
      CHECK(false);
  }
}

void AsyncMcClientImpl::getReadBuffer(void** bufReturn, size_t* lenReturn) {
  curBuffer_ = parser_->getReadBuffer();
  *bufReturn = curBuffer_.first;
  *lenReturn = curBuffer_.second;
}

void AsyncMcClientImpl::readDataAvailable(size_t len) noexcept {
  assert(curBuffer_.first != nullptr && curBuffer_.second >= len);
  DestructorGuard dg(this);
  parser_->readDataAvailable(len);
}

void AsyncMcClientImpl::readEOF() noexcept {
  assert(connectionState_ == ConnectionState::UP);
  processShutdown("Connection closed by the server.");
}

void AsyncMcClientImpl::readErr(
    const folly::AsyncSocketException& ex) noexcept {
  assert(connectionState_ == ConnectionState::UP);
  std::string errorMessage = folly::sformat(
      "Failed to read from socket with remote endpoint \"{}\". Exception: {}",
      connectionOptions_.accessPoint->toString(),
      ex.what());
  VLOG(1) << errorMessage;
  processShutdown(errorMessage);
}

void AsyncMcClientImpl::writeSuccess() noexcept {
  assert(
      connectionState_ == ConnectionState::UP ||
      connectionState_ == ConnectionState::ERROR);
  DestructorGuard dg(this);
  auto& req = queue_.markNextAsSent();
  req.scheduleTimeout();

  // It is possible that we're already processing error, but still have a
  // successfull write.
  if (connectionState_ == ConnectionState::ERROR) {
    processShutdown("Connection was in ERROR state.");
  }
}

void AsyncMcClientImpl::writeErr(
    size_t bytesWritten,
    const folly::AsyncSocketException& ex) noexcept {
  assert(
      connectionState_ == ConnectionState::UP ||
      connectionState_ == ConnectionState::ERROR);

  std::string errorMessage = folly::sformat(
      "Failed to write into socket with remote endpoint \"{}\", "
      "wrote {} bytes. Exception: {}",
      connectionOptions_.accessPoint->toString(),
      bytesWritten,
      ex.what());
  VLOG(1) << errorMessage;

  // We're already in an error state, so all requests in pendingReplyQueue_ will
  // be replied with an error.
  queue_.markNextAsSent();
  processShutdown(errorMessage);
}

folly::StringPiece AsyncMcClientImpl::clientStateToStr() const {
  switch (connectionState_) {
    case ConnectionState::UP:
      return "UP";
    case ConnectionState::DOWN:
      return "DOWN";
    case ConnectionState::CONNECTING:
      return "CONNECTING";
    case ConnectionState::ERROR:
      return "ERROR";
  }
  return "state is incorrect";
}

void AsyncMcClientImpl::logErrorWithContext(folly::StringPiece reason) {
  LOG_FAILURE(
      "AsyncMcClient",
      failure::Category::kOther,
      "Error: \"{}\", client state: {}, remote endpoint: {}, "
      "number of requests sent through this client: {}, "
      "McClientRequestContextQueue info: {}",
      reason,
      clientStateToStr(),
      connectionOptions_.accessPoint->toString(),
      nextMsgId_,
      queue_.debugInfo());
}

void AsyncMcClientImpl::handleConnectionControlMessage(
    const UmbrellaMessageInfo& headerInfo) {
  DestructorGuard dg(this);
  // Handle go away request.
  switch (headerInfo.typeId) {
    case GoAwayRequest::typeId: {
      // No need to process GoAway if the connection is already closing.
      if (connectionState_ != ConnectionState::UP) {
        break;
      }
      if (statusCallbacks_.onDown) {
        statusCallbacks_.onDown(ConnectionDownReason::SERVER_GONE_AWAY);
      }
      pendingGoAwayReply_ = true;
      scheduleNextWriterLoop();
      break;
    }
    default:
      // Ignore unknown control messages.
      break;
  }
}

void AsyncMcClientImpl::parseError(mc_res_t result, folly::StringPiece reason) {
  logErrorWithContext(reason);
  // mc_parser can call the parseError multiple times, process only first.
  if (connectionState_ != ConnectionState::UP) {
    return;
  }
  DestructorGuard dg(this);
  processShutdown(reason);
}

bool AsyncMcClientImpl::nextReplyAvailable(uint64_t reqId) {
  assert(connectionState_ == ConnectionState::UP);

  auto initializer = queue_.getParserInitializer(reqId);

  if (initializer) {
    (*initializer)(*parser_);
    return true;
  }

  return false;
}

void AsyncMcClientImpl::incMsgId(uint32_t& msgId) {
  msgId += 2;
}

void AsyncMcClientImpl::updateWriteTimeout(std::chrono::milliseconds timeout) {
  if (!timeout.count()) {
    return;
  }
  auto selfWeak = selfPtr_;
  eventBase_.runInEventBaseThread([selfWeak, timeout]() {
    if (auto self = selfWeak.lock()) {
      if (!self->connectionOptions_.writeTimeout.count() ||
          self->connectionOptions_.writeTimeout > timeout) {
        self->connectionOptions_.writeTimeout = timeout;
      }

      if (self->socket_) {
        self->socket_->setSendTimeout(
            self->connectionOptions_.writeTimeout.count());
      }
    }
  });
}

double AsyncMcClientImpl::getRetransmissionInfo() {
  if (socket_ != nullptr) {
    struct tcp_info tcpinfo;
    socklen_t len = sizeof(struct tcp_info);

    auto& socket = dynamic_cast<folly::AsyncSocket&>(*socket_);

    if (socket.getSockOpt(IPPROTO_TCP, TCP_INFO, &tcpinfo, &len) == 0) {
      const uint64_t totalKBytes = socket.getRawBytesWritten() / 1000;
      if (totalKBytes == lastKBytes_) {
        return 0.0;
      }
      const auto retransPerKByte = (tcpinfo.tcpi_total_retrans - lastRetrans_) /
          (double)(totalKBytes - lastKBytes_);
      lastKBytes_ = totalKBytes;
      lastRetrans_ = tcpinfo.tcpi_total_retrans;
      return retransPerKByte;
    }
  }
  return -1.0;
}
} // memcache
} // facebook

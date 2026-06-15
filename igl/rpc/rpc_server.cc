/*
 * Unix-domain RPC bridge for submitting external block commands to SimpleSSD.
 */

#include "igl/rpc/rpc_server.hh"

#include <cerrno>
#include <cstring>
#include <sstream>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "simplessd/sim/trace.hh"

namespace IGL {

RPCServer::RPCServer(Engine &e, BIL::BlockIOEntry &b, std::function<void()> &f,
                     ConfigReader &c)
    : IOGenerator(e, b, f),
      socketPath(c.readString(CONFIG_GLOBAL, GLOBAL_RPC_SOCKET_PATH)),
      ssdSize(0),
      blockSize(0),
      commandCount(0),
      readCount(0),
      writeCount(0),
      migrateCount(0),
      stopping(false),
      serverFd(-1),
      commandCompleted(false),
      completionStatus(0),
      submittedAt(0),
      completedAt(0) {
  tierSize[0] = 0;
  tierSize[1] = 0;
  submitEvent =
      engine.allocateEvent([this](uint64_t tick) { submitPending(tick); });
}

RPCServer::~RPCServer() {
  stopping = true;
  int fd = serverFd.exchange(-1);

  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
    close(fd);
  }

  commandDone.notify_all();

  if (worker.joinable()) {
    worker.join();
  }

  engine.deallocateEvent(submitEvent);

  if (!socketPath.empty()) {
    unlink(socketPath.c_str());
  }
}

void RPCServer::init(uint64_t bytesize, uint32_t bs, uint64_t slcBytes,
                     uint64_t tlcBytes) {
  ssdSize = bytesize;
  blockSize = bs;
  tierSize[0] = slcBytes;
  tierSize[1] = tlcBytes;
}

bool RPCServer::sendLine(int fd, const std::string &line) {
  std::string output = line + "\n";
  size_t sent = 0;

  while (sent < output.size()) {
    ssize_t ret = send(fd, output.data() + sent, output.size() - sent, 0);

    if (ret <= 0) {
      return false;
    }
    sent += (size_t)ret;
  }

  return true;
}

bool RPCServer::submitAndWait(int fd, BIL::BIO &bio) {
  {
    std::lock_guard<std::mutex> guard(commandMutex);
    pendingBio = bio;
    pendingBio.source = BIL::BIO_SOURCE_RPC;
    commandCompleted = false;
    submittedAt = engine.getCurrentTick();
    completedAt = submittedAt;
  }

  engine.scheduleEvent(submitEvent, submittedAt);

  std::unique_lock<std::mutex> guard(commandMutex);
  commandDone.wait(guard,
                   [this]() { return stopping || commandCompleted; });

  if (!commandCompleted) {
    return false;
  }

  std::ostringstream response;
  response << "OK " << bio.id << " " << completionStatus << " " << submittedAt
           << " " << completedAt << " " << completedAt - submittedAt;
  guard.unlock();
  return sendLine(fd, response.str());
}

void RPCServer::submitPending(uint64_t) {
  BIL::BIO bio;

  {
    std::lock_guard<std::mutex> guard(commandMutex);
    bio = pendingBio;
  }

  bio.callback = [this](uint64_t id, uint16_t status) {
    completePending(id, status);
  };
  bioEntry.submitIO(bio);
}

void RPCServer::completePending(uint64_t, uint16_t status) {
  {
    std::lock_guard<std::mutex> guard(commandMutex);
    completionStatus = status;
    completedAt = engine.getCurrentTick();
    commandCompleted = true;
  }
  commandDone.notify_one();
}

bool RPCServer::handleCommand(int fd, const std::string &line) {
  std::istringstream input(line);
  std::string operation;

  input >> operation;

  if (operation == "INFO") {
    std::ostringstream response;
    response << "OK INFO " << ssdSize << " " << blockSize << " " << tierSize[0]
             << " " << tierSize[1] << " " << engine.getCurrentTick();
    return sendLine(fd, response.str());
  }

  if (operation == "STOP") {
    stopping = true;
    sendLine(fd, "OK STOP");
    endCallback();
    return false;
  }

  BIL::BIO bio;

  if (operation == "READ" || operation == "WRITE") {
    uint32_t tier = 0;
    uint64_t lba = 0;
    uint64_t nlb = 0;

    if (!(input >> bio.id >> tier >> lba >> nlb) || tier > 1 || nlb == 0) {
      return sendLine(fd, "ERR usage_READ_WRITE_id_tier_lba_nlb");
    }

    bio.type = operation == "READ" ? BIL::BIO_READ : BIL::BIO_WRITE;
    bio.tier = (uint8_t)tier;
    bio.offset = lba * blockSize;
    bio.length = nlb * blockSize;
    bio.nlb = nlb;
    commandCount++;
    operation == "READ" ? readCount++ : writeCount++;
    return submitAndWait(fd, bio);
  }

  if (operation == "MIGRATE") {
    uint32_t srcTier = 0;
    uint32_t dstTier = 0;

    if (!(input >> bio.id >> srcTier >> bio.srcLBA >> dstTier >> bio.dstLBA >>
          bio.nlb) ||
        srcTier > 1 || dstTier > 1 || bio.nlb == 0) {
      return sendLine(
          fd, "ERR usage_MIGRATE_id_src_tier_src_lba_dst_tier_dst_lba_nlb");
    }

    bio.type = BIL::BIO_MIGRATE;
    bio.srcTier = (uint8_t)srcTier;
    bio.dstTier = (uint8_t)dstTier;
    bio.length = bio.nlb * blockSize;
    commandCount++;
    migrateCount++;
    return submitAndWait(fd, bio);
  }

  return sendLine(fd, "ERR unknown_command");
}

void RPCServer::begin() {
  worker = std::thread([this]() { run(); });
}

void RPCServer::run() {
  int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);

  if (listenFd < 0) {
    SimpleSSD::panic("RPC socket() failed: %s", strerror(errno));
  }
  serverFd = listenFd;

  sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;

  if (socketPath.empty() || socketPath.size() >= sizeof(address.sun_path)) {
    close(listenFd);
    serverFd = -1;
    SimpleSSD::panic("Invalid RPC socket path");
  }

  strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
  unlink(socketPath.c_str());

  if (bind(listenFd, (sockaddr *)&address, sizeof(address)) < 0 ||
      listen(listenFd, 1) < 0) {
    int error = errno;
    close(listenFd);
    serverFd = -1;
    SimpleSSD::panic("RPC bind/listen failed: %s", strerror(error));
  }

  SimpleSSD::info("IGL::RPCServer: Listening on %s", socketPath.c_str());

  while (!stopping) {
    int clientFd = accept(listenFd, nullptr, nullptr);

    if (clientFd < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    std::string pending;
    char buffer[4096];
    ssize_t received = 0;

    while (!stopping &&
           (received = recv(clientFd, buffer, sizeof(buffer), 0)) > 0) {
      pending.append(buffer, (size_t)received);
      size_t newline = 0;

      while ((newline = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);

        if (!line.empty() && !handleCommand(clientFd, line)) {
          break;
        }
      }
    }

    close(clientFd);
  }

  if (serverFd.exchange(-1) >= 0) {
    close(listenFd);
  }
  unlink(socketPath.c_str());
}

void RPCServer::printStats(std::ostream &out) {
  out << "*** Statistics of RPC Server ***" << std::endl;
  out << "Commands: " << commandCount << " (Read: " << readCount
      << ", Write: " << writeCount << ", Migrate: " << migrateCount << ")"
      << std::endl;
  out << "*** End of statistics ***" << std::endl;

  if (commandCount > 0) {
    bioEntry.printStats(out);
  }
}

void RPCServer::getProgress(float &val) { val = 0.f; }

}  // namespace IGL

/*
 * Unix-domain RPC bridge for submitting external block commands to SimpleSSD.
 */

#pragma once

#ifndef __IGL_RPC_SERVER__
#define __IGL_RPC_SERVER__

#include <atomic>
#include <condition_variable>
#include <deque>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <string>
#include <thread>

#include "igl/io_gen.hh"

namespace IGL {

struct RPCCompletion {
  uint64_t submittedAt;
  uint64_t completedAt;
  uint16_t status;
  bool completed;
};

class RPCServer : public IOGenerator {
 private:
  std::string socketPath;
  uint64_t ssdSize;
  uint64_t tierSize[2];
  uint32_t blockSize;
  uint64_t commandCount;
  uint64_t readCount;
  uint64_t writeCount;
  uint64_t migrateCount;
  std::atomic<bool> stopping;
  std::atomic<int> serverFd;
  std::thread worker;
  SimpleSSD::Event submitEvent;
  std::mutex commandMutex;
  std::condition_variable commandDone;
  std::deque<BIL::BIO> pendingBios;
  std::unordered_map<uint64_t, RPCCompletion> completions;
  uint64_t pendingCompletionCount;

  bool handleCommand(int, const std::string &);
  bool submitAndWait(int, BIL::BIO &);
  bool submitBatchAndWait(int, std::vector<BIL::BIO> &, bool);
  bool sendLine(int, const std::string &);
  bool parseBIO(std::istringstream &, const std::string &, BIL::BIO &);
  void run();
  void submitPending(uint64_t);
  void completePending(uint64_t, uint16_t);

 public:
  RPCServer(Engine &, BIL::BlockIOEntry &, std::function<void()> &,
            ConfigReader &);
  ~RPCServer();

  void init(uint64_t, uint32_t, uint64_t, uint64_t) override;
  void begin() override;
  void printStats(std::ostream &) override;
  void getProgress(float &) override;
};

}  // namespace IGL

#endif

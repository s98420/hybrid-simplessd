/*
 * Copyright (C) 2017 CAMELab
 *
 * This file is part of SimpleSSD.
 *
 * SimpleSSD is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SimpleSSD is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SimpleSSD.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#ifndef __IGL_TRACE_REPLAYER__
#define __IGL_TRACE_REPLAYER__

#include <fstream>
#include <list>
#include <mutex>
#include <regex>
#include <thread>
#include <unordered_map>

#include "bil/entry.hh"
#include "igl/io_gen.hh"
#include "sim/cfg_reader.hh"
#include "sim/engine.hh"

namespace IGL {

class TraceReplayer : public IOGenerator {
 private:
  enum {
    ID_OPERATION,
    ID_BYTE_OFFSET,
    ID_BYTE_LENGTH,
    ID_LBA_OFFSET,
    ID_LBA_LENGTH,
    ID_TIER,
    ID_TIME_SEC,
    ID_TIME_MS,
    ID_TIME_US,
    ID_TIME_NS,
    ID_TIME_PS,
    ID_NUM
  };

  std::mutex m;

  std::ifstream file;
  std::regex regex;

  uint64_t fileSize;

  TIMING_MODE mode;
  uint64_t submissionLatency;
  uint64_t completionLatency;
  uint32_t maxQueueDepth;  // Only used in MODE_ASYNC

  bool useLBAOffset;
  bool useLBALength;
  uint32_t lbaSize;
  uint32_t groupID[ID_NUM];
  uint8_t defaultTier;
  bool timeValids[5];
  bool useHex;

  uint64_t ssdSize;
  uint64_t tierSize[2];
  uint32_t blocksize;

  uint64_t initTime;   // Only used in MODE_STRICT
  uint64_t firstTick;  // Only used in MODE_STRICT
  bool nextIOIsSync;   // Only used in MODE_ASYNC

  bool reserveTermination;

  uint64_t max_io;
  uint64_t io_submitted;  // Submitted I/O in bytes
  uint64_t io_count;      // I/O count created and submitted
  uint64_t read_count;
  uint64_t write_count;
  uint64_t query_count;

  uint64_t io_depth;
  std::unordered_map<
      uint64_t, std::shared_ptr<SimpleSSD::TierSpaceInfo>> queryResults;

  uint64_t mergeTime(std::smatch &);
  BIL::BIO_TYPE getType(std::string);
  uint8_t getTier(std::string);
  uint8_t getQueryTier(std::string);
  bool parseSimpleQueryLine(const std::string &);
  bool parseSimpleLine(const std::string &);
  void parseLine();
  void rescheduleSubmit(uint64_t);

  struct TraceLine {
    uint64_t tick;
    uint64_t offset;
    uint64_t length;
    uint64_t nlb;
    BIL::BIO_TYPE type;
    uint8_t tier;

    TraceLine()
        : tick(0),
          offset(0),
          length(0),
          nlb(0),
          type(BIL::BIO_NUM),
          tier(1) {}
  } linedata;

  SimpleSSD::Event submitEvent;
  std::function<void(uint64_t, uint16_t)> completionEvent;

  void submitIO();
  void iocallback(uint64_t, uint16_t);

 public:
  TraceReplayer(Engine &, BIL::BlockIOEntry &, std::function<void()> &,
                ConfigReader &);
  ~TraceReplayer();

  void init(uint64_t, uint32_t, uint64_t, uint64_t) override;
  void begin() override;
  void printStats(std::ostream &) override;
  void getProgress(float &) override;
};

}  // namespace IGL

#endif

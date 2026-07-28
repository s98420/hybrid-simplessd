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

#include "sil/none/none.hh"

#include "simplessd/pal/config.hh"
#include "simplessd/util/algorithm.hh"

namespace SIL {

namespace None {

Driver::Driver(Engine &e, SimpleSSD::ConfigReader &c)
    : BIL::DriverInterface(e),
      conf(c),
      totalLogicalPages(0),
      logicalPageSize(0) {
  pHIL = new SimpleSSD::HIL::HIL(c);
}

Driver::~Driver() {
  delete pHIL;
}

void Driver::init(std::function<void()> &func) {
  pHIL->getLPNInfo(totalLogicalPages, logicalPageSize);

  // No initialization process is needed for NoneDriver
  beginFunction = func;

  auto eid = engine.allocateEvent([this](uint64_t) { beginFunction(); });
  engine.scheduleEvent(eid, 0);

  SimpleSSD::info("SIL::None::Driver: Total SSD capacity: %" PRIu64 " bytes",
                  totalLogicalPages * logicalPageSize);
  SimpleSSD::info("SIL::None::Driver: Logical Page Size: %" PRIu32 " bytes",
                  logicalPageSize);
}

void Driver::getInfo(uint64_t &bytesize, uint32_t &minbs) {
  bytesize = totalLogicalPages * logicalPageSize;
  minbs = 512;
}

void Driver::getInfo(uint8_t tier, uint64_t &bytesize, uint32_t &minbs) {
  if (tier == 0) {
    pHIL->getTierLPNInfo(SimpleSSD::Tier::SLC, bytesize, minbs);
  }
  else if (tier == 1) {
    pHIL->getTierLPNInfo(SimpleSSD::Tier::TLC, bytesize, minbs);
  }
  else {
    SimpleSSD::panic("Invalid tier");
  }

  bytesize *= minbs;
  minbs = 512;
}

void Driver::submitIO(BIL::BIO &bio) {
  SimpleSSD::HIL::Request req;
  auto *pFunc = new std::function<void(uint64_t, uint16_t)>(bio.callback);

  // Convert to request
  req.reqID = bio.id;
  req.range.slpn = bio.offset / logicalPageSize;
  req.range.nlp = DIVCEIL(bio.length, logicalPageSize);
  req.range.tier = bio.tier == 0 ? SimpleSSD::Tier::SLC : SimpleSSD::Tier::TLC;
  req.tier = req.range.tier;
  req.offset = bio.offset % logicalPageSize;
  req.length = bio.length;
  req.context = (void *)bio.id;
  req.function = [pFunc](uint64_t, void *context) {
    pFunc->operator()((uint64_t)context, 0);
    delete pFunc;
  };

  // Submit
  switch (bio.type) {
    case BIL::BIO_READ:
      pHIL->read(req);
      break;
    case BIL::BIO_WRITE:
      pHIL->write(req);
      break;
    case BIL::BIO_FLUSH:
      pHIL->flush(req);
      break;
    case BIL::BIO_TRIM:
      pHIL->trim(req);
      break;
    case BIL::BIO_MIGRATE:
      SimpleSSD::panic("Migration requires NVMe interface");
      break;
    case BIL::BIO_QUERY: {
      bool success = bio.tierSpace && pHIL->getTierSpaceInfo(
                                          static_cast<SimpleSSD::Tier>(bio.tier),
                                          *bio.tierSpace);
      bio.callback(bio.id, success ? 0 : 1);
      delete pFunc;
    } break;
    default:
      break;
  }
}

void Driver::initStats(std::vector<SimpleSSD::Stats> &list) {
  pHIL->getStatList(list, "");
  SimpleSSD::getCPUStatList(list, "cpu");
}

void Driver::getStats(std::vector<double> &values) {
  pHIL->getStatValues(values);
  SimpleSSD::getCPUStatValues(values);
}

}  // namespace None

}  // namespace SIL

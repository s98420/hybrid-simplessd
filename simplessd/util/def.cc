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

#include "util/def.hh"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace SimpleSSD {

MappingEntry::_MappingEntry()
    : valid(false),
      tier(Tier::TLC),
      block(std::numeric_limits<uint32_t>::max()),
      page(std::numeric_limits<uint32_t>::max()) {}

MappingEntry::_MappingEntry(Tier t, uint32_t b, uint32_t p)
    : valid(true), tier(t), block(b), page(p) {}

MigrationRequest::_MigrationRequest()
    : startLCA(0),
      count(0),
      targetTier(Tier::TLC),
      status(MigrationStatus::Success) {}

MigrationRequest::_MigrationRequest(LCA lca, uint64_t n, Tier tier)
    : startLCA(lca),
      count(n),
      targetTier(tier),
      status(MigrationStatus::Success) {}

TierSpaceInfo::_TierSpaceInfo()
    : version(1),
      tier(Tier::TLC),
      writablePagesWithoutGC(0),
      writableBytesWithoutGC(0),
      pendingReservedPages(0),
      reclaimableInvalidPages(0),
      freePhysicalBlocks(0) {}

LPNRange::_LPNRange() : slpn(0), nlp(0), tier(Tier::TLC) {}

LPNRange::_LPNRange(uint64_t s, uint64_t n)
    : slpn(s), nlp(n), tier(Tier::TLC) {}

namespace HIL {

Request::_Request()
    : reqID(0),
      reqSubID(0),
      offset(0),
      length(0),
      tier(Tier::TLC),
      finishedAt(0),
      context(nullptr) {}

Request::_Request(DMAFunction &f, void *c)
    : reqID(0),
      reqSubID(0),
      offset(0),
      length(0),
      tier(Tier::TLC),
      finishedAt(0),
      function(f),
      context(c) {}

bool Request::operator()(const Request &a, const Request &b) {
  return a.finishedAt > b.finishedAt;
}

}  // namespace HIL

namespace ICL {

Request::_Request()
    : reqID(0), reqSubID(0), offset(0), length(0), tier(Tier::TLC) {}

Request::_Request(HIL::Request &r)
    : reqID(r.reqID),
      reqSubID(r.reqSubID),
      offset(r.offset),
      length(r.length),
      tier(r.tier),
      range(r.range) {}

}  // namespace ICL

namespace FTL {

Request::_Request(uint32_t iocount)
    : reqID(0),
      reqSubID(0),
      tier(Tier::TLC),
      lpn(0),
      ioFlag(iocount),
      reservationOwner(ReservationOwner::None),
      supersedesPendingMigration(true) {}

Request::_Request(uint32_t iocount, ICL::Request &r)
    : reqID(r.reqID),
      reqSubID(r.reqSubID),
      tier(r.tier),
      lpn(lcaToLpn(r.range.slpn, iocount)),
      ioFlag(iocount),
      reservationOwner(ReservationOwner::None),
      supersedesPendingMigration(true) {
  ioFlag.set(lcaToMappingIndex(r.range.slpn, iocount));
}

}  // namespace FTL

namespace PAL {

Request::_Request(uint32_t iocount)
    : reqID(0),
      reqSubID(0),
      tier(Tier::TLC),
      blockIndex(0),
      pageIndex(0),
      ioFlag(iocount) {}

Request::_Request(FTL::Request &r)
    : reqID(r.reqID),
      reqSubID(r.reqSubID),
      tier(r.tier),
      blockIndex(0),
      pageIndex(0),
      ioFlag(r.ioFlag) {}

}  // namespace PAL

}  // namespace SimpleSSD

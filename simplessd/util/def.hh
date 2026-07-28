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

#ifndef __UTIL_DEF__
#define __UTIL_DEF__

#include <cinttypes>
#include <cstddef>

#include "sim/dma_interface.hh"
#include "util/bitset.hh"
#include "util/tier.hh"

namespace SimpleSSD {

using LCA = uint64_t;
using LPN = uint64_t;

inline bool isValidTier(Tier tier) {
  return tier == Tier::SLC || tier == Tier::TLC;
}

inline size_t tierIndex(Tier tier) {
  return static_cast<size_t>(tier);
}

inline LPN lcaToLpn(LCA lca, uint32_t mappingEntriesPerPage) {
  return lca / mappingEntriesPerPage;
}

inline uint32_t lcaToMappingIndex(LCA lca,
                                  uint32_t mappingEntriesPerPage) {
  return static_cast<uint32_t>(lca % mappingEntriesPerPage);
}

inline LCA lpnToLca(LPN lpn, uint32_t mappingIndex,
                    uint32_t mappingEntriesPerPage) {
  return lpn * mappingEntriesPerPage + mappingIndex;
}

inline bool isValidLogicalRange(LCA startLCA, uint64_t count,
                                uint64_t totalLogicalUnits) {
  return startLCA <= totalLogicalUnits && count <= totalLogicalUnits - startLCA;
}

typedef struct _MappingEntry {
  bool valid;
  Tier tier;
  uint32_t block;
  uint32_t page;

  _MappingEntry();
  _MappingEntry(Tier, uint32_t, uint32_t);
} MappingEntry;

enum class MigrationStatus : uint8_t {
  Success = 0,
  InvalidTier,
  OutOfRange,
  UnmappedSource,
  NoSpace,
  InternalError,
};

enum class MigrationTrigger : uint8_t {
  GarbageCollection = 0,
  ListFull,
};

enum class ReservationOwner : uint8_t {
  None = 0,
  CacheWrite,
  Migration,
};

typedef struct _MigrationRequest {
  LCA startLCA;
  uint64_t count;
  Tier targetTier;
  MigrationStatus status;

  _MigrationRequest();
  _MigrationRequest(LCA, uint64_t, Tier);
} MigrationRequest;

typedef struct _TierSpaceInfo {
  uint32_t version;
  Tier tier;
  uint64_t writablePagesWithoutGC;
  uint64_t writableBytesWithoutGC;
  uint64_t pendingReservedPages;
  uint64_t reclaimableInvalidPages;
  uint64_t freePhysicalBlocks;

  _TierSpaceInfo();
} TierSpaceInfo;

typedef struct _LPNRange {
  uint64_t slpn;
  uint64_t nlp;
  Tier tier;

  _LPNRange();
  _LPNRange(uint64_t, uint64_t);
} LPNRange;

namespace HIL {

typedef struct _Request {
  uint64_t reqID;
  uint64_t reqSubID;
  uint64_t offset;
  uint64_t length;
  Tier tier;
  LPNRange range;

  uint64_t finishedAt;
  DMAFunction function;
  void *context;

  _Request();
  _Request(DMAFunction &, void *);

  bool operator()(const _Request &a, const _Request &b);
} Request;

}  // namespace HIL

namespace ICL {

typedef struct _Request {
  uint64_t reqID;
  uint64_t reqSubID;
  uint64_t offset;
  uint64_t length;
  Tier tier;
  LPNRange range;

  _Request();
  _Request(HIL::Request &);
} Request;

}  // namespace ICL

namespace FTL {

typedef struct _Request {
  uint64_t reqID;  // ID of ICL::Request
  uint64_t reqSubID;
  Tier tier;
  uint64_t lpn;
  Bitset ioFlag;
  ReservationOwner reservationOwner;
  bool supersedesPendingMigration;

  _Request(uint32_t);
  _Request(uint32_t, ICL::Request &);
} Request;

}  // namespace FTL

namespace PAL {

typedef struct _Request {
  uint64_t reqID;  // ID of ICL::Request
  uint64_t reqSubID;
  Tier tier;
  uint32_t blockIndex;
  uint32_t pageIndex;
  Bitset ioFlag;

  _Request(uint32_t);
  _Request(FTL::Request &);
} Request;

}  // namespace PAL

}  // namespace SimpleSSD

#endif

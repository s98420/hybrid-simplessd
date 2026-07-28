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

#include "ftl/page_mapping.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <unordered_set>

#include "util/algorithm.hh"
#include "util/bitset.hh"

namespace SimpleSSD {

namespace FTL {

PageMapping::RegionState::_RegionState(uint32_t pageCountToMaxPerf,
                                        uint32_t ioUnitInPage)
    : tier(Tier::TLC),
      blockBegin(0),
      blockEnd(0),
      totalPhysicalBlocks(0),
      placementLogicalBlocks(0),
      placementLogicalPages(0),
      nFreeBlocks(0),
      lastFreeBlock(pageCountToMaxPerf),
      lastFreeBlockIOMap(ioUnitInPage),
      lastFreeBlockIndex(0),
      bReclaimMore(false) {
  status.totalLogicalPages = 0;
  status.mappedLogicalPages = 0;
  status.freePhysicalBlocks = 0;
  memset(&stat, 0, sizeof(stat));
}

PageMapping::PageMapping(ConfigReader &c, Parameter &p, PAL::PAL *l,
                         DRAM::AbstractDRAM *d)
    : AbstractFTL(p, l, d),
      pPAL(l),
      conf(c),
      regions{RegionState(param.pageCountToMaxPerf, param.ioUnitInPage),
              RegionState(param.pageCountToMaxPerf, param.ioUnitInPage)},
      pendingCacheWriteUnitsByTier{0, 0},
      pendingMigrationUnitsByTier{0, 0},
      migrationListLimit(
          conf.readUint(CONFIG_FTL, FTL_MIGRATION_LIST_LIMIT)),
      migrationStat{} {
  bRandomTweak = conf.readBoolean(CONFIG_FTL, FTL_USE_RANDOM_IO_TWEAK);
  bitsetSize = param.mappingEntriesPerPage;
  table.reserve(param.globalLogicalPages);

  if (migrationListLimit == 0) {
    panic("ftl: MigrationListLimit must be greater than zero");
  }

  uint32_t begin = 0;

  for (uint32_t rid = 0; rid < regions.size(); rid++) {
    auto &region = regions[rid];

    region.tier = rid == 0 ? Tier::SLC : Tier::TLC;
    region.blockBegin = begin;
    region.blockEnd = begin + param.totalPhysicalBlocksByTier[rid];
    region.totalPhysicalBlocks = param.totalPhysicalBlocksByTier[rid];
    region.placementLogicalBlocks = param.placementLogicalBlocksByTier[rid];
    region.placementLogicalPages = param.placementLogicalPagesByTier[rid];
    region.status.totalLogicalPages = region.placementLogicalPages;

    region.blocks.reserve(region.totalPhysicalBlocks);

    for (uint32_t i = region.blockBegin; i < region.blockEnd; i++) {
      region.freeBlocks.emplace_back(
          Block(i, param.pagesInBlock, param.ioUnitInPage));
    }

    region.nFreeBlocks = region.totalPhysicalBlocks;

    for (uint32_t i = 0; i < param.pageCountToMaxPerf; i++) {
      uint32_t blockIndex;

      if (!getFreeBlock(region, i, blockIndex)) {
        panic("ftl: insufficient free blocks for initial write frontiers");
      }

      region.lastFreeBlock.at(i) = blockIndex;
    }

    begin = region.blockEnd;
  }
}

PageMapping::~PageMapping() {}

bool PageMapping::initialize() {
  uint64_t tick;
  uint64_t valid;
  uint64_t invalid;
  FILLING_MODE mode = (FILLING_MODE)conf.readUint(CONFIG_FTL, FTL_FILLING_MODE);
  Request req(param.ioUnitInPage);
  std::vector<LPN> globalLpnPool(param.globalLogicalPages);
  std::array<std::vector<LPN>, 2> mappedLpnsByTier;
  std::unordered_set<LPN> initializedLpns;
  std::random_device rd;
  std::mt19937_64 gen(rd());
  uint64_t nextGlobalLpn = 0;

  debugprint(LOG_FTL_PAGE_MAPPING, "Initialization started");

  if (mode != FILLING_MODE_0 && mode != FILLING_MODE_1 &&
      mode != FILLING_MODE_2) {
    panic("ftl: invalid filling mode");
  }

  std::iota(globalLpnPool.begin(), globalLpnPool.end(), static_cast<LPN>(0));

  if (mode == FILLING_MODE_2) {
    std::shuffle(globalLpnPool.begin(), globalLpnPool.end(), gen);
  }

  for (uint32_t rid = 0; rid < regions.size(); rid++) {
    auto &region = regions[rid];
    const char *tierName = region.tier == Tier::SLC ? "SLC" : "TLC";
    float fillRatio =
        conf.readFloat(CONFIG_FTL, region.tier == Tier::SLC
                                       ? FTL_SLC_FILL_RATIO
                                       : FTL_TLC_FILL_RATIO);
    float invalidRatio =
        conf.readFloat(CONFIG_FTL, region.tier == Tier::SLC
                                       ? FTL_SLC_INVALID_PAGE_RATIO
                                       : FTL_TLC_INVALID_PAGE_RATIO);
    uint64_t nTotalLogicalPages = region.placementLogicalPages;
    uint64_t nPagesToWarmup = nTotalLogicalPages * fillRatio;
    uint64_t nPagesToInvalidate = nTotalLogicalPages * invalidRatio;
    uint64_t maxPagesBeforeGC =
        param.pagesInBlock *
        (region.totalPhysicalBlocks *
             (1 - conf.readFloat(CONFIG_FTL, FTL_GC_THRESHOLD_RATIO)) -
         param.pageCountToMaxPerf);  // # free blocks to maintain

    if (nPagesToWarmup + nPagesToInvalidate > maxPagesBeforeGC) {
      warn("ftl: Too high filling ratio. Adjusting invalidPageRatio.");
      if (nPagesToWarmup >= maxPagesBeforeGC) {
        nPagesToInvalidate = 0;
      }
      else {
        nPagesToInvalidate = maxPagesBeforeGC - nPagesToWarmup;
      }
    }

    debugprint(LOG_FTL_PAGE_MAPPING, "%s warm-up ratios: fill %.4f, invalid %.4f",
               tierName, fillRatio, invalidRatio);
    debugprint(LOG_FTL_PAGE_MAPPING, "%s total logical pages: %" PRIu64,
               tierName, nTotalLogicalPages);
    debugprint(LOG_FTL_PAGE_MAPPING,
               "%s logical pages to fill: %" PRIu64 " (%.2f %%)", tierName,
               nPagesToWarmup, nPagesToWarmup * 100.f / nTotalLogicalPages);
    debugprint(LOG_FTL_PAGE_MAPPING,
               "%s invalidated pages to create: %" PRIu64 " (%.2f %%)",
               tierName, nPagesToInvalidate,
               nPagesToInvalidate * 100.f / nTotalLogicalPages);

    req.tier = region.tier;
    req.ioFlag.set();

    if (!isValidLogicalRange(nextGlobalLpn, nPagesToWarmup,
                             globalLpnPool.size())) {
      panic("ftl: tier warm-up exceeds the global logical LPN pool");
    }

    // Step 1. Filling. Logical selection is global and unique; tier placement
    // remains an independent choice made by the enclosing region loop.
    for (uint64_t i = 0; i < nPagesToWarmup; i++) {
      tick = 0;
      req.lpn = globalLpnPool.at(nextGlobalLpn++);

      if (!initializedLpns.insert(req.lpn).second) {
        panic("ftl: duplicate global LPN selected during warm-up");
      }

      if (!writeInternal(req, region, tick, false)) {
        panic("ftl: warm-up allocation unexpectedly failed");
      }
      mappedLpnsByTier[rid].push_back(req.lpn);
    }

    if (mappedLpnsByTier[rid].empty() && nPagesToInvalidate > 0) {
      warn("ftl: cannot create invalid pages without mapped global LPNs");
      nPagesToInvalidate = 0;
    }

    // Step 2. Invalidating by overwriting only globally mapped LPNs in the
    // selected physical tier.
    if (nPagesToInvalidate > 0) {
      std::uniform_int_distribution<uint64_t> dist(
          0, mappedLpnsByTier[rid].size() - 1);

      for (uint64_t i = 0; i < nPagesToInvalidate; i++) {
        tick = 0;
        uint64_t selected =
            mode == FILLING_MODE_0 ? i % mappedLpnsByTier[rid].size()
                                   : dist(gen);
        req.lpn = mappedLpnsByTier[rid].at(selected);
        if (!writeInternal(req, region, tick, false)) {
          panic("ftl: warm-up overwrite allocation unexpectedly failed");
        }
      }
    }

    // Report
    calculateTotalPages(region, valid, invalid);
    debugprint(LOG_FTL_PAGE_MAPPING, "%s filling finished. Page status:",
               tierName);
    debugprint(LOG_FTL_PAGE_MAPPING,
               "  %s valid physical pages: %" PRIu64
               " (%.2f %%, target: %" PRIu64 ", error: %" PRId64 ")",
               tierName, valid, valid * 100.f / nTotalLogicalPages,
               nPagesToWarmup, (int64_t)(valid - nPagesToWarmup));
    debugprint(LOG_FTL_PAGE_MAPPING,
               "  %s invalid physical pages: %" PRIu64
               " (%.2f %%, target: %" PRIu64 ", error: %" PRId64 ")",
               tierName, invalid, invalid * 100.f / nTotalLogicalPages,
               nPagesToInvalidate, (int64_t)(invalid - nPagesToInvalidate));
  }

  if (initializedLpns.size() != nextGlobalLpn) {
    panic("ftl: global warm-up LPN accounting is inconsistent");
  }

  debugprint(LOG_FTL_PAGE_MAPPING,
             "Global unique initialized logical pages: %" PRIu64,
             static_cast<uint64_t>(initializedLpns.size()));

  debugprint(LOG_FTL_PAGE_MAPPING, "Initialization finished");

  return true;
}

void PageMapping::read(Request &req, uint64_t &tick) {
  uint64_t begin = tick;

  if (req.lpn >= param.globalLogicalPages) {
    panic("ftl: read LPN out of global logical range");
  }

  if (req.ioFlag.count() > 0) {
    readInternal(req, tick);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "READ  | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               req.lpn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::READ);
}

bool PageMapping::write(Request &req, uint64_t &tick) {
  uint64_t begin = tick;

  if (!isValidTier(req.tier)) {
    panic("ftl: write target tier is invalid");
  }

  auto &region = getRegion(req.tier);

  if (req.lpn >= param.globalLogicalPages) {
    panic("ftl: write LPN out of global logical range");
  }

  if (req.ioFlag.count() > 0) {
    uint64_t units = req.ioFlag.count();
    std::array<uint64_t, 2> migrationReleases{0, 0};
    std::vector<LCA> migrationsToCancel;

    if (req.supersedesPendingMigration) {
      for (uint32_t idx = 0; idx < bitsetSize; idx++) {
        if (!req.ioFlag.test(idx)) {
          continue;
        }

        LCA lca = lpnToLca(req.lpn, idx, bitsetSize);
        auto pending = pendingMigrations.find(lca);

        if (pending != pendingMigrations.end()) {
          migrationReleases.at(tierIndex(pending->second.targetTier))++;
          migrationsToCancel.push_back(lca);
        }
      }
    }

    if (!canAllocateAfterMigrationCancellation(
            region, units, req.reservationOwner, migrationReleases, false)) {
      warn("ftl: write rejected by target-tier reservation admission");
      tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE);

      return false;
    }

    if (!writeInternal(req, region, tick)) {
      warn("ftl: write rejected because target tier has no writable page");
      tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE);

      return false;
    }

    for (LCA lca : migrationsToCancel) {
      cancelPendingMigration(lca, CancellationReason::Write);
    }

    consumeReservation(req.tier, units, req.reservationOwner);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "WRITE | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               req.lpn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE);

  return true;
}

void PageMapping::trim(Request &req, uint64_t &tick) {
  uint64_t begin = tick;

  if (req.lpn >= param.globalLogicalPages) {
    panic("ftl: trim LPN out of global logical range");
  }

  for (uint32_t idx = 0; idx < bitsetSize; idx++) {
    if (req.ioFlag.test(idx)) {
      cancelPendingMigration(lpnToLca(req.lpn, idx, bitsetSize),
                             CancellationReason::Trim);
    }
  }

  trimInternal(req, tick);

  debugprint(LOG_FTL_PAGE_MAPPING,
             "TRIM  | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
             ")",
             req.lpn, begin, tick, tick - begin);

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::TRIM);
}

void PageMapping::migrate(MigrationRequest &req, uint64_t &tick) {
  req.status = enqueueMigration(req, tick);
}

void PageMapping::drainMigrations(MigrationRequest &req, uint64_t &tick) {
  if (req.status == MigrationStatus::Success && migrationDrainRequired()) {
    req.status = drainPendingMigrations(tick);
  }
}

bool PageMapping::migrationDrainRequired() const {
  return pendingMigrations.size() >= migrationListLimit;
}

std::vector<LCA> PageMapping::getPendingMigrationLCAs() const {
  return std::vector<LCA>(pendingMigrationOrder.begin(),
                          pendingMigrationOrder.end());
}

MigrationStatus PageMapping::enqueueMigration(MigrationRequest &request,
                                              uint64_t tick) {
  enum class Action : uint8_t {
    None = 0,
    Add,
    Update,
    RemoveSatisfied,
  };

  typedef struct _PlannedUpdate {
    LCA lca;
    Action action;
  } PlannedUpdate;

  if (!isValidTier(request.targetTier)) {
    return MigrationStatus::InvalidTier;
  }

  if (request.count == 0 ||
      !isValidLogicalRange(request.startLCA, request.count,
                           param.globalLogicalUnits)) {
    return MigrationStatus::OutOfRange;
  }

  std::vector<PlannedUpdate> updates;
  std::array<uint64_t, 2> cacheReleases{0, 0};
  std::array<uint64_t, 2> cacheAdds{0, 0};
  std::array<uint64_t, 2> migrationReleases{0, 0};
  std::array<uint64_t, 2> migrationAdds{0, 0};
  std::array<uint64_t, 2> finalCache{0, 0};
  std::array<uint64_t, 2> finalMigration{0, 0};

  updates.reserve(request.count);

  for (uint64_t offset = 0; offset < request.count; offset++) {
    LCA lca = request.startLCA + offset;
    LPN lpn = lcaToLpn(lca, bitsetSize);
    uint32_t mappingIndex = lcaToMappingIndex(lca, bitsetSize);
    auto mappingList = table.find(lpn);

    if (mappingList == table.end() ||
        !checkMappingAddress(
            mappingList->second.at(mappingIndex),
            "ftl: migration source mapping is inconsistent")) {
      return MigrationStatus::UnmappedSource;
    }

    const MappingEntry &mapping = mappingList->second.at(mappingIndex);
    auto pending = pendingMigrations.find(lca);
    Action action = Action::None;

    if (mapping.tier == request.targetTier) {
      if (pending != pendingMigrations.end()) {
        migrationReleases.at(tierIndex(pending->second.targetTier))++;
        action = Action::RemoveSatisfied;
      }
    }
    else if (pending == pendingMigrations.end()) {
      migrationAdds.at(tierIndex(request.targetTier))++;
      action = Action::Add;
    }
    else if (pending->second.targetTier != request.targetTier) {
      migrationReleases.at(tierIndex(pending->second.targetTier))++;
      migrationAdds.at(tierIndex(request.targetTier))++;
      action = Action::Update;
    }

    updates.push_back({lca, action});
  }

  if (!calculateFinalReservations(
          cacheReleases, cacheAdds, migrationReleases, migrationAdds,
          finalCache, finalMigration)) {
    return MigrationStatus::NoSpace;
  }

  for (const auto &update : updates) {
    auto pending = pendingMigrations.find(update.lca);

    switch (update.action) {
      case Action::None:
        break;
      case Action::Add: {
        pendingMigrationOrder.push_back(update.lca);
        auto orderIterator = pendingMigrationOrder.end();
        --orderIterator;
        auto inserted = pendingMigrations.emplace(
            update.lca,
            PendingMigration(request.targetTier, tick, orderIterator));

        if (!inserted.second) {
          panic("ftl: duplicate pending migration during atomic commit");
        }

        break;
      }
      case Action::Update:
        if (pending == pendingMigrations.end()) {
          panic("ftl: pending migration disappeared during atomic update");
        }

        pending->second.targetTier = request.targetTier;
        break;
      case Action::RemoveSatisfied:
        if (pending == pendingMigrations.end()) {
          panic("ftl: satisfied pending migration disappeared during commit");
        }

        removePendingMigration(pending, false);
        break;
    }
  }

  pendingCacheWriteUnitsByTier = finalCache;
  pendingMigrationUnitsByTier = finalMigration;
  migrationStat.maximumQueueEntries =
      MAX(migrationStat.maximumQueueEntries,
          static_cast<uint64_t>(pendingMigrations.size()));

  return MigrationStatus::Success;
}

MigrationStatus PageMapping::executePendingMigration(
    LCA lca, MigrationTrigger trigger, uint64_t &tick,
    const MappingEntry *expectedSource, bool sourceDataReady) {
  if (sourceDataReady && expectedSource == nullptr) {
    return MigrationStatus::InternalError;
  }

  auto pending = pendingMigrations.find(lca);

  if (pending == pendingMigrations.end()) {
    return MigrationStatus::InternalError;
  }

  LPN lpn = lcaToLpn(lca, bitsetSize);
  uint32_t mappingIndex = lcaToMappingIndex(lca, bitsetSize);
  auto mappingList = table.find(lpn);

  if (mappingList == table.end()) {
    return MigrationStatus::InternalError;
  }

  MappingEntry &mapping = mappingList->second.at(mappingIndex);

  if (!checkMappingAddress(
          mapping, "ftl: pending migration source mapping is inconsistent")) {
    return MigrationStatus::InternalError;
  }

  if (expectedSource != nullptr &&
      (!expectedSource->valid || mapping.tier != expectedSource->tier ||
       mapping.block != expectedSource->block ||
       mapping.page != expectedSource->page)) {
    return MigrationStatus::InternalError;
  }

  Tier targetTier = pending->second.targetTier;

  if (mapping.tier == targetTier) {
    removePendingMigration(pending, true);

    return MigrationStatus::Success;
  }

  Bitset migrationIOMap(param.ioUnitInPage);

  if (bRandomTweak) {
    migrationIOMap.set(mappingIndex);
  }
  else {
    migrationIOMap.set();
  }

  auto &sourceRegion = getRegion(mapping.tier);
  auto sourceBlock = sourceRegion.blocks.find(mapping.block);

  if (sourceBlock == sourceRegion.blocks.end() ||
      !sourceBlock->second.read(mapping.page, mappingIndex, tick)) {
    return MigrationStatus::InternalError;
  }

  if (!sourceDataReady) {
    PAL::Request readRequest(param.ioUnitInPage);
    readRequest.tier = mapping.tier;
    readRequest.blockIndex = mapping.block;
    readRequest.pageIndex = mapping.page;
    readRequest.ioFlag = migrationIOMap;
    pPAL->read(readRequest, tick);
  }

  auto &targetRegion = getRegion(targetTier);

  if (!canAllocate(targetRegion, 1, ReservationOwner::Migration, true)) {
    return MigrationStatus::InternalError;
  }

  uint32_t targetBlockIndex;

  if (!getLastFreeBlock(targetRegion, migrationIOMap,
                        targetBlockIndex)) {
    return MigrationStatus::InternalError;
  }

  auto targetBlock = targetRegion.blocks.find(targetBlockIndex);

  if (targetBlock == targetRegion.blocks.end() ||
      !isInRegion(targetRegion, targetBlockIndex)) {
    return MigrationStatus::InternalError;
  }

  uint32_t targetPage =
      targetBlock->second.getNextWritePageIndex(mappingIndex);

  if (!targetBlock->second.write(targetPage, lpn, mappingIndex, tick)) {
    panic("ftl: reserved migration target is not writable");
  }

  PAL::Request writeRequest(param.ioUnitInPage);
  writeRequest.tier = targetTier;
  writeRequest.blockIndex = targetBlockIndex;
  writeRequest.pageIndex = targetPage;
  writeRequest.ioFlag = migrationIOMap;
  pPAL->write(writeRequest, tick);

  if (expectedSource != nullptr &&
      (mapping.tier != expectedSource->tier ||
       mapping.block != expectedSource->block ||
       mapping.page != expectedSource->page)) {
    panic("ftl: pending migration source changed before mapping commit");
  }

  commitMappingReplacement(
      mapping, MappingEntry(targetTier, targetBlockIndex, targetPage),
      mappingIndex);
  removePendingMigration(pending, true);

  switch (trigger) {
    case MigrationTrigger::GarbageCollection:
      migrationStat.executedByGarbageCollection++;
      break;
    case MigrationTrigger::ListFull:
      migrationStat.executedByListFull++;
      break;
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE_INTERNAL);

  return MigrationStatus::Success;
}

MigrationStatus PageMapping::drainPendingMigrations(uint64_t &tick) {
  while (!pendingMigrationOrder.empty()) {
    LCA lca = pendingMigrationOrder.front();
    MigrationStatus status =
        executePendingMigration(lca, MigrationTrigger::ListFull, tick);

    if (status != MigrationStatus::Success) {
      return status;
    }
  }

  return MigrationStatus::Success;
}

void PageMapping::format(LPNRange &range, uint64_t &tick) {
  std::array<std::vector<uint32_t>, 2> blocksToReclaim;

  if (!isValidLogicalRange(range.slpn, range.nlp,
                           param.globalLogicalPages)) {
    panic("ftl: format LPN range is outside global logical capacity");
  }

  LCA formatStart = lpnToLca(range.slpn, 0, bitsetSize);
  LCA formatEnd = lpnToLca(range.slpn + range.nlp, 0, bitsetSize);

  for (auto iter = pendingMigrationOrder.begin();
       iter != pendingMigrationOrder.end();) {
    LCA lca = *iter++;

    if (lca >= formatStart && lca < formatEnd) {
      cancelPendingMigration(lca, CancellationReason::Format);
    }
  }

  for (auto iter = table.begin(); iter != table.end();) {
    if (iter->first >= range.slpn && iter->first < range.slpn + range.nlp) {
      auto &mappingList = iter->second;

      for (uint32_t idx = 0; idx < bitsetSize; idx++) {
        auto &mapping = mappingList.at(idx);

        if (!mapping.valid) {
          continue;
        }

        if (!checkMappingAddress(
                mapping, "ftl: format mapping points outside its tier")) {
          panic("ftl: valid format mapping disappeared");
        }

        auto &region = getRegion(mapping.tier);
        auto block = region.blocks.find(mapping.block);

        if (block == region.blocks.end()) {
          panic("Block is not in use");
        }

        block->second.invalidate(mapping.page, idx);

        // Collect block indices
        blocksToReclaim.at(tierIndex(mapping.tier)).push_back(mapping.block);
        mapping = MappingEntry();
      }

      bool mappingEmpty = true;

      for (auto &mapping : mappingList) {
        if (mapping.valid) {
          mappingEmpty = false;
          break;
        }
      }

      if (mappingEmpty) {
        iter = table.erase(iter);
      }
      else {
        iter++;
      }
    }
    else {
      iter++;
    }
  }

  // Reclaim every affected physical block in its actual tier. A formatted
  // namespace can share blocks with mappings outside the formatted range;
  // normal same-tier GC relocation preserves those mappings before erase.
  for (uint32_t rid = 0; rid < regions.size(); rid++) {
    auto &region = regions[rid];
    auto &list = blocksToReclaim[rid];

    std::sort(list.begin(), list.end());
    auto last = std::unique(list.begin(), list.end());
    list.erase(last, list.end());

    // Format is allowed to select a partially written active frontier. Move
    // such a frontier first so GC never relocates data into a victim block or
    // leaves the allocator pointing at an erased block.
    for (uint32_t frontier = 0; frontier < region.lastFreeBlock.size();
         frontier++) {
      if (!std::binary_search(list.begin(), list.end(),
                              region.lastFreeBlock[frontier])) {
        continue;
      }

      uint32_t replacement;

      if (!getFreeBlock(region, frontier, replacement)) {
        panic("ftl: format cannot replace an affected write frontier");
      }

      region.lastFreeBlock[frontier] = replacement;

      if (region.lastFreeBlockIndex == frontier) {
        region.lastFreeBlockIOMap.reset();
      }
    }

    doGarbageCollection(list, region, tick);
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::FORMAT);
}

bool PageMapping::getTierSpaceInfo(Tier tier, TierSpaceInfo &info) const {
  if (!isValidTier(tier)) {
    return false;
  }

  const auto &region = getRegion(tier);
  const size_t rid = tierIndex(tier);
  const uint64_t cacheReserved = pendingCacheWriteUnitsByTier.at(rid);
  const uint64_t migrationReserved = pendingMigrationUnitsByTier.at(rid);

  if (cacheReserved >
      std::numeric_limits<uint64_t>::max() - migrationReserved) {
    panic("ftl: tier-space reservation counter overflow");
  }

  const uint64_t pendingReserved = cacheReserved + migrationReserved;
  const uint64_t rawWritable = countWritableUnitsWithoutGC(region);
  uint64_t reclaimableInvalid = 0;

  for (const auto &block : region.blocks) {
    reclaimableInvalid += block.second.getDirtyPageCountRaw();
  }

  info = TierSpaceInfo();
  info.tier = tier;
  info.writablePagesWithoutGC =
      rawWritable > pendingReserved ? rawWritable - pendingReserved : 0;
  info.writableBytesWithoutGC =
      info.writablePagesWithoutGC * (param.pageSize / bitsetSize);
  info.pendingReservedPages = pendingReserved;
  info.reclaimableInvalidPages = reclaimableInvalid;
  info.freePhysicalBlocks = region.nFreeBlocks;

  return true;
}

Status *PageMapping::getStatus(uint64_t lpnBegin, uint64_t lpnEnd) {
  status.totalLogicalPages = param.globalLogicalPages;
  status.freePhysicalBlocks = 0;
  status.mappedLogicalPages = 0;

  for (auto &region : regions) {
    status.freePhysicalBlocks += region.nFreeBlocks;
  }

  if (lpnBegin == 0 && lpnEnd >= param.globalLogicalPages) {
    status.mappedLogicalPages = table.size();
  }
  else {
    for (auto &mappingList : table) {
      if (mappingList.first >= lpnBegin && mappingList.first < lpnEnd) {
        status.mappedLogicalPages++;
      }
    }
  }

  return &status;
}

PageMapping::RegionState &PageMapping::getRegion(Tier tier) {
  if (!isValidTier(tier)) {
    panic("ftl: invalid physical tier");
  }

  return regions[tierIndex(tier)];
}

const PageMapping::RegionState &PageMapping::getRegion(Tier tier) const {
  if (!isValidTier(tier)) {
    panic("ftl: invalid physical tier");
  }

  return regions[tierIndex(tier)];
}

bool PageMapping::isInRegion(const RegionState &region,
                             uint32_t blockIndex) const {
  return blockIndex >= region.blockBegin && blockIndex < region.blockEnd;
}

bool PageMapping::checkMappingAddress(const MappingEntry &mapping,
                                      const char *message) const {
  if (!mapping.valid) {
    return false;
  }

  if (!isValidTier(mapping.tier)) {
    panic(message);
  }

  const auto &region = getRegion(mapping.tier);

  if (mapping.page >= param.pagesInBlock ||
      !isInRegion(region, mapping.block)) {
    panic(message);
  }

  return true;
}

void PageMapping::commitMappingReplacement(MappingEntry &current,
                                           const MappingEntry &replacement,
                                           uint32_t mappingIndex) {
  if (!checkMappingAddress(
          replacement, "ftl: replacement mapping points outside its tier")) {
    panic("ftl: cannot commit an invalid replacement mapping");
  }

  MappingEntry previous = current;

  // The new physical location becomes authoritative before the old physical
  // subpage is invalidated.
  current = replacement;

  if (!previous.valid) {
    return;
  }

  if (!checkMappingAddress(previous,
                           "ftl: previous mapping points outside its tier")) {
    panic("ftl: valid previous mapping disappeared during commit");
  }

  if (previous.tier == replacement.tier &&
      previous.block == replacement.block &&
      previous.page == replacement.page) {
    panic("ftl: out-of-place write reused the previous physical location");
  }

  auto &oldRegion = getRegion(previous.tier);
  auto oldBlock = oldRegion.blocks.find(previous.block);

  if (oldBlock == oldRegion.blocks.end()) {
    panic("ftl: previous write block is not in use during commit");
  }

  oldBlock->second.invalidate(previous.page, mappingIndex);
}

uint64_t PageMapping::countWritableUnitsWithoutGC(
    const RegionState &region) const {
  return countWritableUnits(region, true);
}

uint64_t PageMapping::countWritableUnits(const RegionState &region,
                                         bool preserveGCReserve) const {
  uint64_t writablePhysicalPages = 0;

  for (uint32_t blockIndex : region.lastFreeBlock) {
    auto block = region.blocks.find(blockIndex);

    if (block == region.blocks.end()) {
      panic("ftl: active write block is missing");
    }

    uint32_t nextPage = block->second.getNextWritePageIndex();

    if (nextPage > param.pagesInBlock) {
      panic("ftl: active write block cursor is out of range");
    }

    writablePhysicalPages += param.pagesInBlock - nextPage;
  }

  uint64_t allocatableFreeBlocks = region.nFreeBlocks;

  if (preserveGCReserve) {
    const float gcThreshold =
        conf.readFloat(CONFIG_FTL, FTL_GC_THRESHOLD_RATIO);
    uint64_t gcReserveBlocks = static_cast<uint64_t>(
        std::ceil(gcThreshold * region.totalPhysicalBlocks));

    allocatableFreeBlocks =
        region.nFreeBlocks > gcReserveBlocks
            ? region.nFreeBlocks - gcReserveBlocks
            : 0;
  }

  writablePhysicalPages += allocatableFreeBlocks * param.pagesInBlock;

  return writablePhysicalPages * bitsetSize;
}

bool PageMapping::canAllocate(const RegionState &region, uint64_t units,
                              ReservationOwner owner,
                              bool withoutGarbageCollection) const {
  size_t rid = tierIndex(region.tier);
  uint64_t cacheReserved = pendingCacheWriteUnitsByTier.at(rid);
  uint64_t migrationReserved = pendingMigrationUnitsByTier.at(rid);
  uint64_t ownedUnits = 0;

  switch (owner) {
    case ReservationOwner::None:
      break;
    case ReservationOwner::CacheWrite:
      if (cacheReserved < units) {
        panic("ftl: cache write exceeds its target-tier reservation");
      }

      ownedUnits = units;
      break;
    case ReservationOwner::Migration:
      if (migrationReserved < units) {
        panic("ftl: migration exceeds its target-tier reservation");
      }

      ownedUnits = units;
      break;
    default:
      panic("ftl: write has an invalid reservation owner");
  }

  if (cacheReserved > std::numeric_limits<uint64_t>::max() -
                          migrationReserved) {
    panic("ftl: pending reservation counter overflow");
  }

  uint64_t otherReservations =
      cacheReserved + migrationReserved - ownedUnits;
  uint64_t writable =
      countWritableUnits(region, withoutGarbageCollection);

  return writable >= otherReservations &&
         units <= writable - otherReservations;
}

bool PageMapping::canAllocateAfterMigrationCancellation(
    const RegionState &region, uint64_t units, ReservationOwner owner,
    const std::array<uint64_t, 2> &migrationReleases,
    bool withoutGarbageCollection) const {
  size_t rid = tierIndex(region.tier);
  uint64_t cacheReserved = pendingCacheWriteUnitsByTier.at(rid);
  uint64_t migrationReserved = pendingMigrationUnitsByTier.at(rid);
  uint64_t ownedUnits = 0;

  if (migrationReserved < migrationReleases.at(rid)) {
    panic("ftl: migration cancellation exceeds pending reservations");
  }

  switch (owner) {
    case ReservationOwner::None:
      break;
    case ReservationOwner::CacheWrite:
      if (cacheReserved < units) {
        panic("ftl: cache write exceeds its target-tier reservation");
      }

      ownedUnits = units;
      break;
    case ReservationOwner::Migration:
      if (migrationReserved - migrationReleases.at(rid) < units) {
        panic("ftl: migration exceeds its target-tier reservation");
      }

      ownedUnits = units;
      break;
    default:
      panic("ftl: write has an invalid reservation owner");
  }

  uint64_t finalMigration =
      migrationReserved - migrationReleases.at(rid);

  if (cacheReserved >
      std::numeric_limits<uint64_t>::max() - finalMigration) {
    panic("ftl: pending reservation counter overflow");
  }

  uint64_t otherReservations =
      cacheReserved + finalMigration - ownedUnits;
  uint64_t writable =
      countWritableUnits(region, withoutGarbageCollection);

  return writable >= otherReservations &&
         units <= writable - otherReservations;
}

bool PageMapping::calculateFinalReservations(
    const std::array<uint64_t, 2> &cacheReleases,
    const std::array<uint64_t, 2> &cacheAdds,
    const std::array<uint64_t, 2> &migrationReleases,
    const std::array<uint64_t, 2> &migrationAdds,
    std::array<uint64_t, 2> &finalCache,
    std::array<uint64_t, 2> &finalMigration) const {
  for (size_t rid = 0; rid < regions.size(); rid++) {
    uint64_t cacheReserved = pendingCacheWriteUnitsByTier.at(rid);
    uint64_t migrationReserved = pendingMigrationUnitsByTier.at(rid);

    if (cacheReserved < cacheReleases.at(rid) ||
        migrationReserved < migrationReleases.at(rid)) {
      panic("ftl: reservation transaction underflow");
    }

    finalCache.at(rid) = cacheReserved - cacheReleases.at(rid);
    finalMigration.at(rid) =
        migrationReserved - migrationReleases.at(rid);

    if (finalCache.at(rid) >
            std::numeric_limits<uint64_t>::max() - cacheAdds.at(rid) ||
        finalMigration.at(rid) >
            std::numeric_limits<uint64_t>::max() - migrationAdds.at(rid)) {
      panic("ftl: reservation transaction overflow");
    }

    finalCache.at(rid) += cacheAdds.at(rid);
    finalMigration.at(rid) += migrationAdds.at(rid);

    if (finalCache.at(rid) >
        std::numeric_limits<uint64_t>::max() - finalMigration.at(rid)) {
      panic("ftl: total reservation transaction overflow");
    }

    if (countWritableUnitsWithoutGC(regions.at(rid)) <
        finalCache.at(rid) + finalMigration.at(rid)) {
      return false;
    }
  }

  return true;
}

void PageMapping::removePendingMigration(
    std::unordered_map<LCA, PendingMigration>::iterator iter,
    bool releaseReservation, CancellationReason reason) {
  if (iter == pendingMigrations.end() ||
      iter->second.orderIterator == pendingMigrationOrder.end() ||
      *iter->second.orderIterator != iter->first) {
    panic("ftl: pending migration containers are inconsistent");
  }

  if (releaseReservation) {
    releaseMigrationReservation(iter->second.targetTier, 1);
  }

  pendingMigrationOrder.erase(iter->second.orderIterator);
  pendingMigrations.erase(iter);

  switch (reason) {
    case CancellationReason::None:
      break;
    case CancellationReason::Write:
      migrationStat.cancelledByWrite++;
      break;
    case CancellationReason::Trim:
      migrationStat.cancelledByTrim++;
      break;
    case CancellationReason::Format:
      migrationStat.cancelledByFormat++;
      break;
  }
}

void PageMapping::cancelPendingMigration(LCA lca,
                                         CancellationReason reason) {
  auto iter = pendingMigrations.find(lca);

  if (iter != pendingMigrations.end()) {
    removePendingMigration(iter, true, reason);
  }
}

void PageMapping::consumeReservation(Tier tier, uint64_t units,
                                     ReservationOwner owner) {
  switch (owner) {
    case ReservationOwner::None:
      return;
    case ReservationOwner::CacheWrite:
      releaseCacheWriteReservation(tier, units);
      return;
    case ReservationOwner::Migration:
      releaseMigrationReservation(tier, units);
      return;
    default:
      panic("ftl: cannot consume an invalid reservation owner");
  }
}

bool PageMapping::reserveCacheWrite(Tier tier, uint64_t units) {
  if (!isValidTier(tier)) {
    return false;
  }

  auto &region = getRegion(tier);

  if (!canAllocate(region, units, ReservationOwner::None, true)) {
    return false;
  }

  auto &reserved = pendingCacheWriteUnitsByTier.at(tierIndex(tier));

  if (reserved > std::numeric_limits<uint64_t>::max() - units) {
    panic("ftl: cache reservation counter overflow");
  }

  reserved += units;

  return true;
}

bool PageMapping::admitCacheWrite(LCA lca, Tier targetTier,
                                  bool hasOldReservation, Tier oldTier) {
  if (!isValidTier(targetTier) ||
      (hasOldReservation && !isValidTier(oldTier)) ||
      lca >= param.globalLogicalUnits) {
    return false;
  }

  std::array<uint64_t, 2> cacheReleases{0, 0};
  std::array<uint64_t, 2> cacheAdds{0, 0};
  std::array<uint64_t, 2> migrationReleases{0, 0};
  std::array<uint64_t, 2> migrationAdds{0, 0};
  std::array<uint64_t, 2> finalCache{0, 0};
  std::array<uint64_t, 2> finalMigration{0, 0};
  auto pending = pendingMigrations.find(lca);

  if (hasOldReservation) {
    cacheReleases.at(tierIndex(oldTier))++;
  }

  cacheAdds.at(tierIndex(targetTier))++;

  if (pending != pendingMigrations.end()) {
    migrationReleases.at(tierIndex(pending->second.targetTier))++;
  }

  if (!calculateFinalReservations(
          cacheReleases, cacheAdds, migrationReleases, migrationAdds,
          finalCache, finalMigration)) {
    return false;
  }

  pendingCacheWriteUnitsByTier = finalCache;
  pendingMigrationUnitsByTier = finalMigration;

  if (pending != pendingMigrations.end()) {
    removePendingMigration(pending, false, CancellationReason::Write);
  }

  return true;
}

void PageMapping::releaseCacheWriteReservation(Tier tier, uint64_t units) {
  if (!isValidTier(tier)) {
    panic("ftl: invalid tier while releasing cache reservation");
  }

  auto &reserved = pendingCacheWriteUnitsByTier.at(tierIndex(tier));

  if (reserved < units) {
    panic("ftl: cache reservation counter underflow");
  }

  reserved -= units;
}

bool PageMapping::reserveMigration(Tier tier, uint64_t units) {
  if (!isValidTier(tier)) {
    return false;
  }

  auto &region = getRegion(tier);

  if (!canAllocate(region, units, ReservationOwner::None, true)) {
    return false;
  }

  auto &reserved = pendingMigrationUnitsByTier.at(tierIndex(tier));

  if (reserved > std::numeric_limits<uint64_t>::max() - units) {
    panic("ftl: migration reservation counter overflow");
  }

  reserved += units;

  return true;
}

void PageMapping::releaseMigrationReservation(Tier tier, uint64_t units) {
  if (!isValidTier(tier)) {
    panic("ftl: invalid tier while releasing migration reservation");
  }

  auto &reserved = pendingMigrationUnitsByTier.at(tierIndex(tier));

  if (reserved < units) {
    panic("ftl: migration reservation counter underflow");
  }

  reserved -= units;
}

float PageMapping::freeBlockRatio(RegionState &region) {
  return (float)region.nFreeBlocks / region.totalPhysicalBlocks;
}

uint32_t PageMapping::convertBlockIdx(uint32_t blockIdx) {
  return blockIdx % param.pageCountToMaxPerf;
}

bool PageMapping::getFreeBlock(RegionState &region, uint32_t idx,
                               uint32_t &blockIndex) {
  if (idx >= param.pageCountToMaxPerf) {
    panic("Index out of range");
  }

  if (region.nFreeBlocks == 0) {
    return false;
  }

  if (region.freeBlocks.empty()) {
    panic("ftl: free-block count does not match the free-block list");
  }

  // Search block which is blockIdx % param.pageCountToMaxPerf == idx
  auto iter = region.freeBlocks.begin();

  for (; iter != region.freeBlocks.end(); iter++) {
    blockIndex = iter->getBlockIndex();

    if (!isInRegion(region, blockIndex)) {
      panic("ftl: free block is outside selected region");
    }

    if (blockIndex % param.pageCountToMaxPerf == idx) {
      break;
    }
  }

  // Sanity check
  if (iter == region.freeBlocks.end()) {
    // Just use first one
    iter = region.freeBlocks.begin();
    blockIndex = iter->getBlockIndex();

    if (!isInRegion(region, blockIndex)) {
      panic("ftl: free block is outside selected region");
    }
  }

  // Insert found block to block list
  if (region.blocks.find(blockIndex) != region.blocks.end()) {
    panic("Corrupted");
  }

  region.blocks.emplace(blockIndex, std::move(*iter));

  // Remove found block from free block list
  region.freeBlocks.erase(iter);
  region.nFreeBlocks--;

  return true;
}

bool PageMapping::getLastFreeBlock(RegionState &region, Bitset &iomap,
                                   uint32_t &blockIndex) {
  uint32_t frontier = region.lastFreeBlockIndex;
  Bitset nextIOMap = region.lastFreeBlockIOMap;

  if (!bRandomTweak || (region.lastFreeBlockIOMap & iomap).any()) {
    frontier++;

    if (frontier == param.pageCountToMaxPerf) {
      frontier = 0;
    }

    nextIOMap = iomap;
  }
  else {
    nextIOMap |= iomap;
  }

  auto freeBlock = region.blocks.find(region.lastFreeBlock.at(frontier));

  // Sanity check
  if (freeBlock == region.blocks.end()) {
    panic("Corrupted");
  }

  // If current free block is full, get next block
  if (freeBlock->second.getNextWritePageIndex() == param.pagesInBlock) {
    uint32_t nextBlock;

    if (!getFreeBlock(region, frontier, nextBlock)) {
      return false;
    }

    region.lastFreeBlock.at(frontier) = nextBlock;

    region.bReclaimMore = true;
  }

  region.lastFreeBlockIndex = frontier;
  region.lastFreeBlockIOMap = nextIOMap;
  blockIndex = region.lastFreeBlock.at(frontier);

  return true;
}

// calculate weight of each block regarding victim selection policy
void PageMapping::calculateVictimWeight(
    std::vector<std::pair<uint32_t, float>> &weight, RegionState &region,
    const EVICT_POLICY policy, uint64_t tick) {
  float temp;

  weight.reserve(region.blocks.size());

  switch (policy) {
    case POLICY_GREEDY:
    case POLICY_RANDOM:
    case POLICY_DCHOICE:
      for (auto &iter : region.blocks) {
        if (!isInRegion(region, iter.first)) {
          panic("ftl: used block is outside selected region");
        }

        if (iter.second.getNextWritePageIndex() != param.pagesInBlock) {
          continue;
        }

        weight.push_back({iter.first, iter.second.getValidPageCountRaw()});
      }

      break;
    case POLICY_COST_BENEFIT:
      for (auto &iter : region.blocks) {
        if (!isInRegion(region, iter.first)) {
          panic("ftl: used block is outside selected region");
        }

        if (iter.second.getNextWritePageIndex() != param.pagesInBlock) {
          continue;
        }

        temp = (float)(iter.second.getValidPageCountRaw()) / param.pagesInBlock;

        weight.push_back(
            {iter.first,
             temp / ((1 - temp) * (tick - iter.second.getLastAccessedTime()))});
      }

      break;
    default:
      panic("Invalid evict policy");
  }
}

void PageMapping::selectVictimBlock(std::vector<uint32_t> &list,
                                    RegionState &region, uint64_t &tick) {
  static const GC_MODE mode = (GC_MODE)conf.readInt(CONFIG_FTL, FTL_GC_MODE);
  static const EVICT_POLICY policy =
      (EVICT_POLICY)conf.readInt(CONFIG_FTL, FTL_GC_EVICT_POLICY);
  static uint32_t dChoiceParam =
      conf.readUint(CONFIG_FTL, FTL_GC_D_CHOICE_PARAM);
  uint64_t nBlocks = conf.readUint(CONFIG_FTL, FTL_GC_RECLAIM_BLOCK);
  std::vector<std::pair<uint32_t, float>> weight;

  list.clear();

  // Calculate number of blocks to reclaim
  if (mode == GC_MODE_0) {
    // DO NOTHING
  }
  else if (mode == GC_MODE_1) {
    static const float t = conf.readFloat(CONFIG_FTL, FTL_GC_RECLAIM_THRESHOLD);

    nBlocks = region.totalPhysicalBlocks * t - region.nFreeBlocks;
  }
  else {
    panic("Invalid GC mode");
  }

  // reclaim one more if last free block fully used
  if (region.bReclaimMore) {
    nBlocks += param.pageCountToMaxPerf;

    region.bReclaimMore = false;
  }

  // Calculate weights of all blocks
  calculateVictimWeight(weight, region, policy, tick);

  if (weight.size() == 0) {
    panic("ftl: no GC victim candidate in selected region");
  }

  if (policy == POLICY_RANDOM || policy == POLICY_DCHOICE) {
    uint64_t randomRange =
        policy == POLICY_RANDOM ? nBlocks : dChoiceParam * nBlocks;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, weight.size() - 1);
    std::vector<std::pair<uint32_t, float>> selected;

    while (selected.size() < randomRange) {
      uint64_t idx = dist(gen);

      if (weight.at(idx).first < std::numeric_limits<uint32_t>::max()) {
        selected.push_back(weight.at(idx));
        weight.at(idx).first = std::numeric_limits<uint32_t>::max();
      }
    }

    weight = std::move(selected);
  }

  // Sort weights
  std::sort(
      weight.begin(), weight.end(),
      [](std::pair<uint32_t, float> a, std::pair<uint32_t, float> b) -> bool {
        return a.second < b.second;
      });

  // Select victims from the blocks with the lowest weight
  nBlocks = MIN(nBlocks, weight.size());

  for (uint64_t i = 0; i < nBlocks; i++) {
    list.push_back(weight.at(i).first);
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::SELECT_VICTIM_BLOCK);
}

void PageMapping::doGarbageCollection(std::vector<uint32_t> &blocksToReclaim,
                                      RegionState &region, uint64_t &tick) {
  struct GCSource {
    LCA lca;
    MappingEntry source;
    uint32_t mappingIndex;
  };

  PAL::Request req(param.ioUnitInPage);
  std::vector<PAL::Request> readRequests;
  std::vector<PAL::Request> eraseRequests;
  std::vector<GCSource> sources;
  std::vector<uint64_t> lpns;
  Bitset bit(param.ioUnitInPage);
  uint64_t beginAt;
  uint64_t readFinishedAt = tick;
  uint64_t writeFinishedAt = tick;
  uint64_t eraseFinishedAt = tick;

  req.tier = region.tier;

  if (blocksToReclaim.size() == 0) {
    return;
  }

  // For all blocks to reclaim, collecting request structure only
  for (auto &iter : blocksToReclaim) {
    if (!isInRegion(region, iter)) {
      panic("ftl: victim block is outside selected region");
    }

    auto block = region.blocks.find(iter);

    if (block == region.blocks.end()) {
      panic("Invalid block");
    }

    // Copy valid pages to free block
    for (uint32_t pageIndex = 0; pageIndex < param.pagesInBlock; pageIndex++) {
      // Valid?
      if (block->second.getPageInfo(pageIndex, lpns, bit)) {
        if (!bRandomTweak) {
          bit.set();
        }

        // Issue Read
        req.blockIndex = block->first;
        req.pageIndex = pageIndex;
        req.ioFlag = bit;

        readRequests.push_back(req);

        // Resolve and validate every reverse-mapped logical sub-entry before
        // any victim data is moved or invalidated.
        for (uint32_t idx = 0; idx < bitsetSize; idx++) {
          if (bit.test(idx)) {
            LPN lpn = lpns.at(idx);
            LCA lca = lpnToLca(lpn, idx, bitsetSize);
            auto mappingList = table.find(lpn);

            if (mappingList == table.end()) {
              panic("ftl: GC reverse mapping has no global table entry");
            }

            pDRAM->read(&(*mappingList),
                        sizeof(MappingEntry) * bitsetSize, tick);

            auto &mapping = mappingList->second.at(idx);

            if (!checkMappingAddress(
                    mapping, "ftl: GC found an invalid global mapping") ||
                mapping.tier != region.tier || mapping.block != block->first ||
                mapping.page != pageIndex) {
              panic("ftl: stale or inconsistent reverse mapping during GC");
            }

            if (!block->second.read(pageIndex, idx, tick)) {
              panic("ftl: GC reverse mapping points to invalid victim data");
            }

            sources.push_back({lca, mapping, idx});
          }
        }

        region.stat.validSuperPageCopies++;
      }
    }

    // Erase block
    req.blockIndex = block->first;
    req.pageIndex = 0;
    req.ioFlag.set();

    eraseRequests.push_back(req);
  }

  // Do actual I/O here
  // This handles PAL2 limitation (SIGSEGV, infinite loop, or so-on)
  for (auto &iter : readRequests) {
    beginAt = tick;

    pPAL->read(iter, beginAt);

    readFinishedAt = MAX(readFinishedAt, beginAt);
  }

  // Every source page is now available in the GC data path. Relocate each
  // logical sub-entry exactly once, selecting its queued migration target when
  // present or the victim tier otherwise.
  for (const auto &source : sources) {
    beginAt = readFinishedAt;
    LPN lpn = lcaToLpn(source.lca, bitsetSize);
    auto mappingList = table.find(lpn);

    if (mappingList == table.end()) {
      panic("ftl: GC source mapping disappeared after victim read");
    }

    auto &mapping = mappingList->second.at(source.mappingIndex);

    if (!checkMappingAddress(
            mapping, "ftl: GC source mapping became invalid") ||
        mapping.tier != source.source.tier ||
        mapping.block != source.source.block ||
        mapping.page != source.source.page) {
      panic("ftl: stale or inconsistent reverse mapping during GC commit");
    }

    auto pending = pendingMigrations.find(source.lca);

    if (pending != pendingMigrations.end() &&
        pending->second.targetTier == region.tier) {
      // The request is already satisfied by the current tier, but the victim
      // data still needs ordinary same-tier relocation before erase.
      removePendingMigration(pending, true);
      pending = pendingMigrations.end();
    }

    if (pending != pendingMigrations.end()) {
      if (pending->second.targetTier == region.tier) {
        panic("ftl: stale same-tier migration survived GC no-op removal");
      }

      MigrationStatus status = executePendingMigration(
          source.lca, MigrationTrigger::GarbageCollection, beginAt,
          &source.source, true);

      if (status != MigrationStatus::Success) {
        panic("ftl: GC could not honor a reserved pending migration");
      }
    }
    else {
      Bitset relocationIOMap(param.ioUnitInPage);

      if (bRandomTweak) {
        relocationIOMap.set(source.mappingIndex);
      }
      else {
        relocationIOMap.set();
      }

      // Normal GC consumes the source tier's configured reserve and never
      // chooses a cross-tier destination on its own.
      uint32_t destinationBlock;

      if (!getLastFreeBlock(region, relocationIOMap, destinationBlock)) {
        panic("ftl: GC cannot allocate its reserved same-tier destination");
      }

      auto freeBlock = region.blocks.find(destinationBlock);

      if (freeBlock == region.blocks.end() ||
          !isInRegion(region, freeBlock->first)) {
        panic("ftl: GC destination block is outside victim tier");
      }

      uint32_t destinationPage =
          freeBlock->second.getNextWritePageIndex(source.mappingIndex);

      if (!freeBlock->second.write(destinationPage, lpn,
                                   source.mappingIndex, beginAt)) {
        panic("ftl: GC same-tier destination is not writable");
      }

      PAL::Request writeRequest(param.ioUnitInPage);
      writeRequest.tier = region.tier;
      writeRequest.blockIndex = destinationBlock;
      writeRequest.pageIndex = destinationPage;
      writeRequest.ioFlag = relocationIOMap;
      pPAL->write(writeRequest, beginAt);

      if (mapping.tier != source.source.tier ||
          mapping.block != source.source.block ||
          mapping.page != source.source.page) {
        panic("ftl: GC mapping changed before same-tier commit");
      }

      commitMappingReplacement(
          mapping,
          MappingEntry(region.tier, destinationBlock, destinationPage),
          source.mappingIndex);
    }

    writeFinishedAt = MAX(writeFinishedAt, beginAt);
    region.stat.validPageCopies++;
  }

  for (auto &iter : eraseRequests) {
    beginAt = readFinishedAt;

    eraseInternal(iter, region, beginAt);

    eraseFinishedAt = MAX(eraseFinishedAt, beginAt);
  }

  tick = MAX(writeFinishedAt, eraseFinishedAt);
  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::DO_GARBAGE_COLLECTION);
}

void PageMapping::readInternal(Request &req, uint64_t &tick) {
  PAL::Request palRequest(req);
  uint64_t beginAt;
  uint64_t finishedAt = tick;

  auto mappingList = table.find(req.lpn);

  if (mappingList != table.end()) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList),
                  sizeof(MappingEntry) * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), sizeof(MappingEntry), tick);
    }

    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      if (req.ioFlag.test(idx) || !bRandomTweak) {
        auto &mapping = mappingList->second.at(idx);

        if (checkMappingAddress(mapping,
                                "ftl: read mapping points outside its tier")) {
          auto &region = getRegion(mapping.tier);
          palRequest.tier = mapping.tier;
          palRequest.blockIndex = mapping.block;
          palRequest.pageIndex = mapping.page;

          if (bRandomTweak) {
            palRequest.ioFlag.reset();
            palRequest.ioFlag.set(idx);
          }
          else {
            palRequest.ioFlag.set();
          }

          auto block = region.blocks.find(palRequest.blockIndex);

          if (block == region.blocks.end()) {
            panic("Block is not in use");
          }

          beginAt = tick;

          if (!block->second.read(palRequest.pageIndex, idx, beginAt)) {
            panic("ftl: global read mapping points to invalid physical data");
          }
          pPAL->read(palRequest, beginAt);

          finishedAt = MAX(finishedAt, beginAt);
        }
      }
    }

    tick = finishedAt;
    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::READ_INTERNAL);
  }
}

bool PageMapping::writeInternal(Request &req, RegionState &region,
                                uint64_t &tick, bool sendToPAL) {
  PAL::Request palRequest(req);
  auto mappingList = table.find(req.lpn);
  uint64_t beginAt;
  uint64_t finishedAt = tick;
  bool readBeforeWrite = false;

  // Validate every existing source before changing allocator or physical
  // state. Mixed-tier sub-entries are expected and resolved independently.
  if (mappingList != table.end()) {
    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      if (req.ioFlag.test(idx) || !bRandomTweak) {
        auto &mapping = mappingList->second.at(idx);

        if (checkMappingAddress(mapping,
                                "ftl: overwrite source is inconsistent")) {
          auto &oldRegion = getRegion(mapping.tier);
          auto oldBlock = oldRegion.blocks.find(mapping.block);

          if (oldBlock == oldRegion.blocks.end()) {
            panic("ftl: overwrite source block is not in use");
          }
        }
      }
    }
  }

  // Admission/allocation happens before the global table or old physical
  // mapping is modified. A normal no-space condition is recoverable.
  uint32_t targetBlockIndex;

  if (!getLastFreeBlock(region, req.ioFlag, targetBlockIndex)) {
    return false;
  }

  auto block = region.blocks.find(targetBlockIndex);

  if (block == region.blocks.end() || !isInRegion(region, block->first)) {
    panic("ftl: allocated write block is outside target tier");
  }

  if (mappingList == table.end()) {
    auto ret = table.emplace(req.lpn, std::vector<MappingEntry>(bitsetSize));

    if (!ret.second) {
      panic("Failed to insert new mapping");
    }

    mappingList = ret.first;
  }

  if (sendToPAL) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList),
                  sizeof(MappingEntry) * req.ioFlag.count(), tick);
      pDRAM->write(&(*mappingList),
                   sizeof(MappingEntry) * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), sizeof(MappingEntry), tick);
      pDRAM->write(&(*mappingList), sizeof(MappingEntry), tick);
    }
  }

  if (!bRandomTweak && !req.ioFlag.all()) {
    // We have to read old data
    readBeforeWrite = true;
  }

  for (uint32_t idx = 0; idx < bitsetSize; idx++) {
    if (req.ioFlag.test(idx) || !bRandomTweak) {
      uint32_t pageIndex = block->second.getNextWritePageIndex(idx);
      auto &mapping = mappingList->second.at(idx);
      MappingEntry previous = mapping;

      beginAt = tick;

      // Read old data if needed (Only executed when bRandomTweak = false)
      // Maybe some other init procedures want to perform 'partial-write'
      // So check sendToPAL variable
      if (readBeforeWrite && sendToPAL && previous.valid) {
        palRequest.tier = previous.tier;
        palRequest.blockIndex = previous.block;
        palRequest.pageIndex = previous.page;

        // We don't need to read old data
        palRequest.ioFlag = req.ioFlag;
        palRequest.ioFlag.flip();

        pPAL->read(palRequest, beginAt);
      }

      block->second.write(pageIndex, req.lpn, idx, beginAt);

      if (sendToPAL) {
        palRequest.tier = region.tier;
        palRequest.blockIndex = block->first;
        palRequest.pageIndex = pageIndex;

        if (bRandomTweak) {
          palRequest.ioFlag.reset();
          palRequest.ioFlag.set(idx);
        }
        else {
          palRequest.ioFlag.set();
        }

        pPAL->write(palRequest, beginAt);
      }

      // Commit only after the target program has been issued. The helper
      // publishes the new global mapping before invalidating the old page.
      commitMappingReplacement(
          mapping, MappingEntry(region.tier, block->first, pageIndex), idx);

      finishedAt = MAX(finishedAt, beginAt);
    }
  }

  // Exclude CPU operation when initializing
  if (sendToPAL) {
    tick = finishedAt;
    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE_INTERNAL);
  }

  // GC if needed
  // I assumed that init procedure never invokes GC
  static float gcThreshold = conf.readFloat(CONFIG_FTL, FTL_GC_THRESHOLD_RATIO);

  if (freeBlockRatio(region) < gcThreshold) {
    if (!sendToPAL) {
      panic("ftl: GC triggered while in initialization");
    }

    std::vector<uint32_t> list;
    uint64_t beginAt = tick;

    selectVictimBlock(list, region, beginAt);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "GC   | On-demand | %u blocks will be reclaimed", list.size());

    doGarbageCollection(list, region, beginAt);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "GC   | Done | %" PRIu64 " - %" PRIu64 " (%" PRIu64 ")", tick,
               beginAt, beginAt - tick);

    region.stat.gcCount++;
    region.stat.reclaimedBlocks += list.size();
  }

  return true;
}

void PageMapping::trimInternal(Request &req, uint64_t &tick) {
  auto mappingList = table.find(req.lpn);

  if (mappingList != table.end()) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList),
                  sizeof(MappingEntry) * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), sizeof(MappingEntry), tick);
    }

    // Do trim
    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      if (!req.ioFlag.test(idx) && bRandomTweak) {
        continue;
      }

      auto &mapping = mappingList->second.at(idx);

      if (!checkMappingAddress(
              mapping, "ftl: trim mapping points outside its tier")) {
        continue;
      }

      auto &region = getRegion(mapping.tier);
      auto block = region.blocks.find(mapping.block);

      if (block == region.blocks.end()) {
        panic("Block is not in use");
      }

      block->second.invalidate(mapping.page, idx);
      mapping = MappingEntry();
    }

    bool mappingEmpty = true;

    for (auto &mapping : mappingList->second) {
      if (mapping.valid) {
        checkMappingAddress(mapping,
                            "ftl: trim mapping points outside its tier");
        mappingEmpty = false;

        break;
      }
    }

    if (mappingEmpty) {
      table.erase(mappingList);
    }

    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::TRIM_INTERNAL);
  }
}

void PageMapping::eraseInternal(PAL::Request &req, RegionState &region,
                                uint64_t &tick) {
  static uint64_t threshold =
      conf.readUint(CONFIG_FTL, FTL_BAD_BLOCK_THRESHOLD);
  auto block = region.blocks.find(req.blockIndex);

  // Sanity checks
  if (block == region.blocks.end()) {
    panic("No such block");
  }

  if (!isInRegion(region, req.blockIndex)) {
    panic("ftl: erase block is outside selected region");
  }

  if (block->second.getValidPageCount() != 0) {
    panic("There are valid pages in victim block");
  }

  // Erase block
  block->second.erase();

  pPAL->erase(req, tick);

  // Check erase count
  uint32_t erasedCount = block->second.getEraseCount();

  if (erasedCount < threshold) {
    // Reverse search
    auto iter = region.freeBlocks.end();

    while (true) {
      iter--;

      if (iter->getEraseCount() <= erasedCount) {
        // emplace: insert before pos
        iter++;

        break;
      }

      if (iter == region.freeBlocks.begin()) {
        break;
      }
    }

    // Insert block to free block list
    region.freeBlocks.emplace(iter, std::move(block->second));
    region.nFreeBlocks++;
  }

  // Remove block from block list
  region.blocks.erase(block);

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::ERASE_INTERNAL);
}

float PageMapping::calculateWearLeveling(RegionState &region) {
  uint64_t totalEraseCnt = 0;
  uint64_t sumOfSquaredEraseCnt = 0;
  uint64_t numOfBlocks = region.totalPhysicalBlocks;
  uint64_t eraseCnt;

  for (auto &iter : region.blocks) {
    eraseCnt = iter.second.getEraseCount();
    totalEraseCnt += eraseCnt;
    sumOfSquaredEraseCnt += eraseCnt * eraseCnt;
  }

  // freeBlocks is sorted
  // Calculate from backward, stop when eraseCnt is zero
  for (auto riter = region.freeBlocks.rbegin(); riter != region.freeBlocks.rend();
       riter++) {
    eraseCnt = riter->getEraseCount();

    if (eraseCnt == 0) {
      break;
    }

    totalEraseCnt += eraseCnt;
    sumOfSquaredEraseCnt += eraseCnt * eraseCnt;
  }

  if (sumOfSquaredEraseCnt == 0) {
    return -1;  // no meaning of wear-leveling
  }

  return (float)totalEraseCnt * totalEraseCnt /
         (numOfBlocks * sumOfSquaredEraseCnt);
}

void PageMapping::calculateTotalPages(RegionState &region, uint64_t &valid,
                                      uint64_t &invalid) {
  valid = 0;
  invalid = 0;

  for (auto &iter : region.blocks) {
    valid += iter.second.getValidPageCount();
    invalid += iter.second.getDirtyPageCount();
  }
}

void PageMapping::getStatList(std::vector<Stats> &list, std::string prefix) {
  Stats temp;

  temp.name = prefix + "page_mapping.slc.gc.count";
  temp.desc = "Total GC count";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.slc.gc.reclaimed_blocks";
  temp.desc = "Total reclaimed blocks in GC";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.slc.gc.superpage_copies";
  temp.desc = "Total copied valid superpages during GC";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.slc.gc.page_copies";
  temp.desc = "Total copied valid pages during GC";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.slc.wear_leveling";
  temp.desc = "Wear-leveling factor";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.tlc.gc.count";
  temp.desc = "Total GC count";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.tlc.gc.reclaimed_blocks";
  temp.desc = "Total reclaimed blocks in GC";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.tlc.gc.superpage_copies";
  temp.desc = "Total copied valid superpages during GC";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.tlc.gc.page_copies";
  temp.desc = "Total copied valid pages during GC";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.tlc.wear_leveling";
  temp.desc = "Wear-leveling factor";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.slc.pending_cache_write_units";
  temp.desc = "Pending SLC cache-write reservations";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.tlc.pending_cache_write_units";
  temp.desc = "Pending TLC cache-write reservations";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.slc.pending_migration_units";
  temp.desc = "Pending SLC migration reservations";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.tlc.pending_migration_units";
  temp.desc = "Pending TLC migration reservations";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.migration.current_queue_entries";
  temp.desc = "Current deferred migration queue entries";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.migration.maximum_queue_entries";
  temp.desc = "Maximum deferred migration queue entries";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.migration.executed_by_gc";
  temp.desc = "Migrations executed by GC piggyback";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.migration.executed_by_list_full";
  temp.desc = "Migrations executed by list-full drain";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.migration.cancelled_by_write";
  temp.desc = "Pending migrations cancelled by admitted Write";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.migration.cancelled_by_trim";
  temp.desc = "Pending migrations cancelled by Trim";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.migration.cancelled_by_format";
  temp.desc = "Pending migrations cancelled by Format";
  list.push_back(temp);
}

void PageMapping::getStatValues(std::vector<double> &values) {
  for (auto &region : regions) {
    values.push_back(region.stat.gcCount);
    values.push_back(region.stat.reclaimedBlocks);
    values.push_back(region.stat.validSuperPageCopies);
    values.push_back(region.stat.validPageCopies);
    values.push_back(calculateWearLeveling(region));
  }

  values.push_back(pendingCacheWriteUnitsByTier.at(tierIndex(Tier::SLC)));
  values.push_back(pendingCacheWriteUnitsByTier.at(tierIndex(Tier::TLC)));
  values.push_back(pendingMigrationUnitsByTier.at(tierIndex(Tier::SLC)));
  values.push_back(pendingMigrationUnitsByTier.at(tierIndex(Tier::TLC)));
  values.push_back(pendingMigrations.size());
  values.push_back(migrationStat.maximumQueueEntries);
  values.push_back(migrationStat.executedByGarbageCollection);
  values.push_back(migrationStat.executedByListFull);
  values.push_back(migrationStat.cancelledByWrite);
  values.push_back(migrationStat.cancelledByTrim);
  values.push_back(migrationStat.cancelledByFormat);
}

void PageMapping::resetStatValues() {
  for (auto &region : regions) {
    memset(&region.stat, 0, sizeof(region.stat));
  }

  memset(&migrationStat, 0, sizeof(migrationStat));
  migrationStat.maximumQueueEntries = pendingMigrations.size();
}

}  // namespace FTL

}  // namespace SimpleSSD

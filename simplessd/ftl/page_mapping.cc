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
#include <limits>
#include <random>

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
      totalLogicalBlocks(0),
      totalLogicalPages(0),
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
              RegionState(param.pageCountToMaxPerf, param.ioUnitInPage)} {
  bRandomTweak = conf.readBoolean(CONFIG_FTL, FTL_USE_RANDOM_IO_TWEAK);
  bitsetSize = bRandomTweak ? param.ioUnitInPage : 1;

  uint32_t begin = 0;

  for (uint32_t rid = 0; rid < regions.size(); rid++) {
    auto &region = regions[rid];

    region.tier = rid == 0 ? Tier::SLC : Tier::TLC;
    region.blockBegin = begin;
    region.blockEnd = begin + param.totalPhysicalBlocksByTier[rid];
    region.totalPhysicalBlocks = param.totalPhysicalBlocksByTier[rid];
    region.totalLogicalBlocks = param.totalLogicalBlocksByTier[rid];
    region.totalLogicalPages = region.totalLogicalBlocks * param.pagesInBlock;
    region.status.totalLogicalPages = region.totalLogicalPages;

    region.blocks.reserve(region.totalPhysicalBlocks);
    region.table.reserve(region.totalLogicalPages);

    for (uint32_t i = region.blockBegin; i < region.blockEnd; i++) {
      region.freeBlocks.emplace_back(
          Block(i, param.pagesInBlock, param.ioUnitInPage));
    }

    region.nFreeBlocks = region.totalPhysicalBlocks;

    for (uint32_t i = 0; i < param.pageCountToMaxPerf; i++) {
      region.lastFreeBlock.at(i) = getFreeBlock(region, i);
    }

    begin = region.blockEnd;
  }
}

PageMapping::~PageMapping() {}

bool PageMapping::initialize() {
  uint64_t tick;
  uint64_t valid;
  uint64_t invalid;
  FILLING_MODE mode;

  Request req(param.ioUnitInPage);

  debugprint(LOG_FTL_PAGE_MAPPING, "Initialization started");

  mode = (FILLING_MODE)conf.readUint(CONFIG_FTL, FTL_FILLING_MODE);

  for (auto &region : regions) {
    const char *tierName = region.tier == Tier::SLC ? "SLC" : "TLC";
    float fillRatio =
        conf.readFloat(CONFIG_FTL, region.tier == Tier::SLC
                                       ? FTL_SLC_FILL_RATIO
                                       : FTL_TLC_FILL_RATIO);
    float invalidRatio =
        conf.readFloat(CONFIG_FTL, region.tier == Tier::SLC
                                       ? FTL_SLC_INVALID_PAGE_RATIO
                                       : FTL_TLC_INVALID_PAGE_RATIO);
    uint64_t nTotalLogicalPages = region.totalLogicalPages;
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

    // Step 1. Filling
    if (mode == FILLING_MODE_0 || mode == FILLING_MODE_1) {
      // Sequential
      for (uint64_t i = 0; i < nPagesToWarmup; i++) {
        tick = 0;
        req.lpn = i;
        writeInternal(req, region, tick, false);
      }
    }
    else {
      // Random
      std::random_device rd;
      std::mt19937_64 gen(rd());
      std::uniform_int_distribution<uint64_t> dist(0, nTotalLogicalPages - 1);

      for (uint64_t i = 0; i < nPagesToWarmup; i++) {
        tick = 0;
        req.lpn = dist(gen);
        writeInternal(req, region, tick, false);
      }
    }

    // Step 2. Invalidating
    if (mode == FILLING_MODE_0) {
      // Sequential
      for (uint64_t i = 0; i < nPagesToInvalidate; i++) {
        tick = 0;
        req.lpn = i;
        writeInternal(req, region, tick, false);
      }
    }
    else if (mode == FILLING_MODE_1) {
      std::random_device rd;
      std::mt19937_64 gen(rd());
      std::uniform_int_distribution<uint64_t> dist(0, nPagesToWarmup - 1);

      for (uint64_t i = 0; i < nPagesToInvalidate; i++) {
        tick = 0;
        req.lpn = dist(gen);
        writeInternal(req, region, tick, false);
      }
    }
    else {
      // Random
      std::random_device rd;
      std::mt19937_64 gen(rd());
      std::uniform_int_distribution<uint64_t> dist(0, nTotalLogicalPages - 1);

      for (uint64_t i = 0; i < nPagesToInvalidate; i++) {
        tick = 0;
        req.lpn = dist(gen);
        writeInternal(req, region, tick, false);
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

  debugprint(LOG_FTL_PAGE_MAPPING, "Initialization finished");

  return true;
}

void PageMapping::read(Request &req, uint64_t &tick) {
  uint64_t begin = tick;
  auto &region = getRegion(req.tier);

  if (req.lpn >= region.totalLogicalPages) {
    panic("ftl: read LPN out of selected tier range");
  }

  if (req.ioFlag.count() > 0) {
    readInternal(req, region, tick);

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

void PageMapping::write(Request &req, uint64_t &tick) {
  uint64_t begin = tick;
  auto &region = getRegion(req.tier);

  if (req.lpn >= region.totalLogicalPages) {
    panic("ftl: write LPN out of selected tier range");
  }

  if (req.ioFlag.count() > 0) {
    writeInternal(req, region, tick);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "WRITE | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               req.lpn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE);
}

void PageMapping::trim(Request &req, uint64_t &tick) {
  uint64_t begin = tick;
  auto &region = getRegion(req.tier);

  if (req.lpn >= region.totalLogicalPages) {
    panic("ftl: trim LPN out of selected tier range");
  }

  trimInternal(req, region, tick);

  debugprint(LOG_FTL_PAGE_MAPPING,
             "TRIM  | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
             ")",
             req.lpn, begin, tick, tick - begin);

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::TRIM);
}

bool PageMapping::migrate(MigrationRequest &req, uint64_t &tick) {
  RegionState &srcRegion = getRegion(req.srcTier);
  RegionState &dstRegion = getRegion(req.dstTier);
  uint64_t beginAt;
  uint64_t finishedAt = tick;

  if (req.srcTier == req.dstTier) {
    warn("ftl: migration source and destination tier are identical");
    return false;
  }

  if (req.nlp == 0) {
    warn("ftl: migration requested zero logical pages");
    return false;
  }

  for (uint64_t i = 0; i < req.nlp; i++) {
    uint64_t srcLCA = req.srcLPN + i;
    uint64_t dstLCA = req.dstLPN + i;

    if (srcLCA >= srcRegion.totalLogicalPages * bitsetSize ||
        dstLCA >= dstRegion.totalLogicalPages * bitsetSize) {
      warn("ftl: migration LPN out of selected tier range");
      return false;
    }

    if (!isValidMapping(srcRegion, srcLCA / bitsetSize,
                        srcLCA % bitsetSize)) {
      warn("ftl: migration source is invalid");
      return false;
    }

    if (isValidMapping(dstRegion, dstLCA / bitsetSize, dstLCA % bitsetSize)) {
      warn("ftl: migration destination is already valid");
      return false;
    }
  }

  for (uint64_t i = 0; i < req.nlp; i++) {
    uint64_t srcLCA = req.srcLPN + i;
    uint64_t dstLCA = req.dstLPN + i;
    uint64_t srcLPN = srcLCA / bitsetSize;
    uint64_t dstLPN = dstLCA / bitsetSize;
    uint32_t srcIdx = srcLCA % bitsetSize;
    uint32_t dstIdx = dstLCA % bitsetSize;
    Bitset iomap(param.ioUnitInPage);

    auto srcMappingList = srcRegion.table.find(srcLPN);
    auto dstMappingList = dstRegion.table.find(dstLPN);

    if (srcMappingList == srcRegion.table.end()) {
      panic("ftl: migration source mapping disappeared");
    }

    if (dstMappingList == dstRegion.table.end()) {
      auto ret = dstRegion.table.emplace(
          dstLPN,
          std::vector<std::pair<uint32_t, uint32_t>>(
              bitsetSize, {param.totalPhysicalBlocks, param.pagesInBlock}));

      if (!ret.second) {
        panic("ftl: failed to insert migration destination mapping");
      }

      dstMappingList = ret.first;
    }

    auto &srcMapping = srcMappingList->second.at(srcIdx);
    auto &dstMapping = dstMappingList->second.at(dstIdx);

    if (!checkMappingAddress(srcRegion, srcMapping,
                             "ftl: migration source mapping is invalid")) {
      panic("ftl: migration source mapping disappeared");
    }

    if (checkMappingAddress(dstRegion, dstMapping,
                            "ftl: migration destination mapping is invalid")) {
      panic("ftl: migration destination became valid");
    }

    auto srcBlock = srcRegion.blocks.find(srcMapping.first);

    if (srcBlock == srcRegion.blocks.end()) {
      panic("ftl: migration source block is not in use");
    }

    iomap.set(dstIdx);

    auto dstBlock = dstRegion.blocks.find(getLastFreeBlock(dstRegion, iomap));

    if (dstBlock == dstRegion.blocks.end() ||
        !isInRegion(dstRegion, dstBlock->first)) {
      panic("ftl: migration destination block is outside selected region");
    }

    uint32_t dstPageIndex = dstBlock->second.getNextWritePageIndex(dstIdx);
    PAL::Request palReq(param.ioUnitInPage);

    beginAt = tick;

    srcBlock->second.read(srcMapping.second, srcIdx, beginAt);
    palReq.tier = req.srcTier;
    palReq.blockIndex = srcMapping.first;
    palReq.pageIndex = srcMapping.second;
    palReq.ioFlag.reset();
    palReq.ioFlag.set(srcIdx);
    pPAL->read(palReq, beginAt);

    dstBlock->second.write(dstPageIndex, dstLPN, dstIdx, beginAt);
    palReq.tier = req.dstTier;
    palReq.blockIndex = dstBlock->first;
    palReq.pageIndex = dstPageIndex;
    palReq.ioFlag.reset();
    palReq.ioFlag.set(dstIdx);
    pPAL->write(palReq, beginAt);

    srcBlock->second.invalidate(srcMapping.second, srcIdx);
    srcMapping = {param.totalPhysicalBlocks, param.pagesInBlock};
    dstMapping = {dstBlock->first, dstPageIndex};

    bool sourceEmpty = true;

    for (auto &mapping : srcMappingList->second) {
      if (checkMappingAddress(srcRegion, mapping,
                              "ftl: migration source mapping is invalid")) {
        sourceEmpty = false;

        break;
      }
    }

    if (sourceEmpty) {
      srcRegion.table.erase(srcMappingList);
    }

    finishedAt = MAX(finishedAt, beginAt);
  }

  tick = finishedAt;
  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE_INTERNAL);

  return true;
}

void PageMapping::format(LPNRange &range, uint64_t &tick) {
  PAL::Request req(param.ioUnitInPage);
  std::vector<uint32_t> list;
  auto &region = getRegion(range.tier);

  req.tier = region.tier;
  req.ioFlag.set();

  for (auto iter = region.table.begin(); iter != region.table.end();) {
    if (iter->first >= range.slpn && iter->first < range.slpn + range.nlp) {
      auto &mappingList = iter->second;

      // Do trim
      for (uint32_t idx = 0; idx < bitsetSize; idx++) {
        auto &mapping = mappingList.at(idx);

        if (!checkMappingAddress(region, mapping,
                                 "ftl: format mapping points outside selected region")) {
          continue;
        }

        auto block = region.blocks.find(mapping.first);

        if (block == region.blocks.end()) {
          panic("Block is not in use");
        }

        block->second.invalidate(mapping.second, idx);

        // Collect block indices
        list.push_back(mapping.first);
      }

      iter = region.table.erase(iter);
    }
    else {
      iter++;
    }
  }

  // Get blocks to erase
  std::sort(list.begin(), list.end());
  auto last = std::unique(list.begin(), list.end());
  list.erase(last, list.end());

  // Do GC only in specified blocks
  doGarbageCollection(list, region, tick);

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::FORMAT);
}

Status *PageMapping::getStatus(uint64_t lpnBegin, uint64_t lpnEnd) {
  status.totalLogicalPages = 0;
  status.freePhysicalBlocks = 0;
  status.mappedLogicalPages = 0;

  for (auto &region : regions) {
    status.totalLogicalPages += region.totalLogicalPages;
    status.freePhysicalBlocks += region.nFreeBlocks;

    if (lpnBegin == 0 && lpnEnd >= region.totalLogicalPages) {
      status.mappedLogicalPages += region.table.size();
    }
    else {
      for (uint64_t lpn = lpnBegin; lpn < lpnEnd; lpn++) {
        if (region.table.count(lpn) > 0) {
          status.mappedLogicalPages++;
        }
      }
    }
  }

  return &status;
}

PageMapping::RegionState &PageMapping::getRegion(Tier tier) {
  return regions[tier == Tier::SLC ? 0 : 1];
}

const PageMapping::RegionState &PageMapping::getRegion(Tier tier) const {
  return regions[tier == Tier::SLC ? 0 : 1];
}

bool PageMapping::isInRegion(const RegionState &region,
                             uint32_t blockIndex) const {
  return blockIndex >= region.blockBegin && blockIndex < region.blockEnd;
}

bool PageMapping::checkMappingAddress(RegionState &region,
                                      std::pair<uint32_t, uint32_t> &mapping,
                                      const char *message) {
  if (mapping.first == param.totalPhysicalBlocks &&
      mapping.second == param.pagesInBlock) {
    return false;
  }

  if (mapping.second >= param.pagesInBlock) {
    panic(message);
  }

  if (!isInRegion(region, mapping.first)) {
    panic(message);
  }

  return true;
}

bool PageMapping::isValidMapping(RegionState &region, uint64_t lpn,
                                 uint32_t idx) {
  auto mappingList = region.table.find(lpn);

  if (idx >= bitsetSize || mappingList == region.table.end()) {
    return false;
  }

  auto &mapping = mappingList->second.at(idx);

  return isInRegion(region, mapping.first) &&
         mapping.second < param.pagesInBlock;
}

float PageMapping::freeBlockRatio(RegionState &region) {
  return (float)region.nFreeBlocks / region.totalPhysicalBlocks;
}

uint32_t PageMapping::convertBlockIdx(uint32_t blockIdx) {
  return blockIdx % param.pageCountToMaxPerf;
}

uint32_t PageMapping::getFreeBlock(RegionState &region, uint32_t idx) {
  uint32_t blockIndex = 0;

  if (idx >= param.pageCountToMaxPerf) {
    panic("Index out of range");
  }

  if (region.nFreeBlocks > 0) {
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
  }
  else {
    panic("No free block left");
  }

  return blockIndex;
}

uint32_t PageMapping::getLastFreeBlock(RegionState &region, Bitset &iomap) {
  if (!bRandomTweak || (region.lastFreeBlockIOMap & iomap).any()) {
    // Update lastFreeBlockIndex
    region.lastFreeBlockIndex++;

    if (region.lastFreeBlockIndex == param.pageCountToMaxPerf) {
      region.lastFreeBlockIndex = 0;
    }

    region.lastFreeBlockIOMap = iomap;
  }
  else {
    region.lastFreeBlockIOMap |= iomap;
  }

  auto freeBlock =
      region.blocks.find(region.lastFreeBlock.at(region.lastFreeBlockIndex));

  // Sanity check
  if (freeBlock == region.blocks.end()) {
    panic("Corrupted");
  }

  // If current free block is full, get next block
  if (freeBlock->second.getNextWritePageIndex() == param.pagesInBlock) {
    region.lastFreeBlock.at(region.lastFreeBlockIndex) =
        getFreeBlock(region, region.lastFreeBlockIndex);

    region.bReclaimMore = true;
  }

  return region.lastFreeBlock.at(region.lastFreeBlockIndex);
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
  PAL::Request req(param.ioUnitInPage);
  std::vector<PAL::Request> readRequests;
  std::vector<PAL::Request> writeRequests;
  std::vector<PAL::Request> eraseRequests;
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

        // Retrive free block
        auto freeBlock = region.blocks.find(getLastFreeBlock(region, bit));

        if (freeBlock == region.blocks.end() ||
            !isInRegion(region, freeBlock->first)) {
          panic("ftl: GC destination block is outside selected region");
        }

        // Issue Read
        req.blockIndex = block->first;
        req.pageIndex = pageIndex;
        req.ioFlag = bit;

        readRequests.push_back(req);

        // Update mapping table
        uint32_t newBlockIdx = freeBlock->first;

        for (uint32_t idx = 0; idx < bitsetSize; idx++) {
          if (bit.test(idx)) {
            // Invalidate
            block->second.invalidate(pageIndex, idx);

            auto mappingList = region.table.find(lpns.at(idx));

            if (mappingList == region.table.end()) {
              panic("Invalid mapping table entry");
            }

            pDRAM->read(&(*mappingList), 8 * param.ioUnitInPage, tick);

            auto &mapping = mappingList->second.at(idx);

            uint32_t newPageIdx = freeBlock->second.getNextWritePageIndex(idx);

            mapping.first = newBlockIdx;
            mapping.second = newPageIdx;

            freeBlock->second.write(newPageIdx, lpns.at(idx), idx, beginAt);

            // Issue Write
            req.blockIndex = newBlockIdx;
            req.pageIndex = newPageIdx;

            if (bRandomTweak) {
              req.ioFlag.reset();
              req.ioFlag.set(idx);
            }
            else {
              req.ioFlag.set();
            }

            writeRequests.push_back(req);

            region.stat.validPageCopies++;
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

  for (auto &iter : writeRequests) {
    beginAt = readFinishedAt;

    pPAL->write(iter, beginAt);

    writeFinishedAt = MAX(writeFinishedAt, beginAt);
  }

  for (auto &iter : eraseRequests) {
    beginAt = readFinishedAt;

    eraseInternal(iter, region, beginAt);

    eraseFinishedAt = MAX(eraseFinishedAt, beginAt);
  }

  tick = MAX(writeFinishedAt, eraseFinishedAt);
  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::DO_GARBAGE_COLLECTION);
}

void PageMapping::readInternal(Request &req, RegionState &region,
                               uint64_t &tick) {
  PAL::Request palRequest(req);
  uint64_t beginAt;
  uint64_t finishedAt = tick;

  auto mappingList = region.table.find(req.lpn);

  if (mappingList != region.table.end()) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList), 8 * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), 8, tick);
    }

    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      if (req.ioFlag.test(idx) || !bRandomTweak) {
        auto &mapping = mappingList->second.at(idx);

        if (checkMappingAddress(
                region, mapping,
                "ftl: read mapping points outside selected region")) {
          palRequest.blockIndex = mapping.first;
          palRequest.pageIndex = mapping.second;

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

          block->second.read(palRequest.pageIndex, idx, beginAt);
          pPAL->read(palRequest, beginAt);

          finishedAt = MAX(finishedAt, beginAt);
        }
      }
    }

    tick = finishedAt;
    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::READ_INTERNAL);
  }
}

void PageMapping::writeInternal(Request &req, RegionState &region,
                                uint64_t &tick, bool sendToPAL) {
  PAL::Request palRequest(req);
  std::unordered_map<uint32_t, Block>::iterator block;
  auto mappingList = region.table.find(req.lpn);
  uint64_t beginAt;
  uint64_t finishedAt = tick;
  bool readBeforeWrite = false;

  if (mappingList != region.table.end()) {
    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      if (req.ioFlag.test(idx) || !bRandomTweak) {
        auto &mapping = mappingList->second.at(idx);

        if (checkMappingAddress(
                region, mapping,
                "ftl: overwrite mapping points outside selected region")) {
          block = region.blocks.find(mapping.first);

          if (block == region.blocks.end()) {
            panic("Block is not in use");
          }

          // Invalidate current page
          block->second.invalidate(mapping.second, idx);
        }
      }
    }
  }
  else {
    // Create empty mapping
    auto ret = region.table.emplace(
        req.lpn,
        std::vector<std::pair<uint32_t, uint32_t>>(
            bitsetSize, {param.totalPhysicalBlocks, param.pagesInBlock}));

    if (!ret.second) {
      panic("Failed to insert new mapping");
    }

    mappingList = ret.first;
  }

  // Write data to free block
  block = region.blocks.find(getLastFreeBlock(region, req.ioFlag));

  if (block == region.blocks.end()) {
    panic("No such block");
  }

  if (sendToPAL) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList), 8 * req.ioFlag.count(), tick);
      pDRAM->write(&(*mappingList), 8 * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), 8, tick);
      pDRAM->write(&(*mappingList), 8, tick);
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

      beginAt = tick;

      block->second.write(pageIndex, req.lpn, idx, beginAt);

      // Read old data if needed (Only executed when bRandomTweak = false)
      // Maybe some other init procedures want to perform 'partial-write'
      // So check sendToPAL variable
      if (readBeforeWrite && sendToPAL) {
        palRequest.blockIndex = mapping.first;
        palRequest.pageIndex = mapping.second;

        // We don't need to read old data
        palRequest.ioFlag = req.ioFlag;
        palRequest.ioFlag.flip();

        pPAL->read(palRequest, beginAt);
      }

      // update mapping to table
      mapping.first = block->first;
      mapping.second = pageIndex;

      if (sendToPAL) {
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
}

void PageMapping::trimInternal(Request &req, RegionState &region,
                               uint64_t &tick) {
  auto mappingList = region.table.find(req.lpn);

  if (mappingList != region.table.end()) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList), 8 * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), 8, tick);
    }

    // Do trim
    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      if (!req.ioFlag.test(idx) && bRandomTweak) {
        continue;
      }

      auto &mapping = mappingList->second.at(idx);

      if (!checkMappingAddress(region, mapping,
                               "ftl: trim mapping points outside selected region")) {
        continue;
      }

      auto block = region.blocks.find(mapping.first);

      if (block == region.blocks.end()) {
        panic("Block is not in use");
      }

      block->second.invalidate(mapping.second, idx);
      mapping = {param.totalPhysicalBlocks, param.pagesInBlock};
    }

    bool mappingEmpty = true;

    for (auto &mapping : mappingList->second) {
      if (checkMappingAddress(region, mapping,
                              "ftl: trim mapping points outside selected region")) {
        mappingEmpty = false;

        break;
      }
    }

    if (mappingEmpty) {
      region.table.erase(mappingList);
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
  uint64_t numOfBlocks = region.totalLogicalBlocks;
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
}

void PageMapping::getStatValues(std::vector<double> &values) {
  for (auto &region : regions) {
    values.push_back(region.stat.gcCount);
    values.push_back(region.stat.reclaimedBlocks);
    values.push_back(region.stat.validSuperPageCopies);
    values.push_back(region.stat.validPageCopies);
    values.push_back(calculateWearLeveling(region));
  }
}

void PageMapping::resetStatValues() {
  for (auto &region : regions) {
    memset(&region.stat, 0, sizeof(region.stat));
  }
}

}  // namespace FTL

}  // namespace SimpleSSD

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

#include "ftl/ftl.hh"

#include "ftl/page_mapping.hh"

namespace SimpleSSD {

namespace FTL {

FTL::FTL(ConfigReader &c, DRAM::AbstractDRAM *d) : conf(c), pDRAM(d) {
  PAL::Parameter *palparam;

  pPAL = new PAL::PAL(conf);
  palparam = pPAL->getInfo();

  uint64_t blocksPerPlane = conf.readUint(CONFIG_PAL, PAL::NAND_BLOCK);
  uint64_t slcBlocksPerPlane =
      conf.readUint(CONFIG_PAL, PAL::NAND_SLC_BLOCK);
  uint64_t tlcBlocksPerPlane =
      conf.readUint(CONFIG_PAL, PAL::NAND_TLC_BLOCK);
  float overProvision = conf.readFloat(CONFIG_FTL, FTL_OVERPROVISION_RATIO);

  param.totalPhysicalBlocks = palparam->superBlock;
  param.totalPhysicalBlocksByTier[0] =
      palparam->superBlock * slcBlocksPerPlane / blocksPerPlane;
  param.totalPhysicalBlocksByTier[1] =
      palparam->superBlock * tlcBlocksPerPlane / blocksPerPlane;
  param.placementLogicalBlocksByTier[0] =
      param.totalPhysicalBlocksByTier[0] * (1 - overProvision);
  param.placementLogicalBlocksByTier[1] =
      param.totalPhysicalBlocksByTier[1] * (1 - overProvision);
  param.pagesInBlock = palparam->page;
  param.pageSize = palparam->superPageSize;
  param.ioUnitInPage = palparam->pageInSuperPage;
  param.mappingEntriesPerPage =
      conf.readBoolean(CONFIG_FTL, FTL_USE_RANDOM_IO_TWEAK)
          ? param.ioUnitInPage
          : 1;
  param.placementLogicalPagesByTier[0] =
      param.placementLogicalBlocksByTier[0] * param.pagesInBlock;
  param.placementLogicalPagesByTier[1] =
      param.placementLogicalBlocksByTier[1] * param.pagesInBlock;
  param.globalLogicalBlocks = param.placementLogicalBlocksByTier[0] +
                              param.placementLogicalBlocksByTier[1];
  param.globalLogicalPages = param.globalLogicalBlocks * param.pagesInBlock;
  param.globalLogicalUnits =
      param.globalLogicalPages * param.mappingEntriesPerPage;
  param.pageCountToMaxPerf = palparam->superBlock / palparam->block;

  switch (conf.readInt(CONFIG_FTL, FTL_MAPPING_MODE)) {
    case PAGE_MAPPING:
      pFTL = new PageMapping(conf, param, pPAL, pDRAM);
      break;
  }

  if (param.totalPhysicalBlocks <=
      param.globalLogicalBlocks + param.pageCountToMaxPerf) {
    panic("FTL Over-Provision Ratio is too small");
  }

  // Print mapping Information
  debugprint(LOG_FTL, "Total physical blocks %" PRIu64,
             param.totalPhysicalBlocks);
  debugprint(LOG_FTL, "Global logical blocks/pages/units %" PRIu64 "/%" PRIu64
                      "/%" PRIu64,
             param.globalLogicalBlocks, param.globalLogicalPages,
             param.globalLogicalUnits);
  debugprint(LOG_FTL, "SLC physical/logical blocks %" PRIu64 "/%" PRIu64,
             param.totalPhysicalBlocksByTier[0],
             param.placementLogicalBlocksByTier[0]);
  debugprint(LOG_FTL, "TLC physical/logical blocks %" PRIu64 "/%" PRIu64,
             param.totalPhysicalBlocksByTier[1],
             param.placementLogicalBlocksByTier[1]);
  debugprint(LOG_FTL, "Logical page size %u", param.pageSize);

  // Initialize pFTL
  pFTL->initialize();
}

FTL::~FTL() {
  delete pPAL;
  delete pFTL;
}

void FTL::read(Request &req, uint64_t &tick) {
  debugprint(LOG_FTL, "READ  | LPN %" PRIu64, req.lpn);

  pFTL->read(req, tick);

  tick += applyLatency(CPU::FTL, CPU::READ);
}

bool FTL::write(Request &req, uint64_t &tick) {
  debugprint(LOG_FTL, "WRITE | LPN %" PRIu64, req.lpn);

  bool success = pFTL->write(req, tick);

  tick += applyLatency(CPU::FTL, CPU::WRITE);

  return success;
}

void FTL::trim(Request &req, uint64_t &tick) {
  debugprint(LOG_FTL, "TRIM  | LPN %" PRIu64, req.lpn);

  pFTL->trim(req, tick);

  tick += applyLatency(CPU::FTL, CPU::TRIM);
}

void FTL::migrate(MigrationRequest &req, uint64_t &tick) {
  debugprint(LOG_FTL,
             "MIGRAT| LCA %" PRIu64 " + %" PRIu64 " | TARGET %u",
             req.startLCA, req.count, (uint8_t)req.targetTier);

  pFTL->migrate(req, tick);

  tick += applyLatency(CPU::FTL, CPU::WRITE);
}

void FTL::drainMigrations(MigrationRequest &req, uint64_t &tick) {
  pFTL->drainMigrations(req, tick);

  tick += applyLatency(CPU::FTL, CPU::WRITE);
}

bool FTL::migrationDrainRequired() const {
  return pFTL->migrationDrainRequired();
}

std::vector<LCA> FTL::getPendingMigrationLCAs() const {
  return pFTL->getPendingMigrationLCAs();
}

bool FTL::reserveCacheWrite(Tier tier, uint64_t units) {
  return pFTL->reserveCacheWrite(tier, units);
}

bool FTL::admitCacheWrite(LCA lca, Tier tier, bool hasOldReservation,
                          Tier oldTier) {
  return pFTL->admitCacheWrite(lca, tier, hasOldReservation, oldTier);
}

void FTL::releaseCacheWriteReservation(Tier tier, uint64_t units) {
  pFTL->releaseCacheWriteReservation(tier, units);
}

bool FTL::reserveMigration(Tier tier, uint64_t units) {
  return pFTL->reserveMigration(tier, units);
}

void FTL::releaseMigrationReservation(Tier tier, uint64_t units) {
  pFTL->releaseMigrationReservation(tier, units);
}

void FTL::format(LPNRange &range, uint64_t &tick) {
  pFTL->format(range, tick);

  tick += applyLatency(CPU::FTL, CPU::FORMAT);
}

bool FTL::getTierSpaceInfo(Tier tier, TierSpaceInfo &info) const {
  return pFTL->getTierSpaceInfo(tier, info);
}

Parameter *FTL::getInfo() {
  return &param;
}

void FTL::getTierLPNInfo(Tier tier, uint64_t &totalLogicalPages,
                         uint32_t &logicalPageSize) {
  if (!isValidTier(tier)) {
    panic("FTL tier placement-capacity query has invalid tier");
  }

  totalLogicalPages = param.placementLogicalPagesByTier[tierIndex(tier)];
  logicalPageSize = param.pageSize;
}

uint64_t FTL::getUsedPageCount(uint64_t lpnBegin, uint64_t lpnEnd) {
  return pFTL->getStatus(lpnBegin, lpnEnd)->mappedLogicalPages;
}

void FTL::getStatList(std::vector<Stats> &list, std::string prefix) {
  pFTL->getStatList(list, prefix + "ftl.");
  pPAL->getStatList(list, prefix);
}

void FTL::getStatValues(std::vector<double> &values) {
  pFTL->getStatValues(values);
  pPAL->getStatValues(values);
}

void FTL::resetStatValues() {
  pFTL->resetStatValues();
  pPAL->resetStatValues();
}

}  // namespace FTL

}  // namespace SimpleSSD

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
  param.totalLogicalBlocksByTier[0] =
      param.totalPhysicalBlocksByTier[0] * (1 - overProvision);
  param.totalLogicalBlocksByTier[1] =
      param.totalPhysicalBlocksByTier[1] * (1 - overProvision);
  param.totalLogicalBlocks = param.totalLogicalBlocksByTier[0] +
                             param.totalLogicalBlocksByTier[1];
  param.pagesInBlock = palparam->page;
  param.pageSize = palparam->superPageSize;
  param.ioUnitInPage = palparam->pageInSuperPage;
  param.pageCountToMaxPerf = palparam->superBlock / palparam->block;

  switch (conf.readInt(CONFIG_FTL, FTL_MAPPING_MODE)) {
    case PAGE_MAPPING:
      pFTL = new PageMapping(conf, param, pPAL, pDRAM);
      break;
  }

  if (param.totalPhysicalBlocks <=
      param.totalLogicalBlocks + param.pageCountToMaxPerf) {
    panic("FTL Over-Provision Ratio is too small");
  }

  // Print mapping Information
  debugprint(LOG_FTL, "Total physical blocks %u", param.totalPhysicalBlocks);
  debugprint(LOG_FTL, "Total logical blocks %u", param.totalLogicalBlocks);
  debugprint(LOG_FTL, "SLC physical/logical blocks %" PRIu64 "/%" PRIu64,
             param.totalPhysicalBlocksByTier[0],
             param.totalLogicalBlocksByTier[0]);
  debugprint(LOG_FTL, "TLC physical/logical blocks %" PRIu64 "/%" PRIu64,
             param.totalPhysicalBlocksByTier[1],
             param.totalLogicalBlocksByTier[1]);
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

void FTL::write(Request &req, uint64_t &tick) {
  debugprint(LOG_FTL, "WRITE | LPN %" PRIu64, req.lpn);

  pFTL->write(req, tick);

  tick += applyLatency(CPU::FTL, CPU::WRITE);
}

void FTL::trim(Request &req, uint64_t &tick) {
  debugprint(LOG_FTL, "TRIM  | LPN %" PRIu64, req.lpn);

  pFTL->trim(req, tick);

  tick += applyLatency(CPU::FTL, CPU::TRIM);
}

void FTL::migrate(MigrationRequest &req, uint64_t &tick) {
  debugprint(LOG_FTL,
             "MIGRAT| SRC %u:%" PRIu64 " | DST %u:%" PRIu64
             " | NLP %" PRIu64,
             (uint8_t)req.srcTier, req.srcLPN, (uint8_t)req.dstTier,
             req.dstLPN, req.nlp);

  req.success = pFTL->migrate(req, tick);

  tick += applyLatency(CPU::FTL, CPU::WRITE);
}

void FTL::format(LPNRange &range, uint64_t &tick) {
  pFTL->format(range, tick);

  tick += applyLatency(CPU::FTL, CPU::FORMAT);
}

Parameter *FTL::getInfo() {
  return &param;
}

void FTL::getTierLPNInfo(Tier tier, uint64_t &totalLogicalPages,
                         uint32_t &logicalPageSize) {
  uint32_t idx = tier == Tier::SLC ? 0 : 1;

  totalLogicalPages = param.totalLogicalBlocksByTier[idx] * param.pagesInBlock;
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

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

#ifndef __FTL_FTL__
#define __FTL_FTL__

#include "dram/abstract_dram.hh"
#include "pal/pal.hh"
#include "util/simplessd.hh"

namespace SimpleSSD {

namespace FTL {

class AbstractFTL;

typedef struct {
  uint64_t totalPhysicalBlocks;  //!< (PAL::Parameter::superBlock)
  uint64_t totalPhysicalBlocksByTier[2];
  // Per-tier placement limits describe physical-region provisioning only.
  // They must not be used as host-visible logical address ranges.
  uint64_t placementLogicalBlocksByTier[2];
  uint64_t placementLogicalPagesByTier[2];
  uint64_t globalLogicalBlocks;
  uint64_t globalLogicalPages;
  uint64_t globalLogicalUnits;
  uint64_t pagesInBlock;  //!< (PAL::Parameter::page)
  uint32_t pageSize;      //!< Mapping unit (PAL::Parameter::superPageSize)
  uint32_t ioUnitInPage;  //!< # smallest I/O unit in one page
  uint32_t mappingEntriesPerPage;
  uint32_t pageCountToMaxPerf;  //!< # pages to fully utilize internal parallism
} Parameter;

class FTL : public StatObject {
 private:
  Parameter param;
  PAL::PAL *pPAL;

  ConfigReader &conf;
  AbstractFTL *pFTL;
  DRAM::AbstractDRAM *pDRAM;

 public:
  FTL(ConfigReader &, DRAM::AbstractDRAM *);
  ~FTL();

  void read(Request &, uint64_t &);
  bool write(Request &, uint64_t &);
  void trim(Request &, uint64_t &);
  void migrate(MigrationRequest &, uint64_t &);
  void drainMigrations(MigrationRequest &, uint64_t &);
  bool migrationDrainRequired() const;
  std::vector<LCA> getPendingMigrationLCAs() const;

  bool reserveCacheWrite(Tier, uint64_t);
  bool admitCacheWrite(LCA, Tier, bool, Tier);
  void releaseCacheWriteReservation(Tier, uint64_t);
  bool reserveMigration(Tier, uint64_t);
  void releaseMigrationReservation(Tier, uint64_t);

  void format(LPNRange &, uint64_t &);
  bool getTierSpaceInfo(Tier, TierSpaceInfo &) const;

  Parameter *getInfo();
  void getTierLPNInfo(Tier, uint64_t &, uint32_t &);
  uint64_t getUsedPageCount(uint64_t, uint64_t);

  void getStatList(std::vector<Stats> &, std::string) override;
  void getStatValues(std::vector<double> &) override;
  void resetStatValues() override;
};

}  // namespace FTL

}  // namespace SimpleSSD

#endif

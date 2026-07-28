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

#ifndef __FTL_PAGE_MAPPING__
#define __FTL_PAGE_MAPPING__

#include <array>
#include <cinttypes>
#include <list>
#include <unordered_map>
#include <vector>

#include "ftl/abstract_ftl.hh"
#include "ftl/common/block.hh"
#include "ftl/ftl.hh"
#include "pal/pal.hh"

namespace SimpleSSD {

namespace FTL {

class PageMapping : public AbstractFTL {
 private:
  PAL::PAL *pPAL;

  ConfigReader &conf;

  bool bRandomTweak;
  uint32_t bitsetSize;

  typedef struct _RegionStat {
    uint64_t gcCount;
    uint64_t reclaimedBlocks;
    uint64_t validSuperPageCopies;
    uint64_t validPageCopies;
  } RegionStat;

  typedef struct _PendingMigration {
    Tier targetTier;
    uint64_t enqueueTick;
    std::list<LCA>::iterator orderIterator;

    _PendingMigration(Tier tier, uint64_t tick,
                      std::list<LCA>::iterator iter)
        : targetTier(tier), enqueueTick(tick), orderIterator(iter) {}
  } PendingMigration;

  enum class CancellationReason : uint8_t {
    None = 0,
    Write,
    Trim,
    Format,
  };

  typedef struct _MigrationStat {
    uint64_t maximumQueueEntries;
    uint64_t executedByGarbageCollection;
    uint64_t executedByListFull;
    uint64_t cancelledByWrite;
    uint64_t cancelledByTrim;
    uint64_t cancelledByFormat;
  } MigrationStat;

  typedef struct _RegionState {
    Tier tier;
    uint32_t blockBegin;
    uint32_t blockEnd;
    uint64_t totalPhysicalBlocks;
    uint64_t placementLogicalBlocks;
    uint64_t placementLogicalPages;

    std::unordered_map<uint32_t, Block> blocks;
    std::list<Block> freeBlocks;
    uint32_t nFreeBlocks;
    std::vector<uint32_t> lastFreeBlock;
    Bitset lastFreeBlockIOMap;
    uint32_t lastFreeBlockIndex;

    bool bReclaimMore;
    Status status;
    RegionStat stat;

    _RegionState(uint32_t pageCountToMaxPerf, uint32_t ioUnitInPage);
  } RegionState;

  std::array<RegionState, 2> regions;
  std::unordered_map<LPN, std::vector<MappingEntry>> table;
  std::array<uint64_t, 2> pendingCacheWriteUnitsByTier;
  std::array<uint64_t, 2> pendingMigrationUnitsByTier;
  uint64_t migrationListLimit;
  std::list<LCA> pendingMigrationOrder;
  std::unordered_map<LCA, PendingMigration> pendingMigrations;
  MigrationStat migrationStat;

  RegionState &getRegion(Tier);
  const RegionState &getRegion(Tier) const;
  bool isInRegion(const RegionState &, uint32_t) const;
  float freeBlockRatio(RegionState &);
  uint32_t convertBlockIdx(uint32_t);
  bool getFreeBlock(RegionState &, uint32_t, uint32_t &);
  bool getLastFreeBlock(RegionState &, Bitset &, uint32_t &);
  void calculateVictimWeight(std::vector<std::pair<uint32_t, float>> &,
                             RegionState &, const EVICT_POLICY, uint64_t);
  void selectVictimBlock(std::vector<uint32_t> &, RegionState &, uint64_t &);
  void doGarbageCollection(std::vector<uint32_t> &, RegionState &, uint64_t &);

  float calculateWearLeveling(RegionState &);
  void calculateTotalPages(RegionState &, uint64_t &, uint64_t &);

  void readInternal(Request &, uint64_t &);
  bool writeInternal(Request &, RegionState &, uint64_t &, bool = true);
  void trimInternal(Request &, uint64_t &);
  void eraseInternal(PAL::Request &, RegionState &, uint64_t &);
  bool checkMappingAddress(const MappingEntry &, const char *) const;
  void commitMappingReplacement(MappingEntry &, const MappingEntry &,
                                uint32_t);
  uint64_t countWritableUnitsWithoutGC(const RegionState &) const;
  uint64_t countWritableUnits(const RegionState &, bool) const;
  bool canAllocate(const RegionState &, uint64_t, ReservationOwner,
                   bool) const;
  bool canAllocateAfterMigrationCancellation(
      const RegionState &, uint64_t, ReservationOwner,
      const std::array<uint64_t, 2> &, bool) const;
  void consumeReservation(Tier, uint64_t, ReservationOwner);
  bool calculateFinalReservations(
      const std::array<uint64_t, 2> &, const std::array<uint64_t, 2> &,
      const std::array<uint64_t, 2> &, const std::array<uint64_t, 2> &,
      std::array<uint64_t, 2> &, std::array<uint64_t, 2> &) const;
  void removePendingMigration(
      std::unordered_map<LCA, PendingMigration>::iterator, bool,
      CancellationReason = CancellationReason::None);
  void cancelPendingMigration(LCA, CancellationReason);
  MigrationStatus enqueueMigration(MigrationRequest &, uint64_t);
  MigrationStatus executePendingMigration(
      LCA, MigrationTrigger, uint64_t &, const MappingEntry * = nullptr,
      bool = false);
  MigrationStatus drainPendingMigrations(uint64_t &);

 public:
  PageMapping(ConfigReader &, Parameter &, PAL::PAL *, DRAM::AbstractDRAM *);
  ~PageMapping();

  bool initialize() override;

  void read(Request &, uint64_t &) override;
  bool write(Request &, uint64_t &) override;
  void trim(Request &, uint64_t &) override;
  void migrate(MigrationRequest &, uint64_t &) override;
  void drainMigrations(MigrationRequest &, uint64_t &) override;
  bool migrationDrainRequired() const override;
  std::vector<LCA> getPendingMigrationLCAs() const override;

  bool reserveCacheWrite(Tier, uint64_t) override;
  bool admitCacheWrite(LCA, Tier, bool, Tier) override;
  void releaseCacheWriteReservation(Tier, uint64_t) override;
  bool reserveMigration(Tier, uint64_t) override;
  void releaseMigrationReservation(Tier, uint64_t) override;

  void format(LPNRange &, uint64_t &) override;

  bool getTierSpaceInfo(Tier, TierSpaceInfo &) const override;

  Status *getStatus(uint64_t, uint64_t) override;

  void getStatList(std::vector<Stats> &, std::string) override;
  void getStatValues(std::vector<double> &) override;
  void resetStatValues() override;
};

}  // namespace FTL

}  // namespace SimpleSSD

#endif

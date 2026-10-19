#include "ftl/subpage_mapping.hh"

namespace SimpleSSD {
namespace FTL {

SubpageMapping::SubpageMapping(ConfigReader &conf, Parameter &logical,
                             Parameter &physical, PAL::PAL *pal,
                             DRAM::AbstractDRAM *dram)
    : AbstractFTL(logical, pal, dram), physicalParam(physical),
      coalescing(conf.readBoolean(CONFIG_FTL, FTL_MIGRATION_COALESCING)),
      backing(conf, physical, pal, dram) {
  backing.setGCCopyObserver([this](Tier tier, uint64_t page) {
    uint32_t rid = tier == Tier::SLC ? 0 : 1;
    uint8_t bits = mask(tier, page);
    for (uint32_t i = 0; i < subpages; ++i) {
      if (bits & (1u << i)) gcLogical[rid] += logicalBytes;
    }
    gcPhysical[rid] += physicalBytes;
  });
}

bool SubpageMapping::initialize() {
  backing.initialize();
  // Legacy warm-up fills complete physical pages. Two tiers must not own
  // the same logical subpage at initialization.
  uint64_t common = std::min(capacity(Tier::SLC), capacity(Tier::TLC)) / subpages;
  for (uint64_t page = 0; page < common; ++page) {
    if (backing.hasMapping(Tier::SLC, page) && backing.hasMapping(Tier::TLC, page)) {
      panic("4 KiB mapping requires non-overlapping initial tier placement");
    }
  }
  return true;
}

uint64_t SubpageMapping::capacity(Tier tier) const {
  return param.totalLogicalBlocksByTier[tier == Tier::SLC ? 0 : 1] *
         param.pagesInBlock * param.ioUnitInPage;
}

uint8_t SubpageMapping::mask(Tier tier, uint64_t page) {
  auto &table = masks[tier == Tier::SLC ? 0 : 1];
  auto found = table.find(page);
  if (found != table.end()) return found->second;
  return backing.hasMapping(tier, page) ? 0xF : 0;
}

void SubpageMapping::setMask(Tier tier, uint64_t page, uint8_t value) {
  auto &table = masks[tier == Tier::SLC ? 0 : 1];
  // Explicit zero/full overrides are required while a migration is buffered:
  // the logical ownership may differ from the still-retained backing page.
  table[page] = value;
}

uint8_t SubpageMapping::pendingMask(Tier tier, uint64_t page) {
  auto &table = pending[tier == Tier::SLC ? 0 : 1];
  auto found = table.find(page);
  if (found == table.end()) return 0;
  uint8_t bits = 0;
  for (const auto &move : found->second) bits |= move.dstBit;
  return bits;
}

void SubpageMapping::releaseSource(const PendingMove &move, uint64_t &tick) {
  uint32_t rid = move.srcTier == Tier::SLC ? 0 : 1;
  auto found = pendingSource[rid].find(move.srcPage);
  if (found != pendingSource[rid].end()) {
    found->second &= ~move.srcBit;
    if (!found->second) pendingSource[rid].erase(found);
  }
  if (!mask(move.srcTier, move.srcPage) &&
      pendingSource[rid].count(move.srcPage) == 0 &&
      backing.hasMapping(move.srcTier, move.srcPage)) {
    auto physical = request(move.srcTier, move.srcPage);
    backing.trim(physical, tick);
  }
}

void SubpageMapping::materialize(Tier tier, uint64_t page, uint64_t &tick,
                                 bool forced) {
  uint32_t rid = tier == Tier::SLC ? 0 : 1;
  auto found = pending[rid].find(page);
  if (found == pending[rid].end()) return;

  std::set<std::pair<Tier, uint64_t>> sourcePages;
  for (const auto &move : found->second) {
    sourcePages.insert({move.srcTier, move.srcPage});
  }
  for (const auto &source : sourcePages) {
    auto physical = request(source.first, source.second);
    backing.read(physical, tick);
    stat.sourceRead += physicalBytes;
  }

  auto destination = request(tier, page);
  if (backing.hasMapping(tier, page)) {
    backing.read(destination, tick);
    stat.destinationRead += physicalBytes;
  }
  backing.write(destination, tick);
  stat.program += physicalBytes;
  stat.pagePrograms++;
  if (mask(tier, page) == 0xF) stat.fullPagePrograms++;
  if (forced) stat.forcedPagePrograms++;
  migrationPhysical[rid] += physicalBytes;

  std::vector<PendingMove> moves = found->second;
  pending[rid].erase(found);
  for (const auto &move : moves) releaseSource(move, tick);
}

void SubpageMapping::materializeRelated(Tier tier, uint64_t page, uint8_t bits,
                                        uint64_t &tick) {
  uint32_t rid = tier == Tier::SLC ? 0 : 1;
  if (pending[rid].count(page)) materialize(tier, page, tick, true);
  materializeSourceRelated(tier, page, bits, tick);
}

void SubpageMapping::materializeSourceRelated(Tier tier, uint64_t page,
                                              uint8_t bits, uint64_t &tick) {
  std::set<std::pair<Tier, uint64_t>> affected;
  for (uint32_t dst = 0; dst < 2; ++dst) {
    Tier dstTier = dst == 0 ? Tier::SLC : Tier::TLC;
    for (const auto &group : pending[dst]) {
      for (const auto &move : group.second) {
        if (move.srcTier == tier && move.srcPage == page &&
            (move.srcBit & bits)) {
          affected.insert({dstTier, group.first});
        }
      }
    }
  }
  for (const auto &group : affected) {
    materialize(group.first, group.second, tick, true);
  }
}

bool SubpageMapping::combinePendingWithWrite(Tier tier, uint64_t page,
                                             uint8_t writeBits,
                                             uint64_t &tick) {
  uint32_t rid = tier == Tier::SLC ? 0 : 1;
  auto found = pending[rid].find(page);
  if (found == pending[rid].end()) return false;

  std::set<std::pair<Tier, uint64_t>> sourcePages;
  for (const auto &move : found->second) {
    if (!(move.dstBit & writeBits)) {
      sourcePages.insert({move.srcTier, move.srcPage});
    }
  }
  for (const auto &source : sourcePages) {
    auto physical = request(source.first, source.second);
    backing.read(physical, tick);
    stat.sourceRead += physicalBytes;
  }

  auto destination = request(tier, page);
  if (backing.hasMapping(tier, page)) {
    backing.read(destination, tick);
    stat.destinationRead += physicalBytes;
  }
  setMask(tier, page, mask(tier, page) | writeBits);
  backing.write(destination, tick);
  stat.program += physicalBytes;
  stat.pagePrograms++;
  stat.forcedPagePrograms++;
  combinedPhysical[rid] += physicalBytes;

  std::vector<PendingMove> moves = found->second;
  pending[rid].erase(found);
  for (const auto &move : moves) releaseSource(move, tick);
  return true;
}

void SubpageMapping::materializeAll(uint64_t &tick) {
  std::vector<std::pair<Tier, uint64_t>> groups;
  for (uint32_t rid = 0; rid < 2; ++rid) {
    Tier tier = rid == 0 ? Tier::SLC : Tier::TLC;
    for (const auto &group : pending[rid]) groups.push_back({tier, group.first});
  }
  for (const auto &group : groups) {
    materialize(group.first, group.second, tick, true);
  }
}

Request SubpageMapping::request(Tier tier, uint64_t page) {
  Request req(physicalParam.ioUnitInPage);
  req.tier = tier;
  req.lpn = page / physicalParam.ioUnitInPage;
  req.ioFlag.reset();
  req.ioFlag.set(page % physicalParam.ioUnitInPage);
  return req;
}

std::map<uint64_t, uint8_t> SubpageMapping::groups(Request &req) {
  if (req.ioFlag.size() != param.ioUnitInPage ||
      req.lpn >= param.totalLogicalBlocksByTier[req.tier == Tier::SLC ? 0 : 1] *
                     param.pagesInBlock) {
    panic("4 KiB request outside selected tier or wrong bitmap size");
  }
  std::map<uint64_t, uint8_t> result;
  for (uint32_t i = 0; i < param.ioUnitInPage; ++i) {
    if (req.ioFlag.test(i)) {
      uint64_t lca = req.lpn * param.ioUnitInPage + i;
      result[lca / subpages] |= 1u << (lca % subpages);
    }
  }
  return result;
}

void SubpageMapping::read(Request &req, uint64_t &tick) {
  for (auto &part : groups(req)) {
    uint8_t valid = mask(req.tier, part.first) & part.second;
    if (!valid) continue;

    // Pending destination data can be served from the retained source page.
    // A host read therefore does not prematurely program a partial group.
    uint32_t rid = req.tier == Tier::SLC ? 0 : 1;
    uint8_t buffered = pendingMask(req.tier, part.first) & valid;
    if (buffered) {
      std::set<std::pair<Tier, uint64_t>> sourcePages;
      auto found = pending[rid].find(part.first);
      for (const auto &move : found->second) {
        if (move.dstBit & buffered) {
          sourcePages.insert({move.srcTier, move.srcPage});
        }
      }
      for (const auto &source : sourcePages) {
        auto physical = request(source.first, source.second);
        backing.read(physical, tick);
        stat.bufferedReadSource += physicalBytes;
      }
    }
    if (valid & ~buffered) {
      auto physical = request(req.tier, part.first);
      backing.read(physical, tick);
    }
  }
}

void SubpageMapping::remove(Tier tier, uint64_t page, uint8_t bits, uint64_t &tick) {
  uint8_t left = mask(tier, page) & ~bits;
  if (!left) {
    auto physical = request(tier, page);
    backing.trim(physical, tick);
  }
  setMask(tier, page, left);
}

void SubpageMapping::write(Request &req, uint64_t &tick) {
  for (auto &part : groups(req)) {
    // Resolve migrations that use the overwritten logical bits as a source.
    materializeSourceRelated(req.tier, part.first, part.second, tick);
    uint8_t previous = mask(req.tier, part.first);
    if (!combinePendingWithWrite(req.tier, part.first, part.second, tick)) {
      auto physical = request(req.tier, part.first);
      // Preserve other valid subpages in this tier by full-page copy-on-write.
      if (previous & ~part.second) backing.read(physical, tick);
      setMask(req.tier, part.first, previous | part.second);
      backing.write(physical, tick);
      writePhysical[req.tier == Tier::SLC ? 0 : 1] += physicalBytes;
    }
    Tier other = req.tier == Tier::SLC ? Tier::TLC : Tier::SLC;
    if (part.first < capacity(other) / subpages && (mask(other, part.first) & part.second)) {
      materializeRelated(other, part.first, part.second, tick);
      remove(other, part.first, part.second, tick);
    }
  }
}

void SubpageMapping::trim(Request &req, uint64_t &tick) {
  for (auto &part : groups(req)) {
    materializeRelated(req.tier, part.first, part.second, tick);
    remove(req.tier, part.first, part.second, tick);
  }
}

bool SubpageMapping::migrate(MigrationRequest &req, uint64_t &tick) {
  if (req.srcTier == req.dstTier || !req.nlp ||
      req.srcLPN >= capacity(req.srcTier) || req.dstLPN >= capacity(req.dstTier) ||
      req.nlp > capacity(req.srcTier) - req.srcLPN ||
      req.nlp > capacity(req.dstTier) - req.dstLPN) {
    ++stat.rejected;
    return false;
  }
  std::map<uint64_t, uint8_t> destination;
  // Logical preflight comes before dependency materialization so a duplicate
  // request cannot force an otherwise coalescible pending group to NAND.
  for (uint64_t i = 0; i < req.nlp; ++i) {
    uint64_t src = req.srcLPN + i, dst = req.dstLPN + i;
    uint8_t srcBit = 1u << (src % subpages), dstBit = 1u << (dst % subpages);
    if (!(mask(req.srcTier, src / subpages) & srcBit) ||
        (mask(req.dstTier, dst / subpages) & dstBit)) {
      ++stat.rejected;
      return false;
    }
    destination[dst / subpages] |= dstBit;
  }

  // A valid migration sourced from a buffered destination is a dependency
  // barrier; make that source durable before buffering its next move.
  for (uint64_t i = 0; i < req.nlp; ++i) {
    uint64_t src = req.srcLPN + i;
    materializeRelated(req.srcTier, src / subpages,
                       1u << (src % subpages), tick);
  }

  uint32_t srcRid = req.srcTier == Tier::SLC ? 0 : 1;
  uint32_t dstRid = req.dstTier == Tier::SLC ? 0 : 1;
  for (uint64_t i = 0; i < req.nlp; ++i) {
    uint64_t src = req.srcLPN + i, dst = req.dstLPN + i;
    uint64_t srcPage = src / subpages, dstPage = dst / subpages;
    uint8_t srcBit = 1u << (src % subpages), dstBit = 1u << (dst % subpages);
    setMask(req.srcTier, srcPage, mask(req.srcTier, srcPage) & ~srcBit);
    setMask(req.dstTier, dstPage, mask(req.dstTier, dstPage) | dstBit);
    pendingSource[srcRid][srcPage] |= srcBit;
    pending[dstRid][dstPage].push_back(
        PendingMove{req.srcTier, srcPage, srcBit, dstBit});
  }
  ++stat.commands;
  stat.logical += req.nlp * logicalBytes;
  stat.peakPendingGroups = std::max<uint64_t>(
      stat.peakPendingGroups, pending[0].size() + pending[1].size());
  for (const auto &part : destination) {
    if (!coalescing || mask(req.dstTier, part.first) == 0xF) {
      materialize(req.dstTier, part.first, tick, false);
    }
  }
  return true;
}

void SubpageMapping::flush(Tier, uint64_t &tick) {
  // A migration changes both tiers, so either namespace FLUSH drains all
  // acknowledged migration-buffer entries.
  materializeAll(tick);
}

void SubpageMapping::format(LPNRange &range, uint64_t &tick) {
  materializeAll(tick);
  // ICL format ranges already use superpage LPNs.
  backing.format(range, tick);
  auto &table = masks[range.tier == Tier::SLC ? 0 : 1];
  for (auto it = table.begin(); it != table.end();) {
    uint64_t lpn = it->first / physicalParam.ioUnitInPage;
    if (lpn >= range.slpn && lpn - range.slpn < range.nlp) it = table.erase(it);
    else ++it;
  }
}

Status *SubpageMapping::getStatus(uint64_t begin, uint64_t end) {
  return backing.getStatus(begin, end);
}

uint64_t SubpageMapping::validBytes(Tier tier) {
  uint64_t bytes = 0;
  for (uint64_t page = 0; page < capacity(tier) / subpages; ++page) {
    uint8_t bits = mask(tier, page);
    for (uint32_t i = 0; i < subpages; ++i) if (bits & (1u << i)) bytes += logicalBytes;
  }
  return bytes;
}

void SubpageMapping::getStatList(std::vector<Stats> &list, std::string prefix) {
  backing.getStatList(list, prefix);
  prefix += "subpage.";
  list.push_back({prefix + "migration.commands", "Successful migration commands"});
  list.push_back({prefix + "migration.rejected", "Rejected migration commands"});
  list.push_back({prefix + "migration.logical_bytes", "Logical bytes relocated"});
  list.push_back({prefix + "migration.source_read_bytes", "Physical source NAND read bytes"});
  list.push_back({prefix + "migration.buffered_read_source_bytes", "Host-read NAND bytes served from retained migration source pages"});
  list.push_back({prefix + "migration.destination_read_bytes", "Physical destination RMW NAND read bytes"});
  list.push_back({prefix + "migration.program_bytes", "Physical destination NAND program bytes consumed by buffered migration, including programs shared with normal writes"});
  list.push_back({prefix + "migration.page_programs", "Destination physical pages programmed from the migration buffer"});
  list.push_back({prefix + "migration.full_page_programs", "Migration-buffer programs triggered by four collected subpages"});
  list.push_back({prefix + "migration.forced_page_programs", "Migration-buffer programs forced by dependencies, destination writes, or FLUSH"});
  list.push_back({prefix + "migration.pending_groups", "Current buffered 16 KiB destination page groups"});
  list.push_back({prefix + "migration.peak_pending_groups", "Peak buffered 16 KiB destination page groups"});
  list.push_back({prefix + "slc.valid_bytes", "Logically valid SLC bytes"});
  list.push_back({prefix + "tlc.valid_bytes", "Logically valid TLC bytes"});
  for (const auto *tier : {"slc", "tlc"}) {
    list.push_back({prefix + tier + ".write.program_bytes", "Physical NAND program bytes caused by normal writes"});
    list.push_back({prefix + tier + ".migration.program_bytes", "Physical destination NAND program bytes caused by migration"});
    list.push_back({prefix + tier + ".combined_migration_write.program_bytes", "Physical NAND program bytes shared by pending migration and a normal write"});
    list.push_back({prefix + tier + ".gc.logical_copy_bytes", "Valid logical bytes copied by GC"});
    list.push_back({prefix + tier + ".gc.physical_copy_bytes", "NAND bytes per GC read/program leg"});
  }
}

void SubpageMapping::getStatValues(std::vector<double> &values) {
  backing.getStatValues(values);
  values.push_back(stat.commands);
  values.push_back(stat.rejected);
  values.push_back(stat.logical);
  values.push_back(stat.sourceRead);
  values.push_back(stat.bufferedReadSource);
  values.push_back(stat.destinationRead);
  values.push_back(stat.program);
  values.push_back(stat.pagePrograms);
  values.push_back(stat.fullPagePrograms);
  values.push_back(stat.forcedPagePrograms);
  values.push_back(pending[0].size() + pending[1].size());
  values.push_back(stat.peakPendingGroups);
  values.push_back(validBytes(Tier::SLC));
  values.push_back(validBytes(Tier::TLC));
  for (uint32_t rid = 0; rid < 2; ++rid) {
    values.push_back(writePhysical[rid]);
    values.push_back(migrationPhysical[rid]);
    values.push_back(combinedPhysical[rid]);
    values.push_back(gcLogical[rid]);
    values.push_back(gcPhysical[rid]);
  }
}

void SubpageMapping::resetStatValues() {
  backing.resetStatValues();
  stat = {};
  stat.peakPendingGroups = pending[0].size() + pending[1].size();
  gcLogical[0] = gcLogical[1] = 0;
  gcPhysical[0] = gcPhysical[1] = 0;
  writePhysical[0] = writePhysical[1] = 0;
  migrationPhysical[0] = migrationPhysical[1] = 0;
  combinedPhysical[0] = combinedPhysical[1] = 0;
}
}  // namespace FTL
}  // namespace SimpleSSD

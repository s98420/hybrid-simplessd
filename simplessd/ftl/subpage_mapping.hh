#ifndef __FTL_SUBPAGE_MAPPING__
#define __FTL_SUBPAGE_MAPPING__

#include <array>
#include <map>
#include <set>
#include <unordered_map>
#include "ftl/page_mapping.hh"

namespace SimpleSSD {
namespace FTL {

// 4 KiB logical ownership over 16 KiB copy-on-write physical pages.
// Masks are keyed by logical physical-page number, so GC relocation does not
// change them. The backing FTL retains a page until its last subpage is invalid.
class SubpageMapping : public AbstractFTL {
  Parameter &physicalParam;
  bool coalescing;
  PageMapping backing;
  std::array<std::unordered_map<uint64_t, uint8_t>, 2> masks;
  struct PendingMove {
    Tier srcTier;
    uint64_t srcPage;
    uint8_t srcBit;
    uint8_t dstBit;
  };
  std::array<std::map<uint64_t, std::vector<PendingMove>>, 2> pending;
  std::array<std::unordered_map<uint64_t, uint8_t>, 2> pendingSource;
  static constexpr uint32_t subpages = 4;
  static constexpr uint32_t logicalBytes = 4096;
  static constexpr uint32_t physicalBytes = 16384;
  struct {
    uint64_t commands = 0;
    uint64_t rejected = 0;
    uint64_t logical = 0;
    uint64_t sourceRead = 0;
    uint64_t bufferedReadSource = 0;
    uint64_t destinationRead = 0;
    uint64_t program = 0;
    uint64_t pagePrograms = 0;
    uint64_t fullPagePrograms = 0;
    uint64_t forcedPagePrograms = 0;
    uint64_t peakPendingGroups = 0;
  } stat;
  uint64_t gcLogical[2] = {0, 0};
  uint64_t gcPhysical[2] = {0, 0};
  uint64_t writePhysical[2] = {0, 0};
  uint64_t migrationPhysical[2] = {0, 0};
  uint64_t combinedPhysical[2] = {0, 0};

  uint8_t mask(Tier, uint64_t);
  void setMask(Tier, uint64_t, uint8_t);
  Request request(Tier, uint64_t);
  uint64_t capacity(Tier) const;
  std::map<uint64_t, uint8_t> groups(Request &);
  void remove(Tier, uint64_t, uint8_t, uint64_t &);
  uint8_t pendingMask(Tier, uint64_t);
  void materialize(Tier, uint64_t, uint64_t &, bool);
  void materializeRelated(Tier, uint64_t, uint8_t, uint64_t &);
  void materializeSourceRelated(Tier, uint64_t, uint8_t, uint64_t &);
  bool combinePendingWithWrite(Tier, uint64_t, uint8_t, uint64_t &);
  void materializeAll(uint64_t &);
  void releaseSource(const PendingMove &, uint64_t &);
  uint64_t validBytes(Tier);

 public:
  SubpageMapping(ConfigReader &, Parameter &, Parameter &, PAL::PAL *, DRAM::AbstractDRAM *);
  bool initialize() override;
  void read(Request &, uint64_t &) override;
  void write(Request &, uint64_t &) override;
  void trim(Request &, uint64_t &) override;
  bool migrate(MigrationRequest &, uint64_t &) override;
  void flush(Tier, uint64_t &) override;
  void format(LPNRange &, uint64_t &) override;
  Status *getStatus(uint64_t, uint64_t) override;
  void getStatList(std::vector<Stats> &, std::string) override;
  void getStatValues(std::vector<double> &) override;
  void resetStatValues() override;
};
}  // namespace FTL
}  // namespace SimpleSSD
#endif

# hybrid-simplessd
support slc + tlc hybrid ssd and migrate request

執行模擬的指令 

./simplessd-standalone ./config/sample.cfg ./simplessd/config/sample.cfg ./output

trace 形式

1. W(write)、R(read)、T(trim)

time | opration | LBA | NLB | tier

2. M(migrate)

time | M | srcTier | srcLBA | dstTier | dstLBA | NLB

SLC -> tier 0

TLC -> tier 1

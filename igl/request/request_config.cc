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

#include "igl/request/request_config.hh"

#include "simplessd/sim/trace.hh"
#include "util/convert.hh"

namespace IGL {

const char NAME_IO_SIZE[] = "io_size";
const char NAME_IO_TYPE[] = "readwrite";
const char NAME_IO_MIX_RATIO[] = "rwmixread";
const char NAME_BLOCK_SIZE[] = "blocksize";
const char NAME_BLOCK_ALIGN[] = "blockalign";
const char NAME_IO_MODE[] = "iomode";
const char NAME_IO_DEPTH[] = "iodepth";
const char NAME_OFFSET[] = "offset";
const char NAME_SIZE[] = "size";
const char NAME_THINKTIME[] = "thinktime";
const char NAME_RANDOM_SEED[] = "randseed";
const char NAME_DEFAULT_TIER[] = "default_tier";
const char NAME_ENABLE_MIGRATION[] = "EnableMigration";
const char NAME_MIGRATION_RATIO[] = "MigrationRatio";
const char NAME_MIGRATION_DIRECTION[] = "MigrationDirection";
const char NAME_TIME_BASED[] = "time_based";
const char NAME_RUN_TIME[] = "runtime";

RequestConfig::RequestConfig() {
  io_size = 0;
  type = IO_READ;
  rwmixread = 0;
  blocksize = 0;
  blockalign = 0;
  mode = IO_SYNC;
  iodepth = 0;
  offset = 0;
  size = 0;
  thinktime = 0;
  randseed = 0;
  defaultTier = 1;
  enableMigration = false;
  migrationRatio = 0.f;
  migrationDirection = MIGRATION_BOTH;
  time_based = false;
  runtime = 0;
}

bool RequestConfig::setConfig(const char *name, const char *value) {
  bool ret = true;

  if (MATCH_NAME(NAME_IO_SIZE)) {
    io_size = convertInteger(value);
  }
  else if (MATCH_NAME(NAME_IO_TYPE)) {
    if (strcasecmp(value, "read") == 0) {
      type = IO_READ;
    }
    else if (strcasecmp(value, "write") == 0) {
      type = IO_WRITE;
    }
    else if (strcasecmp(value, "randread") == 0) {
      type = IO_RANDREAD;
    }
    else if (strcasecmp(value, "randwrite") == 0) {
      type = IO_RANDWRITE;
    }
    else if (strcasecmp(value, "readwrite") == 0) {
      type = IO_READWRITE;
    }
    else if (strcasecmp(value, "randrw") == 0) {
      type = IO_RANDRW;
    }
    else {
      type = IO_TYPE_NUM;
    }
  }
  else if (MATCH_NAME(NAME_IO_MIX_RATIO)) {
    rwmixread = strtof(value, nullptr);
  }
  else if (MATCH_NAME(NAME_BLOCK_SIZE)) {
    blocksize = convertInteger(value);
  }
  else if (MATCH_NAME(NAME_BLOCK_ALIGN)) {
    blockalign = convertInteger(value);
  }
  else if (MATCH_NAME(NAME_IO_MODE)) {
    if (strcasecmp(value, "sync") == 0) {
      mode = IO_SYNC;
    }
    else if (strcasecmp(value, "async") == 0) {
      mode = IO_ASYNC;
    }
    else {
      mode = IO_MODE_NUM;
    }
  }
  else if (MATCH_NAME(NAME_IO_DEPTH)) {
    iodepth = convertInteger(value);
  }
  else if (MATCH_NAME(NAME_OFFSET)) {
    offset = convertInteger(value);
  }
  else if (MATCH_NAME(NAME_SIZE)) {
    size = convertInteger(value);
  }
  else if (MATCH_NAME(NAME_THINKTIME)) {
    thinktime = convertInteger(value);
  }
  else if (MATCH_NAME(NAME_RANDOM_SEED)) {
    randseed = convertInteger(value);
  }
  else if (MATCH_NAME(NAME_DEFAULT_TIER)) {
    defaultTier = convertInteger(value);
  }
  else if (MATCH_NAME(NAME_ENABLE_MIGRATION)) {
    enableMigration = convertBoolean(value);
  }
  else if (MATCH_NAME(NAME_MIGRATION_RATIO)) {
    migrationRatio = strtof(value, nullptr);
  }
  else if (MATCH_NAME(NAME_MIGRATION_DIRECTION)) {
    if (strcasecmp(value, "both") == 0) {
      migrationDirection = MIGRATION_BOTH;
    }
    else if (strcasecmp(value, "slc_to_tlc") == 0) {
      migrationDirection = MIGRATION_SLC_TO_TLC;
    }
    else if (strcasecmp(value, "tlc_to_slc") == 0) {
      migrationDirection = MIGRATION_TLC_TO_SLC;
    }
    else {
      migrationDirection = MIGRATION_DIRECTION_NUM;
    }
  }
  else if (MATCH_NAME(NAME_TIME_BASED)) {
    time_based = convertBoolean(value);
  }
  else if (MATCH_NAME(NAME_RUN_TIME)) {
    runtime = convertTime(value);
  }
  else {
    ret = false;
  }

  return ret;
}

void RequestConfig::update() {
  if (type == IO_TYPE_NUM) {
    SimpleSSD::panic("Invalid value of readwrite");
  }
  if (mode == IO_MODE_NUM) {
    SimpleSSD::panic("Invalid value of iomode");
  }
  if (rwmixread < 0 || rwmixread > 1) {
    SimpleSSD::panic("Invalid value of rwmixread");
  }
  if (defaultTier > 1) {
    SimpleSSD::panic("Invalid value of default_tier");
  }
  if (migrationRatio < 0 || migrationRatio > 1) {
    SimpleSSD::panic("Invalid value of MigrationRatio");
  }
  if (migrationDirection >= MIGRATION_DIRECTION_NUM) {
    SimpleSSD::panic("Invalid value of MigrationDirection");
  }
}

uint64_t RequestConfig::readUint(uint32_t idx) {
  uint64_t ret = 0;

  switch (idx) {
    case REQUEST_IO_SIZE:
      ret = io_size;
      break;
    case REQUEST_IO_TYPE:
      ret = type;
      break;
    case REQUEST_BLOCK_SIZE:
      ret = blocksize;
      break;
    case REQUEST_BLOCK_ALIGN:
      ret = blockalign;
      break;
    case REQUEST_IO_MODE:
      ret = mode;
      break;
    case REQUEST_IO_DEPTH:
      ret = iodepth;
      break;
    case REQUEST_OFFSET:
      ret = offset;
      break;
    case REQUEST_SIZE:
      ret = size;
      break;
    case REQUEST_THINKTIME:
      ret = thinktime;
      break;
    case REQUEST_RANDOM_SEED:
      ret = randseed;
      break;
    case REQUEST_DEFAULT_TIER:
      ret = defaultTier;
      break;
    case REQUEST_ENABLE_MIGRATION:
      ret = enableMigration;
      break;
    case REQUEST_MIGRATION_DIRECTION:
      ret = migrationDirection;
      break;
    case REQUEST_RUN_TIME:
      ret = runtime;
      break;
  }

  return ret;
}

float RequestConfig::readFloat(uint32_t idx) {
  float ret = 0.f;

  switch (idx) {
    case REQUEST_IO_MIX_RATIO:
      ret = rwmixread;
      break;
    case REQUEST_MIGRATION_RATIO:
      ret = migrationRatio;
      break;
  }

  return ret;
}

bool RequestConfig::readBoolean(uint32_t idx) {
  bool ret = false;

  switch (idx) {
    case REQUEST_TIME_BASED:
      ret = time_based;
      break;
    case REQUEST_ENABLE_MIGRATION:
      ret = enableMigration;
      break;
  }

  return ret;
}

}  // namespace IGL

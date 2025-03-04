//==============================================================
// Copyright (C) Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#ifndef PTI_TOOLS_UNITRACE_LEVEL_ZERO_COLLECTOR_H_
#define PTI_TOOLS_UNITRACE_LEVEL_ZERO_COLLECTOR_H_

#include <chrono>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <dlfcn.h>

#include <level_zero/ze_api.h>
#include <level_zero/layers/zel_tracing_api.h>

#include "correlator.h"
#include "utils.h"
#include "ze_event_cache.h"
#include "ze_utils.h"
#include "collector_options.h"
#include "unikernel.h"
#include "unitimer.h"
#include "unicontrol.h"
#include "unimemory.h"

#include "common_header.gen"

struct ZeMetricQueryPoolKey {
  ze_context_handle_t context_;
  ze_device_handle_t device_;
  zet_metric_group_handle_t group_;
};

struct ZeMetricQueryPoolKeyCompare {
  bool operator()(const ZeMetricQueryPoolKey& lhs, const ZeMetricQueryPoolKey& rhs) const {
    if (lhs.context_ < rhs.context_) {
      return true;
    }
    if (lhs.context_ == rhs.context_) {
      if (lhs.device_ < rhs.device_) {
        return true;
      }
      if (lhs.device_ == rhs.device_) {
        return (lhs.group_ < rhs.group_);
      }
    }
    return false;
  }
};
 
struct ZeMetricQueryPools {
  constexpr static uint32_t pool_size_ = 128;
  std::mutex query_pool_mutex_;
  std::map<zet_metric_query_handle_t, ZeMetricQueryPoolKey> query_pool_map_;
  std::map<ZeMetricQueryPoolKey, std::vector<zet_metric_query_handle_t>, ZeMetricQueryPoolKeyCompare> free_pool_;
  std::vector<zet_metric_query_pool_handle_t> pools_;

  ZeMetricQueryPools() {}
  
  ZeMetricQueryPools(const struct ZeMetricQueryPools& that) = delete;

  ZeMetricQueryPools& operator=(const struct ZeMetricQueryPools& that) = delete;

  ~ZeMetricQueryPools() {
    ze_result_t status;

    const std::lock_guard<std::mutex> lock(query_pool_mutex_);
    for (auto it = query_pool_map_.begin(); it != query_pool_map_.end(); it++) {
      status = zetMetricQueryDestroy(it->first);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);
    }
    query_pool_map_.clear();

    for (auto it = pools_.begin(); it != pools_.end(); it++) {
      status = zetMetricQueryPoolDestroy(*it);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);
    }
    
    pools_.clear();

    free_pool_.clear();
  }

  zet_metric_query_handle_t
  GetQuery(ze_context_handle_t context, ze_device_handle_t device, zet_metric_group_handle_t group) {
    ze_result_t status;
    zet_metric_query_handle_t query;

    const std::lock_guard<std::mutex> lock(query_pool_mutex_);
    auto it = free_pool_.find({context, device, group});
    if (it == free_pool_.end()) {
      // no pools created

      zet_metric_query_pool_desc_t desc = {ZET_STRUCTURE_TYPE_METRIC_QUERY_POOL_DESC, nullptr, ZET_METRIC_QUERY_POOL_TYPE_PERFORMANCE, pool_size_};
      zet_metric_query_pool_handle_t pool;

      status = zetMetricQueryPoolCreate(context, device, group, &desc, &pool);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      pools_.push_back(pool);

      std::vector<zet_metric_query_handle_t> queries;
      for (int i = 0; i < pool_size_ - 1; i++) {
        status = zetMetricQueryCreate(pool, i, &query);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        queries.push_back(query);
        query_pool_map_.insert({query, {context, device, group}});
      }
      status = zetMetricQueryCreate(pool, pool_size_ - 1, &query);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      query_pool_map_.insert({query, {context, device, group}});
      
      free_pool_.insert({{context, device, group}, std::move(queries)});
    }
    else {
      if (it->second.size() == 0) {
        // no free queries, create a new pool

        zet_metric_query_pool_desc_t desc = {ZET_STRUCTURE_TYPE_METRIC_QUERY_POOL_DESC, nullptr, ZET_METRIC_QUERY_POOL_TYPE_PERFORMANCE, pool_size_};
        zet_metric_query_pool_handle_t pool;

        status = zetMetricQueryPoolCreate(context, device, group, &desc, &pool);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        pools_.push_back(pool);

        for (int i = 0; i < pool_size_ - 1; i++) {
          status = zetMetricQueryCreate(pool, i, &query);
          PTI_ASSERT(status == ZE_RESULT_SUCCESS);
          it->second.push_back(query);
          query_pool_map_.insert({query, {context, device, group}});
        }
        status = zetMetricQueryCreate(pool, pool_size_ - 1, &query);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        query_pool_map_.insert({query, {context, device, group}});
      }
      else {
        query = it->second.back();
        it->second.pop_back();
      }
    }

    return query;
  }
    
  void
  PutQuery(zet_metric_query_handle_t query) {
    if (query == nullptr) {
      return;
    }

    const std::lock_guard<std::mutex> lock(query_pool_mutex_);
    auto it = query_pool_map_.find(query);
    if (it == query_pool_map_.end()) {
      return;
    }
    auto it2 = free_pool_.find(it->second);
    PTI_ASSERT(it2 != free_pool_.end());
    it2->second.push_back(query);
  }

  void
  ResetQuery(zet_metric_query_handle_t query) {
    const std::lock_guard<std::mutex> lock(query_pool_mutex_);
    if (query_pool_map_.find(query) == query_pool_map_.end()) {
      return;
    }
    ze_result_t status = zetMetricQueryReset(query);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  }
};
 
struct ZeInstanceData {
  uint64_t start_time_host;
  uint64_t end_time_host;
  uint64_t kid;	// passing kid from enter callback to exit callback
};

thread_local ZeInstanceData ze_instance_data;

struct ZeFunctionTime {
  uint64_t total_time_;
  uint64_t min_time_;
  uint64_t max_time_;
  uint64_t call_count_;

  bool operator>(const ZeFunctionTime& r) const {
    if (total_time_ != r.total_time_) {
      return total_time_ > r.total_time_;
    }
    return call_count_ > r.call_count_;
  }

  bool operator!=(const ZeFunctionTime& r) const {
    if (total_time_ == r.total_time_) {
      return call_count_ != r.call_count_;
    }
    return true;
  }
};

struct ZeKernelGroupSize {
  uint32_t x;
  uint32_t y;
  uint32_t z;
};

enum ZeKernelCommandType {
  KERNEL_COMMAND_TYPE_INVALID = 0,
  KERNEL_COMMAND_TYPE_COMPUTE = 1,
  KERNEL_COMMAND_TYPE_MEMORY = 2,
  KERNEL_COMMAND_TYPE_COMMAND = 3
};

enum ZeDeviceCommandHandle {
  MemoryCopy = 0,
  MemoryCopyH2H = MemoryCopy,
  MemoryCopyH2D,
  MemoryCopyH2M,
  MemoryCopyH2S,
  MemoryCopyD2H,
  MemoryCopyD2D,
  MemoryCopyD2M,
  MemoryCopyD2S,
  MemoryCopyM2H,
  MemoryCopyM2D,
  MemoryCopyM2M,
  MemoryCopyM2S,
  MemoryCopyS2H,
  MemoryCopyS2D,
  MemoryCopyS2M,
  MemoryCopyS2S,
  MemoryCopyRegion,
  MemoryCopyRegionH2H = MemoryCopyRegion,
  MemoryCopyRegionH2D,
  MemoryCopyRegionH2M,
  MemoryCopyRegionH2S,
  MemoryCopyRegionD2H,
  MemoryCopyRegionD2D,
  MemoryCopyRegionD2M,
  MemoryCopyRegionD2S,
  MemoryCopyRegionM2H,
  MemoryCopyRegionM2D,
  MemoryCopyRegionM2M,
  MemoryCopyRegionM2S,
  MemoryCopyRegionS2H,
  MemoryCopyRegionS2D,
  MemoryCopyRegionS2M,
  MemoryCopyRegionS2S,
  MemoryCopyFromContext,
  MemoryCopyFromContextH2H = MemoryCopyFromContext,
  MemoryCopyFromContextH2D,
  MemoryCopyFromContextH2M,
  MemoryCopyFromContextH2S,
  MemoryCopyFromContextD2H,
  MemoryCopyFromContextD2D,
  MemoryCopyFromContextD2M,
  MemoryCopyFromContextD2S,
  MemoryCopyFromContextM2H,
  MemoryCopyFromContextM2D,
  MemoryCopyFromContextM2M,
  MemoryCopyFromContextM2S,
  MemoryCopyFromContextS2H,
  MemoryCopyFromContextS2D,
  MemoryCopyFromContextS2M,
  MemoryCopyFromContextS2S,
  ImageCopy,
  ImageCopyH2H = ImageCopy,
  ImageCopyH2D,
  ImageCopyH2M,
  ImageCopyH2S,
  ImageCopyD2H,
  ImageCopyD2D,
  ImageCopyD2M,
  ImageCopyD2S,
  ImageCopyM2H,
  ImageCopyM2D,
  ImageCopyM2M,
  ImageCopyM2S,
  ImageCopyS2H,
  ImageCopyS2D,
  ImageCopyS2M,
  ImageCopyS2S,
  ImageCopyRegion,
  ImageCopyRegionH2H = ImageCopyRegion,
  ImageCopyRegionH2D,
  ImageCopyRegionH2M,
  ImageCopyRegionH2S,
  ImageCopyRegionD2H,
  ImageCopyRegionD2D,
  ImageCopyRegionD2M,
  ImageCopyRegionD2S,
  ImageCopyRegionM2H,
  ImageCopyRegionM2D,
  ImageCopyRegionM2M,
  ImageCopyRegionM2S,
  ImageCopyRegionS2H,
  ImageCopyRegionS2D,
  ImageCopyRegionS2M,
  ImageCopyRegionS2S,
  ImageCopyFromMemory,
  ImageCopyFromMemoryH2H = ImageCopyFromMemory,
  ImageCopyFromMemoryH2D,
  ImageCopyFromMemoryH2M,
  ImageCopyFromMemoryH2S,
  ImageCopyFromMemoryD2H,
  ImageCopyFromMemoryD2D,
  ImageCopyFromMemoryD2M,
  ImageCopyFromMemoryD2S,
  ImageCopyFromMemoryM2H,
  ImageCopyFromMemoryM2D,
  ImageCopyFromMemoryM2M,
  ImageCopyFromMemoryM2S,
  ImageCopyFromMemoryS2H,
  ImageCopyFromMemoryS2D,
  ImageCopyFromMemoryS2M,
  ImageCopyFromMemoryS2S,
  ImageCopyToMemory,
  ImageCopyToMemoryH2H = ImageCopyToMemory,
  ImageCopyToMemoryH2D,
  ImageCopyToMemoryH2M,
  ImageCopyToMemoryH2S,
  ImageCopyToMemoryD2H,
  ImageCopyToMemoryD2D,
  ImageCopyToMemoryD2M,
  ImageCopyToMemoryD2S,
  ImageCopyToMemoryM2H,
  ImageCopyToMemoryM2D,
  ImageCopyToMemoryM2M,
  ImageCopyToMemoryM2S,
  ImageCopyToMemoryS2H,
  ImageCopyToMemoryS2D,
  ImageCopyToMemoryS2M,
  ImageCopyToMemoryS2S,
  MemoryFill,
  MemoryFillH = MemoryFill,
  MemoryFillD,
  MemoryFillM,
  MemoryFillS,
  Barrier,
  MemoryRangesBarrier,
  LastCommand = MemoryRangesBarrier
};

static const char *device_command_names[] = {
  "zeCommandListAppendMemoryCopy(H2H)",
  "zeCommandListAppendMemoryCopy(H2D)",
  "zeCommandListAppendMemoryCopy(H2M)",
  "zeCommandListAppendMemoryCopy(H2S)",
  "zeCommandListAppendMemoryCopy(D2H)",
  "zeCommandListAppendMemoryCopy(D2D)",
  "zeCommandListAppendMemoryCopy(D2M)",
  "zeCommandListAppendMemoryCopy(D2S)",
  "zeCommandListAppendMemoryCopy(M2H)",
  "zeCommandListAppendMemoryCopy(M2D)",
  "zeCommandListAppendMemoryCopy(M2M)",
  "zeCommandListAppendMemoryCopy(M2S)",
  "zeCommandListAppendMemoryCopy(S2H)",
  "zeCommandListAppendMemoryCopy(S2D)",
  "zeCommandListAppendMemoryCopy(S2M)",
  "zeCommandListAppendMemoryCopy(S2S)",
  "zeCommandListAppendMemoryCopyRegion(H2H)",
  "zeCommandListAppendMemoryCopyRegion(H2D)",
  "zeCommandListAppendMemoryCopyRegion(H2M)",
  "zeCommandListAppendMemoryCopyRegion(H2S)",
  "zeCommandListAppendMemoryCopyRegion(D2H)",
  "zeCommandListAppendMemoryCopyRegion(D2D)",
  "zeCommandListAppendMemoryCopyRegion(D2M)",
  "zeCommandListAppendMemoryCopyRegion(D2S)",
  "zeCommandListAppendMemoryCopyRegion(M2H)",
  "zeCommandListAppendMemoryCopyRegion(M2D)",
  "zeCommandListAppendMemoryCopyRegion(M2M)",
  "zeCommandListAppendMemoryCopyRegion(M2S)",
  "zeCommandListAppendMemoryCopyRegion(S2H)",
  "zeCommandListAppendMemoryCopyRegion(S2D)",
  "zeCommandListAppendMemoryCopyRegion(S2M)",
  "zeCommandListAppendMemoryCopyRegion(S2S)",
  "zeCommandListAppendMemoryCopyFromContext(H2H)",
  "zeCommandListAppendMemoryCopyFromContext(H2D)",
  "zeCommandListAppendMemoryCopyFromContext(H2M)",
  "zeCommandListAppendMemoryCopyFromContext(H2S)",
  "zeCommandListAppendMemoryCopyFromContext(D2H)",
  "zeCommandListAppendMemoryCopyFromContext(D2D)",
  "zeCommandListAppendMemoryCopyFromContext(D2M)",
  "zeCommandListAppendMemoryCopyFromContext(D2S)",
  "zeCommandListAppendMemoryCopyFromContext(M2H)",
  "zeCommandListAppendMemoryCopyFromContext(M2D)",
  "zeCommandListAppendMemoryCopyFromContext(M2M)",
  "zeCommandListAppendMemoryCopyFromContext(M2S)",
  "zeCommandListAppendMemoryCopyFromContext(S2H)",
  "zeCommandListAppendMemoryCopyFromContext(S2D)",
  "zeCommandListAppendMemoryCopyFromContext(S2M)",
  "zeCommandListAppendMemoryCopyFromContext(S2S)",
  "zeCommandListAppendImageCopy(H2H)",
  "zeCommandListAppendImageCopy(H2D)",
  "zeCommandListAppendImageCopy(H2M)",
  "zeCommandListAppendImageCopy(H2S)",
  "zeCommandListAppendImageCopy(D2H)",
  "zeCommandListAppendImageCopy(D2D)",
  "zeCommandListAppendImageCopy(D2M)",
  "zeCommandListAppendImageCopy(D2S)",
  "zeCommandListAppendImageCopy(M2H)",
  "zeCommandListAppendImageCopy(M2D)",
  "zeCommandListAppendImageCopy(M2M)",
  "zeCommandListAppendImageCopy(M2S)",
  "zeCommandListAppendImageCopy(S2H)",
  "zeCommandListAppendImageCopy(S2D)",
  "zeCommandListAppendImageCopy(S2M)",
  "zeCommandListAppendImageCopy(S2S)",
  "zeCommandListAppendImageCopyRegion(H2H)",
  "zeCommandListAppendImageCopyRegion(H2D)",
  "zeCommandListAppendImageCopyRegion(H2M)",
  "zeCommandListAppendImageCopyRegion(H2S)",
  "zeCommandListAppendImageCopyRegion(D2H)",
  "zeCommandListAppendImageCopyRegion(D2D)",
  "zeCommandListAppendImageCopyRegion(D2M)",
  "zeCommandListAppendImageCopyRegion(D2S)",
  "zeCommandListAppendImageCopyRegion(M2H)",
  "zeCommandListAppendImageCopyRegion(M2D)",
  "zeCommandListAppendImageCopyRegion(M2M)",
  "zeCommandListAppendImageCopyRegion(M2S)",
  "zeCommandListAppendImageCopyRegion(S2H)",
  "zeCommandListAppendImageCopyRegion(S2D)",
  "zeCommandListAppendImageCopyRegion(S2M)",
  "zeCommandListAppendImageCopyRegion(S2S)",
  "zeCommandListAppendImageCopyFromMemory(H2H)",
  "zeCommandListAppendImageCopyFromMemory(H2D)",
  "zeCommandListAppendImageCopyFromMemory(H2M)",
  "zeCommandListAppendImageCopyFromMemory(H2S)",
  "zeCommandListAppendImageCopyFromMemory(D2H)",
  "zeCommandListAppendImageCopyFromMemory(D2D)",
  "zeCommandListAppendImageCopyFromMemory(D2M)",
  "zeCommandListAppendImageCopyFromMemory(D2S)",
  "zeCommandListAppendImageCopyFromMemory(M2H)",
  "zeCommandListAppendImageCopyFromMemory(M2D)",
  "zeCommandListAppendImageCopyFromMemory(M2M)",
  "zeCommandListAppendImageCopyFromMemory(M2S)",
  "zeCommandListAppendImageCopyFromMemory(S2H)",
  "zeCommandListAppendImageCopyFromMemory(S2D)",
  "zeCommandListAppendImageCopyFromMemory(S2M)",
  "zeCommandListAppendImageCopyFromMemory(S2S)",
  "zeCommandListAppendImageCopyToMemory(H2H)",
  "zeCommandListAppendImageCopyToMemory(H2D)",
  "zeCommandListAppendImageCopyToMemory(H2M)",
  "zeCommandListAppendImageCopyToMemory(H2S)",
  "zeCommandListAppendImageCopyToMemory(D2H)",
  "zeCommandListAppendImageCopyToMemory(D2D)",
  "zeCommandListAppendImageCopyToMemory(D2M)",
  "zeCommandListAppendImageCopyToMemory(D2S)",
  "zeCommandListAppendImageCopyToMemory(M2H)",
  "zeCommandListAppendImageCopyToMemory(M2D)",
  "zeCommandListAppendImageCopyToMemory(M2M)",
  "zeCommandListAppendImageCopyToMemory(M2S)",
  "zeCommandListAppendImageCopyToMemory(S2H)",
  "zeCommandListAppendImageCopyToMemory(S2D)",
  "zeCommandListAppendImageCopyToMemory(S2M)",
  "zeCommandListAppendImageCopyToMemory(S2S)",
  "zeCommandListAppendMemoryFill(H)",
  "zeCommandListAppendMemoryFill(D)",
  "zeCommandListAppendMemoryFill(M)",
  "zeCommandListAppendMemoryFill(S)",
  "zeCommandListAppendBarrier", 
  "zeCommandListAppendMemoryRangesBarrier" 
};

struct ZeKernelCommandTime {
  uint64_t append_time_;
  uint64_t submit_time_;
  uint64_t execute_time_;
  uint64_t min_time_;
  uint64_t max_time_;
  uint64_t call_count_;

  bool operator>(const ZeKernelCommandTime& r) const {
    if (execute_time_ != r.execute_time_) {
      return execute_time_ > r.execute_time_;
    }
    return call_count_ > r.call_count_;
  }

  bool operator!=(const ZeKernelCommandTime& r) const {
    if (execute_time_ == r.execute_time_) {
      return call_count_ != r.call_count_;
    }
    return true;
  }
};

struct ZeKernelCommandNameKey {
  uint64_t kernel_command_id_;
  uint64_t mem_size_;
  int tile_;
  ze_group_count_t group_count_;

  bool operator>(const ZeKernelCommandNameKey& r) const {
    if (kernel_command_id_ != r.kernel_command_id_) {
      return kernel_command_id_ > r.kernel_command_id_;
    }
    if (mem_size_ != r.mem_size_) {
      return mem_size_ > r.mem_size_;
    }
    if (tile_ != r.tile_) {
      return tile_ > r.tile_;
    }
    
    if (group_count_.groupCountX != r.group_count_.groupCountX) {
      return (group_count_.groupCountX > r.group_count_.groupCountX);
    }

    if (group_count_.groupCountY != r.group_count_.groupCountY) {
      return (group_count_.groupCountY > r.group_count_.groupCountY);
    }

    return (group_count_.groupCountZ > r.group_count_.groupCountZ);
  }

  bool operator!=(const ZeKernelCommandNameKey& r) const {
    if (kernel_command_id_ == r.kernel_command_id_) {
      if (mem_size_ == r.mem_size_) {
        if (tile_ == r.tile_) {
          return ((group_count_.groupCountX != r.group_count_.groupCountX) ||
              (group_count_.groupCountX != r.group_count_.groupCountX) || (group_count_.groupCountX != r.group_count_.groupCountX));
        }
      }
    }

    return true;
  }
};

struct ZeKernelCommandNameKeyCompare {
  bool operator()(const ZeKernelCommandNameKey& lhs, const ZeKernelCommandNameKey& rhs) const {
    if (lhs.kernel_command_id_ != rhs.kernel_command_id_) {
      return (lhs.kernel_command_id_ < rhs.kernel_command_id_);
    }
    if (lhs.mem_size_ != rhs.mem_size_) {
      return (lhs.mem_size_ < rhs.mem_size_);
    }
    if (lhs.tile_ != rhs.tile_) {
      return (lhs.tile_ < rhs.tile_);
    }
    if (lhs.group_count_.groupCountX != rhs.group_count_.groupCountX) {
      return (lhs.group_count_.groupCountX < rhs.group_count_.groupCountX);
    }
    if (lhs.group_count_.groupCountY != rhs.group_count_.groupCountY) {
      return (lhs.group_count_.groupCountY < rhs.group_count_.groupCountY);
    }
    if (lhs.group_count_.groupCountZ != rhs.group_count_.groupCountZ) {
      return (lhs.group_count_.groupCountZ < rhs.group_count_.groupCountZ);
    }
    return false;
  }
};
  
struct ZeKernelProfileTimestamps {
  uint64_t metric_start;
  uint64_t metric_end;
  uint64_t device_start;
  uint64_t device_end;
  int32_t subdevice_id;
};

struct ZeKernelProfileRecord {
  ze_device_handle_t device_ = nullptr;
  std::vector<ZeKernelProfileTimestamps> timestamps_;
  uint64_t kernel_command_id_;
  uint64_t instance_id_;
  ze_group_count_t group_count_;
  size_t mem_size_;
  std::vector<uint8_t> *metrics_ = nullptr;
};

using ZeKernelProfiles = std::map<uint64_t, ZeKernelProfileRecord>;

static std::mutex global_kernel_profiles_mutex_;
static ZeKernelProfiles global_kernel_profiles_;

void SweepKernelProfiles(ZeKernelProfiles& profiles) {
  const std::lock_guard<std::mutex> lock(global_kernel_profiles_mutex_);
  global_kernel_profiles_.insert(profiles.begin(), profiles.end());
}

static std::mutex global_device_time_stats_mutex_;
static std::map<ZeKernelCommandNameKey, ZeKernelCommandTime, ZeKernelCommandNameKeyCompare> *global_device_time_stats_;

void SweepKernelCommandTimeStats(std::map<ZeKernelCommandNameKey, ZeKernelCommandTime, ZeKernelCommandNameKeyCompare>& stats) {
  global_device_time_stats_mutex_.lock();
  if (global_device_time_stats_ == nullptr) {
    global_device_time_stats_ = new std::map<ZeKernelCommandNameKey, ZeKernelCommandTime, ZeKernelCommandNameKeyCompare>;
    UniMemory::ExitIfOutOfMemory((void *)(global_device_time_stats_));
  }
  for (auto it = stats.begin(); it != stats.end(); it++) {
    auto it2 = global_device_time_stats_->find(it->first);
    if (it2 == global_device_time_stats_->end()) {
      ZeKernelCommandTime stat;
      stat.append_time_ = it->second.append_time_;
      stat.submit_time_ = it->second.submit_time_;
      stat.execute_time_ = it->second.execute_time_;
      stat.min_time_ = it->second.min_time_;
      stat.max_time_ = it->second.max_time_;
      stat.call_count_ = it->second.call_count_;
      global_device_time_stats_->insert({it->first, std::move(stat)});
    }
    else {
      it2->second.append_time_ += it->second.append_time_;
      it2->second.submit_time_ +=  it->second.submit_time_;
      it2->second.execute_time_ += it->second.execute_time_;
      if (it->second.max_time_ > it2->second.max_time_) {
        it2->second.max_time_ = it->second.max_time_;
      }
      if (it->second.min_time_ < it2->second.min_time_) {
        it2->second.min_time_ = it->second.min_time_;
      }
      it2->second.call_count_ += it->second.call_count_;
    }
  }
  global_device_time_stats_mutex_.unlock();
}

static std::mutex global_host_time_stats_mutex_;
static std::map<uint32_t, ZeFunctionTime> *global_host_time_stats_;

void SweepHostFunctionTimeStats(std::map<uint32_t, ZeFunctionTime>& stats) {
  global_host_time_stats_mutex_.lock();
  if (global_host_time_stats_ == nullptr) {
    global_host_time_stats_ = new std::map<uint32_t, ZeFunctionTime>;
    UniMemory::ExitIfOutOfMemory((void *)(global_host_time_stats_));
  }
  for (auto it = stats.begin(); it != stats.end(); it++) {
    auto it2 = global_host_time_stats_->find(it->first);
    if (it2 == global_host_time_stats_->end()) {
      ZeFunctionTime stat;
      stat.total_time_ = it->second.total_time_;
      stat.min_time_ = it->second.min_time_;
      stat.max_time_ = it->second.max_time_;
      stat.call_count_ = it->second.call_count_;
      global_host_time_stats_->insert({it->first, std::move(stat)});
    }
    else {
      it2->second.total_time_ += it->second.total_time_;
      if (it->second.max_time_ > it2->second.max_time_) {
        it2->second.max_time_ = it->second.max_time_;
      }
      if (it->second.min_time_ < it2->second.min_time_) {
        it2->second.min_time_ = it->second.min_time_;
      }
      it2->second.call_count_ += it->second.call_count_;
    }
  }
  global_host_time_stats_mutex_.unlock();
}

struct ZeCommandMetricQuery {
  uint64_t instance_id_ = 0;	//unique kernel or command instance identifier
  zet_metric_query_handle_t metric_query_ = nullptr;
  ze_event_handle_t metric_query_event_ = nullptr;
  ze_device_handle_t device_ = nullptr;
  ZeKernelCommandType type_;
  bool immediate_;
};

struct ZeCommand {
  uint64_t kernel_command_id_;	//kernel or command identifier
  uint64_t instance_id_ = 0;	//unique kernel or command instance identifier
  ze_event_handle_t event_ = nullptr;
  ze_device_handle_t device_ = nullptr;
  uint64_t host_time_origin_;	// in ns
  uint64_t device_timer_frequency_;
  uint64_t device_timer_mask_;
  uint64_t metric_timer_frequency_;
  uint64_t metric_timer_mask_;
  uint64_t append_time_ = 0;
  uint64_t submit_time_ = 0;		//in ns
  uint64_t submit_time_device_ = 0;	//in ns
  ze_command_list_handle_t command_list_ = nullptr;
  ze_command_queue_handle_t queue_ = nullptr;
  ze_fence_handle_t fence_;
  uint64_t tid_;
  uint64_t mem_size_;	// memory copy/fill size
  ZeCommandMetricQuery *command_metric_query_;
  uint32_t engine_ordinal_;
  uint32_t engine_index_;
  ZeKernelGroupSize group_size_;
  ze_group_count_t group_count_;
  ZeKernelCommandType type_;	
  bool implicit_scaling_;
  bool immediate_;
};


struct ZeDeviceSubmissions;
std::shared_mutex global_device_submissions_mutex_;
std::set<ZeDeviceSubmissions *> *global_device_submissions_ = nullptr;

struct ZeDeviceSubmissions {
  std::list<ZeCommand *> commands_submitted_;
  std::list<ZeCommand *> commands_free_pool_;
  std::list<ZeCommandMetricQuery *> metric_queries_submitted_;
  std::list<ZeCommandMetricQuery *> metric_queries_free_pool_;
  std::map<ZeKernelCommandNameKey, ZeKernelCommandTime, ZeKernelCommandNameKeyCompare> device_time_stats_;
  std::map<uint32_t, ZeFunctionTime> host_time_stats_;
  ZeKernelProfiles kernel_profiles_;
  std::atomic<bool> finalized_;

  ZeDeviceSubmissions() {
    finalized_.store(false, std::memory_order_release);

    ZeCommand *command = new ZeCommand;

    UniMemory::ExitIfOutOfMemory((void *)(command));
    
    commands_free_pool_.push_back(command);
    global_device_submissions_mutex_.lock();
    if (global_device_submissions_ == nullptr) {
      global_device_submissions_ = new std::set<ZeDeviceSubmissions *>;
      UniMemory::ExitIfOutOfMemory((void *)(global_device_submissions_));
    }

    global_device_submissions_->insert(this);
    global_device_submissions_mutex_.unlock();
  }

  ~ZeDeviceSubmissions() {
    global_device_submissions_mutex_.lock();
    if (!finalized_.exchange(true)) {
      // finalize if not finalized
      SweepKernelCommandTimeStats(device_time_stats_);
      SweepHostFunctionTimeStats(host_time_stats_);
      SweepKernelProfiles(kernel_profiles_);
      global_device_submissions_->erase(this);
    }
    global_device_submissions_mutex_.unlock();
  }
  
  ZeDeviceSubmissions(const struct ZeDeviceSubmissions& that) = delete;

  ZeDeviceSubmissions& operator=(const struct ZeDeviceSubmissions& that) = delete;

  inline void SubmitKernelCommand(ZeCommand *command) {
    if (!IsFinalized()) {
      commands_submitted_.push_back(command);
    }
    else {
      commands_free_pool_.push_back(command);
    }
  }
  
  inline ZeCommand *GetKernelCommand(void) {
    ZeCommand *command;

    if (commands_free_pool_.empty()) {
      command = new ZeCommand;
      UniMemory::ExitIfOutOfMemory((void *)(command));
    }
    else {
      command = commands_free_pool_.front();
      commands_free_pool_.pop_front();
    }

    return command;
  }

  inline void SubmitCommandMetricQuery(ZeCommandMetricQuery *query) {
    if (!IsFinalized()) {
      metric_queries_submitted_.push_back(query);
    }
    else {
      metric_queries_free_pool_.push_back(query);
    }
  }
  
  inline ZeCommandMetricQuery *GetCommandMetricQuery(void) {
    ZeCommandMetricQuery *query;

    if (metric_queries_free_pool_.empty()) {
      query = new ZeCommandMetricQuery;
      UniMemory::ExitIfOutOfMemory((void *)(query));
    }
    else {
      query = metric_queries_free_pool_.front();
      metric_queries_free_pool_.pop_front();
    }

    return query;
  }

  inline void CollectHostFunctionTimeStats(uint32_t id, uint64_t host_time) {
    auto it = host_time_stats_.find(id);
    if (it == host_time_stats_.end()){
      ZeFunctionTime stat;
      stat.total_time_ = host_time;
      stat.min_time_ = host_time;
      stat.max_time_ = host_time;
      stat.call_count_ = 1;
      host_time_stats_.insert({id, std::move(stat)});
    }
    else {
      it->second.total_time_ += host_time;
      if (host_time > it->second.max_time_) {
        it->second.max_time_ = host_time;
      }
      if (host_time < it->second.min_time_) {
        it->second.min_time_ = host_time;
      }
      it->second.call_count_ += 1;
    }
  }

  inline void CollectKernelCommandTimeStats(const ZeCommand *command, uint64_t kernel_start, uint64_t kernel_end, int tile) {
    ZeKernelCommandNameKey key {command->kernel_command_id_, command->mem_size_, tile, command->group_count_};
    uint64_t kernel_time = kernel_end - kernel_start;
    auto it = device_time_stats_.find(key);
    if (it == device_time_stats_.end()){
      ZeKernelCommandTime stat;
      stat.append_time_ = command->submit_time_ - command->append_time_;
      stat.submit_time_ = kernel_start - command->submit_time_;
      stat.execute_time_ = kernel_time;
      stat.min_time_ = kernel_time;
      stat.max_time_ = kernel_time;
      stat.call_count_ = 1;
      device_time_stats_.insert({std::move(key), std::move(stat)});
    }
    else {
      it->second.append_time_ += (command->submit_time_ - command->append_time_);
      it->second.submit_time_ +=  (kernel_start - command->submit_time_);
      it->second.execute_time_ += kernel_time;
      if (kernel_time > it->second.max_time_) {
        it->second.max_time_ = kernel_time;
      }
      if (kernel_time < it->second.min_time_) {
        it->second.min_time_ = kernel_time;
      }
      it->second.call_count_ += 1;
    }
  }

  inline bool IsFinalized(void) {
    return finalized_.load(std::memory_order_acquire);
  }

  inline void Finalize(void) {
    // caller holds exclusive global_device_submissions_mutex_ lock
    finalized_.store(true, std::memory_order_release);
    SweepKernelCommandTimeStats(device_time_stats_);
    SweepHostFunctionTimeStats(host_time_stats_);
    SweepKernelProfiles(kernel_profiles_);
  }
};

thread_local ZeDeviceSubmissions local_device_submissions_;

struct ZeKernelCommandProperties {
  uint64_t id_;		// unique identidier
  uint64_t size_;	// kernel binary size
  uint64_t base_addr_;	// kernel base address
  ze_device_handle_t device_;
  int32_t device_id_;
  uint32_t simd_width_;	// SIMD
  uint32_t nargs_;	// number of kernel arguments
  uint32_t nsubgrps_;	// maximal number of subgroups
  uint32_t slmsize_;	// SLM size
  uint32_t private_mem_size_;	// private memory size for each thread
  uint32_t spill_mem_size_;	// spill memory size for each thread
  ZeKernelGroupSize group_size_;	// group size
  ZeKernelCommandType type_;
  std::string name_;	// kernel or command name
};

// these will not go away when ZeCollector is destructed
static std::shared_mutex kernel_command_properties_mutex_;
static std::map<uint64_t, ZeKernelCommandProperties> *kernel_command_properties_ = nullptr;
static std::map<ze_kernel_handle_t, ZeKernelCommandProperties> *active_kernel_properties_ = nullptr;
static std::map<uint64_t, ZeKernelCommandProperties> *active_command_properties_ = nullptr;

static std::shared_mutex modules_on_devices_mutex_;
static std::map<ze_module_handle_t, std::pair<ze_device_handle_t, size_t>> modules_on_devices_; //module to <device, native binary size> map

struct ZeDevice {
  ze_device_handle_t device_;
  ze_device_handle_t parent_device_;
  uint64_t host_time_origin_;	// in ns
  uint64_t device_timer_frequency_;
  uint64_t device_timer_mask_;
  uint64_t metric_timer_frequency_;
  uint64_t metric_timer_mask_;
  ze_driver_handle_t driver_;
  ze_context_handle_t context_;
  zet_metric_group_handle_t metric_group_;
  int32_t id_;
  int32_t parent_id_;
  int32_t subdevice_id_;
  int32_t num_subdevices_;
  ze_pci_ext_properties_t pci_properties_;
};

// these will no go away when ZeCollector is destructed
static std::shared_mutex devices_mutex_;
static std::map<ze_device_handle_t, ZeDevice> *devices_;

struct ZeCommandQueue {
  ze_command_queue_handle_t queue_;
  ze_context_handle_t context_;
  ze_device_handle_t device_;
  uint32_t engine_ordinal_;
  uint32_t engine_index_;
};

struct ZeCommandList {
  ze_command_list_handle_t cmdlist_;
  ze_context_handle_t context_;
  ze_device_handle_t device_;
  uint64_t host_time_origin_;	// in ns
  uint64_t device_timer_frequency_;
  uint64_t device_timer_mask_;
  uint64_t metric_timer_frequency_;
  uint64_t metric_timer_mask_;
  uint32_t engine_ordinal_;	// valid if immediate command list
  uint32_t engine_index_;	// valid if immediate command list
  bool immediate_;
  bool implicit_scaling_;
  std::vector<ZeCommand *> commands_;	// if non-immediate command list
  std::vector<ZeCommandMetricQuery *> metric_queries_;	// if non-immediate command list
};

typedef void (*OnZeFunctionFinishCallback)(std::vector<uint64_t> *kids, FLOW_DIR flow_dir, API_TRACING_ID api_id, uint64_t started, uint64_t ended);

typedef void (*OnZeKernelFinishCallback)(uint64_t kid, uint64_t tid, uint64_t start, uint64_t end, uint32_t ordinal, uint32_t index, int32_t tile, const ze_device_handle_t device, const uint64_t kernel_command_id, bool implicit_scaling, const ze_group_count_t& group_count, size_t mem_size);

ze_result_t (*zexKernelGetBaseAddress)(ze_kernel_handle_t hKernel, uint64_t *baseAddress) = nullptr;

inline std::string GetZeKernelCommandName(uint64_t id, const ze_group_count_t& group_count, size_t size, bool detailed = true) {
  std::stringstream s;
  kernel_command_properties_mutex_.lock_shared();
  auto it = kernel_command_properties_->find(id);
  if (it != kernel_command_properties_->end()) {
    s << utils::Demangle(it->second.name_.c_str());
    if (detailed) {
      if (it->second.type_ == KERNEL_COMMAND_TYPE_COMPUTE) {
        if (it->second.simd_width_ > 0) {
          s << "[SIMD";
          if (it->second.simd_width_ == 1) {
            s << "_ANY";
          } else {
            s << it->second.simd_width_;
          }
        }
        s << " {" <<
          group_count.groupCountX << "; " <<
          group_count.groupCountY << "; " <<
          group_count.groupCountZ << "} {" <<
          it->second.group_size_.x << "; " <<
          it->second.group_size_.y << "; " <<
          it->second.group_size_.z << "}]";
      }
      else if ((it->second.type_ == KERNEL_COMMAND_TYPE_MEMORY) && (size > 0)) {
        s << "[" << size << "]";
      }
    }
  }

  kernel_command_properties_mutex_.unlock_shared();
  return s.str();
}

inline std::string GetZeKernelCommandName(uint64_t id, ze_group_count_t& group_count, size_t size, bool detailed = true) {
  const ze_group_count_t& gcount = group_count;
  return GetZeKernelCommandName(id, gcount, size, detailed);
}

inline ze_pci_ext_properties_t *GetZeDevicePciPropertiesAndId(ze_device_handle_t device, int32_t *parent_device_id, int32_t *device_id, int32_t *subdevice_id){
  devices_mutex_.lock_shared();
  ze_pci_ext_properties_t *props = nullptr;

  if (devices_) {
    auto it = devices_->find(device);
    if (it != devices_->end()) {
      if (parent_device_id) {
        *parent_device_id = it->second.parent_id_;
      }
      if (device_id) {
        *device_id = it->second.id_;
      }
      if (subdevice_id) {
        *subdevice_id = it->second.subdevice_id_;
      }
      
      props = &(it->second.pci_properties_);
    }
  }
  devices_mutex_.unlock_shared();
 
  return props;
  
}

class ZeCollector {
 public: // Interface

  static ZeCollector* Create(
      Correlator* correlator,
      CollectorOptions options,
      OnZeKernelFinishCallback kcallback = nullptr,
      OnZeFunctionFinishCallback fcallback = nullptr,
      void* callback_data = nullptr) {
    ze_api_version_t version = utils::ze::GetVersion();
    PTI_ASSERT(
        ZE_MAJOR_VERSION(version) >= 1 &&
        ZE_MINOR_VERSION(version) >= 2);

    PTI_ASSERT(correlator != nullptr);

    std::string data_dir_name = utils::GetEnv("UNITRACE_DataDir");
    ZeCollector* collector = new ZeCollector(
        correlator, options, kcallback, fcallback, callback_data, data_dir_name);

    UniMemory::ExitIfOutOfMemory((void *)(collector));

    ze_result_t status = ZE_RESULT_SUCCESS;
    zel_tracer_desc_t tracer_desc = {
        ZEL_STRUCTURE_TYPE_TRACER_EXP_DESC, nullptr, collector};
    zel_tracer_handle_t tracer = nullptr;
    status = zelTracerCreate(&tracer_desc, &tracer);
    if (status != ZE_RESULT_SUCCESS) {
      std::cerr << "[WARNING] Unable to create Level Zero tracer" << std::endl;
      delete collector;
      return nullptr;
    }

    collector->EnableTracing(tracer);

    collector->tracer_ = tracer;
    
    ze_driver_handle_t driver;
    uint32_t count = 1;
    if (zeDriverGet(&count, &driver) == ZE_RESULT_SUCCESS) {
      if (zeDriverGetExtensionFunctionAddress(driver, "zexKernelGetBaseAddress", (void **)&zexKernelGetBaseAddress) != ZE_RESULT_SUCCESS) {
        zexKernelGetBaseAddress = nullptr;
      }
    }

    return collector;
  }

  ZeCollector(const ZeCollector& that) = delete;

  ZeCollector& operator=(const ZeCollector& that) = delete;

  ~ZeCollector() {
    
    ProcessAllCommandsSubmitted(nullptr);

    if (tracer_ != nullptr) {
      ze_result_t status = zelTracerDestroy(tracer_);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);
    }

    global_device_submissions_mutex_.lock();
    if (global_device_submissions_) {
      for (auto it = global_device_submissions_->begin(); it != global_device_submissions_->end();) {
        (*it)->Finalize();
        it = global_device_submissions_->erase(it);
      }
    }
    global_device_submissions_mutex_.unlock();

    if (options_.metric_query) {
      for (auto it = metric_activations_.begin(); it != metric_activations_.end(); it++) {
        zetContextActivateMetricGroups(it->first, it->second, 0, nullptr);
      }
      metric_activations_.clear();
      for (auto& context : metric_contexts_) {
        zeContextDestroy(context);
      }
      metric_contexts_.clear();
    }

    DumpKernelProfiles();
  }

  uint64_t CalculateTotalKernelTime() const {
    uint64_t total_time = 0;

    global_device_time_stats_mutex_.lock();
    if (global_device_time_stats_) {
      for (auto it = global_device_time_stats_->begin(); it != global_device_time_stats_->end(); it++) {
        total_time += it->second.execute_time_;
      }
    }
    global_device_time_stats_mutex_.unlock();

    return total_time;
  }

  void PrintKernelsTable() const {
    uint64_t total_time = 0;
    std::vector<std::string> knames;
    size_t max_name_size = 0;
    global_device_time_stats_mutex_.lock();

    AggregateDeviceTimeStats();

    std::set<std::pair<ZeKernelCommandNameKey, ZeKernelCommandTime>, utils::Comparator> sorted_list(
        global_device_time_stats_->begin(), global_device_time_stats_->end());

    for (auto& it : sorted_list) {
      total_time += it.second.execute_time_;
      std::string kname;
      if (it.first.tile_ >= 0) {
        kname = "Tile #" + std::to_string(it.first.tile_) + ": " + GetZeKernelCommandName(it.first.kernel_command_id_, it.first.group_count_, it.first.mem_size_, options_.verbose);
      }
      else {
        kname = GetZeKernelCommandName(it.first.kernel_command_id_, it.first.group_count_, it.first.mem_size_, options_.verbose);
      }
      if (kname.size() > max_name_size) {
        max_name_size = kname.size();
      }
      knames.push_back(kname);
    }

    if (total_time != 0) {
      std::stringstream stream;
      stream << std::setw(max_name_size) << "Kernel" << "," <<
        std::setw(kCallsLength) << "Calls" << "," <<
        std::setw(kTimeLength) << "Time (ns)" << "," <<
        std::setw(sizeof("Time (%)")) << "Time (%)" << "," <<
        std::setw(kTimeLength) << "Average (ns)" << "," <<
        std::setw(kTimeLength) << "Min (ns)" << "," <<
        std::setw(kTimeLength) << "Max (ns)" << std::endl;
  
      int i = 0;
      for (auto& it : sorted_list) {
        uint64_t call_count = it.second.call_count_;
        uint64_t time = it.second.execute_time_;
        uint64_t avg_time = time / call_count;
        uint64_t min_time = it.second.min_time_;
        uint64_t max_time = it.second.max_time_;
        float percent_time = 100.0f * time / total_time;
        stream << std::setw(max_name_size) << knames[i] << "," <<
          std::setw(kCallsLength) << call_count << "," <<
          std::setw(kTimeLength) << time << "," <<
          std::setw(sizeof("Time (%)")) << std::setprecision(2) <<
            std::fixed << percent_time << "," <<
          std::setw(kTimeLength) << avg_time << "," <<
          std::setw(kTimeLength) << min_time << "," <<
          std::setw(kTimeLength) << max_time << std::endl;
        i++;
      }
  
      
      stream << std::endl << std::endl << "=== Kernel Properties ===" << std::endl << std::endl;
      stream << std::setw(max_name_size) << "Kernel" << "," <<
        std::setw(sizeof("Number of Arguments")) << "Number of Arguments" << "," <<
        std::setw(sizeof("SLM Per Work Group")) << "SLM Per Work Group" << "," <<
        std::setw(sizeof("Private Memory Per Thread")) << "Private Memory Per Thread" << "," <<
        std::setw(sizeof("Spill Memory Per Thread")) << "Spill Memory Per Thread" << std::endl;
  
      i = -1; 
      for (auto& it : sorted_list) {
        ++i;
        auto kit = kernel_command_properties_->find(it.first.kernel_command_id_);
        if (kit == kernel_command_properties_->end()) {
          continue;
        }
        if (kit->second.type_ != KERNEL_COMMAND_TYPE_COMPUTE) {
          continue;
        }
        
        stream << std::setw(max_name_size) << knames[i] << "," <<
          std::setw(sizeof("Number of Arguments")) << kit->second.nargs_ <<
          std::setw(sizeof("SLM Per Work Group")) << kit->second.slmsize_ <<
          std::setw(sizeof("Private Memory Per Thread")) <<  kit->second.private_mem_size_ <<
          std::setw(sizeof("Spill Memory Per Thread")) << kit->second.spill_mem_size_ <<
          std::endl;
      }
      correlator_->Log(stream.str());
    }

    global_device_time_stats_mutex_.unlock();

  }

  void PrintSubmissionTable() const {
    uint64_t total_submit_time = 0;
    uint64_t total_append_time = 0;
    uint64_t total_device_time = 0;
    std::vector<std::string> knames;
    size_t max_name_size = 0;
    global_device_time_stats_mutex_.lock();

    AggregateDeviceTimeStats();

    std::set<std::pair<ZeKernelCommandNameKey, ZeKernelCommandTime>, utils::Comparator> sorted_list(
        global_device_time_stats_->begin(), global_device_time_stats_->end());

    for (auto& it : sorted_list) {
      total_device_time += it.second.execute_time_;
      total_append_time += it.second.append_time_;
      total_submit_time += it.second.submit_time_;
      std::string kname;
      if (it.first.tile_ >= 0) {
        kname = "Tile #" + std::to_string(it.first.tile_) + ": " + GetZeKernelCommandName(it.first.kernel_command_id_, it.first.group_count_, it.first.mem_size_, options_.verbose);
      }
      else {
        kname = GetZeKernelCommandName(it.first.kernel_command_id_, it.first.group_count_, it.first.mem_size_, options_.verbose);
      }
      if (kname.size() > max_name_size) {
        max_name_size = kname.size();
      }
      knames.push_back(kname);
    }

    if (total_device_time != 0) {

      std::stringstream stream;
      stream << std::setw(max_name_size) << "Kernel" << "," <<
        std::setw(kCallsLength) << "Calls" << "," <<
        std::setw(kTimeLength) << "Append (ns)" << "," <<
        std::setw(sizeof("Append (%)")) << "Append (%)" << "," <<
        std::setw(kTimeLength) << "Submit (ns)" << "," <<
        std::setw(sizeof("Submit (%)")) << "Submit (%)" << "," <<
        std::setw(kTimeLength) << "Execute (ns)" << "," <<
        std::setw(sizeof("Execute (%)")) << "Execute (%)" << "," <<
        std::endl;
  
      int i = 0;
      for (auto& it : sorted_list) {
        uint64_t call_count = it.second.call_count_;
        float append_percent = 100.0f * it.second.append_time_ / total_append_time;
        float submit_percent = 100.0f * it.second.submit_time_ / total_submit_time;
        float device_percent = 100.0f * it.second.execute_time_ / total_device_time;
        stream << std::setw(max_name_size) << knames[i] << "," <<
          std::setw(kCallsLength) << call_count << "," <<
          std::setw(kTimeLength) << it.second.append_time_ << "," <<
          std::setw(sizeof("Append (%)")) << std::setprecision(2) <<
          std::fixed << append_percent << "," <<
          std::setw(kTimeLength) << it.second.submit_time_ << "," <<
          std::setw(sizeof("Submit (%)")) << std::setprecision(2) <<
          std::fixed << submit_percent << "," <<
          std::setw(kTimeLength) << it.second.execute_time_ << "," <<
          std::setw(sizeof("Execute (%)")) << std::setprecision(2) <<
          std::fixed << device_percent << "," << std::endl;
        i++;
      }
      correlator_->Log(stream.str());
    }

    global_device_time_stats_mutex_.unlock();

  }

  void DisableTracing() {
    ze_result_t status = zelTracerSetEnabled(tracer_, false);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  }

  uint64_t CalculateTotalFunctionTime() const {
    global_host_time_stats_mutex_.lock();

    uint64_t total_time = 0;
    for (auto it = global_host_time_stats_->begin(); it != global_host_time_stats_->end(); it++) {
      total_time += it->second.total_time_;
    }

    global_host_time_stats_mutex_.unlock();
 
    return total_time;
  }

  void PrintFunctionsTable() const {
    global_host_time_stats_mutex_.lock();
    std::set<std::pair<uint32_t, ZeFunctionTime>, utils::Comparator> sorted_list(
        global_host_time_stats_->begin(), global_host_time_stats_->end());

    uint64_t total_time = 0;
    size_t max_name_size = 0;
    for (auto& stat : sorted_list) {
      total_time += stat.second.total_time_;
      if (get_symbol(API_TRACING_ID(stat.first)).size() > max_name_size) {
        max_name_size = get_symbol(API_TRACING_ID(stat.first)).size();
      }
    }

    if (total_time != 0) {
      std::stringstream stream;
      stream << std::setw(max_name_size) << "Function" << "," <<
        std::setw(kCallsLength) << "Calls" << "," <<
        std::setw(kTimeLength) << "Time (ns)" << "," <<
        std::setw(sizeof("Time (%)")) << "Time (%)" << "," <<
        std::setw(kTimeLength) << "Average (ns)" << "," <<
        std::setw(kTimeLength) << "Min (ns)" << "," <<
        std::setw(kTimeLength) << "Max (ns)" << std::endl;
  
      for (auto& stat : sorted_list) {
        const std::string function = get_symbol(API_TRACING_ID(stat.first));
        uint64_t time = stat.second.total_time_;
        uint64_t call_count = stat.second.call_count_;
        uint64_t avg_time = time / call_count;
        uint64_t min_time = stat.second.min_time_;
        uint64_t max_time = stat.second.max_time_;
        float percent_time = 100.0f * time / total_time;
        stream << std::setw(max_name_size) << function << "," <<
          std::setw(kCallsLength) << call_count << "," <<
          std::setw(kTimeLength) << time << "," <<
          std::setw(sizeof("Time (%)")) << std::setprecision(2) <<
          std::fixed << percent_time << "," <<
          std::setw(kTimeLength) << avg_time << "," <<
          std::setw(kTimeLength) << min_time << "," <<
          std::setw(kTimeLength) << max_time << std::endl;
      }
      correlator_->Log(stream.str());
    }
    global_host_time_stats_mutex_.unlock();
  }

  void ProcessCommandsSubmitted(std::vector<uint64_t> *kids) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }
    ze_result_t status = ZE_RESULT_SUCCESS;

    global_device_submissions_mutex_.lock_shared();
    auto it = local_device_submissions_.commands_submitted_.begin();
    while (it != local_device_submissions_.commands_submitted_.end()) {
      ZeCommand *command = *it;

      if (command->event_ != nullptr) {
        status = zeEventQueryStatus(command->event_);
        if (status == ZE_RESULT_SUCCESS) {
          ProcessCommandSubmitted(local_device_submissions_, command, kids);
          local_device_submissions_.commands_free_pool_.push_back(command);
          it = local_device_submissions_.commands_submitted_.erase(it);
          continue;
        }
      }
      ++it;
    }
    if (options_.metric_query) {
      ProcessCommandMetricQueriesSubmitted();
    }
    global_device_submissions_mutex_.unlock_shared();
  }

  void ProcessAllCommandsSubmitted(std::vector<uint64_t> *kids) {
    if (local_device_submissions_.IsFinalized()) {
      return;
    }
    ze_result_t status = ZE_RESULT_SUCCESS;

    global_device_submissions_mutex_.lock();
    if (global_device_submissions_) {
      for (auto s : *global_device_submissions_) {
        auto& local_device_submissions = *s;
        auto it = local_device_submissions.commands_submitted_.begin();
        while (it != local_device_submissions.commands_submitted_.end()) {
          ZeCommand *command = *it;
    
          if (command->event_ != nullptr) {
            status = zeEventQueryStatus(command->event_);
            if (status == ZE_RESULT_SUCCESS) {
              ProcessCommandSubmitted(local_device_submissions, command, kids);
              local_device_submissions_.commands_free_pool_.push_back(command);
              it = local_device_submissions.commands_submitted_.erase(it);
              continue;
            }
          }
          ++it;
        }
        if (options_.metric_query) {
          ProcessCommandMetricQueriesSubmitted();
        }
      }
    }
    global_device_submissions_mutex_.unlock();
  }

  void FinalizeDeviceSubmissions(std::vector<uint64_t> *kids) {
    ze_result_t status = ZE_RESULT_SUCCESS;

    // Do not acquire any locks!
    auto it = local_device_submissions_.commands_submitted_.begin();
    while (it != local_device_submissions_.commands_submitted_.end()) {
      ZeCommand *command = *it;

      if (command->event_ != nullptr) {
        status = zeEventQueryStatus(command->event_);
        if (status == ZE_RESULT_SUCCESS) {
          ProcessCommandSubmitted(local_device_submissions_, command, kids);
          local_device_submissions_.commands_free_pool_.push_back(command);
          it = local_device_submissions_.commands_submitted_.erase(it);
          continue;
        }
      }
      ++it;
    }
    if (options_.metric_query) {
      ProcessCommandMetricQueriesSubmitted();
    }
  }

 private: // Implementation

  ZeCollector(
      Correlator* correlator,
      CollectorOptions options,
      OnZeKernelFinishCallback kcallback,
      OnZeFunctionFinishCallback fcallback,
      void* callback_data,
      std::string& data_dir_name)
      : correlator_(correlator),
        options_(options),
        kcallback_(kcallback),
        fcallback_(fcallback),
        callback_data_(callback_data),
        event_cache_(ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP) {
    data_dir_name_ = data_dir_name;
    EnumerateAndSetupDevices();
    InitializeKernelCommandProperties();
  }

  void InitializeKernelCommandProperties(void) {
    if (active_command_properties_ == nullptr) {
      active_command_properties_ = new std::map<uint64_t, ZeKernelCommandProperties>;
      UniMemory::ExitIfOutOfMemory((void *)(active_command_properties_));
    }
    if (active_kernel_properties_ == nullptr) {
      active_kernel_properties_ = new std::map<ze_kernel_handle_t, ZeKernelCommandProperties>;
      UniMemory::ExitIfOutOfMemory((void *)(active_kernel_properties_));
    }
    if (kernel_command_properties_ == nullptr) {
      kernel_command_properties_ = new std::map<uint64_t, ZeKernelCommandProperties>;
      UniMemory::ExitIfOutOfMemory((void *)(kernel_command_properties_));
    }

    for (uint32_t i = 0; i <= uint32_t(ZeDeviceCommandHandle::LastCommand); i++) {
      ZeKernelCommandProperties desc;
      
      desc.name_ = device_command_names[i];
      desc.id_ = UniKernelId::GetKernelId();
      if (i <= uint32_t(ZeDeviceCommandHandle::Barrier)) {
        desc.type_ = KERNEL_COMMAND_TYPE_MEMORY;
      }
      else {
        desc.type_ = KERNEL_COMMAND_TYPE_COMMAND;
      }
      
      ZeKernelCommandProperties desc2;
      desc2 = desc;

      active_command_properties_->insert({uint64_t(i), std::move(desc)});
      kernel_command_properties_->insert({desc2.id_, std::move(desc2)});
    }
  }

  void EnumerateAndSetupDevices() {
    if (devices_ == nullptr) {
      devices_ = new std::map<ze_device_handle_t, ZeDevice>;
      UniMemory::ExitIfOutOfMemory((void *)(devices_));
    }

    ze_result_t status = ZE_RESULT_SUCCESS;
    uint32_t num_drivers = 0;
    status = zeDriverGet(&num_drivers, nullptr);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);
    if (num_drivers > 0) {
      int32_t did = 0;
      std::vector<ze_driver_handle_t> drivers(num_drivers);
      std::vector<ze_context_handle_t> contexts;
      status = zeDriverGet(&num_drivers, drivers.data());
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      for (auto driver : drivers) {
        ze_context_handle_t context = nullptr;
        if (options_.metric_query) {
          ze_context_desc_t cdesc = {ZE_STRUCTURE_TYPE_CONTEXT_DESC, nullptr, 0};

          status = zeContextCreate(driver, &cdesc, &context);
          PTI_ASSERT(status == ZE_RESULT_SUCCESS);
          metric_contexts_.push_back(context);
        }

        uint32_t num_devices = 0;
        status = zeDeviceGet(driver, &num_devices, nullptr);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        if (num_devices) {
          std::vector<ze_device_handle_t> devices(num_devices);
          status = zeDeviceGet(driver, &num_devices, devices.data());
          PTI_ASSERT(status == ZE_RESULT_SUCCESS);
          for (auto device : devices) {
            ZeDevice desc;
  
            desc.device_ = device;
            desc.id_ = did;
            desc.parent_id_ = -1;	// no parent
            desc.parent_device_ = nullptr;
            desc.subdevice_id_ = -1;	// not a subdevice
            desc.device_timer_frequency_ = utils::ze::GetDeviceTimerFrequency(device);
            desc.device_timer_mask_ = utils::ze::GetDeviceTimestampMask(device);
            desc.metric_timer_frequency_ = utils::ze::GetMetricTimerFrequency(device);
            desc.metric_timer_mask_ = utils::ze::GetMetricTimestampMask(device);

            ze_pci_ext_properties_t pci_device_properties;
            ze_result_t status = zeDevicePciGetPropertiesExt(device, &pci_device_properties);
            PTI_ASSERT(status == ZE_RESULT_SUCCESS);
            desc.pci_properties_ = pci_device_properties;

            desc.driver_ = driver;
            desc.context_ = context;

            uint32_t num_sub_devices = 0;
            status = zeDeviceGetSubDevices(device, &num_sub_devices, nullptr);
            PTI_ASSERT(status == ZE_RESULT_SUCCESS);

            desc.num_subdevices_ = num_sub_devices;

            if (options_.metric_query) {
              uint32_t num_groups = 0;
              zet_metric_group_handle_t group = nullptr;
              status = zetMetricGroupGet(device, &num_groups, nullptr);
              PTI_ASSERT(status == ZE_RESULT_SUCCESS);
              if (num_groups > 0) {
                std::vector<zet_metric_group_handle_t> groups(num_groups, nullptr);
                status = zetMetricGroupGet(device, &num_groups, groups.data());
                PTI_ASSERT(status == ZE_RESULT_SUCCESS);

                for (uint32_t k = 0; k < num_groups; ++k) {
                  zet_metric_group_properties_t group_props{};
                  group_props.stype = ZET_STRUCTURE_TYPE_METRIC_GROUP_PROPERTIES;
                  status = zetMetricGroupGetProperties(groups[k], &group_props);
                  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

                  
                  if ((strcmp(group_props.name, utils::GetEnv("UNITRACE_MetricGroup").c_str()) == 0) && (group_props.samplingType & ZET_METRIC_GROUP_SAMPLING_TYPE_FLAG_EVENT_BASED)) {
                    group = groups[k];
                    break;
                  }
                }
              }

              status = zetContextActivateMetricGroups(context, device, 1, &group);
              PTI_ASSERT(status == ZE_RESULT_SUCCESS);
              metric_activations_.insert({context, device});

              desc.metric_group_ = group;
            }
            else {
              desc.metric_group_ = nullptr;
            }

            uint64_t host_time;
            uint64_t ticks;

            status = zeDeviceGetGlobalTimestamps(device, &host_time, &ticks);
            PTI_ASSERT(status == ZE_RESULT_SUCCESS);

            desc.host_time_origin_ = host_time;

            devices_->insert({device, std::move(desc)});

            if (num_sub_devices > 0) {
              std::vector<ze_device_handle_t> sub_devices(num_sub_devices);

              status = zeDeviceGetSubDevices(device, &num_sub_devices, sub_devices.data());
              PTI_ASSERT(status == ZE_RESULT_SUCCESS);

              for (int j = 0; j < num_sub_devices; j++) {
                ZeDevice sub_desc;
  
                sub_desc.device_ = sub_devices[j];
                sub_desc.parent_id_ = did;
                sub_desc.parent_device_ = device;
                sub_desc.num_subdevices_ = 0;
                sub_desc.subdevice_id_ = j;
                sub_desc.id_ = did;	// take parent device's id
                sub_desc.device_timer_frequency_ = utils::ze::GetDeviceTimerFrequency(sub_devices[j]);
                sub_desc.device_timer_mask_ = utils::ze::GetDeviceTimestampMask(sub_devices[j]);
                sub_desc.metric_timer_frequency_ = utils::ze::GetMetricTimerFrequency(sub_devices[j]);
                sub_desc.metric_timer_mask_ = utils::ze::GetMetricTimestampMask(sub_devices[j]);
  
                ze_pci_ext_properties_t pci_device_properties;
                ze_result_t status = zeDevicePciGetPropertiesExt(sub_devices[j], &pci_device_properties);
                PTI_ASSERT(status == ZE_RESULT_SUCCESS);
                sub_desc.pci_properties_ = pci_device_properties;
  
                uint64_t ticks;
                uint64_t host_time;
                status = zeDeviceGetGlobalTimestamps(sub_devices[j], &host_time, &ticks);
                PTI_ASSERT(status == ZE_RESULT_SUCCESS);

                sub_desc.host_time_origin_ = host_time;
  
                sub_desc.driver_ = driver;
                sub_desc.context_ = context;
            
                sub_desc.metric_group_ = nullptr;

                devices_->insert({sub_devices[j], std::move(sub_desc)});
              }
            }
            did++;
          }
        }
      }
    }
  }

  static void PrintTypedValue(std::stringstream& stream, const zet_typed_value_t& typed_value) {
    switch (typed_value.type) {
      case ZET_VALUE_TYPE_UINT32:
        stream << typed_value.value.ui32;
        break;
      case ZET_VALUE_TYPE_UINT64:
        stream << typed_value.value.ui64;
        break;
      case ZET_VALUE_TYPE_FLOAT32:
        stream << typed_value.value.fp32;
        break;
      case ZET_VALUE_TYPE_FLOAT64:
        stream << typed_value.value.fp64;
        break;
      case ZET_VALUE_TYPE_BOOL8:
        stream << static_cast<uint32_t>(typed_value.value.b8);
        break;
      default:
        PTI_ASSERT(0);
        break;
    }
  }

  inline static std::string GetMetricUnits(const char* units) {
    PTI_ASSERT(units != nullptr);

    std::string result = units;
    if (result.find("null") != std::string::npos) {
      result = "";
    } else if (result.find("percent") != std::string::npos) {
      result = "%";
    }

    return result;
  }

  static uint32_t GetMetricCount(zet_metric_group_handle_t group) {
    PTI_ASSERT(group != nullptr);

    zet_metric_group_properties_t group_props{};
    group_props.stype = ZET_STRUCTURE_TYPE_METRIC_GROUP_PROPERTIES;
    ze_result_t status = zetMetricGroupGetProperties(group, &group_props);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);

    return group_props.metricCount;
  }

  static std::vector<std::string> GetMetricNames(zet_metric_group_handle_t group) {
    PTI_ASSERT(group != nullptr);

    uint32_t metric_count = GetMetricCount(group);
    PTI_ASSERT(metric_count > 0);

    std::vector<zet_metric_handle_t> metrics(metric_count);
    ze_result_t status = zetMetricGet(group, &metric_count, metrics.data());
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);
    PTI_ASSERT(metric_count == metrics.size());

    std::vector<std::string> names;
    for (auto metric : metrics) {
      zet_metric_properties_t metric_props{
          ZET_STRUCTURE_TYPE_METRIC_PROPERTIES, };
      status = zetMetricGetProperties(metric, &metric_props);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);

      std::string units = GetMetricUnits(metric_props.resultUnits);
      std::string name = metric_props.name;
      if (!units.empty()) {
        name += "[" + units + "]";
      }
      names.push_back(name);
    }

    return names;
  }

  bool QueryKernelCommandMetrics(ZeDeviceSubmissions& submissions, ZeCommandMetricQuery *command_metric_query) {

    ze_result_t status;
    if ((status = zeEventQueryStatus(command_metric_query->metric_query_event_)) == ZE_RESULT_SUCCESS) {

      auto it = submissions.kernel_profiles_.find(command_metric_query->instance_id_);
      if (it != submissions.kernel_profiles_.end()) {
        size_t size = 0;
        status = zetMetricQueryGetData(command_metric_query->metric_query_, &size, nullptr);
        if ((status == ZE_RESULT_SUCCESS) && (size > 0)) {

          std::vector<uint8_t> *kmetrics = new std::vector<uint8_t>(size);
          UniMemory::ExitIfOutOfMemory((void *)(kmetrics));
          size_t size2 = size;
          status = zetMetricQueryGetData(command_metric_query->metric_query_, &size2, kmetrics->data());
          if (size2 == size) {
            it->second.metrics_ = kmetrics;
          }
          else {
            delete kmetrics;
          }
        }
      }
      event_cache_.ResetEvent(command_metric_query->metric_query_event_);
      query_pools_.ResetQuery(command_metric_query->metric_query_);
      if (command_metric_query->immediate_) {
        event_cache_.ReleaseEvent(command_metric_query->metric_query_event_);
        query_pools_.PutQuery(command_metric_query->metric_query_);
      }
      command_metric_query->metric_query_event_ = nullptr;
      command_metric_query->metric_query_ = nullptr;

      return true;
    }
    else {
      return false;
    }
  }

  void ProcessCommandMetricQueriesSubmitted(void) {
    for (auto it = local_device_submissions_.metric_queries_submitted_.begin(); it != local_device_submissions_.metric_queries_submitted_.end();) {
      if (QueryKernelCommandMetrics(local_device_submissions_, *it)) {
        local_device_submissions_.metric_queries_free_pool_.push_back(*it);
        it = local_device_submissions_.metric_queries_submitted_.erase(it);
      }
      else {
        it++;
      }
    }
  }

  void DumpKernelProfiles(void) {
    ze_device_handle_t device = nullptr;
    int did = -1;
    zet_metric_group_handle_t group = nullptr;
    std::vector<std::string> metric_names;

    if (options_.stall_sampling) {
      kernel_command_properties_mutex_.lock();
      std::map<int32_t, std::map<uint64_t, ZeKernelCommandProperties *>> device_kprops; // sorted by device id then base address;
      for (auto it = kernel_command_properties_->begin(); it != kernel_command_properties_->end(); it++) {
        if (it->second.type_ != KERNEL_COMMAND_TYPE_COMPUTE) {
          continue;
        }
        auto dkit = device_kprops.find(it->second.device_id_);
        if (dkit == device_kprops.end()) {
          std::map<uint64_t, ZeKernelCommandProperties *> kprops;
          kprops.insert({it->second.base_addr_, &(it->second)});
          device_kprops.insert({it->second.device_id_, std::move(kprops)});
        }
        else {
          if (dkit->second.find(it->second.base_addr_) != dkit->second.end()) {
            // already inserted
            continue;
          }
          dkit->second.insert({it->second.base_addr_, &(it->second)});
        }
      }

      for (auto& props : device_kprops) {
        // kernel properties file path: data_dir/.kprops.<device_id>.<pid>.txt
        std::string fpath = data_dir_name_ + "/.kprops."  + std::to_string(props.first) + "." + std::to_string(utils::GetPid()) + ".txt";
        std::ofstream kpfs = std::ofstream(fpath, std::ios::out | std::ios::trunc);
        uint64_t prev_base = 0;
        for (auto it = props.second.crbegin(); it != props.second.crend(); it++) {
          kpfs << utils::Demangle(it->second->name_.c_str()) << std::endl;
          kpfs << it->second->base_addr_ << std::endl;
          if (prev_base == 0) {
            kpfs << it->second->size_ << std::endl;
          }
          else {
            size_t size = prev_base - it->second->base_addr_;
            if (size > it->second->size_) {
              size = it->second->size_;
            }
            kpfs << size << std::endl;
          }
          prev_base = it->second->base_addr_;
        }
        kpfs.close();
      }

      kernel_command_properties_mutex_.unlock();
    }
    
    const std::lock_guard<std::mutex> lock(global_kernel_profiles_mutex_);
    if (global_kernel_profiles_.size() == 0) {
      return;
    }
    
    if (options_.metric_stream) {
      devices_mutex_.lock_shared();
      std::map<int32_t, std::vector<ZeKernelProfileRecord *>> device_kprofiles; // kernel profiles by device;
      for (auto it = global_kernel_profiles_.begin(); it != global_kernel_profiles_.end(); it++) {
        int32_t device_id = -1;
        auto dit = devices_->find(it->second.device_);
        if (dit != devices_->end()) {
          device_id = dit->second.id_;
        }
        if (device_id == -1) {
          continue;
        }
        auto dpit = device_kprofiles.find(device_id);
        if (dpit == device_kprofiles.end()) {
          std::vector<ZeKernelProfileRecord *> kprofiles;
          kprofiles.push_back(&(it->second));
          device_kprofiles.insert({device_id, std::move(kprofiles)});
        }
        else {
          dpit->second.push_back(&(it->second));
        }
      }
      devices_mutex_.unlock_shared();

      for (auto& profiles : device_kprofiles) {
        std::ofstream ouf;
        // kernel instance time file path: <data_dir>/.ktime.<device_id>.<pid>.txt
        std::string fpath = data_dir_name_ + "/.ktime."  + std::to_string(profiles.first) + "." + std::to_string(utils::GetPid()) + ".txt";
        ouf = std::ofstream(fpath, std::ios::out | std::ios::trunc);
        for (auto& prof : profiles.second) {
          for (auto& ts : prof->timestamps_) {
            std::string kname = GetZeKernelCommandName(prof->kernel_command_id_, prof->group_count_, prof->mem_size_);
            ouf << ts.subdevice_id << std::endl;
            ouf << ts.metric_start << std::endl;
            ouf << ts.metric_end << std::endl;
            ouf << kname << std::endl;
          }
        }
        ouf.close();
      }

      return;
    }

    if (!options_.metric_query) {
      return;
    }

    // metric query
    correlator_->Log("\n== Kernel Metrics ==\n\n");

    for (auto it = global_kernel_profiles_.begin(); it != global_kernel_profiles_.end(); it++) {
      if ((it->second.metrics_ == nullptr) || it->second.metrics_->empty()) {
        continue;
      }

      std::string kname = GetZeKernelCommandName(it->second.kernel_command_id_, it->second.group_count_, it->second.mem_size_);
      if (device != it->second.device_) {
        device = it->second.device_;
        auto it2 = devices_->find(it->second.device_);
        
        if (it2 == devices_->end()) {
          // should never get here
          continue;
        }

        did = it2->second.id_;
        group = it2->second.metric_group_;
        metric_names = GetMetricNames(it2->second.metric_group_);
        PTI_ASSERT(!metric_names.empty());
      }

      uint32_t num_samples = 0;
      uint32_t num_metrics = 0;
      ze_result_t status = zetMetricGroupCalculateMultipleMetricValuesExp(
        group, ZET_METRIC_GROUP_CALCULATION_TYPE_METRIC_VALUES,
        it->second.metrics_->size(), it->second.metrics_->data(), &num_samples, &num_metrics,
        nullptr, nullptr);

      if ((status == ZE_RESULT_SUCCESS) && (num_samples > 0) && (num_metrics > 0)) {
        std::vector<uint32_t> samples(num_samples);
        std::vector<zet_typed_value_t> metrics(num_metrics);

        status = zetMetricGroupCalculateMultipleMetricValuesExp(
          group, ZET_METRIC_GROUP_CALCULATION_TYPE_METRIC_VALUES,
          it->second.metrics_->size(), it->second.metrics_->data(), &num_samples, &num_metrics,
          samples.data(), metrics.data());

        if (status == ZE_RESULT_SUCCESS) {
          std::stringstream header;
          header << std::endl;
          header << "Kernel,";
          header << "Device,";
          header << "SubDeviceId,";
          for (auto& metric : metric_names) {
            header << metric << ",";
          }
          header << std::endl;
          correlator_->Log(header.str());
    
          std::stringstream stream;
          for (uint32_t i = 0; i < num_samples; ++i) {
            stream << kname << ",";
            stream << did << ",";
            stream << i << ",";
    
            uint32_t size = samples[i];
            PTI_ASSERT(size == metric_names.size());
    
            const zet_typed_value_t *value = metrics.data() + i * size;
            for (int j = 0; j < size; ++j) {
              PrintTypedValue(stream, value[j]);
              stream << ",";
            }
            stream << std::endl;
          }
    
          correlator_->Log(stream.str());
        }
        else {
          std::cerr << "[WARNING] Not able to calculate metrics" << std::endl;
        }
      }
      else {
        std::cerr << "[WARNING] Not able to calculate metrics" << std::endl;
      }
    }

    global_kernel_profiles_.clear();
  }

  void ProcessCommandsSubmittedOnSignaledEvent(ze_event_handle_t event, std::vector<uint64_t> *kids) {
    if (local_device_submissions_.IsFinalized()) {
      return;
    }
    global_device_submissions_mutex_.lock_shared();
    for (auto it = local_device_submissions_.commands_submitted_.begin(); it != local_device_submissions_.commands_submitted_.end();) {
      ZeCommand *command = *it;

      if (command->event_ == event) {
        ProcessCommandSubmitted(local_device_submissions_, command, kids);
        if (command->immediate_) {
          event_cache_.ReleaseEvent(command->event_);
        }
        else {
          event_cache_.ResetEvent(command->event_);
        }
        local_device_submissions_.commands_free_pool_.push_back(command);
        it = local_device_submissions_.commands_submitted_.erase(it);
        continue;
      }
      else {
        if (zeEventQueryStatus(command->event_) == ZE_RESULT_SUCCESS) {
          ProcessCommandSubmitted(local_device_submissions_, command, nullptr);

          if (command->immediate_) {
            event_cache_.ReleaseEvent(command->event_);
          }
          else {
            event_cache_.ResetEvent(command->event_);
          }
          local_device_submissions_.commands_free_pool_.push_back(command);
          it = local_device_submissions_.commands_submitted_.erase(it);
          continue;
        }
      }
      it++;
    }

    if (options_.metric_query) {
      ProcessCommandMetricQueriesSubmitted();
    }
    global_device_submissions_mutex_.unlock_shared();
  }

  void ProcessCommandsSubmittedOnFenceSynchronization(ze_fence_handle_t fence, std::vector<uint64_t> *kids) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    global_device_submissions_mutex_.lock_shared();
    for (auto it = local_device_submissions_.commands_submitted_.begin(); it != local_device_submissions_.commands_submitted_.end();) {
      ZeCommand *command = *it;
      if ((command->fence_ != nullptr) && (command->fence_ == fence)) {
        ProcessCommandSubmitted(local_device_submissions_, command, kids);
        if (command->immediate_) {
          event_cache_.ReleaseEvent(command->event_);
        }
        else {
          event_cache_.ResetEvent(command->event_);
        }
        local_device_submissions_.commands_free_pool_.push_back(command);
        it = local_device_submissions_.commands_submitted_.erase(it);
        continue;
      }
      else {
        if ((command->event_ != nullptr) && (zeEventQueryStatus(command->event_) == ZE_RESULT_SUCCESS)) {
          ProcessCommandSubmitted(local_device_submissions_, command, nullptr);
          if (command->immediate_) {
            event_cache_.ReleaseEvent(command->event_);
          }
          else {
            event_cache_.ResetEvent(command->event_);
          }
          local_device_submissions_.commands_free_pool_.push_back(command);
          it = local_device_submissions_.commands_submitted_.erase(it);
          continue;
        }
      }
      it++;
    }
    if (options_.metric_query) {
      ProcessCommandMetricQueriesSubmitted();
    }
    global_device_submissions_mutex_.unlock_shared();
  }

  inline uint64_t ComputeDuration(uint64_t start, uint64_t end, uint64_t freq, uint64_t mask) {
    uint64_t duration = 0;
    if (start <= end) {
      duration = (end - start) * static_cast<uint64_t>(NSEC_IN_SEC) / freq;
    } else { // Timer Overflow
      duration = ((mask + 1ull) + end - start) * static_cast<uint64_t>(NSEC_IN_SEC) / freq;
    }
    return duration;
  }

  inline void GetHostTime(const ZeCommand *command, const ze_kernel_timestamp_result_t& ts, uint64_t& start, uint64_t& end) {
    uint64_t device_freq = command->device_timer_frequency_;
    uint64_t device_mask = command->device_timer_mask_;
    uint64_t tspan = (device_mask + 1ull) * NSEC_IN_SEC / device_freq;	// time span of universe

    uint64_t device_start = ts.global.kernelStart & device_mask;
    uint64_t device_end = ts.global.kernelEnd & device_mask;

    uint64_t start_ns = (device_start * NSEC_IN_SEC / device_freq);
    uint64_t device_submit_time = command->submit_time_device_;

    int64_t time_shift = start_ns - device_submit_time;

    if (start_ns > device_submit_time) {
      uint64_t diff = device_submit_time + tspan - start_ns;
      if (diff < time_shift) {
        // overflow
        time_shift = -diff;
      }
    }
    else {
      uint64_t diff = start_ns + tspan - device_submit_time;
      if (diff < (-time_shift)) {
        // overflow
        time_shift = diff;
      }
    }

    int64_t duration = ComputeDuration(device_start, device_end, device_freq, device_mask);

    start = command->submit_time_ + time_shift;
    end = start + duration;
  }

  void PrintCommandCompleted(const ZeCommand *command, uint64_t kernel_start, uint64_t kernel_end) {
    std::stringstream stream;
    stream << "Thread " << command->tid_ << " Device " << command->device_ <<
      " : " << GetZeKernelCommandName(command->kernel_command_id_, command->group_count_, command->mem_size_) << " [ns] " <<
      command->append_time_ << " (append) " <<
      command->submit_time_ << " (submit) " <<
      kernel_start << " (start) " <<
      kernel_end << " (end)" << std::endl;
    correlator_->Log(stream.str());
  }

  inline void LogCommandCompleted(const ZeCommand *command, const ze_kernel_timestamp_result_t& timestamp, int tile) {

    uint64_t kernel_start = 0, kernel_end = 0;
    GetHostTime(command, timestamp, kernel_start, kernel_end);
    PTI_ASSERT(kernel_start <= kernel_end);

    if (options_.device_timing || options_.kernel_submission) {
      local_device_submissions_.CollectKernelCommandTimeStats(command, kernel_start, kernel_end, tile);
    }

    if (options_.device_timeline) {
      PrintCommandCompleted(command, kernel_start, kernel_end);
    }

    if (kcallback_) {

      bool implicit_scaling = ((tile >= 0) && command->implicit_scaling_);
      
      kcallback_(command->instance_id_, command->tid_, kernel_start, kernel_end, command->engine_ordinal_, command->engine_index_, tile, command->device_, command->kernel_command_id_, implicit_scaling, command->group_count_, command->mem_size_);
    }
  }

  inline void ProcessCommandSubmitted(ZeDeviceSubmissions& submissions, ZeCommand *command, std::vector<uint64_t> *kids) {

    if (kids) {
        kids->push_back(command->instance_id_);
    }

    ze_kernel_timestamp_result_t timestamp{};
    ze_result_t status = zeEventQueryKernelTimestamp(command->event_, &timestamp);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);

    uint64_t device_freq = command->device_timer_frequency_;
    uint64_t device_mask = command->device_timer_mask_;

    uint64_t metric_freq = command->metric_timer_frequency_;
    uint64_t metric_mask = command->metric_timer_mask_;

    ZeKernelProfileRecord r;
    
    if (options_.metric_query || options_.metric_stream) {
      r.device_ = command->device_;
      r.instance_id_ = command->instance_id_;
      r.kernel_command_id_ = command->kernel_command_id_;
      r.group_count_ = command->group_count_;
      r.mem_size_ = command->mem_size_;
    }
      

    if (options_.kernels_per_tile && (command->type_ == KERNEL_COMMAND_TYPE_COMPUTE)) {
      if (command->implicit_scaling_) { // Implicit Scaling
        uint32_t count = 0;
        ze_result_t status = zeEventQueryTimestampsExp(command->event_, command->device_, &count, nullptr);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        PTI_ASSERT(count > 0);

        std::vector<ze_kernel_timestamp_result_t> timestamps(count);
        status = zeEventQueryTimestampsExp(command->event_, command->device_, &count, timestamps.data());
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);

        if (options_.metric_query || options_.metric_stream) {
          for (int i = 0; i < count; i++) {
            ZeKernelProfileTimestamps ts;

            ts.subdevice_id = i;
            
            ts.device_start = timestamps[i].global.kernelStart & device_mask;
            ts.device_start = (ts.device_start * NSEC_IN_SEC / device_freq);
  
            ts.metric_start = timestamps[i].global.kernelStart & metric_mask;
            ts.metric_start = (ts.metric_start * NSEC_IN_SEC / metric_freq);
            
            ts.device_end = timestamps[i].global.kernelEnd & device_mask;
            ts.device_end = (ts.device_end * NSEC_IN_SEC / device_freq);
  
            ts.metric_end = timestamps[i].global.kernelEnd & metric_mask;
            ts.metric_end = (ts.metric_end * NSEC_IN_SEC / metric_freq);
      
            r.timestamps_.push_back(std::move(ts));
          }
          
          submissions.kernel_profiles_.insert({command->instance_id_, std::move(r)});
        }

        if (count == 1) { // First tile is used only
          LogCommandCompleted(command, timestamps[0], 0);
        } else {
          for (uint32_t i = 0; i < count; ++i) {
            LogCommandCompleted(command, timestamps[i], static_cast<int>(i));
          }
        }
      } else { // Explicit Scaling
        auto it = devices_->find(command->device_);
        if (it != devices_->end()) {
          LogCommandCompleted(command, timestamp, -1);
        }

        if (options_.metric_query || options_.metric_stream) {
          ZeKernelProfileTimestamps ts;

          ts.device_start = timestamp.global.kernelStart & device_mask;
          ts.device_start = (ts.device_start * NSEC_IN_SEC / device_freq);
  
          ts.metric_start = timestamp.global.kernelStart & metric_mask;
          ts.metric_start = (ts.metric_start * NSEC_IN_SEC / metric_freq);
            
          ts.device_end = timestamp.global.kernelEnd & device_mask;
          ts.device_end = (ts.device_end * NSEC_IN_SEC / device_freq);
  
          ts.metric_end = timestamp.global.kernelEnd & metric_mask;
          ts.metric_end = (ts.metric_end * NSEC_IN_SEC / metric_freq);
  
          ts.subdevice_id = -1;
      
  
          r.timestamps_.push_back(std::move(ts));
            
          submissions.kernel_profiles_.insert({command->instance_id_, std::move(r)});
        }
      }
    } else {
      if (options_.metric_query || options_.metric_stream) {
        ZeKernelProfileTimestamps ts;

        ts.device_start = timestamp.global.kernelStart & device_mask;
        ts.device_start = (ts.device_start * NSEC_IN_SEC / device_freq);
  
        ts.metric_start = timestamp.global.kernelStart & metric_mask;
        ts.metric_start = (ts.metric_start * NSEC_IN_SEC / metric_freq);
            
        ts.device_end = timestamp.global.kernelEnd & device_mask;
        ts.device_end = (ts.device_end * NSEC_IN_SEC / device_freq);
  
        ts.metric_end = timestamp.global.kernelEnd & metric_mask;
        ts.metric_end = (ts.metric_end * NSEC_IN_SEC / metric_freq);
      
        ts.subdevice_id = -1;
        r.timestamps_.push_back(std::move(ts));
            
        submissions.kernel_profiles_.insert({command->instance_id_, std::move(r)});
      }

      LogCommandCompleted(command, timestamp, -1);
    }

    if (command->immediate_) {
      event_cache_.ReleaseEvent(command->event_);
    }
    else {
      event_cache_.ResetEvent(command->event_);
    }
    command->event_ = nullptr;
  }

  void CreateCommandList(
    ze_command_list_handle_t command_list,
    ze_context_handle_t context,
    ze_device_handle_t device,
    uint32_t ordinal,
    uint32_t index,
    bool immediate) {

    
    ZeCommandList *desc;

    command_lists_mutex_.lock();
    auto it = command_lists_.find(command_list);
    if (it != command_lists_.end()) {
      desc = it->second;
      command_lists_.erase(it);
    }
    else {
      desc = new ZeCommandList;
      UniMemory::ExitIfOutOfMemory((void *)(desc));
    }
    command_lists_mutex_.unlock();

    desc->cmdlist_ = command_list;
    desc->context_ = context;
    desc->device_ = device;
    desc->immediate_ = immediate;
    desc->engine_ordinal_ = ordinal;	// valid if immediate command list
    desc->engine_index_ = index;;	// valid if immediate command list

    devices_mutex_.lock_shared();
    auto it2 = devices_->find(device);
    if (it2 != devices_->end()) {
      desc->host_time_origin_ = it2->second.host_time_origin_;
      desc->device_timer_frequency_ = it2->second.device_timer_frequency_;
      desc->device_timer_mask_ = it2->second.device_timer_mask_;
      desc->metric_timer_frequency_ = it2->second.metric_timer_frequency_;
      desc->metric_timer_mask_ = it2->second.metric_timer_mask_;
      desc->implicit_scaling_ = (it2->second.num_subdevices_ != 0);
    }
    devices_mutex_.unlock_shared();

    command_lists_mutex_.lock();
    command_lists_.insert({command_list, desc});
    command_lists_mutex_.unlock();
  }

  void DestroyCommandList(ze_command_list_handle_t command_list) {

    command_lists_mutex_.lock();
    
    auto it = command_lists_.find(command_list);
    if (it != command_lists_.end()) {
      if (!it->second->immediate_) {
        for (auto& command : it->second->commands_) {
          if (command->event_) {
            event_cache_.ReleaseEvent(command->event_);
          }
        }
        it->second->commands_.clear();
      }
      command_lists_.erase(it);
    }

    command_lists_mutex_.unlock();
  }

  void ResetCommandList(ze_command_list_handle_t command_list) {

    command_lists_mutex_.lock();
    
    auto it = command_lists_.find(command_list);
    if (it != command_lists_.end()) {
      if (!it->second->immediate_) {
        for (auto& command : it->second->commands_) {
          if (command->event_) {
            event_cache_.ReleaseEvent(command->event_);
          }
        }
        it->second->commands_.clear();
      }
    }

    command_lists_mutex_.unlock();
  }

  void ExecuteCommandLists(
    ze_command_list_handle_t *cmdlists, uint32_t count,
    ze_command_queue_handle_t queue, ze_fence_handle_t fence, const uint64_t ts,
    std::vector<uint64_t> *kids) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    command_queues_mutex_.lock_shared();
    auto qit = command_queues_.find(queue);
    if (qit != command_queues_.end()) {
      command_lists_mutex_.lock_shared();

      for (uint32_t i = 0; i < count; i++) {
        ze_command_list_handle_t cmdlist = cmdlists[i];
        
        auto it = command_lists_.find(cmdlist);
        
        if (it == command_lists_.end()) {
          std::cerr << "[ERROR] Command list (" << cmdlist << ") is not found to execute." << std::endl;
          continue;
        }

        if (!it->second->immediate_) {
          for (auto command : it->second->commands_) {
            ZeCommand *cmd = nullptr;
            ZeCommandMetricQuery *cmd_query = nullptr;
            
            cmd = local_device_submissions_.GetKernelCommand();

            if (command->command_metric_query_ != nullptr) {
              cmd_query = local_device_submissions_.GetCommandMetricQuery();
            }
            *cmd = *command;

            uint64_t host_timestamp;
            uint64_t ticks;
            uint64_t device_timestamp;
            ze_result_t status;

            status = zeDeviceGetGlobalTimestamps(cmd->device_, &host_timestamp, &ticks);
            PTI_ASSERT(status == ZE_RESULT_SUCCESS);

            device_timestamp = ticks & cmd->device_timer_mask_;
            device_timestamp = device_timestamp * NSEC_IN_SEC / cmd->device_timer_frequency_;

            cmd->engine_ordinal_ = qit->second.engine_ordinal_;
            cmd->engine_index_ = qit->second.engine_index_;
            cmd->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
            cmd->submit_time_ = host_timestamp;		//in ns
            cmd->submit_time_device_ = device_timestamp;//in ns
            cmd->tid_ = utils::GetTid();;
            cmd->fence_ = fence;
            event_cache_.ResetEvent(cmd->event_);
            local_device_submissions_.SubmitKernelCommand(cmd);

            if (kids) {
              kids->push_back(cmd->instance_id_);
            }

            if (cmd_query) {
              *cmd_query = *(command->command_metric_query_);
              cmd_query->instance_id_ = cmd->instance_id_;
              event_cache_.ResetEvent(cmd_query->metric_query_event_);
              local_device_submissions_.SubmitCommandMetricQuery(cmd_query);
            }
          }
        }
      }
      command_lists_mutex_.unlock_shared();
    }
    command_queues_mutex_.unlock_shared();
  }

  void CreateImage(ze_image_handle_t image, size_t size) {
    images_mutex_.lock();
    if (images_.find(image) != images_.end()) {
      images_.erase(image);
    }
    images_.insert({image, size});
    images_mutex_.unlock();
  }

  void DestroyImage(ze_image_handle_t image) {
    images_mutex_.lock();
    images_.erase(image);
    images_mutex_.unlock();
  }

  size_t GetImageSize(ze_image_handle_t image) {
    size_t size;

    images_mutex_.lock_shared();
    auto it = images_.find(image);
    if (it != images_.end()) {
      size = it->second;
    }
    else {
      size = 0;
    }
    images_mutex_.unlock_shared();

    return size;
  }

 private: // Callbacks

  static void OnEnterEventPoolCreate(ze_event_pool_create_params_t *params, ze_result_t result, void *global_data, void **instance_data) {
    const ze_event_pool_desc_t* desc = *(params->pdesc);
    if (desc == nullptr) {
      return;
    }
    if (desc->flags & ZE_EVENT_POOL_FLAG_IPC) {
      return;
    }

    ze_event_pool_desc_t* profiling_desc = new ze_event_pool_desc_t;
    UniMemory::ExitIfOutOfMemory((void *)(profiling_desc));
    PTI_ASSERT(profiling_desc != nullptr);
    profiling_desc->stype = desc->stype;
    profiling_desc->pNext = desc->pNext;
    profiling_desc->flags = desc->flags;
    profiling_desc->flags |= ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP;
    profiling_desc->flags |= ZE_EVENT_POOL_FLAG_HOST_VISIBLE;
    profiling_desc->count = desc->count;
    
    *(params->pdesc) = profiling_desc;
    *instance_data = profiling_desc;
  }

  static void OnExitEventPoolCreate(ze_event_pool_create_params_t *params,
                                    ze_result_t result,
                                    void *global_data,
                                    void **instance_data) {
    ze_event_pool_desc_t* desc = static_cast<ze_event_pool_desc_t*>(*instance_data);
    if (desc != nullptr) {
      delete desc;
    }
  }

  static void OnEnterEventDestroy(
      ze_event_destroy_params_t *params,
      ze_result_t result, void *global_data, void **instance_data, std::vector<uint64_t> *kids) {

    if (*(params->phEvent) != nullptr) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      if (zeEventQueryStatus(*(params->phEvent)) == ZE_RESULT_SUCCESS) {
        collector->ProcessCommandsSubmittedOnSignaledEvent(*(params->phEvent), kids);
      }
    }
  }

  static void OnEnterEventHostReset(
      ze_event_host_reset_params_t *params, ze_result_t result,
      void *global_data, void **instance_data, std::vector<uint64_t> *kids) {
    if (*(params->phEvent) != nullptr) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      if (zeEventQueryStatus(*(params->phEvent)) == ZE_RESULT_SUCCESS) {
        collector->ProcessCommandsSubmittedOnSignaledEvent(*(params->phEvent), kids);
      }
    }
  }

  static void OnExitEventHostSynchronize(
      ze_event_host_synchronize_params_t *params,
      ze_result_t result, void *global_data, void **instance_data, std::vector<uint64_t> *kids) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessCommandsSubmittedOnSignaledEvent(*(params->phEvent), kids);
    }
  }

  static void OnExitEventQueryStatus(
      ze_event_query_status_params_t *params,
      ze_result_t result, void *global_data, void **instance_data, std::vector<uint64_t> *kids) {
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(*(params->phEvent) != nullptr);
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      if (zeEventQueryStatus(*(params->phEvent)) == ZE_RESULT_SUCCESS) {
        collector->ProcessCommandsSubmittedOnSignaledEvent(*(params->phEvent), kids);
      }
    }
  }

  static void OnExitFenceHostSynchronize(
      ze_fence_host_synchronize_params_t *params,
      ze_result_t result, void *global_data, void **instance_data, std::vector<uint64_t> *kids) {
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(*(params->phFence) != nullptr);
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessCommandsSubmittedOnFenceSynchronization(*(params->phFence), kids);
    }
  }

  static void OnExitImageCreate(
      ze_image_create_params_t *params, ze_result_t result,
      void *global_data, void **instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

      ze_image_desc_t image_desc = **(params->pdesc);
      size_t image_size = image_desc.width;
      switch(image_desc.type) {
        case ZE_IMAGE_TYPE_2D:
        case ZE_IMAGE_TYPE_2DARRAY:
          image_size *= image_desc.height;
          break;
        case ZE_IMAGE_TYPE_3D:
          image_size *= image_desc.height * image_desc.depth;
          break;
        default:
          break;
      }

      switch(image_desc.format.type) {
        case ZE_IMAGE_FORMAT_TYPE_UINT:
        case ZE_IMAGE_FORMAT_TYPE_UNORM:
        case ZE_IMAGE_FORMAT_TYPE_FORCE_UINT32:
          image_size *= sizeof(unsigned int);
          break;
        case ZE_IMAGE_FORMAT_TYPE_SINT:
        case ZE_IMAGE_FORMAT_TYPE_SNORM:
          image_size *= sizeof(int);
          break;
        case ZE_IMAGE_FORMAT_TYPE_FLOAT:
          image_size *= sizeof(float);
          break;
      }

      collector->CreateImage(**(params->pphImage), image_size);
    }
  }

  static void OnExitImageDestroy(
      ze_image_destroy_params_t *params, ze_result_t result,
      void *global_data, void **instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->DestroyImage(*(params->phImage));
    }
  }

  static zet_metric_query_handle_t PrepareToAppendKernelCommand(
    ZeCollector* collector,
    ze_event_handle_t& signal_event,
    ze_command_list_handle_t command_list,
    bool iskernel) {

    ze_context_handle_t context = nullptr;
    ze_device_handle_t device = nullptr;

    collector->command_lists_mutex_.lock_shared();

    auto it = collector->command_lists_.find(command_list);
    if (it != collector->command_lists_.end()) {
      context = it->second->context_;
      device = it->second->device_;
    }

    collector->command_lists_mutex_.unlock_shared();

    if ((context == nullptr) || (device == nullptr)) {
      // should never get here
      std::cerr << "[ERROR] Command list (" << command_list << ") is not found for appending." << std::endl;
      return nullptr;
    }

    if (signal_event == nullptr) {
      signal_event = collector->event_cache_.GetEvent(context);
      PTI_ASSERT(signal_event != nullptr);
    } 
   
    zet_metric_query_handle_t query = nullptr;
    if (collector->options_.metric_query && iskernel) {
      devices_mutex_.lock_shared();

      auto it2 = devices_->find(device);

      PTI_ASSERT(it2 != devices_->end());
      query = collector->query_pools_.GetQuery(context, device, it2->second.metric_group_);

      devices_mutex_.unlock_shared();

      ze_result_t status = zetCommandListAppendMetricQueryBegin(command_list, query);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);
    }

    return query;
  }

  void AppendLaunchKernel(
    ZeCollector *collector, 
    ze_kernel_handle_t kernel,
    const ze_group_count_t *group_count,
    ze_event_handle_t& event_to_signal,
    zet_metric_query_handle_t& query,
    ze_command_list_handle_t command_list,
    std::vector<uint64_t> *kids) {

    uint64_t kernel_id;
    ZeKernelGroupSize group_size;
    bool found = false;

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    kernel_command_properties_mutex_.lock_shared();
    auto kit = active_kernel_properties_->find(kernel);
    if (kit != active_kernel_properties_->end()) {
      kernel_id = kit->second.id_;
      group_size = kit->second.group_size_;
      found = true;
    }
    kernel_command_properties_mutex_.unlock_shared();

    if (!found) {
      std::cerr << "[ERROR] Kernel (" << kernel << ") is not found." << std::endl;
      return;
    }

    command_lists_mutex_.lock_shared();

    auto it = command_lists_.find(command_list);

    if (it != command_lists_.end()) {
      ZeCommand *desc = nullptr;
      ZeCommandMetricQuery *desc_query = nullptr;
        
      desc = local_device_submissions_.GetKernelCommand();

      desc->type_ = KERNEL_COMMAND_TYPE_COMPUTE;
      desc->kernel_command_id_ = kernel_id;
      desc->group_size_ = group_size;
      desc->group_count_ = *group_count;
      desc->engine_ordinal_ = it->second->engine_ordinal_;
      desc->engine_index_ = it->second->engine_index_;
      desc->host_time_origin_ = it->second->host_time_origin_;	// in ns
      desc->device_timer_frequency_ = it->second->device_timer_frequency_;
      desc->device_timer_mask_ = it->second->device_timer_mask_;
      desc->metric_timer_frequency_ = it->second->metric_timer_frequency_;
      desc->metric_timer_mask_ = it->second->metric_timer_mask_;
      desc->implicit_scaling_ = it->second->implicit_scaling_;
      desc->device_ = it->second->device_;
      ze_context_handle_t context = it->second->context_;

      command_lists_mutex_.unlock_shared();

      desc->mem_size_ = 0;
      desc->event_ = event_to_signal;
      desc->command_list_ = command_list;
      desc->queue_ = nullptr;
      desc->tid_ = utils::GetTid();

      if (query != nullptr) {
        desc_query = local_device_submissions_.GetCommandMetricQuery();

        ze_event_handle_t metric_query_event = collector->event_cache_.GetEvent(context);
        ze_result_t status = zetCommandListAppendMetricQueryEnd(command_list, query, metric_query_event, 0, nullptr);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        desc_query->metric_query_event_ = metric_query_event;
        desc_query->metric_query_ = query;
        desc_query->device_ = it->second->device_;
      }

      if (it->second->immediate_) {
        uint64_t host_timestamp;
        uint64_t ticks;
        uint64_t device_timestamp;
        ze_result_t status;

        status = zeDeviceGetGlobalTimestamps(desc->device_, &host_timestamp, &ticks);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);

        device_timestamp = ticks & desc->device_timer_mask_;
        device_timestamp = device_timestamp * NSEC_IN_SEC / desc->device_timer_frequency_;

        desc->immediate_ = true;
        desc->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
        desc->append_time_ = host_timestamp;
        desc->submit_time_ = host_timestamp;
        desc->submit_time_device_ = device_timestamp;
        desc->command_metric_query_ = nullptr;		// don't care metric query in case of immediate command list
        local_device_submissions_.SubmitKernelCommand(desc);
        kids->push_back(desc->instance_id_);

        if (query != nullptr) {
          desc_query->instance_id_ = desc->instance_id_;
          desc_query->immediate_ = true;
          local_device_submissions_.SubmitCommandMetricQuery(desc_query);
        }
      }
      else {
        uint64_t host_timestamp = ze_instance_data.start_time_host;

        desc->append_time_ = host_timestamp;
        desc->immediate_ = false;
        desc->command_metric_query_ = desc_query;	// need metric query upon submission
        command_lists_mutex_.lock();
        it->second->commands_.push_back(desc);
        if (query != nullptr) {
          desc_query->immediate_ = false;
          it->second->metric_queries_.push_back(desc_query);
        }
        command_lists_mutex_.unlock();
      }
    }
    else {
      command_lists_mutex_.unlock_shared();
    }
  }

  void AppendMemoryCommand(
    ZeCollector *collector, 
    ZeDeviceCommandHandle handle,
    size_t size,
    const void* src,
    const void* dst,
    ze_event_handle_t& event_to_signal,
    zet_metric_query_handle_t& query,
    ze_command_list_handle_t command_list,
    std::vector<uint64_t> *kids) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    ze_context_handle_t context = nullptr;
    command_lists_mutex_.lock_shared();
    auto it = command_lists_.find(command_list);
    if (it != command_lists_.end()) {
      context = it->second->context_;
    }

    int mtype = GetMemoryTransferType(context, src, context, dst);
    if (mtype != -1) {
      handle = ZeDeviceCommandHandle(int(handle) + mtype);
    }
      
    uint64_t command_id;
    bool found = false;

    kernel_command_properties_mutex_.lock_shared();
    auto kit = active_command_properties_->find(handle);
    if (kit != active_command_properties_->end()) {
      command_id = kit->second.id_;
      found = true;
    }
    kernel_command_properties_mutex_.unlock_shared();

    if (found && (it != command_lists_.end())) {
      ZeCommand *desc = nullptr;
      ZeCommandMetricQuery *desc_query = nullptr;
      
      desc = local_device_submissions_.GetKernelCommand();

      desc->type_ = KERNEL_COMMAND_TYPE_MEMORY;
      desc->kernel_command_id_ = command_id;;
      desc->engine_ordinal_ = it->second->engine_ordinal_;
      desc->engine_index_ = it->second->engine_index_;
      desc->host_time_origin_ = it->second->host_time_origin_;	// in ns
      desc->device_timer_frequency_ = it->second->device_timer_frequency_;
      desc->device_timer_mask_ = it->second->device_timer_mask_;
      desc->metric_timer_frequency_ = it->second->metric_timer_frequency_;
      desc->metric_timer_mask_ = it->second->metric_timer_mask_;
      desc->event_ = event_to_signal;
      desc->device_ = it->second->device_;
      ze_context_handle_t context = it->second->context_;
      command_lists_mutex_.unlock_shared();

      desc->group_count_ = {0, 0, 0};
      desc->command_list_ = command_list;
      desc->queue_ = nullptr;
      desc->mem_size_ = size;
      desc->tid_ = utils::GetTid();

      if (query != nullptr) {
        desc_query = local_device_submissions_.GetCommandMetricQuery();

        ze_event_handle_t metric_query_event = collector->event_cache_.GetEvent(context);
        ze_result_t status = zetCommandListAppendMetricQueryEnd(command_list, query, metric_query_event, 0, nullptr);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        desc_query->metric_query_event_ = metric_query_event;
        desc_query->metric_query_ = query;
        desc_query->device_ = it->second->device_;
      }

      if (it->second->immediate_) {
        uint64_t host_timestamp;
        uint64_t ticks;
        uint64_t device_timestamp;
        ze_result_t status;

        status = zeDeviceGetGlobalTimestamps(desc->device_, &host_timestamp, &ticks);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);

        device_timestamp = ticks & desc->device_timer_mask_;
        device_timestamp = device_timestamp * NSEC_IN_SEC / desc->device_timer_frequency_;

        desc->immediate_ = true;
        desc->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
        desc->append_time_ = host_timestamp;
        desc->submit_time_ = host_timestamp;
        desc->submit_time_device_ = device_timestamp;
        desc->command_metric_query_ = nullptr;	// do not care metric query in case of immediate command list

        local_device_submissions_.SubmitKernelCommand(desc);

        kids->push_back(desc->instance_id_);

        if (query != nullptr) {
          desc_query->instance_id_ = desc->instance_id_;
          desc_query->immediate_ = true;
          local_device_submissions_.SubmitCommandMetricQuery(desc_query);
        }
      }
      else {
        uint64_t host_timestamp = ze_instance_data.start_time_host;

        desc->append_time_ = host_timestamp;
        desc->immediate_ = false;
        desc->command_metric_query_ = desc_query;
        command_lists_mutex_.lock();
        it->second->commands_.push_back(desc);
        if (query != nullptr) {
          desc_query->immediate_ = false;
          it->second->metric_queries_.push_back(desc_query);
        }
        command_lists_mutex_.unlock();
      }
    }
    else {
      command_lists_mutex_.unlock_shared();
    }
  }

  void AppendMemoryCommandContext(
      ZeCollector *collector, 
      ZeDeviceCommandHandle handle,
      size_t size,
      ze_context_handle_t src_context,
      const void* src,
      ze_context_handle_t dst_context,
      const void* dst,
      ze_event_handle_t& event_to_signal,
      zet_metric_query_handle_t& query,
      ze_command_list_handle_t command_list,
      std::vector<uint64_t> *kids) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    command_lists_mutex_.lock_shared();
    auto it = command_lists_.find(command_list);

    int mtype = GetMemoryTransferType(src_context, src, dst_context, dst);
    if (mtype != -1) {
      handle = ZeDeviceCommandHandle(int(handle) + mtype);
    }
      
    uint64_t command_id;
    bool found = false;

    kernel_command_properties_mutex_.lock_shared();
    auto kit = active_command_properties_->find(handle);
    if (kit != active_command_properties_->end()) {
      command_id = kit->second.id_;
      found = true;
    }
    kernel_command_properties_mutex_.unlock_shared();

    if (found && (it != command_lists_.end())) {
      ZeCommand *desc = nullptr;
      ZeCommandMetricQuery *desc_query = nullptr;
        
      desc = local_device_submissions_.GetKernelCommand();

      desc->type_ = KERNEL_COMMAND_TYPE_MEMORY;
      desc->kernel_command_id_ = command_id;;
      desc->engine_ordinal_ = it->second->engine_ordinal_;
      desc->engine_index_ = it->second->engine_index_;
      desc->host_time_origin_ = it->second->host_time_origin_;	// in ns
      desc->device_timer_frequency_ = it->second->device_timer_frequency_;
      desc->device_timer_mask_ = it->second->device_timer_mask_;
      desc->metric_timer_frequency_ = it->second->metric_timer_frequency_;
      desc->metric_timer_mask_ = it->second->metric_timer_mask_;
      desc->event_ = event_to_signal;
      desc->device_ = it->second->device_;
      ze_context_handle_t context = it->second->context_;
      command_lists_mutex_.unlock_shared();

      desc->group_count_ = {0, 0, 0};
      desc->command_list_ = command_list;
      desc->queue_ = nullptr;
      desc->mem_size_ = size;
      desc->tid_ = utils::GetTid();
      if (query != nullptr) {
        desc_query = local_device_submissions_.GetCommandMetricQuery();

        ze_event_handle_t metric_query_event = collector->event_cache_.GetEvent(context);
        ze_result_t status = zetCommandListAppendMetricQueryEnd(command_list, query, metric_query_event, 0, nullptr);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        desc_query->metric_query_ = query;
        desc_query->metric_query_event_ = metric_query_event;
      }
      if (it->second->immediate_) {
        uint64_t host_timestamp;
        uint64_t ticks;
        uint64_t device_timestamp;
        ze_result_t status;

        status = zeDeviceGetGlobalTimestamps(desc->device_, &host_timestamp, &ticks);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);

        device_timestamp = ticks & desc->device_timer_mask_;
        device_timestamp = device_timestamp * NSEC_IN_SEC / desc->device_timer_frequency_;

        desc->immediate_ = true;
        desc->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
        desc->append_time_ = host_timestamp;
        desc->submit_time_ = host_timestamp;
        desc->submit_time_device_ = device_timestamp;
        desc->command_metric_query_ = nullptr;

        local_device_submissions_.SubmitKernelCommand(desc);

        kids->push_back(desc->instance_id_);
        if (query != nullptr) {
          desc_query->instance_id_ = desc->instance_id_;
          desc_query->immediate_ = true;
          local_device_submissions_.SubmitCommandMetricQuery(desc_query);
        }
      }
      else {
        uint64_t host_timestamp = ze_instance_data.start_time_host;

        desc->append_time_ = host_timestamp;
        desc->immediate_ = false;
        desc->command_metric_query_ = desc_query;
        command_lists_mutex_.lock();
        it->second->commands_.push_back(desc);
        if (query != nullptr) {
          desc_query->immediate_ = false;
          it->second->metric_queries_.push_back(desc_query);
        }
        command_lists_mutex_.unlock();
      }
    }
    else {
      command_lists_mutex_.unlock_shared();
    }
  }

  void AppendImageMemoryCopyCommand(
    ZeCollector *collector, 
    ZeDeviceCommandHandle handle,
    ze_image_handle_t image,
    const void* src,
    const void* dst,
    ze_event_handle_t& event_to_signal,
    zet_metric_query_handle_t& query,
    ze_command_list_handle_t command_list,
    std::vector<uint64_t> *kids) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    ze_context_handle_t context = nullptr;
    command_lists_mutex_.lock_shared();
    auto it = command_lists_.find(command_list);
    if (it != command_lists_.end()) {
      context = it->second->context_;
    }

    int mtype = GetMemoryTransferType(context, src, context, dst);
    if (mtype != -1) {
      handle = ZeDeviceCommandHandle(int(handle) + mtype);
    }
      
    size_t size = GetImageSize(image);

    uint64_t command_id;
    bool found = false;

    kernel_command_properties_mutex_.lock_shared();
    auto kit = active_command_properties_->find(handle);
    if (kit != active_command_properties_->end()) {
      command_id = kit->second.id_;
      found = true;
    }
    kernel_command_properties_mutex_.unlock_shared();

    if (found && (it != command_lists_.end())) {
      ZeCommand *desc = nullptr;
      ZeCommandMetricQuery *desc_query = nullptr;
        
      desc = local_device_submissions_.GetKernelCommand();

      desc->type_ = KERNEL_COMMAND_TYPE_MEMORY;
      desc->kernel_command_id_ = command_id;;
      desc->engine_ordinal_ = it->second->engine_ordinal_;
      desc->engine_index_ = it->second->engine_index_;
      desc->host_time_origin_ = it->second->host_time_origin_;	// in ns
      desc->device_timer_frequency_ = it->second->device_timer_frequency_;
      desc->device_timer_mask_ = it->second->device_timer_mask_;
      desc->metric_timer_frequency_ = it->second->metric_timer_frequency_;
      desc->metric_timer_mask_ = it->second->metric_timer_mask_;
      desc->device_ = it->second->device_;
      ze_context_handle_t context = it->second->context_;
      command_lists_mutex_.unlock_shared();

      desc->group_count_ = {0, 0, 0};
      desc->event_ = event_to_signal;
      desc->command_list_ = command_list;
      desc->mem_size_ = size;
      desc->queue_ = nullptr;
      desc->tid_ = utils::GetTid();
      if (query != nullptr) {
        desc_query = local_device_submissions_.GetCommandMetricQuery();

        ze_event_handle_t metric_query_event = collector->event_cache_.GetEvent(context);
        ze_result_t status = zetCommandListAppendMetricQueryEnd(command_list, query, metric_query_event, 0, nullptr);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        desc_query->metric_query_ = query;
        desc_query->metric_query_event_ = metric_query_event;
      }
      if (it->second->immediate_) {
        uint64_t host_timestamp;
        uint64_t ticks;
        uint64_t device_timestamp;
        ze_result_t status;

        status = zeDeviceGetGlobalTimestamps(desc->device_, &host_timestamp, &ticks);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);

        device_timestamp = ticks & desc->device_timer_mask_;
        device_timestamp = device_timestamp * NSEC_IN_SEC / desc->device_timer_frequency_;

        desc->immediate_ = true;
        desc->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
        desc->append_time_ = host_timestamp;
        desc->submit_time_ = host_timestamp;
        desc->submit_time_device_ = device_timestamp;
        desc->command_metric_query_ = nullptr;

        local_device_submissions_.SubmitKernelCommand(desc);

        kids->push_back(desc->instance_id_);
        if (query != nullptr) {
          desc_query->instance_id_ = desc->instance_id_;
          desc_query->immediate_ = true;
          local_device_submissions_.SubmitCommandMetricQuery(desc_query);
        }
      }
      else {
        uint64_t host_timestamp = ze_instance_data.start_time_host;

        desc->append_time_ = host_timestamp;
        desc->immediate_ = false;
        desc->command_metric_query_ = desc_query;
        command_lists_mutex_.lock();
        it->second->commands_.push_back(desc);
        if (query != nullptr) {
          desc_query->immediate_ = false;
          it->second->metric_queries_.push_back(desc_query);
        }
        command_lists_mutex_.unlock();
      }
    }
    else {
      command_lists_mutex_.unlock_shared();
    }
  }

  void AppendCommand(
      ZeCollector *collector, 
      ZeDeviceCommandHandle handle,
      ze_event_handle_t& event_to_signal,
      zet_metric_query_handle_t& query,
      ze_command_list_handle_t command_list,
      std::vector<uint64_t> *kids) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    command_lists_mutex_.lock_shared();
      
    uint64_t command_id;
    bool found = false;

    kernel_command_properties_mutex_.lock_shared();
    auto kit = active_command_properties_->find(handle);
    if (kit != active_command_properties_->end()) {
      command_id = kit->second.id_;
      found = true;
    }
    kernel_command_properties_mutex_.unlock_shared();

    auto it = command_lists_.find(command_list);
    if (found && (it != command_lists_.end())) {
      ZeCommand *desc = nullptr;
      ZeCommandMetricQuery *desc_query = nullptr;
        
      desc = local_device_submissions_.GetKernelCommand();

      desc->type_ = KERNEL_COMMAND_TYPE_COMMAND;
      desc->kernel_command_id_ = command_id;;
      desc->engine_ordinal_ = it->second->engine_ordinal_;
      desc->engine_index_ = it->second->engine_index_;
      desc->host_time_origin_ = it->second->host_time_origin_;	// in ns
      desc->device_timer_frequency_ = it->second->device_timer_frequency_;
      desc->device_timer_mask_ = it->second->device_timer_mask_;
      desc->metric_timer_frequency_ = it->second->metric_timer_frequency_;
      desc->metric_timer_mask_ = it->second->metric_timer_mask_;
      desc->device_ = it->second->device_;
      ze_context_handle_t context = it->second->context_;
      command_lists_mutex_.unlock_shared();

      desc->group_count_ = {0, 0, 0};
      desc->mem_size_ = 0;
      desc->event_ = event_to_signal;
      desc->command_list_ = command_list;
      desc->queue_ = nullptr;
      desc->tid_ = utils::GetTid();
      if (query != nullptr) {
        desc_query = local_device_submissions_.GetCommandMetricQuery();

        ze_event_handle_t metric_query_event = collector->event_cache_.GetEvent(context);
        desc_query->metric_query_ = query;
        ze_result_t status = zetCommandListAppendMetricQueryEnd(command_list, query, metric_query_event, 0, nullptr);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      }
      if (it->second->immediate_) {
        uint64_t host_timestamp;
        uint64_t ticks;
        uint64_t device_timestamp;
        ze_result_t status;

        status = zeDeviceGetGlobalTimestamps(desc->device_, &host_timestamp, &ticks);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);

        device_timestamp = ticks & desc->device_timer_mask_;
        device_timestamp = device_timestamp * NSEC_IN_SEC / desc->device_timer_frequency_;

        desc->immediate_ = true;
        desc->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
        desc->append_time_ = host_timestamp;
        desc->submit_time_ = host_timestamp;
        desc->submit_time_device_ = device_timestamp;
        desc->command_metric_query_ = nullptr;

        local_device_submissions_.SubmitKernelCommand(desc);
        kids->push_back(desc->instance_id_);
        if (query != nullptr) {
          desc_query->instance_id_ = desc->instance_id_;
          desc_query->immediate_ = true;
          local_device_submissions_.SubmitCommandMetricQuery(desc_query);
        }
      }
      else {
        uint64_t host_timestamp = ze_instance_data.start_time_host;

        desc->append_time_ = host_timestamp;
        desc->immediate_ = false;
        desc->command_metric_query_ = desc_query;
        command_lists_mutex_.lock();
        it->second->commands_.push_back(desc);
        if (query != nullptr) {
          desc_query->immediate_ = false;
          it->second->metric_queries_.push_back(desc_query);
        }
        command_lists_mutex_.unlock();
      }
    }
    else {
      command_lists_mutex_.unlock_shared();
    }
  }

  static void OnEnterCommandListAppendLaunchKernel(
      ze_command_list_append_launch_kernel_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), true);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendLaunchKernel(
    ze_command_list_append_launch_kernel_params_t* params,
    ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {

    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      collector->AppendLaunchKernel(
        collector,
        *(params->phKernel),
        *(params->ppLaunchFuncArgs),
        *(params->phSignalEvent),
        query,
        *(params->phCommandList),
        kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendLaunchCooperativeKernel(
      ze_command_list_append_launch_cooperative_kernel_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), true);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendLaunchCooperativeKernel(
      ze_command_list_append_launch_cooperative_kernel_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
        collector->AppendLaunchKernel(
          collector,
          *(params->phKernel),
          *(params->ppLaunchFuncArgs),
          *(params->phSignalEvent),
          query,
          *(params->phCommandList),
          kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendLaunchKernelIndirect(
      ze_command_list_append_launch_kernel_indirect_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), true);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendLaunchKernelIndirect(
      ze_command_list_append_launch_kernel_indirect_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
        collector->AppendLaunchKernel(
          collector,
          *(params->phKernel),
          *(params->ppLaunchArgumentsBuffer),
          *(params->phSignalEvent),
          query,
          *(params->phCommandList),
          kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  int GetMemoryTransferType(ze_context_handle_t src_context, const void *src) {

    int stype = -1;

    if (src_context != nullptr && src != nullptr) {
      ze_memory_allocation_properties_t props{ZE_STRUCTURE_TYPE_MEMORY_ALLOCATION_PROPERTIES,};
      ze_result_t status = zeMemGetAllocProperties(src_context, src, &props, nullptr);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);

      switch (props.type) {
        case ZE_MEMORY_TYPE_HOST:
          stype = 0;
          break;
        case ZE_MEMORY_TYPE_DEVICE:
          stype = 1;
          break;
        case ZE_MEMORY_TYPE_UNKNOWN:
          stype = 2;
          break;
        case ZE_MEMORY_TYPE_SHARED:
          stype = 3;
          break;
        default:
          break;
      }
    }
    return stype;
  }

  int GetMemoryTransferType(ze_context_handle_t src_context, const void *src, ze_context_handle_t dst_context, const void *dst) {

    int stype = -1;
    int dtype = -1;

    if (src_context != nullptr && src != nullptr) {
      ze_memory_allocation_properties_t props{ZE_STRUCTURE_TYPE_MEMORY_ALLOCATION_PROPERTIES,};
      ze_result_t status = zeMemGetAllocProperties(src_context, src, &props, nullptr);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);

      switch (props.type) {
        case ZE_MEMORY_TYPE_HOST:
	  stype = 0;
	  break;
        case ZE_MEMORY_TYPE_DEVICE:
	  stype = 1;
	  break;
        case ZE_MEMORY_TYPE_UNKNOWN:
	  stype = 2;
	  break;
        case ZE_MEMORY_TYPE_SHARED:
	  stype = 3;
	  break;
        default:
	  break;
      }
    }

    if (dst_context != nullptr && dst != nullptr) {
      ze_memory_allocation_properties_t props{ZE_STRUCTURE_TYPE_MEMORY_ALLOCATION_PROPERTIES,};
      ze_result_t status = zeMemGetAllocProperties(dst_context, dst, &props, nullptr);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);

      switch (props.type) {
        case ZE_MEMORY_TYPE_HOST:
	  dtype = 0;
	  break;
        case ZE_MEMORY_TYPE_DEVICE:
	  dtype = 1;
	  break;
        case ZE_MEMORY_TYPE_UNKNOWN:
	  dtype = 2;
	  break;
        case ZE_MEMORY_TYPE_SHARED:
	  dtype = 3;
	  break;
        default:
	  break;
      }
    }

    if ((stype != -1) && (dtype != -1)) {
      return (stype << 2 | dtype);
    }
    else {
      return stype;
    }
  }

  static void OnEnterCommandListAppendMemoryCopy(
      ze_command_list_append_memory_copy_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendMemoryCopy(
      ze_command_list_append_memory_copy_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      collector->AppendMemoryCommand(collector, MemoryCopy, *(params->psize),
          *(params->psrcptr), *(params->pdstptr), *(params->phSignalEvent), query,
          *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendMemoryFill(
      ze_command_list_append_memory_fill_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendMemoryFill(
      ze_command_list_append_memory_fill_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      collector->AppendMemoryCommand(collector, MemoryFill, *(params->psize),
          *(params->pptr), nullptr, *(params->phSignalEvent), query,
          *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendBarrier(
      ze_command_list_append_barrier_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendBarrier(
      ze_command_list_append_barrier_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      collector->AppendCommand(collector, Barrier, *(params->phSignalEvent), query,
          *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendMemoryRangesBarrier(
      ze_command_list_append_memory_ranges_barrier_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendMemoryRangesBarrier(
      ze_command_list_append_memory_ranges_barrier_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      collector->AppendCommand(collector, MemoryRangesBarrier, *(params->phSignalEvent), query,
          *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendMemoryCopyRegion(
      ze_command_list_append_memory_copy_region_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendMemoryCopyRegion(
      ze_command_list_append_memory_copy_region_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      size_t bytes_transferred = 0;
      const ze_copy_region_t* region = *(params->psrcRegion);

      if (region != nullptr) {
        size_t bytes_transferred = region->width * region->height * (*(params->psrcPitch));
        if (region->depth != 0) {
          bytes_transferred *= region->depth;
        }
      }

      collector->AppendMemoryCommand(collector, MemoryCopyRegion, bytes_transferred,
        *(params->psrcptr), *(params->pdstptr), *(params->phSignalEvent), query,
         *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendMemoryCopyFromContext(
      ze_command_list_append_memory_copy_from_context_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendMemoryCopyFromContext(
      ze_command_list_append_memory_copy_from_context_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ze_context_handle_t src_context = *(params->phContextSrc);
      ze_context_handle_t dst_context = nullptr;
      collector->AppendMemoryCommandContext(collector, MemoryCopyFromContext, *(params->psize),
        src_context, *(params->psrcptr), nullptr, *(params->pdstptr), *(params->phSignalEvent), query,
        *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendImageCopy(
      ze_command_list_append_image_copy_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendImageCopy(
      ze_command_list_append_image_copy_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      collector->AppendImageMemoryCopyCommand(collector, ImageCopy, *(params->phSrcImage),
        nullptr, nullptr, *(params->phSignalEvent), query,
        *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendImageCopyRegion(
      ze_command_list_append_image_copy_region_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendImageCopyRegion(
      ze_command_list_append_image_copy_region_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      collector->AppendImageMemoryCopyCommand(collector, ImageCopyRegion, *(params->phSrcImage),
        nullptr, nullptr, *(params->phSignalEvent), query,
        *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendImageCopyToMemory(
      ze_command_list_append_image_copy_to_memory_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendImageCopyToMemory(
      ze_command_list_append_image_copy_to_memory_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      collector->AppendImageMemoryCopyCommand(collector, ImageCopyToMemory, *(params->phSrcImage),
        nullptr, *(params->pdstptr), *(params->phSignalEvent), query,
        *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendImageCopyFromMemory(
      ze_command_list_append_image_copy_from_memory_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      zet_metric_query_handle_t query = PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false);
      *instance_data = reinterpret_cast<void*>(query);
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendImageCopyFromMemory(
      ze_command_list_append_image_copy_from_memory_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    zet_metric_query_handle_t query = reinterpret_cast<zet_metric_query_handle_t>(*instance_data);
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && (UniController::IsCollectionEnabled())) {
      size_t bytes_transferred = 0;
      const ze_image_region_t* region = *(params->ppDstRegion);

      if (region != nullptr) {
        bytes_transferred = region->width * region->height;
        if (region->depth != 0) {
          bytes_transferred *= region->depth;
        }
      }

      collector->AppendMemoryCommand(collector, ImageCopyFromMemory, bytes_transferred,
        *(params->psrcptr), nullptr, *(params->phSignalEvent), query,
        *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(query);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnExitCommandListCreate(
      ze_command_list_create_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

      // dummy engine ordinal and index
      collector->CreateCommandList( **(params->pphCommandList), *(params->phContext), *(params->phDevice), -1, -1, false);
    }
  }

  static void OnExitCommandListCreateImmediate(
      ze_command_list_create_immediate_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(**params->pphCommandList != nullptr);
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      ze_device_handle_t* hDevice = params->phDevice;
      if (hDevice == nullptr) {
        return;
      }

      const ze_command_queue_desc_t* clq_desc = *params->paltdesc;
      if (clq_desc == nullptr) {
        return;
      }

      ze_command_list_handle_t*  command_list = *params->pphCommandList;
      if (command_list == nullptr) {
        return;
      }

      collector->CreateCommandList(**(params->pphCommandList), *(params->phContext), *(params->phDevice), clq_desc->ordinal, clq_desc->index, true);
    }
  }

  static void OnExitCommandListDestroy(
    ze_command_list_destroy_params_t* params,
    ze_result_t result, void* global_data, void** instance_data) {

    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(*params->phCommandList != nullptr);
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessCommandsSubmitted(nullptr);
      collector->DestroyCommandList(*params->phCommandList);
    }
  }

  static void OnExitCommandListReset(ze_command_list_reset_params_t* params, ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(*params->phCommandList != nullptr);
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessCommandsSubmitted(nullptr);
      collector->ResetCommandList(*params->phCommandList);
    }
  }

  static void OnExitCommandQueueExecuteCommandLists(
      ze_command_queue_execute_command_lists_params_t* params,
      ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {

    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

      if (UniController::IsCollectionEnabled()) {
        uint32_t count = *params->pnumCommandLists;
        if (count == 0) {
          return;
        }

        ze_command_list_handle_t* cmdlists = *params->pphCommandLists;
        if (cmdlists == nullptr) {
          return;
        }

        uint64_t ts = ze_instance_data.start_time_host;
        collector->ExecuteCommandLists(cmdlists, count, *(params->phCommandQueue), *(params->phFence), ts, kids);
      }
    }
  }

  static void OnExitCommandQueueSynchronize(
    ze_command_queue_synchronize_params_t* params,
    ze_result_t result, void* global_data, void** instance_data, std::vector<uint64_t> *kids) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessAllCommandsSubmitted(kids);
    }
  }

  static void OnExitCommandQueueCreate(ze_command_queue_create_params_t* params, ze_result_t result, void* global_data, void** instance_data) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    ze_device_handle_t* device = params->phDevice;
    if (device == nullptr) {
      return;
    }
    const ze_command_queue_desc_t* queue_desc = *params->pdesc;
    if (queue_desc == nullptr) {
      return;
    }
    ze_command_queue_handle_t*  command_queue = *params->pphCommandQueue;
    if (command_queue == nullptr) {
      return;
    }

    ZeCommandQueue desc;
    desc.queue_ = *command_queue;
    desc.context_ = *(params->phContext);
    desc.device_ = *device;
    desc.engine_ordinal_ = queue_desc->ordinal;
    desc.engine_index_ = queue_desc->index;;

    collector->command_queues_mutex_.lock();
    collector->command_queues_.erase(*command_queue);
    collector->command_queues_.insert({*command_queue, std::move(desc)});
    collector->command_queues_mutex_.unlock();
  }

  static void OnExitCommandQueueDestroy(ze_command_queue_destroy_params_t* params, ze_result_t result,
    void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessAllCommandsSubmitted(nullptr);
      collector->command_queues_mutex_.lock();
      collector->command_queues_.erase(*params->phCommandQueue);
      collector->command_queues_mutex_.unlock();
    }
  }

  static void OnExitModuleCreate(ze_module_create_params_t* params, ze_result_t result, void* global_data, void** instance_user_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      ze_module_handle_t mod = **(params->pphModule);
      ze_device_handle_t device = *(params->phDevice);
      size_t binary_size;
      if (zeModuleGetNativeBinary(mod, &binary_size, nullptr) != ZE_RESULT_SUCCESS) {
        binary_size = (size_t)(-1);
      }
      modules_on_devices_mutex_.lock();
      modules_on_devices_.insert({mod, {device, binary_size}});
      modules_on_devices_mutex_.unlock();
    }
  }

  static void OnEnterModuleDestroy(ze_module_destroy_params_t* params, ze_result_t result, void* global_data, void** instance_user_data) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    ze_module_handle_t mod = *(params->phModule);
    modules_on_devices_mutex_.lock();
    modules_on_devices_.erase(mod);
    modules_on_devices_mutex_.unlock();
  }

  static void OnExitKernelCreate(ze_kernel_create_params_t *params, ze_result_t result, void* global_data, void** instance_user_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      ze_kernel_handle_t kernel = **(params->pphKernel);

      ze_module_handle_t mod = *(params->phModule);
      ze_device_handle_t device = nullptr;
      size_t module_binary_size = (size_t)(-1);
      modules_on_devices_mutex_.lock_shared();
      auto mit = modules_on_devices_.find(mod);
      if (mit != modules_on_devices_.end()) {
        device = mit->second.first; 
        module_binary_size = mit->second.second;
      }
      modules_on_devices_mutex_.unlock_shared();

      int did = -1;
      if (device != nullptr) {
        devices_mutex_.lock_shared();
        auto dit = devices_->find(device);
        if (dit != devices_->end()) {
          did = dit->second.id_;
        } 
        devices_mutex_.unlock_shared();
      }
      kernel_command_properties_mutex_.lock();

      auto it = active_kernel_properties_->find(kernel);
      if (it != active_kernel_properties_->end()) {
        active_kernel_properties_->erase(it);
      }

      ZeKernelCommandProperties desc;

      desc.type_ = KERNEL_COMMAND_TYPE_COMPUTE;

      ze_result_t status;

      desc.id_ = UniKernelId::GetKernelId();

      if ((*(params->pdesc) != nullptr) && ((*(params->pdesc))->pKernelName != nullptr)) {
        desc.name_ = std::string((*(params->pdesc))->pKernelName);
      }
      else {
        // try one more time
        size_t kname_size = 0;
        status = zeKernelGetName(kernel, &kname_size, nullptr);
        if ((status == ZE_RESULT_SUCCESS) && (kname_size > 0)) {
          char kname[kname_size];
          status = zeKernelGetName(kernel, &kname_size, kname);
          PTI_ASSERT(status == ZE_RESULT_SUCCESS);
          desc.name_ = std::string(kname);
        }
        else {
          desc.name_ = "UnknownKernel";
        }
      }

      desc.device_id_ = did;
      desc.device_ = device;

      ze_kernel_properties_t kprops{};
      status = zeKernelGetProperties(kernel, &kprops);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      desc.simd_width_ = kprops.maxSubgroupSize;
      desc.nargs_ = kprops.numKernelArgs;
      desc.nsubgrps_ = kprops.maxNumSubgroups;
      desc.slmsize_ = kprops.localMemSize;
      desc.private_mem_size_ = kprops.privateMemSize;
      desc.spill_mem_size_ = kprops.spillMemSize;
      ZeKernelGroupSize group_size{kprops.requiredGroupSizeX, kprops.requiredGroupSizeY, kprops.requiredGroupSizeZ};
      desc.group_size_ = group_size;

      // for stall sampling
      uint64_t base_addr = 0;
      uint64_t binary_size = 0;
      if (collector->options_.stall_sampling && (zexKernelGetBaseAddress != nullptr) && (zexKernelGetBaseAddress(kernel, &base_addr) == ZE_RESULT_SUCCESS)) {
        base_addr &= 0xFFFFFFFF;
        binary_size = module_binary_size;	// store module binary size. only an upper bound is needed
      }

      desc.base_addr_ = base_addr;
      desc.size_ = binary_size;

      ZeKernelCommandProperties desc2 = desc;
      active_kernel_properties_->insert({kernel, std::move(desc)});
      kernel_command_properties_->insert({desc2.id_, std::move(desc2)});

      kernel_command_properties_mutex_.unlock();
    }
  }

  static void OnExitKernelSetGroupSize(ze_kernel_set_group_size_params_t* params, ze_result_t result,
    void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      if (UniController::IsCollectionEnabled()) {
        ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
        ZeKernelGroupSize group_size{*(params->pgroupSizeX), *(params->pgroupSizeY), *(params->pgroupSizeZ)};
        kernel_command_properties_mutex_.lock();

        auto it = active_kernel_properties_->find(*(params->phKernel));
        PTI_ASSERT(it != active_kernel_properties_->end());
        if ((it->second.group_size_.x != group_size.x) || (it->second.group_size_.y != group_size.y) ||
          (it->second.group_size_.z != group_size.z)) {
          // new group size
          it->second.group_size_ = group_size;
          auto it2 = kernel_command_properties_->find(it->second.id_);
          if ((it2 != kernel_command_properties_->end()) && 
            (it2->second.group_size_.x == group_size.x) && 
            (it2->second.group_size_.y == group_size.y) && 
            (it2->second.group_size_.z == group_size.z)) {
            // group size was used before 
            it->second.id_ = it2->second.id_;
          }
          else {
            // first time use the group size
            it->second.id_ = UniKernelId::GetKernelId();
            ZeKernelCommandProperties desc2 = it->second;
            kernel_command_properties_->insert({desc2.id_, std::move(desc2)});
          }
        }
        else {
          // do nothing
        }

        kernel_command_properties_mutex_.unlock();
      }
    }
  }

  static void OnExitKernelDestroy(ze_kernel_destroy_params_t* params, ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      kernel_command_properties_mutex_.lock();
      active_kernel_properties_->erase(*(params->phKernel));
      kernel_command_properties_mutex_.unlock();
    }
  }

  static void OnExitContextDestroy(ze_context_destroy_params_t* params, ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessAllCommandsSubmitted(nullptr);
      collector->event_cache_.ReleaseContext(*(params->phContext));
    }
  }

  #include <tracing.gen> // Auto-generated callbacks

  void CollectHostFunctionTimeStats(uint32_t id, uint64_t time) {
    local_device_submissions_.CollectHostFunctionTimeStats(id, time);
  }

  void AggregateDeviceTimeStats() const {
    // do not acquire global_device_time_stats_mutex_. caller does it.
    for (auto it = global_device_time_stats_->begin(); it != global_device_time_stats_->end(); it++) {
      std::string kname;
      if (it->first.tile_ >= 0) {
        kname = "Tile #" + std::to_string(it->first.tile_) + ": " + GetZeKernelCommandName(it->first.kernel_command_id_, it->first.group_count_, it->first.mem_size_, options_.verbose);
      }
      else {
        kname = GetZeKernelCommandName(it->first.kernel_command_id_, it->first.group_count_, it->first.mem_size_, options_.verbose);
      }

      auto it2 = it;
      it2++;

      for (; it2 != global_device_time_stats_->end();) {
        std::string kname2;
        if (it2->first.tile_ >= 0) {
          kname2 = "Tile #" + std::to_string(it2->first.tile_) + ": " + GetZeKernelCommandName(it2->first.kernel_command_id_, it2->first.group_count_, it2->first.mem_size_, options_.verbose);
        }
        else {
          kname2 = GetZeKernelCommandName(it2->first.kernel_command_id_, it2->first.group_count_, it2->first.mem_size_, options_.verbose);
        }

        if (kname2 == kname) {
          it->second.append_time_ += it2->second.append_time_;
          it->second.submit_time_ += it2->second.submit_time_;
          it->second.execute_time_ += it2->second.execute_time_;
          if (it->second.min_time_ > it2->second.min_time_) {
            it->second.min_time_ = it2->second.min_time_;
          }
          if (it->second.max_time_ < it2->second.max_time_) {
            it->second.max_time_ = it2->second.max_time_;
          }
          it->second.call_count_ += it2->second.call_count_;
          it2 = global_device_time_stats_->erase(it2);
        }
        else {
          it2++;
        }
      }
    }
  }

 private: // Data
  zel_tracer_handle_t tracer_ = nullptr;
  CollectorOptions options_;
  Correlator* correlator_ = nullptr;
  OnZeKernelFinishCallback kcallback_ = nullptr;
  OnZeFunctionFinishCallback fcallback_ = nullptr;
  void* callback_data_ = nullptr;

  mutable std::shared_mutex images_mutex_;
  std::map<ze_image_handle_t, size_t> images_;

  ZeEventCache event_cache_;
  
  mutable std::shared_mutex command_queues_mutex_;
  std::map<ze_command_queue_handle_t, ZeCommandQueue> command_queues_;

  mutable std::shared_mutex command_lists_mutex_;
  std::map<ze_command_list_handle_t, ZeCommandList *> command_lists_;

  std::set<std::pair<ze_context_handle_t, ze_device_handle_t>> metric_activations_;

  ZeMetricQueryPools query_pools_;

  std::vector<ze_context_handle_t> metric_contexts_;

  constexpr static uint32_t kCallsLength = 12;
  constexpr static uint32_t kTimeLength = 20;

  std::string data_dir_name_;
};

#endif // PTI_TOOLS_UNITRACE_LEVEL_ZERO_COLLECTOR_H_

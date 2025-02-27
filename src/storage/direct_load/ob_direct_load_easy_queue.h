// Copyright (c) 2022-present Oceanbase Inc. All Rights Reserved.
// Author:
//   suzhi.yt <>

#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/lock/ob_mutex.h"
#include "lib/list/ob_list.h"
#include "share/ob_errno.h"
#include "share/rc/ob_tenant_base.h"

namespace oceanbase
{
namespace storage
{

template<class T>
class ObDirectLoadEasyQueue //性能很差的一个queue，主要为了方便使用
{
public:
  ObDirectLoadEasyQueue() : malloc_(ObMemAttr(MTL_ID(), "TLD_EasyQueue")), queue_(malloc_) {}

  int push(const T &e) {
    int ret = OB_SUCCESS;
    lib::ObMutexGuard guard(mutex_);
    if (OB_FAIL(queue_.push_back(e))) {
      SERVER_LOG(WARN, "fail to push back queue", KR(ret));
    }
    return ret;
  }

  int64_t size() {
    lib::ObMutexGuard guard(mutex_);
    return queue_.size();
  }

  void pop_one(T &value) {
    value = nullptr;
    lib::ObMutexGuard guard(mutex_);
    if (!queue_.empty()) {
      value = queue_.get_first();
      queue_.pop_front();
    }
  }

  void pop_all(common::ObIArray<T> &values) {
    lib::ObMutexGuard guard(mutex_);
    while (!queue_.empty()) {
      T &e = queue_.get_first();
      values.push_back(e);
      queue_.pop_front();
    }
  }

private:
  ObMalloc malloc_;
  common::ObList<T, common::ObIAllocator> queue_;
  lib::ObMutex mutex_;
};


}  // namespace storage
}  // namespace oceanbase

/**
 * Copyright (c) 2022 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_OB_TRANSFER_LOCK_UTILS
#define OCEANBASE_OB_TRANSFER_LOCK_UTILS

#include "share/transfer/ob_transfer_info.h"
#include "share/ob_ls_id.h"
#include "common/ob_member_list.h"
#include "ob_transfer_lock_info_operator.h"
#include "storage/ls/ob_ls.h"

namespace oceanbase {
namespace storage {

class ObMemberListLockUtils {
public:
  /* member list*/
  static int batch_lock_ls_member_list(const uint64_t tenant_id, const int64_t task_id,
      const common::ObArray<share::ObLSID> &lock_ls_list, const common::ObMemberList &member_list,
      const ObTransferLockStatus &status, common::ObMySQLProxy &sql_proxy);
  static int lock_ls_member_list(const uint64_t tenant_id, const share::ObLSID &ls_id, const int64_t task_id,
      const common::ObMemberList &member_list, const ObTransferLockStatus &status, ObLS *ls,
      common::ObMySQLProxy &sql_proxy);
  static int unlock_ls_member_list(const uint64_t tenant_id, const share::ObLSID &ls_id, const int64_t task_id,
      const common::ObMemberList &member_list, const ObTransferLockStatus &status, common::ObMySQLProxy &sql_proxy);

private:
  /* sql operator */
  static int insert_lock_info(const uint64_t tenant_id, const share::ObLSID &ls_id, const int64_t task_id,
      const ObTransferLockStatus &status, const int64_t lock_owner, const common::ObString &comment,
      int64_t &real_lock_owner, common::ObISQLClient &sql_proxy);
  static int get_lock_info(const uint64_t tenant_id, const share::ObLSID &ls_id, const int64_t task_id,
      const ObTransferLockStatus &status, ObTransferTaskLockInfo &lock_info, common::ObISQLClient &sql_proxy);

private:
  /* monotonic id*/
  static int get_lock_owner(const uint64_t tenant_id, const share::ObLSID &ls_id, const int64_t task_id,
      const ObTransferLockStatus &status, common::ObMySQLProxy &sql_proxy, int64_t &lock_owner);
  static int get_member_list_str(const common::ObMemberList &member_list, ObSqlString &member_list_str);

private:
  /* palf lock config*/
  static int try_lock_config_change(const ObTransferTaskLockInfo &lock_info, const int64_t lock_timeout, ObLS *ls);
  static int record_config_change_lock_stat(const ObTransferTaskLockInfo &lock_info, ObLS *ls);
  static int unlock_config_change(const ObTransferTaskLockInfo &lock_info, const int64_t lock_timeout, ObLS *ls);

private:
  static int check_lock_status_(
      const bool palf_is_locked, const int64_t palf_lock_owner, const int64_t inner_table_lock_owner, bool &need_lock);
  static int check_unlock_status_(const bool palf_is_locked, const int64_t palf_lock_owner,
      const int64_t inner_table_lock_owner, bool &need_unlock, bool &need_relock_before_unlock);
  static int relock_before_unlock_(const ObTransferTaskLockInfo &lock_info, const int64_t palf_lock_owner,
      const int64_t lock_timeout, ObLS *ls);

private:
  static const int64_t CONFIG_CHANGE_TIMEOUT = 10 * 1000 * 1000L;  // 10s
};

}  // namespace storage
}  // namespace oceanbase

#endif

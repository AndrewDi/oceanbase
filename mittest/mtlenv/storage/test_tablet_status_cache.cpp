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

#include <gtest/gtest.h>

#define private public
#define protected public

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log.h"
#include "mtlenv/mock_tenant_module_env.h"
#include "unittest/storage/test_tablet_helper.h"
#include "unittest/storage/test_dml_common.h"
#include "share/ob_ls_id.h"
#include "common/ob_tablet_id.h"
#include "storage/ls/ob_ls.h"
#include "storage/ls/ob_ls_get_mod.h"
#include "storage/multi_data_source/mds_ctx.h"
#include "storage/tablet/ob_tablet_create_delete_mds_user_data.h"
#include "storage/tx/ob_trans_define.h"
#include "observer/ob_safe_destroy_thread.h"

using namespace oceanbase::common;

#define USING_LOG_PREFIX STORAGE

namespace oceanbase
{
namespace storage
{
class TestTabletStatusCache : public::testing::Test
{
public:
  TestTabletStatusCache();
  virtual ~TestTabletStatusCache() = default;
  static void SetUpTestCase();
  static void TearDownTestCase();
  virtual void SetUp();
  virtual void TearDown();
public:
  static int create_ls(const uint64_t tenant_id, const share::ObLSID &ls_id, ObLSHandle &ls_handle);
  static int remove_ls(const share::ObLSID &ls_id);
  int create_tablet(
      const common::ObTabletID &tablet_id,
      ObTabletHandle &tablet_handle,
      const share::SCN &create_commit_scn = share::SCN::min_scn());
public:
  static constexpr uint64_t TENANT_ID = 1001;
  static const share::ObLSID LS_ID;

  common::ObArenaAllocator allocator_;
};

TestTabletStatusCache::TestTabletStatusCache()
  : allocator_()
{
}

const share::ObLSID TestTabletStatusCache::LS_ID(1001);

void TestTabletStatusCache::SetUpTestCase()
{
  int ret = OB_SUCCESS;
  ret = MockTenantModuleEnv::get_instance().init();
  ASSERT_EQ(OB_SUCCESS, ret);

  SAFE_DESTROY_INSTANCE.init();
  SAFE_DESTROY_INSTANCE.start();
  ObServerCheckpointSlogHandler::get_instance().is_started_ = true;

  // create ls
  ObLSHandle ls_handle;
  ret = create_ls(TENANT_ID, LS_ID, ls_handle);
  ASSERT_EQ(OB_SUCCESS, ret);
}

void TestTabletStatusCache::TearDownTestCase()
{
  int ret = OB_SUCCESS;

  // remove ls
  ret = remove_ls(LS_ID);
  ASSERT_EQ(OB_SUCCESS, ret);

  SAFE_DESTROY_INSTANCE.stop();
  SAFE_DESTROY_INSTANCE.wait();
  SAFE_DESTROY_INSTANCE.destroy();

  MockTenantModuleEnv::get_instance().destroy();
}

void TestTabletStatusCache::SetUp()
{
}

void TestTabletStatusCache::TearDown()
{
}

int TestTabletStatusCache::create_ls(const uint64_t tenant_id, const share::ObLSID &ls_id, ObLSHandle &ls_handle)
{
  int ret = OB_SUCCESS;
  ret = TestDmlCommon::create_ls(tenant_id, ls_id, ls_handle);
  return ret;
}

int TestTabletStatusCache::remove_ls(const share::ObLSID &ls_id)
{
  int ret = OB_SUCCESS;
  ret = MTL(ObLSService*)->remove_ls(ls_id, false);
  return ret;
}

int TestTabletStatusCache::create_tablet(
    const common::ObTabletID &tablet_id,
    ObTabletHandle &tablet_handle,
    const share::SCN &create_commit_scn)
{
  int ret = OB_SUCCESS;
  const uint64_t table_id = 1234567;
  share::schema::ObTableSchema table_schema;
  ObLSHandle ls_handle;
  ObLS *ls = nullptr;
  mds::MdsTableHandle mds_table;

  if (OB_FAIL(MTL(ObLSService*)->get_ls(LS_ID, ls_handle, ObLSGetMod::STORAGE_MOD))) {
    LOG_WARN("failed to get ls", K(ret));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls is null", K(ret), KP(ls));
  } else if (OB_FAIL(build_test_schema(table_schema, table_id))) {
    LOG_WARN("failed to build table schema");
  } else if (OB_FAIL(TestTabletHelper::create_tablet(ls_handle, tablet_id, table_schema, allocator_,
      ObTabletStatus::Status::NORMAL, create_commit_scn))) {
    LOG_WARN("failed to create tablet", K(ret), K(ls_id), K(tablet_id), K(create_commit_scn));
  } else if (OB_FAIL(ls->get_tablet(tablet_id, tablet_handle))) {
    LOG_WARN("failed to get tablet", K(ret));
  } else if (OB_FAIL(tablet_handle.get_obj()->inner_get_mds_table(mds_table, true/*not_exist_create*/))) {
    LOG_WARN("failed to get mds table", K(ret));
  }

  return ret;
}

TEST_F(TestTabletStatusCache, weak_read)
{
  int ret = OB_SUCCESS;

  // create tablet
  const common::ObTabletID tablet_id(ObTimeUtility::fast_current_time() % 10000000000000);
  ObTabletHandle tablet_handle;
  // create commit scn: 50
  share::SCN min_scn;
  min_scn.set_min();
  ret = create_tablet(tablet_id, tablet_handle, share::SCN::plus(min_scn, 50));
  ASSERT_EQ(OB_SUCCESS, ret);

  ObTablet *tablet = tablet_handle.get_obj();
  ASSERT_NE(nullptr, tablet);

  // disable cache
  {
    SpinWLockGuard guard(tablet->mds_cache_lock_);
    tablet->tablet_status_cache_.reset();
  }

  const ObTabletMapKey key(LS_ID, tablet_id);
  tablet_handle.reset();
  // mode is READ_READABLE_COMMITED, snapshot version is smaller than create commit version, return OB_SNAPSHOT_DISCARDED
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_READABLE_COMMITED, 20/*snapshot*/);
  ASSERT_EQ(OB_SNAPSHOT_DISCARDED, ret);
  // mode is READ_ALL_COMMITED, but snapshot is not max scn, not supported
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_ALL_COMMITED, 20/*snapshot*/);
  ASSERT_EQ(OB_NOT_SUPPORTED, ret);
  // mode is READ_READABLE_COMMITED, snapshot version is bigger than create commit version, return OB_SUCCESS
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_READABLE_COMMITED, 60/*snapshot*/);
  ASSERT_EQ(OB_SUCCESS, ret);
}

TEST_F(TestTabletStatusCache, get_transfer_out_tablet)
{
  int ret = OB_SUCCESS;

  // create tablet
  const common::ObTabletID tablet_id(ObTimeUtility::fast_current_time() % 10000000000000);
  ObTabletHandle tablet_handle;
  ret = create_tablet(tablet_id, tablet_handle);
  ASSERT_EQ(OB_SUCCESS, ret);

  ObTablet *tablet = tablet_handle.get_obj();
  ASSERT_NE(nullptr, tablet);

  // set transfer scn
  // create commit scn: 50
  // transfer scn: 100
  ObTabletCreateDeleteMdsUserData user_data;
  user_data.tablet_status_ = ObTabletStatus::TRANSFER_OUT;
  share::SCN min_scn;
  min_scn.set_min();
  user_data.transfer_scn_ = share::SCN::plus(min_scn, 100);
  user_data.transfer_ls_id_ = share::ObLSID(1010);
  user_data.data_type_ = ObTabletMdsUserDataType::START_TRANSFER_OUT;
  user_data.create_commit_scn_ = share::SCN::plus(min_scn, 50);
  user_data.create_commit_version_ = 50;

  mds::MdsCtx ctx1(mds::MdsWriter(transaction::ObTransID(123)));
  ret = tablet->set(user_data, ctx1);
  ASSERT_EQ(OB_SUCCESS, ret);

  // disable cache
  {
    SpinWLockGuard guard(tablet->mds_cache_lock_);
    tablet->tablet_status_cache_.reset();
  }

  const ObTabletMapKey key(LS_ID, tablet_id);
  tablet_handle.reset();
  // mode is READ_READABLE_COMMITED, can not get tablet
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_READABLE_COMMITED, ObTransVersion::MAX_TRANS_VERSION/*snapshot*/);
  ASSERT_EQ(OB_SCHEMA_EAGAIN, ret);
  // mode is READ_ALL_COMMITED, allow to get TRANSFER_OUT status tablet
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_ALL_COMMITED, ObTransVersion::MAX_TRANS_VERSION/*snapshot*/);
  ASSERT_EQ(OB_SUCCESS, ret);

  tablet_handle.reset();
  // read snapshot: 80. not max scn, return OB_NOT_SUPPORTED
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_ALL_COMMITED, 80/*snapshot*/);
  ASSERT_EQ(OB_NOT_SUPPORTED, ret);
  // read snapshot: 80. less than transfer scn
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_ALL_COMMITED, ObTransVersion::MAX_TRANS_VERSION/*snapshot*/);
  ASSERT_EQ(OB_SUCCESS, ret);

  // check tablet status cache
  {
    SpinRLockGuard guard(tablet->mds_cache_lock_);
    const ObTabletStatusCache &tablet_status_cache = tablet->tablet_status_cache_;

    // we won't update tablet status cache if current status is TRANSFER_OUT,
    // so here the cache remains invalid
    ASSERT_EQ(ObTabletStatus::MAX, tablet_status_cache.tablet_status_);
  }

  // let transaction commit
  // commit scn: 120
  share::SCN commit_scn = share::SCN::plus(min_scn, 120);
  ctx1.single_log_commit(commit_scn, commit_scn);

  tablet_handle.reset();
  // mode is READ_READABLE_COMMITED, read snapshot is max scn, greater than transfer scn 100, not allow to get tablet
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_READABLE_COMMITED, ObTransVersion::MAX_TRANS_VERSION/*snapshot*/);
  ASSERT_EQ(OB_TABLET_NOT_EXIST, ret);
  // mode is READ_ALL_COMMITED, allow to get tablet
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_ALL_COMMITED, ObTransVersion::MAX_TRANS_VERSION/*snapshot*/);
  ASSERT_EQ(OB_SUCCESS, ret);

  tablet_handle.reset();
  // mode is READ_READABLE_COMMITED, read snapshot 80 less than transfer scn 100, allow to get tablet
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_READABLE_COMMITED, 80/*snapshot*/);
  ASSERT_EQ(OB_SUCCESS, ret);
  // mode is READ_ALL_COMMITED, snapshot is not max scn, return OB_NOT_SUPPORTED
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_ALL_COMMITED, 80/*snapshot*/);
  ASSERT_EQ(OB_NOT_SUPPORTED, ret);
  // mode is READ_ALL_COMMITED, allow to get tablet
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_ALL_COMMITED, ObTransVersion::MAX_TRANS_VERSION/*snapshot*/);
  ASSERT_EQ(OB_SUCCESS, ret);

  // begin transfer out deleted transaction
  user_data.tablet_status_ = ObTabletStatus::TRANSFER_OUT_DELETED;
  user_data.transfer_scn_ = share::SCN::plus(min_scn, 100);
  user_data.transfer_ls_id_ = share::ObLSID(1010);
  user_data.data_type_ = ObTabletMdsUserDataType::FINISH_TRANSFER_OUT;
  user_data.create_commit_scn_ = share::SCN::plus(min_scn, 50);
  user_data.create_commit_version_ = 50;
  user_data.delete_commit_scn_ = share::SCN::plus(min_scn, 200);
  user_data.delete_commit_version_ = 200;

  mds::MdsCtx ctx2(mds::MdsWriter(transaction::ObTransID(456)));
  ret = tablet->set(user_data, ctx2);
  ASSERT_EQ(OB_SUCCESS, ret);

  // disable cache
  {
    SpinWLockGuard guard(tablet->mds_cache_lock_);
    tablet->tablet_status_cache_.reset();
  }

  // mode is READ_READABLE_COMMITED, snpashot status is transfer out, not allow to get
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_READABLE_COMMITED, ObTransVersion::MAX_TRANS_VERSION/*snapshot*/);
  ASSERT_EQ(OB_TABLET_NOT_EXIST, ret);
  // mode is READ_ALL_COMMITED, allow to get tablet whose snapshot status is transfer out
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_ALL_COMMITED, ObTransVersion::MAX_TRANS_VERSION/*snapshot*/);
  ASSERT_EQ(OB_SUCCESS, ret);

  // transaction commit
  commit_scn = share::SCN::plus(min_scn, 220);
  ctx2.single_log_commit(commit_scn, commit_scn);

  // mode is READ_READABLE_COMMITED, snapshot status is transfer out deleted, not allow to get
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_READABLE_COMMITED, ObTransVersion::MAX_TRANS_VERSION/*snapshot*/);
  ASSERT_EQ(OB_TABLET_NOT_EXIST, ret);
  // mode is READ_ALL_COMMITED, snapshot status is transfer out deleted, not allow to get
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_ALL_COMMITED, ObTransVersion::MAX_TRANS_VERSION/*snapshot*/);
  ASSERT_EQ(OB_TABLET_NOT_EXIST, ret);

  // begin transfer out deleted transaction
  user_data.tablet_status_ = ObTabletStatus::DELETED;
  user_data.transfer_scn_ = share::SCN::plus(min_scn, 100);
  user_data.transfer_ls_id_ = share::ObLSID(1010);
  user_data.data_type_ = ObTabletMdsUserDataType::REMOVE_TABLET;
  user_data.create_commit_scn_ = share::SCN::plus(min_scn, 50);
  user_data.create_commit_version_ = 50;
  user_data.delete_commit_scn_ = share::SCN::plus(min_scn, 200);
  user_data.delete_commit_version_ = 200;

  mds::MdsCtx ctx3(mds::MdsWriter(transaction::ObTransID(789)));
  ret = tablet->set(user_data, ctx3);
  ASSERT_EQ(OB_SUCCESS, ret);

  // disable cache
  {
    SpinWLockGuard guard(tablet->mds_cache_lock_);
    tablet->tablet_status_cache_.reset();
  }
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_READABLE_COMMITED, 100/*snapshot*/);
  ASSERT_EQ(OB_SUCCESS, ret);

  ObLSHandle ls_handle;
  ret = MTL(ObLSService*)->get_ls(LS_ID, ls_handle, ObLSGetMod::STORAGE_MOD);
  ASSERT_EQ(OB_SUCCESS, ret);
  ObLS *ls = ls_handle.get_ls();
  ASSERT_NE(nullptr, ls);

  ret = ls->get_tablet_svr()->update_tablet_to_empty_shell(tablet_id);
  ASSERT_EQ(OB_SUCCESS, ret);

  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_READABLE_COMMITED, 100/*snapshot*/);
  ASSERT_EQ(OB_TABLET_NOT_EXIST, ret);
}

TEST_F(TestTabletStatusCache, get_transfer_deleted)
{
  int ret = OB_SUCCESS;
  // create tablet
  const common::ObTabletID tablet_id(ObTimeUtility::fast_current_time() % 10000000000000);
  ObTabletHandle tablet_handle;
  ret = create_tablet(tablet_id, tablet_handle);
  ASSERT_EQ(OB_SUCCESS, ret);

  ObTablet *tablet = tablet_handle.get_obj();
  ASSERT_NE(nullptr, tablet);

  ObLSHandle ls_handle;
  ret = MTL(ObLSService*)->get_ls(LS_ID, ls_handle, ObLSGetMod::STORAGE_MOD);
  ASSERT_EQ(OB_SUCCESS, ret);
  ObLS *ls = ls_handle.get_ls();
  ASSERT_NE(nullptr, ls);

  ObTabletCreateDeleteMdsUserData user_data;
  share::SCN min_scn;
  min_scn.set_min();
  // set transfer scn
  // create commit scn: 50
  // transfer scn: 100
  // delete commit scn: 200
  // begin deleted transaction
  user_data.tablet_status_ = ObTabletStatus::DELETED;
  user_data.transfer_scn_ = share::SCN::plus(min_scn, 100);
  user_data.transfer_ls_id_ = share::ObLSID(1010);
  user_data.data_type_ = ObTabletMdsUserDataType::REMOVE_TABLET;
  user_data.create_commit_scn_ = share::SCN::plus(min_scn, 50);
  user_data.create_commit_version_ = 50;
  user_data.delete_commit_scn_ = share::SCN::plus(min_scn, 200);
  user_data.delete_commit_version_ = 200;

  mds::MdsCtx ctx(mds::MdsWriter(transaction::ObTransID(789)));
  ret = tablet->set(user_data, ctx);
  ASSERT_EQ(OB_SUCCESS, ret);
  share::SCN commit_scn = share::SCN::plus(min_scn, 120);
  ctx.single_log_commit(commit_scn, commit_scn);

  // disable cache
  {
    SpinWLockGuard guard(tablet->mds_cache_lock_);
    tablet->tablet_status_cache_.reset();
  }

  const ObTabletMapKey key(LS_ID, tablet_id);
  tablet_handle.reset();
  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_READABLE_COMMITED, 100/*snapshot*/);
  ASSERT_EQ(OB_SUCCESS, ret);

  ret = ls->get_tablet_svr()->update_tablet_to_empty_shell(tablet_id);
  ASSERT_EQ(OB_SUCCESS, ret);

  ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, tablet_handle, 1 * 1000 * 1000/*timeout_us*/,
      ObMDSGetTabletMode::READ_READABLE_COMMITED, 100/*snapshot*/);
  ASSERT_EQ(OB_TABLET_NOT_EXIST, ret);
}
} // namespace storage
} // namespace oceanbase

int main(int argc, char **argv)
{
  system("rm -f test_tablet_status_cache.log*");
  oceanbase::common::ObLogger::get_logger().set_log_level("INFO");
  OB_LOGGER.set_file_name("test_tablet_status_cache.log", true);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

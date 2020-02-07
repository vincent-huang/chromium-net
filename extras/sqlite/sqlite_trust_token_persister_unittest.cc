// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/extras/sqlite/sqlite_trust_token_persister.h"

#include <memory>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/run_loop.h"
#include "base/task/post_task.h"
#include "base/test/bind_test_util.h"
#include "base/test/task_environment.h"
#include "base/threading/thread_task_runner_handle.h"
#include "components/sqlite_proto/key_value_data.h"
#include "net/extras/sqlite/trust_token_database_owner.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace net {

// This file contains tests specific to the SQLite persister. The class's basic
// functionality is also covered by the parameterized test
// TrustTokenPersisterUnittest.
//
// Test that writing, saving to disk, reinitializing, and reading yields the
// written value.
TEST(SQLiteTrustTokenPersister, PutReinitializeAndGet) {
  base::test::TaskEnvironment env;

  base::FilePath temp_path;
  ASSERT_TRUE(base::CreateTemporaryFile(&temp_path));
  base::File temp_file(temp_path,
                       base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
  ASSERT_TRUE(temp_file.IsValid());

  auto origin = url::Origin::Create(GURL("https://a.com/"));
  {
    std::unique_ptr<SQLiteTrustTokenPersister> persister;
    SQLiteTrustTokenPersister::CreateForFilePath(
        base::ThreadTaskRunnerHandle::Get(), temp_path,
        /*flush_delay_for_writes=*/base::TimeDelta(),
        base::BindLambdaForTesting(
            [&persister](std::unique_ptr<SQLiteTrustTokenPersister> created) {
              persister = std::move(created);
              base::RunLoop().Quit();
            }));
    env.RunUntilIdle();  // Allow initialization to complete.
    ASSERT_TRUE(persister);

    TrustTokenIssuerConfig config;
    config.set_batch_size(5);

    auto config_to_store = std::make_unique<TrustTokenIssuerConfig>(config);
    persister->SetIssuerConfig(origin, std::move(config_to_store));

    env.RunUntilIdle();  // Allow the write to persist.
  }

  std::unique_ptr<SQLiteTrustTokenPersister> persister;
  SQLiteTrustTokenPersister::CreateForFilePath(
      base::ThreadTaskRunnerHandle::Get(), temp_path,
      /*flush_delay_for_writes=*/base::TimeDelta(),
      base::BindLambdaForTesting(
          [&persister](std::unique_ptr<SQLiteTrustTokenPersister> created) {
            persister = std::move(created);
            base::RunLoop().Quit();
          }));
  env.RunUntilIdle();  // Allow initialization to complete.
  ASSERT_TRUE(persister);

  auto got = persister->GetIssuerConfig(origin);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->batch_size(), 5);

  persister.reset();
  // Wait until the persister's TrustTokenDatabaseOwner finishes closing its
  // database asynchronously, so as not to leak after the test concludes.
  env.RunUntilIdle();

  base::DeleteFile(temp_path, false);
}

}  // namespace net

// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_EXTRAS_SQLITE_SQLITE_TRUST_TOKEN_PERSISTER_H_
#define NET_EXTRAS_SQLITE_SQLITE_TRUST_TOKEN_PERSISTER_H_

#include <memory>

#include "base/component_export.h"
#include "base/files/file_path.h"
#include "base/task/task_traits.h"
#include "components/sqlite_proto/key_value_data.h"
#include "net/extras/sqlite/trust_token_database_owner.h"
#include "net/trust_tokens/proto/storage.pb.h"
#include "net/trust_tokens/trust_token_persister.h"
#include "sql/database.h"

namespace base {
class SequencedTaskRunner;
}  // namespace base

namespace net {

// An SQLiteTrustTokenPersister implements low-level get and put operations on
// Trust Tokens types by managing a collection of tables in an underlying SQLite
// database.
//
// It uses the //components/sqlite_proto database management
// utility to avoid dealing with too much database logic directly.
class COMPONENT_EXPORT(NET_EXTRAS) SQLiteTrustTokenPersister
    : public TrustTokenPersister {
 public:
  // Constructs a SQLiteTrustTokenPersister backed by |database_owner|.
  explicit SQLiteTrustTokenPersister(
      std::unique_ptr<TrustTokenDatabaseOwner> database_owner);

  ~SQLiteTrustTokenPersister() override;

  // Constructs a SQLiteTrustTokenPersister backed by an on-disk
  // database:
  // - |db_task_runner| will be used for posting blocking database IO;
  // - |path| will store the database.
  // - |flush_delay_for_writes| is the maximum time before each write is flushed
  // to the underlying database.
  //
  // |on_done_initializing| will be called once the persister's underlying
  // state has been initialized from disk.
  static void CreateForFilePath(
      scoped_refptr<base::SequencedTaskRunner> db_task_runner,
      const base::FilePath& path,
      base::TimeDelta flush_delay_for_writes,
      base::OnceCallback<void(std::unique_ptr<SQLiteTrustTokenPersister>)>
          on_done_initializing);

  // TrustTokenPersister implementation:

  // Preconditions:
  // - All of these methods require that ther Origin inputs' schemes must be
  // HTTP or HTTPS.
  //
  // Postconditions:
  // - Each getter returns nullptr when the requested record was not found.
  std::unique_ptr<TrustTokenIssuerConfig> GetIssuerConfig(
      const url::Origin& issuer) override;
  std::unique_ptr<TrustTokenToplevelConfig> GetToplevelConfig(
      const url::Origin& toplevel) override;
  std::unique_ptr<TrustTokenIssuerToplevelPairConfig>
  GetIssuerToplevelPairConfig(const url::Origin& issuer,
                              const url::Origin& toplevel) override;

  void SetIssuerConfig(const url::Origin& issuer,
                       std::unique_ptr<TrustTokenIssuerConfig> config) override;
  void SetToplevelConfig(
      const url::Origin& toplevel,
      std::unique_ptr<TrustTokenToplevelConfig> config) override;
  void SetIssuerToplevelPairConfig(
      const url::Origin& issuer,
      const url::Origin& toplevel,
      std::unique_ptr<TrustTokenIssuerToplevelPairConfig> config) override;

 private:
  // Manages the underlying database.
  std::unique_ptr<TrustTokenDatabaseOwner> database_owner_;
};

}  // namespace net

#endif  // NET_EXTRAS_SQLITE_SQLITE_TRUST_TOKEN_PERSISTER_H_

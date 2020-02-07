// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/extras/sqlite/trust_token_database_owner.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/ptr_util.h"
#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "base/sequenced_task_runner.h"
#include "components/sqlite_proto/key_value_data.h"
#include "components/sqlite_proto/key_value_table.h"
#include "components/sqlite_proto/proto_table_manager.h"
#include "net/trust_tokens/proto/storage.pb.h"

namespace net {

namespace {
const char kIssuerTableName[] = "trust_tokens_issuer_config";
const char kToplevelTableName[] = "trust_tokens_toplevel_config";
const char kIssuerToplevelPairTableName[] =
    "trust_tokens_issuer_toplevel_pair_config";

}  // namespace

void TrustTokenDatabaseOwner::Create(
    base::OnceCallback<bool(sql::Database*)> db_opener,
    scoped_refptr<base::SequencedTaskRunner> db_task_runner,
    base::TimeDelta flush_delay_for_writes,
    base::OnceCallback<void(std::unique_ptr<TrustTokenDatabaseOwner>)>
        on_done_initializing) {
  DCHECK(db_opener);
  DCHECK(on_done_initializing);

  // No leak: the constructed object wraps itself in a unique_ptr and passes
  // that pointer to |on_done_initializing|.
  new TrustTokenDatabaseOwner(std::move(db_opener), std::move(db_task_runner),
                              flush_delay_for_writes,
                              std::move(on_done_initializing));
}

TrustTokenDatabaseOwner::~TrustTokenDatabaseOwner() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  db_task_runner_->DeleteSoon(FROM_HERE, backing_database_.release());
}

sqlite_proto::KeyValueData<TrustTokenIssuerConfig>*
TrustTokenDatabaseOwner::IssuerData() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return issuer_data_.get();
}

sqlite_proto::KeyValueData<TrustTokenToplevelConfig>*
TrustTokenDatabaseOwner::ToplevelData() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return toplevel_data_.get();
}

sqlite_proto::KeyValueData<TrustTokenIssuerToplevelPairConfig>*
TrustTokenDatabaseOwner::IssuerToplevelPairData() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return issuer_toplevel_pair_data_.get();
}

TrustTokenDatabaseOwner::TrustTokenDatabaseOwner(
    base::OnceCallback<bool(sql::Database*)> db_opener,
    scoped_refptr<base::SequencedTaskRunner> db_task_runner,
    base::TimeDelta flush_delay_for_writes,
    base::OnceCallback<void(std::unique_ptr<TrustTokenDatabaseOwner>)>
        on_done_initializing)
    : on_done_initializing_(std::move(on_done_initializing)),
      table_manager_(base::MakeRefCounted<sqlite_proto::ProtoTableManager>(
          db_task_runner)),
      db_task_runner_(db_task_runner),
      backing_database_(std::make_unique<sql::Database>()),
      issuer_table_(
          std::make_unique<sqlite_proto::KeyValueTable<TrustTokenIssuerConfig>>(
              kIssuerTableName)),
      issuer_data_(
          std::make_unique<sqlite_proto::KeyValueData<TrustTokenIssuerConfig>>(
              table_manager_,
              issuer_table_.get(),
              /*max_num_entries=*/base::nullopt,
              flush_delay_for_writes)),
      toplevel_table_(std::make_unique<
                      sqlite_proto::KeyValueTable<TrustTokenToplevelConfig>>(
          kToplevelTableName)),
      toplevel_data_(std::make_unique<
                     sqlite_proto::KeyValueData<TrustTokenToplevelConfig>>(
          table_manager_,
          toplevel_table_.get(),
          /*max_num_entries=*/base::nullopt,
          flush_delay_for_writes)),
      issuer_toplevel_pair_table_(
          std::make_unique<
              sqlite_proto::KeyValueTable<TrustTokenIssuerToplevelPairConfig>>(
              kIssuerToplevelPairTableName)),
      issuer_toplevel_pair_data_(
          std::make_unique<
              sqlite_proto::KeyValueData<TrustTokenIssuerToplevelPairConfig>>(
              table_manager_,
              issuer_toplevel_pair_table_.get(),
              /*max_num_entries=*/base::nullopt,
              flush_delay_for_writes)) {
  // These two lines are boilerplate copied from predictor_database.cc.
  backing_database_->set_histogram_tag("TrustTokens");
  // We have to call this because the database doesn't have a "meta" table.
  backing_database_->set_mmap_alt_status();

  // Because TrustTokenDatabaseOwners are only constructed through an
  // asynchronous factory method, they are impossible to delete prior to their
  // initialization concluding.
  db_task_runner->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&TrustTokenDatabaseOwner::InitializeMembersOnDbSequence,
                     base::Unretained(this), std::move(db_opener)),
      base::BindOnce(
          &TrustTokenDatabaseOwner::FinishInitializationOnMainSequence,
          base::Unretained(this)));
}

void TrustTokenDatabaseOwner::InitializeMembersOnDbSequence(
    base::OnceCallback<bool(sql::Database*)> db_opener) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());

  if (backing_database_ && !std::move(db_opener).Run(backing_database_.get())) {
    // Giving a nullptr database to ProtoTableManager results in the
    // operations it executes no-opping, so KeyValueData will fall back to
    // reasonable behavior of caching operations' results in memory but not
    // writing them to disk.
    backing_database_.reset();
  }

  DCHECK(!backing_database_ || backing_database_->is_open());

  if (backing_database_)
    backing_database_->Preload();

  table_manager_->InitializeOnDbSequence(
      backing_database_.get(),
      std::vector<std::string>{kIssuerTableName, kToplevelTableName,
                               kIssuerToplevelPairTableName});

  issuer_data_->InitializeOnDBSequence();
  toplevel_data_->InitializeOnDBSequence();
  issuer_toplevel_pair_data_->InitializeOnDBSequence();
}

void TrustTokenDatabaseOwner::FinishInitializationOnMainSequence() {
  // Note: If the backing database fails to initialize,
  // InitializeMembersOnDbSequence will provide the table manager a null DB
  // pointer, which will make attempts to execute database operations no-op,
  // so that the KeyValueData handles fall back to just storing this session's
  // Trust Tokens data in memory.
  //
  // We still consider the TrustTokenDatabaseOwner as having initialized
  // "successfully" in this case, since it is safe to execute operations in
  // this single-session fallback state.
  DCHECK(on_done_initializing_);
  std::move(on_done_initializing_).Run(base::WrapUnique(this));
}

}  // namespace net

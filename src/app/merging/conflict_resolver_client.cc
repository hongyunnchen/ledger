// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/merging/conflict_resolver_client.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "apps/ledger/src/app/diff_utils.h"
#include "apps/ledger/src/app/fidl/serialization_size.h"
#include "apps/ledger/src/app/page_manager.h"
#include "apps/ledger/src/app/page_utils.h"
#include "apps/ledger/src/callback/waiter.h"
#include "lib/ftl/functional/closure.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/memory/weak_ptr.h"
#include "lib/mtl/socket/strings.h"

namespace ledger {

ConflictResolverClient::ConflictResolverClient(
    storage::PageStorage* storage,
    PageManager* page_manager,
    ConflictResolver* conflict_resolver,
    std::unique_ptr<const storage::Commit> left,
    std::unique_ptr<const storage::Commit> right,
    std::unique_ptr<const storage::Commit> ancestor,
    ftl::Closure on_done)
    : storage_(storage),
      manager_(page_manager),
      conflict_resolver_(conflict_resolver),
      left_(std::move(left)),
      right_(std::move(right)),
      ancestor_(std::move(ancestor)),
      on_done_(std::move(on_done)),
      merge_result_provider_binding_(this),
      weak_factory_(this) {
  FTL_DCHECK(left_->GetTimestamp() >= right_->GetTimestamp());
  FTL_DCHECK(on_done_);
}

ConflictResolverClient::~ConflictResolverClient() {
  if (journal_) {
    journal_->Rollback();
  }
}

void ConflictResolverClient::Start() {
  // Prepare the journal for the merge commit.
  storage::Status status =
      storage_->StartMergeCommit(left_->GetId(), right_->GetId(), &journal_);
  if (status != storage::Status::OK) {
    FTL_LOG(ERROR) << "Unable to start merge commit: " << status;
    Finalize();
    return;
  }

  PageSnapshotPtr page_snapshot_ancestor;
  manager_->BindPageSnapshot(ancestor_->Clone(),
                             page_snapshot_ancestor.NewRequest(), "");

  PageSnapshotPtr page_snapshot_left;
  manager_->BindPageSnapshot(left_->Clone(), page_snapshot_left.NewRequest(),
                             "");

  PageSnapshotPtr page_snapshot_right;
  manager_->BindPageSnapshot(right_->Clone(), page_snapshot_right.NewRequest(),
                             "");

  in_client_request_ = true;
  conflict_resolver_->Resolve(std::move(page_snapshot_left),
                              std::move(page_snapshot_right),
                              std::move(page_snapshot_ancestor),
                              merge_result_provider_binding_.NewBinding());
}

void ConflictResolverClient::Cancel() {
  cancelled_ = true;
  if (in_client_request_) {
    Finalize();
  }
}

void ConflictResolverClient::OnNextMergeResult(
    const MergedValuePtr& merged_value,
    const ftl::RefPtr<callback::Waiter<storage::Status, storage::ObjectId>>&
        waiter) {
  switch (merged_value->source) {
    case ValueSource::RIGHT: {
      std::string key = convert::ToString(merged_value->key);
      storage_->GetEntryFromCommit(
          *right_, key,
          [ key, callback = waiter->NewCallback() ](storage::Status status,
                                                    storage::Entry entry) {
            if (status != storage::Status::OK) {
              if (status == storage::Status::NOT_FOUND) {
                FTL_LOG(ERROR)
                    << "Key " << key
                    << " is not present in the right change. Unable to proceed";
              }
              callback(status, storage::ObjectId());
              return;
            }
            callback(storage::Status::OK, entry.object_id);
          });
      break;
    }
    case ValueSource::NEW: {
      if (merged_value->new_value->is_bytes()) {
        // TODO(etiennej): Use asynchronous write, otherwise the run loop will
        // block until the socket is drained.
        mx::socket socket = mtl::WriteStringToSocket(
            convert::ToStringView(merged_value->new_value->get_bytes()));
        storage_->AddObjectFromLocal(
            std::move(socket), merged_value->new_value->get_bytes().size(),
            ftl::MakeCopyable([callback = waiter->NewCallback()](
                storage::Status status, storage::ObjectId object_id) {
              callback(status, std::move(object_id));
            }));
      } else {
        waiter->NewCallback()(
            storage::Status::OK,
            convert::ToString(
                merged_value->new_value->get_reference()->opaque_id));
      }
      break;
    }
    case ValueSource::DELETE: {
      journal_->Delete(merged_value->key);
      waiter->NewCallback()(storage::Status::OK, storage::ObjectId());
      break;
    }
  }
}

void ConflictResolverClient::Finalize() {
  if (journal_) {
    journal_->Rollback();
    journal_.reset();
  }
  auto on_done = std::move(on_done_);
  on_done_ = nullptr;
  on_done();
}

// GetLeftDiff(array<uint8>? token)
//     => (Status status, PageChange? change, array<uint8>? next_token);
void ConflictResolverClient::GetLeftDiff(fidl::Array<uint8_t> token,
                                         const GetLeftDiffCallback& callback) {
  GetDiff(*left_, std::move(token), callback);
}

// GetRightDiff(array<uint8>? token)
//     => (Status status, PageChange? change, array<uint8>? next_token);
void ConflictResolverClient::GetRightDiff(
    fidl::Array<uint8_t> token,
    const GetRightDiffCallback& callback) {
  GetDiff(*right_, std::move(token), callback);
}

void ConflictResolverClient::GetDiff(
    const storage::Commit& commit,
    fidl::Array<uint8_t> token,
    const std::function<void(Status, PageChangePtr, fidl::Array<uint8_t>)>&
        callback) {
  diff_utils::ComputePageChange(
      storage_, *ancestor_, commit, "", convert::ToString(token),
      fidl_serialization::kMaxInlineDataSize,
      [
        weak_this = weak_factory_.GetWeakPtr(), callback = std::move(callback)
      ](Status status,
        std::pair<PageChangePtr, std::string> page_change) mutable {
        if (!weak_this) {
          callback(Status::INTERNAL_ERROR, nullptr, nullptr);
          return;
        }
        if (weak_this->cancelled_) {
          callback(Status::INTERNAL_ERROR, nullptr, nullptr);
          weak_this->Finalize();
          return;
        }
        if (status != Status::OK) {
          FTL_LOG(ERROR) << "Unable to compute diff due to error " << status
                         << ", aborting.";
          callback(status, nullptr, nullptr);
          weak_this->Finalize();
          return;
        }

        const std::string& next_token = page_change.second;
        status = next_token.empty() ? Status::OK : Status::PARTIAL_RESULT;
        callback(status, std::move(page_change.first),
                 next_token.empty() ? nullptr : convert::ToArray(next_token));
      });
}

// Merge(array<MergedValue>? merge_changes) => (Status status);
void ConflictResolverClient::Merge(fidl::Array<MergedValuePtr> merged_values,
                                   const MergeCallback& callback) {
  operation_serializer_.Serialize(
      callback, ftl::MakeCopyable([
        weak_this = weak_factory_.GetWeakPtr(),
        merged_values = std::move(merged_values)
      ](MergeCallback callback) mutable {
        if (!weak_this) {
          callback(Status::INTERNAL_ERROR);
          return;
        }
        auto waiter =
            callback::Waiter<storage::Status, storage::ObjectId>::Create(
                storage::Status::OK);
        for (const MergedValuePtr& merged_value : merged_values) {
          weak_this->OnNextMergeResult(merged_value, waiter);
        }
        waiter->Finalize(ftl::MakeCopyable(ftl::MakeCopyable([
          weak_this, merged_values = std::move(merged_values),
          callback = std::move(callback)
        ](storage::Status status, std::vector<storage::ObjectId> object_ids) {
          if (!weak_this) {
            callback(Status::INTERNAL_ERROR);
            return;
          }
          if (weak_this->cancelled_ || status != storage::Status::OK) {
            // An eventual error was logged before, no need to do it again here.
            weak_this->Finalize();
            callback(weak_this->cancelled_ ? Status::INTERNAL_ERROR
                                           : PageUtils::ConvertStatus(status));
            return;
          }

          for (size_t i = 0; i < object_ids.size(); ++i) {
            if (object_ids[i].empty()) {
              continue;
            }
            storage::Status status = weak_this->journal_->Put(
                merged_values[i]->key, object_ids[i],
                merged_values[i]->priority == Priority::EAGER
                    ? storage::KeyPriority::EAGER
                    : storage::KeyPriority::LAZY);
            if (status != storage::Status::OK) {
              callback(PageUtils::ConvertStatus(status));
              return;
            }
          }
          callback(Status::OK);
        })));
      }));
}

// Done() => (Status status);
void ConflictResolverClient::Done(const DoneCallback& callback) {
  in_client_request_ = false;
  FTL_DCHECK(!cancelled_);
  FTL_DCHECK(journal_);

  journal_->Commit([
    weak_this = weak_factory_.GetWeakPtr(), callback = std::move(callback)
  ](storage::Status status, std::unique_ptr<const storage::Commit>) {
    if (status != storage::Status::OK) {
      FTL_LOG(ERROR) << "Unable to commit merge journal: " << status;
    }
    callback(PageUtils::ConvertStatus(status));
    if (weak_this) {
      weak_this->journal_.reset();
      weak_this->Finalize();
    }
  });
}

}  // namespace ledger

// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/merging/merge_resolver.h"

#include <algorithm>
#include <memory>
#include <queue>
#include <set>

#include "apps/ledger/src/app/merging/common_ancestor.h"
#include "apps/ledger/src/app/merging/ledger_merge_manager.h"
#include "apps/ledger/src/app/merging/merge_strategy.h"
#include "apps/ledger/src/app/page_manager.h"
#include "apps/ledger/src/app/page_utils.h"
#include "apps/ledger/src/callback/waiter.h"
#include "apps/ledger/src/glue/crypto/rand.h"
#include "lib/ftl/functional/auto_call.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/memory/weak_ptr.h"
#include "lib/mtl/tasks/message_loop.h"

namespace ledger {

MergeResolver::MergeResolver(ftl::Closure on_destroyed,
                             Environment* environment,
                             storage::PageStorage* storage)
    : storage_(storage),
      environment_(environment),
      wait_distribution_(0, environment_->max_merging_delay().ToMilliseconds()),
      rng_(glue::RandUint64()),
      on_destroyed_(on_destroyed),
      weak_ptr_factory_(this) {
  storage_->AddCommitWatcher(this);
  PostCheckConflicts();
}

MergeResolver::~MergeResolver() {
  storage_->RemoveCommitWatcher(this);
  on_destroyed_();
}

void MergeResolver::set_on_empty(ftl::Closure on_empty_callback) {
  on_empty_callback_ = std::move(on_empty_callback);
}

bool MergeResolver::IsEmpty() {
  return !merge_in_progress_;
}

void MergeResolver::SetMergeStrategy(std::unique_ptr<MergeStrategy> strategy) {
  if (merge_in_progress_) {
    FTL_DCHECK(strategy_);
    next_strategy_ = std::move(strategy);
    strategy_->Cancel();
    return;
  }
  strategy_.swap(strategy);
  if (strategy_) {
    PostCheckConflicts();
  }
}

void MergeResolver::SetPageManager(PageManager* page_manager) {
  FTL_DCHECK(page_manager_ == nullptr);
  page_manager_ = page_manager;
}

void MergeResolver::OnNewCommits(
    const std::vector<std::unique_ptr<const storage::Commit>>& commits,
    storage::ChangeSource source) {
  PostCheckConflicts();
}

void MergeResolver::PostCheckConflicts() {
  mtl::MessageLoop::GetCurrent()->task_runner()->PostDelayedTask(
      [weak_this_ptr = weak_ptr_factory_.GetWeakPtr()]() {
        if (weak_this_ptr) {
          weak_this_ptr->CheckConflicts();
        }
      },
      ftl::TimeDelta::FromMilliseconds(wait_distribution_(rng_)));
}
void MergeResolver::CheckConflicts() {
  if (!strategy_ || merge_in_progress_) {
    // No strategy, or a merge already in progress. Let's bail out early.
    return;
  }

  std::vector<storage::CommitId> heads;
  storage::Status s = storage_->GetHeadCommitIds(&heads);
  FTL_DCHECK(s == storage::Status::OK);
  if (heads.size() == 1) {
    // No conflict.
    return;
  }
  ResolveConflicts(std::move(heads));
}

void MergeResolver::ResolveConflicts(std::vector<storage::CommitId> heads) {
  FTL_DCHECK(heads.size() >= 2);
  FTL_DCHECK(std::is_sorted(heads.begin(), heads.end()));

  merge_in_progress_ = true;
  auto cleanup = ftl::MakeAutoCall([this] {
    // |merge_in_progress_| must be reset before calling |on_empty_callback_|.
    merge_in_progress_ = false;

    if (next_strategy_) {
      strategy_ = std::move(next_strategy_);
      next_strategy_.reset();
    }
    PostCheckConflicts();
    // Call on_empty_callback_ at the very end as this might delete this.
    if (on_empty_callback_) {
      on_empty_callback_();
    }
  });

  auto waiter = callback::
      Waiter<storage::Status, std::unique_ptr<const storage::Commit>>::Create(
          storage::Status::OK);
  for (const storage::CommitId& id : heads) {
    storage_->GetCommit(id, waiter->NewCallback());
  }
  waiter->Finalize(ftl::MakeCopyable([ this, cleanup = std::move(cleanup) ](
      storage::Status status,
      std::vector<std::unique_ptr<const storage::Commit>> commits) mutable {
    // Check if the 2 parents have the same content.
    if (commits[0]->GetRootId() == commits[1]->GetRootId()) {
      // In that case, the result must be a commit with the same content, and
      // the smallest timestamp.
      storage_->MergeIdenticalCommits(
          std::move(commits[0]), std::move(commits[1]),
          ftl::MakeCopyable([cleanup =
                                 std::move(cleanup)](storage::Status status) {
            if (status != storage::Status::OK) {
              FTL_LOG(ERROR) << "Unable to merge identical commits.";
            }
          }));
      return;
    }

    // If the strategy has been changed, bail early.
    if (next_strategy_) {
      return;
    }

    if (status != storage::Status::OK) {
      FTL_LOG(ERROR) << "Failed to retrieve head commits.";
      return;
    }
    FTL_DCHECK(commits.size() >= 2);
    std::sort(commits.begin(), commits.end(),
              [](const std::unique_ptr<const storage::Commit>& lhs,
                 const std::unique_ptr<const storage::Commit>& rhs) {
                return lhs->GetTimestamp() < rhs->GetTimestamp();
              });

    // Merge the first two commits using the most recent one as the base.
    auto head1 = std::move(commits[0]);
    auto head2 = std::move(commits[1]);
    FindCommonAncestor(
        environment_->main_runner(), storage_, head1->Clone(), head2->Clone(),
        ftl::MakeCopyable([
          this, head1 = std::move(head1), head2 = std::move(head2),
          cleanup = std::move(cleanup)
        ](Status status,
          std::unique_ptr<const storage::Commit> common_ancestor) mutable {
          // If the strategy has been changed, bail early.
          if (next_strategy_) {
            return;
          }

          if (status != Status::OK) {
            FTL_LOG(ERROR) << "Failed to find common ancestor of head commits.";
            return;
          }
          strategy_->Merge(storage_, page_manager_, std::move(head1),
                           std::move(head2), std::move(common_ancestor),
                           ftl::MakeCopyable([cleanup = std::move(cleanup)]{}));
        }));
  }));
}

}  // namespace ledger

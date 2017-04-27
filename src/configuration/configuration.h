// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_CONFIGURATION_CONFIGURATION_H_
#define APPS_LEDGER_SRC_CONFIGURATION_CONFIGURATION_H_

#include <string>

#include "lib/ftl/strings/string_view.h"

namespace configuration {

constexpr ftl::StringView kDefaultConfigurationFile =
    "/data/ledger/config.json";

// File path under which is stored the last user id for which the Ledger was
// initialized. This is a stop-gap convenience solution to allow `cloud_sync
// clean` to reset the Ledger for the concrete user on the device (and not wipe
// the entire cloud which can be shared between many users).
constexpr ftl::StringView kLastUserIdPath = "/data/ledger/last_user_id";
constexpr ftl::StringView kLastUserRepositoryPath =
    "/data/ledger/last_user_dir";

// Filename under which the server id used to sync a given user is stored within
// the repository dir of that user.
constexpr ftl::StringView kServerIdFilename = "server_id";

// The configuration for the Ledger.
struct Configuration {
  // Creates a default, empty configuration.
  Configuration();
  Configuration(const Configuration&);
  Configuration(Configuration&&);

  Configuration& operator=(const Configuration&);
  Configuration& operator=(Configuration&&);

  // Set to true to enable Cloud Sync. False by default.
  bool use_sync;

  // Cloud Sync parameters.
  struct SyncParams {
    // ID of the firebase instance.
    std::string firebase_id;
  };

  // sync_params holds the parameters used for cloud synchronization if
  // |use_sync| is true.
  SyncParams sync_params;
};

bool operator==(const Configuration& lhs, const Configuration& rhs);
bool operator!=(const Configuration& lhs, const Configuration& rhs);
bool operator==(const Configuration::SyncParams& lhs,
                const Configuration::SyncParams& rhs);
bool operator!=(const Configuration::SyncParams& lhs,
                const Configuration::SyncParams& rhs);
}  // namespace configuration

#endif  // APPS_LEDGER_SRC_CONFIGURATION_CONFIGURATION_H_

/*  Copyright 2014 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#ifndef MAIDSAFE_VAULT_MANAGER_CONFIG_H_
#define MAIDSAFE_VAULT_MANAGER_CONFIG_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "boost/asio/steady_timer.hpp"

#include "maidsafe/common/type_macros.h"

namespace maidsafe {

namespace vault_manager {

typedef boost::asio::steady_timer Timer;
typedef std::shared_ptr<Timer> TimerPtr;

extern const std::string kConfigFilename;
extern const std::string kBootstrapFilename;
extern const std::chrono::seconds kRpcTimeout;
extern const std::chrono::seconds kVaultStopTimeout;
extern const int kMaxVaultRestarts;

DEFINE_OSTREAMABLE_ENUM_VALUES(MessageType, int32_t,
    (ValidateConnectionRequest)
    (Challenge)
    (ChallengeResponse)
    (ConnectionValidated)
    (StartVaultRequest)
    (TakeOwnershipRequest)
    (VaultRunningResponse)
    (VaultStarted)
    (VaultStartedResponse)
    (VaultShutdownRequest)
    (MaxDiskUsageUpdate)
    (JoinedNetwork)
    (LogMessage)
    (MarkNetworkAsStable)
    (NetworkStableRequest)
    (NetworkStableResponse))

typedef std::pair<std::string, MessageType> MessageAndType;

}  // namespace vault_manager

}  // namespace maidsafe

#endif  // MAIDSAFE_VAULT_MANAGER_CONFIG_H_

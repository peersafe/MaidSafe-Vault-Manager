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

#include "maidsafe/vault_manager/process_manager.h"

#include <algorithm>
#include <type_traits>

#include "boost/process/execute.hpp"
#include "boost/process/initializers.hpp"
#include "boost/process/mitigate.hpp"
#include "boost/process/terminate.hpp"
#include "boost/process/wait_for_exit.hpp"

#include "maidsafe/common/log.h"
#include "maidsafe/common/make_unique.h"
#include "maidsafe/common/on_scope_exit.h"
#include "maidsafe/common/process.h"
#include "maidsafe/common/utils.h"

#include "maidsafe/vault_manager/utils.h"
#include "maidsafe/vault_manager/vault_info.pb.h"

namespace bp = boost::process;
namespace fs = boost::filesystem;

namespace maidsafe {

namespace vault_manager {

namespace {

bool ConnectionsEqual(const TcpConnectionPtr& lhs, const TcpConnectionPtr& rhs) {
  return !lhs.owner_before(rhs) && !rhs.owner_before(lhs);
}

void CheckNewVaultDoesntConflict(const VaultInfo& new_vault, const VaultInfo& existing_vault) {
  if (new_vault.pmid_and_signer && existing_vault.pmid_and_signer &&
      new_vault.pmid_and_signer->first.name() == existing_vault.pmid_and_signer->first.name()) {
    LOG(kError) << "Vault process with Pmid "
                << DebugId(new_vault.pmid_and_signer->first.name().value) << " already exists.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::already_initialised));
  }

  if (new_vault.chunkstore_path == existing_vault.chunkstore_path) {
    LOG(kError) << "Vault process with chunkstore path " << new_vault.chunkstore_path
                << " already exists.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::already_initialised));
  }

  if (new_vault.label == existing_vault.label) {
    LOG(kError) << "Vault process with label " << new_vault.label.string() << " already exists.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::already_initialised));
  }

  if (ConnectionsEqual(new_vault.tcp_connection, existing_vault.tcp_connection)) {
    LOG(kError) << "Vault process with this tcp_connection already exists.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::already_initialised));
  }

#ifdef TESTING
  if (new_vault.identity_index == existing_vault.identity_index) {
    LOG(kError) << "Vault process with identity_index " << new_vault.identity_index
                << " already exists.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::already_initialised));
  }
#endif
}

}  // unnamed namespace

ProcessManager::Child::Child(VaultInfo info, boost::asio::io_service &io_service)
    : info(std::move(info)),
      on_exit(),
      process_args(),
      status(ProcessStatus::kBeforeStarted),
#ifdef MAIDSAFE_WIN32
      process(PROCESS_INFORMATION()),
      handle(io_service) {}
#else
      process(0),
      signal_set(maidsafe::make_unique<boost::asio::signal_set>(io_service_, SIGCHLD)) {}
#endif

ProcessManager::Child::Child(Child&& other)
    : info(std::move(other.info)),
      on_exit(std::move(other.on_exit)),
      process_args(std::move(other.process_args)),
      status(std::move(other.status)),
      process(std::move(other.process)),
#ifdef MAIDSAFE_WIN32
      handle(std::move(other.handle)) {}
#else
      signal_set(std::move(other.signal_set)) {}
#endif

ProcessManager::Child& ProcessManager::Child::operator=(Child other) {
  swap(*this, other);
  return *this;
}

void swap(ProcessManager::Child& lhs, ProcessManager::Child& rhs){
  using std::swap;
  swap(lhs.info, rhs.info);
  swap(lhs.on_exit, rhs.on_exit);
  swap(lhs.process_args, rhs.process_args);
  swap(lhs.status, rhs.status);
  swap(lhs.process, rhs.process);
#ifdef MAIDSAFE_WIN32
  swap(lhs.handle, rhs.handle);
#else
  swap(lhs.signal_set, rhs.signal_set);
#endif
}



ProcessManager::ProcessManager(boost::asio::io_service &io_service, fs::path vault_executable_path,
                               Port listening_port)
    : io_service_(io_service),
      kListeningPort_(listening_port),
      kVaultExecutablePath_(vault_executable_path),
      vaults_(),
      mutex_() {
  static_assert(std::is_same<ProcessId, process::ProcessId>::value,
                "process::ProcessId is statically checked as being of suitable size for holding a "
                "pid_t or DWORD, so vault_manager::ProcessId should use the same type.");
  boost::system::error_code ec;
  if (!fs::exists(kVaultExecutablePath_, ec) || ec) {
    LOG(kError) << kVaultExecutablePath_ << " doesn't exist.  " << (ec ? ec.message() : "");
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::invalid_parameter));
  }
  if (!fs::is_regular_file(kVaultExecutablePath_, ec) || ec) {
    LOG(kError) << kVaultExecutablePath_ << " is not a regular file.  " << (ec ? ec.message() : "");
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::invalid_parameter));
  }
  if (fs::is_symlink(kVaultExecutablePath_, ec) || ec) {
    LOG(kError) << kVaultExecutablePath_ << " is a symlink.  " << (ec ? ec.message() : "");
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::invalid_parameter));
  }
  LOG(kVerbose) << "Vault executable found at " << kVaultExecutablePath_;
}

std::vector<VaultInfo> ProcessManager::GetAll() const {
  std::vector<VaultInfo> all_vaults;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& vault : vaults_)
    all_vaults.push_back(vault.info);
  return all_vaults;
}

void ProcessManager::AddProcess(VaultInfo info) {
  if (info.chunkstore_path.empty()) {
    LOG(kError) << "Can't add vault process - chunkstore path is empty.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::invalid_parameter));
  }
  std::lock_guard<std::mutex> lock{ mutex_ };
  for (const auto& vault : vaults_)
    CheckNewVaultDoesntConflict(info, vault.info);

  // emplace offers strong exception guarantee - only need to cover subsequent calls.
  auto itr(vaults_.emplace(std::end(vaults_), Child{ info, io_service_ }));
  on_scope_exit strong_guarantee{ [this, itr] { vaults_.erase(itr); } };
  StartProcess(itr);
  strong_guarantee.Release();
}

VaultInfo ProcessManager::HandleVaultStarted(TcpConnectionPtr connection, ProcessId process_id) {
  std::lock_guard<std::mutex> lock{ mutex_ };
  auto itr(std::find_if(std::begin(vaults_), std::end(vaults_),
                        [this, process_id](const Child& vault) {
                          return GetProcessId(vault) == process_id;
                        }));
  if (itr == std::end(vaults_)) {
    LOG(kError) << "Failed to find vault with process ID " << process_id << " in child processes.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::no_such_element));
  }
  itr->info.tcp_connection = connection;
  return itr->info;
}

void ProcessManager::AssignOwner(const NonEmptyString& label,
                                 const passport::PublicMaid::Name& owner_name,
                                 DiskUsage max_disk_usage) {
  std::lock_guard<std::mutex> lock{ mutex_ };
  auto itr(DoFind(label));
  itr->info.owner_name = owner_name;
  itr->info.max_disk_usage = max_disk_usage;
}

bool ProcessManager::StopProcess(TcpConnectionPtr connection,
                                 std::function<void(maidsafe_error, int)> functor) {
  std::lock_guard<std::mutex> lock{ mutex_ };
  auto itr(std::find_if(std::begin(vaults_), std::end(vaults_),
                        [this, connection](const Child& vault) {
                          return ConnectionsEqual(vault.info.tcp_connection, connection);
                        }));
  if (itr == std::end(vaults_)) {
    LOG(kWarning) << "Vault process doesn't exist.";
    return false;
  }
  itr->on_exit = functor;
  StopProcess(*itr);
  return true;
}

void ProcessManager::StartProcess(std::vector<Child>::iterator itr) {
  if (itr->status != ProcessStatus::kBeforeStarted) {
    LOG(kError) << "Process has already been started.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::already_initialised));
  }

  std::vector<std::string> args{ 1, kVaultExecutablePath_.string() };
  args.emplace_back("--vm_port " + std::to_string(kListeningPort_));
  args.insert(std::end(args), std::begin(itr->process_args), std::end(itr->process_args));

  NonEmptyString label{ itr->info.label };

#ifndef MAIDSAFE_WIN32
  itr->signal_set->async_wait([this, label](const boost::system::error_code&, int) {
    int exit_code;
    wait(&exit_code);
    OnProcessExit(label, BOOST_PROCESS_EXITSTATUS(exit_code));
  });
#endif

  itr->process = bp::execute(
      bp::initializers::run_exe(kVaultExecutablePath_),
      bp::initializers::set_cmd_line(process::ConstructCommandLine(args)),
#ifndef MAIDSAFE_WIN32
      bp::notify_io_service(io_service_),
#endif
      bp::initializers::throw_on_error(),
      bp::initializers::inherit_env());

  itr->status = ProcessStatus::kRunning;

#ifdef MAIDSAFE_WIN32
  itr->handle.assign(itr->process.process_handle());
  HANDLE native_handle{ itr->handle.native_handle() };
  itr->handle.async_wait([this, label, native_handle](const boost::system::error_code&) {
    DWORD exit_code;
    GetExitCodeProcess(native_handle, &exit_code);
    OnProcessExit(label, BOOST_PROCESS_EXITSTATUS(exit_code));
  });
#endif

}

void ProcessManager::StopProcess(Child& vault) {
  vault.status = ProcessStatus::kStopping;
  NonEmptyString label{ vault.info.label };
  TimerPtr timer{ std::make_shared<Timer>(io_service_, kVaultStopTimeout) };
  timer->async_wait([this, label, timer](const boost::system::error_code& error_code) {
    if (error_code && error_code == boost::asio::error::operation_aborted) {
      LOG(kVerbose) << "Vault termination timer cancelled OK.";
      return;
    }
    LOG(kWarning) << "Timed out waiting for Vault to stop; terminating now.";
    std::function<void(maidsafe_error, int)> on_exit;
    maidsafe_error error{ MakeError(CommonErrors::success) };
    try {
      std::lock_guard<std::mutex> lock{ mutex_ };
      auto itr(DoFind(label));
      on_exit = itr->on_exit;
      bp::terminate(itr->process);
    }
    catch (const std::exception& e) {
      LOG(kError) << "Error while terminating vault: " << boost::diagnostic_information(e);
    }
    if (on_exit)
      on_exit(MakeError(VaultManagerErrors::vault_terminated), -1);
  });
}

VaultInfo ProcessManager::Find(const NonEmptyString& label) const {
  std::lock_guard<std::mutex> lock{ mutex_ };
  return DoFind(label)->info;
}

std::vector<ProcessManager::Child>::const_iterator ProcessManager::DoFind(
    const NonEmptyString& label) const {
  auto itr(std::find_if(std::begin(vaults_), std::end(vaults_),
                        [this, &label](const Child& vault) {
                          return vault.info.label == label;
                        }));
  if (itr == std::end(vaults_)) {
    LOG(kError) << "Vault process with label " << label.string() << " doesn't exist.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::no_such_element));
  }
  return itr;
}

std::vector<ProcessManager::Child>::iterator ProcessManager::DoFind(const NonEmptyString& label) {
  auto itr(std::find_if(std::begin(vaults_), std::end(vaults_),
                        [this, &label](const Child& vault) {
                          return vault.info.label == label;
                        }));
  if (itr == std::end(vaults_)) {
    LOG(kError) << "Vault process with label " << label.string() << " doesn't exist.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::no_such_element));
  }
  return itr;
}

ProcessId ProcessManager::GetProcessId(const Child& vault) const {
#ifdef MAIDSAFE_WIN32
  return static_cast<ProcessId>(vault.process.proc_info.dwProcessId);
#else
  return static_cast<ProcessId>(vault.process.pid);
#endif
}

bool ProcessManager::IsRunning(const Child& vault) const {
  try {
#ifdef MAIDSAFE_WIN32
    return process::IsRunning(vault.process.process_handle());
#else
    return process::IsRunning(vault.process.pid);
#endif
  }
  catch (const std::exception& e) {
    LOG(kInfo) << boost::diagnostic_information(e);
    return false;
  }
}

void ProcessManager::OnProcessExit(const NonEmptyString& label, int exit_code) {
  std::function<void(maidsafe_error, int)> on_exit;
  {
    std::lock_guard<std::mutex> lock{ mutex_ };
    auto child_itr(std::find_if(std::begin(vaults_), std::end(vaults_),
                   [this, &label](const Child& vault) { return vault.info.label == label; }));
    if (child_itr == std::end(vaults_))
      return;
    on_exit = child_itr->on_exit;
    vaults_.erase(child_itr);
  }
  if (on_exit) {
    if (exit_code == 0)
      on_exit(MakeError(CommonErrors::success), exit_code);
    else
      on_exit(MakeError(VaultManagerErrors::vault_exited_with_error), exit_code);
  }
}

/*
std::vector<ProcessManager::ProcessInfo>::iterator ProcessManager::FindProcess(ProcessIndex index) {
  return std::find_if(processes_.begin(), processes_.end(), [index](ProcessInfo & process_info) {
    return (process_info.index == index);
  });
}

void ProcessManager::StartProcess(ProcessIndex index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto itr = FindProcess(index);
  if (itr == processes_.end())
    return;
  (*itr).done = false;
  (*itr).restart_count = 0;
  LOG(kInfo) << "StartProcess: AddStatus. ID: " << index;
  (*itr).thread =
      std::move(boost::thread([=] { RunProcess(index, false, false); }));
}

void ProcessManager::RunProcess(ProcessIndex index, bool restart, bool logging) {
  std::string process_name;
  std::vector<std::string> process_args;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto itr = FindProcess(index);
    if (itr == processes_.end()) {
      LOG(kError) << "RunProcess: process with specified VMID cannot be found";
      return;
    }
    process_name = (*itr).process.name();
    process_args = (*itr).process.args();
  }

  if (restart) {
    Sleep(std::chrono::milliseconds(600));
    // SetInstruction(id, ProcessInstruction::kRun);
    //    if (logging) {
    //      log::FilterMap filter;
    //      filter["*"] = log::kVerbose;
    //      log::Logging::instance().SetFilter(filter);
    //      log::Logging::instance().SetAsync(true);
    //    }
  }
  boost::system::error_code error_code;
  // TODO(Fraser#5#): 2012-08-29 - Handle logging to a file.  See:
  // http://www.highscore.de/boost/process0.5/boost_process/tutorial.html#boost_process.tutorial.setting_up_standard_streams
  // NOLINT (Fraser)
  SetProcessStatus(index, ProcessStatus::kRunning);
  bp::child child(bp::execute(
      bp::initializers::run_exe(process_name),
      bp::initializers::set_cmd_line(process::ConstructCommandLine(process_args)),
      bp::initializers::set_on_error(error_code),
      bp::initializers::inherit_env()));
  boost::system::error_code error;
  auto exit_code = wait_for_exit(child, error);
  if (error) {
    LOG(kError) << "Error waiting for child to exit: " << error.message();
  }
  SetProcessStatus(index, ProcessStatus::kStopped);
  LOG(kInfo) << "Process " << index << " has completed with exit code " << exit_code;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto itr(FindProcess(index));
    LOG(kInfo) << "Restart count = " << (*itr).restart_count;
    if ((*itr).done)
      return;

    if ((*itr).restart_count > 4) {
      LOG(kInfo) << "A process " << (*itr).index << " is consistently failing. Stopping..."
                 << " Restart count = " << (*itr).restart_count;
      return;
    }

    if ((*itr).restart_count < 3) {
      ++(*itr).restart_count;
      logging = false;
    } else {
      ++(*itr).restart_count;
      logging = true;
    }
  }
  RunProcess(index, true, logging);
}

void ProcessManager::LetProcessDie(ProcessIndex index) {
  LOG(kVerbose) << "LetProcessDie: ID: " << index;
  std::lock_guard<std::mutex> lock(mutex_);
  auto itr = FindProcess(index);
  if (itr == processes_.end())
    return;
  (*itr).done = true;
}

void ProcessManager::LetAllProcessesDie() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& process : processes_)
    process.done = true;
}

void ProcessManager::WaitForProcesses() {
  bool done(false);
  boost::thread thread;
  while (!done) {
    done = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& process : processes_) {
        if (!process.done) {
          done = false;
          break;
        }
        if (process.thread.joinable()) {
          thread = std::move(process.thread);
          done = false;
          break;
        }
      }
    }
    thread.join();
    Sleep(std::chrono::milliseconds(100));
  }
}

void ProcessManager::KillProcess(ProcessIndex index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto itr = FindProcess(index);
  if (itr == processes_.end())
    return;
  (*itr).done = true;
  bp::terminate((*itr).child);
}

void ProcessManager::StopProcess(ProcessIndex index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto itr = FindProcess(index);
  if (itr == processes_.end())
    return;
  (*itr).done = true;
}

void ProcessManager::RestartProcess(ProcessIndex index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto itr = FindProcess(index);
  if (itr == processes_.end())
    return;
  (*itr).done = false;
  // SetInstruction(id, ProcessInstruction::kTerminate);
}

ProcessStatus ProcessManager::GetProcessStatus(ProcessIndex index) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto itr = FindProcess(index);
  if (itr == processes_.end())
    return ProcessStatus::kError;
  return (*itr).status;
}

bool ProcessManager::WaitForProcessToStop(ProcessIndex index) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto itr = FindProcess(index);
  if (itr == processes_.end())
    return false;
  if (cond_var_.wait_for(lock, std::chrono::seconds(5), [&]()->bool {
        return (*itr).status != ProcessStatus::kRunning;
      }))
    return true;
  LOG(kError) << "Wait for process " << index << " to stop timed out. Terminating...";
  lock.unlock();
  KillProcess(index);
  return true;
}

bool ProcessManager::SetProcessStatus(ProcessIndex index, const ProcessStatus& status) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto itr = FindProcess(index);
    if (itr == processes_.end())
      return false;
    (*itr).status = status;
  }
  cond_var_.notify_all();
  return true;
}

void ProcessManager::TerminateAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& process : processes_) {
    LOG(kInfo) << "Terminating: " << process.index << ", port: " << process.port;
    if (process.thread.joinable() && process.status == ProcessStatus::kRunning)
      process.thread.join();
  }
  processes_.clear();
}
*/
}  // namespace vault_manager

}  // namespace maidsafe
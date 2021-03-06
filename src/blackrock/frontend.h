// Sandstorm Blackrock
// Copyright (c) 2015 Sandstorm Development Group, Inc.
// All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BLACKROCK_FRONTEND_H_
#define BLACKROCK_FRONTEND_H_

#include "common.h"
#include <blackrock/frontend.capnp.h>
#include <blackrock/storage.capnp.h>
#include <blackrock/cluster-rpc.capnp.h>
#include <blackrock/worker.capnp.h>
#include <sandstorm/backend.capnp.h>
#include <capnp/message.h>
#include <sandstorm/util.h>
#include <kj/async-io.h>
#include <capnp/rpc.h>
#include <capnp/rpc-twoparty.h>
#include "backend-set.h"
#include "cluster-rpc.h"

namespace blackrock {

class FrontendImpl: public Frontend::Server, private kj::TaskSet::ErrorHandler {
public:
  FrontendImpl(kj::Network& network, kj::Timer& timer, sandstorm::SubprocessSet& subprocessSet,
               FrontendConfig::Reader config, uint replicaNumber,
               kj::PromiseFulfillerPair<sandstorm::Backend::Client> paf =
                   kj::newPromiseAndFulfiller<sandstorm::Backend::Client>());

  void setConfig(FrontendConfig::Reader config);

  BackendSet<StorageRootSet>::Client getStorageRootBackendSet();
  BackendSet<StorageFactory>::Client getStorageFactoryBackendSet();
  BackendSet<Worker>::Client getWorkerBackendSet();
  BackendSet<Mongo>::Client getMongoBackendSet();

private:
  class BackendImpl;
  struct MongoInfo;

  kj::Timer& timer;
  sandstorm::SubprocessSet& subprocessSet;
  kj::Own<capnp::MallocMessageBuilder> configMessage;
  FrontendConfig::Reader config;
  sandstorm::TwoPartyServerWithClientBootstrap capnpServer;

  kj::Own<BackendSetImpl<StorageRootSet>> storageRoots;
  kj::Own<BackendSetImpl<StorageFactory>> storageFactories;
  kj::Own<BackendSetImpl<Worker>> workers;
  kj::Own<BackendSetImpl<Mongo>> mongos;

  kj::Array<pid_t> frontendPids = 0;
  kj::TaskSet tasks;

  kj::Promise<void> startExecLoop(MongoInfo&& mongoInfo, uint replicaNumber,
                                  uint port, uint smtpPort, pid_t& pid);
  kj::Promise<void> execLoop(MongoInfo&& mongoInfo, uint replicaNumber,
                             kj::AutoCloseFd&& http, kj::AutoCloseFd&& smtp, pid_t& pid);

  void taskFailed(kj::Exception&& exception) override;
};

class MongoImpl: public Mongo::Server {
public:
  explicit MongoImpl(
      kj::Timer& timer, sandstorm::SubprocessSet& subprocessSet, SimpleAddress bindAddress,
      kj::PromiseFulfillerPair<void> passwordPaf = kj::newPromiseAndFulfiller<void>());

protected:
  kj::Promise<void> getConnectionInfo(GetConnectionInfoContext context) override;

private:
  kj::Timer& timer;
  sandstorm::SubprocessSet& subprocessSet;
  SimpleAddress bindAddress;
  kj::Maybe<kj::String> password;
  kj::ForkedPromise<void> passwordPromise;
  kj::Promise<void> execTask;

  kj::Promise<void> startExecLoop(kj::Own<kj::PromiseFulfiller<void>> passwordFulfiller);
  kj::Promise<void> execLoop(kj::PromiseFulfiller<void>& passwordFulfiller);
  kj::Promise<kj::String> initializeMongo();
  kj::Promise<void> mongoCommand(kj::String command, kj::StringPtr dbName = "meteor");
};

} // namespace blackrock

#endif // BLACKROCK_FRONTEND_H_

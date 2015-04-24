// Sandstorm Blackrock
// Copyright (c) 2015 Sandstorm Development Group, Inc.
// All Rights Reserved

#include "frontend.h"
#include <grp.h>
#include <signal.h>
#include <sandstorm/version.h>
#include <sys/mount.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sodium/randombytes.h>
#include <unistd.h>
#include <limits.h>

namespace blackrock {

#define BUNDLE_PATH "/blackrock/bundle"

class FrontendImpl::BackendImpl: public sandstorm::Backend::Server {
public:
  explicit BackendImpl(FrontendImpl& frontend): frontend(frontend) {}

protected:
  kj::Promise<void> startGrain(StartGrainContext context) override {
    auto params = context.getParams();

    StorageRootSet::Client storage = frontend.storageRoots->chooseOne();
    StorageFactory::Client storageFactory = storage.getFactoryRequest().send().getFactory();

    // Load the package volume.
    auto packageId = params.getPackageId();
    auto packageVolume = ({
      auto req = storage.getRequest<Volume>();
      req.setName(kj::str("package-", packageId));
      req.send().getObject().castAs<OwnedVolume>();
    });

    // TODO(security): make `packageVolume` read-only somehow

    // Get the owner user data.
    auto owner = ({
      auto userObjectName = kj::str("user-", params.getOwnerId());

      auto req = storage.getOrCreateAssignableRequest<AccountStorage>();
      req.setName(userObjectName);
      req.initDefaultValue();
      req.send().getObject();
    });

    auto ownerGet = owner.getRequest().send();

    if (params.getIsNew()) {
      Worker::Client worker = frontend.workers->chooseOne();

      auto promise = ({
        auto req = worker.newGrainRequest();
        auto packageInfo = req.initPackage();
        packageInfo.setId(packageId.asBytes());  // TODO(perf): parse ID hex to bytes?
        packageInfo.setVolume(kj::mv(packageVolume));
        req.setCommand(params.getCommand());
        req.setStorage(kj::mv(storageFactory));
        req.send();
      });

      context.getResults(capnp::MessageSize { 4, 1 }).setSupervisor(promise.getGrain());
      auto grainState = promise.getGrainState();
      auto grainId = params.getGrainId();

      // Update owner.
      return ownerGet.then([KJ_MVCAP(grainState),grainId](auto&& getResults) mutable {
        auto userInfo = getResults.getValue();

        // Ugh, extending a list in Cap'n Proto is kind of a pain...
        // We copy the user info to a temporary, and then we use Orphan::truncate() to extend the
        // list by one, and then we fill in the new slot.
        capnp::MallocMessageBuilder temp;
        temp.setRoot(userInfo);
        auto userInfoCopy = temp.getRoot<AccountStorage>();
        auto grainsOrphan = userInfoCopy.disownGrains();
        grainsOrphan.truncate(grainsOrphan.get().size() + 1);
        auto grains = grainsOrphan.get();
        auto newGrainInfo = grains[grains.size() - 1];
        newGrainInfo.setId(grainId);
        newGrainInfo.setState(kj::mv(grainState));
        userInfoCopy.adoptGrains(kj::mv(grainsOrphan));

        // Now we can copy the whole thing back into a set request. (We didn't build it there in
        // the first place because extending a list usually leaves an allocation hole, requiring
        // a copy to GC.)
        auto req = getResults.getSetter().setRequest();
        req.setValue(userInfoCopy);
        return req.send().then([](auto) {});
      });
    } else {
      return ownerGet.then([params](auto&& getResults) {
        auto grainId = params.getGrainId();
        for (auto grainInfo: getResults.getValue().getGrains()) {
          if (grainInfo.getId() == grainId) {
            // This is the grain we're looking for.
            return grainInfo.getState();
          }
        }
        KJ_FAIL_REQUIRE("no such grain", grainId);
      }).then([this,context,packageId,params,KJ_MVCAP(storageFactory),KJ_MVCAP(packageVolume)]
              (auto grainState) mutable {
        auto volume = grainState.getRequest().send().getValue().getVolume();
        Worker::Client worker = frontend.workers->chooseOne();

        auto req = worker.restoreGrainRequest();
        auto packageInfo = req.initPackage();
        packageInfo.setId(packageId.asBytes());  // TODO(perf): parse ID hex to bytes?
        packageInfo.setVolume(kj::mv(packageVolume));
        req.setCommand(params.getCommand());
        req.setStorage(kj::mv(storageFactory));
        req.setGrainState(kj::mv(grainState));
        // TODO(now): Disconnect the volume!
        req.setVolume(kj::mv(volume));

        context.getResults().setSupervisor(req.send().getGrain());
      });
    }
  }

  kj::Promise<void> getGrain(GetGrainContext context) override {
    // TODO(now): implement -- probably requires restorable sturdyrefs. For now, thorwing
    //   disconnected will cause the caller to try startGrain().
    return KJ_EXCEPTION(DISCONNECTED, "getGrain() not implemented");
  }

  kj::Promise<void> deleteGrain(DeleteGrainContext context) override {
    auto params = context.getParams();
    StorageRootSet::Client storage = frontend.storageRoots->chooseOne();

    auto owner = ({
      auto userObjectName = kj::str("user-", params.getOwnerId());

      auto req = storage.getOrCreateAssignableRequest<AccountStorage>();
      req.setName(userObjectName);
      req.initDefaultValue();
      req.send().getObject();
    });

    return owner.getRequest().send()
        .then([params](auto&& getResults) -> kj::Promise<void> {
      auto grainId = params.getGrainId();
      auto userInfo = getResults.getValue();

      capnp::MallocMessageBuilder temp;
      temp.setRoot(userInfo);
      auto userInfoCopy = temp.getRoot<AccountStorage>();
      auto grainsOrphan = userInfoCopy.disownGrains();

      auto listBuilder = grainsOrphan.get();
      bool found = false;
      for (auto i: kj::indices(listBuilder)) {
        if (listBuilder[i].getId().asReader() == grainId) {
          if (i == listBuilder.size() - 1) {
            // Copy the last element in the list over this one.
            listBuilder.setWithCaveats(i, listBuilder[listBuilder.size() - 1]);
          }
          found = true;
          break;
        }
      }
      if (found) {
        grainsOrphan.truncate(listBuilder.size() - 1);
        userInfoCopy.adoptGrains(kj::mv(grainsOrphan));

        auto req = getResults.getSetter().setRequest();
        req.setValue(userInfoCopy);
        return req.send().then([](auto) {});
      } else {
        return kj::READY_NOW;
      }
    });
  }

  kj::Promise<void> installPackage(InstallPackageContext context) override {
    Worker::Client worker = frontend.workers->chooseOne();
    StorageRootSet::Client storage = frontend.storageRoots->chooseOne();
    StorageFactory::Client storageFactory = storage.getFactoryRequest().send().getFactory();

    auto stream = ({
      auto req = worker.unpackPackageRequest();
      req.setStorage(kj::mv(storageFactory));
      req.send().getStream();
    });

    context.getResults().setStream(
        kj::heap<PackageUploadStreamImpl>(kj::mv(storage), kj::mv(stream)));
    return kj::READY_NOW;
  }

  kj::Promise<void> getPackage(GetPackageContext context) override {
    // TODO(now): We need to actually mount up the volume and read the manifest. Need a new worker
    //   method for this.
    KJ_UNIMPLEMENTED("getPackage()");
  }

  kj::Promise<void> deletePackage(DeletePackageContext context) override {
    StorageRootSet::Client storage = frontend.storageRoots->chooseOne();
    auto req = storage.removeRequest();
    req.setName(kj::str("package-", context.getParams().getPackageId()));
    context.releaseParams();
    return req.send().then([](auto) {});
  }

private:
  FrontendImpl& frontend;

  class PackageUploadStreamImpl: public sandstorm::Backend::PackageUploadStream::Server {
  public:
    PackageUploadStreamImpl(StorageRootSet::Client storage,
                            Worker::PackageUploadStream::Client inner)
        : storage(kj::mv(storage)), inner(kj::mv(inner)) {}

  protected:
    kj::Promise<void> write(WriteContext context) override {
      auto params = context.getParams();
      auto req = inner.writeRequest(params.totalSize());
      req.setData(params.getData());
      return context.tailCall(kj::mv(req));
    }

    kj::Promise<void> done(DoneContext context) override {
      auto params = context.getParams();
      auto req = inner.doneRequest(params.totalSize());
      return context.tailCall(kj::mv(req));
    }

    kj::Promise<void> expectSize(ExpectSizeContext context) override {
      auto params = context.getParams();
      auto req = inner.expectSizeRequest(params.totalSize());
      req.setSize(params.getSize());
      return context.tailCall(kj::mv(req));
    }

    kj::Promise<void> saveAs(SaveAsContext context) override {
      auto req = inner.getResultRequest(capnp::MessageSize { 8, 0 });
      return req.send().then([this,context](auto&& results) mutable {
        auto packageId = context.getParams().getPackageId();

        auto promise = ({
          auto req = storage.setRequest<Volume>();
          req.setName(kj::str("package-", packageId));
          req.setObject(results.getVolume());
          req.send();
        });

        context.releaseParams();
        auto outerResults = context.getResults();
        outerResults.setAppId(results.getAppId());
        outerResults.setManifest(results.getManifest());

        return promise.then([](auto&&) {});
      });
    }

  private:
    StorageRootSet::Client storage;
    Worker::PackageUploadStream::Client inner;
  };
};

// =======================================================================================

struct FrontendImpl::MongoInfo {
  SimpleAddress address;
  kj::String username;
  kj::String password;

  explicit MongoInfo(Mongo::GetConnectionInfoResults::Reader reader)
      : address(reader.getAddress()),
        username(kj::str(reader.getUsername())),
        password(kj::str(reader.getPassword())) {}

  kj::String toString() {
    return kj::str(username, ':', password, '@', address);
  }
};

FrontendImpl::FrontendImpl(kj::Network& network, kj::Timer& timer,
                           sandstorm::SubprocessSet& subprocessSet,
                           FrontendConfig::Reader config)
    : timer(timer),
      subprocessSet(subprocessSet),
      capnpServer(kj::heap<BackendImpl>(*this)),
      storageRoots(kj::refcounted<BackendSetImpl<StorageRootSet>>()),
      storageFactories(kj::refcounted<BackendSetImpl<StorageFactory>>()),
      workers(kj::refcounted<BackendSetImpl<Worker>>()),
      mongos(kj::refcounted<BackendSetImpl<Mongo>>()),
      tasks(*this) {
  setConfig(config);

  kj::StringPtr socketPath = sandstorm::Backend::SOCKET_PATH;
  KJ_ASSERT(socketPath.startsWith("/var/"));
  kj::String outsideSandboxSocketPath =
      kj::str("/var/blackrock/bundle", socketPath.slice(strlen("/var")));
  sandstorm::recursivelyCreateParent(outsideSandboxSocketPath);
  unlink(outsideSandboxSocketPath.cStr());

  auto mongoInfoPromise = mongos->chooseOne().getConnectionInfoRequest().send();

  auto promise = network.parseAddress(kj::str("unix:", outsideSandboxSocketPath));
  tasks.add(promise.then([this,KJ_MVCAP(outsideSandboxSocketPath),KJ_MVCAP(mongoInfoPromise)]
                         (kj::Own<kj::NetworkAddress>&& addr) mutable {
    return mongoInfoPromise
        .then([this,KJ_MVCAP(addr),KJ_MVCAP(outsideSandboxSocketPath)](auto&& mongoInfo) mutable {
      auto listener = addr->listen();
      KJ_SYSCALL(chown(outsideSandboxSocketPath.cStr(), 0, 1000));
      KJ_SYSCALL(chmod(outsideSandboxSocketPath.cStr(), 0770));

      // Now that we're listening on the socket, and we have Mongo's address, it's safe to start
      // the front-end.
      tasks.add(execLoop(MongoInfo(mongoInfo)));

      auto promise = capnpServer.listen(*listener);
      return promise.attach(kj::mv(listener));
    });
  }));
}

void FrontendImpl::taskFailed(kj::Exception&& exception) {
  KJ_LOG(ERROR, exception);
}

void FrontendImpl::setConfig(FrontendConfig::Reader config) {
  configMessage = kj::heap<capnp::MallocMessageBuilder>();
  configMessage->setRoot(config);
  this->config = configMessage->getRoot<FrontendConfig>();
  if (frontendPid != 0) {
    // Restart frontend.
    KJ_LOG(INFO, "restarting front-end due to config change");
    KJ_SYSCALL(kill(frontendPid, SIGTERM));
  }
}

BackendSet<StorageRootSet>::Client FrontendImpl::getStorageRootBackendSet() {
  return kj::addRef(*storageRoots);
}
BackendSet<StorageFactory>::Client FrontendImpl::getStorageFactoryBackendSet() {
  return kj::addRef(*storageFactories);
}
BackendSet<Worker>::Client FrontendImpl::getWorkerBackendSet() {
  return kj::addRef(*workers);
}
BackendSet<Mongo>::Client FrontendImpl::getMongoBackendSet() {
  return kj::addRef(*mongos);
}

static void createSandstormDirectories() {
  kj::StringPtr paths[] = {
    "/var/blackrock",
    "/var/blackrock/bundle",
    "/var/blackrock/bundle/sandstorm",
    "/var/blackrock/bundle/sandstorm/socket",
    "/var/blackrock/bundle/mongo",
    "/var/blackrock/bundle/log",
    "/var/blackrock/bundle/pid",
    "/tmp/blackrock-bundle"
  };

  if (access("/tmp/blackrock-bundle", F_OK) >= 0) {
    sandstorm::recursivelyDelete("/tmp/blackrock-bundle");
  }
  for (auto path: paths) {
    mkdir(path.cStr(), (path.startsWith("/tmp/") ? S_ISVTX | 0770 : 0750));
    KJ_SYSCALL(chown(path.cStr(), 1000, 1000));
  }
}

static void enterSandstormBundle() {
  // Set up a small sandbox located inside the Sandstorm (i.e. non-Blackrock) bundle, for running
  // things like the front-end and Mongo.
  //
  // TODO(cleanup): Extend Subprocess to support a lot of these things?

  // Enter mount namespace so that we can bind stuff in.
  KJ_SYSCALL(unshare(CLONE_NEWNS));

  KJ_SYSCALL(chdir(BUNDLE_PATH));

  // To really unshare the mount namespace, we also have to make sure all mounts are private.
  // The parameters here were derived by strace'ing `mount --make-rprivate /`.  AFAICT the flags
  // are undocumented.  :(
  KJ_SYSCALL(mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr));

  // Make sure that the current directory is a mount point so that we can use pivot_root.
  KJ_SYSCALL(mount(".", ".", nullptr, MS_BIND | MS_REC, nullptr));

  // Now change directory into the new mount point.
  char cwdBuf[PATH_MAX + 1];
  if (getcwd(cwdBuf, sizeof(cwdBuf)) == nullptr) {
    KJ_FAIL_SYSCALL("getcwd", errno);
  }
  KJ_SYSCALL(chdir(cwdBuf));

  // Bind /proc for the global pid namespace in the chroot.
  KJ_SYSCALL(mount("/proc", "proc", nullptr, MS_BIND | MS_REC, nullptr));

  // Bind /var and /tmp.
  KJ_SYSCALL(mount("/tmp/blackrock-bundle", "tmp", nullptr, MS_BIND, nullptr));
  KJ_SYSCALL(mount("/var/blackrock/bundle", "var", nullptr, MS_BIND, nullptr));

  // Bind desired devices from /dev into our chroot environment.
  KJ_SYSCALL(mount("/dev/null", "dev/null", nullptr, MS_BIND, nullptr));
  KJ_SYSCALL(mount("/dev/zero", "dev/zero", nullptr, MS_BIND, nullptr));
  KJ_SYSCALL(mount("/dev/random", "dev/random", nullptr, MS_BIND, nullptr));
  KJ_SYSCALL(mount("/dev/urandom", "dev/urandom", nullptr, MS_BIND, nullptr));

  // Mount a tmpfs at /etc and copy over necessary config files from the host.
  KJ_SYSCALL(mount("tmpfs", "etc", "tmpfs", MS_NOSUID | MS_NOEXEC,
                   kj::str("size=2m,nr_inodes=128,mode=755,uid=0,gid=0").cStr()));
  {
    auto files = sandstorm::splitLines(sandstorm::readAll("etc.list"));

    // Now copy over each file.
    for (auto& file: files) {
      if (access(file.cStr(), R_OK) == 0) {
        auto in = sandstorm::raiiOpen(file, O_RDONLY);
        auto out = sandstorm::raiiOpen(kj::str(".", file), O_WRONLY | O_CREAT | O_EXCL);
        ssize_t n;
        do {
          KJ_SYSCALL(n = sendfile(out, in, nullptr, 1 << 20));
        } while (n > 0);
      }
    }
  }

  // pivot_root into the frontend dir. (This is just a fancy more-secure chroot.)
  KJ_SYSCALL(syscall(SYS_pivot_root, ".", "tmp"));
  KJ_SYSCALL(chdir("/"));
  KJ_SYSCALL(umount2("tmp", MNT_DETACH));

  // Drop privileges. Since we own the machine we can choose any UID, just don't want it to be 0.
  KJ_SYSCALL(setresgid(1000, 1000, 1000));
  KJ_SYSCALL(setgroups(0, nullptr));
  KJ_SYSCALL(setresuid(1000, 1000, 1000));

  // Clear signal mask. Not strictly a sandboxing measure, just cleanup.
  // TODO(cleanup): We should probably discard any signals in this mask which are currently pending
  //   before we unblock them. We should probably fix this in Sandstorm as well.
  sigset_t sigset;
  KJ_SYSCALL(sigemptyset(&sigset));
  KJ_SYSCALL(sigprocmask(SIG_SETMASK, &sigset, nullptr));
}

kj::Promise<void> FrontendImpl::execLoop(MongoInfo&& mongoInfo) {
  // If node fails, we will wait until at least 10 seconds from now before restarting it.
#warning "bring back to 10s"
  auto rateLimit = timer.afterDelay(10000 * kj::SECONDS);

  auto promise = kj::evalNow([&]() {
    createSandstormDirectories();

    sandstorm::Subprocess subprocess([&]() -> int {
      enterSandstormBundle();

      // Set up environment.
      KJ_SYSCALL(setenv("ROOT_URL", config.getBaseUrl().cStr(), true));
      KJ_SYSCALL(setenv("PORT", "6080", true));
      KJ_SYSCALL(setenv("MONGO_URL",
          kj::str("mongodb://", mongoInfo, "/meteor?authSource=admin").cStr(), true));
      KJ_SYSCALL(setenv("MONGO_OPLOG_URL",
          kj::str("mongodb://", mongoInfo, "/local?authSource=admin").cStr(), true));
      KJ_SYSCALL(setenv("BIND_IP", "0.0.0.0", true));
      if (config.hasMailUrl()) {
        KJ_SYSCALL(setenv("MAIL_URL", config.getMailUrl().cStr(), true));
      }
      if (config.hasDdpUrl()) {
        KJ_SYSCALL(setenv("DDP_DEFAULT_CONNECTION_URL", config.getDdpUrl().cStr(), true));
      }
      kj::String buildstamp;
      if (SANDSTORM_BUILD == 0) {
        buildstamp = kj::str("\"[", sandstorm::trim(sandstorm::readAll("buildstamp")), "]\"");
      } else {
        buildstamp = kj::str(SANDSTORM_BUILD);
      }
      KJ_SYSCALL(setenv("METEOR_SETTINGS", kj::str(
          "{\"public\":{\"build\":", buildstamp,
          ", \"kernelTooOld\": false"
          ", \"allowDemoAccounts\":", config.getAllowDemoAccounts() ? "true" : "false",
          ", \"allowDevAccounts\": false"
          ", \"isTesting\":", config.getIsTesting() ? "true" : "false",
          ", \"wildcardHost\":\"", config.getWildcardHost(), "\"",
          "}}").cStr(), true));

      // Execute!
      KJ_SYSCALL(execl("bin/node", "bin/node", "main.js", (char*)nullptr));
      KJ_UNREACHABLE;
    });

    frontendPid = subprocess.getPid();

    return subprocessSet.waitForSuccess(kj::mv(subprocess));
  });
  return promise.then([]() {
    KJ_FAIL_ASSERT("frontend exited 'successfully' (shouldn't happen); restarting");
  }).catch_([KJ_MVCAP(rateLimit)](kj::Exception&& exception) mutable {
    KJ_LOG(ERROR, "frontend died; restarting", exception);
    return kj::mv(rateLimit);
  }).then([this,KJ_MVCAP(mongoInfo)]() mutable {
    return execLoop(kj::mv(mongoInfo));
  });
}

// =======================================================================================
// Mongo harness.
//
// This is in frontend.c++ mostly because it shares a bunch of code related to using the
// Sandstorm bundle, and because the Frontend is the only thing that uses Mongo.

MongoImpl::MongoImpl(
    kj::Timer& timer, sandstorm::SubprocessSet& subprocessSet, SimpleAddress bindAddress,
    kj::PromiseFulfillerPair<void> passwordPaf)
    : timer(timer), subprocessSet(subprocessSet), bindAddress(bindAddress),
      passwordPromise(passwordPaf.promise.fork()),
      execTask(startExecLoop(kj::mv(passwordPaf.fulfiller))) {}

kj::Promise<void> MongoImpl::getConnectionInfo(GetConnectionInfoContext context) {
  return passwordPromise.addBranch().then([this,context]() mutable {
    auto results = context.getResults();
    bindAddress.copyTo(results.initAddress());
    results.setUsername("sandstorm");
    results.setPassword(KJ_ASSERT_NONNULL(password));
  });
}

kj::Promise<void> MongoImpl::startExecLoop(kj::Own<kj::PromiseFulfiller<void>> passwordFulfiller) {
  auto promise = execLoop(*passwordFulfiller);
  return promise.attach(kj::mv(passwordFulfiller));
}

kj::Promise<void> MongoImpl::execLoop(kj::PromiseFulfiller<void>& passwordFulfiller) {
  // If mongo fails, we will wait until at least 10 seconds from now before restarting it.
#warning "bring back to 10s"
  auto rateLimit = timer.afterDelay(10000 * kj::SECONDS);

  return kj::evalNow([&]() {
    createSandstormDirectories();

    sandstorm::Subprocess subprocess([&]() -> int {
      enterSandstormBundle();

      KJ_SYSCALL(execl("bin/mongod", "bin/mongod", "--fork",
          "--bind_ip", kj::str("127.0.0.1,", bindAddress.toStringWithoutPort()).cStr(),
          "--port", kj::str(bindAddress.getPort()).cStr(),
          "--dbpath", "/var/mongo", "--logpath", "/var/log/mongo.log",
          "--pidfilepath", "/var/pid/mongo.pid",
          "--auth", "--nohttpinterface", "--replSet", "ssrs", "--oplogSize", "128",
          (char*)nullptr));
      KJ_UNREACHABLE;
    });

    // Wait for mongod to return, meaning the database is up.  Then get its real pid via the
    // pidfile.
    return subprocessSet.waitForSuccess(kj::mv(subprocess));
  }).then([this,&passwordFulfiller]() {
    pid_t pid = KJ_ASSERT_NONNULL(sandstorm::parseUInt(sandstorm::trim(
        sandstorm::readAll("/var/blackrock/bundle/pid/mongo.pid")), 10));

    // Check if mongo pid still exists. If not, it's possible that we already received its
    // termination signal and so it's too late to adopt it formally.
    KJ_SYSCALL(kill(pid, 0), "mongod dead on arrival");

    // OK, the pid is live, so let's adopt it. Note that the reason this works is because we are
    // pid 1 of the pid namespace, making us "init" for everything inside, therefore when the mongo
    // starter process exits, the daemon process reparents to us!
    KJ_ASSERT(getpid() == 1);
    sandstorm::Subprocess mongoProc(pid);

    return initializeMongo()
        .then([this,KJ_MVCAP(mongoProc),&passwordFulfiller](kj::String&& pw) mutable {
      password = kj::mv(pw);
      passwordFulfiller.fulfill();
      return subprocessSet.waitForSuccess(kj::mv(mongoProc));
    });
  }).then([this]() {
    KJ_FAIL_ASSERT("mongo exited successfully?");
  }).catch_([KJ_MVCAP(rateLimit)](kj::Exception&& exception) mutable {
    KJ_LOG(ERROR, "mongo died; restarting", exception);
    return kj::mv(rateLimit);
  }).then([this,&passwordFulfiller]() {
    return execLoop(passwordFulfiller);
  });
}

kj::Promise<kj::String> MongoImpl::initializeMongo() {
  bool isNew = access("/var/blackrock/bundle/mongo/passwd", F_OK) < 0;

  return kj::evalNow([&]() {
    if (isNew) {
      // We need to initialize the repl set to get oplog tailing. Our set isn't actually much of a
      // set since it only contains one instance, but you need that for oplog.
      return mongoCommand(kj::str(
          "rs.initiate({_id: 'ssrs', members: [{_id: 0, host: '", bindAddress, "'}]})"));
    } else {
      // It's possible that the bind address has changed, so reconfig the repl set.
      return mongoCommand(kj::str(
          "rs.reconfig({_id: 'ssrs', members: [{_id: 0, host: '", bindAddress, "'}]})"));
    }
  }).then([this]() {
    // We have to wait a few seconds for Mongo to elect itself master of the repl set. Mongo does
    // some sort of heartbeat every second and it takes three of these for Mongo to elect itself,
    // meaning the whole process always takes 2-3 seconds. We'll sleep for 4.
    // TODO(cleanup): This is ugly.
    return timer.afterDelay(4 * kj::SECONDS);
  }).then([this,isNew]() -> kj::Promise<kj::String> {
    if (isNew) {
      // Get 30 random chars for password.
      char passwd[30];
      randombytes_buf(passwd, sizeof(passwd));

      const char* digits =
          "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_";

      for (auto& c: passwd) {
        c = digits[byte(c) % 64];
      }

      auto passwdStr = kj::heapString(passwd, sizeof(passwd));

      // Create the mongo user.
      auto command = kj::str(
        "db.addUser({user: \"sandstorm\", pwd: \"", passwdStr, "\", "
        "roles: [\"readWriteAnyDatabase\",\"userAdminAnyDatabase\",\"dbAdminAnyDatabase\","
                "\"clusterAdmin\"]})");
      return mongoCommand(kj::mv(command), "admin").then([KJ_MVCAP(passwdStr)]() mutable {
        // Store the password.
        auto outFd = sandstorm::raiiOpen(
            "/var/blackrock/bundle/mongo/passwd", O_WRONLY | O_CREAT | O_EXCL, 0600);
        KJ_SYSCALL(fchown(outFd, 1000, 1000));
        kj::FdOutputStream((int)outFd).write(passwdStr.begin(), passwdStr.size());
        return kj::mv(passwdStr);
      });
    } else {
      // Read existing password.
      return sandstorm::trim(sandstorm::readAll(
          sandstorm::raiiOpen("/var/blackrock/bundle/mongo/passwd", O_RDONLY)));
    }
  });
}

kj::Promise<void> MongoImpl::mongoCommand(kj::String command, kj::StringPtr dbName) {
  sandstorm::Subprocess subprocess([&]() -> int {
    enterSandstormBundle();

    auto db = kj::str("localhost:", kj::str(bindAddress.getPort()).cStr(), '/', dbName);

    kj::Vector<const char*> args;
    args.add("/bin/mongo");

    // If /var/blackrock/bundle/mongo/passwd exists, we interpret it as containing the password
    // for a Mongo user "sandstorm", and assume we are expected to log in as this user. (If it
    // doesn't exist yet, then probably we haven't actually initialized the database yet.)
    kj::String password;
    if (access("/var/mongo/passwd", F_OK) == 0) {
      password = sandstorm::trim(sandstorm::readAll(
          sandstorm::raiiOpen("/var/mongo/passwd", O_RDONLY)));

      args.add("-u");
      args.add("sandstorm");
      args.add("-p");
      args.add(password.cStr());
      args.add("--authenticationDatabase");
      args.add("admin");
    }

    args.add("--quiet");
    args.add("--eval");
    args.add(command.cStr());

    args.add(db.cStr());
    args.add(nullptr);

    // OK, run the Mongo client!
    KJ_SYSCALL(execv(args[0], const_cast<char**>(args.begin())));

    KJ_UNREACHABLE;
  });

  return subprocessSet.waitForSuccess(kj::mv(subprocess))
      .catch_([KJ_MVCAP(command)](kj::Exception&& exception) -> kj::Promise<void> {
    KJ_LOG(FATAL, "Mongo client command failed! State is inconsistent! Hanging forever!",
        command, exception.getDescription());
    return kj::NEVER_DONE;
  });
}

} // namespace blackrock


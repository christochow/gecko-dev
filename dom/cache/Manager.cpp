/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/Manager.h"

#include "mozilla/AutoRestore.h"
#include "mozilla/Mutex.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/Unused.h"
#include "mozilla/dom/cache/Context.h"
#include "mozilla/dom/cache/DBAction.h"
#include "mozilla/dom/cache/DBSchema.h"
#include "mozilla/dom/cache/FileUtils.h"
#include "mozilla/dom/cache/ManagerId.h"
#include "mozilla/dom/cache/CacheTypes.h"
#include "mozilla/dom/cache/SavedTypes.h"
#include "mozilla/dom/cache/StreamList.h"
#include "mozilla/dom/cache/Types.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozStorageHelper.h"
#include "nsIInputStream.h"
#include "nsID.h"
#include "nsIFile.h"
#include "nsIThread.h"
#include "nsThreadUtils.h"
#include "nsTObserverArray.h"
#include "QuotaClientImpl.h"

namespace mozilla {
namespace dom {
namespace cache {

namespace {

/**
 * Note: The aCommitHook argument will be invoked while a lock is held. Callers
 * should be careful not to pass a hook that might lock on something else and
 * trigger a deadlock.
 */
template <typename Callable>
nsresult MaybeUpdatePaddingFile(nsIFile* aBaseDir, mozIStorageConnection* aConn,
                                const int64_t aIncreaseSize,
                                const int64_t aDecreaseSize,
                                Callable aCommitHook) {
  MOZ_ASSERT(!NS_IsMainThread());
  MOZ_DIAGNOSTIC_ASSERT(aBaseDir);
  MOZ_DIAGNOSTIC_ASSERT(aConn);
  MOZ_DIAGNOSTIC_ASSERT(aIncreaseSize >= 0);
  MOZ_DIAGNOSTIC_ASSERT(aDecreaseSize >= 0);

  RefPtr<CacheQuotaClient> cacheQuotaClient = CacheQuotaClient::Get();
  MOZ_DIAGNOSTIC_ASSERT(cacheQuotaClient);

  nsresult rv = cacheQuotaClient->MaybeUpdatePaddingFileInternal(
      aBaseDir, aConn, aIncreaseSize, aDecreaseSize, aCommitHook);
  Unused << NS_WARN_IF(NS_FAILED(rv));

  return rv;
}

// An Action that is executed when a Context is first created.  It ensures that
// the directory and database are setup properly.  This lets other actions
// not worry about these details.
class SetupAction final : public SyncDBAction {
 public:
  SetupAction() : SyncDBAction(DBAction::Create) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    MOZ_DIAGNOSTIC_ASSERT(aDBDir);

    nsresult rv = BodyCreateDir(aDBDir);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    // executes in its own transaction
    rv = db::CreateOrMigrateSchema(*aConn);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    // If the Context marker file exists, then the last session was
    // not cleanly shutdown.  In these cases sqlite will ensure that
    // the database is valid, but we might still orphan data.  Both
    // Cache objects and body files can be referenced by DOM objects
    // after they are "removed" from their parent.  So we need to
    // look and see if any of these late access objects have been
    // orphaned.
    //
    // Note, this must be done after any schema version updates to
    // ensure our DBSchema methods work correctly.
    if (MarkerFileExists(aQuotaInfo)) {
      NS_WARNING("Cache not shutdown cleanly! Cleaning up stale data...");
      mozStorageTransaction trans(aConn, false,
                                  mozIStorageConnection::TRANSACTION_IMMEDIATE);

      // Clean up orphaned Cache objects
      AutoTArray<CacheId, 8> orphanedCacheIdList;
      CACHE_TRY_VAR(orphanedCacheIdList, db::FindOrphanedCacheIds(*aConn));

      int64_t overallDeletedPaddingSize = 0;
      for (uint32_t i = 0; i < orphanedCacheIdList.Length(); ++i) {
        DeletionInfo deletionInfo;
        CACHE_TRY_VAR(deletionInfo,
                      db::DeleteCacheId(*aConn, orphanedCacheIdList[i]));

        rv = BodyDeleteFiles(aQuotaInfo, aDBDir,
                             deletionInfo.mDeletedBodyIdList);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          return rv;
        }

        if (deletionInfo.mDeletedPaddingSize > 0) {
          DecreaseUsageForQuotaInfo(aQuotaInfo,
                                    deletionInfo.mDeletedPaddingSize);
        }

        MOZ_DIAGNOSTIC_ASSERT(INT64_MAX - deletionInfo.mDeletedPaddingSize >=
                              overallDeletedPaddingSize);
        overallDeletedPaddingSize += deletionInfo.mDeletedPaddingSize;
      }

      // Clean up orphaned body objects
      AutoTArray<nsID, 64> knownBodyIdList;
      CACHE_TRY_VAR(knownBodyIdList, db::GetKnownBodyIds(*aConn));

      rv = BodyDeleteOrphanedFiles(aQuotaInfo, aDBDir, knownBodyIdList);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      // Commit() explicitly here, because we want to ensure the padding file
      // has the correct content.
      rv = MaybeUpdatePaddingFile(
          aDBDir, aConn, /* aIncreaceSize */ 0, overallDeletedPaddingSize,
          [&trans]() mutable { return trans.Commit(); });
      // We'll restore padding file below, so just warn here if failure happens.
      Unused << NS_WARN_IF(NS_FAILED(rv));
    }

    if (DirectoryPaddingFileExists(aDBDir, DirPaddingFile::TMP_FILE) ||
        !DirectoryPaddingFileExists(aDBDir, DirPaddingFile::FILE)) {
      rv = RestorePaddingFile(aDBDir, aConn);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }

    return rv;
  }
};

// ----------------------------------------------------------------------------

// Action that is executed when we determine that content has stopped using
// a body file that has been orphaned.
class DeleteOrphanedBodyAction final : public Action {
 public:
  using DeletedBodyIdList = AutoTArray<nsID, 64>;

  explicit DeleteOrphanedBodyAction(DeletedBodyIdList&& aDeletedBodyIdList)
      : mDeletedBodyIdList(std::move(aDeletedBodyIdList)) {}

  explicit DeleteOrphanedBodyAction(const nsID& aBodyId)
      : mDeletedBodyIdList{aBodyId} {}

  void RunOnTarget(SafeRefPtr<Resolver> aResolver, const QuotaInfo& aQuotaInfo,
                   Data*) override {
    MOZ_DIAGNOSTIC_ASSERT(aResolver);
    MOZ_DIAGNOSTIC_ASSERT(aQuotaInfo.mDir);

    // Note that since DeleteOrphanedBodyAction isn't used while the context is
    // being initialized, we don't need to check for cancellation here.

    nsCOMPtr<nsIFile> dbDir;
    nsresult rv = aQuotaInfo.mDir->Clone(getter_AddRefs(dbDir));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      aResolver->Resolve(rv);
      return;
    }

    rv = dbDir->Append(u"cache"_ns);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      aResolver->Resolve(rv);
      return;
    }

    rv = BodyDeleteFiles(aQuotaInfo, dbDir, mDeletedBodyIdList);
    Unused << NS_WARN_IF(NS_FAILED(rv));

    aResolver->Resolve(rv);
  }

 private:
  DeletedBodyIdList mDeletedBodyIdList;
};

bool IsHeadRequest(const CacheRequest& aRequest,
                   const CacheQueryParams& aParams) {
  return !aParams.ignoreMethod() &&
         aRequest.method().LowerCaseEqualsLiteral("head");
}

bool IsHeadRequest(const Maybe<CacheRequest>& aRequest,
                   const CacheQueryParams& aParams) {
  if (aRequest.isSome()) {
    return !aParams.ignoreMethod() &&
           aRequest.ref().method().LowerCaseEqualsLiteral("head");
  }
  return false;
}

}  // namespace

// ----------------------------------------------------------------------------

// Singleton class to track Manager instances and ensure there is only
// one for each unique ManagerId.
class Manager::Factory {
 public:
  friend class StaticAutoPtr<Manager::Factory>;

  static Result<SafeRefPtr<Manager>, nsresult> AcquireCreateIfNonExistent(
      const SafeRefPtr<ManagerId>& aManagerId) {
    mozilla::ipc::AssertIsOnBackgroundThread();

    // Ensure there is a factory instance.  This forces the Acquire() call
    // below to use the same factory.
    nsresult rv = MaybeCreateInstance();
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return Err(rv);
    }

    SafeRefPtr<Manager> ref = Acquire(*aManagerId);
    if (!ref) {
      // TODO: replace this with a thread pool (bug 1119864)
      nsCOMPtr<nsIThread> ioThread;
      rv = NS_NewNamedThread("DOMCacheThread", getter_AddRefs(ioThread));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return Err(rv);
      }

      ref = MakeSafeRefPtr<Manager>(aManagerId.clonePtr(), ioThread,
                                    ConstructorGuard{});

      // There may be an old manager for this origin in the process of
      // cleaning up.  We need to tell the new manager about this so
      // that it won't actually start until the old manager is done.
      SafeRefPtr<Manager> oldManager = Acquire(*aManagerId, Closing);
      ref->Init(oldManager ? SomeRef(*oldManager) : Nothing());

      MOZ_ASSERT(!sFactory->mManagerList.Contains(ref));
      sFactory->mManagerList.AppendElement(ref.unsafeGetRawPtr());
    }

    return ref;
  }

  static void Remove(Manager& aManager) {
    mozilla::ipc::AssertIsOnBackgroundThread();
    MOZ_DIAGNOSTIC_ASSERT(sFactory);

    MOZ_ALWAYS_TRUE(sFactory->mManagerList.RemoveElement(&aManager));

    // clean up the factory singleton if there are no more managers
    MaybeDestroyInstance();
  }

  static void Abort(const nsACString& aOrigin) {
    mozilla::ipc::AssertIsOnBackgroundThread();

    if (!sFactory) {
      return;
    }

    MOZ_DIAGNOSTIC_ASSERT(!sFactory->mManagerList.IsEmpty());

    {
      // Note that we are synchronously calling abort code here.  If any
      // of the shutdown code synchronously decides to delete the Factory
      // we need to delay that delete until the end of this method.
      AutoRestore<bool> restore(sFactory->mInSyncAbortOrShutdown);
      sFactory->mInSyncAbortOrShutdown = true;

      for (auto* manager : sFactory->mManagerList.ForwardRange()) {
        if (aOrigin.IsVoid() || manager->mManagerId->QuotaOrigin() == aOrigin) {
          auto pinnedManager =
              SafeRefPtr{manager, AcquireStrongRefFromRawPtr{}};
          pinnedManager->Abort();
        }
      }
    }

    MaybeDestroyInstance();
  }

  static void ShutdownAll() {
    mozilla::ipc::AssertIsOnBackgroundThread();

    if (!sFactory) {
      return;
    }

    MOZ_DIAGNOSTIC_ASSERT(!sFactory->mManagerList.IsEmpty());

    {
      // Note that we are synchronously calling shutdown code here.  If any
      // of the shutdown code synchronously decides to delete the Factory
      // we need to delay that delete until the end of this method.
      AutoRestore<bool> restore(sFactory->mInSyncAbortOrShutdown);
      sFactory->mInSyncAbortOrShutdown = true;

      for (auto* manager : sFactory->mManagerList.ForwardRange()) {
        auto pinnedManager = SafeRefPtr{manager, AcquireStrongRefFromRawPtr{}};
        pinnedManager->Shutdown();
      }
    }

    MaybeDestroyInstance();
  }

  static bool IsShutdownAllComplete() {
    mozilla::ipc::AssertIsOnBackgroundThread();
    return !sFactory;
  }

 private:
  Factory() : mInSyncAbortOrShutdown(false) {
    MOZ_COUNT_CTOR(cache::Manager::Factory);
  }

  ~Factory() {
    MOZ_COUNT_DTOR(cache::Manager::Factory);
    MOZ_DIAGNOSTIC_ASSERT(mManagerList.IsEmpty());
    MOZ_DIAGNOSTIC_ASSERT(!mInSyncAbortOrShutdown);
  }

  static nsresult MaybeCreateInstance() {
    mozilla::ipc::AssertIsOnBackgroundThread();

    if (!sFactory) {
      // Be clear about what we are locking.  sFactory is bg thread only, so
      // we don't need to lock it here.  Just protect sFactoryShutdown and
      // sBackgroundThread.
      {
        StaticMutexAutoLock lock(sMutex);

        if (sFactoryShutdown) {
          return NS_ERROR_ILLEGAL_DURING_SHUTDOWN;
        }
      }

      // We cannot use ClearOnShutdown() here because we're not on the main
      // thread.  Instead, we delete sFactory in Factory::Remove() after the
      // last manager is removed.  ShutdownObserver ensures this happens
      // before shutdown.
      sFactory = new Factory();
    }

    // Never return sFactory to code outside Factory.  We need to delete it
    // out from under ourselves just before we return from Remove().  This
    // would be (even more) dangerous if other code had a pointer to the
    // factory itself.

    return NS_OK;
  }

  static void MaybeDestroyInstance() {
    mozilla::ipc::AssertIsOnBackgroundThread();
    MOZ_DIAGNOSTIC_ASSERT(sFactory);

    // If the factory is is still in use then we cannot delete yet.  This
    // could be due to managers still existing or because we are in the
    // middle of aborting or shutting down.  We need to be careful not to delete
    // ourself synchronously during shutdown.
    if (!sFactory->mManagerList.IsEmpty() || sFactory->mInSyncAbortOrShutdown) {
      return;
    }

    sFactory = nullptr;
  }

  static SafeRefPtr<Manager> Acquire(const ManagerId& aManagerId,
                                     State aState = Open) {
    mozilla::ipc::AssertIsOnBackgroundThread();

    nsresult rv = MaybeCreateInstance();
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return nullptr;
    }

    // Iterate in reverse to find the most recent, matching Manager.  This
    // is important when looking for a Closing Manager.  If a new Manager
    // chains to an old Manager we want it to be the most recent one.
    const auto range = Reversed(sFactory->mManagerList.NonObservingRange());
    const auto foundIt = std::find_if(
        range.begin(), range.end(), [aState, &aManagerId](const auto* manager) {
          return aState == manager->GetState() &&
                 *manager->mManagerId == aManagerId;
        });
    return foundIt != range.end()
               ? SafeRefPtr{*foundIt, AcquireStrongRefFromRawPtr{}}
               : nullptr;
  }

  // Singleton created on demand and deleted when last Manager is cleared
  // in Remove().
  // PBackground thread only.
  static StaticAutoPtr<Factory> sFactory;

  // protects following static attribute
  static StaticMutex sMutex;

  // Indicate if shutdown has occurred to block re-creation of sFactory.
  // Must hold sMutex to access.
  static bool sFactoryShutdown;

  // Weak references as we don't want to keep Manager objects alive forever.
  // When a Manager is destroyed it calls Factory::Remove() to clear itself.
  // PBackground thread only.
  nsTObserverArray<Manager*> mManagerList;

  // This flag is set when we are looping through the list and calling Abort()
  // or Shutdown() on each Manager.  We need to be careful not to synchronously
  // trigger the deletion of the factory while still executing this loop.
  bool mInSyncAbortOrShutdown;
};

// static
StaticAutoPtr<Manager::Factory> Manager::Factory::sFactory;

// static
StaticMutex Manager::Factory::sMutex;

// static
bool Manager::Factory::sFactoryShutdown = false;

// ----------------------------------------------------------------------------

// Abstract class to help implement the various Actions.  The vast majority
// of Actions are synchronous and need to report back to a Listener on the
// Manager.
class Manager::BaseAction : public SyncDBAction {
 protected:
  BaseAction(SafeRefPtr<Manager> aManager, ListenerId aListenerId)
      : SyncDBAction(DBAction::Existing),
        mManager(std::move(aManager)),
        mListenerId(aListenerId) {}

  virtual void Complete(Listener* aListener, ErrorResult&& aRv) = 0;

  virtual void CompleteOnInitiatingThread(nsresult aRv) override {
    NS_ASSERT_OWNINGTHREAD(Manager::BaseAction);
    Listener* listener = mManager->GetListener(mListenerId);
    if (listener) {
      Complete(listener, ErrorResult(aRv));
    }

    // ensure we release the manager on the initiating thread
    mManager = nullptr;
  }

  SafeRefPtr<Manager> mManager;
  const ListenerId mListenerId;
};

// ----------------------------------------------------------------------------

// Action that is executed when we determine that content has stopped using
// a Cache object that has been orphaned.
class Manager::DeleteOrphanedCacheAction final : public SyncDBAction {
 public:
  DeleteOrphanedCacheAction(SafeRefPtr<Manager> aManager, CacheId aCacheId)
      : SyncDBAction(DBAction::Existing),
        mManager(std::move(aManager)),
        mCacheId(aCacheId) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    mQuotaInfo.emplace(aQuotaInfo);

    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    CACHE_TRY_VAR(mDeletionInfo, db::DeleteCacheId(*aConn, mCacheId));

    nsresult rv = MaybeUpdatePaddingFile(
        aDBDir, aConn, /* aIncreaceSize */ 0, mDeletionInfo.mDeletedPaddingSize,
        [&trans]() mutable { return trans.Commit(); });
    Unused << NS_WARN_IF(NS_FAILED(rv));

    return rv;
  }

  virtual void CompleteOnInitiatingThread(nsresult aRv) override {
    // If the transaction fails, we shouldn't delete the body files and decrease
    // their padding size.
    if (NS_FAILED(aRv)) {
      mDeletionInfo.mDeletedBodyIdList.Clear();
      mDeletionInfo.mDeletedPaddingSize = 0;
    }

    mManager->NoteOrphanedBodyIdList(mDeletionInfo.mDeletedBodyIdList);

    if (mDeletionInfo.mDeletedPaddingSize > 0) {
      DecreaseUsageForQuotaInfo(mQuotaInfo.ref(),
                                mDeletionInfo.mDeletedPaddingSize);
    }

    // ensure we release the manager on the initiating thread
    mManager = nullptr;
  }

 private:
  SafeRefPtr<Manager> mManager;
  const CacheId mCacheId;
  DeletionInfo mDeletionInfo;
  Maybe<QuotaInfo> mQuotaInfo;
};

// ----------------------------------------------------------------------------

class Manager::CacheMatchAction final : public Manager::BaseAction {
 public:
  CacheMatchAction(SafeRefPtr<Manager> aManager, ListenerId aListenerId,
                   CacheId aCacheId, const CacheMatchArgs& aArgs,
                   SafeRefPtr<StreamList> aStreamList)
      : BaseAction(std::move(aManager), aListenerId),
        mCacheId(aCacheId),
        mArgs(aArgs),
        mStreamList(std::move(aStreamList)),
        mFoundResponse(false) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    Maybe<SavedResponse> maybeResponse;
    CACHE_TRY_VAR(
        maybeResponse,
        db::CacheMatch(*aConn, mCacheId, mArgs.request(), mArgs.params()));

    mFoundResponse = maybeResponse.isSome();
    if (mFoundResponse) {
      mResponse = std::move(maybeResponse.ref());
    }

    if (!mFoundResponse || !mResponse.mHasBodyId ||
        IsHeadRequest(mArgs.request(), mArgs.params())) {
      mResponse.mHasBodyId = false;
      return NS_OK;
    }

    nsCOMPtr<nsIInputStream> stream;
    if (mArgs.openMode() == OpenMode::Eager) {
      nsresult rv = BodyOpen(aQuotaInfo, aDBDir, mResponse.mBodyId,
                             getter_AddRefs(stream));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
      if (NS_WARN_IF(!stream)) {
        return NS_ERROR_FILE_NOT_FOUND;
      }
    }

    mStreamList->Add(mResponse.mBodyId, std::move(stream));

    return NS_OK;
  }

  virtual void Complete(Listener* aListener, ErrorResult&& aRv) override {
    if (!mFoundResponse) {
      aListener->OnOpComplete(std::move(aRv), CacheMatchResult(Nothing()));
    } else {
      mStreamList->Activate(mCacheId);
      aListener->OnOpComplete(std::move(aRv), CacheMatchResult(Nothing()),
                              mResponse, *mStreamList);
    }
    mStreamList = nullptr;
  }

  virtual bool MatchesCacheId(CacheId aCacheId) const override {
    return aCacheId == mCacheId;
  }

 private:
  const CacheId mCacheId;
  const CacheMatchArgs mArgs;
  SafeRefPtr<StreamList> mStreamList;
  bool mFoundResponse;
  SavedResponse mResponse;
};

// ----------------------------------------------------------------------------

class Manager::CacheMatchAllAction final : public Manager::BaseAction {
 public:
  CacheMatchAllAction(SafeRefPtr<Manager> aManager, ListenerId aListenerId,
                      CacheId aCacheId, const CacheMatchAllArgs& aArgs,
                      SafeRefPtr<StreamList> aStreamList)
      : BaseAction(std::move(aManager), aListenerId),
        mCacheId(aCacheId),
        mArgs(aArgs),
        mStreamList(std::move(aStreamList)) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    CACHE_TRY_VAR(mSavedResponses,
                  db::CacheMatchAll(*aConn, mCacheId, mArgs.maybeRequest(),
                                    mArgs.params()));

    for (uint32_t i = 0; i < mSavedResponses.Length(); ++i) {
      if (!mSavedResponses[i].mHasBodyId ||
          IsHeadRequest(mArgs.maybeRequest(), mArgs.params())) {
        mSavedResponses[i].mHasBodyId = false;
        continue;
      }

      nsCOMPtr<nsIInputStream> stream;
      if (mArgs.openMode() == OpenMode::Eager) {
        nsresult rv = BodyOpen(aQuotaInfo, aDBDir, mSavedResponses[i].mBodyId,
                               getter_AddRefs(stream));
        if (NS_WARN_IF(NS_FAILED(rv))) {
          return rv;
        }
        if (NS_WARN_IF(!stream)) {
          return NS_ERROR_FILE_NOT_FOUND;
        }
      }

      mStreamList->Add(mSavedResponses[i].mBodyId, std::move(stream));
    }

    return NS_OK;
  }

  virtual void Complete(Listener* aListener, ErrorResult&& aRv) override {
    mStreamList->Activate(mCacheId);
    aListener->OnOpComplete(std::move(aRv), CacheMatchAllResult(),
                            mSavedResponses, *mStreamList);
    mStreamList = nullptr;
  }

  virtual bool MatchesCacheId(CacheId aCacheId) const override {
    return aCacheId == mCacheId;
  }

 private:
  const CacheId mCacheId;
  const CacheMatchAllArgs mArgs;
  SafeRefPtr<StreamList> mStreamList;
  nsTArray<SavedResponse> mSavedResponses;
};

// ----------------------------------------------------------------------------

// This is the most complex Action.  It puts a request/response pair into the
// Cache.  It does not complete until all of the body data has been saved to
// disk.  This means its an asynchronous Action.
class Manager::CachePutAllAction final : public DBAction {
 public:
  CachePutAllAction(
      SafeRefPtr<Manager> aManager, ListenerId aListenerId, CacheId aCacheId,
      const nsTArray<CacheRequestResponse>& aPutList,
      const nsTArray<nsCOMPtr<nsIInputStream>>& aRequestStreamList,
      const nsTArray<nsCOMPtr<nsIInputStream>>& aResponseStreamList)
      : DBAction(DBAction::Existing),
        mManager(std::move(aManager)),
        mListenerId(aListenerId),
        mCacheId(aCacheId),
        mList(aPutList.Length()),
        mExpectedAsyncCopyCompletions(1),
        mAsyncResult(NS_OK),
        mMutex("cache::Manager::CachePutAllAction"),
        mUpdatedPaddingSize(0),
        mDeletedPaddingSize(0) {
    MOZ_DIAGNOSTIC_ASSERT(!aPutList.IsEmpty());
    MOZ_DIAGNOSTIC_ASSERT(aPutList.Length() == aRequestStreamList.Length());
    MOZ_DIAGNOSTIC_ASSERT(aPutList.Length() == aResponseStreamList.Length());

    for (uint32_t i = 0; i < aPutList.Length(); ++i) {
      Entry* entry = mList.AppendElement();
      entry->mRequest = aPutList[i].request();
      entry->mRequestStream = aRequestStreamList[i];
      entry->mResponse = aPutList[i].response();
      entry->mResponseStream = aResponseStreamList[i];
    }
  }

 private:
  ~CachePutAllAction() = default;

  virtual void RunWithDBOnTarget(SafeRefPtr<Resolver> aResolver,
                                 const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
                                 mozIStorageConnection* aConn) override {
    MOZ_DIAGNOSTIC_ASSERT(aResolver);
    MOZ_DIAGNOSTIC_ASSERT(aDBDir);
    MOZ_DIAGNOSTIC_ASSERT(aConn);
    MOZ_DIAGNOSTIC_ASSERT(!mResolver);
    MOZ_DIAGNOSTIC_ASSERT(!mDBDir);
    MOZ_DIAGNOSTIC_ASSERT(!mConn);

    MOZ_DIAGNOSTIC_ASSERT(!mTarget);
    mTarget = GetCurrentSerialEventTarget();
    MOZ_DIAGNOSTIC_ASSERT(mTarget);

    // We should be pre-initialized to expect one async completion.  This is
    // the "manual" completion we call at the end of this method in all
    // cases.
    MOZ_DIAGNOSTIC_ASSERT(mExpectedAsyncCopyCompletions == 1);

    mResolver = std::move(aResolver);
    mDBDir = aDBDir;
    mConn = aConn;
    mQuotaInfo.emplace(aQuotaInfo);

    // File bodies are streamed to disk via asynchronous copying.  Start
    // this copying now.  Each copy will eventually result in a call
    // to OnAsyncCopyComplete().
    nsresult rv = NS_OK;
    for (uint32_t i = 0; i < mList.Length(); ++i) {
      rv = StartStreamCopy(aQuotaInfo, mList[i], RequestStream,
                           &mExpectedAsyncCopyCompletions);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        break;
      }

      rv = StartStreamCopy(aQuotaInfo, mList[i], ResponseStream,
                           &mExpectedAsyncCopyCompletions);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        break;
      }
    }

    // Always call OnAsyncCopyComplete() manually here.  This covers the
    // case where there is no async copying and also reports any startup
    // errors correctly.  If we hit an error, then OnAsyncCopyComplete()
    // will cancel any async copying.
    OnAsyncCopyComplete(rv);
  }

  // Called once for each asynchronous file copy whether it succeeds or
  // fails.  If a file copy is canceled, it still calls this method with
  // an error code.
  void OnAsyncCopyComplete(nsresult aRv) {
    MOZ_ASSERT(mTarget->IsOnCurrentThread());
    MOZ_DIAGNOSTIC_ASSERT(mConn);
    MOZ_DIAGNOSTIC_ASSERT(mResolver);
    MOZ_DIAGNOSTIC_ASSERT(mExpectedAsyncCopyCompletions > 0);

    // Explicitly check for cancellation here to catch a race condition.
    // Consider:
    //
    // 1) NS_AsyncCopy() executes on IO thread, but has not saved its
    //    copy context yet.
    // 2) CancelAllStreamCopying() occurs on PBackground thread
    // 3) Copy context from (1) is saved on IO thread.
    //
    // Checking for cancellation here catches this condition when we
    // first call OnAsyncCopyComplete() manually from RunWithDBOnTarget().
    //
    // This explicit cancellation check also handles the case where we
    // are canceled just after all stream copying completes.  We should
    // abort the synchronous DB operations in this case if we have not
    // started them yet.
    if (NS_SUCCEEDED(aRv) && IsCanceled()) {
      aRv = NS_ERROR_ABORT;
    }

    // If any of the async copies fail, we need to still wait for them all to
    // complete.  Cancel any other streams still working and remember the
    // error.  All canceled streams will call OnAsyncCopyComplete().
    if (NS_FAILED(aRv) && NS_SUCCEEDED(mAsyncResult)) {
      CancelAllStreamCopying();
      mAsyncResult = aRv;
    }

    // Check to see if async copying is still on-going.  If so, then simply
    // return for now.  We must wait for a later OnAsyncCopyComplete() call.
    mExpectedAsyncCopyCompletions -= 1;
    if (mExpectedAsyncCopyCompletions > 0) {
      return;
    }

    // We have finished with all async copying.  Indicate this by clearing all
    // our copy contexts.
    {
      MutexAutoLock lock(mMutex);
      mCopyContextList.Clear();
    }

    // An error occurred while async copying.  Terminate the Action.
    // DoResolve() will clean up any files we may have written.
    if (NS_FAILED(mAsyncResult)) {
      DoResolve(mAsyncResult);
      return;
    }

    mozStorageTransaction trans(mConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    nsresult rv = NS_OK;
    for (uint32_t i = 0; i < mList.Length(); ++i) {
      Entry& e = mList[i];
      if (e.mRequestStream) {
        rv = BodyFinalizeWrite(mDBDir, e.mRequestBodyId);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          DoResolve(rv);
          return;
        }
      }
      if (e.mResponseStream) {
        // Gerenate padding size for opaque response if needed.
        if (e.mResponse.type() == ResponseType::Opaque) {
          // It'll generate padding if we've not set it yet.
          rv = BodyMaybeUpdatePaddingSize(
              mQuotaInfo.ref(), mDBDir, e.mResponseBodyId,
              e.mResponse.paddingInfo(), &e.mResponse.paddingSize());
          if (NS_WARN_IF(NS_FAILED(rv))) {
            DoResolve(rv);
            return;
          }

          MOZ_DIAGNOSTIC_ASSERT(INT64_MAX - e.mResponse.paddingSize() >=
                                mUpdatedPaddingSize);
          mUpdatedPaddingSize += e.mResponse.paddingSize();
        }

        rv = BodyFinalizeWrite(mDBDir, e.mResponseBodyId);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          DoResolve(rv);
          return;
        }
      }

      // XXXtt:: CACHE_TRY_VAR might need to handle cases like this.
      auto deletionInfoOrErr = db::CachePut(
          *mConn, mCacheId, e.mRequest,
          e.mRequestStream ? &e.mRequestBodyId : nullptr, e.mResponse,
          e.mResponseStream ? &e.mResponseBodyId : nullptr);
      if (NS_WARN_IF(deletionInfoOrErr.isErr())) {
        DoResolve(deletionInfoOrErr.unwrapErr());
        return;
      }

      const int64_t deletedPaddingSize =
          deletionInfoOrErr.inspect().mDeletedPaddingSize;
      mDeletedBodyIdList = deletionInfoOrErr.unwrap().mDeletedBodyIdList;

      MOZ_DIAGNOSTIC_ASSERT(INT64_MAX - mDeletedPaddingSize >=
                            deletedPaddingSize);
      mDeletedPaddingSize += deletedPaddingSize;
    }

    // Update padding file when it's necessary
    rv = MaybeUpdatePaddingFile(mDBDir, mConn, mUpdatedPaddingSize,
                                mDeletedPaddingSize,
                                [&trans]() mutable { return trans.Commit(); });
    Unused << NS_WARN_IF(NS_FAILED(rv));

    DoResolve(rv);
  }

  virtual void CompleteOnInitiatingThread(nsresult aRv) override {
    NS_ASSERT_OWNINGTHREAD(Action);

    for (uint32_t i = 0; i < mList.Length(); ++i) {
      mList[i].mRequestStream = nullptr;
      mList[i].mResponseStream = nullptr;
    }

    // If the transaction fails, we shouldn't delete the body files and decrease
    // their padding size.
    if (NS_FAILED(aRv)) {
      mDeletedBodyIdList.Clear();
      mDeletedPaddingSize = 0;
    }

    mManager->NoteOrphanedBodyIdList(mDeletedBodyIdList);

    if (mDeletedPaddingSize > 0) {
      DecreaseUsageForQuotaInfo(mQuotaInfo.ref(), mDeletedPaddingSize);
    }

    Listener* listener = mManager->GetListener(mListenerId);
    mManager = nullptr;
    if (listener) {
      listener->OnOpComplete(ErrorResult(aRv), CachePutAllResult());
    }
  }

  virtual void CancelOnInitiatingThread() override {
    NS_ASSERT_OWNINGTHREAD(Action);
    Action::CancelOnInitiatingThread();
    CancelAllStreamCopying();
  }

  virtual bool MatchesCacheId(CacheId aCacheId) const override {
    NS_ASSERT_OWNINGTHREAD(Action);
    return aCacheId == mCacheId;
  }

  struct Entry {
    CacheRequest mRequest;
    nsCOMPtr<nsIInputStream> mRequestStream;
    nsID mRequestBodyId;
    nsCOMPtr<nsISupports> mRequestCopyContext;

    CacheResponse mResponse;
    nsCOMPtr<nsIInputStream> mResponseStream;
    nsID mResponseBodyId;
    nsCOMPtr<nsISupports> mResponseCopyContext;
  };

  enum StreamId { RequestStream, ResponseStream };

  nsresult StartStreamCopy(const QuotaInfo& aQuotaInfo, Entry& aEntry,
                           StreamId aStreamId, uint32_t* aCopyCountOut) {
    MOZ_ASSERT(mTarget->IsOnCurrentThread());
    MOZ_DIAGNOSTIC_ASSERT(aCopyCountOut);

    if (IsCanceled()) {
      return NS_ERROR_ABORT;
    }

    nsCOMPtr<nsIInputStream> source;
    nsID* bodyId;

    if (aStreamId == RequestStream) {
      source = aEntry.mRequestStream;
      bodyId = &aEntry.mRequestBodyId;
    } else {
      MOZ_DIAGNOSTIC_ASSERT(aStreamId == ResponseStream);
      source = aEntry.mResponseStream;
      bodyId = &aEntry.mResponseBodyId;
    }

    if (!source) {
      return NS_OK;
    }

    nsCOMPtr<nsISupports> copyContext;

    nsresult rv = BodyStartWriteStream(aQuotaInfo, mDBDir, source, this,
                                       AsyncCopyCompleteFunc, bodyId,
                                       getter_AddRefs(copyContext));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    mBodyIdWrittenList.AppendElement(*bodyId);

    if (copyContext) {
      MutexAutoLock lock(mMutex);
      mCopyContextList.AppendElement(copyContext);
    }

    *aCopyCountOut += 1;

    return rv;
  }

  void CancelAllStreamCopying() {
    // May occur on either owning thread or target thread
    MutexAutoLock lock(mMutex);
    for (uint32_t i = 0; i < mCopyContextList.Length(); ++i) {
      BodyCancelWrite(mDBDir, mCopyContextList[i]);
    }
    mCopyContextList.Clear();
  }

  static void AsyncCopyCompleteFunc(void* aClosure, nsresult aRv) {
    // May be on any thread, including STS event target.
    MOZ_DIAGNOSTIC_ASSERT(aClosure);
    // Weak ref as we are guaranteed to the action is alive until
    // CompleteOnInitiatingThread is called.
    CachePutAllAction* action = static_cast<CachePutAllAction*>(aClosure);
    action->CallOnAsyncCopyCompleteOnTargetThread(aRv);
  }

  void CallOnAsyncCopyCompleteOnTargetThread(nsresult aRv) {
    // May be on any thread, including STS event target.  Non-owning runnable
    // here since we are guaranteed the Action will survive until
    // CompleteOnInitiatingThread is called.
    nsCOMPtr<nsIRunnable> runnable = NewNonOwningRunnableMethod<nsresult>(
        "dom::cache::Manager::CachePutAllAction::OnAsyncCopyComplete", this,
        &CachePutAllAction::OnAsyncCopyComplete, aRv);
    MOZ_ALWAYS_SUCCEEDS(
        mTarget->Dispatch(runnable.forget(), nsIThread::DISPATCH_NORMAL));
  }

  void DoResolve(nsresult aRv) {
    MOZ_ASSERT(mTarget->IsOnCurrentThread());

    // DoResolve() must not be called until all async copying has completed.
#ifdef DEBUG
    {
      MutexAutoLock lock(mMutex);
      MOZ_ASSERT(mCopyContextList.IsEmpty());
    }
#endif

    // Clean up any files we might have written before hitting the error.
    if (NS_FAILED(aRv)) {
      BodyDeleteFiles(mQuotaInfo.ref(), mDBDir, mBodyIdWrittenList);
      if (mUpdatedPaddingSize > 0) {
        DecreaseUsageForQuotaInfo(mQuotaInfo.ref(), mUpdatedPaddingSize);
      }
    }

    // Must be released on the target thread where it was opened.
    mConn = nullptr;

    // Drop our ref to the target thread as we are done with this thread.
    // Also makes our thread assertions catch any incorrect method calls
    // after resolve.
    mTarget = nullptr;

    // Make sure to de-ref the resolver per the Action API contract.
    SafeRefPtr<Action::Resolver> resolver = std::move(mResolver);
    resolver->Resolve(aRv);
  }

  // initiating thread only
  SafeRefPtr<Manager> mManager;
  const ListenerId mListenerId;

  // Set on initiating thread, read on target thread.  State machine guarantees
  // these are not modified while being read by the target thread.
  const CacheId mCacheId;
  nsTArray<Entry> mList;
  uint32_t mExpectedAsyncCopyCompletions;

  // target thread only
  SafeRefPtr<Resolver> mResolver;
  nsCOMPtr<nsIFile> mDBDir;
  nsCOMPtr<mozIStorageConnection> mConn;
  nsCOMPtr<nsISerialEventTarget> mTarget;
  nsresult mAsyncResult;
  nsTArray<nsID> mBodyIdWrittenList;

  // Written to on target thread, accessed on initiating thread after target
  // thread activity is guaranteed complete
  nsTArray<nsID> mDeletedBodyIdList;

  // accessed from any thread while mMutex locked
  Mutex mMutex;
  nsTArray<nsCOMPtr<nsISupports>> mCopyContextList;

  Maybe<QuotaInfo> mQuotaInfo;
  // Track how much pad amount has been added for new entries so that it can be
  // removed if an error occurs.
  int64_t mUpdatedPaddingSize;
  // Track any pad amount associated with overwritten entries.
  int64_t mDeletedPaddingSize;
};

// ----------------------------------------------------------------------------

class Manager::CacheDeleteAction final : public Manager::BaseAction {
 public:
  CacheDeleteAction(SafeRefPtr<Manager> aManager, ListenerId aListenerId,
                    CacheId aCacheId, const CacheDeleteArgs& aArgs)
      : BaseAction(std::move(aManager), aListenerId),
        mCacheId(aCacheId),
        mArgs(aArgs),
        mSuccess(false) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    mQuotaInfo.emplace(aQuotaInfo);

    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    Maybe<DeletionInfo> maybeDeletionInfo;
    CACHE_TRY_VAR(
        maybeDeletionInfo,
        db::CacheDelete(*aConn, mCacheId, mArgs.request(), mArgs.params()));

    mSuccess = maybeDeletionInfo.isSome();
    if (mSuccess) {
      mDeletionInfo = std::move(maybeDeletionInfo.ref());
    }

    nsresult rv = MaybeUpdatePaddingFile(
        aDBDir, aConn, /* aIncreaceSize */ 0, mDeletionInfo.mDeletedPaddingSize,
        [&trans]() mutable { return trans.Commit(); });
    if (NS_WARN_IF(NS_FAILED(rv))) {
      mSuccess = false;
      return rv;
    }

    return rv;
  }

  virtual void Complete(Listener* aListener, ErrorResult&& aRv) override {
    // If the transaction fails, we shouldn't delete the body files and decrease
    // their padding size.
    if (aRv.Failed()) {
      mDeletionInfo.mDeletedBodyIdList.Clear();
      mDeletionInfo.mDeletedPaddingSize = 0;
    }

    mManager->NoteOrphanedBodyIdList(mDeletionInfo.mDeletedBodyIdList);

    if (mDeletionInfo.mDeletedPaddingSize > 0) {
      DecreaseUsageForQuotaInfo(mQuotaInfo.ref(),
                                mDeletionInfo.mDeletedPaddingSize);
    }

    aListener->OnOpComplete(std::move(aRv), CacheDeleteResult(mSuccess));
  }

  virtual bool MatchesCacheId(CacheId aCacheId) const override {
    return aCacheId == mCacheId;
  }

 private:
  const CacheId mCacheId;
  const CacheDeleteArgs mArgs;
  bool mSuccess;
  DeletionInfo mDeletionInfo;
  Maybe<QuotaInfo> mQuotaInfo;
};

// ----------------------------------------------------------------------------

class Manager::CacheKeysAction final : public Manager::BaseAction {
 public:
  CacheKeysAction(SafeRefPtr<Manager> aManager, ListenerId aListenerId,
                  CacheId aCacheId, const CacheKeysArgs& aArgs,
                  SafeRefPtr<StreamList> aStreamList)
      : BaseAction(std::move(aManager), aListenerId),
        mCacheId(aCacheId),
        mArgs(aArgs),
        mStreamList(std::move(aStreamList)) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    CACHE_TRY_VAR(
        mSavedRequests,
        db::CacheKeys(*aConn, mCacheId, mArgs.maybeRequest(), mArgs.params()));

    for (uint32_t i = 0; i < mSavedRequests.Length(); ++i) {
      if (!mSavedRequests[i].mHasBodyId ||
          IsHeadRequest(mArgs.maybeRequest(), mArgs.params())) {
        mSavedRequests[i].mHasBodyId = false;
        continue;
      }

      nsCOMPtr<nsIInputStream> stream;
      if (mArgs.openMode() == OpenMode::Eager) {
        nsresult rv = BodyOpen(aQuotaInfo, aDBDir, mSavedRequests[i].mBodyId,
                               getter_AddRefs(stream));
        if (NS_WARN_IF(NS_FAILED(rv))) {
          return rv;
        }
        if (NS_WARN_IF(!stream)) {
          return NS_ERROR_FILE_NOT_FOUND;
        }
      }

      mStreamList->Add(mSavedRequests[i].mBodyId, std::move(stream));
    }

    return NS_OK;
  }

  virtual void Complete(Listener* aListener, ErrorResult&& aRv) override {
    mStreamList->Activate(mCacheId);
    aListener->OnOpComplete(std::move(aRv), CacheKeysResult(), mSavedRequests,
                            *mStreamList);
    mStreamList = nullptr;
  }

  virtual bool MatchesCacheId(CacheId aCacheId) const override {
    return aCacheId == mCacheId;
  }

 private:
  const CacheId mCacheId;
  const CacheKeysArgs mArgs;
  SafeRefPtr<StreamList> mStreamList;
  nsTArray<SavedRequest> mSavedRequests;
};

// ----------------------------------------------------------------------------

class Manager::StorageMatchAction final : public Manager::BaseAction {
 public:
  StorageMatchAction(SafeRefPtr<Manager> aManager, ListenerId aListenerId,
                     Namespace aNamespace, const StorageMatchArgs& aArgs,
                     SafeRefPtr<StreamList> aStreamList)
      : BaseAction(std::move(aManager), aListenerId),
        mNamespace(aNamespace),
        mArgs(aArgs),
        mStreamList(std::move(aStreamList)),
        mFoundResponse(false) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    auto maybeResponse =
        db::StorageMatch(*aConn, mNamespace, mArgs.request(), mArgs.params());
    if (NS_WARN_IF(maybeResponse.isErr())) {
      return maybeResponse.unwrapErr();
    }

    mFoundResponse = maybeResponse.inspect().isSome();
    if (mFoundResponse) {
      mSavedResponse = maybeResponse.unwrap().ref();
    }

    if (!mFoundResponse || !mSavedResponse.mHasBodyId ||
        IsHeadRequest(mArgs.request(), mArgs.params())) {
      mSavedResponse.mHasBodyId = false;
      return NS_OK;
    }

    nsCOMPtr<nsIInputStream> stream;
    if (mArgs.openMode() == OpenMode::Eager) {
      nsresult rv = BodyOpen(aQuotaInfo, aDBDir, mSavedResponse.mBodyId,
                             getter_AddRefs(stream));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
      if (NS_WARN_IF(!stream)) {
        return NS_ERROR_FILE_NOT_FOUND;
      }
    }

    mStreamList->Add(mSavedResponse.mBodyId, std::move(stream));

    return NS_OK;
  }

  virtual void Complete(Listener* aListener, ErrorResult&& aRv) override {
    if (!mFoundResponse) {
      aListener->OnOpComplete(std::move(aRv), StorageMatchResult(Nothing()));
    } else {
      mStreamList->Activate(mSavedResponse.mCacheId);
      aListener->OnOpComplete(std::move(aRv), StorageMatchResult(Nothing()),
                              mSavedResponse, *mStreamList);
    }
    mStreamList = nullptr;
  }

 private:
  const Namespace mNamespace;
  const StorageMatchArgs mArgs;
  SafeRefPtr<StreamList> mStreamList;
  bool mFoundResponse;
  SavedResponse mSavedResponse;
};

// ----------------------------------------------------------------------------

class Manager::StorageHasAction final : public Manager::BaseAction {
 public:
  StorageHasAction(SafeRefPtr<Manager> aManager, ListenerId aListenerId,
                   Namespace aNamespace, const StorageHasArgs& aArgs)
      : BaseAction(std::move(aManager), aListenerId),
        mNamespace(aNamespace),
        mArgs(aArgs),
        mCacheFound(false) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    Maybe<CacheId> maybeCacheId;
    CACHE_TRY_VAR(maybeCacheId,
                  db::StorageGetCacheId(*aConn, mNamespace, mArgs.key()));

    mCacheFound = maybeCacheId.isSome();

    return NS_OK;
  }

  virtual void Complete(Listener* aListener, ErrorResult&& aRv) override {
    aListener->OnOpComplete(std::move(aRv), StorageHasResult(mCacheFound));
  }

 private:
  const Namespace mNamespace;
  const StorageHasArgs mArgs;
  bool mCacheFound;
};

// ----------------------------------------------------------------------------

class Manager::StorageOpenAction final : public Manager::BaseAction {
 public:
  StorageOpenAction(SafeRefPtr<Manager> aManager, ListenerId aListenerId,
                    Namespace aNamespace, const StorageOpenArgs& aArgs)
      : BaseAction(std::move(aManager), aListenerId),
        mNamespace(aNamespace),
        mArgs(aArgs),
        mCacheId(INVALID_CACHE_ID) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    // Cache does not exist, create it instead
    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    // Look for existing cache
    Maybe<CacheId> maybeCacheId;
    CACHE_TRY_VAR(maybeCacheId,
                  db::StorageGetCacheId(*aConn, mNamespace, mArgs.key()));

    if (maybeCacheId.isSome()) {
      mCacheId = maybeCacheId.ref();
      MOZ_DIAGNOSTIC_ASSERT(mCacheId != INVALID_CACHE_ID);
      return NS_OK;
    }

    CACHE_TRY_VAR(mCacheId, db::CreateCacheId(*aConn));

    nsresult rv =
        db::StoragePutCache(*aConn, mNamespace, mArgs.key(), mCacheId);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = trans.Commit();
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    MOZ_DIAGNOSTIC_ASSERT(mCacheId != INVALID_CACHE_ID);
    return rv;
  }

  virtual void Complete(Listener* aListener, ErrorResult&& aRv) override {
    MOZ_DIAGNOSTIC_ASSERT(aRv.Failed() || mCacheId != INVALID_CACHE_ID);
    aListener->OnOpComplete(std::move(aRv),
                            StorageOpenResult(nullptr, nullptr, mNamespace),
                            mCacheId);
  }

 private:
  const Namespace mNamespace;
  const StorageOpenArgs mArgs;
  CacheId mCacheId;
};

// ----------------------------------------------------------------------------

class Manager::StorageDeleteAction final : public Manager::BaseAction {
 public:
  StorageDeleteAction(SafeRefPtr<Manager> aManager, ListenerId aListenerId,
                      Namespace aNamespace, const StorageDeleteArgs& aArgs)
      : BaseAction(std::move(aManager), aListenerId),
        mNamespace(aNamespace),
        mArgs(aArgs),
        mCacheDeleted(false),
        mCacheId(INVALID_CACHE_ID) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    Maybe<CacheId> maybeCacheId;
    CACHE_TRY_VAR(maybeCacheId,
                  db::StorageGetCacheId(*aConn, mNamespace, mArgs.key()));

    if (maybeCacheId.isNothing()) {
      mCacheDeleted = false;
      return NS_OK;
    }
    mCacheId = maybeCacheId.ref();

    // Don't delete the removing padding size here, we'll delete it on
    // DeleteOrphanedCacheAction.
    nsresult rv = db::StorageForgetCache(*aConn, mNamespace, mArgs.key());
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = trans.Commit();
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    mCacheDeleted = true;
    return rv;
  }

  virtual void Complete(Listener* aListener, ErrorResult&& aRv) override {
    if (mCacheDeleted) {
      // If content is referencing this cache, mark it orphaned to be
      // deleted later.
      if (!mManager->SetCacheIdOrphanedIfRefed(mCacheId)) {
        // no outstanding references, delete immediately
        const auto pinnedContext =
            SafeRefPtr{mManager->mContext, AcquireStrongRefFromRawPtr{}};

        if (pinnedContext->IsCanceled()) {
          pinnedContext->NoteOrphanedData();
        } else {
          pinnedContext->CancelForCacheId(mCacheId);
          pinnedContext->Dispatch(MakeSafeRefPtr<DeleteOrphanedCacheAction>(
              mManager.clonePtr(), mCacheId));
        }
      }
    }

    aListener->OnOpComplete(std::move(aRv), StorageDeleteResult(mCacheDeleted));
  }

 private:
  const Namespace mNamespace;
  const StorageDeleteArgs mArgs;
  bool mCacheDeleted;
  CacheId mCacheId;
};

// ----------------------------------------------------------------------------

class Manager::StorageKeysAction final : public Manager::BaseAction {
 public:
  StorageKeysAction(SafeRefPtr<Manager> aManager, ListenerId aListenerId,
                    Namespace aNamespace)
      : BaseAction(std::move(aManager), aListenerId), mNamespace(aNamespace) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    CACHE_TRY_VAR(mKeys, db::StorageGetKeys(*aConn, mNamespace));

    return NS_OK;
  }

  virtual void Complete(Listener* aListener, ErrorResult&& aRv) override {
    if (aRv.Failed()) {
      mKeys.Clear();
    }
    aListener->OnOpComplete(std::move(aRv), StorageKeysResult(mKeys));
  }

 private:
  const Namespace mNamespace;
  nsTArray<nsString> mKeys;
};

// ----------------------------------------------------------------------------

class Manager::OpenStreamAction final : public Manager::BaseAction {
 public:
  OpenStreamAction(SafeRefPtr<Manager> aManager, ListenerId aListenerId,
                   InputStreamResolver&& aResolver, const nsID& aBodyId)
      : BaseAction(std::move(aManager), aListenerId),
        mResolver(std::move(aResolver)),
        mBodyId(aBodyId) {}

  virtual nsresult RunSyncWithDBOnTarget(
      const QuotaInfo& aQuotaInfo, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override {
    nsresult rv =
        BodyOpen(aQuotaInfo, aDBDir, mBodyId, getter_AddRefs(mBodyStream));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
    if (NS_WARN_IF(!mBodyStream)) {
      return NS_ERROR_FILE_NOT_FOUND;
    }

    return rv;
  }

  virtual void Complete(Listener* aListener, ErrorResult&& aRv) override {
    if (aRv.Failed()) {
      // Ignore the reason for fail and just pass a null input stream to let it
      // fail.
      aRv.SuppressException();
      mResolver(nullptr);
    } else {
      mResolver(std::move(mBodyStream));
    }

    mResolver = nullptr;
  }

 private:
  InputStreamResolver mResolver;
  const nsID mBodyId;
  nsCOMPtr<nsIInputStream> mBodyStream;
};

// ----------------------------------------------------------------------------

// static
Manager::ListenerId Manager::sNextListenerId = 0;

void Manager::Listener::OnOpComplete(ErrorResult&& aRv,
                                     const CacheOpResult& aResult) {
  OnOpComplete(std::move(aRv), aResult, INVALID_CACHE_ID, Nothing());
}

void Manager::Listener::OnOpComplete(ErrorResult&& aRv,
                                     const CacheOpResult& aResult,
                                     CacheId aOpenedCacheId) {
  OnOpComplete(std::move(aRv), aResult, aOpenedCacheId, Nothing());
}

void Manager::Listener::OnOpComplete(ErrorResult&& aRv,
                                     const CacheOpResult& aResult,
                                     const SavedResponse& aSavedResponse,
                                     StreamList& aStreamList) {
  AutoTArray<SavedResponse, 1> responseList;
  responseList.AppendElement(aSavedResponse);
  OnOpComplete(
      std::move(aRv), aResult, INVALID_CACHE_ID,
      Some(StreamInfo{responseList, nsTArray<SavedRequest>(), aStreamList}));
}

void Manager::Listener::OnOpComplete(
    ErrorResult&& aRv, const CacheOpResult& aResult,
    const nsTArray<SavedResponse>& aSavedResponseList,
    StreamList& aStreamList) {
  OnOpComplete(std::move(aRv), aResult, INVALID_CACHE_ID,
               Some(StreamInfo{aSavedResponseList, nsTArray<SavedRequest>(),
                               aStreamList}));
}

void Manager::Listener::OnOpComplete(
    ErrorResult&& aRv, const CacheOpResult& aResult,
    const nsTArray<SavedRequest>& aSavedRequestList, StreamList& aStreamList) {
  OnOpComplete(std::move(aRv), aResult, INVALID_CACHE_ID,
               Some(StreamInfo{nsTArray<SavedResponse>(), aSavedRequestList,
                               aStreamList}));
}

// static
Result<SafeRefPtr<Manager>, nsresult> Manager::AcquireCreateIfNonExistent(
    const SafeRefPtr<ManagerId>& aManagerId) {
  mozilla::ipc::AssertIsOnBackgroundThread();
  return Factory::AcquireCreateIfNonExistent(aManagerId);
}

// static
void Manager::ShutdownAll() {
  mozilla::ipc::AssertIsOnBackgroundThread();

  Factory::ShutdownAll();

  if (!mozilla::SpinEventLoopUntil(
          []() { return Factory::IsShutdownAllComplete(); })) {
    NS_WARNING("Something bad happened!");
  }
}

// static
void Manager::Abort(const nsACString& aOrigin) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  Factory::Abort(aOrigin);
}

void Manager::RemoveListener(Listener* aListener) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  // There may not be a listener here in the case where an actor is killed
  // before it can perform any actual async requests on Manager.
  mListeners.RemoveElement(aListener, ListenerEntryListenerComparator());
  MOZ_ASSERT(
      !mListeners.Contains(aListener, ListenerEntryListenerComparator()));
  MaybeAllowContextToClose();
}

void Manager::RemoveContext(Context& aContext) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_DIAGNOSTIC_ASSERT(mContext);
  MOZ_DIAGNOSTIC_ASSERT(mContext == &aContext);

  // Whether the Context destruction was triggered from the Manager going
  // idle or the underlying storage being invalidated, we should know we
  // are closing before the Context is destroyed.
  MOZ_DIAGNOSTIC_ASSERT(mState == Closing);

  // Before forgetting the Context, check to see if we have any outstanding
  // cache or body objects waiting for deletion.  If so, note that we've
  // orphaned data so it will be cleaned up on the next open.
  for (uint32_t i = 0; i < mCacheIdRefs.Length(); ++i) {
    if (mCacheIdRefs[i].mOrphaned) {
      aContext.NoteOrphanedData();
      break;
    }
  }

  for (uint32_t i = 0; i < mBodyIdRefs.Length(); ++i) {
    if (mBodyIdRefs[i].mOrphaned) {
      aContext.NoteOrphanedData();
      break;
    }
  }

  mContext = nullptr;

  // Once the context is gone, we can immediately remove ourself from the
  // Factory list.  We don't need to block shutdown by staying in the list
  // any more.
  Factory::Remove(*this);
}

void Manager::NoteClosing() {
  NS_ASSERT_OWNINGTHREAD(Manager);
  // This can be called more than once legitimately through different paths.
  mState = Closing;
}

Manager::State Manager::GetState() const {
  NS_ASSERT_OWNINGTHREAD(Manager);
  return mState;
}

void Manager::AddRefCacheId(CacheId aCacheId) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  for (uint32_t i = 0; i < mCacheIdRefs.Length(); ++i) {
    if (mCacheIdRefs[i].mCacheId == aCacheId) {
      mCacheIdRefs[i].mCount += 1;
      return;
    }
  }
  CacheIdRefCounter* entry = mCacheIdRefs.AppendElement();
  entry->mCacheId = aCacheId;
  entry->mCount = 1;
  entry->mOrphaned = false;
}

void Manager::ReleaseCacheId(CacheId aCacheId) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  for (uint32_t i = 0; i < mCacheIdRefs.Length(); ++i) {
    if (mCacheIdRefs[i].mCacheId == aCacheId) {
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
      uint32_t oldRef = mCacheIdRefs[i].mCount;
#endif
      mCacheIdRefs[i].mCount -= 1;
      MOZ_DIAGNOSTIC_ASSERT(mCacheIdRefs[i].mCount < oldRef);
      if (mCacheIdRefs[i].mCount == 0) {
        bool orphaned = mCacheIdRefs[i].mOrphaned;
        mCacheIdRefs.RemoveElementAt(i);
        const auto pinnedContext =
            SafeRefPtr{mContext, AcquireStrongRefFromRawPtr{}};
        // If the context is already gone, then orphan flag should have been
        // set in RemoveContext().
        if (orphaned && pinnedContext) {
          if (pinnedContext->IsCanceled()) {
            pinnedContext->NoteOrphanedData();
          } else {
            pinnedContext->CancelForCacheId(aCacheId);
            pinnedContext->Dispatch(MakeSafeRefPtr<DeleteOrphanedCacheAction>(
                SafeRefPtrFromThis(), aCacheId));
          }
        }
      }
      MaybeAllowContextToClose();
      return;
    }
  }
  MOZ_ASSERT_UNREACHABLE("Attempt to release CacheId that is not referenced!");
}

void Manager::AddRefBodyId(const nsID& aBodyId) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  for (uint32_t i = 0; i < mBodyIdRefs.Length(); ++i) {
    if (mBodyIdRefs[i].mBodyId == aBodyId) {
      mBodyIdRefs[i].mCount += 1;
      return;
    }
  }
  BodyIdRefCounter* entry = mBodyIdRefs.AppendElement();
  entry->mBodyId = aBodyId;
  entry->mCount = 1;
  entry->mOrphaned = false;
}

void Manager::ReleaseBodyId(const nsID& aBodyId) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  for (uint32_t i = 0; i < mBodyIdRefs.Length(); ++i) {
    if (mBodyIdRefs[i].mBodyId == aBodyId) {
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
      uint32_t oldRef = mBodyIdRefs[i].mCount;
#endif
      mBodyIdRefs[i].mCount -= 1;
      MOZ_DIAGNOSTIC_ASSERT(mBodyIdRefs[i].mCount < oldRef);
      if (mBodyIdRefs[i].mCount < 1) {
        bool orphaned = mBodyIdRefs[i].mOrphaned;
        mBodyIdRefs.RemoveElementAt(i);
        const auto pinnedContext =
            SafeRefPtr{mContext, AcquireStrongRefFromRawPtr{}};
        // If the context is already gone, then orphan flag should have been
        // set in RemoveContext().
        if (orphaned && pinnedContext) {
          if (pinnedContext->IsCanceled()) {
            pinnedContext->NoteOrphanedData();
          } else {
            pinnedContext->Dispatch(
                MakeSafeRefPtr<DeleteOrphanedBodyAction>(aBodyId));
          }
        }
      }
      MaybeAllowContextToClose();
      return;
    }
  }
  MOZ_ASSERT_UNREACHABLE("Attempt to release BodyId that is not referenced!");
}

const ManagerId& Manager::GetManagerId() const { return *mManagerId; }

void Manager::AddStreamList(StreamList* aStreamList) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_DIAGNOSTIC_ASSERT(aStreamList);
  mStreamLists.AppendElement(aStreamList);
}

void Manager::RemoveStreamList(StreamList* aStreamList) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_DIAGNOSTIC_ASSERT(aStreamList);
  mStreamLists.RemoveElement(aStreamList);
}

void Manager::ExecuteCacheOp(Listener* aListener, CacheId aCacheId,
                             const CacheOpArgs& aOpArgs) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_DIAGNOSTIC_ASSERT(aListener);
  MOZ_DIAGNOSTIC_ASSERT(aOpArgs.type() != CacheOpArgs::TCachePutAllArgs);

  if (NS_WARN_IF(mState == Closing)) {
    aListener->OnOpComplete(ErrorResult(NS_ERROR_FAILURE), void_t());
    return;
  }

  const auto pinnedContext = SafeRefPtr{mContext, AcquireStrongRefFromRawPtr{}};
  MOZ_DIAGNOSTIC_ASSERT(!pinnedContext->IsCanceled());

  auto action = [this, aListener, aCacheId, &aOpArgs,
                 &pinnedContext]() -> SafeRefPtr<Action> {
    const ListenerId listenerId = SaveListener(aListener);

    if (CacheOpArgs::TCacheDeleteArgs == aOpArgs.type()) {
      return MakeSafeRefPtr<CacheDeleteAction>(SafeRefPtrFromThis(), listenerId,
                                               aCacheId,
                                               aOpArgs.get_CacheDeleteArgs());
    }

    auto streamList = MakeSafeRefPtr<StreamList>(SafeRefPtrFromThis(),
                                                 pinnedContext.clonePtr());

    switch (aOpArgs.type()) {
      case CacheOpArgs::TCacheMatchArgs:
        return MakeSafeRefPtr<CacheMatchAction>(
            SafeRefPtrFromThis(), listenerId, aCacheId,
            aOpArgs.get_CacheMatchArgs(), std::move(streamList));
      case CacheOpArgs::TCacheMatchAllArgs:
        return MakeSafeRefPtr<CacheMatchAllAction>(
            SafeRefPtrFromThis(), listenerId, aCacheId,
            aOpArgs.get_CacheMatchAllArgs(), std::move(streamList));
      case CacheOpArgs::TCacheKeysArgs:
        return MakeSafeRefPtr<CacheKeysAction>(
            SafeRefPtrFromThis(), listenerId, aCacheId,
            aOpArgs.get_CacheKeysArgs(), std::move(streamList));
      default:
        MOZ_CRASH("Unknown Cache operation!");
    }
  }();

  pinnedContext->Dispatch(std::move(action));
}

void Manager::ExecuteStorageOp(Listener* aListener, Namespace aNamespace,
                               const CacheOpArgs& aOpArgs) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_DIAGNOSTIC_ASSERT(aListener);

  if (NS_WARN_IF(mState == Closing)) {
    aListener->OnOpComplete(ErrorResult(NS_ERROR_FAILURE), void_t());
    return;
  }

  const auto pinnedContext = SafeRefPtr{mContext, AcquireStrongRefFromRawPtr{}};
  MOZ_DIAGNOSTIC_ASSERT(!pinnedContext->IsCanceled());

  auto action = [this, aListener, aNamespace, &aOpArgs,
                 &pinnedContext]() -> SafeRefPtr<Action> {
    const ListenerId listenerId = SaveListener(aListener);

    switch (aOpArgs.type()) {
      case CacheOpArgs::TStorageMatchArgs:
        return MakeSafeRefPtr<StorageMatchAction>(
            SafeRefPtrFromThis(), listenerId, aNamespace,
            aOpArgs.get_StorageMatchArgs(),
            MakeSafeRefPtr<StreamList>(SafeRefPtrFromThis(),
                                       pinnedContext.clonePtr()));
      case CacheOpArgs::TStorageHasArgs:
        return MakeSafeRefPtr<StorageHasAction>(SafeRefPtrFromThis(),
                                                listenerId, aNamespace,
                                                aOpArgs.get_StorageHasArgs());
      case CacheOpArgs::TStorageOpenArgs:
        return MakeSafeRefPtr<StorageOpenAction>(SafeRefPtrFromThis(),
                                                 listenerId, aNamespace,
                                                 aOpArgs.get_StorageOpenArgs());
      case CacheOpArgs::TStorageDeleteArgs:
        return MakeSafeRefPtr<StorageDeleteAction>(
            SafeRefPtrFromThis(), listenerId, aNamespace,
            aOpArgs.get_StorageDeleteArgs());
      case CacheOpArgs::TStorageKeysArgs:
        return MakeSafeRefPtr<StorageKeysAction>(SafeRefPtrFromThis(),
                                                 listenerId, aNamespace);
      default:
        MOZ_CRASH("Unknown CacheStorage operation!");
    }
  }();

  pinnedContext->Dispatch(std::move(action));
}

void Manager::ExecuteOpenStream(Listener* aListener,
                                InputStreamResolver&& aResolver,
                                const nsID& aBodyId) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_DIAGNOSTIC_ASSERT(aListener);
  MOZ_DIAGNOSTIC_ASSERT(aResolver);

  if (NS_WARN_IF(mState == Closing)) {
    aResolver(nullptr);
    return;
  }

  const auto pinnedContext = SafeRefPtr{mContext, AcquireStrongRefFromRawPtr{}};
  MOZ_DIAGNOSTIC_ASSERT(!pinnedContext->IsCanceled());

  // We save the listener simply to track the existence of the caller here.
  // Our returned value will really be passed to the resolver when the
  // operation completes.  In the future we should remove the Listener
  // mechanism in favor of std::function or MozPromise.
  ListenerId listenerId = SaveListener(aListener);

  pinnedContext->Dispatch(MakeSafeRefPtr<OpenStreamAction>(
      SafeRefPtrFromThis(), listenerId, std::move(aResolver), aBodyId));
}

void Manager::ExecutePutAll(
    Listener* aListener, CacheId aCacheId,
    const nsTArray<CacheRequestResponse>& aPutList,
    const nsTArray<nsCOMPtr<nsIInputStream>>& aRequestStreamList,
    const nsTArray<nsCOMPtr<nsIInputStream>>& aResponseStreamList) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_DIAGNOSTIC_ASSERT(aListener);

  if (NS_WARN_IF(mState == Closing)) {
    aListener->OnOpComplete(ErrorResult(NS_ERROR_FAILURE), CachePutAllResult());
    return;
  }

  const auto pinnedContext = SafeRefPtr{mContext, AcquireStrongRefFromRawPtr{}};
  MOZ_DIAGNOSTIC_ASSERT(!pinnedContext->IsCanceled());

  ListenerId listenerId = SaveListener(aListener);
  pinnedContext->Dispatch(MakeSafeRefPtr<CachePutAllAction>(
      SafeRefPtrFromThis(), listenerId, aCacheId, aPutList, aRequestStreamList,
      aResponseStreamList));
}

Manager::Manager(SafeRefPtr<ManagerId> aManagerId, nsIThread* aIOThread,
                 const ConstructorGuard&)
    : mManagerId(std::move(aManagerId)),
      mIOThread(aIOThread),
      mContext(nullptr),
      mShuttingDown(false),
      mState(Open) {
  MOZ_DIAGNOSTIC_ASSERT(mManagerId);
  MOZ_DIAGNOSTIC_ASSERT(mIOThread);
}

Manager::~Manager() {
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_DIAGNOSTIC_ASSERT(mState == Closing);
  MOZ_DIAGNOSTIC_ASSERT(!mContext);

  nsCOMPtr<nsIThread> ioThread;
  mIOThread.swap(ioThread);

  // Don't spin the event loop in the destructor waiting for the thread to
  // shutdown.  Defer this to the main thread, instead.
  MOZ_ALWAYS_SUCCEEDS(NS_DispatchToMainThread(NewRunnableMethod(
      "nsIThread::AsyncShutdown", ioThread, &nsIThread::AsyncShutdown)));
}

void Manager::Init(Maybe<Manager&> aOldManager) {
  NS_ASSERT_OWNINGTHREAD(Manager);

  // Create the context immediately.  Since there can at most be one Context
  // per Manager now, this lets us cleanly call Factory::Remove() once the
  // Context goes away.
  SafeRefPtr<Context> ref = Context::Create(
      SafeRefPtrFromThis(), mIOThread->SerialEventTarget(),
      MakeSafeRefPtr<SetupAction>(),
      aOldManager ? SomeRef(*aOldManager->mContext) : Nothing());
  mContext = ref.unsafeGetRawPtr();
}

void Manager::Shutdown() {
  NS_ASSERT_OWNINGTHREAD(Manager);

  // Ignore duplicate attempts to shutdown.  This can occur when we start
  // a browser initiated shutdown and then run ~Manager() which also
  // calls Shutdown().
  if (mShuttingDown) {
    return;
  }

  mShuttingDown = true;

  // Note that we are closing to prevent any new requests from coming in and
  // creating a new Context.  We must ensure all Contexts and IO operations are
  // complete before shutdown proceeds.
  NoteClosing();

  // If there is a context, then cancel and only note that we are done after
  // its cleaned up.
  if (mContext) {
    const auto pinnedContext =
        SafeRefPtr{mContext, AcquireStrongRefFromRawPtr{}};
    pinnedContext->CancelAll();
    return;
  }
}

void Manager::Abort() {
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_DIAGNOSTIC_ASSERT(mContext);

  // Note that we are closing to prevent any new requests from coming in and
  // creating a new Context.  We must ensure all Contexts and IO operations are
  // complete before origin clear proceeds.
  NoteClosing();

  // Cancel and only note that we are done after the context is cleaned up.
  const auto pinnedContext = SafeRefPtr{mContext, AcquireStrongRefFromRawPtr{}};
  pinnedContext->CancelAll();
}

Manager::ListenerId Manager::SaveListener(Listener* aListener) {
  NS_ASSERT_OWNINGTHREAD(Manager);

  // Once a Listener is added, we keep a reference to it until its
  // removed.  Since the same Listener might make multiple requests,
  // ensure we only have a single reference in our list.
  ListenerList::index_type index =
      mListeners.IndexOf(aListener, 0, ListenerEntryListenerComparator());
  if (index != ListenerList::NoIndex) {
    return mListeners[index].mId;
  }

  ListenerId id = sNextListenerId;
  sNextListenerId += 1;

  mListeners.AppendElement(ListenerEntry(id, aListener));
  return id;
}

Manager::Listener* Manager::GetListener(ListenerId aListenerId) const {
  NS_ASSERT_OWNINGTHREAD(Manager);
  ListenerList::index_type index =
      mListeners.IndexOf(aListenerId, 0, ListenerEntryIdComparator());
  if (index != ListenerList::NoIndex) {
    return mListeners[index].mListener;
  }

  // This can legitimately happen if the actor is deleted while a request is
  // in process.  For example, the child process OOMs.
  return nullptr;
}

bool Manager::SetCacheIdOrphanedIfRefed(CacheId aCacheId) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  for (uint32_t i = 0; i < mCacheIdRefs.Length(); ++i) {
    if (mCacheIdRefs[i].mCacheId == aCacheId) {
      MOZ_DIAGNOSTIC_ASSERT(mCacheIdRefs[i].mCount > 0);
      MOZ_DIAGNOSTIC_ASSERT(!mCacheIdRefs[i].mOrphaned);
      mCacheIdRefs[i].mOrphaned = true;
      return true;
    }
  }
  return false;
}

// TODO: provide way to set body non-orphaned if its added back to a cache (bug
// 1110479)

bool Manager::SetBodyIdOrphanedIfRefed(const nsID& aBodyId) {
  NS_ASSERT_OWNINGTHREAD(Manager);
  for (uint32_t i = 0; i < mBodyIdRefs.Length(); ++i) {
    if (mBodyIdRefs[i].mBodyId == aBodyId) {
      MOZ_DIAGNOSTIC_ASSERT(mBodyIdRefs[i].mCount > 0);
      MOZ_DIAGNOSTIC_ASSERT(!mBodyIdRefs[i].mOrphaned);
      mBodyIdRefs[i].mOrphaned = true;
      return true;
    }
  }
  return false;
}

void Manager::NoteOrphanedBodyIdList(const nsTArray<nsID>& aDeletedBodyIdList) {
  NS_ASSERT_OWNINGTHREAD(Manager);

  // XXX TransformIfIntoNewArray might be generalized to allow specifying the
  // type of nsTArray to create, so that it can create an AutoTArray as well; an
  // TransformIf (without AbortOnErr) might be added, which could be used here.
  DeleteOrphanedBodyAction::DeletedBodyIdList deleteNowList;
  deleteNowList.SetCapacity(aDeletedBodyIdList.Length());

  for (uint32_t i = 0; i < aDeletedBodyIdList.Length(); ++i) {
    if (!SetBodyIdOrphanedIfRefed(aDeletedBodyIdList[i])) {
      deleteNowList.AppendElement(aDeletedBodyIdList[i]);
    }
  }

  // TODO: note that we need to check these bodies for staleness on startup (bug
  // 1110446)
  const auto pinnedContext = SafeRefPtr{mContext, AcquireStrongRefFromRawPtr{}};
  if (!deleteNowList.IsEmpty() && pinnedContext &&
      !pinnedContext->IsCanceled()) {
    pinnedContext->Dispatch(
        MakeSafeRefPtr<DeleteOrphanedBodyAction>(std::move(deleteNowList)));
  }
}

void Manager::MaybeAllowContextToClose() {
  NS_ASSERT_OWNINGTHREAD(Manager);

  // If we have an active context, but we have no more users of the Manager,
  // then let it shut itself down.  We must wait for all possible users of
  // Cache state information to complete before doing this.  Once we allow
  // the Context to close we may not reliably get notified of storage
  // invalidation.
  const auto pinnedContext = SafeRefPtr{mContext, AcquireStrongRefFromRawPtr{}};
  if (pinnedContext && mListeners.IsEmpty() && mCacheIdRefs.IsEmpty() &&
      mBodyIdRefs.IsEmpty()) {
    // Mark this Manager as invalid so that it won't get used again.  We don't
    // want to start any new operations once we allow the Context to close since
    // it may race with the underlying storage getting invalidated.
    NoteClosing();

    pinnedContext->AllowToClose();
  }
}

}  // namespace cache
}  // namespace dom
}  // namespace mozilla

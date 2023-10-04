/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

#include "eden/fs/store/hg/HgDatapackStore.h"

#include <folly/Optional.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>

#include "eden/fs/config/ReloadableConfig.h"
#include "eden/fs/model/Blob.h"
#include "eden/fs/model/BlobMetadata.h"
#include "eden/fs/model/Hash.h"
#include "eden/fs/model/Tree.h"
#include "eden/fs/model/TreeEntry.h"
#include "eden/fs/store/ObjectFetchContext.h"
#include "eden/fs/store/hg/HgImportRequest.h"
#include "eden/fs/store/hg/HgProxyHash.h"
#include "eden/fs/telemetry/LogEvent.h"
#include "eden/fs/telemetry/StructuredLogger.h"
#include "eden/fs/utils/Bug.h"
#include "eden/fs/utils/RefPtr.h"

namespace facebook::eden {

namespace {

TreeEntryType fromRawTreeEntryType(sapling::TreeEntryType type) {
  switch (type) {
    case sapling::TreeEntryType::RegularFile:
      return TreeEntryType::REGULAR_FILE;
    case sapling::TreeEntryType::Tree:
      return TreeEntryType::TREE;
    case sapling::TreeEntryType::ExecutableFile:
      return TreeEntryType::EXECUTABLE_FILE;
    case sapling::TreeEntryType::Symlink:
      return TreeEntryType::SYMLINK;
  }
  EDEN_BUG() << "unknown tree entry type " << static_cast<uint32_t>(type)
             << " loaded from data store";
}

Tree::value_type fromRawTreeEntry(
    sapling::TreeEntry entry,
    RelativePathPiece path,
    HgObjectIdFormat hgObjectIdFormat) {
  std::optional<uint64_t> size;
  std::optional<Hash20> contentSha1;
  std::optional<Hash32> contentBlake3;

  if (entry.size != nullptr) {
    size = *entry.size;
  }

  if (entry.content_sha1 != nullptr) {
    contentSha1.emplace(*entry.content_sha1);
  }

  if (entry.content_blake3 != nullptr) {
    contentBlake3.emplace(*entry.content_blake3);
  }

  auto name = PathComponent(folly::StringPiece{entry.name.asByteRange()});
  auto hash = Hash20{entry.hash};

  auto fullPath = path + name;
  auto proxyHash = HgProxyHash::store(fullPath, hash, hgObjectIdFormat);

  auto treeEntry = TreeEntry{
      proxyHash,
      fromRawTreeEntryType(entry.ttype),
      size,
      contentSha1,
      contentBlake3};
  return {std::move(name), std::move(treeEntry)};
}

TreePtr fromRawTree(
    const sapling::Tree* tree,
    const ObjectId& edenTreeId,
    RelativePathPiece path,
    HgObjectIdFormat hgObjectIdFormat,
    const std::unordered_set<RelativePath>& filteredPaths) {
  Tree::container entries{kPathMapDefaultCaseSensitive};

  entries.reserve(tree->length);
  for (uintptr_t i = 0; i < tree->length; i++) {
    try {
      auto entry = fromRawTreeEntry(tree->entries[i], path, hgObjectIdFormat);
      // TODO(xavierd): In the case where this checks becomes too hot, we may
      // need to change to a Trie like datastructure for fast filtering.
      if (filteredPaths.empty() ||
          filteredPaths.count(path + entry.first) == 0) {
        entries.emplace(entry.first, std::move(entry.second));
      }
    } catch (const PathComponentContainsDirectorySeparator& ex) {
      XLOG(WARN) << "Ignoring directory entry: " << ex.what();
    }
  }
  return std::make_shared<TreePtr::element_type>(
      std::move(entries), edenTreeId);
}

} // namespace

void HgDatapackStore::getTreeBatch(const ImportRequestsList& importRequests) {
  // TODO: extract each ClientRequestInfo from importRequests into a
  // sapling::ClientRequestInfo and pass them with the corresponding
  // sapling::NodeId

  // Group requests by proxyHash to ensure no duplicates in fetch request to
  // SaplingNativeBackingStore.
  ImportRequestsMap importRequestsMap;
  for (const auto& importRequest : importRequests) {
    auto nodeId = importRequest->getRequest<HgImportRequest::TreeImport>()
                      ->proxyHash.byteHash();
    // Look for and log duplicates.
    const auto& importRequestsEntry = importRequestsMap.find(nodeId);
    if (importRequestsEntry != importRequestsMap.end()) {
      XLOGF(DBG9, "Duplicate tree fetch request with proxyHash: {}", nodeId);
      auto& importRequestList = importRequestsEntry->second.first;

      // Only look for mismatched requests if logging level is high enough.
      // Make sure this level is the same as the XLOG_IF statement below.
      if (XLOG_IS_ON(DBG9)) {
        // Log requests that do not have the same hash (ObjectId).
        // This happens when two paths (file or directory) have same content.
        for (const auto& priorRequest : importRequestList) {
          XLOGF_IF(
              DBG9,
              UNLIKELY(
                  priorRequest->getRequest<HgImportRequest::TreeImport>()
                      ->hash !=
                  importRequest->getRequest<HgImportRequest::TreeImport>()
                      ->hash),
              "Tree requests have the same proxyHash (HgProxyHash) but different hash (ObjectId). "
              "This should not happen. Previous request: proxyHash='{}', hash='{}'; "
              "current request: proxyHash ='{}', hash='{}'.",
              folly::hexlify(
                  priorRequest->getRequest<HgImportRequest::TreeImport>()
                      ->proxyHash.getValue()),
              priorRequest->getRequest<HgImportRequest::TreeImport>()
                  ->hash.asHexString(),
              folly::hexlify(
                  importRequest->getRequest<HgImportRequest::TreeImport>()
                      ->proxyHash.getValue()),
              importRequest->getRequest<HgImportRequest::TreeImport>()
                  ->hash.asHexString());
        }
      }
      importRequestList.emplace_back(importRequest);
    } else {
      std::vector<std::shared_ptr<HgImportRequest>> requests({importRequest});
      importRequestsMap.emplace(
          nodeId, make_pair(requests, &liveBatchedBlobWatches_));
    }
  }

  // Indexable vector of nodeIds - required by SaplingNativeBackingStore API.
  std::vector<sapling::NodeId> requests;
  requests.reserve(importRequestsMap.size());
  std::transform(
      importRequestsMap.begin(),
      importRequestsMap.end(),
      std::back_inserter(requests),
      [](auto& pair) { return pair.first; });

  auto hgObjectIdFormat = config_->getEdenConfig()->hgObjectIdFormat.getValue();
  const auto& filteredPaths =
      config_->getEdenConfig()->hgFilteredPaths.getValue();

  store_.getTreeBatch(
      folly::range(requests),
      false,
      // store_.getTreeBatch is blocking, hence we can take these by reference.
      [&](size_t index,
          folly::Try<std::shared_ptr<sapling::Tree>> content) mutable {
        if (config_->getEdenConfig()->hgTreeFetchFallback.getValue() &&
            content.hasException()) {
          if (logger_) {
            logger_->logEvent(EdenApiMiss{
                repoName_,
                EdenApiMiss::Tree,
                content.exception().what().toStdString()});
          }

          // If we're falling back, the caller will fulfill this Promise with a
          // tree from HgImporter.
          return;
        }

        XLOGF(DBG9, "Imported tree node={}", folly::hexlify(requests[index]));
        const auto& nodeId = requests[index];
        auto& [importRequestList, watch] = importRequestsMap[nodeId];
        for (auto& importRequest : importRequestList) {
          auto* treeRequest =
              importRequest->getRequest<HgImportRequest::TreeImport>();
          importRequest->getPromise<TreePtr>()->setWith(
              [&]() -> folly::Try<TreePtr> {
                if (content.hasException()) {
                  return folly::Try<TreePtr>{content.exception()};
                }
                return folly::Try{fromRawTree(
                    content.value().get(),
                    treeRequest->hash,
                    treeRequest->proxyHash.path(),
                    hgObjectIdFormat,
                    filteredPaths)};
              });
        }

        // Make sure that we're stopping this watch.
        watch.reset();
      });
}

TreePtr HgDatapackStore::getTree(
    const RelativePath& path,
    const Hash20& manifestId,
    const ObjectId& edenTreeId,
    const ObjectFetchContextPtr& /*context*/) {
  // For root trees we will try getting the tree locally first.  This allows
  // us to catch when Mercurial might have just written a tree to the store,
  // and refresh the store so that the store can pick it up.  We don't do
  // this for all trees, as it would cause a lot of additional work on every
  // cache miss, and just doing it for root trees is sufficient to detect the
  // scenario where Mercurial just wrote a brand new tree.
  bool local_only = path.empty();
  auto tree = store_.getTree(
      manifestId.getBytes(),
      local_only /*, sapling::ClientRequestInfo(context)*/);
  if (!tree && local_only) {
    // Mercurial might have just written the tree to the store. Refresh the
    // store and try again, this time allowing remote fetches.
    store_.flush();
    tree = store_.getTree(
        manifestId.getBytes(), false /*, sapling::ClientRequestInfo(context)*/);
  }
  if (tree) {
    auto hgObjectIdFormat =
        config_->getEdenConfig()->hgObjectIdFormat.getValue();
    const auto& filteredPaths =
        config_->getEdenConfig()->hgFilteredPaths.getValue();
    return fromRawTree(
        tree.get(), edenTreeId, path, hgObjectIdFormat, filteredPaths);
  }
  return nullptr;
}

TreePtr HgDatapackStore::getTreeLocal(
    const ObjectId& edenTreeId,
    const HgProxyHash& proxyHash) {
  auto tree = store_.getTree(proxyHash.byteHash(), /*local=*/true);
  if (tree) {
    auto hgObjectIdFormat =
        config_->getEdenConfig()->hgObjectIdFormat.getValue();
    const auto& filteredPaths =
        config_->getEdenConfig()->hgFilteredPaths.getValue();
    return fromRawTree(
        tree.get(),
        edenTreeId,
        proxyHash.path(),
        hgObjectIdFormat,
        filteredPaths);
  }

  return nullptr;
}

void HgDatapackStore::getBlobBatch(const ImportRequestsList& importRequests) {
  // TODO: extract each ClientRequestInfo from importRequests into a
  // sapling::ClientRequestInfo and pass them with the corresponding
  // sapling::NodeId

  // Group requests by proxyHash to ensure no duplicates in fetch request to
  // SaplingNativeBackingStore.
  ImportRequestsMap importRequestsMap;
  for (const auto& importRequest : importRequests) {
    auto nodeId = importRequest->getRequest<HgImportRequest::BlobImport>()
                      ->proxyHash.byteHash();
    // Look for and log duplicates.
    const auto& importRequestsEntry = importRequestsMap.find(nodeId);
    if (importRequestsEntry != importRequestsMap.end()) {
      XLOGF(DBG9, "Duplicate blob fetch request with proxyHash: {}", nodeId);
      auto& importRequestList = importRequestsEntry->second.first;

      // Only look for mismatched requests if logging level is high enough.
      // Make sure this level is the same as the XLOG_IF statement below.
      if (XLOG_IS_ON(DBG9)) {
        // Log requests that do not have the same hash (ObjectId).
        // This happens when two paths (file or directory) have same content.
        for (const auto& priorRequest : importRequestList) {
          XLOGF_IF(
              DBG9,
              UNLIKELY(
                  priorRequest->getRequest<HgImportRequest::BlobImport>()
                      ->hash !=
                  importRequest->getRequest<HgImportRequest::BlobImport>()
                      ->hash),
              "Blob requests have the same proxyHash (HgProxyHash) but different hash (ObjectId). "
              "This should not happen. Previous request: proxyHash='{}', hash='{}'; "
              "current request: proxyHash ='{}', hash='{}'.",
              folly::hexlify(
                  priorRequest->getRequest<HgImportRequest::BlobImport>()
                      ->proxyHash.getValue()),
              priorRequest->getRequest<HgImportRequest::BlobImport>()
                  ->hash.asHexString(),
              folly::hexlify(
                  importRequest->getRequest<HgImportRequest::BlobImport>()
                      ->proxyHash.getValue()),
              importRequest->getRequest<HgImportRequest::BlobImport>()
                  ->hash.asHexString());
        }
      }
      importRequestList.emplace_back(importRequest);
    } else {
      std::vector<std::shared_ptr<HgImportRequest>> requests({importRequest});
      importRequestsMap.emplace(
          nodeId, make_pair(requests, &liveBatchedBlobWatches_));
    }
  }

  // Indexable vector of nodeIds - required by SaplingNativeBackingStore API.
  std::vector<sapling::NodeId> requests;
  requests.reserve(importRequestsMap.size());
  std::transform(
      importRequestsMap.begin(),
      importRequestsMap.end(),
      std::back_inserter(requests),
      [](auto& pair) { return pair.first; });

  store_.getBlobBatch(
      folly::range(requests),
      false,
      // store_.getBlobBatch is blocking, hence we can take these by reference.
      [&](size_t index, folly::Try<std::unique_ptr<folly::IOBuf>> content) {
        if (config_->getEdenConfig()->hgBlobFetchFallback.getValue() &&
            content.hasException()) {
          if (logger_) {
            logger_->logEvent(EdenApiMiss{
                repoName_,
                EdenApiMiss::Blob,
                content.exception().what().toStdString()});
          }

          // If we're falling back, the caller will fulfill this Promise with a
          // blob from HgImporter.
          return;
        }

        XLOGF(DBG9, "Imported blob node={}", folly::hexlify(requests[index]));
        const auto& nodeId = requests[index];
        auto& [importRequestList, watch] = importRequestsMap[nodeId];
        auto result = content.hasException()
            ? folly::Try<BlobPtr>{content.exception()}
            : folly::Try{
                  std::make_shared<BlobPtr::element_type>(*content.value())};
        for (auto& importRequest : importRequestList) {
          importRequest->getPromise<BlobPtr>()->setWith(
              [&]() -> folly::Try<BlobPtr> { return result; });
        }

        // Make sure that we're stopping this watch.
        watch.reset();
      });
}

BlobPtr HgDatapackStore::getBlobLocal(const HgProxyHash& hgInfo) {
  auto content = store_.getBlob(hgInfo.byteHash(), true);
  if (content) {
    return std::make_shared<BlobPtr::element_type>(std::move(*content));
  }

  return nullptr;
}

BlobMetadataPtr HgDatapackStore::getLocalBlobMetadata(
    const HgProxyHash& hgInfo) {
  auto metadata =
      store_.getBlobMetadata(hgInfo.byteHash(), true /*local_only*/);
  if (metadata) {
    std::optional<Hash32> blake3;
    if (metadata->content_blake3 != nullptr) {
      blake3.emplace(*metadata->content_blake3);
    }
    return std::make_shared<BlobMetadataPtr::element_type>(BlobMetadata{
        Hash20{metadata->content_sha1}, blake3, metadata->total_size});
  }
  return nullptr;
}

void HgDatapackStore::getBlobMetadataBatch(
    const ImportRequestsList& importRequests) {
  size_t count = importRequests.size();

  // TODO: extract each ClientRequestInfo from importRequests into a
  // sapling::ClientRequestInfo and pass them with the corresponding
  // sapling::NodeId
  std::vector<sapling::NodeId> requests;
  requests.reserve(count);
  for (const auto& importRequest : importRequests) {
    requests.emplace_back(
        importRequest->getRequest<HgImportRequest::BlobMetaImport>()
            ->proxyHash.byteHash());
  }

  std::vector<RequestMetricsScope> requestsWatches;
  requestsWatches.reserve(count);
  for (auto i = 0ul; i < count; i++) {
    requestsWatches.emplace_back(&liveBatchedBlobMetaWatches_);
  }

  store_.getBlobMetadataBatch(
      folly::range(requests),
      false,
      [&](size_t index,
          folly::Try<std::shared_ptr<sapling::FileAuxData>> auxTry) {
        if (auxTry.hasException() &&
            config_->getEdenConfig()->hgBlobMetaFetchFallback.getValue()) {
          // The caller will fallback to fetching the blob.
          // TODO: Remove this.
          return;
        }

        XLOGF(DBG9, "Imported aux={}", folly::hexlify(requests[index]));
        auto& importRequest = importRequests[index];
        importRequest->getPromise<BlobMetadataPtr>()->setWith(
            [&]() -> folly::Try<BlobMetadataPtr> {
              if (auxTry.hasException()) {
                return folly::Try<BlobMetadataPtr>{
                    std::move(auxTry).exception()};
              }

              auto& aux = auxTry.value();
              std::optional<Hash32> blake3;
              if (aux->content_blake3 != nullptr) {
                blake3.emplace(*aux->content_blake3);
              }

              return folly::Try{std::make_shared<BlobMetadataPtr::element_type>(
                  Hash20{aux->content_sha1}, blake3, aux->total_size)};
            });

        // Make sure that we're stopping this watch.
        requestsWatches[index].reset();
      });
}

void HgDatapackStore::flush() {
  store_.flush();
}

} // namespace facebook::eden

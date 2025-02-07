/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"

#include "ChannelMediaResource.h"
#include "CloneableWithRangeMediaResource.h"
#include "DecoderTraits.h"
#include "FileMediaResource.h"
#include "MediaResource.h"
#include "MediaResourceCallback.h"

#include "mozilla/Mutex.h"
#include "nsDebug.h"
#include "nsNetUtil.h"
#include "nsThreadUtils.h"
#include "nsIFile.h"
#include "nsIFileChannel.h"
#include "nsIHttpChannel.h"
#include "nsISeekableStream.h"
#include "nsIInputStream.h"
#include "nsIRequestObserver.h"
#include "nsIStreamListener.h"
#include "nsIScriptSecurityManager.h"
#include "mozilla/dom/BlobImpl.h"
#include "mozilla/dom/HTMLMediaElement.h"
#include "nsError.h"
#include "nsContentUtils.h"
#include "nsHostObjectProtocolHandler.h"
#include <algorithm>
#include "nsProxyRelease.h"
#include "nsICloneableInputStream.h"
#include "nsIContentPolicy.h"
#include "mozilla/ErrorNames.h"

using mozilla::media::TimeUnit;

#undef ILOG

mozilla::LazyLogModule gMediaResourceIndexLog("MediaResourceIndex");
// Debug logging macro with object pointer and class name.
#define ILOG(msg, ...)                                                         \
  MOZ_LOG(gMediaResourceIndexLog,                                              \
          mozilla::LogLevel::Debug,                                            \
          ("%p " msg, this, ##__VA_ARGS__))

namespace mozilla {

void
MediaResource::Destroy()
{
  // Ensures we only delete the MediaResource on the main thread.
  if (NS_IsMainThread()) {
    delete this;
    return;
  }
  nsresult rv = SystemGroup::Dispatch(
    TaskCategory::Other,
    NewNonOwningRunnableMethod(
      "MediaResource::Destroy", this, &MediaResource::Destroy));
  MOZ_ALWAYS_SUCCEEDS(rv);
}

NS_IMPL_ADDREF(MediaResource)
NS_IMPL_RELEASE_WITH_DESTROY(MediaResource, Destroy())

already_AddRefed<BaseMediaResource>
BaseMediaResource::Create(MediaResourceCallback* aCallback,
                          nsIChannel* aChannel,
                          bool aIsPrivateBrowsing)
{
  NS_ASSERTION(NS_IsMainThread(),
               "MediaResource::Open called on non-main thread");

  // If the channel was redirected, we want the post-redirect URI;
  // but if the URI scheme was expanded, say from chrome: to jar:file:,
  // we want the original URI.
  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_GetFinalChannelURI(aChannel, getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, nullptr);

  nsAutoCString contentTypeString;
  aChannel->GetContentType(contentTypeString);
  Maybe<MediaContainerType> containerType = MakeMediaContainerType(contentTypeString);
  if (!containerType) {
    return nullptr;
  }

  // Let's try to create a FileMediaResource in case the channel is a nsIFile
  nsCOMPtr<nsIFileChannel> fc = do_QueryInterface(aChannel);
  if (fc) {
    RefPtr<BaseMediaResource> resource =
      new FileMediaResource(aCallback, aChannel, uri);
    return resource.forget();
  }

  RefPtr<mozilla::dom::BlobImpl> blobImpl;
  if (IsBlobURI(uri) &&
      NS_SUCCEEDED(NS_GetBlobForBlobURI(uri, getter_AddRefs(blobImpl))) &&
      blobImpl) {
    IgnoredErrorResult rv;

    nsCOMPtr<nsIInputStream> stream;
    blobImpl->CreateInputStream(getter_AddRefs(stream), rv);
    if (NS_WARN_IF(rv.Failed())) {
      return nullptr;
    }

    // It's better to read the size from the blob instead of using ::Available,
    // because, if the stream implements nsIAsyncInputStream interface,
    // ::Available will not return the size of the stream, but what can be
    // currently read.
    uint64_t size = blobImpl->GetSize(rv);
    if (NS_WARN_IF(rv.Failed())) {
      return nullptr;
    }

    // If the URL is a blob URL, with a seekable inputStream, we can still use
    // a FileMediaResource.
    nsCOMPtr<nsISeekableStream> seekableStream = do_QueryInterface(stream);
    if (seekableStream) {
      RefPtr<BaseMediaResource> resource =
        new FileMediaResource(aCallback, aChannel, uri, size);
      return resource.forget();
    }

    // Maybe this blob URL can be cloned with a range.
    nsCOMPtr<nsICloneableInputStreamWithRange> cloneableWithRange =
      do_QueryInterface(stream);
    if (cloneableWithRange) {
      RefPtr<BaseMediaResource> resource =
        new CloneableWithRangeMediaResource(aCallback, aChannel, uri, stream,
                                            size);
      return resource.forget();
    }
  }

  RefPtr<BaseMediaResource> resource =
      new ChannelMediaResource(aCallback, aChannel, uri, aIsPrivateBrowsing);
  return resource.forget();
}

void BaseMediaResource::SetLoadInBackground(bool aLoadInBackground) {
  if (aLoadInBackground == mLoadInBackground) {
    return;
  }
  mLoadInBackground = aLoadInBackground;
  if (!mChannel) {
    // No channel, resource is probably already loaded.
    return;
  }

  MediaDecoderOwner* owner = mCallback->GetMediaOwner();
  if (!owner) {
    NS_WARNING("Null owner in MediaResource::SetLoadInBackground()");
    return;
  }
  dom::HTMLMediaElement* element = owner->GetMediaElement();
  if (!element) {
    NS_WARNING("Null element in MediaResource::SetLoadInBackground()");
    return;
  }

  bool isPending = false;
  if (NS_SUCCEEDED(mChannel->IsPending(&isPending)) &&
      isPending) {
    nsLoadFlags loadFlags;
    DebugOnly<nsresult> rv = mChannel->GetLoadFlags(&loadFlags);
    NS_ASSERTION(NS_SUCCEEDED(rv), "GetLoadFlags() failed!");

    if (aLoadInBackground) {
      loadFlags |= nsIRequest::LOAD_BACKGROUND;
    } else {
      loadFlags &= ~nsIRequest::LOAD_BACKGROUND;
    }
    ModifyLoadFlags(loadFlags);
  }
}

void BaseMediaResource::ModifyLoadFlags(nsLoadFlags aFlags)
{
  nsCOMPtr<nsILoadGroup> loadGroup;
  nsresult rv = mChannel->GetLoadGroup(getter_AddRefs(loadGroup));
  MOZ_ASSERT(NS_SUCCEEDED(rv), "GetLoadGroup() failed!");

  nsresult status;
  mChannel->GetStatus(&status);

  bool inLoadGroup = false;
  if (loadGroup) {
    rv = loadGroup->RemoveRequest(mChannel, nullptr, status);
    if (NS_SUCCEEDED(rv)) {
      inLoadGroup = true;
    }
  }

  rv = mChannel->SetLoadFlags(aFlags);
  MOZ_ASSERT(NS_SUCCEEDED(rv), "SetLoadFlags() failed!");

  if (inLoadGroup) {
    rv = loadGroup->AddRequest(mChannel, nullptr);
    MOZ_ASSERT(NS_SUCCEEDED(rv), "AddRequest() failed!");
  }
}

void BaseMediaResource::DispatchBytesConsumed(int64_t aNumBytes, int64_t aOffset)
{
  if (aNumBytes <= 0) {
    return;
  }
  mCallback->NotifyBytesConsumed(aNumBytes, aOffset);
}

nsresult
MediaResourceIndex::Read(char* aBuffer, uint32_t aCount, uint32_t* aBytes)
{
  NS_ASSERTION(!NS_IsMainThread(), "Don't call on main thread");

  // We purposefuly don't check that we may attempt to read past
  // mResource->GetLength() as the resource's length may change over time.

  nsresult rv = ReadAt(mOffset, aBuffer, aCount, aBytes);
  if (NS_FAILED(rv)) {
    return rv;
  }
  mOffset += *aBytes;
  if (mOffset < 0) {
    // Very unlikely overflow; just return to position 0.
    mOffset = 0;
  }
  return NS_OK;
}

static nsCString
ResultName(nsresult aResult)
{
  nsCString name;
  GetErrorName(aResult, name);
  return name;
}

nsresult
MediaResourceIndex::ReadAt(int64_t aOffset,
                           char* aBuffer,
                           uint32_t aCount,
                           uint32_t* aBytes)
{
  if (mCacheBlockSize == 0) {
    return UncachedReadAt(aOffset, aBuffer, aCount, aBytes);
  }

  *aBytes = 0;

  if (aCount == 0) {
    return NS_OK;
  }

  const int64_t endOffset = aOffset + aCount;
  if (aOffset < 0 || endOffset < aOffset) {
    return NS_ERROR_ILLEGAL_VALUE;
  }

  const int64_t lastBlockOffset = CacheOffsetContaining(endOffset - 1);

  if (mCachedBytes != 0 && mCachedOffset + mCachedBytes >= aOffset &&
      mCachedOffset < endOffset) {
    // There is data in the cache that is not completely before aOffset and not
    // completely after endOffset, so it could be usable (with potential top-up).
    if (aOffset < mCachedOffset) {
      // We need to read before the cached data.
      const uint32_t toRead = uint32_t(mCachedOffset - aOffset);
      MOZ_ASSERT(toRead > 0);
      MOZ_ASSERT(toRead < aCount);
      uint32_t read = 0;
      nsresult rv = UncachedReadAt(aOffset, aBuffer, toRead, &read);
      if (NS_FAILED(rv)) {
        ILOG("ReadAt(%" PRIu32 "@%" PRId64
             ") uncached read before cache -> %s, %" PRIu32,
             aCount,
             aOffset,
             ResultName(rv).get(),
             *aBytes);
        return rv;
      }
      *aBytes = read;
      if (read < toRead) {
        // Could not read everything we wanted, we're done.
        ILOG("ReadAt(%" PRIu32 "@%" PRId64
             ") uncached read before cache, incomplete -> OK, %" PRIu32,
             aCount,
             aOffset,
             *aBytes);
        return NS_OK;
      }
      ILOG("ReadAt(%" PRIu32 "@%" PRId64
           ") uncached read before cache: %" PRIu32 ", remaining: %" PRIu32
           "@%" PRId64 "...",
           aCount,
           aOffset,
           read,
           aCount - read,
           aOffset + read);
      aOffset += read;
      aBuffer += read;
      aCount -= read;
      // We should have reached the cache.
      MOZ_ASSERT(aOffset == mCachedOffset);
    }
    MOZ_ASSERT(aOffset >= mCachedOffset);

    // We've reached our cache.
    const uint32_t toCopy =
      std::min(aCount, uint32_t(mCachedOffset + mCachedBytes - aOffset));
    // Note that we could in fact be just after the last byte of the cache, in
    // which case we can't actually read from it! (But we will top-up next.)
    if (toCopy != 0) {
      memcpy(aBuffer, &mCachedBlock[IndexInCache(aOffset)], toCopy);
      *aBytes += toCopy;
      aCount -= toCopy;
      if (aCount == 0) {
        // All done!
        ILOG("ReadAt(%" PRIu32 "@%" PRId64 ") copied everything (%" PRIu32
             ") from cache(%" PRIu32 "@%" PRId64 ") :-D -> OK, %" PRIu32,
             aCount,
             aOffset,
             toCopy,
             mCachedBytes,
             mCachedOffset,
             *aBytes);
        return NS_OK;
      }
      aOffset += toCopy;
      aBuffer += toCopy;
      ILOG("ReadAt(%" PRIu32 "@%" PRId64 ") copied %" PRIu32
           " from cache(%" PRIu32 "@%" PRId64 ") :-), remaining: %" PRIu32
           "@%" PRId64 "...",
           aCount + toCopy,
           aOffset - toCopy,
           toCopy,
           mCachedBytes,
           mCachedOffset,
           aCount,
           aOffset);
    }

    if (aOffset - 1 >= lastBlockOffset) {
      // We were already reading cached data from the last block, we need more
      // from it -> try to top-up, read what we can, and we'll be done.
      MOZ_ASSERT(aOffset == mCachedOffset + mCachedBytes);
      MOZ_ASSERT(endOffset <= lastBlockOffset + mCacheBlockSize);
      return CacheOrReadAt(aOffset, aBuffer, aCount, aBytes);
    }

    // We were not in the last block (but we may just have crossed the line now)
    MOZ_ASSERT(aOffset <= lastBlockOffset);
    // Continue below...
  } else if (aOffset >= lastBlockOffset) {
    // There was nothing we could get from the cache.
    // But we're already in the last block -> Cache or read what we can.
    // Make sure to invalidate the cache first.
    mCachedBytes = 0;
    return CacheOrReadAt(aOffset, aBuffer, aCount, aBytes);
  }

  // If we're here, either there was nothing usable in the cache, or we've just
  // read what was in the cache but there's still more to read.

  if (aOffset < lastBlockOffset) {
    // We need to read before the last block.
    // Start with an uncached read up to the last block.
    const uint32_t toRead = uint32_t(lastBlockOffset - aOffset);
    MOZ_ASSERT(toRead > 0);
    MOZ_ASSERT(toRead < aCount);
    uint32_t read = 0;
    nsresult rv = UncachedReadAt(aOffset, aBuffer, toRead, &read);
    if (NS_FAILED(rv)) {
      ILOG("ReadAt(%" PRIu32 "@%" PRId64
           ") uncached read before last block failed -> %s, %" PRIu32,
           aCount,
           aOffset,
           ResultName(rv).get(),
           *aBytes);
      return rv;
    }
    if (read == 0) {
      ILOG("ReadAt(%" PRIu32 "@%" PRId64
           ") uncached read 0 before last block -> OK, %" PRIu32,
           aCount,
           aOffset,
           *aBytes);
      return NS_OK;
    }
    *aBytes += read;
    if (read < toRead) {
      // Could not read everything we wanted, we're done.
      ILOG("ReadAt(%" PRIu32 "@%" PRId64
           ") uncached read before last block, incomplete -> OK, %" PRIu32,
           aCount,
           aOffset,
           *aBytes);
      return NS_OK;
    }
    ILOG("ReadAt(%" PRIu32 "@%" PRId64 ") read %" PRIu32
         " before last block, remaining: %" PRIu32 "@%" PRId64 "...",
         aCount,
         aOffset,
         read,
         aCount - read,
         aOffset + read);
    aOffset += read;
    aBuffer += read;
    aCount -= read;
  }

  // We should just have reached the start of the last block.
  MOZ_ASSERT(aOffset == lastBlockOffset);
  MOZ_ASSERT(aCount <= mCacheBlockSize);
  // Make sure to invalidate the cache first.
  mCachedBytes = 0;
  return CacheOrReadAt(aOffset, aBuffer, aCount, aBytes);
}

nsresult
MediaResourceIndex::CacheOrReadAt(int64_t aOffset,
                                  char* aBuffer,
                                  uint32_t aCount,
                                  uint32_t* aBytes)
{
  // We should be here because there is more data to read.
  MOZ_ASSERT(aCount > 0);
  // We should be in the last block, so we shouldn't try to read past it.
  MOZ_ASSERT(IndexInCache(aOffset) + aCount <= mCacheBlockSize);

  const int64_t length = GetLength();
  // If length is unknown (-1), look at resource-cached data.
  // If length is known and equal or greater than requested, also look at
  // resource-cached data.
  // Otherwise, if length is known but same, or less than(!?), requested, don't
  // attempt to access resource-cached data, as we're not expecting it to ever
  // be greater than the length.
  if (length < 0 || length >= aOffset + aCount) {
    // Is there cached data covering at least the requested range?
    const int64_t cachedDataEnd = mResource->GetCachedDataEnd(aOffset);
    if (cachedDataEnd >= aOffset + aCount) {
      // Try to read as much resource-cached data as can fill our local cache.
      // Assume we can read as much as is cached without blocking.
      const uint32_t cacheIndex = IndexInCache(aOffset);
      const uint32_t toRead =
        uint32_t(std::min(cachedDataEnd - aOffset,
                          int64_t(mCacheBlockSize - cacheIndex)));
      MOZ_ASSERT(toRead >= aCount);
      uint32_t read = 0;
      // We would like `toRead` if possible, but ok with at least `aCount`.
      nsresult rv = UncachedRangedReadAt(
        aOffset, &mCachedBlock[cacheIndex], aCount, toRead - aCount, &read);
      if (NS_SUCCEEDED(rv)) {
        if (read == 0) {
          ILOG("ReadAt(%" PRIu32 "@%" PRId64 ") - UncachedRangedReadAt(%" PRIu32
               "..%" PRIu32 "@%" PRId64
               ") to top-up succeeded but read nothing -> OK anyway",
               aCount,
               aOffset,
               aCount,
               toRead,
               aOffset);
          // Couldn't actually read anything, but didn't error out, so count
          // that as success.
          return NS_OK;
        }
        if (mCachedOffset + mCachedBytes == aOffset) {
          // We were topping-up the cache, just update its size.
          ILOG("ReadAt(%" PRIu32 "@%" PRId64 ") - UncachedRangedReadAt(%" PRIu32
               "..%" PRIu32 "@%" PRId64 ") to top-up succeeded to read %" PRIu32
               "...",
               aCount,
               aOffset,
               aCount,
               toRead,
               aOffset,
               read);
          mCachedBytes += read;
        } else {
          // We were filling the cache from scratch, save new cache information.
          ILOG("ReadAt(%" PRIu32 "@%" PRId64 ") - UncachedRangedReadAt(%" PRIu32
               "..%" PRIu32 "@%" PRId64
               ") to fill cache succeeded to read %" PRIu32 "...",
               aCount,
               aOffset,
               aCount,
               toRead,
               aOffset,
               read);
          mCachedOffset = aOffset;
          mCachedBytes = read;
        }
        // Copy relevant part into output.
        uint32_t toCopy = std::min(aCount, read);
        memcpy(aBuffer, &mCachedBlock[cacheIndex], toCopy);
        *aBytes += toCopy;
        ILOG("ReadAt(%" PRIu32 "@%" PRId64 ") - copied %" PRIu32 "@%" PRId64
             " -> OK, %" PRIu32,
             aCount,
             aOffset,
             toCopy,
             aOffset,
             *aBytes);
        // We may not have read all that was requested, but we got everything
        // we could get, so we're done.
        return NS_OK;
      }
      ILOG("ReadAt(%" PRIu32 "@%" PRId64 ") - UncachedRangedReadAt(%" PRIu32
           "..%" PRIu32 "@%" PRId64
           ") failed: %s, will fallback to blocking read...",
           aCount,
           aOffset,
           aCount,
           toRead,
           aOffset,
           ResultName(rv).get());
      // Failure during reading. Note that this may be due to the cache
      // changing between `GetCachedDataEnd` and `ReadAt`, so it's not
      // totally unexpected, just hopefully rare; but we do need to handle it.

      // Invalidate part of cache that may have been partially overridden.
      if (mCachedOffset + mCachedBytes == aOffset) {
        // We were topping-up the cache, just keep the old untouched data.
        // (i.e., nothing to do here.)
      } else {
        // We were filling the cache from scratch, invalidate cache.
        mCachedBytes = 0;
      }
    } else {
      ILOG("ReadAt(%" PRIu32 "@%" PRId64
           ") - no cached data, will fallback to blocking read...",
           aCount,
           aOffset);
    }
  } else {
    ILOG("ReadAt(%" PRIu32 "@%" PRId64 ") - length is %" PRId64
         " (%s), will fallback to blocking read as the caller requested...",
         aCount,
         aOffset,
         length,
         length < 0 ? "unknown" : "too short!");
  }
  uint32_t read = 0;
  nsresult rv = UncachedReadAt(aOffset, aBuffer, aCount, &read);
  if (NS_SUCCEEDED(rv)) {
    *aBytes += read;
    ILOG("ReadAt(%" PRIu32 "@%" PRId64 ") - fallback uncached read got %" PRIu32
         " bytes -> %s, %" PRIu32,
         aCount,
         aOffset,
         read,
         ResultName(rv).get(),
         *aBytes);
  } else {
    ILOG("ReadAt(%" PRIu32 "@%" PRId64
         ") - fallback uncached read failed -> %s, %" PRIu32,
         aCount,
         aOffset,
         ResultName(rv).get(),
         *aBytes);
  }
  return rv;
}

nsresult
MediaResourceIndex::UncachedReadAt(int64_t aOffset,
                                   char* aBuffer,
                                   uint32_t aCount,
                                   uint32_t* aBytes) const
{
  *aBytes = 0;
  if (aOffset < 0) {
    return NS_ERROR_ILLEGAL_VALUE;
  }
  if (aCount != 0) {
    for (;;) {
      uint32_t bytesRead = 0;
      nsresult rv = mResource->ReadAt(aOffset, aBuffer, aCount, &bytesRead);
      if (NS_FAILED(rv)) {
        return rv;
      }
      if (bytesRead == 0) {
        break;
      }
      *aBytes += bytesRead;
      aCount -= bytesRead;
      if (aCount == 0) {
        break;
      }
      aOffset += bytesRead;
      if (aOffset < 0) {
        // Very unlikely overflow.
        return NS_ERROR_FAILURE;
      }
      aBuffer += bytesRead;
    }
  }
  return NS_OK;
}

nsresult
MediaResourceIndex::UncachedRangedReadAt(int64_t aOffset,
                                         char* aBuffer,
                                         uint32_t aRequestedCount,
                                         uint32_t aExtraCount,
                                         uint32_t* aBytes) const
{
  *aBytes = 0;
  uint32_t count = aRequestedCount + aExtraCount;
  if (aOffset < 0 || count < aRequestedCount) {
    return NS_ERROR_ILLEGAL_VALUE;
  }
  if (count != 0) {
    for (;;) {
      uint32_t bytesRead = 0;
      nsresult rv = mResource->ReadAt(aOffset, aBuffer, count, &bytesRead);
      if (NS_FAILED(rv)) {
        return rv;
      }
      if (bytesRead == 0) {
        break;
      }
      *aBytes += bytesRead;
      count -= bytesRead;
      if (count <= aExtraCount) {
        // We have read at least aRequestedCount, don't loop anymore.
        break;
      }
      aOffset += bytesRead;
      if (aOffset < 0) {
        // Very unlikely overflow.
        return NS_ERROR_FAILURE;
      }
      aBuffer += bytesRead;
    }
  }
  return NS_OK;
}

nsresult
MediaResourceIndex::Seek(int32_t aWhence, int64_t aOffset)
{
  switch (aWhence) {
    case SEEK_SET:
      break;
    case SEEK_CUR:
      aOffset += mOffset;
      break;
    case SEEK_END:
    {
      int64_t length = mResource->GetLength();
      if (length == -1 || length - aOffset < 0) {
        return NS_ERROR_FAILURE;
      }
      aOffset = mResource->GetLength() - aOffset;
    }
      break;
    default:
      return NS_ERROR_FAILURE;
  }

  if (aOffset < 0) {
    return NS_ERROR_ILLEGAL_VALUE;
  }
  mOffset = aOffset;

  return NS_OK;
}

} // namespace mozilla

// avoid redefined macro in unified build
#undef ILOG

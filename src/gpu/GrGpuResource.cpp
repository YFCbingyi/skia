/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrGpuResource.h"
#include "GrContext.h"
#include "GrContextPriv.h"
#include "GrResourceCache.h"
#include "GrGpu.h"
#include "GrGpuResourcePriv.h"
#include "SkTraceMemoryDump.h"
#include <atomic>

static inline GrResourceCache* get_resource_cache(GrGpu* gpu) {
    SkASSERT(gpu);
    SkASSERT(gpu->getContext());
    SkASSERT(gpu->getContext()->contextPriv().getResourceCache());
    return gpu->getContext()->contextPriv().getResourceCache();
}

GrGpuResource::GrGpuResource(GrGpu* gpu) : fGpu(gpu), fUniqueID(CreateUniqueID()) {
    SkDEBUGCODE(fCacheArrayIndex = -1);
}

void GrGpuResource::registerWithCache(SkBudgeted budgeted) {
    SkASSERT(fBudgetedType == GrBudgetedType::kUnbudgetedCacheable);
    fBudgetedType = budgeted == SkBudgeted::kYes ? GrBudgetedType::kBudgeted
                                                 : GrBudgetedType::kUnbudgetedCacheable;
    this->computeScratchKey(&fScratchKey);
    get_resource_cache(fGpu)->resourceAccess().insertResource(this);
}

void GrGpuResource::registerWithCacheWrapped(GrWrapCacheable wrapType) {
    SkASSERT(fBudgetedType == GrBudgetedType::kUnbudgetedCacheable);
    // Resources referencing wrapped objects are never budgeted. They may be cached or uncached.
    fBudgetedType = wrapType == GrWrapCacheable::kNo ? GrBudgetedType::kUnbudgetedUncacheable
                                                     : GrBudgetedType::kUnbudgetedCacheable;
    fRefsWrappedObjects = true;
    get_resource_cache(fGpu)->resourceAccess().insertResource(this);
}

GrGpuResource::~GrGpuResource() {
    // The cache should have released or destroyed this resource.
    SkASSERT(this->wasDestroyed());
}

void GrGpuResource::release() {
    SkASSERT(fGpu);
    this->onRelease();
    get_resource_cache(fGpu)->resourceAccess().removeResource(this);
    fGpu = nullptr;
    fGpuMemorySize = 0;
}

void GrGpuResource::abandon() {
    if (this->wasDestroyed()) {
        return;
    }
    SkASSERT(fGpu);
    this->onAbandon();
    get_resource_cache(fGpu)->resourceAccess().removeResource(this);
    fGpu = nullptr;
    fGpuMemorySize = 0;
}

void GrGpuResource::dumpMemoryStatistics(SkTraceMemoryDump* traceMemoryDump) const {
    if (this->fRefsWrappedObjects && !traceMemoryDump->shouldDumpWrappedObjects()) {
        return;
    }

    this->dumpMemoryStatisticsPriv(traceMemoryDump, this->getResourceName(),
                                   this->getResourceType(), this->gpuMemorySize());
}

void GrGpuResource::dumpMemoryStatisticsPriv(SkTraceMemoryDump* traceMemoryDump,
                                             const SkString& resourceName,
                                             const char* type, size_t size) const {
    const char* tag = "Scratch";
    if (fUniqueKey.isValid()) {
        tag = (fUniqueKey.tag() != nullptr) ? fUniqueKey.tag() : "Other";
    }

    traceMemoryDump->dumpNumericValue(resourceName.c_str(), "size", "bytes", size);
    traceMemoryDump->dumpStringValue(resourceName.c_str(), "type", type);
    traceMemoryDump->dumpStringValue(resourceName.c_str(), "category", tag);
    if (this->isPurgeable()) {
        traceMemoryDump->dumpNumericValue(resourceName.c_str(), "purgeable_size", "bytes", size);
    }

    this->setMemoryBacking(traceMemoryDump, resourceName);
}

SkString GrGpuResource::getResourceName() const {
    // Dump resource as "skia/gpu_resources/resource_#".
    SkString resourceName("skia/gpu_resources/resource_");
    resourceName.appendU32(this->uniqueID().asUInt());
    return resourceName;
}

const GrContext* GrGpuResource::getContext() const {
    if (fGpu) {
        return fGpu->getContext();
    } else {
        return nullptr;
    }
}

GrContext* GrGpuResource::getContext() {
    if (fGpu) {
        return fGpu->getContext();
    } else {
        return nullptr;
    }
}

void GrGpuResource::removeUniqueKey() {
    if (this->wasDestroyed()) {
        return;
    }
    SkASSERT(fUniqueKey.isValid());
    get_resource_cache(fGpu)->resourceAccess().removeUniqueKey(this);
}

void GrGpuResource::setUniqueKey(const GrUniqueKey& key) {
    SkASSERT(this->internalHasRef());
    SkASSERT(key.isValid());

    // Uncached resources can never have a unique key, unless they're wrapped resources. Wrapped
    // resources are a special case: the unique keys give us a weak ref so that we can reuse the
    // same resource (rather than re-wrapping). When a wrapped resource is no longer referenced,
    // it will always be released - it is never converted to a scratch resource.
    if (this->resourcePriv().budgetedType() != GrBudgetedType::kBudgeted &&
        !this->fRefsWrappedObjects) {
        return;
    }

    if (this->wasDestroyed()) {
        return;
    }

    get_resource_cache(fGpu)->resourceAccess().changeUniqueKey(this, key);
}

void GrGpuResource::notifyAllCntsAreZero(CntType lastCntTypeToReachZero) const {
    if (this->wasDestroyed()) {
        // We've already been removed from the cache. Goodbye cruel world!
        delete this;
        return;
    }

    // We should have already handled this fully in notifyRefCntIsZero().
    SkASSERT(kRef_CntType != lastCntTypeToReachZero);

    GrGpuResource* mutableThis = const_cast<GrGpuResource*>(this);
    static const uint32_t kFlag =
        GrResourceCache::ResourceAccess::kAllCntsReachedZero_RefNotificationFlag;
    get_resource_cache(fGpu)->resourceAccess().notifyCntReachedZero(mutableThis, kFlag);
}

bool GrGpuResource::notifyRefCountIsZero() const {
    if (this->wasDestroyed()) {
        // handle this in notifyAllCntsAreZero().
        return true;
    }

    GrGpuResource* mutableThis = const_cast<GrGpuResource*>(this);
    uint32_t flags = GrResourceCache::ResourceAccess::kRefCntReachedZero_RefNotificationFlag;
    if (!this->internalHasPendingIO()) {
        flags |= GrResourceCache::ResourceAccess::kAllCntsReachedZero_RefNotificationFlag;
    }
    get_resource_cache(fGpu)->resourceAccess().notifyCntReachedZero(mutableThis, flags);

    // There is no need to call our notifyAllCntsAreZero function at this point since we already
    // told the cache about the state of cnts.
    return false;
}

void GrGpuResource::removeScratchKey() {
    if (!this->wasDestroyed() && fScratchKey.isValid()) {
        get_resource_cache(fGpu)->resourceAccess().willRemoveScratchKey(this);
        fScratchKey.reset();
    }
}

void GrGpuResource::makeBudgeted() {
    // We should never make a wrapped resource budgeted.
    SkASSERT(!fRefsWrappedObjects);
    // Only wrapped resources can be in the kUnbudgetedUncacheable state.
    SkASSERT(fBudgetedType != GrBudgetedType::kUnbudgetedUncacheable);
    if (!this->wasDestroyed() && fBudgetedType == GrBudgetedType::kUnbudgetedCacheable) {
        // Currently resources referencing wrapped objects are not budgeted.
        fBudgetedType = GrBudgetedType::kBudgeted;
        get_resource_cache(fGpu)->resourceAccess().didChangeBudgetStatus(this);
    }
}

void GrGpuResource::makeUnbudgeted() {
    if (!this->wasDestroyed() && fBudgetedType == GrBudgetedType::kBudgeted &&
        !fUniqueKey.isValid()) {
        fBudgetedType = GrBudgetedType::kUnbudgetedCacheable;
        get_resource_cache(fGpu)->resourceAccess().didChangeBudgetStatus(this);
    }
}

uint32_t GrGpuResource::CreateUniqueID() {
    static std::atomic<uint32_t> nextID{1};
    uint32_t id;
    do {
        id = nextID++;
    } while (id == SK_InvalidUniqueID);
    return id;
}

/*
 * Copyright 2019 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrCopyRenderTask_DEFINED
#define GrCopyRenderTask_DEFINED

#include "src/gpu/GrRenderTask.h"

class GrCopyRenderTask final : public GrRenderTask {
public:
    static sk_sp<GrRenderTask> Make(GrDrawingManager*,
                                    GrSurfaceProxyView srcView,
                                    const SkIRect& srcRect,
                                    GrSurfaceProxyView dstView,
                                    const SkIPoint& dstPoint,
                                    const GrCaps*);

private:
    GrCopyRenderTask(GrDrawingManager*,
                     GrSurfaceProxyView srcView,
                     const SkIRect& srcRect,
                     GrSurfaceProxyView dstView,
                     const SkIPoint& dstPoint);

    bool onIsUsed(GrSurfaceProxy* proxy) const override {
        return proxy == fSrcView.proxy();
    }
    // If instantiation failed, at flush time we simply will skip doing the copy.
    void handleInternalAllocationFailure() override {}
    void gatherProxyIntervals(GrResourceAllocator*) const override;
    ExpectedOutcome onMakeClosed(const GrCaps&, SkIRect* targetUpdateBounds) override {
        targetUpdateBounds->setXYWH(fDstPoint.x(), fDstPoint.y(), fSrcRect.width(),
                                    fSrcRect.height());
        return ExpectedOutcome::kTargetDirty;
    }
    bool onExecute(GrOpFlushState*) override;

#if GR_TEST_UTILS
    const char* name() const final { return "Copy"; }
#endif
#ifdef SK_DEBUG
    void visitProxies_debugOnly(const GrOp::VisitProxyFunc& fn) const override {
        fn(fSrcView.proxy(), GrMipmapped::kNo);
    }
#endif

    GrSurfaceProxyView fSrcView;
    SkIRect fSrcRect;
    SkIPoint fDstPoint;
};

#endif


/*
 * Copyright (C) 2011-2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */



#include "rsCpuCore.h"

#include "rsCpuScript.h"
//#include "rsdRuntime.h"
//#include "rsdAllocation.h"
//#include "rsCpuIntrinsics.h"


#include "utils/Vector.h"
#include "utils/Timers.h"
#include "utils/StopWatch.h"


#include <bcc/BCCContext.h>
#include <bcc/Renderscript/RSCompilerDriver.h>
#include <bcc/Renderscript/RSExecutable.h>
#include <bcc/Renderscript/RSInfo.h>

namespace android {
namespace renderscript {



RsdCpuScriptImpl::RsdCpuScriptImpl(RsdCpuReferenceImpl *ctx, const Script *s) {
    mCtx = ctx;
    mScript = s;

    mRoot = NULL;
    mRootExpand = NULL;
    mInit = NULL;
    mFreeChildren = NULL;

    mCompilerContext = NULL;
    mCompilerDriver = NULL;
    mExecutable = NULL;

    mBoundAllocs = NULL;
    mIntrinsicData = NULL;
    mIsThreadable = true;
}


bool RsdCpuScriptImpl::init(char const *resName, char const *cacheDir,
                            uint8_t const *bitcode, size_t bitcodeSize,
                            uint32_t flags) {
    //ALOGE("rsdScriptCreate %p %p %p %p %i %i %p", rsc, resName, cacheDir, bitcode, bitcodeSize, flags, lookupFunc);
    //ALOGE("rsdScriptInit %p %p", rsc, script);

    mCtx->lockMutex();

    bcc::RSExecutable *exec;
    const bcc::RSInfo *info;

    mCompilerContext = NULL;
    mCompilerDriver = NULL;
    mExecutable = NULL;

    mCompilerContext = new bcc::BCCContext();
    if (mCompilerContext == NULL) {
        ALOGE("bcc: FAILS to create compiler context (out of memory)");
        mCtx->unlockMutex();
        return false;
    }

    mCompilerDriver = new bcc::RSCompilerDriver();
    if (mCompilerDriver == NULL) {
        ALOGE("bcc: FAILS to create compiler driver (out of memory)");
        mCtx->unlockMutex();
        return false;
    }

    mCompilerDriver->setRSRuntimeLookupFunction(lookupRuntimeStub);
    mCompilerDriver->setRSRuntimeLookupContext(this);

    exec = mCompilerDriver->build(*mCompilerContext, cacheDir, resName,
                                  (const char *)bitcode, bitcodeSize);

    if (exec == NULL) {
        ALOGE("bcc: FAILS to prepare executable for '%s'", resName);
        mCtx->unlockMutex();
        return false;
    }

    mExecutable = exec;

    exec->setThreadable(mIsThreadable);
    if (!exec->syncInfo()) {
        ALOGW("bcc: FAILS to synchronize the RS info file to the disk");
    }

    mRoot = reinterpret_cast<int (*)()>(exec->getSymbolAddress("root"));
    mRootExpand =
        reinterpret_cast<int (*)()>(exec->getSymbolAddress("root.expand"));
    mInit = reinterpret_cast<void (*)()>(exec->getSymbolAddress("init"));
    mFreeChildren =
        reinterpret_cast<void (*)()>(exec->getSymbolAddress(".rs.dtor"));


    info = &mExecutable->getInfo();
    if (info->getExportVarNames().size()) {
        mBoundAllocs = new Allocation *[info->getExportVarNames().size()];
        memset(mBoundAllocs, 0, sizeof(void *) * info->getExportVarNames().size());
    }

    mCtx->unlockMutex();
    return true;
}

void RsdCpuScriptImpl::populateScript(Script *script) {
    const bcc::RSInfo *info = &mExecutable->getInfo();

    // Copy info over to runtime
    script->mHal.info.exportedFunctionCount = info->getExportFuncNames().size();
    script->mHal.info.exportedVariableCount = info->getExportVarNames().size();
    script->mHal.info.exportedPragmaCount = info->getPragmas().size();
    script->mHal.info.exportedPragmaKeyList =
        const_cast<const char**>(mExecutable->getPragmaKeys().array());
    script->mHal.info.exportedPragmaValueList =
        const_cast<const char**>(mExecutable->getPragmaValues().array());

    if (mRootExpand) {
        script->mHal.info.root = mRootExpand;
    } else {
        script->mHal.info.root = mRoot;
    }
}

/*
bool rsdInitIntrinsic(const Context *rsc, Script *s, RsScriptIntrinsicID iid, Element *e) {
    pthread_mutex_lock(&rsdgInitMutex);

    DrvScript *drv = (DrvScript *)calloc(1, sizeof(DrvScript));
    if (drv == NULL) {
        goto error;
    }
    s->mHal.drv = drv;
    drv->mIntrinsicID = iid;
    drv->mIntrinsicData = rsdIntrinsic_Init(rsc, s, iid, &drv->mIntrinsicFuncs);
    s->mHal.info.isThreadable = true;

    pthread_mutex_unlock(&rsdgInitMutex);
    return true;

error:
    pthread_mutex_unlock(&rsdgInitMutex);
    return false;
}
*/

typedef void (*rs_t)(const void *, void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t);

void RsdCpuScriptImpl::forEachMtlsSetup(const Allocation * ain, Allocation * aout,
                                        const void * usr, uint32_t usrLen,
                                        const RsScriptCall *sc,
                                        MTLaunchStruct *mtls) {

    memset(mtls, 0, sizeof(MTLaunchStruct));

    if (ain) {
        mtls->fep.dimX = ain->getType()->getDimX();
        mtls->fep.dimY = ain->getType()->getDimY();
        mtls->fep.dimZ = ain->getType()->getDimZ();
        //mtls->dimArray = ain->getType()->getDimArray();
    } else if (aout) {
        mtls->fep.dimX = aout->getType()->getDimX();
        mtls->fep.dimY = aout->getType()->getDimY();
        mtls->fep.dimZ = aout->getType()->getDimZ();
        //mtls->dimArray = aout->getType()->getDimArray();
    } else {
        mCtx->getContext()->setError(RS_ERROR_BAD_SCRIPT, "rsForEach called with null allocations");
        return;
    }

    if (!sc || (sc->xEnd == 0)) {
        mtls->xEnd = mtls->fep.dimX;
    } else {
        rsAssert(sc->xStart < mtls->fep.dimX);
        rsAssert(sc->xEnd <= mtls->fep.dimX);
        rsAssert(sc->xStart < sc->xEnd);
        mtls->xStart = rsMin(mtls->fep.dimX, sc->xStart);
        mtls->xEnd = rsMin(mtls->fep.dimX, sc->xEnd);
        if (mtls->xStart >= mtls->xEnd) return;
    }

    if (!sc || (sc->yEnd == 0)) {
        mtls->yEnd = mtls->fep.dimY;
    } else {
        rsAssert(sc->yStart < mtls->fep.dimY);
        rsAssert(sc->yEnd <= mtls->fep.dimY);
        rsAssert(sc->yStart < sc->yEnd);
        mtls->yStart = rsMin(mtls->fep.dimY, sc->yStart);
        mtls->yEnd = rsMin(mtls->fep.dimY, sc->yEnd);
        if (mtls->yStart >= mtls->yEnd) return;
    }

    mtls->xEnd = rsMax((uint32_t)1, mtls->xEnd);
    mtls->yEnd = rsMax((uint32_t)1, mtls->yEnd);
    mtls->zEnd = rsMax((uint32_t)1, mtls->zEnd);
    mtls->arrayEnd = rsMax((uint32_t)1, mtls->arrayEnd);

    rsAssert(!ain || (ain->getType()->getDimZ() == 0));

    mtls->rsc = mCtx;
    mtls->ain = ain;
    mtls->aout = aout;
    mtls->fep.usr = usr;
    mtls->fep.usrLen = usrLen;
    mtls->mSliceSize = 1;
    mtls->mSliceNum = 0;

    mtls->fep.ptrIn = NULL;
    mtls->fep.eStrideIn = 0;
    mtls->isThreadable = mIsThreadable;

    if (ain) {
        mtls->fep.ptrIn = (const uint8_t *)ain->mHal.drvState.lod[0].mallocPtr;
        mtls->fep.eStrideIn = ain->getType()->getElementSizeBytes();
        mtls->fep.yStrideIn = ain->mHal.drvState.lod[0].stride;
    }

    mtls->fep.ptrOut = NULL;
    mtls->fep.eStrideOut = 0;
    if (aout) {
        mtls->fep.ptrOut = (uint8_t *)aout->mHal.drvState.lod[0].mallocPtr;
        mtls->fep.eStrideOut = aout->getType()->getElementSizeBytes();
        mtls->fep.yStrideOut = aout->mHal.drvState.lod[0].stride;
    }
}


void RsdCpuScriptImpl::invokeForEach(uint32_t slot,
                                     const Allocation * ain,
                                     Allocation * aout,
                                     const void * usr,
                                     uint32_t usrLen,
                                     const RsScriptCall *sc) {

    MTLaunchStruct mtls;
    forEachMtlsSetup(ain, aout, usr, usrLen, sc, &mtls);
    forEachKernelSetup(slot, &mtls);

    RsdCpuScriptImpl * oldTLS = mCtx->setTLS(this);
    mCtx->launchThreads(ain, aout, sc, &mtls);
    mCtx->setTLS(oldTLS);
}

void RsdCpuScriptImpl::forEachKernelSetup(uint32_t slot, MTLaunchStruct *mtls) {

    mtls->script = this;
    mtls->fep.slot = slot;

    rsAssert(slot < mExecutable->getExportForeachFuncAddrs().size());
    mtls->kernel = reinterpret_cast<ForEachFunc_t>(
                      mExecutable->getExportForeachFuncAddrs()[slot]);
    rsAssert(mtls->kernel != NULL);
    mtls->sig = mExecutable->getInfo().getExportForeachFuncs()[slot].second;
}

int RsdCpuScriptImpl::invokeRoot() {
    RsdCpuScriptImpl * oldTLS = mCtx->setTLS(this);
    int ret = mRoot();
    mCtx->setTLS(oldTLS);
    return ret;
}

void RsdCpuScriptImpl::invokeInit() {
    if (mInit) {
        mInit();
    }
}

void RsdCpuScriptImpl::invokeFreeChildren() {
    if (mFreeChildren) {
        mFreeChildren();
    }
}

void RsdCpuScriptImpl::invokeFunction(uint32_t slot, const void *params,
                                      size_t paramLength) {
    //ALOGE("invoke %p %p %i %p %i", dc, script, slot, params, paramLength);

    RsdCpuScriptImpl * oldTLS = mCtx->setTLS(this);
    reinterpret_cast<void (*)(const void *, uint32_t)>(
        mExecutable->getExportFuncAddrs()[slot])(params, paramLength);
    mCtx->setTLS(oldTLS);
}

void RsdCpuScriptImpl::setGlobalVar(uint32_t slot, const void *data, size_t dataLength) {
    //rsAssert(!script->mFieldIsObject[slot]);
    //ALOGE("setGlobalVar %p %p %i %p %i", dc, script, slot, data, dataLength);

    //if (mIntrinsicID) {
        //mIntrinsicFuncs.setVar(dc, script, drv->mIntrinsicData, slot, data, dataLength);
        //return;
    //}

    int32_t *destPtr = reinterpret_cast<int32_t *>(
                          mExecutable->getExportVarAddrs()[slot]);
    if (!destPtr) {
        //ALOGV("Calling setVar on slot = %i which is null", slot);
        return;
    }

    memcpy(destPtr, data, dataLength);
}

void RsdCpuScriptImpl::setGlobalVarWithElemDims(uint32_t slot, const void *data, size_t dataLength,
                                                const Element *elem,
                                                const size_t *dims, size_t dimLength) {

    int32_t *destPtr = reinterpret_cast<int32_t *>(
        mExecutable->getExportVarAddrs()[slot]);
    if (!destPtr) {
        //ALOGV("Calling setVar on slot = %i which is null", slot);
        return;
    }

    // We want to look at dimension in terms of integer components,
    // but dimLength is given in terms of bytes.
    dimLength /= sizeof(int);

    // Only a single dimension is currently supported.
    rsAssert(dimLength == 1);
    if (dimLength == 1) {
        // First do the increment loop.
        size_t stride = elem->getSizeBytes();
        const char *cVal = reinterpret_cast<const char *>(data);
        for (size_t i = 0; i < dims[0]; i++) {
            elem->incRefs(cVal);
            cVal += stride;
        }

        // Decrement loop comes after (to prevent race conditions).
        char *oldVal = reinterpret_cast<char *>(destPtr);
        for (size_t i = 0; i < dims[0]; i++) {
            elem->decRefs(oldVal);
            oldVal += stride;
        }
    }

    memcpy(destPtr, data, dataLength);
}

void RsdCpuScriptImpl::setGlobalBind(uint32_t slot, Allocation *data) {

    //rsAssert(!script->mFieldIsObject[slot]);
    //ALOGE("setGlobalBind %p %p %i %p", dc, script, slot, data);

    int32_t *destPtr = reinterpret_cast<int32_t *>(
                          mExecutable->getExportVarAddrs()[slot]);
    if (!destPtr) {
        //ALOGV("Calling setVar on slot = %i which is null", slot);
        return;
    }

    void *ptr = NULL;
    mBoundAllocs[slot] = data;
    if(data) {
        ptr = data->mHal.drvState.lod[0].mallocPtr;
    }
    memcpy(destPtr, &ptr, sizeof(void *));
}

void RsdCpuScriptImpl::setGlobalObj(uint32_t slot, ObjectBase *data) {

    //rsAssert(script->mFieldIsObject[slot]);
    //ALOGE("setGlobalObj %p %p %i %p", dc, script, slot, data);

    //if (mIntrinsicID) {
        //mIntrinsicFuncs.setVarObj(dc, script, drv->mIntrinsicData, slot, alloc);
        //return;
    //}

    int32_t *destPtr = reinterpret_cast<int32_t *>(
                          mExecutable->getExportVarAddrs()[slot]);
    if (!destPtr) {
        //ALOGV("Calling setVar on slot = %i which is null", slot);
        return;
    }

    rsrSetObject(mCtx->getContext(), (ObjectBase **)destPtr, data);
}

RsdCpuScriptImpl::~RsdCpuScriptImpl() {

    if (mExecutable) {
        Vector<void *>::const_iterator var_addr_iter =
            mExecutable->getExportVarAddrs().begin();
        Vector<void *>::const_iterator var_addr_end =
            mExecutable->getExportVarAddrs().end();

        bcc::RSInfo::ObjectSlotListTy::const_iterator is_object_iter =
            mExecutable->getInfo().getObjectSlots().begin();
        bcc::RSInfo::ObjectSlotListTy::const_iterator is_object_end =
            mExecutable->getInfo().getObjectSlots().end();

        while ((var_addr_iter != var_addr_end) &&
               (is_object_iter != is_object_end)) {
            // The field address can be NULL if the script-side has optimized
            // the corresponding global variable away.
            ObjectBase **obj_addr =
                reinterpret_cast<ObjectBase **>(*var_addr_iter);
            if (*is_object_iter) {
                if (*var_addr_iter != NULL) {
                    rsrClearObject(mCtx->getContext(), obj_addr);
                }
            }
            var_addr_iter++;
            is_object_iter++;
        }
    }

    if (mCompilerContext) {
        delete mCompilerContext;
    }
    if (mCompilerDriver) {
        delete mCompilerDriver;
    }
    if (mExecutable) {
        delete mExecutable;
    }
    if (mBoundAllocs) {
        delete[] mBoundAllocs;
    }
}

Allocation * RsdCpuScriptImpl::getAllocationForPointer(const void *ptr) const {
    if (!ptr) {
        return NULL;
    }

    for (uint32_t ct=0; ct < mScript->mHal.info.exportedVariableCount; ct++) {
        Allocation *a = mBoundAllocs[ct];
        if (!a) continue;
        if (a->mHal.drvState.lod[0].mallocPtr == ptr) {
            return a;
        }
    }
    ALOGE("rsGetAllocation, failed to find %p", ptr);
    return NULL;
}


}
}

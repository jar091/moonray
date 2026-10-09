// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

#include <moonray/rendering/pbr/light/LightSet.h>

#include <moonray/rendering/pbr/core//RayState.h>
#include <moonray/rendering/pbr/core/Util.h>
#include <moonray/rendering/pbr/light/Light.h>
#include <moonray/rendering/pbr/light/LightUtil.h>

#include <moonray/rendering/pbr/light/LightSet_ispc_stubs.h>

namespace moonray {
namespace pbr {

using namespace scene_rdl2::math;

//----------------------------------------------------------------------------

HUD_VALIDATOR(LightSet);

int
LightSet::intersect(const Vec3f &P, const Vec3f *N, const Vec3f &wi, float time,
        float maxDistance, bool includeRayTerminationLights, IntegratorSample1D &samples, int depth,
        int visibilityMask, LightIntersection &chosenIsect, int &numHits,
        const scene_rdl2::rdl2::LightSet* bsdfLobeLightSet,
        const Rdl2LightSetList& parentLobeLightSets) const
{
    chosenIsect.distance = maxDistance;

    numHits = 0;

    // Use acceleration if it would be quicker
    const LightAccelerator* acc = mAccelerator;
    if (acc && acc->useAcceleration()) {
        int idx = acc->intersect(P, N, wi, time, maxDistance, includeRayTerminationLights, visibilityMask,
            samples, depth, chosenIsect, numHits, mAcceleratorLightIdMap, bsdfLobeLightSet, parentLobeLightSets);
        return idx >= 0 ? mAcceleratorLightIdMap[idx] : idx;
    }

    int chosenLightIdx = -1;

    // Test lights for intersection
    for (int l = 0; l < mLightCount; l++) {

        LightIntersection currentIsect;
        const Light* light = mLights[l];

        if (!isLightActive(light, visibilityMask, includeRayTerminationLights, 
                           bsdfLobeLightSet, &parentLobeLightSets)) {
            continue;
        }

        if (light->intersect(P, N, wi, time, maxDistance, currentIsect)) {
            numHits++;
            if (chooseThisLight(samples, depth, numHits)) {
                chosenLightIdx = l;
                chosenIsect = currentIsect;
            }
        }
    }

    MNRY_ASSERT(numHits==0 || chosenLightIdx >= 0);

    return chosenLightIdx;
}

void
LightSet::intersectAndEval(mcrt_common::ThreadLocalState *tls, const Vec3f &P, const Vec3f *N,
        const Vec3f &wi, const LightFilterRandomValues& filterR, float time, const PathVertex *pv,
        bool fromCamera, bool includeRayTerminationLights, IntegratorSample1D &samples, int depth,
        int visibilityMask, LightContribution &lCo, float rayDirFootprint,
        const scene_rdl2::rdl2::LightSet* bsdfLobeLightSet,
        const Rdl2LightSetList& parentLobeLightSets) const
{
    // Initialize contribution
    lCo.isInvalid = true;
    lCo.light = nullptr;
    lCo.distance = sMaxValue;
    lCo.Li = sBlack;
    lCo.pdf = 0.0f;

    // Compute intersection with a random light
    LightIntersection isect;
    int numLightsHit = 0;
    int lightIdx = intersect(P, N, wi, time, sMaxValue, includeRayTerminationLights,
        samples, depth, visibilityMask, isect, numLightsHit, bsdfLobeLightSet, parentLobeLightSets);

    const Light* light = nullptr;
    const LightFilterList* lightFilterList = nullptr;
    if (lightIdx >= 0) {
        light = getLight(lightIdx);
        lightFilterList = getLightFilterList(lightIdx);
    }

    // Early return if we didn't hit a light
    if (numLightsHit == 0) {
        return;
    }

    lCo.light = light;
    lCo.distance = isect.distance;

    // Evaluate the intersected light Li and pdf
    lCo.Li = light->eval(tls, wi, P, filterR, time, isect, fromCamera, lightFilterList, pv, rayDirFootprint, nullptr, &lCo.pdf);
    lCo.isInvalid = isSampleInvalid(lCo.Li, lCo.pdf);

    // Radiance has to be scaled by number of lights hit because we're only sampling one
    lCo.Li *= (float)numLightsHit;
}

bool
CPP_LobeLightSetContainsLight(intptr_t lobeLightSet,
                              intptr_t rdlLight)
{
    const scene_rdl2::rdl2::LightSet* lobeLightSetPtr =
        reinterpret_cast<const scene_rdl2::rdl2::LightSet*>(lobeLightSet);
    const scene_rdl2::rdl2::Light* rdlLightPtr =
        reinterpret_cast<const scene_rdl2::rdl2::Light*>(rdlLight);

    return lobeLightSetPtr->contains(rdlLightPtr);
}

//----------------------------------------------------------------------------

} // namespace pbr
} // namespace moonray


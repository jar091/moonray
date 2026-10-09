// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

#include <moonray/rendering/pbr/camera/DicingCamera.h>

#include <moonray/rendering/pbr/camera/PerspectiveCamera.h>
#include <moonray/rendering/pbr/core/Util.h>

#include <scene_rdl2/scene/rdl2/Camera.h>
#include <scene_rdl2/scene/rdl2/Node.h>

namespace moonray {
namespace pbr {

bool
computeDicingCameraFrustums(const scene_rdl2::rdl2::Camera* dicingRdlCamera,
                            const scene_rdl2::math::Mat4d& mainWorld2Render,
                            std::vector<mcrt_common::Frustum>* frustums,
                            scene_rdl2::math::Mat4d* dicingWorld2Render)
{
    moonray::pbr::PerspectiveCamera dicingCamera(dicingRdlCamera);
    scene_rdl2::math::Mat4d dicingRender2World = dicingRdlCamera->get(scene_rdl2::rdl2::Node::sNodeXformKey);
    *dicingWorld2Render = moonray::pbr::rtInverse(dicingRender2World);

    // The dicing camera's frustums by default assume it's looking down the negative z-axis in main camera space
    // The transform we need moves the frustums into main camera space, then to the dicing camera's position
    scene_rdl2::math::Mat4d frustumTransform(dicingRender2World * mainWorld2Render);
    dicingCamera.update(*dicingWorld2Render);

    // compute dicing camera frustums, then transform them to be in the main camera's viewing space
    frustums->push_back(mcrt_common::Frustum());
    dicingCamera.computeFrustum(&frustums->back(), 0, true);  // frustum at shutter open
    frustums->back().transformClipPlanes(frustumTransform);

    frustums->push_back(mcrt_common::Frustum());
    dicingCamera.computeFrustum(&frustums->back(), 1, true);  // frustum at shutter close
    frustums->back().transformClipPlanes(frustumTransform);
    return true;
}

} // namespace pbr
} // namespace moonray

// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <moonray/rendering/mcrt_common/Frustum.h>

#include <scene_rdl2/common/math/Mat4.h>

#include <vector>

namespace scene_rdl2 {
namespace rdl2 {
class Camera;
}
}

namespace moonray {
namespace pbr {

/// Computes the frustums (at shutter open and shutter close) and the
/// world2render transform of a dicing camera, expressed in the main camera's
/// viewing space.
///
/// This is the pbr-side implementation of `rt::DicingCameraFrustumsFn`:
/// `rendering_rt` decides *which* rdl2 camera is the dicing camera and only
/// calls back here for the camera math, so that it needs no pbr symbol of its
/// own (`rendering_pbr` links `rendering_rt`, and the reverse edge would close
/// a library cycle). `rendering_rndr` links both and installs this function on
/// the GeometryManager.
///
/// @param dicingRdlCamera  the dicing camera; must be non-null and of
///                         SceneClass "PerspectiveCamera"
/// @param mainWorld2Render the main camera's world2render transform
/// @param frustums         receives the shutter-open and shutter-close frustums
/// @param dicingWorld2Render receives the dicing camera's world2render transform
/// @return whether the frustums were computed
bool computeDicingCameraFrustums(const scene_rdl2::rdl2::Camera* dicingRdlCamera,
                                 const scene_rdl2::math::Mat4d& mainWorld2Render,
                                 std::vector<mcrt_common::Frustum>* frustums,
                                 scene_rdl2::math::Mat4d* dicingWorld2Render);

} // namespace pbr
} // namespace moonray

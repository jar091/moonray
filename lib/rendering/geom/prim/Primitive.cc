// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

#include <moonray/rendering/geom/prim/Primitive.h>

#include <moonray/rendering/geom/prim/VolumeAssignmentTable.h>

#include <moonray/rendering/shading/State.h>
#include <moonray/rendering/shading/ShadingTLState.h>
#include <scene_rdl2/scene/rdl2/VolumeShader.h>

#include <openvdb/openvdb.h>
#include <openvdb/Grid.h>
#include <openvdb/math/Transform.h>
#include <openvdb/tools/ValueTransformer.h>

namespace moonray {
namespace geom {
namespace internal {

using namespace scene_rdl2::math;
using namespace scene_rdl2::rdl2;

size_t
Primitive::getMemory() const
{
    size_t mem = sizeof(Primitive);
    mem += mBakedDensitySampler.getMemory();
    if (mBakedDensityGrid) {
        mem += mBakedDensityGrid->memUsage();
    }

    // VDBVelocity stores a vdb grid and samplers.
    if (mVdbVelocity) {
        mem += mVdbVelocity->getMemory();
    }

    return mem;
}

std::unique_ptr<EmissionDistribution>
Primitive::computeEmissionDistribution(const VolumeShader* volumeShader) const
{
    // We voxelize the geometry bounding box into approx 1000 voxels.
    // (The reason it's only approximate is that in initializing the variable 'res' below,
    // we perform a rounding operation in each axis.)
    int numVoxels = 1000;

    // Generate (up to) 2 bounding boxes and their associated distToRender matrices (for t=0 and t=1)
    BBox3f bboxes[2];
    Mat4f distToRender[2];
    size_t numTimeSteps = getMotionSamplesCount();
    for (size_t timeStep = 0; timeStep < numTimeSteps; timeStep++) {
        BBox3f bbox = computeAABBAtTimeStep(timeStep);
        bboxes[timeStep] = bbox;
        Vec3f low = bbox.lower;
        Vec3f upp = bbox.upper;
        Vec3f len = upp - low;
        float s = std::cbrt(len.x * len.y * len.z / numVoxels);

        Vec3i res(scene_rdl2::math::ceil(len.x / s),
                  scene_rdl2::math::ceil(len.y / s),
                  scene_rdl2::math::ceil(len.z / s));

        distToRender[timeStep] = Mat4f(s     ,0.0f  ,0.0f  ,0.0f,
                                       0.0f  ,s     ,0.0f  ,0.0f,
                                       0.0f  ,0.0f  ,s     ,0.0f,
                                       low.x ,low.y ,low.z ,1.0f);
    }

    Vec3f low, upp;
    if (numTimeSteps == 1) {
        low = bboxes[0].lower;
        upp = bboxes[0].upper;
        distToRender[1] = distToRender[0];
    } else {
        // Values for the bounding box at t = 0.5, used for generating the histogram for the emission distribution
        low = lerp(bboxes[0].lower, bboxes[1].lower, 0.5f);
        upp = lerp(bboxes[0].upper, bboxes[1].upper, 0.5f);
    }
    Vec3f len = upp - low;
    float sCubed = len.x * len.y * len.z / numVoxels;
    float s = std::cbrt(sCubed);
    Vec3i res(scene_rdl2::math::ceil(len.x / s),
              scene_rdl2::math::ceil(len.y / s),
              scene_rdl2::math::ceil(len.z / s));
    std::vector<float> histogram(res.x * res.y * res.z, 1.0f);

    // Bake volume shader emission into the grid.
    shading::TLState *tls = mcrt_common::getFrameUpdateTLS()->mShadingTls.get();
    float *hist = histogram.data();
    for (int k = 0; k < res.z; ++k) {
        float z = low.z + k*s;
        for (int j = 0; j < res.y; ++j) {
            float y = low.y + j*s;
            for (int i = 0; i < res.x; ++i) {
                float x = low.x + i*s;
                shading::Intersection isect;
                isect.setP(Vec3f(x,y,z));
                *hist++ = luminance(volumeShader->emission(tls,
                                                           shading::State(&isect),
                                                           Color(1.0f)));
            }
        }
    }

    // Construct and return emission distribution
    float invUnitVolume = 1.0f / sCubed;
    return std::unique_ptr<EmissionDistribution>(new DenseEmissionDistribution(res,
                                                                               distToRender,
                                                                               invUnitVolume,
                                                                               histogram));
}

void
Primitive::bakeVolumeShaderDensityMap(const VolumeShader* volumeShader,
                                      const Mat4f& primToRender,
                                      const MotionBlurParams& motionBlurParams,
                                      const VolumeAssignmentTable* volumeAssignmentTable,
                                      const int assignmentId)
{
    constexpr int sDefaultRez = 100;

    if (volumeShader->isHomogenous() || getType() != POLYMESH) {
        // If there is no map binding, or the primitive does not support volume shaders (is not a mesh),
        // do not bake velocity grid nor density grid. Store uniform color for density instead.
        // There is no need to initialize isect nor get shading tls in order to evaluate a solid color.
        shading::Intersection isect;
        shading::TLState *tls = nullptr;
        mDensityColor = volumeShader->extinct(tls, shading::State(&isect), 
                                              Color(1.f), 
                                              /*rayVolumeDepth*/ -1);
        return;
    }

    // local space bounds
    // shared primitives already return their aabb in their local space
    const BBox3f localBbox = getIsReference() ?
                             computeAABB() :
                             transformBBox(primToRender.inverse(), computeAABB());

    const Vec3f& localMin = localBbox.lower;
    const Vec3f& localMax = localBbox.upper;

    // world space bounds
    const Mat4f& primToWorld = toFloat(mRdlGeometry->get(Node::sNodeXformKey));
    const BBox3f worldSpaceBBox = transformBBox(primToWorld, localBbox);
    const Vec3f worldSpaceSize = worldSpaceBBox.upper - worldSpaceBBox.lower;
    float maxSize = max(worldSpaceSize.x,
                        max(worldSpaceSize.y,
                            worldSpaceSize.z));

    // set resolution of grid
    int rez = sDefaultRez;
    int bakeResolutionMode = volumeShader->getBakeResolutionMode();
    switch (bakeResolutionMode) {
    case 1:
    {
        // Divisions
        rez = volumeShader->getBakeDivisions();
        break;
    }
    case 2:
    {
        // Voxel size
        float voxelSize = volumeShader->getBakeVoxelSize();
        rez = scene_rdl2::math::ceil(maxSize / voxelSize);
        break;
    }
    }

    if (rez <= 0) {
        // Bad user input. Don't bake grid.
        mDensityColor = Color(1.0);
        return;
    }

    // Get volume ids. There is a separate velocity/density sampler for each volume id.
    const std::vector<int>& volumeIds = volumeAssignmentTable->getVolumeIds(assignmentId);

    // Velocity grid for motion blur.
    createVelocityGrid(rez / 2, motionBlurParams, volumeIds);

    if(!volumeShader->hasExtinctionMapBinding()) {
        // If there is no map binding for density, don't bake. Store uniform color instead.
        // There is no need to initialize isect nor get shading tls in order to evaluate a solid color.
        shading::Intersection isect;
        shading::TLState *tls = nullptr;
        mDensityColor = volumeShader->extinct(tls, shading::State(&isect), 
                                              Color(1.f), 
                                              /*rayVolumeDepth*/ -1);
        return;
    }

    // make resolution for grid
    const Vec3f upperBound = static_cast<float>(rez) * worldSpaceSize / maxSize;
    const openvdb::math::CoordBBox vdbBbox(0, 0, 0,
                                           upperBound.x,
                                           upperBound.y,
                                           upperBound.z);

    // Make transform of grid
    // 1. Scale grid to be same size as primitive (localMax - localMin)
    // 2. Translate grid to same location as primitive in local space.
    // 3. Apply local to render space transform (if primitive is not shared)
    const Mat4f l2r = getIsReference() ? Mat4f(one) : primToRender;
    openvdb::math::Transform::Ptr transform = openvdb::math::Transform::createLinearTransform(
        // step 3
        openvdb::math::Mat4f(l2r.vx.x, l2r.vx.y, l2r.vx.z, l2r.vx.w,
                             l2r.vy.x, l2r.vy.y, l2r.vy.z, l2r.vy.w,
                             l2r.vz.x, l2r.vz.y, l2r.vz.z, l2r.vz.w,
                             l2r.vw.x, l2r.vw.y, l2r.vw.z, l2r.vw.w)
    );

    // step 2
    transform->preTranslate(openvdb::Vec3f(localMin.x,
                                           localMin.y,
                                           localMin.z));
    // step 1
    const Vec3f scale = (localMax- localMin) / upperBound;
    transform->preScale(openvdb::Vec3d(scale.x,
                                       scale.y,
                                       scale.z));

    // initialize density grid
    mBakedDensityGrid = openvdb::Vec3SGrid::create();
    mBakedDensityGrid->denseFill(vdbBbox,
                                 openvdb::Vec3f(1.f, 1.f, 1.f),
                                 true);
    mBakedDensityGrid->setTransform(transform);

    // compute feature size from transform
    const openvdb::Vec3d voxelSize = transform->voxelSize();
    mFeatureSize = max(voxelSize.x(),
                       voxelSize.y(),
                       voxelSize.z());
    volumeAssignmentTable->setFeatureSizes(this,
                                           volumeIds,
                                           mFeatureSize);
    // bake map shader
    openvdb::tools::foreach(mBakedDensityGrid->beginValueOn(),
        [&](const openvdb::Vec3SGrid::ValueOnIter& it) {
            // get xyz position of grid in render space
            // world space == render space
            const openvdb::Vec3d pd = mBakedDensityGrid->indexToWorld(it.getCoord());
            // If the primitive is a reference, it is generated and baked
            // in local space.  Unfortunately, some shaders (such as projection maps
            // and the OpenVdbMap) require a true render space position in order
            // to work correctly.  So to handle this we'll pretend that the reference
            // primitive is located at the world origin and transform it into render
            // space using the world2render transform (which should be what primToRender is).
            const Vec3f p = getIsReference() ?
                transformPoint(primToRender, Vec3f(pd.x(), pd.y(), pd.z())) :
                Vec3f(pd.x(), pd.y(), pd.z());
            // sample volume shader
            shading::Intersection isect;
            isect.init(getRdlGeometry());
            isect.setP(p);
            shading::TLState *tls = mcrt_common::getFrameUpdateTLS()->mShadingTls.get();
            const Color result = volumeShader->extinct(tls,
                                                       shading::State(&isect), 
                                                       Color(1.f), 
                                                       /*rayVolumeDepth*/ -1);
            // set result on grid
            it.setValue(openvdb::Vec3f(result.r,
                                       result.g,
                                       result.b));
    });

    mBakedDensityGrid->pruneGrid();
    mBakedDensitySampler.initialize(mBakedDensityGrid,
                                    volumeIds,
                                    STATS_BAKED_DENSITY_GRID_SAMPLES);
}

 Vec3f
 Primitive::evalVolumeSamplePosition(mcrt_common::ThreadLocalState* tls,
                                     uint32_t volumeId,
                                     const Vec3f& pSample,
                                     float time) const
{
    openvdb::Vec3d p = mVdbVelocity->getEvalPosition(tls,
                                                     volumeId,
                                                     pSample,
                                                     time);
    return Vec3f(p.x(),
                 p.y(),
                 p.z());
}

Color
Primitive::evalDensity(mcrt_common::ThreadLocalState* tls,
                       uint32_t volumeId,
                       const Vec3f& pSample,
                       float rayVolumeDepth,
                       const VolumeShader* const volumeShader) const
{
    if (mBakedDensitySampler.mIsValid) {
        const openvdb::Vec3d p(pSample[0],
                               pSample[1],
                               pSample[2]);

        const openvdb::Vec3f density = mBakedDensitySampler.eval(tls,
                                                                 volumeId,
                                                                 p,
                                                                 geom::internal::Interpolation::POINT);
        return Color(density.x(),
                     density.y(),
                     density.z());
    } else {
        // For homogenous volumes, we don't bake. Will always evaluate this block instead
        if (volumeShader) {
            // Calculate the volume density color at a certain rayVolumeDepth. This will allow you to specify 
            // density colors based on the depth of the volume (with use_attenuation_ramp), instead of using 
            // a uniform density color throughout
            shading::Intersection isect;
            shading::TLState *tls = nullptr;
            return volumeShader->extinct(tls,
                                         shading::State(&isect),
                                         Color(1.f),
                                         rayVolumeDepth);
        } 
        return mDensityColor;
    }
}

void
Primitive::evalVolumeCoefficients(mcrt_common::ThreadLocalState* tls,
                                  uint32_t volumeId,
                                  const Vec3f& pSample,
                                  Color* extinction,
                                  Color* albedo,
                                  Color* temperature,
                                  bool highQuality,
                                  float rayVolumeDepth,
                                  const VolumeShader* const volumeShader) const
{
    *extinction = evalDensity(tls,
                              volumeId,
                              pSample,
                              rayVolumeDepth,
                              volumeShader);
    *albedo = Color(1.0f);
    if (temperature) {
        *temperature = Color(1.0f);
    }
}

} // namespace internal
} // namespace geom
} // namespace moonray



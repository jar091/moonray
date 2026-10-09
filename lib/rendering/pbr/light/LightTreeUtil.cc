#include <moonray/rendering/pbr/light/LightTreeUtil.h>

using namespace scene_rdl2::math;

namespace moonray {
namespace pbr {

// =====================================================================================================================
// References:
// =====================================================================================================================
// [1] Alejandro Conty Estevez and Christopher Kulla. 2018. 
//     "Importance Sampling of Many Lights with Adaptive Tree Splitting"
// [2] Cem Yuksel. 2019. 
//     "Stochastic Lightcuts"
// =====================================================================================================================


/// --------------------------------------------- LightTreeCone --------------------------------------------------------

LightTreeCone combineCones(const LightTreeCone& a, const LightTreeCone& b)
{
    if (a.isEmpty()) { return b; }
    if (b.isEmpty()) { return a; }

    // find cone with the largest orientation angle
    const auto sortedCones = std::minmax(a, b, [](const LightTreeCone& x, const LightTreeCone& y) { 
        return x.mCosThetaO > y.mCosThetaO; 
    });
    const LightTreeCone& smallerCone = sortedCones.first;
    const LightTreeCone& largerCone = sortedCones.second;

    const float largerConeThetaO = largerCone.getThetaO();
    const float smallerConeThetaO = smallerCone.getThetaO();
    const bool twoSided = a.mTwoSided || b.mTwoSided;

    // get angle between two axes
    const float theta_d = scene_rdl2::math::dw_acos(dot(largerCone.mAxis, smallerCone.mAxis));
    // get the max emission angle (min cosine)
    const float theta_e = max(largerCone.getThetaE(), smallerCone.getThetaE());

    // check if bounds for "a" already cover "b"
    if (min(theta_d + smallerConeThetaO, sPi) <= largerConeThetaO) {
        return LightTreeCone(largerCone.mAxis, largerCone.mCosThetaO, scene_rdl2::math::cos(theta_e), twoSided);
    } else {
        // generate a new cone that covers a and b
        const float theta_o = (largerConeThetaO + theta_d + smallerConeThetaO) * 0.5f;
        // if theta_o > pi, this is a sphere
        if (theta_o >= sPi) {
            return LightTreeCone(largerCone.mAxis, /* cos of Pi*/ -1, scene_rdl2::math::cos(theta_e), twoSided);
        }

        // rotate a's axis towards b's axis to get the new central axis for the cone
        const float theta_r = theta_o - largerConeThetaO;
        // axis to rotate about (axis orthogonal to a axis and b axis)
        Vec3f rotAxis = cross(largerCone.mAxis, smallerCone.mAxis);

        // if facing the same way, keep the same axis
        if (length(rotAxis) < sEpsilon) {
            return LightTreeCone(largerCone.mAxis, scene_rdl2::math::cos(theta_o), scene_rdl2::math::cos(theta_e), twoSided);
        }
        rotAxis = normalize(rotAxis);

        // rotation a's axis around rotAxis by theta_r
        const Vec3f axis = scene_rdl2::math::cos(theta_r) * largerCone.mAxis + 
                           scene_rdl2::math::sin(theta_r) * cross(rotAxis, largerCone.mAxis);
        return LightTreeCone(normalize(axis), scene_rdl2::math::cos(theta_o), scene_rdl2::math::cos(theta_e), twoSided);
    }
}


/// ----------------------------------------- LightTreeBucket ----------------------------------------------------------

void LightTreeBucket::addLight(const Light* const light)
{
    mEnergy += scene_rdl2::math::luminance(light->getRadiance());
    mBBox.extend(light->getBounds());
    LightTreeCone cone(light->getDirection(0.f), scene_rdl2::math::cos(light->getThetaO()), 
              scene_rdl2::math::cos(light->getThetaE()), light->isTwoSided());
    mCone = combineCones(cone, mCone);
    mNumLights++;
}


/// ----------------------------------------- SplitCandidate -----------------------------------------------------------

bool SplitCandidate::isOnLeftSide(const Light* const light) const
{
    return light->getPosition(0)[mAxis.first] <= mAxis.second;
}

void SplitCandidate::setLeftSide(const LightTreeBucket& leftBucket)
{
    mLeftEnergy = leftBucket.mEnergy;
    mLeftBBox = leftBucket.mBBox;
    mLeftCone = leftBucket.mCone;
}
void SplitCandidate::setLeftSide(const SplitCandidate& leftSplit, const LightTreeBucket& leftBucket)
{
    mLeftEnergy = leftSplit.mLeftEnergy + leftBucket.mEnergy;
    mLeftBBox = leftSplit.mLeftBBox;
    mLeftBBox.extend(leftBucket.mBBox);
    mLeftCone = combineCones(leftSplit.mLeftCone, leftBucket.mCone);
}
void SplitCandidate::setRightSide(const LightTreeBucket& rightBucket)
{
    mRightEnergy = rightBucket.mEnergy;
    mRightBBox = rightBucket.mBBox;
    mRightCone = rightBucket.mCone;
}
void SplitCandidate::setRightSide(const SplitCandidate& rightSplit, const LightTreeBucket& rightBucket)
{
    mRightEnergy = rightSplit.mRightEnergy + rightBucket.mEnergy;
    mRightBBox = rightSplit.mRightBBox;
    mRightBBox.extend(rightBucket.mBBox);
    mRightCone = combineCones(rightSplit.mRightCone, rightBucket.mCone);
}

float SplitCandidate::calcOrientationTerm(const LightTreeCone& cone) const
{
    // Handle empty or invalid cones
    if (cone.isEmpty()) {
        return 4.0f * sPi; // Full sphere
    }

    const float theta_o = cone.getThetaO();
    const float theta_e = cone.getThetaE();

    // Calculate theta_w = min(theta_o + theta_e, π)
    const float theta_w = scene_rdl2::math::min(theta_o + theta_e, sPi);
    
    // Paper formula: MΩ = 2π (1 − cos θo) + (π/2) [2θw sin θo − cos(θo − 2θw) − 2θo sin θo + cos θo]
    const float orientationTermLeft = 2.f * sPi * (1.f - cone.mCosThetaO);
    const float orientationTermRight = 0.5f * sPi * (
                                      (2.0f * theta_w * cone.mSinThetaO) - 
                                      scene_rdl2::math::cos(theta_o - 2.0f * theta_w) -
                                      (2.0f * theta_o * cone.mSinThetaO) + cone.mCosThetaO
    );
    
    return orientationTermLeft + orientationTermRight;
}

float SplitCandidate::cost(const BBox3f& parentBBox, const LightTreeCone& parentCone) const
{
    // regularization factor Kr = length_max / length_axis
    const float length_max = parentBBox.size()[maxDim(parentBBox.size())];
    const float length_axis = scene_rdl2::math::max(parentBBox.size()[mAxis.first], sEpsilon);
    const float kr = length_max / length_axis;

    // Calculate areas and orientation terms
    const float leftArea = bboxArea(mLeftBBox);
    const float rightArea = bboxArea(mRightBBox);
    const float parentArea = bboxArea(parentBBox);

    const float leftOrientation = calcOrientationTerm(mLeftCone);
    const float rightOrientation = calcOrientationTerm(mRightCone);
    const float parentOrientation = calcOrientationTerm(parentCone);

    const float numeratorL = mLeftEnergy * leftArea * leftOrientation;
    const float numeratorR = mRightEnergy * rightArea * rightOrientation;
    const float denominator = parentArea * parentOrientation;
    
    return kr * ( (numeratorL + numeratorR) / denominator );
}

void SplitCandidate::performSplit(LightTreeNode& leftNode, LightTreeNode& rightNode, const Light* const* lights, 
                                  LightTreeNode& parent)
{
    // sort lights on either side of axis
    // Note: parent is non-const because std::partition modifies the light index array in-place
    uint32_t* startIt = parent.getLightIndexBegin();
    uint32_t* endIt   = parent.getLightIndexBegin() + parent.getLightCount();
    const float splitPlane = mAxis.second;
    const auto splitFunc = [&](uint32_t lightIndex) {
        return isOnLeftSide(lights[lightIndex]);
    };
    uint32_t* rightStartIt = std::partition(startIt, endIt, splitFunc);

    // create left and right child nodes
    const uint32_t lightCountLeft  = rightStartIt - startIt;
    const uint32_t lightCountRight = parent.getLightCount() - lightCountLeft;
    uint32_t* lightIndexBeginLeft  = parent.getLightIndexBegin();
    uint32_t* lightIndexBeginRight = parent.getLightIndexBegin() + lightCountLeft;

    // initialize nodes
    leftNode.init(lightCountLeft, lightIndexBeginLeft, lights, mLeftEnergy, mLeftCone, mLeftBBox);
    rightNode.init(lightCountRight, lightIndexBeginRight, lights, mRightEnergy, mRightCone, mRightBBox);
}



/// ------------------------------------------- LightTreeNode ----------------------------------------------------------

void LightTreeNode::crawlLights(const Light* const* lights, const std::function<void(const Light* light)>& func)
{
    for (uint32_t i = 0; i < mLightCount; ++i) {
        func(lights[mLightIndexBegin[i]]);
    }
}

void LightTreeNode::crawlLights(const Light* const* lights, const std::function<void(const Light* light)>& func) const
{
    for (uint32_t i = 0; i < mLightCount; ++i) {
        func(lights[mLightIndexBegin[i]]);
    }
}

// Given a list of lights, computes whether all lights included in the node
// are coincident (returns true or false), and calculates the bounding box of the light positions
bool 
LightTreeNode::computeLightDistribution(const Light* const* lights, 
                                        scene_rdl2::math::Vec3f& minBound, 
                                        scene_rdl2::math::Vec3f& range) const
{
    const scene_rdl2::math::Vec3f firstLightPosition = lights[mLightIndexBegin[0]]->getPosition(0.f);
    scene_rdl2::math::Vec3f maxBound = firstLightPosition;
    minBound = firstLightPosition;
    bool areCoincident = true;

    crawlLights(lights, [&](const Light* light) {
        const scene_rdl2::math::Vec3f& p = light->getPosition(0.f);

        if (!scene_rdl2::math::isEqual(p, firstLightPosition)) {
            areCoincident = false;
        }

        if (p.x < minBound[0]) minBound[0] = p.x;
        if (p.y < minBound[1]) minBound[1] = p.y;
        if (p.z < minBound[2]) minBound[2] = p.z;

        if (p.x > maxBound[0]) maxBound[0] = p.x;
        if (p.y > maxBound[1]) maxBound[1] = p.y;
        if (p.z > maxBound[2]) maxBound[2] = p.z;
    });

    range = {maxBound[0] - minBound[0], maxBound[1] - minBound[1], maxBound[2] - minBound[2]};

    return areCoincident;
}

void LightTreeNode::calcEnergyVariance(uint32_t lightCount, const Light* const* lights)
{
    mEnergyMean = mEnergy / lightCount;

    crawlLights(lights, [&](const Light* light) {
        float diff = luminance(light->getRadiance()) - mEnergyMean;
        diff *= diff;
        mEnergyVariance += diff;
    });

    mEnergyVariance /= lightCount;

    // --- Go ahead and do any mappings here to avoid the cost during sampling ---
    // Map the energy variance to [0, 1] range, then take power of 4 to boost splitting chances
    // then map to [0.5, 1] so that we only ever boost (not lower) splitting chances
    // this is the simplified version of the calculation (1 - (1 / (1 + sqrt(x)))^4 ) + 1) / 2
    // (this calculation is primarily based on experimentation; finding what works best)
    // TODO: I'm not sure what was intended by the above formula, because it does not 
    // match the implementation below. When we investigate re-incorporating the energy variance term,
    // we should revisit this calculation. 
    float arg = 1.f + scene_rdl2::math::sqrt(mEnergyVariance);
    float arg2 = arg * arg;
    mEnergyVariance = 1.0f - (0.5f / (arg2*arg2));
}

void LightTreeNode::init(const uint32_t lightCount, uint32_t* lightIndexBegin, const Light* const* lights)
{
    MNRY_ASSERT(lightCount > 0);

    mLightIndexBegin = lightIndexBegin;
    mLightCount = lightCount;

    crawlLights(lights, [&](const Light* light) {
        // add to the total energy
        // "maximum radiance emitted in some direction integrated over the emitting area"
        mEnergy += luminance(light->getRadiance());

        // extend the bounding box
        mBBox.extend(light->getBounds());

        // extend the orientation cone
        const LightTreeCone cone(light);
        mCone = combineCones(mCone, cone);
    });

    calcEnergyVariance(lightCount, lights);
}

void LightTreeNode::init(const uint32_t lightCount, uint32_t* lightIndexBegin, const Light* const* lights,
                         float energy, const LightTreeCone& cone, const BBox3f& bbox)
{
    MNRY_ASSERT(lightCount > 0);
    
    mLightIndexBegin = lightIndexBegin;
    mLightCount = lightCount;

    // We already have the energy, bbox, and cone from the split candidate
    mEnergy = energy;
    mBBox = bbox;
    mCone = cone;

    calcEnergyVariance(lightCount, lights);
}

float LightTreeNode::importance(const Vec3f& p, const Vec3f& n, const LightTreeNode& sibling, bool cullLights) const
{
    if (mLightCount == 0) return 0.f;

    // calculations used throughout
    const Vec3f bboxToPt = p - center(mBBox);
    const Vec3f dirToPt = normalize(bboxToPt);
    const float dSqr = lengthSqr(bboxToPt);           // squared distance to point
    const float rSqr = lengthSqr(mBBox.size() / 2.f); // squared radius of sphere circumscribing the bounding box

    // Find uncertainty angle
    float sinThetaU, cosThetaU;
    calcSinCosThetaU(dSqr, rSqr, &sinThetaU, &cosThetaU);

    const float distanceTerm = calcDistanceTerm(p, sibling, dSqr, rSqr);
    const float geometricTerm = calcGeometricTerm(p, cosThetaU, sinThetaU, dirToPt);
    const float materialTerm = calcMaterialTerm(p, n, cullLights, cosThetaU, sinThetaU, dirToPt);

    return mEnergy * geometricTerm * materialTerm * distanceTerm;
}

void LightTreeNode::calcSinCosThetaU(const float dSqr, const float rSqr, float* sinTheta, float* cosTheta) const
{
    MNRY_ASSERT(rSqr > 0.f);

    // if p is inside bounding sphere, return full sphere of directions
    if (dSqr < rSqr) {
        *cosTheta = -1.f;
        *sinTheta =  0.f;
        return;
    }

    // calc sin(theta)^2
    const float sinSqrTheta = rSqr / dSqr;
    *sinTheta = scene_rdl2::math::sqrt(sinSqrTheta);

    // use the cos(theta)^2 + sin(theta)^2 = 1 identity to find cos(theta)
    *cosTheta = scene_rdl2::math::sqrt(1.f - sinSqrTheta); 
}

float LightTreeNode::calcDistanceTerm(const Vec3f& p, const LightTreeNode& sibling, 
                                      const float rSqr, const float dSqr) const
{
    const float diagLenSqr = 4.f * rSqr; // (2r)^2 = 4r^2
    Vec3f diagSibling = sibling.getBBox().size();
    const float diagLenSqrSibling = lengthSqr(diagSibling);

    // find distance to bbox
    /// NOTE: I've found that getting the min dist to the bbox (instead of the center) produces 
    /// about the same results, but is more problematic
    const float dSqrSibling = lengthSqr(p - center(sibling.getBBox()));

    // We ensure the point is more than a threshold distance outside both bounding boxes because we want to ignore 
    // the distance term for nodes that are higher up in the BVH. The reason for this is that the distance term 
    // doesn't give us very meaningful information when the bounding boxes contain a large number of spread out lights, 
    // and it overpowers more useful info, like the material and geometric terms. So, we just include it at lower 
    // levels of the BVH, where 1/d^2 is more indicative of the lights' positions. [2] eq (3)
    if (dSqr > diagLenSqr && dSqrSibling > diagLenSqrSibling) {
        // we normalize this term with diagLen^2 so that it's continuous from 1.f
        // (then we make it the reciprocal so we can multiply it into the importance term)
        return diagLenSqr / dSqr;
    } 
    // if closer than some threshold distance for either node, just ignore distance term
    return 1.f;
}

float LightTreeNode::calcGeometricTerm(const Vec3f& p, float cosThetaU, float sinThetaU, const Vec3f& dirToPt) const
{
    float cosTheta = clamp(dot(mCone.mAxis, dirToPt), -1.f, 1.f);
    if (mCone.mTwoSided) {
        cosTheta = scene_rdl2::math::abs(cosTheta);
    }
    const float sinTheta = scene_rdl2::math::sqrt(1.f - cosTheta*cosTheta);

    const float cosThetaO = mCone.mCosThetaO;
    const float sinThetaO = mCone.mSinThetaO;

    // minimum angle any emitter in the cluster will form with the direction toward the shading pt
    // max(theta - thetaO - thetaU, 0)
    // ---------------------------------------------------------------------------------------------
    // max(cos(theta - thetaO), 1)
    const float cosThetaSubtract = cosSubClamped(sinTheta, cosTheta, sinThetaO, cosThetaO);
    const float sinThetaSubtract = scene_rdl2::math::sqrt(1.f - cosThetaSubtract*cosThetaSubtract);
    // max(cos(thetaSubtract - thetaU), 1)
    const float cosThetaPrime = cosSubClamped(sinThetaSubtract, cosThetaSubtract, sinThetaU, cosThetaU);
    // ---------------------------------------------------------------------------------------------

    // if min angle of emitter is larger than the cone's emission angle, can't contribute to point
    return cosThetaPrime <= mCone.mCosThetaE ? 0.f : cosThetaPrime;
}

float LightTreeNode::calcMaterialTerm(const Vec3f& p, const Vec3f& n, bool cullLights,
                                      float cosThetaU, float sinThetaU, const Vec3f& dirToPt) const
{
    // calc angle between normal and direction to the center of the bbox (cull lights below the horizon)
    const Vec3f dirToBbox = -dirToPt;
    const float cosThetaI = clamp(dot(n, dirToBbox), -1.f, 1.f);
    const float sinThetaI = scene_rdl2::math::sqrt(1.f - cosThetaI*cosThetaI);

    // get the MINIMUM angle to the light, which is theta_i minus theta_u
    // if theta_u > theta_i, then the normal is pointing toward the bounding box (inside uncertainty angle)
    const float cosThetaIPrime = cosSubClamped(sinThetaI, cosThetaI, sinThetaU, cosThetaU);

    /// TODO: the algorithm in the paper says absdot, but we cull if dot is less than zero, 
    /// so the light sample will be invalid anyway.... look at this again 
    if (cullLights && cosThetaIPrime < 0.f) {
        return 0.f;
    }
    return scene_rdl2::math::abs(cosThetaIPrime);
}

void LightTreeNode::printLights()
{
    std::cout << "{ ";
    for (uint32_t i = 0; i < mLightCount; ++i) {
        std::cout << mLightIndexBegin[i] << ", ";
    }
    std::cout << "}\n";
}

void LightTreeNode::print()
{
    std::cout << "{\n\tmBBox sizes: "       << mBBox.size()
                << "\n\ttype: "             << (mLightCount == 1 ? "leaf" : "cluster") 
                << "\n\tmLightIndexBegin: " << *mLightIndexBegin
                << "\n\tmLightCount: "      << mLightCount
                << "\n\tmRightNodeIndex: "  << mRightNodeIndex 
                << "\n}" << std::endl;
}

} // end namespace pbr
} // end namespace moonray

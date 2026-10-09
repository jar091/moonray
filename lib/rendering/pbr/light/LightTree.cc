// Copyright 2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

#include <moonray/rendering/pbr/light/LightTree.h>
#include <moonray/rendering/pbr/light/LightTree_ispc_stubs.h>

namespace moonray {
namespace pbr {


LightTree::LightTree(float samplingThreshold) 
    : mSamplingThreshold(samplingThreshold) 
{}

// ----------------------------- BUILD METHODS ---------------------------------------------------------------------- //

void LightTree::build(const Light* const* boundedLights, uint32_t boundedLightCount,
                      const Light* const* unboundedLights, uint32_t unboundedLightCount) 
{
    mBoundedLights = boundedLights;
    mUnboundedLights = unboundedLights;
    mBoundedLightCount = boundedLightCount;
    mUnboundedLightCount = unboundedLightCount;

    // Clear previous tree data to ensure a clean rebuild
    mNodes.clear();
    mLightIndices.clear();

    // pre-allocate since we know the size of the array
    mLightIndices.reserve(boundedLightCount);

    // create list of indices to order lights in tree
    for (int i = 0; i < boundedLightCount; ++i) {
        // don't use mesh lights in light bvh
        if (dynamic_cast<const MeshLight*>(boundedLights[i])) {
            mBoundedLightCount--;
        } else {
            mLightIndices.push_back(i);
        }
    }

    if (mBoundedLightCount > 0) {

        // create root node
        mNodes.emplace_back();
        LightTreeNode& rootNode = mNodes.back();
        rootNode.init(mBoundedLightCount, &mLightIndices[0], mBoundedLights);

        // build light tree recursively
        buildRecurse(/* root index */ 0);

        // update HUD data
        mNodesPtr = mNodes.data();
        mLightIndicesPtr = mLightIndices.data();
    }
}

/// Recursively build tree
void LightTree::buildRecurse(uint32_t nodeIndex) 
{
    LightTreeNode* node = &mNodes[nodeIndex];
    MNRY_ASSERT(node->getLightCount() != 0);

    // if leaf node, return
    if (node->isLeaf()) { return; }

    // -------- [1] section 4.4 ---------

    // find the lowest cost axis to split across
    LightTreeNode leftNode, rightNode;
    const float splitCost = split(leftNode, rightNode, nodeIndex);

    /// NOTE: we might investigate gains if we terminate early when cost to split is greater 
    /// than cost of producing a leaf (i.e. the total node energy)
    /// Looked noisy to me, but perhaps it was buggy previously
    /// two options: 1) choose a random light from the resulting cluster, or 
    /// 2) sample all lights in the cluster

    // ----------------------------------

    mNodes.push_back(leftNode);
    buildRecurse(mNodes.size() - 1);

    mNodes.push_back(rightNode);
    mNodes[nodeIndex].setRightNodeIndex(mNodes.size() - 1);
    buildRecurse(mNodes.size() - 1);
}

float LightTree::split(LightTreeNode& leftNode, LightTreeNode& rightNode, uint32_t nodeIndex)
{
    LightTreeNode& node = mNodes[nodeIndex];

    // if there are only two lights in node, just create two nodes
    if (node.getLightCount() == 2) {
        leftNode.init(/* lightCount */ 1, node.getLightIndexBegin(), mBoundedLights);
        rightNode.init(/* lightCount */ 1, node.getLightIndexBegin() + 1, mBoundedLights);
        return /* zero split cost */ 0;
    }

    scene_rdl2::math::Vec3f minBound;
    scene_rdl2::math::Vec3f range;
    bool lightsAreCoincident = node.computeLightDistribution(mBoundedLights, minBound, range);

    if (lightsAreCoincident) {
        // if lights are all stacked on top of each other,
        // just split the number of lights evenly
        return splitLightsEvenly(leftNode, rightNode, node);
    } 
    
    // find the axis with the lowest split cost
    float minCost = -1.f;
    SplitCandidate minSplit;
    for (int axis = 0; axis < 3; ++axis) {
        // calculate cost of splitting down this axis
        SplitCandidate split;

        // split axis into buckets, return splitCandidate for the axis 
        // between buckets that has the minimum cost
        float splitCost = splitAxis(split, axis, node, minBound[axis], range[axis]);

        // if this axis's split cost is lower than the current min
        // OR this is the first time through the loop, replace the min cost
        if (axis == 0 || splitCost < minCost) {
            minCost = splitCost;
            minSplit = split;
        }
    }

    // partition into left and right nodes using the chosen SplitCandidate
    minSplit.performSplit(leftNode, rightNode, mBoundedLights, node);
    return minCost;
}

float LightTree::splitLightsEvenly(LightTreeNode& leftNode, LightTreeNode& rightNode, LightTreeNode& parent) const
{
    // just evenly split the lights into both nodes
    const auto startIt = parent.getLightIndexBegin();
    // start the right node lights halfway through
    const auto rightStartIt = startIt + (parent.getLightCount() / 2);

    // create left and right child nodes
    const uint32_t lightCountLeft  = rightStartIt - startIt;
    const uint32_t lightCountRight = parent.getLightCount() - lightCountLeft;
    uint32_t* lightIndexBeginLeft  = parent.getLightIndexBegin();
    uint32_t* lightIndexBeginRight = parent.getLightIndexBegin() + lightCountLeft;

    // initialize nodes
    leftNode.init(lightCountLeft, lightIndexBeginLeft, mBoundedLights);
    rightNode.init(lightCountRight, lightIndexBeginRight, mBoundedLights);

    // we currently aren't using this cost anywhere, so it can be zero
    return 0.f;
}

float LightTree::splitAxis(SplitCandidate& minSplit, int axis, const LightTreeNode& node,
                           float minBound, float range) const
{
    /// TODO: make this a user parameter
    // constexpr so that the arrays below are ordinary fixed-size arrays:
    // variable-length arrays are a GCC extension
    constexpr int NUM_BUCKETS = 12;
    constexpr int numSplits = NUM_BUCKETS - 1;
    float bucketSize = range / NUM_BUCKETS;  // size of each bucket

    // create buckets
    LightTreeBucket buckets[NUM_BUCKETS];
    SplitCandidate splits[numSplits];

    // initialize split axes
    for (int i = 0; i < numSplits; ++i) {
        float axisOffset = minBound + bucketSize * (i + 1);
        splits[i].mAxis = {axis, axisOffset};
    }

    // populate buckets
    node.crawlLights(mBoundedLights, [&](const Light* light) {
        // Find which bucket this light belongs to by checking against split planes
        // This ensures consistency with performSplit() logic
        int bucketIndex = 0;
        for (int j = 0; j < numSplits; ++j) {
            if (splits[j].isOnLeftSide(light)) {
                bucketIndex = j;
                break;
            }
            bucketIndex = j + 1;
        }

        // add light to bucket
        LightTreeBucket& bucket = buckets[bucketIndex];
        bucket.addLight(light);
    });

    // Purge empty buckets and splits
    LightTreeBucket finalBuckets[NUM_BUCKETS];
    SplitCandidate finalSplits[numSplits];
    int finalSplitCount = purgeEmptyBuckets(finalBuckets, finalSplits, buckets, splits, NUM_BUCKETS);

    // Populate the splits, left and right sides separately
    populateSplitsLeftSide(finalSplits, finalBuckets, finalSplitCount);
    populateSplitsRightSide(finalSplits, finalBuckets, finalSplitCount);

    // find lowest cost split candidate and return
    return getCheapestSplit(minSplit, finalSplitCount, finalSplits, finalBuckets, node);
}



// ---------------------------------------------- SAMPLING METHODS -------------------------------------------------- //

void LightTree::sample(float* lightSelectionPdfs, const scene_rdl2::math::Vec3f& P, const scene_rdl2::math::Vec3f& N, 
                       const scene_rdl2::math::Vec3f* cullingNormal, const IntegratorSample1D& lightSelectionSample,
                       const int* lightIdMap, int nonMirrorDepth) const
{
    // For bounded lights, importance sample the BVH with adaptive tree splitting
    if (mBoundedLightCount > 0) {
        const bool cullLights = cullingNormal != nullptr;
        int startNodes[2] = {0, -1}; // {root index, null index}
        sampleRecurse(lightSelectionPdfs, startNodes, P, N, cullLights, lightSelectionSample, lightIdMap, nonMirrorDepth);
    }
}

void LightTree::sampleBranch(int& lightIndex, float& pdf, float& r, uint32_t nodeIndex,
                             const scene_rdl2::math::Vec3f& p, const scene_rdl2::math::Vec3f& n, 
                             bool cullLights) const
{
    const LightTreeNode& node = mNodes[nodeIndex];

    MNRY_ASSERT(node.getLightCount() > 0);

    // if node is a leaf, return the light
    if (node.isLeaf()) {
        lightIndex = node.getLightIndex();
        return;
    }

    // otherwise, get child nodes and traverse based on importance
    const uint32_t iL = nodeIndex + 1;
    const uint32_t iR = node.getRightNodeIndex();
    const float wL = mNodes[iL].importance(p, n, mNodes[iR], cullLights);
    const float wR = mNodes[iR].importance(p, n, mNodes[iL], cullLights);

    /// detect dead branch
    /// NOTE: there are three options: 1) just return invalid, as we're doing here, 2) choose a random
    /// light and return (technically more correct, but costly and doesn't improve convergence), 3) backtrack
    /// and choose the other branch. Worth exploring the best option in the future
    if (wL + wR == 0.f) {
        lightIndex = -1;
        return;
    }

    const float pdfL = wL / (wL + wR);

    // Choose which branch to traverse
    if (r < pdfL || pdfL == 1.f) {
        r = r / pdfL;
        pdf *= pdfL;
        sampleBranch(lightIndex, pdf, r, iL, p, n, cullLights);
    } else {
        const float pdfR = 1.f - pdfL;
        r = (r - pdfL) / pdfR;
        pdf *= pdfR;
        sampleBranch(lightIndex, pdf, r, iR, p, n, cullLights);
    }
}

/// Computes the spread of the lights in the node, biased by the energy variance. A higher value indicates that the 
/// lights are fairly compact, and/or that the energy doesn't vary much between them. A lower value indicates that the 
/// lights are spread apart, and/or that the energy varies much more. This measure is used to determine whether we 
/// split the node in question during sampling traversal, or whether we use importance sampling to choose a
/// representative light to sample. We could think of this as a confidence measure. How confident are we that our
/// importance sampling algorithm will choose a good representative light for this node? 
///
/// @see Alejandro Conty Estevez and Christopher Kulla. 2018. 
///      "Importance Sampling of Many Lights with Adaptive Tree Splitting" 
///       eqs (8), (9), (10)
///
float splittingHeuristic(const LightTreeNode& node, const scene_rdl2::math::Vec3f& p)
{
    // TODO: also base this on the orientation cone?

    const scene_rdl2::math::BBox3f bbox = node.getBBox();
    // TODO: we could move this calculation inside the node, since the bounding box doesn't change per sample
    const float radius = scene_rdl2::math::length(bbox.size()) * 0.5f;
    const float distance = scene_rdl2::math::distance(p, center(bbox));

    // if inside the bounding box, always split
    if (distance <= radius) {
        return 0.f;
    }

    // Find the size of the bbox (i.e. angle made with the bounding sphere) from the perspective of the point
    // theta should be between 0 and 90 deg (this is the half angle)
    const float lightSpreadTheta = scene_rdl2::math::asin(radius / distance); 
    // map to the [0,1] range by dividing by pi/2
    const float lightSpread = scene_rdl2::math::min(lightSpreadTheta * scene_rdl2::math::sTwoOverPi, 1.f);
    // take the sqrt to boost the lower values (i.e. raise the chance of splitting).
    // when splitting is low, it results in scenes with high amounts of noise -- this is 
    // why we generally bias more splitting over less.
    const float lightSpreadSqrt = sqrt(lightSpread);

    // TODO: we need further investigation on how to incorporate the energy variance.
    // It currently invalidates the lightSpreadSqrt, because the energy variance tends to go to extremes (either
    // 0 or a really high number). For now, we will just use lightSpreadSqrt, as it is stable & well-normalized.
    // We are preserving the energy variance calculation in the LightTreeNode intialization so that we can revisit
    // the calculation in the future.
    return 1.f - lightSpreadSqrt;
}

void LightTree::sampleRecurse(float* lightSelectionPdfs, int nodeIndices[2], const scene_rdl2::math::Vec3f& p, 
                              const scene_rdl2::math::Vec3f& n, bool cullLights, 
                              const IntegratorSample1D& lightSelectionSample, 
                              const int* lightIdMap, int nonMirrorDepth) const
{
    // For each node in list, decide whether to traverse both subtrees or to use a stochastic approach
    for (int i = 0; i < 2; ++i) {
        int nodeIndex = nodeIndices[i];
        if (nodeIndex == -1) continue; // -1 means index doesn't exist

        const LightTreeNode& node = mNodes[nodeIndex];
        float lightPdf = 1.f;
        int lightIndex = -1;

        if (node.getLightCount() == 0) { continue; }

        // There's only 1 light in node -- no splitting left to be done
        if (node.isLeaf()) {
            // The pdf is 1 since splitting is deterministic
            lightIndex = node.getLightIndex();
            chooseLight(lightSelectionPdfs, lightIndex, /*lightPdf*/ 1.f, lightIdMap);
            continue;
        }

        // Decide whether to traverse both subtrees (if the splitting heuristic is below the threshold/sampling quality)
        // OR to stop traversing and choose a light using importance sampling.
        if (mSamplingThreshold == 0.f || splittingHeuristic(node, p) > mSamplingThreshold) {
            // must generate new random number for every subtree traversal
            float r;
            lightSelectionSample.getSample(&r, nonMirrorDepth);
            sampleBranch(lightIndex, lightPdf, r, nodeIndex, p, n, cullLights);
            chooseLight(lightSelectionPdfs, lightIndex, lightPdf, lightIdMap);
            continue;
        } else {
            int iL = nodeIndex + 1;
            int iR = node.getRightNodeIndex();
            int children[2] = {iL, iR};
            sampleRecurse(lightSelectionPdfs, children, p, n, cullLights, 
                          lightSelectionSample, lightIdMap, nonMirrorDepth); 
        } 
    }
}

// --------------------------------- UTILITY FUNCTIONS -------------------------------------------------------------- //

size_t LightTree::getMemoryFootprintRecurse(int nodeIndex) const
{
    const LightTreeNode& node = mNodes[nodeIndex];

    // if node is a leaf, return size of node
    if (node.isLeaf()) {
        return sizeof(LightTreeNode);
    }

    int iL = nodeIndex + 1;
    int iR = node.getRightNodeIndex();

    return getMemoryFootprintRecurse(iL) + getMemoryFootprintRecurse(iR);
}

size_t LightTree::getMemoryFootprint() const
{
    if (mBoundedLightCount > 0 && mNodes.size() > 0) {
        size_t lightIndicesSize = mBoundedLightCount * sizeof(uint32_t);
        return lightIndicesSize + getMemoryFootprintRecurse(0);
    }
    return 0;
}

void LightTree::print() const
{
    std::cout << "nodeCount: " << mNodes.size() << std::endl;
    if (!mNodes.empty()) {
        printRecurse(0, 0);
    }   
}

void LightTree::printRecurse(uint32_t nodeIndex, int depth) const
{
    const LightTreeNode& node = mNodes[nodeIndex];

    for (int i = 0; i < depth; ++i) {
        std::cout << " ";
    }
    std::cout << nodeIndex;

    // if node is a leaf, return
    if (node.isLeaf()) {
        const Light* light = mBoundedLights[node.getLightIndex()];
        std::cout << ", leaf: " << light->getRdlLight()->get(scene_rdl2::rdl2::Light::sLabel) << std::endl;
        return;
    }

    unsigned int iL = nodeIndex + 1;
    unsigned int iR = node.getRightNodeIndex();
    std::cout << "\n";

    printRecurse(iL, depth+1);
    printRecurse(iR, depth+1);
}

// Print the buckets and the num lights associated for debugging
void LightTree::printBuckets(const LightTreeBucket* buckets, int numBuckets) const
{
    std::cout << "Buckets:\n";
    for (int i = 0; i < numBuckets; ++i) {
        const LightTreeBucket& bucket = buckets[i];
        std::cout << "  Bucket " << i << " (light count: " << bucket.mNumLights << "): ";
        std::cout << "\n";
    }
}

} // end namespace pbr
} // end namespace moonray
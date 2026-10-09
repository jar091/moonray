#pragma once

#include <moonray/rendering/pbr/light/Light.h>
#include "LightTree.hh"

namespace moonray {
namespace pbr {

// =====================================================================================================================
// References:
// =====================================================================================================================
// [1] Alejandro Conty Estevez and Christopher Kulla. 2018. 
//     "Importance Sampling of Many Lights with Adaptive Tree Splitting"
// =====================================================================================================================


class LightTreeNode;


/// ------------------------------------------------ LightTreeCone -----------------------------------------------------
/// This struct represents the orientation cone that bounds the normals and emission falloff for a cluster of lights. 
/// We use this LightTreeCone structure to 1) decide how to cluster lights, 2) calculate the material and geometric terms 
/// for the importance heuristic.
///
/// @see [1] section 4.1

struct LightTreeCone
{
    /// Default Constructor
    LightTreeCone() 
        : mAxis(scene_rdl2::math::Vec3f(0.f)),  // central orientation axis
          mCosThetaO(0.f),                      // cosine of the angle bounding the spread of normals around the axis
          mSinThetaO(0.f),                      // sine of the angle bounding the spread of normals around the axis
          mCosThetaE(0.f),                      // cosine of the angle representing the bound on the emission falloff
          mTwoSided(false) {}                   // does this cone contain a two-sided light?

    /// Full Constructor
    LightTreeCone(const scene_rdl2::math::Vec3f& axis, float cosThetaO, float cosThetaE, bool isTwoSided) 
        : mAxis(axis), 
          mCosThetaO(scene_rdl2::math::clamp(cosThetaO, -1.f, 1.f)),
          mSinThetaO(scene_rdl2::math::sqrt(1.f - mCosThetaO*mCosThetaO)),
          mCosThetaE(scene_rdl2::math::clamp(cosThetaE, -1.f, 1.f)),
          mTwoSided(isTwoSided) {}

    /// Copy Constructor
    LightTreeCone(const LightTreeCone& coneToCopy)
        : mAxis(coneToCopy.mAxis),
          mCosThetaO(coneToCopy.mCosThetaO),
          mSinThetaO(coneToCopy.mSinThetaO),
          mCosThetaE(coneToCopy.mCosThetaE),
          mTwoSided(coneToCopy.mTwoSided) {}

    /// Constructor using light properties
    LightTreeCone(const Light* const light)
        : mAxis(light->getDirection(0.f)),
          mCosThetaO(scene_rdl2::math::clamp(scene_rdl2::math::cos(light->getThetaO()), -1.f, 1.f)),
          mSinThetaO(scene_rdl2::math::clamp(scene_rdl2::math::sin(light->getThetaO()), -1.f, 1.f)),
          mCosThetaE(scene_rdl2::math::clamp(scene_rdl2::math::cos(light->getThetaE()), -1.f, 1.f)),
          mTwoSided(light->isTwoSided()) {}

    /// Is this LightTreeCone empty?
    bool isEmpty() const { return isZero(mAxis); }

    /// Get orientation angle in radians
    float getThetaO() const { return scene_rdl2::math::dw_acos(mCosThetaO); }

    /// Get emission angle in radians
    float getThetaE() const { return scene_rdl2::math::dw_acos(mCosThetaE); }

    void print() const
    {
        std::cout << "LightTreeCone:\n\tAxis: " << mAxis << "\n\tCosThetaO: " << mCosThetaO << "\n\tCosThetaE: " 
                  << mCosThetaE << "\n\tTwoSided? " << mTwoSided << std::endl;
    }

    LIGHT_TREE_CONE_MEMBERS;
};

/// Combine orientation cones a and b.
/// @see [1] algorithm 1
LightTreeCone combineCones(const LightTreeCone& a, const LightTreeCone& b);



/// ---------------------------------------- LightTreeBucket -----------------------------------------------------------

struct LightTreeBucket
{
    /// Adds the properties of the light to the bucket
    void addLight(const Light* const light);

    float mEnergy                   = 0.f;
    scene_rdl2::math::BBox3f mBBox  = scene_rdl2::math::BBox3f(scene_rdl2::util::empty);
    LightTreeCone mCone;
    uint32_t mNumLights             = 0;
};



/// ------------------------------------------------ SplitCandidate ----------------------------------------------------
/// A SplitCandidate is a potential split of a node into two children. We will typically have a SplitCandidate for all 
/// three axes, and we choose the SplitCandidate with the lowest cost.

struct SplitCandidate
{
    /// Finds the area of the bbox
    inline float bboxArea(const scene_rdl2::math::BBox3f& bbox) const 
    {
        scene_rdl2::math::Vec3f dim = bbox.size();
        return 2*dim[0]*dim[1] + 2*dim[1]*dim[2] + 2*dim[0]*dim[2];
    }

    /// Is the left side of this split empty?
    bool leftIsEmpty() const { return mLeftBBox.size()[mAxis.first] == 0.f  || mLeftEnergy <= 0.f; }
    /// Is the right side of this split empty?
    bool rightIsEmpty() const { return mRightBBox.size()[mAxis.first] == 0.f  || mRightEnergy <= 0.f; }

    /// Is this light on the left side of the split?
    bool isOnLeftSide(const Light* const light) const;

    // Populate left-side attributes
    void setLeftSide(const LightTreeBucket& leftBucket);
    void setLeftSide(const SplitCandidate& leftSplit, const LightTreeBucket& leftBucket);

    // Populate right-side attributes
    void setRightSide(const LightTreeBucket& rightBucket);
    void setRightSide(const SplitCandidate& rightSplit, const LightTreeBucket& rightBucket);

    /** @see (1) from "Importance Sampling of Many Lights...". */
    float calcOrientationTerm(const LightTreeCone& cone) const;

    // Surface Area Orientation Heuristic (SAOH) (Section 4.4)
    float cost(const scene_rdl2::math::BBox3f& parentBBox, const LightTreeCone& parentCone) const;

    // Having chosen this SplitCandidate, perform the node creation and light partitioning
    void performSplit(LightTreeNode& leftNode, LightTreeNode& rightNode, const Light* const* lights, 
                      LightTreeNode& parent);

    std::pair<int, float> mAxis         = {0, 0.f};   // split axis, 1st: 0, 1, or 2 (x, y, or z), 2nd: value
    float mLeftEnergy                   = 0.f;
    float mRightEnergy                  = 0.f;
    scene_rdl2::math::BBox3f mLeftBBox  = scene_rdl2::math::BBox3f(scene_rdl2::util::empty);
    scene_rdl2::math::BBox3f mRightBBox = scene_rdl2::math::BBox3f(scene_rdl2::util::empty);
    LightTreeCone mLeftCone;
    LightTreeCone mRightCone;
};



// ------------------------------------------- LightTreeNode -----------------------------------------------------------
/// A LightTreeNode represents a cluster in our LightTree. @see [1] section 4.1 

class LightTreeNode
{
public:

    LightTreeNode()
        : mLightIndexBegin(nullptr),        // ptr to the beginning of the node's lights in lightIndices
          mRightNodeIndex(0),               // index of right child node in mNodes
          mLightCount(0),                   // total # lights belonging to node
          mBBox(scene_rdl2::util::empty),   // node's bounding box
          mCone(),                          // orientation cone
          mEnergy(0.f),                     // combined energy of all lights in node
          mEnergyVariance(0.f),             // variance in energy
          mEnergyMean(0.f) {}               // mean of energy

/// ------------------------------------- Inline Utils --------------------------------------------------
    /// Is this node a leaf?
    inline bool isLeaf() const { return mLightCount == 1; }

    /// Get pointer to the start light index
    inline const uint32_t* getLightIndexBegin() const { return mLightIndexBegin; }
    inline uint32_t* getLightIndexBegin() { return mLightIndexBegin; }

    /// Get the index of the node's right child
    inline uint32_t getRightNodeIndex() const { return mRightNodeIndex; }

    /// Get the number of lights in this node
    inline uint32_t getLightCount() const { return mLightCount; }

    /// Gets the light index, if it's a leaf. Otherwise, returns -1.
    inline int getLightIndex() const { return isLeaf() && mLightIndexBegin ? *mLightIndexBegin : -1; }

    /// Gets the bounding box of the node
    inline const scene_rdl2::math::BBox3f& getBBox() const { return mBBox; }

    /// Gets the emission-bounding cone
    inline const LightTreeCone& getCone() const { return mCone; }

    /// Gets the energy variance
    inline float getEnergyVariance() const { return mEnergyVariance; }

    /// Gets the energy mean
    inline float getEnergyMean() const { return mEnergyMean; }

    /// Sets the index of the right child
    inline void setRightNodeIndex(uint32_t i) { mRightNodeIndex = i; }

/// ----------------------------------------------------------------------------------------------------

    // Initialize the node. We do a number of calculations in this step to avoid it during sampling time.
    void init(const uint32_t lightCount, 
              uint32_t* lightIndexBegin, 
              const Light* const* lights);

    /// Initialize the node, with most of the calculations passed in from the SplitCandidate
    void init(const uint32_t lightCount,
              uint32_t* lightIndexBegin,
              const Light* const* lights,
              float energy,
              const LightTreeCone& cone,
              const scene_rdl2::math::BBox3f& bbox);

    // Calculate the importance weight for the node
    float importance(const scene_rdl2::math::Vec3f& p, 
                     const scene_rdl2::math::Vec3f& n, 
                     const LightTreeNode& sibling,
                     bool cullLights) const;

    // Crawl all lights in this node, applying the given function to each light
    // While these functions may be simple for-loops, having them as member functions
    // allows us to encapsulate the logic for accessing the lights in one place,
    // saving future developers from potential mistakes due to the multiple levels 
    // of indirection.
    void crawlLights(const Light* const* lights, const std::function<void(const Light* light)>& func) const;
    void crawlLights(const Light* const* lights, const std::function<void(const Light* light)>& func);

    // Returns if all lights in the node are coincident, 
    // and calculates the bounding box of the light positions
    bool computeLightDistribution(const Light* const* lights,
                                  scene_rdl2::math::Vec3f& minBound, 
                                  scene_rdl2::math::Vec3f& range) const;
    
    void printLights();
    void print();

private:
    /// ---------------------------------- Inline Utils --------------------------------------------------

    // cos(max(0, thetaA - thetaB)) (from PBRT v4)
    // this trick only works because cos is monotonic across [0, pi]
    inline float cosSubClamped(const float sinThetaA, const float cosThetaA,
                               const float sinThetaB, const float cosThetaB) const
    {
        // if thetaB > thetaA, set the angle difference to 0
        // (i.e if cosThetaB < cosThetaA, clamp the cosine difference to 1)
        if (cosThetaB < cosThetaA) { return 1.f; }
        return cosThetaA * cosThetaB + sinThetaA * sinThetaB;
    }

    /// ---------------------------------------------------------------------------------------------------

    void calcEnergyVariance(uint32_t lightCount, const Light* const* lights);

    // Calculate the uncertainty angle (angle subtended by the bounding box)
    void calcSinCosThetaU(const float dSqr, const float rSqr, float* sinTheta, float* cosTheta) const;

    /// Calculate the distance term for the importance weight. @see [2] eq (3)
    float calcDistanceTerm(const scene_rdl2::math::Vec3f& p, const LightTreeNode& sibling, 
                           const float dSqr, const float rSqr) const;

    /// Calculate the geometry term (solid angle)
    /// TODO: cull lights before tree traversal?
    /// @see [1] eq (3)
    float calcGeometricTerm(const scene_rdl2::math::Vec3f& p, float cosThetaU, float sinThetaU,
                            const scene_rdl2::math::Vec3f& dirToPt) const;

    /// Material term calculation
    /// TODO: add some sort of BSDF approximation, perhaps based on Liu et al
    /// @see [1] eq (3)
    float calcMaterialTerm(const scene_rdl2::math::Vec3f& p, const scene_rdl2::math::Vec3f& n, bool cullLights,
                           float cosThetaU, float sinThetaU, const scene_rdl2::math::Vec3f& dirToPt) const;


    LIGHT_TREE_NODE_MEMBERS;
};

} // end namespace pbr
} // end namespace moonray

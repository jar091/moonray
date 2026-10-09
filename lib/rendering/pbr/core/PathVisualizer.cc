// Copyright 2025-2026 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

#include <moonray/rendering/pbr/core/PathVisualizer.h>

#include <moonray/common/time/Timer.h>
#include <moonray/rendering/drawing/drawing.h>

#include <moonray/rendering/mcrt_common/Frustum.h>
#include <moonray/rendering/mcrt_common/Ray.h>

#include <moonray/rendering/pbr/camera/Camera.h>
#include <moonray/rendering/pbr/camera/PerspectiveCamera.h>
#include <moonray/rendering/pbr/core/Scene.h>

#include <moonray/rendering/rndr/RenderContext.h>
#include <moonray/rendering/rndr/Util.h>

#include <moonray/rendering/rt/EmbreeAccelerator.h>

#include <scene_rdl2/common/grid_util/VectorPacket.h>
#include <scene_rdl2/common/rec_time/RecTime.h>
#include <scene_rdl2/render/cache/ValueContainerEnqueue.h>
#include <scene_rdl2/render/util/StrUtil.h>

using RenderTimer = moonray::time::RAIITimerAverageDouble;
using scene_rdl2::math::Vec3f;
using scene_rdl2::math::Vec2f;
using scene_rdl2::math::Vec2i;

namespace moonray {
namespace pbr {

//----------------------------------------------------------------------------------------------------------------------

std::ostream& operator<<(std::ostream& os, const State& state) {
    switch(state) {
        case State::NONE:             return os << "NONE";
        case State::READY:            return os << "READY";
        case State::START_RECORD:     return os << "START_RECORD";
        case State::RECORD:           return os << "RECORD";
        case State::STOP_RECORD:      return os << "STOP_RECORD";
        case State::REQUEST_DRAW:     return os << "REQUEST_DRAW";
        case State::GENERATE_LINES:   return os << "GENERATE_LINES";
        case State::DRAW:             return os << "DRAW";
        default:                      return os << "UNKNOWN FLAG";
    }
}

std::ostream& operator<<(std::ostream& os, const PathVisualizer::Flags& flag) {
    if (PathVisualizer::matchesLineStatus(flag, PathVisualizer::Flags::INACTIVE))     { os << "INACTIVE "; }

    if (PathVisualizer::matchesOriginType(flag, PathVisualizer::Flags::CAMERA))       { os << "CAMERA "; }
    if (PathVisualizer::matchesOriginType(flag, PathVisualizer::Flags::DIFFUSE))      { os << "DIFFUSE "; }
    if (PathVisualizer::matchesOriginType(flag, PathVisualizer::Flags::SPECULAR))     { os << "SPECULAR "; }
    if (PathVisualizer::matchesOriginType(flag, PathVisualizer::Flags::LIGHT))        { os << "LIGHT "; }

    if (PathVisualizer::matchesRayType(flag, PathVisualizer::Flags::DIRECT))          { os << "DIRECT "; }
    if (PathVisualizer::matchesRayType(flag, PathVisualizer::Flags::INDIRECT))        { os << "INDIRECT "; }
    if (PathVisualizer::matchesRayType(flag, PathVisualizer::Flags::SAMPLE))          { os << "SAMPLE "; }
    return os;
}

std::ostream& operator<<(std::ostream& os, const PathVisualizer::LineSegment& line) {
    os << "\nLineSegment {\n";
    os << "  mNodeIndex: " << line.mNodeIndex << ",\n";
    os << "  mPx1: (" << line.mPx1.x << ", " << line.mPx1.y << "),\n";
    os << "  mPx2: (" << line.mPx2.x << ", " << line.mPx2.y << "),\n";
    os << "  mFlags: " << line.mFlags << ",\n";
    os << "  mDrawEndpoint: " << line.mDrawEndpoint << ",\n";
    os << "  mAlpha: " << line.mAlpha << ",\n";
    os << "}\n";
    return os;
}

std::ostream& operator<<(std::ostream& os, const PathVisualizer::Node& node) {
    os << "\nNode {\n";
    os << "  mRayOriginIndex: " << node.mRayOriginIndex << ",\n";
    os << "  mRayEndpointIndex: " << node.mRayEndpointIndex << ",\n";
    os << "  mRayIsectIndex: " << node.mRayIsectIndex << ",\n";
    os << "  mDepth: " << static_cast<int>(node.mDepth) << ",\n";
    os << "  mFlags: " << node.mFlags << ",\n";
    os << "}\n";
    return os;
}

//----------------------------------------------------------------------------------------------------------------------

std::string
PathVisualizerParams::show() const
{
    using scene_rdl2::str_util::boolStr;

    std::ostringstream ostr;
    ostr << "PathVisualizerParams {\n"
         << "  mPixelX:" << mPixelX << '\n'
         << "  mPixelY:" << mPixelY << '\n'
         << "  mMaxDepth:" << mMaxDepth << '\n'
         << "  mPixelSamples:" << mPixelSamples << '\n'
         << "  mLightSamples:" << mLightSamples << '\n'
         << "  mBsdfSamples:" << mBsdfSamples << '\n'
         << "  mUseSceneSamples:" << boolStr(mUseSceneSamples) << '\n'
         << "  mDirectRaysOn:" << boolStr(mDirectRaysOn) << '\n'
         << "  mIndirectRaysOn:" << boolStr(mIndirectRaysOn) << '\n'
         << "  mSamplesOn:" << boolStr(mSamplesOn) << '\n'
         << "  mIndirectSpecularRaysOn:" << boolStr(mIndirectSpecularRaysOn) << '\n'
         << "  mIndirectDiffuseRaysOn:" << boolStr(mIndirectDiffuseRaysOn) << '\n'
         << "  mDirectSpecularRaysOn:" << boolStr(mDirectSpecularRaysOn) << '\n'
         << "  mDirectDiffuseRaysOn:" << boolStr(mDirectDiffuseRaysOn) << '\n'
         << "  mDirectLightRaysOn:" << boolStr(mDirectLightRaysOn) << '\n'
         << "  mDiffuseSamplesOn:" << boolStr(mDiffuseSamplesOn) << '\n'
         << "  mSpecularSamplesOn:" << boolStr(mSpecularSamplesOn) << '\n'
         << "  mLightSamplesOn:" << boolStr(mLightSamplesOn) << '\n'
         << "  mCameraRayColor:" << mCameraRayColor << '\n'
         << "  mIndirectSpecularRayColor:" << mIndirectSpecularRayColor << '\n'
         << "  mIndirectDiffuseRayColor:" << mIndirectDiffuseRayColor << '\n'
         << "  mDirectSpecularRayColor:" << mDirectSpecularRayColor << '\n'
         << "  mDirectDiffuseRayColor:" << mDirectDiffuseRayColor << '\n'
         << "  mDirectLightRayColor:" << mDirectLightRayColor << '\n'
         << "  mLineWidth:" << mLineWidth << '\n'
         << "  mMaxRayLength:" << mMaxRayLength << '\n'
         << "  mHiddenLineOpacity:" << mHiddenLineOpacity << '\n'
         << "  mPixelXmin:" << mPixelXmin << '\n'
         << "  mPixelYmin:" << mPixelYmin << '\n'
         << "  mPixelXmax:" << mPixelXmax << '\n'
         << "  mPixelYmax:" << mPixelYmax << '\n'
         << "}";
    return ostr.str();
}

std::string
PathVisualizerParams::showPixel() const
{
    using scene_rdl2::str_util::boolStr;
    
    std::ostringstream ostr;
    ostr << "pixel {\n"
         << "  mOn:" << boolStr(mOn) << '\n'
         << "  mPixelX:" << mPixelX << '\n'
         << "  mPixelY:" << mPixelY << '\n'
         << "  mPixelXmin:" << mPixelXmin << '\n'
         << "  mPixelYmin:" << mPixelYmin << '\n'
         << "  mPixelXmax:" << mPixelXmax << '\n'
         << "  mPixelYmax:" << mPixelYmax << '\n'
         << "}";
    return ostr.str();
}

void
PathVisualizerParams::parserConfigure()
{
    auto setSingleUInt = [](Arg& arg, const bool active, unsigned& dest, const std::string& msg) {
        if (!active) {
            arg++;
            return arg.msg("skip\n");
        }
        std::ostringstream ostr;
        dest = (arg++).as<unsigned>(0);
        ostr << msg << dest;
        return arg.msg(ostr.str() + '\n');
    };
    auto setDoubleUInt = [](Arg& arg, const bool active, unsigned& destA, unsigned& destB, const std::string& msg) {
        if (!active) {
            arg += 2;
            return arg.msg("skip\n");
        }
        std::ostringstream ostr;
        destA = (arg++).as<unsigned>(0);
        destB = (arg++).as<unsigned>(0);
        ostr << msg << destA << ',' << destB;
        return arg.msg(ostr.str() + '\n');
    };
    auto setSingleFloat = [](Arg& arg, const bool active, float& dest, const std::string& msg) {
        if (!active) {
            arg++;
            return arg.msg("skip\n");
        }
        std::ostringstream ostr;
        dest = (arg++).as<float>(0);
        ostr << msg << dest;
        return arg.msg(ostr.str() + '\n');
    };
    auto setSingleBool = [](Arg& arg, const bool active, bool& dest, const std::string& msg) {
        if (!active) {
            arg++;
            return arg.msg("skip\n");
        }
        std::ostringstream ostr;
        dest = (arg++).as<bool>(0);
        ostr << msg << scene_rdl2::str_util::boolStr(dest);
        return arg.msg(ostr.str() + '\n');
    };
    auto setColorArg = [](Arg& arg, const bool active, scene_rdl2::math::Color& destCol, const std::string& msg) {
        if (!active) {
            arg += 3;
            return arg.msg("skip\n");
        }
        destCol.r = (arg++).as<float>(0);
        destCol.g = (arg++).as<float>(0);
        destCol.b = (arg++).as<float>(0);
        std::ostringstream ostr;
        ostr << msg << destCol;
        return arg.msg(ostr.str() + '\n');
    };
    auto setDeltaPixArg = [](Arg& arg, const bool active, unsigned& pix,
                             const int min, const int max, const std::string& msg) {
        if (!active) {
            arg++;
            return arg.msg("skip\n");
        }
        const int delta = (arg++).as<int>(0);
        pix = static_cast<unsigned>(std::clamp(static_cast<int>(pix) + delta, std::min(min, 0), max));
        std::ostringstream ostr;
        ostr << msg << pix;
        return arg.msg(ostr.str() + '\n');
    };
    auto setDeltaSampleArg = [](Arg& arg, const bool active, unsigned& sample, const std::string& msg) {
        if (!active) {
            arg++;
            return arg.msg("skip\n");
        }
        const int delta = (arg++).as<int>(0);
        sample = static_cast<unsigned>(std::max(static_cast<int>(sample) + delta, 1));
        std::ostringstream ostr;
        ostr << msg << sample;
        return arg.msg(ostr.str() + '\n');
    };
    auto setToggleArg = [](Arg& arg, const bool active, bool& flag, const std::string& msg) {
        if (!active) {
            arg++;
            return arg.msg("skip\n");
        }
        flag = !flag;
        std::ostringstream ostr;
        ostr << msg << scene_rdl2::str_util::boolStr(flag);
        return arg.msg(ostr.str() + '\n');
    };
        
    mParser.description("PathVisualizerParams command");

    mParser.opt("pixel", "<x> <y>", "set pixel X Y",
                [&](Arg& arg) { return setDoubleUInt(arg, mOn, mPixelX, mPixelY, "pixel="); });
    mParser.opt("pixelX", "<x>", "set pixel X",
                [&](Arg& arg) { return setSingleUInt(arg, mOn, mPixelX, "pixelX="); });
    mParser.opt("pixelY", "<y>", "set pixel Y",
                [&](Arg& arg) { return setSingleUInt(arg, mOn, mPixelY, "pixelY="); });
    mParser.opt("maxDepth", "<depth>", "set max depth",
                [&](Arg& arg) { return setSingleUInt(arg, mOn, mMaxDepth, "maxDepth="); }); 
    mParser.opt("pixelSamples", "<n>", "set pixel samples",
                [&](Arg& arg) { return setSingleUInt(arg, mOn, mPixelSamples, "pixelSamples="); });
    mParser.opt("lightSamples", "<n>", "set light samples",
                [&](Arg& arg) { return setSingleUInt(arg, mOn, mLightSamples, "lightSamples="); });
    mParser.opt("bsdfSamples", "<n>", "set BSDF samples",
                [&](Arg& arg) { return setSingleUInt(arg, mOn, mBsdfSamples, "bsdfSamples="); });

    mParser.opt("useSceneSamplesSw", "<on|off>", "set useSceneSamples condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mUseSceneSamples, "useSceneSamplesSw="); });

    // ---- Included for backwards compatibility ----
    mParser.opt("occlusionRaysSw", "<on|off>", "set occlusionRays condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mDirectRaysOn, "occlusionRaysSw="); });
    mParser.opt("specularRaysSw", "<on|off>", "set specularRays condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mIndirectSpecularRaysOn, "specularRaysSw="); });
    mParser.opt("diffuseRaysSw", "<on|off>", "set diffuseRays condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mIndirectDiffuseRaysOn, "diffuseRaysSw="); });
    mParser.opt("bsdfSamplesSw", "<on|off>", "set bsdfSamples condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mDirectDiffuseRaysOn, "bsdfSamplesSw="); });
    // ----------------------------------------------
    
    mParser.opt("directRaysSw", "<on|off>", "set directRays condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mDirectRaysOn, "directRaysSw="); });
    mParser.opt("indirectRaysSw", "<on|off>", "set indirectRays condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mIndirectRaysOn, "indirectRaysSw="); });
    mParser.opt("samplesSw", "<on|off>", "set samples condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mSamplesOn, "samplesSw="); });
    mParser.opt("indirectSpecularRaysSw", "<on|off>", "set indirectSpecularRays condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mIndirectSpecularRaysOn, "indirectSpecularRaysSw="); });
    mParser.opt("indirectDiffuseRaysSw", "<on|off>", "set indirectDiffuseRays condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mIndirectDiffuseRaysOn, "indirectDiffuseRaysSw="); });
    mParser.opt("directSpecularRaysSw", "<on|off>", "set directSpecularRays condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mDirectSpecularRaysOn, "directSpecularRaysSw="); });
    mParser.opt("directDiffuseRaysSw", "<on|off>", "set directDiffuseRays condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mDirectDiffuseRaysOn, "directDiffuseRaysSw="); });
    mParser.opt("directLightRaysSw", "<on|off>", "set directLightRays condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mDirectLightRaysOn, "directLightRaysSw="); });
    mParser.opt("diffuseSamplesSw", "<on|off>", "set diffuseSamples condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mDiffuseSamplesOn, "diffuseSamplesSw="); });
    mParser.opt("specularSamplesSw", "<on|off>", "set specularSamples condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mSpecularSamplesOn, "specularSamplesSw="); });
    mParser.opt("lightSamplesSw", "<on|off>", "set lightSamples condition",
                [&](Arg& arg) { return setSingleBool(arg, mOn, mLightSamplesOn, "lightSamplesSw="); });

    // ---- Included for backwards compatibility ----
    mParser.opt("specularRayColor", "<r> <g> <b>", "set specularRayColor normalized 0~1 col value",
                [&](Arg& arg) { return setColorArg(arg, mOn, mIndirectSpecularRayColor, "specularRayColor="); });
    mParser.opt("diffuseRayColor", "<r> <g> <b>", "set diffuseRayColor normalized 0~1 col value",
                [&](Arg& arg) { return setColorArg(arg, mOn, mIndirectDiffuseRayColor, "diffuseRayColor="); });
    mParser.opt("bsdfSampleColor", "<r> <g> <b>", "set bsdfSampleColor normalized 0~1 col value",
                [&](Arg& arg) { return setColorArg(arg, mOn, mDirectDiffuseRayColor, "bsdfSampleColor="); });
    mParser.opt("lightSampleColor", "<r> <g> <b>", "set lightSampleColor normalized 0~1 col value",
                [&](Arg& arg) { return setColorArg(arg, mOn, mDirectLightRayColor, "lightSampleColor="); });
    // ----------------------------------------------
    
    mParser.opt("cameraRayColor", "<r> <g> <b>", "set cameraRayColor normalized 0~1 col value",
                [&](Arg& arg) { return setColorArg(arg, mOn, mCameraRayColor, "cameraRayColor="); });
    mParser.opt("indirectSpecularRayColor", "<r> <g> <b>", "set indirectSpecularRayColor normalized 0~1 col value",
                [&](Arg& arg) { return setColorArg(arg, mOn, mIndirectSpecularRayColor, "indirectSpecularRayColor="); });
    mParser.opt("indirectDiffuseRayColor", "<r> <g> <b>", "set indirectDiffuseRayColor normalized 0~1 col value",
                [&](Arg& arg) { return setColorArg(arg, mOn, mIndirectDiffuseRayColor, "indirectDiffuseRayColor="); });
    mParser.opt("directSpecularRayColor", "<r> <g> <b>", "set directSpecularRayColor normalized 0~1 col value",
                [&](Arg& arg) { return setColorArg(arg, mOn, mDirectSpecularRayColor, "directSpecularRayColor="); });
    mParser.opt("directDiffuseRayColor", "<r> <g> <b>", "set directDiffuseRayColor normalized 0~1 col value",
                [&](Arg& arg) { return setColorArg(arg, mOn, mDirectDiffuseRayColor, "directDiffuseRayColor="); });
    mParser.opt("directLightRayColor", "<r> <g> <b>", "set directLightRayColor normalized 0~1 col value",
                [&](Arg& arg) { return setColorArg(arg, mOn, mDirectLightRayColor, "directLightRayColor="); });

    mParser.opt("lineWidth", "<w>", "set line width",
                [&](Arg& arg) { return setSingleFloat(arg, mOn, mLineWidth, "lineWidth="); });
    mParser.opt("maxRayLength", "<length>", "set max ray length",
                [&](Arg& arg) { return setSingleFloat(arg, mOn, mMaxRayLength, "maxRayLength="); });
    mParser.opt("hiddenLineOpacity", "<opacity>", "set hidden line opacity",
                [&](Arg& arg) { return setSingleFloat(arg, mOn, mHiddenLineOpacity, "hiddenLineOpacity="); });

    mParser.opt("show", "", "show info",
                [&](Arg& arg) { return arg.msg(show() + '\n'); });
    mParser.opt("showPixel", "", "show pixel info",
                [&](Arg& arg) { return arg.msg(showPixel() + '\n'); });

    mParser.opt("deltaPixelX", "<delta>", "delta move pixelX or pixelY",
                [&](Arg& arg) { return setDeltaPixArg(arg, mOn, mPixelX, mPixelXmin, mPixelXmax, "pixelX="); });
    mParser.opt("deltaPixelY", "<delta>", "delta move pixelY",
                [&](Arg& arg) { return setDeltaPixArg(arg, mOn, mPixelY, mPixelYmin, mPixelYmax, "pixelY="); });
    mParser.opt("deltaPixelSamples", "<delta>", "delta add pixel samples",
                [&](Arg& arg) { return setDeltaSampleArg(arg, mOn, mPixelSamples, "pixelSamples="); });
    mParser.opt("deltaLightSamples", "<delta>", "delta add light samples",
                [&](Arg& arg) { return setDeltaSampleArg(arg, mOn, mLightSamples, "lightSamples="); });
    mParser.opt("deltaBsdfSamples", "<delta>", "delta add bsdf samples",
                [&](Arg& arg) { return setDeltaSampleArg(arg, mOn, mBsdfSamples, "bsdfSamples="); });
    mParser.opt("deltaMaxDepth", "<delta>", "delta add max depth",
                [&](Arg& arg) { return setDeltaSampleArg(arg, mOn, mMaxDepth, "maxDepth="); });

    // ---- Included for backwards compatibility ----
    mParser.opt("toggleOcclusionRays", "", "toggle occlusionRays",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mDirectRaysOn, "occlusionRays="); });
    mParser.opt("toggleSpecularRays", "", "toggle specularRays",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mIndirectSpecularRaysOn, "specularRays="); });
    mParser.opt("toggleDiffuseRays", "", "toggle diffuseRays",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mIndirectDiffuseRaysOn, "diffuseRays="); });
    mParser.opt("toggleBsdfSamples", "", "toggle bsdfSamples",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mDirectDiffuseRaysOn, "bsdfSamples="); });
    // ----------------------------------------------

    mParser.opt("toggleUseSceneSamples", "", "toggle useSceneSamples",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mUseSceneSamples, "useSceneSamples="); });
    mParser.opt("toggleDirectRays", "", "toggle directRays",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mDirectRaysOn, "directRays="); });
    mParser.opt("toggleIndirectRays", "", "toggle indirectRays",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mIndirectRaysOn, "indirectRays="); });
    mParser.opt("toggleIndirectSpecularRays", "", "toggle indirect specularRays",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mIndirectSpecularRaysOn, "indirectSpecularRays="); });
    mParser.opt("toggleIndirectDiffuseRays", "", "toggle indirect diffuseRays",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mIndirectDiffuseRaysOn, "indirectDiffuseRays="); });
    mParser.opt("toggleDirectSpecularRays", "", "toggle direct specularRays",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mDirectSpecularRaysOn, "directSpecularRays="); });
    mParser.opt("toggleDirectDiffuseRays", "", "toggle direct diffuseRays",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mDirectDiffuseRaysOn, "directDiffuseRays="); });
    mParser.opt("toggleDirectLightRays", "", "toggle direct lightRays",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mDirectLightRaysOn, "directLightRays="); });
    mParser.opt("toggleDiffuseSamples", "", "toggle diffuseSamples",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mDiffuseSamplesOn, "diffuseSamples="); });
    mParser.opt("toggleSpecularSamples", "", "toggle specularSamples",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mSpecularSamplesOn, "specularSamples="); });
    mParser.opt("toggleLightSamples", "", "toggle lightSamples",
                [&](Arg& arg) { return setToggleArg(arg, mOn, mLightSamplesOn, "lightSamples="); });
}

//----------------------------------------------------------------------------------------------------------------------

PathVisualizer::PathVisualizer() 
    : mState(State::NONE), 
      mCameraIsectIndex(-1), 
      mWidth(0), 
      mHeight(0), 
      mParams(nullptr),
      mLightSampleRayCount(0),
      mBsdfSampleRayCount(0),
      mDiffuseRayCount(0),
      mSpecularRayCount(0)
{
    parserConfigure();
}

PathVisualizer::~PathVisualizer() {}

// static function
scene_rdl2::grid_util::VectorPacketLineStatus::RayType
PathVisualizer::flagsToRayType(const uint8_t& flags)
//
// This function is used when the backend computation transfers Path Visualizer information and needs to
// determine the rayType based on the line's attribute flags. The core issue here is that the flag
// representing the rayType in PathVisualizer is not exposed as a public enum. Therefore, this function
// currently assumes that the uint8_t flags argument represents the PathVisualizer::Flags and performs
// the conversion based on that assumption.
// Ultimately, the flags representing the RayType should be made public. This is one of the improvements
// planned for the next Path Visualizer update.
//
{
    return flagsToRayType(static_cast<Flags>(flags));
}

// static function
scene_rdl2::grid_util::VectorPacketLineStatus::RayType
PathVisualizer::flagsToRayType(const Flags& flags)
{
    using RayType = scene_rdl2::grid_util::VectorPacketLineStatus::RayType;

    if (isIndirectDiffuseRay(flags)) return RayType::DIFFUSE;
    if (isIndirectSpecularRay(flags)) return RayType::SPECULAR;
    if (isDirectDiffuseRay(flags)) return RayType::BSDF_SAMPLE;
    if (isDirectSpecularRay(flags)) return RayType::BSDF_SAMPLE;
    if (isDirectLightRay(flags)) return RayType::LIGHT_SAMPLE;
    if (isCameraRay(flags)) return RayType::CAMERA;
    if (matchesLineStatus(flags, Flags::INACTIVE)) return RayType::INACTIVE;
    
    return RayType::NONE;
}

scene_rdl2::math::Color
PathVisualizer::getColorByFlags(const uint8_t& flags) const
//
// This function is used when the backend computation transfers Path Visualizer information and needs to
// determine the color based on the line's attribute flags. The core issue here is that the flag
// representing the rayType in PathVisualizer is not exposed as a public enum. Therefore, this function
// currently assumes that the uint8_t flags argument represents the PathVisualizer::Flags and performs
// the conversion based on that assumption.
// Ultimately, the flags representing the RayType should be made public. This is one of the improvements
// planned for the next Path Visualizer update.
//
{
    return getRayColor(static_cast<Flags>(flags));
}

void PathVisualizer::initialize(const unsigned int width, const unsigned int height, 
                                const PathVisualizerParams* params) 
{
    MNRY_ASSERT(width != 0 && height != 0);

    mWidth = width;
    mHeight = height;
    mParams = params;
    mState = State::READY;
}

// -------------------------------------------- BUILDING ---------------------------------------------------------------

void PathVisualizer::reset()
{
    mNodes.clear();
    mVertexBuffer.clear();
}

bool PathVisualizer::setUpFrustum(const Camera& cam)
{
    if (!cam.hasFrustum()) { return false; }

    mFrustum = std::make_unique<mcrt_common::Frustum>();
    cam.computeFrustum(mFrustum.get(), 0, true);  // frustum at shutter open
    // Our points are in world space, so transform the clipping planes
    mFrustum->transformClipPlanes(cam.getCamera2World());

    return true;
}

void
PathVisualizer::recordCameraRay(const mcrt_common::Ray& ray, const Scene& scene, const uint32_t pixel)
{
    Flags flags;
    setOriginType(flags, Flags::CAMERA);
    recordRay(ray, scene, pixel, flags, /*occlusionFlag*/ false);
}

void
PathVisualizer::recordIndirectRay(const mcrt_common::Ray& ray, const Scene& scene, 
                                  const uint32_t pixel, const int lobeType)
{
    Flags flags;
    setRayType(flags, Flags::INDIRECT);

    if (lobeIsDiffuse(lobeType)) {
        setOriginType(flags, Flags::DIFFUSE);
        recordRay(ray, scene, pixel, flags, /*occlusionFlag*/ false);
    } else if (lobeIsSpecular(lobeType)){
        setOriginType(flags, Flags::SPECULAR);
        recordRay(ray, scene, pixel, flags, /*occlusionFlag*/ false);
    }
}

void
PathVisualizer::recordDirectBsdfRay(const mcrt_common::Ray& ray, const Scene& scene, const uint32_t pixel, 
                                    const int lobeType, const bool occlusionFlag)
{
    Flags flags;
    setRayType(flags, Flags::DIRECT);

    if (lobeIsDiffuse(lobeType)) {
        setOriginType(flags, Flags::DIFFUSE);
        recordRay(ray, scene, pixel, flags, occlusionFlag);
    } else if (lobeIsSpecular(lobeType)){
        setOriginType(flags, Flags::SPECULAR);
        recordRay(ray, scene, pixel, flags, occlusionFlag);
    }
}

void
PathVisualizer::recordDirectLightRay(const mcrt_common::Ray& ray, const Scene& scene, 
                                     const uint32_t pixel, const bool occlusionFlag)
{
    Flags flags;
    setRayType(flags, Flags::DIRECT);
    setOriginType(flags, Flags::LIGHT);
    recordRay(ray, scene, pixel, flags, occlusionFlag);
}

void
PathVisualizer::recordBsdfSample(const mcrt_common::Ray& ray, const Scene& scene, 
                                 const uint32_t pixel, const int lobeType)
{
    Flags flags;
    setRayType(flags, Flags::SAMPLE);
    
    if (lobeIsDiffuse(lobeType)) {
        setOriginType(flags, Flags::DIFFUSE);
        recordRay(ray, scene, pixel, flags, /*occlusionFlag*/ false);
    } else if (lobeIsSpecular(lobeType)){
        setOriginType(flags, Flags::SPECULAR);
        recordRay(ray, scene, pixel, flags, /*occlusionFlag*/ false);
    }
}

void
PathVisualizer::recordLightSample(const mcrt_common::Ray& ray, const Scene& scene, const uint32_t pixel)
{
    Flags flags;
    setRayType(flags, Flags::SAMPLE);
    setOriginType(flags, Flags::LIGHT);
    recordRay(ray, scene, pixel, flags, /*occlusionFlag*/ false);
}

void PathVisualizer::recordRay(const mcrt_common::Ray& ray, const Scene& scene, 
                               const uint32_t pixel, Flags flags, const bool occlusionFlag)
{
    MNRY_ASSERT(mState == State::RECORD);
    RenderTimer timer(mInRenderingTime);

    // Only record the ray if it matches the user parameters
    if (!matchesParams(flags, ray.getDepth())) {
        return;
    }

    // Calculate the pixel ID
    const uint32_t pixelID = uint32ToPixelY(pixel) * mWidth + uint32ToPixelX(pixel);

    // Calculate ray endpoints (render space)
    const float tfar = std::min(ray.tfar, mParams->mMaxRayLength);
    const Vec3f rayOrigin = ray.getOrigin();
    const Vec3f rayEnd = rayOrigin + scene_rdl2::math::normalize(ray.getDirection()) * tfar;

    // Compute world-space coordinates (bc we want these points to stay consistent even if the camera transform changes)
    const Vec3f rayOriginWorld = transformPoint(scene.getRender2World(), rayOrigin);
    const Vec3f rayEndWorld    = transformPoint(scene.getRender2World(), rayEnd);

    {
        std::lock_guard<std::mutex> lock(mWriteLock);
        // Add vertex to our vertex list (or find it, if it already exists) and return the index
        const int rayOriginIndex  = addVertex(rayOriginWorld);
        const int rayEndIndex     = addVertex(rayEndWorld);

        int rayIsectIndex = -1;
        if (occlusionFlag) { 
            // If a shadow ray is occluded, we want to find the first object it intersects
            const Vec3f isectPt = findSceneIsect(ray, scene);
            rayIsectIndex = addVertex(isectPt);
        }
        addNode(rayOriginIndex, rayEndIndex, rayIsectIndex, ray.getDepth(), flags);
    }
}

// ---------------------------------------------- FILTERING ------------------------------------------------------------

bool PathVisualizer::matchesParams(const Flags flags, const int depth) const
{
    bool recordRay = depth <= mParams->mMaxDepth;
    recordRay = recordRay && matchesFlags(flags);
    return recordRay;
}

// -------------------------------------- DRAWING ----------------------------------------------------------------------

bool PathVisualizer::pixelIsOccluded(const PixelCoordU& p, const PixelCoordI& p1,
                                     const pbr::Scene* scene, const float totalDistance,
                                     const float invDepth1, const float invDepthDiff) const
{
    // Find t value in pixel space
    const float distanceToPixel = scene_rdl2::math::distance(Vec2f(p.x, p.y), Vec2f(p1.x, p1.y));
    const float t = distanceToPixel / totalDistance;

    // Find depth of this point along the line using
    // rasterization-based depth interpolation
    const float depth = 1.f / (invDepth1 + t*invDepthDiff);

    /// TODO: There should be a way to do this more efficiently using an occlusion ray,
    /// by setting tfar to the depth above; however, this depth is in camera space, and
    /// the occlusion test happens in render space, so the depth somehow needs to be in render space

    // Cast ray to find the first scene intersection
    mcrt_common::RayDifferential ray;
    scene->getCamera()->createRay(&ray, p.x + 0.5f, p.y + 0.5f, 0.f, 0.5f, 0.5f, false);
    scene->getEmbreeAccelerator()->intersect(ray);

    if (isinf(ray.tfar)) {
        // if there is no scene intersection, then the pixel is not occluded
        return false;
    } else {
        // if there IS a scene intersection, find its depth and compare to the ray visualization's depth
        const Vec3f isect = ray.getOrigin() + ray.getDirection() * ray.tfar;
        const scene_rdl2::math::Vec4f isect4d(isect.x, isect.y, isect.z, 1.f);
        const float sceneDepth = -(float) dot(isect4d, scene->getCamera()->getRender2Camera().col2());

        return sceneDepth < depth;
    }
}

uint8_t PathVisualizer::clipPoints(const size_t nodeIndex, Vec3f* outPoints, bool* clipStatus) const
{
    uint8_t numPoints = 0;

    // world-space starting point
    outPoints[numPoints++] = getRayOrigin(nodeIndex);

    // If the ray is an occlusion ray that's failed the occlusion test,
    // we found the world-space closest intersection in recordRay()
    // We now need to convert it to screen space for drawing.
    const bool hasIsect = mNodes[nodeIndex].mRayIsectIndex != -1;
    if (hasIsect) { 
        outPoints[numPoints++] = getRayIsect(nodeIndex);
    }

    // world-space ending point
    outPoints[numPoints++] = getRayEndpoint(nodeIndex);

    // ----- Clip world-space points to viewing frustum -----
    auto clipLine = [&] (Vec3f& p1, Vec3f& p2) {
        Vec3f raySegmentClipped[2];
        const bool lineClipped = mFrustum->clipLine(p1, p2, raySegmentClipped[0], raySegmentClipped[1]);
        if (!lineClipped) {
            return false;
        }
        p1 = raySegmentClipped[0];
        p2 = raySegmentClipped[1];
        return true;
    };

    // ----- Clip the line(s) -----
    bool clippedLine = false;
    for (uint8_t i = 0; i < numPoints - 1; ++i) {
        const bool clippedCurrentLine = clipLine(outPoints[i], outPoints[i+1]);
        clippedLine = clippedLine || clippedCurrentLine;
    }

    /// If the isect and/or startpoint/endpoint have been clipped, flag it
    if (hasIsect) {
        clipStatus[0] = outPoints[0] != getRayOrigin(nodeIndex);
        clipStatus[1] = outPoints[1] != getRayIsect(nodeIndex);
        clipStatus[2] = outPoints[2] != getRayEndpoint(nodeIndex);
    } else {
        clipStatus[0] = outPoints[0] != getRayOrigin(nodeIndex);
        clipStatus[1] = outPoints[1] != getRayEndpoint(nodeIndex);
    }
   
    return clippedLine ? numPoints : 0;
}

void PathVisualizer::addLineSegment(const size_t nodeIndex,
                                    const PosType startPosType,
                                    const PosType endPosType,
                                    const PixelCoordU& start, const PixelCoordU& end, const Flags& flags, 
                                    const bool drawEndpoint, const bool isOccluded)
{
    std::lock_guard<std::mutex> lock(mWriteLock);

    // If the line has no length, exit the function without adding a new line segment
    if (start.x == end.x && start.y == end.y) {
        return;
    }
    float lineOpacity = isOccluded ? mParams->mHiddenLineOpacity : 1.f;
    mLines.push_back({nodeIndex, start, end, flags, drawEndpoint, lineOpacity, startPosType, endPosType});
}

void PathVisualizer::traceLine(const size_t nodeIndex,
                               const PosType startPosType,
                               const PosType endPosType,
                               const PixelCoordI& start, const PixelCoordI& end, 
                               const std::function<bool(const PixelCoordU&)>& isOccludedFunc,
                               const Flags flags, const bool endpointClipped)
{
    const PixelCoordI lineStart = start;
    const PixelCoordI lineEnd = end;

    /* Uses Wu's line drawing algorithm to trace the line until it finds where it's occluded.
     * Once it does, it creates a new line segment. It continues creating new segments
     * until the line is fully traced.
     */

    const uint32_t dx = abs(end.x - start.x);     // change in x
    const uint32_t dy = abs(end.y - start.y);     // change in y

    if (dx == 0 && dy == 0) { return; }

    int sx = start.x < end.x ? 1 : -1;      // step in the x-direction
    int sy = start.y < end.y ? 1 : -1;      // step in the y-direction

    bool steep = false;                      // is the slope > 1 ?
    int x0 = start.x;                        // starting point x
    int x1 = end.x;                          // ending point x
    int y0 = start.y;                        // starting point y
    int y1 = end.y;                          // ending point y

    // Certain values need to be doubles, since using a float leads to precision
    // issues, which causes the line to sometimes stop one pixel short
    double slope;                            // slope/gradient of line  

    // We always want to be drawing a line with
    // a gradual slope, so if it's a steep line, swap the x and y
    // coordinates so we can draw a line with slope < 1
    if (dy > dx) {
        steep = true;
        slope = (double) dx / dy;
        std::swap(x0, y0);
        std::swap(x1, y1);
        std::swap(sx, sy);
    } else {
        slope = (double) dy / dx; 
    }

    PixelCoordU segmentStart = {0,0};
    PixelCoordU segmentEnd = {0,0};
    bool isFirstSegment = true;
    bool prevIsOccluded = false;

    auto calcPosType = [&](const PixelCoordI& orig, const PixelCoordU& p, const PosType posType) -> PosType {
        if (orig.x == static_cast<int>(p.x) && orig.y == static_cast<int>(p.y)) return posType;
        return PosType::UNKNOWN;
    };

    double yIntersect = y0;
    // This is a gradually increasing line, so
    // we always increase in x, and conditionally increase in y
    for (int x = x0; x != x1+sx; x += sx, yIntersect += slope * sy) {
        // find the integer part of yIntersect
        // which is the new y coordinate
        const int y = (int) yIntersect;
        const PixelCoordI candidate = steep ? PixelCoordI{y, x} : PixelCoordI{x, y};

        if (!isInBounds(candidate)) {
            continue; 
        }

        const PixelCoordU out = {static_cast<uint32_t>(candidate.x), static_cast<uint32_t>(candidate.y)};

        if (isFirstSegment) {
            // when we find a pixel that's in bounds, set it as the start point
            segmentStart = out;
            isFirstSegment = false;
        }

        // update the endpoint until we reach the last in-bounds pixel
        segmentEnd = out;

        // if the previous pixel was occluded, but now isn't, OR
        // if the previous pixel was NOT occluded, but now is
        // end the current line segment, and start a new one
        const bool currIsOccluded = isOccludedFunc(out);
        if (currIsOccluded != prevIsOccluded) {
            // Check if the segment start/end point is the same as the line start/end point.
            // If it isn't, the point is interior on the line, or clipped by view frustum,
            // and its position type is UNKNOWN.
            const PosType sPosType = calcPosType(lineStart, segmentStart, startPosType);
            const PosType ePosType = calcPosType(lineEnd, segmentEnd, endPosType);

            addLineSegment(nodeIndex, sPosType, ePosType,
                           segmentStart, segmentEnd, flags, /* draw endpoint */ !endpointClipped, prevIsOccluded);
            prevIsOccluded = currIsOccluded;
            segmentStart = out;
        }
    }
    // add the last line segment
    const PosType sPosType = calcPosType(lineStart, segmentStart, startPosType);
    const PosType ePosType = calcPosType(lineEnd, segmentEnd, endPosType);
    addLineSegment(nodeIndex, sPosType, ePosType,
                   segmentStart, segmentEnd, flags, /* draw endpoint */ !endpointClipped, prevIsOccluded);
}

void PathVisualizer::generateLine(const size_t nodeIndex, const Scene* scene)
{
    MNRY_ASSERT(nodeIndex < mNodes.size());

    const pbr::Camera* cam = scene->getCamera();

    /// ---- Save the position of the pixel focus ----------------------
    if (isCameraRay(nodeIndex) && mCameraIsectIndex == -1) {
        mCameraIsectIndex = mNodes[nodeIndex].mRayEndpointIndex;
    }

     /// ---- Clip the world-space points using the viewing frustum ------
    // Can have 2-3 points per line:
    //  - rayOrigin
    //  - rayEndpoint
    //  - scene intersection (if occlusion ray)
    Vec3f clippedPoints[3];
    bool clipStatus[3] = {false, false, false};
    uint8_t numPoints = clipPoints(nodeIndex, clippedPoints, clipStatus);
    if (numPoints == 0) { return; }

    /// ---- Transform the points into screen-space ---------------------
    PixelCoordI pixelEndpoints[3];
    float inverseDepths[3];

    for (uint8_t i = 0; i < numPoints; ++i) {
        const Vec3f& clippedPoint = clippedPoints[i];

        // Transform the point to screen-space
        Vec2f p = transformPointWorld2Screen(clippedPoint, cam);
        pixelEndpoints[i] = {static_cast<int>(std::round(p.x)), static_cast<int>(std::round(p.y))};

        // Calculate the inverse depth
        /// NOTE: this assumes we're using a Perspective camera
        scene_rdl2::math::Vec4d clippedPoint4d(clippedPoint.x, clippedPoint.y, clippedPoint.z, 1.f);
        const float cpz = -(float) dot(clippedPoint4d, cam->getWorld2Camera().col2());
        inverseDepths[i] = 1.f / cpz;
    }

    // ---- Draw line(s) ----------------------------------------------
    for (uint8_t i = 0; i < numPoints - 1; ++i) {

        const float inverseDepthDiff = inverseDepths[i+1] - inverseDepths[i];
        const float distance = scene_rdl2::math::distance(Vec2f(pixelEndpoints[i].x, pixelEndpoints[i].y), 
                                                          Vec2f(pixelEndpoints[i+1].x, pixelEndpoints[i+1].y));
        auto isOccludedFunc = [&] (const PixelCoordU& p) {
            return pixelIsOccluded(p, pixelEndpoints[i], scene, distance, inverseDepths[i], inverseDepthDiff);
        };

        // if the line has been split, the second part is the "inactive"
        // part of an occlusion ray
        Flags lineFlags = mNodes[nodeIndex].mFlags;
        if (i > 0) {
            setLineStatus(lineFlags, Flags::INACTIVE);
        }

        PosType startPosType {PosType::UNKNOWN};
        PosType endPosType {PosType::UNKNOWN};
        if (numPoints == 2) {
            startPosType = PosType::START;
            endPosType = PosType::END;
        } else if (numPoints == 3) {
            if (i == 0) {
                startPosType = PosType::START;
                endPosType = PosType::ISECT;
            } else {
                startPosType = PosType::ISECT;
                endPosType = PosType::END;
            }
        }
        if (clipStatus[i]) startPosType = PosType::UNKNOWN;
        if (clipStatus[i+1]) endPosType = PosType::UNKNOWN; 

        traceLine(nodeIndex, startPosType, endPosType, pixelEndpoints[i], pixelEndpoints[i+1], 
                  isOccludedFunc, lineFlags, clipStatus[i+1]);
    }
}

void PathVisualizer::generateLines(const Scene* scene)
{
    mLines.clear();
    /// ---- Set up the camera frustum -------------------------------------
    if (!setUpFrustum(*scene->getCamera())) {
        std::cout << "Camera must have a valid frustum\n";
        return;
    }

    if (mNodes.size() == 0) {
        std::cout << "No rays match user parameters at (" << mParams->mPixelX << ", " << mParams->mPixelY << ")\n";
        return;
    }

    resetCameraIsectIndex();

    rndr::simpleLoop (/*parallel*/ true, 0u, (unsigned) mNodes.size(), [&](size_t nodeIndex) {
        generateLine(nodeIndex, scene);
    });
}

void PathVisualizer::draw(scene_rdl2::fb_util::RenderBuffer* renderBuffer, const Scene* scene)
{
    /// ---- Create a function that will write to the render buffer --------
    auto writeToRenderBuffer = [&] (const PixelCoordU& p, const scene_rdl2::math::Color& color, float a) {
        if (!isInBounds(p)) { return; }
        a = scene_rdl2::math::clamp(a, 0.f, 1.f);

        auto& renderColor = renderBuffer->getPixel(p.x, p.y);
        renderColor.x = a * color.r + (1.f - a) * renderColor.x;
        renderColor.y = a * color.g + (1.f - a) * renderColor.y;
        renderColor.z = a * color.b + (1.f - a) * renderColor.z;
        renderColor.w = 1;
    };

    /// ---- For each line, draw it on top of the render buffer ---------------
    using Color = scene_rdl2::math::Color;

    crawlAllLines([&](const Vec2i& px1,
                      const Vec2i& px2,
                      const uint8_t& flags,
                      const float alpha,
                      const float width,
                      const bool drawEndpoint,
                      const size_t nodeId,
                      const PosType startPosType,
                      const PosType endPosType) {
        scene_rdl2::math::Color c = getRayColor(static_cast<Flags>(flags));
        auto writeFunc = [&] (int px, int py, float a) {
            writeToRenderBuffer({static_cast<uint32_t>(px), static_cast<uint32_t>(py)}, c, alpha * a);
        };

        const unsigned uWidth = static_cast<unsigned>(width);
        drawing::drawLineWu(px1.x, px1.y, px2.x, px2.y, uWidth, writeFunc);
        
        if (drawEndpoint) {
            drawing::drawCircle(px2.x, px2.y, width + 2, writeFunc);
        }
    });
}

/// --------------------------------------------- Utilities ------------------------------------------------------------

Vec3f PathVisualizer::findSceneIsect(const mcrt_common::Ray& ray, const Scene& scene) const
{
    // Offset the pt to prevent self-intersection
    /// TODO: We are still getting many false negatives (i.e. finding 
    /// intersections very close to the ray origin). 
    /// Explore a more stable alternative.
    const Vec3f offsetPt = ray.getOrigin() + 0.0001f * ray.getDirection();
    mcrt_common::Ray testRay(offsetPt, ray.getDirection());
    scene.getEmbreeAccelerator()->intersect(testRay); 
    MNRY_ASSERT(testRay.geomID != RTC_INVALID_GEOMETRY_ID);

    // Compute world-space point
    const Vec3f isectPt = testRay.getOrigin() + testRay.tfar * testRay.getDirection();
    return transformPoint(scene.getRender2World(), isectPt);
}

Vec2f 
PathVisualizer::transformPointWorld2Screen(const Vec3f& p, const pbr::Camera* cam) const
{
    const scene_rdl2::math::Vec4d camP = transformPoint(cam->getWorld2Camera(), p);
    Vec3f result = mFrustum->projectToViewport(Vec3f(camP.x, camP.y, camP.z));
    result.x -= cam->getRegionToApertureOffsetX();
    result.y -= cam->getRegionToApertureOffsetY();
    return Vec2f(result.x, result.y);
}

/// -------- Getters ---------

inline bool PathVisualizer::matchesLobeType(const int lobeType1, const int lobeType2) const 
{
    return static_cast<uint8_t>(lobeType1) & static_cast<uint8_t>(lobeType2);
}

inline bool PathVisualizer::matchesFlags(const Flags flags) const
{
    if (isCameraRay(flags))             { return true; }
    if (isIndirectDiffuseRay(flags))    { return mParams->mIndirectRaysOn && mParams->mIndirectDiffuseRaysOn; }
    if (isIndirectSpecularRay(flags))   { return mParams->mIndirectRaysOn && mParams->mIndirectSpecularRaysOn; }
    if (isDirectDiffuseRay(flags))      { return mParams->mDirectRaysOn && mParams->mDirectDiffuseRaysOn; }
    if (isDirectSpecularRay(flags))     { return mParams->mDirectRaysOn && mParams->mDirectSpecularRaysOn; }
    if (isDirectLightRay(flags))        { return mParams->mDirectRaysOn && mParams->mDirectLightRaysOn; }
    if (isDiffuseSample(flags))         { return mParams->mSamplesOn && mParams->mDiffuseSamplesOn; }
    if (isSpecularSample(flags))        { return mParams->mSamplesOn && mParams->mSpecularSamplesOn; }
    if (isLightSample(flags))           { return mParams->mSamplesOn && mParams->mLightSamplesOn; }
}

inline bool PathVisualizer::lobeIsDiffuse(const int lobeType) const
{
    return matchesLobeType(lobeType, shading::BsdfLobe::DIFFUSE);
}

inline bool PathVisualizer::lobeIsSpecular(const int lobeType) const
{
    return matchesLobeType(lobeType, shading::BsdfLobe::GLOSSY) || matchesLobeType(lobeType, shading::BsdfLobe::MIRROR);
}

inline int PathVisualizer::getRayDepth(const size_t nodeIndex) const
{
    return mNodes[nodeIndex].mDepth;
}

inline bool PathVisualizer::isCameraRay(const size_t nodeIndex) const
{
    return matchesOriginType(mNodes[nodeIndex].mFlags, Flags::CAMERA);
}

inline Vec3f PathVisualizer::getRayOrigin(const size_t nodeIndex) const
{
    const int rayOriginIndex = mNodes[nodeIndex].mRayOriginIndex;

    MNRY_ASSERT(rayOriginIndex >= 0 && rayOriginIndex < mVertexBuffer.size());
    return mVertexBuffer[rayOriginIndex];
}

inline Vec3f PathVisualizer::getRayEndpoint(const size_t nodeIndex) const
{
    const int rayEndpointIndex = mNodes[nodeIndex].mRayEndpointIndex;

    MNRY_ASSERT(rayEndpointIndex >= 0 && rayEndpointIndex < mVertexBuffer.size());
    return mVertexBuffer[rayEndpointIndex];
}

inline Vec3f PathVisualizer::getRayIsect(const size_t nodeIndex) const
{
    const int rayIsectIndex = mNodes[nodeIndex].mRayIsectIndex;

    if (rayIsectIndex == -1) { return Vec3f(0.f); }

    MNRY_ASSERT(rayIsectIndex >= 0 && rayIsectIndex < mVertexBuffer.size());
    return mVertexBuffer[rayIsectIndex];
}

inline scene_rdl2::math::Color PathVisualizer::getRayColor(const Flags& flags) const
{
    // Must check if ray is inactive first, as that takes precedence over other flags.
    if (matchesLineStatus(flags, Flags::INACTIVE))  { return scene_rdl2::math::Color(0, 0, 0); }
    if (isCameraRay(flags))                         { return mParams->mCameraRayColor; }
    if (isIndirectDiffuseRay(flags))                { return mParams->mIndirectDiffuseRayColor; }
    if (isIndirectSpecularRay(flags))               { return mParams->mIndirectSpecularRayColor; }
    if (isDirectDiffuseRay(flags))                  { return mParams->mDirectDiffuseRayColor; }
    if (isDirectSpecularRay(flags))                 { return mParams->mDirectSpecularRayColor; }
    if (isDirectLightRay(flags))                    { return mParams->mDirectLightRayColor; }
    if (isDiffuseSample(flags))                     { return mParams->mIndirectDiffuseRayColor; }
    if (isSpecularSample(flags))                    { return mParams->mIndirectSpecularRayColor; }
    if (isLightSample(flags))                       { return mParams->mDirectLightRayColor; }
    return scene_rdl2::math::Color(1, 1, 1);
}

bool PathVisualizer::isCameraRay(const Flags flags)
{
    return PathVisualizer::matchesOriginType(flags, Flags::CAMERA);
}
bool PathVisualizer::isIndirectSpecularRay(const Flags flags)
{
    bool result = PathVisualizer::matchesOriginType(flags, Flags::SPECULAR);
    return result && PathVisualizer::matchesRayType(flags, Flags::INDIRECT);
}
bool PathVisualizer::isIndirectDiffuseRay(const Flags flags)
{
    bool result = PathVisualizer::matchesOriginType(flags, Flags::DIFFUSE);
    return result && PathVisualizer::matchesRayType(flags, Flags::INDIRECT);
}
bool PathVisualizer::isDirectSpecularRay(const Flags flags)
{
    bool result = PathVisualizer::matchesOriginType(flags, Flags::SPECULAR);
    return result && PathVisualizer::matchesRayType(flags, Flags::DIRECT);
}
bool PathVisualizer::isDirectDiffuseRay(const Flags flags)
{
    bool result = PathVisualizer::matchesOriginType(flags, Flags::DIFFUSE);
    return result && PathVisualizer::matchesRayType(flags, Flags::DIRECT);
}
bool PathVisualizer::isDirectLightRay(const Flags flags)
{
    bool result = PathVisualizer::matchesOriginType(flags, Flags::LIGHT);
    return result && PathVisualizer::matchesRayType(flags, Flags::DIRECT);
}
bool PathVisualizer::isDiffuseSample(const Flags flags)
{
    bool result = PathVisualizer::matchesOriginType(flags, Flags::DIFFUSE);
    return result && PathVisualizer::matchesRayType(flags, Flags::SAMPLE);
}
bool PathVisualizer::isSpecularSample(const Flags flags)
{
    bool result = PathVisualizer::matchesOriginType(flags, Flags::SPECULAR);
    return result && PathVisualizer::matchesRayType(flags, Flags::SAMPLE);
}
bool PathVisualizer::isLightSample(const Flags flags)
{
    bool result = PathVisualizer::matchesOriginType(flags, Flags::LIGHT);
    return result && PathVisualizer::matchesRayType(flags, Flags::SAMPLE);
}

/// -------- Setters --------

void PathVisualizer::setState(State state)
{
    std::lock_guard<std::mutex> lock(mWriteLock);
    mState = state;
}

inline int PathVisualizer::addVertex(const Vec3f& v)
{
    // Check the most recent vertex added. If it's the same, don't add again.
    // This is a minor optimization based on the assumption that rays added 
    // sequentially often share the same start/end points.
    if (mVertexBuffer.size() == 0 || !isEqual(v, mVertexBuffer.back())) {
        // If the search fails, add the vertex
        mVertexBuffer.push_back(v);
    }
    // Return the index of the vert position
    return mVertexBuffer.size() - 1;
}

inline void PathVisualizer::addNode(const int originIndex, const int endpointIndex, const int isectIndex, 
                                    const int depth, const Flags& flags)
{
    MNRY_ASSERT(originIndex >= 0 && originIndex < mVertexBuffer.size());
    MNRY_ASSERT(endpointIndex >= 0 && endpointIndex < mVertexBuffer.size());
    MNRY_ASSERT(isectIndex == -1 || (isectIndex >= 0 && isectIndex < mVertexBuffer.size()));

    mNodes.emplace_back(originIndex, endpointIndex, isectIndex, depth, flags);
}

/// -------- Statistics helpers --------

size_t PathVisualizer::getMemoryFootprint() const
{
    const size_t nodesSize = sizeof(mNodes) + sizeof(Node) * mNodes.size();
    const size_t vertsSize = sizeof(mVertexBuffer) + sizeof(Vec3f) * mVertexBuffer.size();

    const size_t totalSize = 
                    nodesSize                           + /* mNodes */
                    vertsSize                           + /* mVertexBuffer */
                    sizeof(PathVisualizerParams*)       + /* mParams */
                    sizeof(State)                       + /* mState */
                    sizeof(int) * 2                     + /* mWidth, mHeight */
                    sizeof(bool)                        + /* mNeedRenderRefresh */
                    sizeof(int)                         + /* mCameraIsectIndex */
                    sizeof(mcrt_common::Frustum)        + /* mFrustum */
                    sizeof(std::mutex)                  + /* mWriteLock */
                    sizeof(moonray::util::AverageDouble)+ /* mInRenderingTime */
                    sizeof(moonray::util::AverageDouble)  /* mPostRenderingTime */
                    ;

    return totalSize;
}

void PathVisualizer::printTimeStats() const
{
    const double inRenderTime = mInRenderingTime.getSum() / mcrt_common::getNumTBBThreads();
    const double postRenderTime = mPostRenderingTime.getSum() / mcrt_common::getNumTBBThreads();
    std::cout << "In Rendering Time / # threads: " << inRenderTime << " ms\n";
    std::cout << "Post-Rendering Time / # threads: " << postRenderTime << " ms\n";
    std::cout << "Combined Time: " << (inRenderTime + postRenderTime) << " ms\n";
}

std::string PathVisualizer::getNodeInfo(const size_t nodeIndex) const {
    std::string info;
    if (nodeIndex >= mNodes.size()) {
        info = "Node at " + std::to_string(nodeIndex) + " doesn't exist\n";
    } else {
        std::ostringstream ostr;
        ostr << "\n";
        ostr << "ID: " << nodeIndex << ",\n";
        ostr << "Origin: " << std::setprecision(2) << getRayOrigin(nodeIndex) << ",\n";
        ostr << "End: " << std::setprecision(2) << getRayEndpoint(nodeIndex) << ",\n";
        ostr << "Isect: " << std::setprecision(2) << getRayIsect(nodeIndex) << ",\n";
        ostr << "Depth: " << static_cast<int>(mNodes[nodeIndex].mDepth) << ",\n";
        ostr << "Flags: " << mNodes[nodeIndex].mFlags << "\n";
        info = ostr.str();
    }
    return info;
}

void PathVisualizer::printNode(const size_t nodeIndex) const {
    std::cout << getNodeInfo(nodeIndex);
}

void PathVisualizer::printNodes(const std::vector<size_t>& nodeList) const
{
    std::cout << "-------- Printing out nodes ---------\n";
    for (const size_t nodeIndex : nodeList) {
        printNode(nodeIndex);
    }
    std::cout << "\n";
}

void PathVisualizer::printNodes(const int maxEntries) const
{
    std::cout << "-------- Printing out nodes ---------\n";
    for (size_t i = 0; i < mNodes.size(); ++i) {
        if (maxEntries != -1 && static_cast<int>(i) >= maxEntries) { return; }
        printNode(static_cast<int>(i));
    }
    std::cout << "\n";
}

bool
PathVisualizer::getCamPos(scene_rdl2::math::Vec3f& camPos) const
{
    bool foundFlag = false;
    crawlAllNodes(Flags::CAMERA,
                  [&](const Node& node) {
                      foundFlag = true;
                      camPos = mVertexBuffer[node.mRayOriginIndex];
                      return false; // early exit
                  });
    return foundFlag;
}

std::vector<scene_rdl2::math::Vec3f>
PathVisualizer::getCamRayIsectSfPos() const
//
// Returns all the camera ray intersection points with the surface.
//
{
    std::vector<scene_rdl2::math::Vec3f> tbl;
    if (mNodes.empty()) return tbl;
    crawlAllNodes(Flags::CAMERA,
                  [&](const Node& node) {
                      tbl.push_back(mVertexBuffer[node.mRayEndpointIndex]);
                      return true;
                  });
    return tbl;
}

size_t
PathVisualizer::serializeNodeDataAll(std::string& buff) const
//
// Return non-zero data size even if node total and vtx total both are zero,
// because the data size is encoded.
//
{
    scene_rdl2::cache::ValueContainerEnqueue vce(&buff);

    /* for debug
    std::cerr << ">> PathVisualizer.cc serializeNodeDataAll() mNodes.size():" << mNodes.size()
              << " mVertexBuffer.size():" << mVertexBuffer.size() << '\n';
    */

    vce.enqVLSizeT(mNodes.size());
    if (mNodes.size()) {
        for (const auto& node : mNodes) {
            scene_rdl2::grid_util::VectorPacketNode vecPacketNode(node.mRayOriginIndex,
                                                                  node.mRayEndpointIndex,
                                                                  node.mRayIsectIndex,
                                                                  static_cast<int>(node.mDepth),
                                                                  flagsToRayType(node.mFlags));
            vecPacketNode.enq(vce);
        }
    }

    vce.enqVLSizeT(mVertexBuffer.size());
    if (mVertexBuffer.size()) {
        vce.enqVec3fVector(mVertexBuffer);
    }
    return vce.finalize();
}

void
PathVisualizer::parserConfigure()
{
    mParser.description("PathVisualizer command");

    mParser.opt("showFlowCtrlStatus", "", "show current pathVisualizer status related it's flow control",
                [&](Arg& arg) { return arg.msg(showFlowCtrlState() + '\n'); });
    mParser.opt("showLinesInfo", "", "show line data information",
                [&](Arg& arg) { return arg.msg(showLinesInfo() + '\n'); });
    mParser.opt("startSim", "", "start simulation phase and constructs PathVis dataBase",
                [&](Arg& arg) {
                    if (!mOn) return arg.msg("skip\n");
                    mState = State::READY;
                    reset();
                    mState = State::START_RECORD;
                    return arg.msg(showFlowCtrlState() + '\n');
                });
    mParser.opt("showNode", "", "show node information",
                [&](Arg& arg) { return arg.msg(showNodeInfo() + '\n'); });
    mParser.opt("showCamPos", "", "show camera position",
                [&](Arg& arg) { return arg.msg(showCamPos() + '\n'); });
    mParser.opt("showCamRayIsectSfPos", "", "show camera ray intersect surface positions",
                [&](Arg& arg) { return arg.msg(showCamRayIsectSfPos() + '\n'); });
}

std::string
PathVisualizer::showFlowCtrlState() const
{
    std::ostringstream ostr;
    ostr << "flowControlStatus {\n"
         << "  mState:" << mState << '\n'
         << "  mNeedRenderRefresh:" << scene_rdl2::str_util::boolStr(mNeedRenderRefresh) << '\n'
         << "}";
    return ostr.str();
}

std::string
PathVisualizer::showLinesInfo() const
{
    if (!mLines.size()) return "linesInfo (size:0)";

    auto showLineSegment = [](const LineSegment& line) {
        auto showPos = [](const unsigned v) {
            std::ostringstream ostr;
            ostr << std::setw(4) << v;
            return ostr.str();
        };
        auto showPosXY = [&](const unsigned x, const unsigned y) {
            return "(" + showPos(x) + ',' + showPos(y) + ")";
        };
        auto showCol = [](const float c) {
            std::ostringstream ostr;
            ostr << std::setw(5) << std::fixed << std::setprecision(3) << c;
            return ostr.str();
        };

        std::ostringstream ostr;
        ostr << "s" << showPosXY(line.mPx1.x, line.mPx1.y)
             << " e" << showPosXY(line.mPx2.x, line.mPx2.y)
             << " f:" << line.mFlags
             << " a:" << showCol(line.mAlpha)
             << " drawE:" << scene_rdl2::str_util::boolStr(line.mDrawEndpoint)
             << " nId:" << line.mNodeIndex
             << " sPos:" << scene_rdl2::grid_util::VectorPacketLineStatus::posTypeStr(line.mStartPosType)
             << " ePos:" << scene_rdl2::grid_util::VectorPacketLineStatus::posTypeStr(line.mEndPosType);
        return ostr.str();
    };

    const int wi = scene_rdl2::str_util::getNumberOfDigits(mLines.size());

    std::ostringstream ostr;
    ostr << "linesInfo (size:" << mLines.size() << ") mParams->mLineWidth:" << mParams->mLineWidth << " {\n";
    for (size_t i = 0; i < mLines.size(); ++i) {
        ostr << "  i:" << std::setw(wi) << i << ' ' << showLineSegment(mLines[i]) << '\n';
    }
    ostr << "}";
    return ostr.str();
}

std::string
PathVisualizer::showNodeInfo() const
{
    auto showPosId = [&](const int posId) {
        auto showPos = [](const scene_rdl2::math::Vec3f& v) {
            auto showF = [](const float v) {
                std::ostringstream ostr;
                ostr << std::setw(10) << std::fixed << std::setprecision(5) << v;
                return ostr.str();
            };
            std::ostringstream ostr;
            ostr << '(' << showF(v[0]) << ',' << showF(v[1]) << ',' << showF(v[2]) << ')';
            return ostr.str();
        };
        std::ostringstream ostr;
        const int w = scene_rdl2::str_util::getNumberOfDigits(mVertexBuffer.size());
        ostr << std::setw(w) << posId << ':' << showPos(mVertexBuffer[posId]);
        return ostr.str();
    };
    auto showNode = [&](const size_t nodeId) {
        auto showFlag = [&](const Flags& flag) {
            std::ostringstream ostr;
            ostr << "0x" << std::setw(2) << std::hex << static_cast<int>(flag) << ' ' << flag;
            return ostr.str();
        };
        std::ostringstream ostr;
        const int w = scene_rdl2::str_util::getNumberOfDigits(mNodes.size());
        const Node& node = mNodes[nodeId];
        ostr << "node:" << std::setw(w) << nodeId << " {\n"
             << scene_rdl2::str_util::addIndent("  mRayOriginIndex:" + showPosId(node.mRayOriginIndex)) << '\n'
             << scene_rdl2::str_util::addIndent("mRayEndpointIndex:" + showPosId(node.mRayEndpointIndex)) << '\n'
             << scene_rdl2::str_util::addIndent("   mRayIsectIndex:" + showPosId(node.mRayIsectIndex)) << '\n'
             <<                               "             mDepth:" << static_cast<int>(node.mDepth) << '\n'
             <<                               "             mFlags:" << showFlag(node.mFlags) << '\n'
             << "}";
        return ostr.str();
    };

    std::ostringstream ostr;
    ostr << "node info (size:" << mNodes.size() << ") {\n";
    for (size_t nodeId = 0; nodeId < mNodes.size(); ++nodeId) {
        ostr << scene_rdl2::str_util::addIndent(showNode(nodeId)) << '\n';
    }
    ostr << "}";
    return ostr.str();
}

std::string
PathVisualizer::showCamPos() const
{
    scene_rdl2::math::Vec3f camPos;
    const bool flag = getCamPos(camPos);

    std::ostringstream ostr;
    if (flag) ostr << "camPos:" << camPos;
    else      ostr << "camPos:empty"; 
    return ostr.str();
}

std::string
PathVisualizer::showCamRayIsectSfPos() const
{
    std::vector<scene_rdl2::math::Vec3f> tbl = getCamRayIsectSfPos();

    const int wi = scene_rdl2::str_util::getNumberOfDigits(tbl.size());
    std::ostringstream ostr;
    ostr << "camRayIsectSfPos (size:" << tbl.size() << ") {\n";
    for (size_t i = 0; i < tbl.size(); ++i) {
        ostr << "  i:" << std::setw(wi) << i << tbl[i] << '\n';
    }
    ostr << "}";
    return ostr.str();
}

// static function
std::string
PathVisualizer::stateStr(const State state)
{
    std::ostringstream ostr;
    ostr << state;
    return ostr.str();
}

} // end namespace rndr
} // end namespace moonray

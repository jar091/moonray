// Copyright 2025 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0
#include "McrtVecSender.h"

#include <moonray/rendering/rndr/PathVisualizerManager.h>
#include <moonray/rendering/rndr/RenderContext.h>

#include <scene_rdl2/common/grid_util/PathVisSimGlobalInfo.h>
#include <scene_rdl2/common/grid_util/ProgressiveFrameBufferName.h>
#include <scene_rdl2/common/grid_util/VectorPacketDictionary.h>
#include <scene_rdl2/render/util/StrUtil.h>

#include <cmath> // sqrtf
#include <limits.h> // HOST_NAME_MAX
#ifdef _WIN32
#include <windows.h> // GetComputerNameA()
#else
#include <unistd.h> // gethostname()
#endif

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

namespace {

unsigned char
f2uc(const float f)
{
    if (f <= 0.0f) return 0;
    else if (f >= 1.0f) return 255;
    return static_cast<unsigned char>(f * 256.0f);
}

std::string
getHostName()
{
#ifdef _WIN32
    // gethostname() is a Winsock call and would drag WSAStartup() in; the Win32
    // computer name is the same machine identifier and needs no initialization.
    char hostname[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD hostnameSize = static_cast<DWORD>(sizeof(hostname));
    if (!::GetComputerNameA(hostname, &hostnameSize)) return "unknown";
#else
    char hostname[HOST_NAME_MAX + 1];
    if (::gethostname(hostname, sizeof(hostname)) == -1) return "unknown";
#endif
    return std::string(hostname);
}

} // namespace

namespace moonray {
namespace engine_tool {

unsigned
McrtVecSenderTestData::calcWinDivCount(const unsigned machineTotal) const
//
// Calculate the best window divide count for the multi-machine total count
//
{
    unsigned winDivCount = static_cast<unsigned>(sqrtf(static_cast<float>(machineTotal)));
    while (true) {
        if (winDivCount * winDivCount >= machineTotal) break;
        winDivCount++;
    }
    return winDivCount;
}

void
McrtVecSenderTestData::calcSubdivWindowPos(const unsigned width,
                                           const unsigned height,
                                           const unsigned winDivCount,
                                           const unsigned subWinId,
                                           const float activeRatioXMin, // ratio 0.0 ~ 1.0
                                           const float activeRatioXMax,
                                           const float activeRatioYMin,
                                           const float activeRatioYMax,
                                           Vec2ui& subWinMin,
                                           Vec2ui& subWinMax) const
//
// Calculate the sub-windows position (min, max) based on the winDivCount and subWinId.
// The window is subdivided N x N if winDivCount value is N. And the subdivided sub-window
// has a subWinId that starts from 0 (the most left down subWindow).
// For example, subWinId looks as follows if N = 3.
//
// 3 x 3 subWindow subdivision.
//       +---+---+---+
//       | 6 | 7 | 8 |
//       +---+---+---+
//       | 3 | 4 | 5 |
//       +---+---+---+
//       | 0 | 1 | 2 |
//       +---+---+---+
//
// In this case, SubWinId shows only from 0 to 8, but SubWinId wraps around, and you can
// use more than 8. So, SubWinId = YourDefinedId % TotalNumberOfSubWin.
//
//       +---+---+---+              +---+---+---+              +---+---+---+
//       | 6 | 7 | 8 |              | 15| 16| 17|              | 24| 25| 26|
//       +---+---+---+              +---+---+---+              +---+---+---+
//       | 3 | 4 | 5 |    . . . >   | 12| 13| 14|    . . . >   | 21| 22| 23|   . . . >
//       +---+---+---+              +---+---+---+              +---+---+---+
//       | 0 | 1 | 2 |              |  9| 10| 11|              | 18| 19| 20|
//       +---+---+---+              +---+---+---+              +---+---+---+
//
{
#ifdef DEBUG_MSG
    auto showResult = [&]() {
        std::ostringstream ostr;
        ostr << ">> McrtVecSender.cc McrtVecSenderTestData::calcSubdivWindowPos() {\n"
             << "  width:" << width << '\n'
             << "  height:" << height << '\n'
             << "  winDivCount:" << winDivCount
             << "  subWinId:" << subWinId
             << "  activeRatioX:(" << activeRatioXMin << ',' << activeRatioXMax << ")\n"
             << "  activeRatioY:(" << activeRatioYMin << ',' << activeRatioYMax << ")\n"
             << "  subWinMin:(" << subWinMin[0] << ',' << subWinMin[1] << ")\n"
             << "  subWinMax:(" << subWinMax[0] << ',' << subWinMax[1] << ")\n"
             << "}";
    };
#endif // end of DEBUG_MSG

    const Vec2f wMin(static_cast<float>(width) * activeRatioXMin, static_cast<float>(height) * activeRatioYMin);
    const Vec2f wMax(static_cast<float>(width) * activeRatioXMax, static_cast<float>(height) * activeRatioYMax);
    if (winDivCount == 1) {
        subWinMin = Vec2ui(wMin);
        subWinMax = Vec2ui(wMax) - Vec2ui(1);
#ifdef DEBUG_MSG
        std::cerr << "winDivCount == 1 " << showResult() << '\n';
#endif // end of DEBUG_MSG
        return;
    }

    const unsigned currSubWinId = subWinId % (winDivCount * winDivCount);
    const Vec2f id(static_cast<float>(currSubWinId % winDivCount), static_cast<float>(currSubWinId / winDivCount));
    const Vec2f wSize = wMax - wMin;
    const Vec2f subWinSize = wSize / static_cast<float>(winDivCount);
    const Vec2f subWinMinf = subWinSize * id + wMin;
    const Vec2f subWinMaxf = subWinMinf + subWinSize;

    subWinMin = Vec2ui(subWinMinf);
    subWinMax = Vec2ui(subWinMaxf) - Vec2ui(1);
#ifdef DEBUG_MSG
    std::cerr << "winDivCount > 1 " << showResult() << '\n';
#endif // end of DEBUG_MSG
}

void
McrtVecSenderTestData::genTestPatternA(const Vec2ui& winMin,
                                       const Vec2ui& winMax,
                                       const unsigned segmentTotal,
                                       const float angleStart,
                                       const float angleEnd,
                                       const float lenScale,
                                       const float widthMax,
                                       const bool constLen,
                                       const bool constWidth,
                                       const DrawLineFunc& drawLineFunc)
//
// Draw spinner test pattern based on the mDrawCounter value.
// mDrawCounter is automatically incremented if this function is called.
//
// angleStart, angleEnd : Top is 0 degree and angle increases CCW.
//
{
    auto degToRad = [](const float deg) { return deg / 180.0f * 3.14159265358979f; };

    const Vec2f winSize = Vec2f(winMax) -Vec2f( winMin) + Vec2f(1.0f);
    const Vec2f center = (Vec2f(winMax) - Vec2f(winMin)) / 2.0f + Vec2f(winMin);
    const float len = std::min(winSize[0], winSize[1]) * lenScale / 2.0f;

    const float angleDelta = angleEnd - angleStart;
    for (int segId = segmentTotal - 1; segId >= 0; --segId) {
        const int angleId = (segId + mDrawCounter) % segmentTotal;
        const float angleRatio = 1.0f - static_cast<float>(angleId) / static_cast<float>(segmentTotal);
        const float alphaRatio = static_cast<float>(segId) / static_cast<float>(segmentTotal - 1);
        const float widthRatio = (constWidth) ? 1.0f : alphaRatio;

        const unsigned char a = static_cast<unsigned char>(alphaRatio * 255.0f);
        const Vec3uc c = (segId == segmentTotal - 1) ? Vec3uc(255, 0, 0) : Vec3uc(255, 255, 255);
        const float w = widthMax * widthRatio;

        const float rad = degToRad(angleRatio * angleDelta + angleStart);
        const Vec2f dir(-1.0f * sin(rad), cos(rad));
        if (constLen) {
            drawLineFunc(center, len * dir + center, w, c, a);
        } else {
            const Vec2f mid = len * 0.5f * dir + center;
            const Vec2f delta = (len * 0.5f * widthRatio) * dir;
            drawLineFunc(mid - delta, mid + delta, w, c, a);
        }
    }
    mDrawCounter++;
}

void
McrtVecSenderTestData::genTestPatternB(const Vec2ui& winMin,
                                       const Vec2ui& winMax,
                                       const unsigned winDivCount,
                                       const DrawBoxFunc& drawBoxFunc)
//
// Draw a fade-out outline box pattern based on the mDrawCounter.
// mDrawCounter is automatically incremented if this function is called.
//
{
    const unsigned width = winMax[0] - winMin[0] + 1;
    const unsigned height = winMax[1] - winMin[1] + 1;
    const unsigned subWinTotal = winDivCount * winDivCount;

    auto calcRatio = [&](const unsigned id) {
        if (id == 0) return 1.0f;
        else return static_cast<float>(id) / static_cast<float>(subWinTotal);
    };

    for (unsigned subWinId = 0; subWinId < subWinTotal; ++subWinId) {
        const unsigned currSubWinId = (subWinId + mDrawCounter) % subWinTotal;

        Vec2ui subWinMin, subWinMax;
        calcSubdivWindowPos(width,
                            height,
                            winDivCount,
                            currSubWinId,
                            0.0f, 1.0f,
                            0.0f, 1.0f,
                            subWinMin,
                            subWinMax);
        subWinMin += winMin;
        subWinMax += winMin;

        const Vec2ui subWinSize = subWinMax - subWinMin + Vec2ui(1);
        if (subWinSize[0] >= 5 && subWinSize[1] >= 5) {
            subWinMin += Vec2ui(2);
            subWinMax -= Vec2ui(2);
        }

        drawBoxFunc(subWinMin, subWinMax,
                    (subWinId == 0) ? Vec3uc(255, 0, 0) : Vec3uc(255, 255, 255),
                    static_cast<unsigned char>(calcRatio(subWinId) * 255.0f));
    }
    mDrawCounter++;
}

//------------------------------------------------------------------------------------------

void
McrtVecSenderTestData1::drawingLines(const unsigned width,
                                     const unsigned height,
                                     const DrawLineFunc& drawLineFunc)
{
    Vec2ui subWinMin, subWinMax;
    calcSubdivWindowPos(width,
                        height,
                        calcWinDivCount(mMachineTotal),
                        mMachineId,
                        mActiveRatioXMin, mActiveRatioXMax,
                        mActiveRatioYMin, mActiveRatioYMax,
                        subWinMin,
                        subWinMax);

    genTestPatternA(subWinMin, subWinMax,
                    mSegmentTotal,
                    0.0f, 360.0f,
                    0.9f,
                    mWidthMax, /* constLen = */ true, /* constWidth = */ false,
                    drawLineFunc);
}

std::string
McrtVecSenderTestData1::show() const
{
    std::ostringstream ostr;
    ostr << "McrtVecSenderTestData1 {\n"
         << "  mMachineTotal:" << mMachineTotal << '\n'
         << "  mMachineId:" << mMachineId << '\n'
         << "  mActiveRatioXMin:" << mActiveRatioXMin << '\n'
         << "  mActiveRatioXMax:" << mActiveRatioXMax << '\n'
         << "  mActiveRatioYMin:" << mActiveRatioYMin << '\n'
         << "  mActiveRatioYMax:" << mActiveRatioYMax << '\n'
         << "  mSegmentTotal:" << mSegmentTotal << '\n'
         << "  mWidthMax:" << mWidthMax << '\n'
         << "}";
    return ostr.str();
}

void
McrtVecSenderTestData1::parserConfigure()
{
    mParser.description("vecSender testData1 command");

    mParser.opt("show", "", "show all", [&](Arg& arg) { return arg.msg(show() + '\n'); });
    mParser.opt("segmentTotal", "<n|show>", "set segmentTotal",
                [&](Arg& arg) {
                    if (arg() == "show") arg++;
                    else mSegmentTotal = (arg++).as<unsigned>(0);
                    return arg.msg(std::to_string(mSegmentTotal) + '\n');
                });
    mParser.opt("widthMax", "<w|show>", "set width max (float)",
                [&](Arg& arg) {
                    if (arg() == "show") arg++;
                    else mWidthMax = (arg++).as<float>(0);
                    return arg.msg(std::to_string(mWidthMax) + '\n');
                });
}

//-----------------------------------------------------------------------------------------

void
McrtVecSenderTestData2::drawingBoxOutline(const unsigned width,
                                          const unsigned height,
                                          const DrawBoxFunc& drawBoxFunc)
{
    Vec2ui winMin, winMax;
    calcSubdivWindowPos(width,
                        height,
                        calcWinDivCount(mMachineTotal),
                        mMachineId,
                        mActiveRatioXMin, mActiveRatioXMax,
                        mActiveRatioYMin, mActiveRatioYMax,
                        winMin,
                        winMax);

    genTestPatternB(winMin, winMax, mWinDivCount, drawBoxFunc);
}

std::string
McrtVecSenderTestData2::show() const
{
    std::ostringstream ostr;
    ostr << "McrtVecSenderTestData2 {\n"
         << "  mMachineTotal:" << mMachineTotal << '\n'
         << "  mMachineId:" << mMachineId << '\n'
         << "  mWinDivCount:" << mWinDivCount << '\n'
         << "}";
    return ostr.str();
}

void
McrtVecSenderTestData2::parserConfigure()
{
    mParser.description("vecSender testData2 command");

    mParser.opt("show", "", "show all", [&](Arg& arg) { return arg.msg(show() + '\n'); });
    mParser.opt("winDiv", "<n|show>", "set window divide count",
                [&](Arg& arg) {
                    if (arg() == "show") arg++;
                    else mWinDivCount = (arg++).as<unsigned>(0);
                    return arg.msg(std::to_string(mWinDivCount) + '\n');
                });
    mParser.opt("activeRatio", "<xMin> <yMin> <xMax> <yMax>", "set active ratio",
                [&](Arg& arg) {
                    mActiveRatioXMin = (arg++).as<float>(0);
                    mActiveRatioYMin = (arg++).as<float>(0);
                    mActiveRatioXMax = (arg++).as<float>(0);
                    mActiveRatioYMax = (arg++).as<float>(0);
                    return true;
                });
}

//-----------------------------------------------------------------------------------------

McrtVecSender::McrtVecSender()
{
    setup();

    parserConfigure();
}

size_t
McrtVecSender::snapshotDrawing(const unsigned width,
                               const unsigned height,
                               const unsigned renderCounter,
                               const unsigned snapshotId, 
                               const rndr::RenderContext& renderContext,
                               const bool needToSendSimGlobalInfo)
//
// renderCounter: This counter indicates the number of times RenderContext::startFrame() has been executed
//                since the process started. It starts at 0 immediately after process launch and is
//                incremented each time startFrame() completes. It never resets to 0 during the lifetime of
//                the process.
//
// snapshotId: This value is reset to 0 each time RenderContext::startFrame() is executed, and it is
//             incremented immediately after snapshotDelta is executed to transfer data downstream.
//             Not used at this moment, might be useful for the future.
//
{
    mBuff.clear();
    mVecPacketEnqueue->reset(*(mVecPacketHeader.get()));
    if (mRenderCounterLastSnapshot < renderCounter) {
        //
        // A new rendering frame is started. 
        //
        mForcedOutputCol = true;
        mForcedOutputWidth = true;
    }

    size_t size = 0;
    if (mTestMode) {
        size = snapshotTestDrawing(width, height);
    } else {
        size = snapshotPathVisualizerEnable(width, height, renderCounter, renderContext, needToSendSimGlobalInfo);
    }

    mRenderCounterLastSnapshot = renderCounter; // update counter
    return size;
}

size_t
McrtVecSender::snapshotPathVisualizerDisable()
{
    using namespace scene_rdl2::grid_util;

    mBuff.clear();
    mVecPacketEnqueue->reset(*(mVecPacketHeader.get()));
    mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntryPathVis(false));
    return mVecPacketEnqueue->finalize();
}

void
McrtVecSender::addVecPacketToProgressiveFrame(const MessageAddBuffFunc& func) const
{
    if (!mBuff.size()) return; // skip

    std::ostringstream ostr;
    ostr << scene_rdl2::grid_util::ProgressiveFrameBufferName::VecPacket << mMachineId;
    std::string buffName = ostr.str();

    func(makeSharedPtr(duplicateVecPacket()), mBuff.size(), buffName.c_str());
}

bool
McrtVecSender::compareDictPathVisActive(const bool flag) const
//
// This function searches the dictionary inside the current VecPack data and checks whether the stored
// PathVisualizer mode on/off value matches the specified flag.
// First, it verifies whether the dictionary entry for the PathVisualizer mode on/off value has been
// set. If no value has been set yet, the function returns false - this corresponds to the state
// immediately after the VecPacket data has been initialized.
// If a value has already been set (i.e., the PathVisualizer mode is explicitly set to either on or
// off), the function then checks whether that stored value matches the flag specified by the argument.
//
{
    using namespace scene_rdl2::grid_util;

    const VectorPacketDictionary& dict = mVecPacketEnqueue->getDictionary();
    const VectorPacketDictEntryPathVis& entry =
        static_cast<const VectorPacketDictEntryPathVis&>(dict.getDictEntry(VectorPacketDictEntry::Key::PATH_VIS));

    if (!entry.getActive()) return false; // not set yet.
    return entry.getPathVis() == flag;
}

std::string
McrtVecSender::show() const
{
    std::ostringstream ostr;
    ostr << "McrtVecSender {\n"
         << "  mTestMode:" << scene_rdl2::str_util::boolStr(mTestMode) << '\n'
         << "  mTestType:" << mTestType << '\n'
         << scene_rdl2::str_util::addIndent(mTestData1.show()) << '\n'
         << scene_rdl2::str_util::addIndent(mTestData2.show()) << '\n'
         << scene_rdl2::str_util::addIndent(showBuff()) << '\n'
         << scene_rdl2::str_util::addIndent(showVecPacketHeader()) << '\n'
         << scene_rdl2::str_util::addIndent(showVecPacketEnqueue()) << '\n'
         << "}";
    return ostr.str();
}

//------------------------------------------------------------------------------------------

void
McrtVecSender::setup()
{
    mVecPacketHeader = std::make_unique<scene_rdl2::grid_util::VectorPacketHeader>(100); // version = 1.00
    mVecPacketEnqueue = std::make_unique<scene_rdl2::grid_util::VectorPacketEnqueue>(&mBuff, *(mVecPacketHeader.get()));
}

size_t
McrtVecSender::snapshotTestDrawing(const unsigned width, const unsigned height)
//
// Test pattern drawing
//
{
    auto drawLineFunc = [&](const Vec2f& s,
                            const Vec2f& e,
                            const float w,
                            const Vec3uc& c,
                            const unsigned char a) {
        enqCol(c, a);
        enqWidth16(w);
        mVecPacketEnqueue->enqLine2D(Vec2ui(s), Vec2ui(e),
                                     scene_rdl2::grid_util::VectorPacketLineStatus(),
                                     0);
    };
    auto drawBoxFunc = [&](const Vec2ui& min,
                           const Vec2ui& max,
                           const Vec3uc& c,
                           const unsigned char a) {
        enqCol(c, a);
        mVecPacketEnqueue->enqBoxOutline2D(min, max);
    };

    switch (mTestType) {
    case 0 : snapshotTestDrawingLinesSimple(drawLineFunc); break;
    case 1 : mTestData1.drawingLines(width, height, drawLineFunc); break;
    case 2 : mTestData2.drawingBoxOutline(width, height, drawBoxFunc); break;
    default :
        break;
    }

    return mVecPacketEnqueue->finalize();
}

void
McrtVecSender::snapshotTestDrawingLinesSimple(const DrawLineFunc& drawLineFunc)
{
    //
    // single line dummy data
    //
    mVecPacketEnqueue->enqDictEntry(scene_rdl2::grid_util::VectorPacketDictEntryRenderCounter(987));
    mVecPacketEnqueue->enqDictEntry(scene_rdl2::grid_util::VectorPacketDictEntryPixPos(Vec2ui(456, 789)));
    mVecPacketEnqueue->enqDictEntry(scene_rdl2::grid_util::VectorPacketDictEntryHostname(getHostName()));
    drawLineFunc(Vec2f(100.0f, 110.0f), Vec2f(200.0f, 210.0f), 1.0f, Vec3uc(255, 255, 0), 255);
}

size_t
McrtVecSender::snapshotPathVisualizerEnable(const unsigned width,
                                            const unsigned height,
                                            const unsigned renderCounter,
                                            const rndr::RenderContext& renderContext,
                                            const bool needToSendSimGlobalInfo)
//
// Encode PathVisualizer drawing segment and internal parameters
//
{
    if (needToSendSimGlobalInfo) {
        snapshotPathVisualizerSimGlobalInfo(renderCounter, renderContext);
        snapshotPathVisualizerNodeData(renderContext);
    }

    rndr::PathVisualizerManager* visMgrObsrPtr = renderContext.getPathVisualizerManager().get();
    if (!visMgrObsrPtr->getTotalLines()) { // empty drawing
        if (needToSendSimGlobalInfo) {
            // already vecPacket data was staged. We should do finalize
            return mVecPacketEnqueue->finalize();
        }
        return 0;
    }

    using PosType = scene_rdl2::grid_util::VectorPacketLineStatus::PosType;
    using RayType = scene_rdl2::grid_util::VectorPacketLineStatus::RayType;

    auto calcStat = [&](const bool drawEndPoint,
                        const uint8_t flag,
                        const PosType sPosType,
                        const PosType ePosType) -> scene_rdl2::grid_util::VectorPacketLineStatus {
        RayType rayType = rndr::PathVisualizerManager::flagsToRayType(flag);
        return scene_rdl2::grid_util::VectorPacketLineStatus(rayType, drawEndPoint, sPosType, ePosType);
    };

    using Vec2i = scene_rdl2::math::Vec2i;
    using Color = scene_rdl2::math::Color;
    const float wf = static_cast<float>(width);
    const float hf = static_cast<float>(height);

    visMgrObsrPtr->crawlAllLines([&](const Vec2i& s,
                                     const Vec2i& e,
                                     const uint8_t& flags,
                                     const float a,
                                     const float w,
                                     const bool drawEndPoint,
                                     const unsigned nodeId,
                                     const PosType startPosType,
                                     const PosType endPosType) {
        Vec2f posA(s[0] + 0.5f, s[1] + 0.5f);
        Vec2f posB(e[0] + 0.5f, e[1] + 0.5f);
        if (WinClip::clipLine(posA, posB, wf, hf)) {
            //
            // The current communication protocol only uses the unsigned value for the position
            // information. Unsigned value variable length coding would be better than signed one.
            // (See ValueContainer’s VL encode/decode logic).
            // We have to clip the line segment by the image window and make sure the position is
            // 0 or positive.
            //
            const Color c = visMgrObsrPtr->getColorByFlags(flags);
            enqCol(Vec3uc(f2uc(c.r), f2uc(c.g), f2uc(c.b)), f2uc(a));
            enqWidth16(w);

            mVecPacketEnqueue->enqLine2D(WinClip::vec2fToVec2ui(posA, wf, hf),
                                         WinClip::vec2fToVec2ui(posB, wf, hf),
                                         calcStat(drawEndPoint, flags, startPosType, endPosType),
                                         nodeId);
        }
    });

    return mVecPacketEnqueue->finalize();
}

void
McrtVecSender::snapshotPathVisualizerSimGlobalInfo(const unsigned renderCounter,
                                                   const rndr::RenderContext& renderContext)
{
    using namespace scene_rdl2::grid_util;

    rndr::PathVisualizerManager* visMgrObsrPtr = renderContext.getPathVisualizerManager().get();

    scene_rdl2::grid_util::PathVisSimGlobalInfo globalInfo;
    visMgrObsrPtr->setupSimGlobalInfo(globalInfo);

    mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntryRenderCounter(renderCounter));
    mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntryHostname(getHostName()));

    bool pathVisActive = globalInfo.getPathVisActive();
    mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntryPathVis(pathVisActive));

    if (!pathVisActive) {
        // We skip the PathVisualizer detailed info when PathVisualizer is disabled.
        return;
    }
    
    //
    // Dictionary information
    //
    mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntryPixPos(globalInfo.getPixelPos()));
    mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntryMaxDepth(globalInfo.getMaxDepth()));
    mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntrySamples(globalInfo.getPixelSamples(),
                                                                 globalInfo.getLightSamples(),
                                                                 globalInfo.getBsdfSamples()));
    mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntryRayTypeSelection(globalInfo.getUseSceneSamples(),
                                                                          globalInfo.getOcclusionRaysOn(),
                                                                          globalInfo.getSpecularRaysOn(),
                                                                          globalInfo.getDiffuseRaysOn(),
                                                                          globalInfo.getBsdfSamplesOn(),
                                                                          globalInfo.getLightSamplesOn()));
    mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntryColor(globalInfo.getCameraRayColor(),
                                                               globalInfo.getSpecularRayColor(),
                                                               globalInfo.getDiffuseRayColor(),
                                                               globalInfo.getBsdfSampleColor(),
                                                               globalInfo.getLightSampleColor()));
    mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntryLineWidth(globalInfo.getLineWidth()));

    scene_rdl2::math::Vec3f camPos;
    const bool camPosFlag = visMgrObsrPtr->getCamPos(camPos);
    if (camPosFlag) {
        mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntryCamPos(camPos));
    }

    // We compute the camera intersect surface position and send it to the Client,
    // but this is not used at this moment yet.
    mVecPacketEnqueue->enqDictEntry(VectorPacketDictEntryCamRayIsectSfPos(visMgrObsrPtr->getCamRayIsectSfPos()));
}

void
McrtVecSender::snapshotPathVisualizerNodeData(const rndr::RenderContext& renderContext)
{
    rndr::PathVisualizerManager* visMgrObsrPtr = renderContext.getPathVisualizerManager().get();

    //
    // dataSize should be non-zero data even if node total and vtx total both are zero,
    // because the data size is encoded.
    //
    std::string data;
    const size_t dataSize = visMgrObsrPtr->serializeNodeDataAll(data);
    mVecPacketEnqueue->enqNodeDataAll(data);

    // for debug
    // std::cerr << ">> McrtVecSender.cc snapshotPathVisualizerNodeData() dataSize:" << dataSize << "byte\n";
}

std::string
McrtVecSender::showBuff() const
{
    std::ostringstream ostr;
    ostr << "mBuff:0x" << std::hex << reinterpret_cast<uintptr_t>(&mBuff[0]) << std::dec
         << " size:" << mBuff.size();
    return ostr.str();
}

std::string
McrtVecSender::showVecPacketHeader() const
{
    if (!mVecPacketHeader) return "empty";
    std::ostringstream ostr;
    ostr << "mVecPacketHeader {\n"
         << scene_rdl2::str_util::addIndent(mVecPacketHeader->show()) << '\n'
         << "}";
    return ostr.str();
}

std::string
McrtVecSender::showVecPacketEnqueue() const
{
    if (!mVecPacketEnqueue) return "empty";
    std::ostringstream ostr;
    ostr << "mVecPacketEnqueue {\n"
         << scene_rdl2::str_util::addIndent(mVecPacketEnqueue->show()) << '\n'
         << "}";
    return ostr.str();
}

void
McrtVecSender::parserConfigure()
{
    mParser.description("McrtVecSender command");

    mParser.opt("show", "", "show all info",
                [&](Arg& arg) { return arg.msg(show() + '\n'); });
    mParser.opt("testMode", "<on|off|show>", "set testMode",
                [&](Arg& arg) {
                    if (arg() == "show") arg++;
                    else mTestMode = (arg++).as<bool>(0);
                    return arg.msg(scene_rdl2::str_util::boolStr(mTestMode) + '\n');
                });
    mParser.opt("testType", "<id|show>", "set testType id",
                [&](Arg& arg) {
                    if (arg() == "show") arg++;
                    else mTestType = (arg++).as<unsigned>(0);
                    return arg.msg(std::to_string(mTestType) + '\n');
                });
    mParser.opt("paramType1", "...command...", "parameters for testType1",
                [&](Arg& arg) { return mTestData1.getParser().main(arg.childArg()); });
    mParser.opt("paramType2", "...command...", "parameters for testType2",
                [&](Arg& arg) { return mTestData2.getParser().main(arg.childArg()); });
    mParser.opt("testRunSnapshot", "<dumpResult-on|off>", "test execution of snapshotDrawingLines()",
                [&](Arg& arg) {
                    const bool dumpResult = (arg++).as<bool>(0);
                    return testRunSnapshot(dumpResult,
                                           [&](const std::string& msg) { return arg.msg(msg); });
                });
}

bool
McrtVecSender::testRunSnapshot(const bool dumpResult, const MsgFunc& msgCallBack)
{
    msgCallBack("testRunSnapshot() start\n");

    auto oldMsgCallBack = mVecPacketEnqueue->getMsgCallBack();
    mVecPacketEnqueue->setMsgCallBack([&](const std::string& msg) {
        msgCallBack(msg);
        return true;
    });

    const size_t size = snapshotTestDrawing(1920, 1080);
    {
        std::ostringstream ostr;
        ostr << "testRunSnapshot() finished. data size:" << scene_rdl2::str_util::byteStr(size);
        msgCallBack(ostr.str() + '\n');
    }

    if (dumpResult) {
        scene_rdl2::grid_util::VectorPacketDequeue vpDec(mBuff.data(), mBuff.size());
        vpDec.setMsgCallBack([&](const std::string& msg) { return msgCallBack(msg); });
        try {
            vpDec.reset(mBuff.data(), mBuff.size()); // just in case
            vpDec.decodeAll();
        }
        catch (const std::string& err) {
            std::ostringstream ostr;
            ostr << "ERROR : VectorPacketDequeue::decodeAll() failed. err=>{\n"
                 << scene_rdl2::str_util::addIndent(err) << '\n'
                 << "}";
            msgCallBack(ostr.str() + '\n');
        }
    }

    mVecPacketEnqueue->setMsgCallBack(std::move(oldMsgCallBack));

    return true;
}

} // namespace engine_tool
} // namespace moonray

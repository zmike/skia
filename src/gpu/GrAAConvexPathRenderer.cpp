
/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrAAConvexPathRenderer.h"

#include "GrContext.h"
#include "GrDrawState.h"
#include "GrPathUtils.h"
#include "SkString.h"
#include "SkTrace.h"


GrAAConvexPathRenderer::GrAAConvexPathRenderer() {
}

bool GrAAConvexPathRenderer::canDrawPath(const GrDrawTarget::Caps& targetCaps,
                                         const SkPath& path,
                                         GrPathFill fill,
                                         bool antiAlias) const {
    return targetCaps.fShaderDerivativeSupport && antiAlias &&
           kHairLine_PathFill != fill && !GrIsFillInverted(fill) &&
           path.isConvex();
}

namespace {

struct Segment {
    enum {
        kLine,
        kQuad
    } fType;
    // line uses one pt, quad uses 2 pts
    GrPoint fPts[2];
    // normal to edge ending at each pt
    GrVec fNorms[2];
    // is the corner where the previous segment meets this segment
    // sharp. If so, fMid is a normalized bisector facing outward.
    GrVec fMid;

    int countPoints() {
        return (kLine == fType) ? 1 : 2;
    }
    const SkPoint& endPt() const {
        return (kLine == fType) ? fPts[0] : fPts[1];
    };
    const SkPoint& endNorm() const {
        return (kLine == fType) ? fNorms[0] : fNorms[1];
    };
};

typedef SkTArray<Segment, true> SegmentArray;

void center_of_mass(const SegmentArray& segments, SkPoint* c) {
    GrScalar area = 0;
    SkPoint center;
    center.set(0, 0);
    int count = segments.count();
    for (int i = 0; i < count; ++i) {
        const SkPoint& pi = segments[i].endPt();
        int j = (i + 1) % count;
        const SkPoint& pj = segments[j].endPt();
        GrScalar t = GrMul(pi.fX, pj.fY) - GrMul(pj.fX, pi.fY);
        area += t;
        center.fX += (pi.fX + pj.fX) * t;
        center.fY += (pi.fY + pj.fY) * t;
    }
    // If the poly has no area then we instead return the average of
    // its points.
    if (SkScalarAbs(area) < SK_ScalarNearlyZero) {
        SkPoint avg;
        avg.set(0, 0);
        for (int i = 0; i < count; ++i) {
            const SkPoint& pt = segments[i].endPt();
            avg.fX += pt.fX;
            avg.fY += pt.fY;
        }
        SkScalar denom = SK_Scalar1 / count;
        avg.scale(denom);
        *c = avg;
    } else {
        area *= 3;
        area = GrScalarDiv(GR_Scalar1, area);
        center.fX = GrScalarMul(center.fX, area);
        center.fY = GrScalarMul(center.fY, area);
        *c = center;
    }
    GrAssert(!SkScalarIsNaN(c->fX) && !SkScalarIsNaN(c->fY));
}

void compute_vectors(SegmentArray* segments,
                     SkPoint* fanPt,
                     SkPath::Direction dir,
                     int* vCount,
                     int* iCount) {
    center_of_mass(*segments, fanPt);
    int count = segments->count();

    // Make the normals point towards the outside
    GrPoint::Side normSide;
    if (dir == SkPath::kCCW_Direction) {
        normSide = GrPoint::kRight_Side;
    } else {
        normSide = GrPoint::kLeft_Side;
    }

    *vCount = 0;
    *iCount = 0;
    // compute normals at all points
    for (int a = 0; a < count; ++a) {
        const Segment& sega = (*segments)[a];
        int b = (a + 1) % count;
        Segment& segb = (*segments)[b];

        const GrPoint* prevPt = &sega.endPt();
        int n = segb.countPoints();
        for (int p = 0; p < n; ++p) {
            segb.fNorms[p] = segb.fPts[p] - *prevPt;
            segb.fNorms[p].normalize();
            segb.fNorms[p].setOrthog(segb.fNorms[p], normSide);
            prevPt = &segb.fPts[p];
        }
        if (Segment::kLine == segb.fType) {
            *vCount += 5;
            *iCount += 9;
        } else {
            *vCount += 6;
            *iCount += 12;
        }
    }

    // compute mid-vectors where segments meet. TODO: Detect shallow corners
    // and leave out the wedges and close gaps by stitching segments together.
    for (int a = 0; a < count; ++a) {
        const Segment& sega = (*segments)[a];
        int b = (a + 1) % count;
        Segment& segb = (*segments)[b];
        segb.fMid = segb.fNorms[0] + sega.endNorm();
        segb.fMid.normalize();
        // corner wedges
        *vCount += 4;
        *iCount += 6;
    }
}

struct DegenerateTestData {
    DegenerateTestData() { fStage = kInitial; }
    bool isDegenerate() const { return kNonDegenerate != fStage; }
    enum {
        kInitial,
        kPoint,
        kLine,
        kNonDegenerate
    }           fStage;
    GrPoint     fFirstPoint;
    GrVec       fLineNormal;
    GrScalar    fLineC;
};

void update_degenerate_test(DegenerateTestData* data, const GrPoint& pt) {
    static const SkScalar TOL = (SK_Scalar1 / 16);
    static const SkScalar TOL_SQD = SkScalarMul(TOL, TOL);

    switch (data->fStage) {
        case DegenerateTestData::kInitial:
            data->fFirstPoint = pt;
            data->fStage = DegenerateTestData::kPoint;
            break;
        case DegenerateTestData::kPoint:
            if (pt.distanceToSqd(data->fFirstPoint) > TOL_SQD) {
                data->fLineNormal = pt - data->fFirstPoint;
                data->fLineNormal.normalize();
                data->fLineNormal.setOrthog(data->fLineNormal);
                data->fLineC = -data->fLineNormal.dot(data->fFirstPoint);
                data->fStage = DegenerateTestData::kLine;
            }
            break;
        case DegenerateTestData::kLine:
            if (SkScalarAbs(data->fLineNormal.dot(pt) + data->fLineC) > TOL) {
                data->fStage = DegenerateTestData::kNonDegenerate;
            }
        case DegenerateTestData::kNonDegenerate:
            break;
        default:
            GrCrash("Unexpected degenerate test stage.");
    }
}

bool get_segments(const GrPath& path,
                 SegmentArray* segments,
                 SkPoint* fanPt,
                 int* vCount,
                 int* iCount) {
    SkPath::Iter iter(path, true);
    // This renderer overemphasises very thin path regions. We use the distance
    // to the path from the sample to compute coverage. Every pixel intersected
    // by the path will be hit and the maximum distance is sqrt(2)/2. We don't
    // notice that the sample may be close to a very thin area of the path and 
    // thus should be very light. This is particularly egregious for degenerate
    // line paths. We detect paths that are very close to a line (zero area) and
    // draw nothing.
    DegenerateTestData degenerateData;

    for (;;) {
        GrPoint pts[4];
        GrPathCmd cmd = (GrPathCmd)iter.next(pts);
        switch (cmd) {
            case kMove_PathCmd:
                update_degenerate_test(&degenerateData, pts[0]);
                break;
            case kLine_PathCmd: {
                update_degenerate_test(&degenerateData, pts[1]);
                segments->push_back();
                segments->back().fType = Segment::kLine;
                segments->back().fPts[0] = pts[1];
                break;
            }
            case kQuadratic_PathCmd:
                update_degenerate_test(&degenerateData, pts[1]);
                update_degenerate_test(&degenerateData, pts[2]);
                segments->push_back();
                segments->back().fType = Segment::kQuad;
                segments->back().fPts[0] = pts[1];
                segments->back().fPts[1] = pts[2];
                break;
            case kCubic_PathCmd: {
                update_degenerate_test(&degenerateData, pts[1]);
                update_degenerate_test(&degenerateData, pts[2]);
                update_degenerate_test(&degenerateData, pts[3]);
                SkSTArray<15, SkPoint, true> quads;
                GrPathUtils::convertCubicToQuads(pts, SK_Scalar1, &quads);
                int count = quads.count();
                for (int q = 0; q < count; q += 3) {
                    segments->push_back();
                    segments->back().fType = Segment::kQuad;
                    segments->back().fPts[0] = quads[q + 1];
                    segments->back().fPts[1] = quads[q + 2];
                }
                break;
            };
            case kEnd_PathCmd:
                if (degenerateData.isDegenerate()) {
                    return false;
                } else {
                    SkPath::Direction dir;
                    GR_DEBUGCODE(bool succeeded = )
                    path.cheapComputeDirection(&dir);
                    GrAssert(succeeded);
                    compute_vectors(segments, fanPt, dir, vCount, iCount);
                    return true;
                }
            default:
                break;
        }
    }
}

struct QuadVertex {
    GrPoint  fPos;
    GrPoint  fUV;
    GrScalar fD0;
    GrScalar fD1;
};
    
void create_vertices(const SegmentArray&  segments,
                     const SkPoint& fanPt,
                     QuadVertex*    verts,
                     uint16_t*      idxs) {
    int v = 0;
    int i = 0;

    int count = segments.count();
    for (int a = 0; a < count; ++a) {
        const Segment& sega = segments[a];
        int b = (a + 1) % count;
        const Segment& segb = segments[b];
        
        // FIXME: These tris are inset in the 1 unit arc around the corner
        verts[v + 0].fPos = sega.endPt();
        verts[v + 1].fPos = verts[v + 0].fPos + sega.endNorm();
        verts[v + 2].fPos = verts[v + 0].fPos + segb.fMid;
        verts[v + 3].fPos = verts[v + 0].fPos + segb.fNorms[0];
        verts[v + 0].fUV.set(0,0);
        verts[v + 1].fUV.set(0,-SK_Scalar1);
        verts[v + 2].fUV.set(0,-SK_Scalar1);
        verts[v + 3].fUV.set(0,-SK_Scalar1);
        verts[v + 0].fD0 = verts[v + 0].fD1 = -SK_Scalar1;
        verts[v + 1].fD0 = verts[v + 1].fD1 = -SK_Scalar1;
        verts[v + 2].fD0 = verts[v + 2].fD1 = -SK_Scalar1;
        verts[v + 3].fD0 = verts[v + 3].fD1 = -SK_Scalar1;
        
        idxs[i + 0] = v + 0;
        idxs[i + 1] = v + 2;
        idxs[i + 2] = v + 1;
        idxs[i + 3] = v + 0;
        idxs[i + 4] = v + 3;
        idxs[i + 5] = v + 2;
        
        v += 4;
        i += 6;

        if (Segment::kLine == segb.fType) {
            verts[v + 0].fPos = fanPt;
            verts[v + 1].fPos = sega.endPt();
            verts[v + 2].fPos = segb.fPts[0];

            verts[v + 3].fPos = verts[v + 1].fPos + segb.fNorms[0];
            verts[v + 4].fPos = verts[v + 2].fPos + segb.fNorms[0];

            // we draw the line edge as a degenerate quad (u is 0, v is the
            // signed distance to the edge)
            GrScalar dist = fanPt.distanceToLineBetween(verts[v + 1].fPos,
                                                        verts[v + 2].fPos);
            verts[v + 0].fUV.set(0, dist);
            verts[v + 1].fUV.set(0, 0);
            verts[v + 2].fUV.set(0, 0);
            verts[v + 3].fUV.set(0, -SK_Scalar1);
            verts[v + 4].fUV.set(0, -SK_Scalar1);

            verts[v + 0].fD0 = verts[v + 0].fD1 = -SK_Scalar1;
            verts[v + 1].fD0 = verts[v + 1].fD1 = -SK_Scalar1;
            verts[v + 2].fD0 = verts[v + 2].fD1 = -SK_Scalar1;
            verts[v + 3].fD0 = verts[v + 3].fD1 = -SK_Scalar1;
            verts[v + 4].fD0 = verts[v + 4].fD1 = -SK_Scalar1;

            idxs[i + 0] = v + 0;
            idxs[i + 1] = v + 2;
            idxs[i + 2] = v + 1;

            idxs[i + 3] = v + 3;
            idxs[i + 4] = v + 1;
            idxs[i + 5] = v + 2;

            idxs[i + 6] = v + 4;
            idxs[i + 7] = v + 3;
            idxs[i + 8] = v + 2;

            v += 5;
            i += 9;
        } else {
            GrPoint qpts[] = {sega.endPt(), segb.fPts[0], segb.fPts[1]};

            GrVec midVec = segb.fNorms[0] + segb.fNorms[1];
            midVec.normalize();

            verts[v + 0].fPos = fanPt;
            verts[v + 1].fPos = qpts[0];
            verts[v + 2].fPos = qpts[2];
            verts[v + 3].fPos = qpts[0] + segb.fNorms[0];
            verts[v + 4].fPos = qpts[2] + segb.fNorms[1];
            verts[v + 5].fPos = qpts[1] + midVec;

            GrScalar c = segb.fNorms[0].dot(qpts[0]);
            verts[v + 0].fD0 =  -segb.fNorms[0].dot(fanPt) + c;
            verts[v + 1].fD0 =  0.f;
            verts[v + 2].fD0 =  -segb.fNorms[0].dot(qpts[2]) + c;
            verts[v + 3].fD0 = -GR_ScalarMax/100;
            verts[v + 4].fD0 = -GR_ScalarMax/100;
            verts[v + 5].fD0 = -GR_ScalarMax/100;

            c = segb.fNorms[1].dot(qpts[2]);
            verts[v + 0].fD1 =  -segb.fNorms[1].dot(fanPt) + c;
            verts[v + 1].fD1 =  -segb.fNorms[1].dot(qpts[0]) + c;
            verts[v + 2].fD1 =  0.f;
            verts[v + 3].fD1 = -GR_ScalarMax/100;
            verts[v + 4].fD1 = -GR_ScalarMax/100;
            verts[v + 5].fD1 = -GR_ScalarMax/100;

            GrMatrix toUV;
            GrPathUtils::quadDesignSpaceToUVCoordsMatrix(qpts, &toUV);
            toUV.mapPointsWithStride(&verts[v].fUV,
                                     &verts[v].fPos,
                                     sizeof(QuadVertex),
                                     6);

            idxs[i + 0] = v + 3;
            idxs[i + 1] = v + 1;
            idxs[i + 2] = v + 2;
            idxs[i + 3] = v + 4;
            idxs[i + 4] = v + 3;
            idxs[i + 5] = v + 2;

            idxs[i + 6] = v + 5;
            idxs[i + 7] = v + 3;
            idxs[i + 8] = v + 4;

            idxs[i +  9] = v + 0;
            idxs[i + 10] = v + 2;
            idxs[i + 11] = v + 1;

            v += 6;
            i += 12;
        }
    }
}

}

void GrAAConvexPathRenderer::drawPath(GrDrawState::StageMask stageMask) {
    GrAssert(fPath->isConvex());
    if (fPath->isEmpty()) {
        return;
    }
    GrDrawState* drawState = fTarget->drawState();

    GrDrawTarget::AutoStateRestore asr;
    GrMatrix vm = drawState->getViewMatrix();
    vm.postTranslate(fTranslate.fX, fTranslate.fY);
    asr.set(fTarget);
    GrMatrix ivm;
    if (vm.invert(&ivm)) {
        drawState->preConcatSamplerMatrices(stageMask, ivm);
    }
    drawState->setViewMatrix(GrMatrix::I());

    SkPath path;
    fPath->transform(vm, &path);

    GrVertexLayout layout = 0;
    for (int s = 0; s < GrDrawState::kNumStages; ++s) {
        if ((1 << s) & stageMask) {
            layout |= GrDrawTarget::StagePosAsTexCoordVertexLayoutBit(s);
        }
    }
    layout |= GrDrawTarget::kEdge_VertexLayoutBit;

    QuadVertex *verts;
    uint16_t* idxs;

    int vCount;
    int iCount;
    SegmentArray segments;
    SkPoint fanPt;
    if (!get_segments(path, &segments, &fanPt, &vCount, &iCount)) {
        return;
    }

    if (!fTarget->reserveVertexSpace(layout,
                                     vCount,
                                     reinterpret_cast<void**>(&verts))) {
        return;
    }
    if (!fTarget->reserveIndexSpace(iCount, reinterpret_cast<void**>(&idxs))) {
        fTarget->resetVertexSource();
        return;
    }

    create_vertices(segments, fanPt, verts, idxs);

    drawState->setVertexEdgeType(GrDrawState::kQuad_EdgeType);
    fTarget->drawIndexed(kTriangles_PrimitiveType,
                         0,        // start vertex
                         0,        // start index
                         vCount,
                         iCount);
}


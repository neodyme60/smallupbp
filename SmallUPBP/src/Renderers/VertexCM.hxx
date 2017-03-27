/*
 * Copyright (C) 2014, Petr Vevoda, Martin Sik (http://cgg.mff.cuni.cz/~sik/), 
 * Tomas Davidovic (http://www.davidovic.cz), Iliyan Georgiev (http://www.iliyan.com/), 
 * Jaroslav Krivanek (http://cgg.mff.cuni.cz/~jaroslav/)
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * (The above is MIT License: http://en.wikipedia.origin/wiki/MIT_License)
 */

#ifndef __VERTEXCM_HXX__
#define __VERTEXCM_HXX__

#include <vector>
#include <cmath>

#include "../Misc/HashGrid.hxx"
#include "../Path/Bsdf.hxx"
#include "Renderer.hxx"

////////////////////////////////////////////////////////////////////////////////
// A NOTE ON PATH MIS WEIGHT EVALUATION
////////////////////////////////////////////////////////////////////////////////
//
// We compute path MIS weights iteratively as we trace the light and eye
// sub-paths. We cache three floating points quantities at each sub-path vertex:
//
//   dVCM  dVC  dVM
//
// These quantities represent partial weights associated with the sub-path. When
// we connect or merge one vertex to another, we use these quantities to quickly
// evaluate the MIS weight for the full path we have constructed. This scheme is
// presented in the technical report
//
//   "Implementing Vertex Connection and Merging"
//   http://www.iliyan.com/publications/ImplementingVCM
//
// The MIS code in the VertexCM class references the corresponding equations in
// the report in the form
//
//   [tech. rep. (##)]
//
// where ## is the equation number. 
//

class VertexCM : public AbstractRenderer
{
    // The sole point of this structure is to make carrying around the ray baggage easier.
    struct SubPathState
    {
        Pos   mOrigin;             // Path origin
        Dir   mDirection;          // Where to go next
        Rgb   mThroughput;         // Path throughput
        uint  mPathLength    : 30; // Number of path segments, including this
        uint  mIsFiniteLight :  1; // Just generate by finite light
        uint  mSpecularPath  :  1; // All scattering events so far were specular

		BoundaryStack mBoundaryStack; // Boundary stack

        float dVCM; // MIS quantity used for vertex connection and merging
        float dVC;  // MIS quantity used for vertex connection
        float dVM;  // MIS quantity used for vertex merging
    };

    // Path vertex, used for merging and connection
    struct PathVertex
    {
        Pos   mHitpoint;   // Position of the vertex
        Rgb   mThroughput; // Path throughput (including emission)
        uint  mPathLength; // Number of segments between source and vertex

        // Stores all required local information, including incoming direction.
        BSDF mBSDF;

        float dVCM; // MIS quantity used for vertex connection and merging
        float dVC;  // MIS quantity used for vertex connection
        float dVM;  // MIS quantity used for vertex merging

        // Used by HashGrid
        const Pos& GetPosition() const
        {
            return mHitpoint;
        }
		const bool MatchesType(uint aType) const
		{
			return true;
		}
    };

    // Range query used for PPM, BPT, and VCM. When HashGrid finds a vertex
    // within range -- Process() is called and vertex
    // merging is performed. BSDF of the camera vertex is used.
    class RangeQuery
    {
    public:

        RangeQuery(
            const VertexCM     &aVertexCM,
            const Pos          &aCameraPosition,
            const BSDF         &aCameraBsdf,
            const SubPathState &aCameraState
        ) : 
            mVertexCM(aVertexCM),
            mCameraPosition(aCameraPosition),
            mCameraBsdf(aCameraBsdf),
            mCameraState(aCameraState),
            mContrib(0)
        {}

        const Pos& GetPosition() const { return mCameraPosition; }

        const Rgb& GetContrib() const { return mContrib; }

        void Process(const PathVertex& aLightVertex)
        {
            // Reject if full path length below/above min/max path length
            if((aLightVertex.mPathLength + mCameraState.mPathLength > mVertexCM.mMaxPathLength) ||
               (aLightVertex.mPathLength + mCameraState.mPathLength < mVertexCM.mMinPathLength))
                 return;

            // Retrieve light incoming direction in world coordinates
            const Dir lightDirection = aLightVertex.mBSDF.WorldDirFix();

            float cosCamera, cameraBsdfDirPdfW, cameraBsdfRevPdfW;
            const Rgb cameraBsdfFactor = mCameraBsdf.Evaluate(
                lightDirection, cosCamera, &cameraBsdfDirPdfW,
                &cameraBsdfRevPdfW);

            if(cameraBsdfFactor.isBlackOrNegative())
                return;

            cameraBsdfDirPdfW *= mCameraBsdf.ContinuationProb();

            // Even though this is PDF from camera BSDF, the continuation probability
            // must come from light BSDF, because that would govern it if light path
            // actually continued
            cameraBsdfRevPdfW *= aLightVertex.mBSDF.ContinuationProb();

            // Partial light sub-path MIS weight [tech. rep. (38)]
            const float wLight = aLightVertex.dVCM * mVertexCM.mMisVcWeightFactor +
                aLightVertex.dVM * mVertexCM.Mis(cameraBsdfDirPdfW);

            // Partial eye sub-path MIS weight [tech. rep. (39)]
            const float wCamera = mCameraState.dVCM * mVertexCM.mMisVcWeightFactor +
                mCameraState.dVM * mVertexCM.Mis(cameraBsdfRevPdfW);

            // Full path MIS weight [tech. rep. (37)]. No MIS for PPM
            const float misWeight = mVertexCM.mPpm ?
                1.f :
                1.f / (wLight + 1.f + wCamera);

            mContrib += misWeight * cameraBsdfFactor * aLightVertex.mThroughput;
        }

    private:

        const VertexCM     &mVertexCM;
        const Pos          &mCameraPosition;
        const BSDF         &mCameraBsdf;
        const SubPathState &mCameraState;
        Rgb                mContrib;
    };

public:

    enum AlgorithmType
    {
        // light vertices contribute to camera,
        // No MIS weights (dVCM, dVM, dVC all ignored)
        kLightTrace = 0,

        // Camera and light vertices merged on first non-specular surface from camera.
        // Cannot handle mixed specular + non-specular materials.
        // No MIS weights (dVCM, dVM, dVC all ignored)
        kPpm,

        // Camera and light vertices merged on along full path.
        // dVCM and dVM used for MIS
        kBpm,

        // Standard bidirectional path tracing
        // dVCM and dVC used for MIS
        kBpt,

        // Vertex connection and mering
        // dVCM, dVM, and dVC used for MIS
        kVcm
    };

public:

    VertexCM(
        const Scene&  aScene,
        AlgorithmType aAlgorithm,
        const float   aRadiusFactor,
        const float   aRadiusAlpha,
        int           aSeed = 1234
    ) :
        AbstractRenderer(aScene),
        mRng(aSeed),
        mLightTraceOnly(false),
        mUseVC(false),
        mUseVM(false),
        mPpm(false)
    {
        switch(aAlgorithm)
        {
        case kLightTrace:
            mLightTraceOnly = true;
            break;
        case kPpm:
            mPpm   = true;
            mUseVM = true;
            break;
        case kBpm:
            mUseVM = true;
            break;
        case kBpt:
            mUseVC = true;
            break;
        case kVcm:
            mUseVC = true;
            mUseVM = true;
            break;
        default:
            std::cerr << "Error: unknown vcm algorithm requested" << std::endl;
            exit(2);
        }

        if(mPpm)
        {
            // We will check the scene to make sure it does not contain mixed
            // specular and non-specular materials
            for(int i = 0; i < mScene.GetMaterialCount(); ++i)
            {
                const Material &mat = mScene.GetMaterial(i);

                const bool hasNonSpecular =
                    (mat.mDiffuseReflectance.max() > 0) ||
                    (mat.mPhongReflectance.max() > 0);

                const bool hasSpecular =
                    (mat.mMirrorReflectance.max() > 0) ||
                    (mat.mIOR > 0);

                if(hasNonSpecular && hasSpecular)
                {
                    printf(
                        "*WARNING* Our PPM implementation cannot handle materials mixing\n"
                        "Specular and NonSpecular BSDFs. The extension would be\n"
                        "fairly straightforward. In SampleScattering for camera sub-paths\n"
                        "limit the considered events to Specular only.\n"
                        "Merging will use non-specular components, scattering will be specular.\n"
                        "If there is no specular component, the ray will terminate.\n\n");

                    printf("We are now switching from *PPM* to *BPM*, which can handle the scene\n\n");

                    mPpm = false;
                    break;
                }
            }
        }

		mBaseRadius  = aRadiusFactor > 0 ? aRadiusFactor : -aRadiusFactor * mScene.mSceneSphere.mSceneRadius;
        mRadiusAlpha = aRadiusAlpha;
    }

    virtual void RunIteration(int aIteration)
    {
        // While we have the same number of pixels (camera paths)
        // and light paths, we do keep them separate for clarity reasons
        const int resX = int(mScene.mCamera.mResolution.get(0));
        const int resY = int(mScene.mCamera.mResolution.get(1));
        const int pathCount = resX * resY;
        mScreenPixelCount = float(resX * resY);
        mLightSubPathCount   = float(resX * resY);

        // Setup our radius, 1st iteration has aIteration == 0, thus offset
        float radius = mBaseRadius;
        radius /= std::pow(float(aIteration + 1), 0.5f * (1 - mRadiusAlpha));
        // Purely for numeric stability
        radius = std::max(radius, 1e-7f);
        const float radiusSqr = Utils::sqr(radius);

        // Factor used to normalize vertex merging contribution.
        // We divide the summed up energy by disk radius and number of light paths
        mVmNormalization = 1.f / (radiusSqr * PI_F * mLightSubPathCount);

        // MIS weight constant [tech. rep. (20)], with n_VC = 1 and n_VM = mLightPathCount
        const float etaVCM = (PI_F * radiusSqr) * mLightSubPathCount;
        mMisVmWeightFactor = mUseVM ? Mis(etaVCM)       : 0.f;
        mMisVcWeightFactor = mUseVC ? Mis(1.f / etaVCM) : 0.f;

        // Clear path ends, nothing ends anywhere
        mPathEnds.resize(pathCount);
        memset(&mPathEnds[0], 0, mPathEnds.size() * sizeof(int));

        // Remove all light vertices and reserve space for some
        mLightVertices.reserve(pathCount);
        mLightVertices.clear();

        //////////////////////////////////////////////////////////////////////////
        // Generate light paths
        //////////////////////////////////////////////////////////////////////////
		for(int pathIdx = 0; (pathIdx < pathCount) &&  mScene.GetLightCount() > 0 && mMaxPathLength > 1; pathIdx++)
        {			
			SubPathState lightState;
            GenerateLightSample(lightState);

            //////////////////////////////////////////////////////////////////////////
            // Trace light path
            for(;; ++lightState.mPathLength)
            {
                Ray ray(lightState.mOrigin, lightState.mDirection);
                Isect isect(1e36f);

                if(!mScene.Intersect(ray, isect, lightState.mBoundaryStack))
                    break;

                const Pos hitPoint = ray.origin + ray.direction * isect.mDist;

				BSDF bsdf(ray, isect, mScene, BSDF::kFromLight, mScene.RelativeIOR(isect, lightState.mBoundaryStack));
                if(!bsdf.IsValid())
                    break;

                // Update the MIS quantities before storing them at the vertex.
                // These updates follow the initialization in GenerateLightSample() or
                // SampleScattering(), and together implement equations [tech. rep. (31)-(33)]
                // or [tech. rep. (34)-(36)], respectively.
                {
                    // Infinite lights use MIS handled via solid angle integration,
                    // so do not divide by the distance for such lights [tech. rep. Section 5.1]
                    if(lightState.mPathLength > 1 || lightState.mIsFiniteLight == 1)
                        lightState.dVCM *= Mis(Utils::sqr(isect.mDist));

                    lightState.dVCM /= Mis(std::abs(bsdf.CosThetaFix()));
                    lightState.dVC  /= Mis(std::abs(bsdf.CosThetaFix()));
                    lightState.dVM  /= Mis(std::abs(bsdf.CosThetaFix()));
                }

                // Store vertex, unless BSDF is purely specular, which prevents
                // vertex connections and merging
                if(!bsdf.IsDelta() && (mUseVC || mUseVM))
                {
                    PathVertex lightVertex;
                    lightVertex.mHitpoint   = hitPoint;
                    lightVertex.mThroughput = lightState.mThroughput;
                    lightVertex.mPathLength = lightState.mPathLength;
                    lightVertex.mBSDF       = bsdf;

                    lightVertex.dVCM = lightState.dVCM;
                    lightVertex.dVC  = lightState.dVC;
                    lightVertex.dVM  = lightState.dVM;

                    mLightVertices.push_back(lightVertex);
                }

                // Connect to camera, unless BSDF is purely specular
                if(!bsdf.IsDelta() && (mUseVC || mLightTraceOnly))
                {
                    if(lightState.mPathLength + 1 >= mMinPathLength)
                        ConnectToCamera(lightState, hitPoint, bsdf);
                }

                // Terminate if the path would become too long after scattering
                if(lightState.mPathLength + 2 > mMaxPathLength)
                    break;

                // Continue random walk
                if(!SampleScattering(bsdf, hitPoint, lightState, isect))
                    break;
            }

            mPathEnds[pathIdx] = (int)mLightVertices.size();
        }

        //////////////////////////////////////////////////////////////////////////
        // Build hash grid
        //////////////////////////////////////////////////////////////////////////

        // Only build grid when merging (VCM, BPM, and PPM)
        if(mUseVM)
        {
            // The number of cells is somewhat arbitrary, but seems to work ok
            mHashGrid.Reserve(pathCount);
            mHashGrid.Build(mLightVertices, radius);
        }

        //////////////////////////////////////////////////////////////////////////
        // Generate camera paths
        //////////////////////////////////////////////////////////////////////////

        // Unless rendering with traditional light tracing
        for(int pathIdx = 0; (pathIdx < pathCount) && (!mLightTraceOnly); ++pathIdx)
        {			
			SubPathState cameraState;
            const Vec2f screenSample = GenerateCameraSample(pathIdx, cameraState);
            Rgb color(0);
			
			//////////////////////////////////////////////////////////////////////
            // Trace camera path
            for(;; ++cameraState.mPathLength)
            {
				Ray ray(cameraState.mOrigin, cameraState.mDirection);
                Isect isect(1e36f);

                // Get radiance from environment
				if(!mScene.Intersect(ray, isect, cameraState.mBoundaryStack))
                {
                    if(mScene.GetBackground() != NULL)
                    {
                        if(cameraState.mPathLength >= mMinPathLength)
                        {
                            color += cameraState.mThroughput *
                                GetLightRadiance(mScene.GetBackground(), cameraState,
                                Pos(0), ray.direction);
                        }
                    }

                    break;
                }

				const Pos hitPoint = ray.origin + ray.direction * isect.mDist;

				BSDF bsdf(ray, isect, mScene, BSDF::kFromCamera,mScene.RelativeIOR(isect, cameraState.mBoundaryStack));
                if(!bsdf.IsValid())
                    break;

                // Update the MIS quantities, following the initialization in
                // GenerateLightSample() or SampleScattering(). Implement equations
                // [tech. rep. (31)-(33)] or [tech. rep. (34)-(36)], respectively.
                {
                    cameraState.dVCM *= Mis(Utils::sqr(isect.mDist));
                    cameraState.dVCM /= Mis(std::abs(bsdf.CosThetaFix()));
                    cameraState.dVC  /= Mis(std::abs(bsdf.CosThetaFix()));
                    cameraState.dVM  /= Mis(std::abs(bsdf.CosThetaFix()));
                }

                // Light source has been hit; terminate afterwards, since
                // our light sources do not have reflective properties
				if(isect.mLightID >= 0)
                {
                    const AbstractLight *light = mScene.GetLightPtr(isect.mLightID);
                
                    if(cameraState.mPathLength >= mMinPathLength)
                    {
                        color += cameraState.mThroughput *
                            GetLightRadiance(light, cameraState, hitPoint, ray.direction);
                    }
                    
                    break;
                }

                // Terminate if eye sub-path is too long for connections or merging
                if(cameraState.mPathLength >= mMaxPathLength)
                    break;

                ////////////////////////////////////////////////////////////////
                // Vertex connection: Connect to a light source
                if(!bsdf.IsDelta() && mUseVC && mScene.GetLightCount() > 0)
                {
                    if(cameraState.mPathLength + 1>= mMinPathLength)
                    {
                        color += cameraState.mThroughput *
                            DirectIllumination(cameraState, hitPoint, bsdf);
                    }
                }

                ////////////////////////////////////////////////////////////////
                // Vertex connection: Connect to light vertices
                if(!bsdf.IsDelta() && mUseVC)
                {
                    // For VC, each light sub-path is assigned to a particular eye
                    // sub-path, as in traditional BPT. It is also possible to
                    // connect to vertices from any light path, but MIS should
                    // be revisited.
                    const Vec2i range(
                        (pathIdx == 0) ? 0 : mPathEnds[pathIdx-1],
                        mPathEnds[pathIdx]);

                    for(int i = range[0]; i < range[1]; i++)
                    {
                        const PathVertex &lightVertex = mLightVertices[i];

                        if(lightVertex.mPathLength + 1 +
                           cameraState.mPathLength < mMinPathLength)
                            continue;

                        // Light vertices are stored in increasing path length
                        // order; once we go above the max path length, we can
                        // skip the rest
                        if(lightVertex.mPathLength + 1 +
                           cameraState.mPathLength > mMaxPathLength)
                            break;

                        color += cameraState.mThroughput * lightVertex.mThroughput *
                            ConnectVertices(lightVertex, bsdf, hitPoint, cameraState);
                    }
                }

                ////////////////////////////////////////////////////////////////
                // Vertex merging: Merge with light vertices
                if(!bsdf.IsDelta() && mUseVM)
                {
                    RangeQuery query(*this, hitPoint, bsdf, cameraState);
                    mHashGrid.Process(mLightVertices, query);
                    color += cameraState.mThroughput * mVmNormalization * query.GetContrib();

                    // PPM merges only at the first non-specular surface from camera
                    if(mPpm) break;
                }

                if(!SampleScattering(bsdf, hitPoint, cameraState, isect))
                    break;
            }

            mFramebuffer.AddColor(screenSample, color);
        }

        mIterations++;
    }

private:

    // MIS power, we use balance heuristic
    float Mis(float aPdf) const
    {
        //return std::pow(aPdf, /*power*/);
        return aPdf;
    }

    //////////////////////////////////////////////////////////////////////////
    // Camera tracing methods
    //////////////////////////////////////////////////////////////////////////

    // Generates new camera sample given a pixel index
    Vec2f GenerateCameraSample(
        const int    aPixelIndex,
        SubPathState &oCameraState)
    {
        const Camera &camera = mScene.mCamera;
        const int resX = int(camera.mResolution.get(0));
        const int resY = int(camera.mResolution.get(1));

        // Determine pixel (x, y)
        const int x = aPixelIndex % resX;
        const int y = aPixelIndex / resX;

        // Jitter pixel position
        const Vec2f sample = Vec2f(float(x), float(y)) + mRng.GetVec2f();

        // Generate ray
        const Ray primaryRay = camera.GenerateRay(sample);

        // Compute PDF conversion factor from area on image plane to solid angle on ray
        const float cosAtCamera = dot(camera.mDirection, primaryRay.direction);
        const float imagePointToCameraDist = camera.mImagePlaneDist / cosAtCamera;
        const float imageToSolidAngleFactor = Utils::sqr(imagePointToCameraDist) / cosAtCamera;

        // We put the virtual image plane at such a distance from the camera origin
        // that the pixel area is one and thus the image plane sampling PDF is 1.
        // The solid angle ray PDF is then equal to the conversion factor from
        // image plane area density to ray solid angle density
        const float cameraPdfW = imageToSolidAngleFactor;

        oCameraState.mOrigin       = primaryRay.origin;
        oCameraState.mDirection    = primaryRay.direction;
        oCameraState.mThroughput   = Rgb(1);

        oCameraState.mPathLength   = 1;
        oCameraState.mSpecularPath = 1;

		mScene.InitBoundaryStack(oCameraState.mBoundaryStack);

        // Eye sub-path MIS quantities. Implements [tech. rep. (31)-(33)] partially.
        // The evaluation is completed after tracing the camera ray in the eye sub-path loop.
        oCameraState.dVCM = Mis(mLightSubPathCount / cameraPdfW);
        oCameraState.dVC  = 0;
        oCameraState.dVM  = 0;

        return sample;
    }

    // Returns the radiance of a light source when hit by a random ray,
    // multiplied by MIS weight. Can be used for both Background and Area lights.
    //
    // For Background lights:
    //    Has to be called BEFORE updating the MIS quantities.
    //    Value of aHitpoint is irrelevant (passing Dir(0))
    //
    // For Area lights:
    //    Has to be called AFTER updating the MIS quantities.
    Rgb GetLightRadiance(
        const AbstractLight *aLight,
        const SubPathState  &aCameraState,
        const Pos           &aHitpoint,
        const Dir           &aRayDirection) const
    {
        // We sample lights uniformly
        const int   lightCount    = mScene.GetLightCount();
        const float lightPickProb = 1.f / lightCount;

        float directPdfA, emissionPdfW;
        const Rgb radiance = aLight->GetRadiance(mScene.mSceneSphere,
            aRayDirection, aHitpoint, &directPdfA, &emissionPdfW);

        if(radiance.isBlackOrNegative())
            return Rgb(0);

        // If we see light source directly from camera, no weighting is required
        if(aCameraState.mPathLength == 1)
            return radiance;

        // When using only vertex merging, we want purely specular paths
        // to give radiance (cannot get it otherwise). Rest is handled
        // by merging and we should return 0.
        if(mUseVM && !mUseVC)
            return aCameraState.mSpecularPath ? radiance : Rgb(0);

        directPdfA   *= lightPickProb;
        emissionPdfW *= lightPickProb;

        // Partial eye sub-path MIS weight [tech. rep. (43)].
        // If the last hit was specular, then dVCM == 0.
        const float wCamera = Mis(directPdfA) * aCameraState.dVCM +
            Mis(emissionPdfW) * aCameraState.dVC;

        // Partial light sub-path weight is 0 [tech. rep. (42)].

        // Full path MIS weight [tech. rep. (37)].
        const float misWeight = 1.f / (1.f + wCamera);
        
        return misWeight * radiance;
    }

    // Connects camera vertex to randomly chosen light point.
    // Returns emitted radiance multiplied by path MIS weight.
    // Has to be called AFTER updating the MIS quantities.
    Rgb DirectIllumination(
        const SubPathState &aCameraState,
        const Pos          &aHitpoint,
        const BSDF         &aBsdf)
    {
        // We sample lights uniformly
        const int   lightCount    = mScene.GetLightCount();
        const float lightPickProb = 1.f / lightCount;

        const int   lightID       = int(mRng.GetFloat() * lightCount);
        const Vec2f rndPosSamples = mRng.GetVec2f();

        const AbstractLight *light = mScene.GetLightPtr(lightID);

        Dir directionToLight;
        float distance;
        float directPdfW, emissionPdfW, cosAtLight;
        const Rgb radiance = light->Illuminate(mScene.mSceneSphere, aHitpoint,
            rndPosSamples, directionToLight, distance, directPdfW,
            &emissionPdfW, &cosAtLight);

        // If radiance == 0, other values are undefined, so have to early exit
        if(radiance.isBlackOrNegative())
            return Rgb(0);

        float bsdfDirPdfW, bsdfRevPdfW, cosToLight;
        const Rgb bsdfFactor = aBsdf.Evaluate(directionToLight, cosToLight, &bsdfDirPdfW, &bsdfRevPdfW);

        if(bsdfFactor.isBlackOrNegative())
            return Rgb(0);

        const float continuationProbability = aBsdf.ContinuationProb();
        
        // If the light is delta light, we can never hit it
        // by BSDF sampling, so the probability of this path is 0
        bsdfDirPdfW *= light->IsDelta() ? 0.f : continuationProbability;

        bsdfRevPdfW *= continuationProbability;

        // Partial light sub-path MIS weight [tech. rep. (44)].
        // Note that wLight is a ratio of area PDFs. But since both are on the
        // light source, their distance^2 and cosine terms cancel out.
        // Therefore we can write wLight as a ratio of solid angle PDFs,
        // both expressed w.r.t. the same shading point.
        const float wLight = Mis(bsdfDirPdfW / (lightPickProb * directPdfW));

        // Partial eye sub-path MIS weight [tech. rep. (45)].
        //
        // In front of the sum in the parenthesis we have Mis(ratio), where
        //    ratio = emissionPdfA / directPdfA,
        // with emissionPdfA being the product of the PDFs for choosing the
        // point on the light source and sampling the outgoing direction.
        // What we are given by the light source instead are emissionPdfW
        // and directPdfW. Converting to area PDFs and plugging into ratio:
        //    emissionPdfA = emissionPdfW * cosToLight / dist^2
        //    directPdfA   = directPdfW * cosAtLight / dist^2
        //    ratio = (emissionPdfW * cosToLight / dist^2) / (directPdfW * cosAtLight / dist^2)
        //    ratio = (emissionPdfW * cosToLight) / (directPdfW * cosAtLight)
        //
        // Also note that both emissionPdfW and directPdfW should be
        // multiplied by lightPickProb, so it cancels out.
        const float wCamera = Mis(emissionPdfW * cosToLight / (directPdfW * cosAtLight)) * (
            mMisVmWeightFactor + aCameraState.dVCM + aCameraState.dVC * Mis(bsdfRevPdfW));

        // Full path MIS weight [tech. rep. (37)]
        const float misWeight = 1.f / (wLight + 1.f + wCamera);

        const Rgb contrib =
            (misWeight * cosToLight / (lightPickProb * directPdfW)) * (radiance * bsdfFactor);

        if(contrib.isBlackOrNegative() || mScene.Occluded(aHitpoint, directionToLight, distance, aCameraState.mBoundaryStack))
            return Rgb(0);

        return contrib;
    }

    // Connects an eye and a light vertex. Result multiplied by MIS weight, but
    // not multiplied by vertex throughputs. Has to be called AFTER updating MIS
    // constants. 'direction' is FROM eye TO light vertex.
    Rgb ConnectVertices(
        const PathVertex   &aLightVertex,
        const BSDF         &aCameraBSDF,
        const Pos          &aCameraHitpoint,
        const SubPathState &aCameraState) const
    {
        // Get the connection
        Dir direction     = aLightVertex.mHitpoint - aCameraHitpoint;
        const float dist2 = direction.square();
        float  distance   = std::sqrt(dist2);
        direction        /= distance;

        // Evaluate BSDF at camera vertex
        float cosCamera, cameraBsdfDirPdfW, cameraBsdfRevPdfW;
        const Rgb cameraBsdfFactor = aCameraBSDF.Evaluate(
            direction, cosCamera, &cameraBsdfDirPdfW,
            &cameraBsdfRevPdfW);

        if(cameraBsdfFactor.isBlackOrNegative())
            return Rgb(0);

        // Camera continuation probability (for Russian roulette)
        const float cameraCont = aCameraBSDF.ContinuationProb();
        cameraBsdfDirPdfW *= cameraCont;
        cameraBsdfRevPdfW *= cameraCont;

        // Evaluate BSDF at light vertex
        float cosLight, lightBsdfDirPdfW, lightBsdfRevPdfW;
        const Rgb lightBsdfFactor = aLightVertex.mBSDF.Evaluate(
            -direction, cosLight, &lightBsdfDirPdfW,
            &lightBsdfRevPdfW);

        if(lightBsdfFactor.isBlackOrNegative())
            return Rgb(0);

        // Light continuation probability (for Russian roulette)
        const float lightCont = aLightVertex.mBSDF.ContinuationProb();
        lightBsdfDirPdfW *= lightCont;
        lightBsdfRevPdfW *= lightCont;

        // Compute geometry term
        const float geometryTerm = cosLight * cosCamera / dist2;
        if(geometryTerm < 0)
            return Rgb(0);

        // Convert PDFs to area PDF
        const float cameraBsdfDirPdfA = PdfWtoA(cameraBsdfDirPdfW, distance, cosLight);
        const float lightBsdfDirPdfA  = PdfWtoA(lightBsdfDirPdfW,  distance, cosCamera);

        // Partial light sub-path MIS weight [tech. rep. (40)]
        const float wLight = Mis(cameraBsdfDirPdfA) * (
            mMisVmWeightFactor + aLightVertex.dVCM + aLightVertex.dVC * Mis(lightBsdfRevPdfW));

        // Partial eye sub-path MIS weight [tech. rep. (41)]
        const float wCamera = Mis(lightBsdfDirPdfA) * (
            mMisVmWeightFactor + aCameraState.dVCM + aCameraState.dVC * Mis(cameraBsdfRevPdfW));

        // Full path MIS weight [tech. rep. (37)]
        const float misWeight = 1.f / (wLight + 1.f + wCamera);

        const Rgb contrib = (misWeight * geometryTerm) * cameraBsdfFactor * lightBsdfFactor;

        if(contrib.isBlackOrNegative() || mScene.Occluded(aCameraHitpoint, direction, distance, aCameraState.mBoundaryStack))
            return Rgb(0);

        return contrib;
    }

    //////////////////////////////////////////////////////////////////////////
    // Light tracing methods
    //////////////////////////////////////////////////////////////////////////

    // Samples light emission
    void GenerateLightSample(SubPathState &oLightState)
    {
        // We sample lights uniformly
        const int   lightCount    = mScene.GetLightCount();
        const float lightPickProb = 1.f / lightCount;

        const int   lightID       = int(mRng.GetFloat() * lightCount);
        const Vec2f rndDirSamples = mRng.GetVec2f();
        const Vec2f rndPosSamples = mRng.GetVec2f();

        const AbstractLight *light = mScene.GetLightPtr(lightID);

        float emissionPdfW, directPdfW, cosLight;
        oLightState.mThroughput = light->Emit(mScene.mSceneSphere, rndDirSamples, rndPosSamples,
            oLightState.mOrigin, oLightState.mDirection,
            emissionPdfW, &directPdfW, &cosLight);

        emissionPdfW *= lightPickProb;
        directPdfW   *= lightPickProb;

        oLightState.mThroughput    /= emissionPdfW;
        oLightState.mPathLength    = 1;
        oLightState.mIsFiniteLight = light->IsFinite() ? 1 : 0;
		mScene.InitBoundaryStack(oLightState.mBoundaryStack);

        // Light sub-path MIS quantities. Implements [tech. rep. (31)-(33)] partially.
        // The evaluation is completed after tracing the emission ray in the light sub-path loop.
        // Delta lights are handled as well [tech. rep. (48)-(50)].
        {
            oLightState.dVCM = Mis(directPdfW / emissionPdfW);

            if(!light->IsDelta())
            {
                const float usedCosLight = light->IsFinite() ? cosLight : 1.f;
                oLightState.dVC = Mis(usedCosLight / emissionPdfW);
            }
            else
            {
                oLightState.dVC = 0.f;
            }

            oLightState.dVM = oLightState.dVC * mMisVcWeightFactor;
        }
    }

    // Computes contribution of light sample to camera by splatting is onto the
    // framebuffer. Multiplies by throughput (obviously, as nothing is returned).
    void ConnectToCamera(
        const SubPathState &aLightState,
        const Pos          &aHitpoint,
        const BSDF         &aBSDF)
    {
        const Camera &camera  = mScene.mCamera;
		Dir directionToCamera = camera.mOrigin - aHitpoint;

        // Check point is in front of camera
		if(dot(camera.mDirection, -directionToCamera) <= 0.f)
            return;

        // Check it projects to the screen (and where)
        const Vec2f imagePos = camera.WorldToRaster(aHitpoint);
        if(!camera.CheckRaster(imagePos))
            return;

        // Compute distance and normalize direction to camera
        const float distEye2 = directionToCamera.square();
        const float distance = std::sqrt(distEye2);
        directionToCamera   /= distance;

        // Get the BSDF
        float cosToCamera, bsdfDirPdfW, bsdfRevPdfW;
        const Rgb bsdfFactor = aBSDF.Evaluate(directionToCamera, cosToCamera, &bsdfDirPdfW, &bsdfRevPdfW);

        if(bsdfFactor.isBlackOrNegative())
            return;

        bsdfRevPdfW *= aBSDF.ContinuationProb();

        // Compute PDF conversion factor from image plane area to surface area
		const float cosAtCamera = dot(camera.mDirection, -directionToCamera);
        const float imagePointToCameraDist = camera.mImagePlaneDist / cosAtCamera;
        const float imageToSolidAngleFactor = Utils::sqr(imagePointToCameraDist) / cosAtCamera;
        const float imageToSurfaceFactor = imageToSolidAngleFactor * std::abs(cosToCamera) / Utils::sqr(distance);

        // We put the virtual image plane at such a distance from the camera origin
        // that the pixel area is one and thus the image plane sampling PDF is 1.
        // The area PDF of aHitpoint as sampled from the camera is then equal to
        // the conversion factor from image plane area density to surface area density
        const float cameraPdfA = imageToSurfaceFactor;

        // Partial light sub-path weight [tech. rep. (46)]. Note the division by
        // mLightPathCount, which is the number of samples this technique uses.
        // This division also appears a few lines below in the framebuffer accumulation.
        const float wLight = Mis(cameraPdfA / mLightSubPathCount) * (
            mMisVmWeightFactor + aLightState.dVCM + aLightState.dVC * Mis(bsdfRevPdfW));

        // Partial eye sub-path weight is 0 [tech. rep. (47)]

        // Full path MIS weight [tech. rep. (37)]. No MIS for traditional light tracing.
        const float misWeight = mLightTraceOnly ? 1.f : (1.f / (wLight + 1.f));

        const float surfaceToImageFactor = 1.f / imageToSurfaceFactor;

        // We divide the contribution by surfaceToImageFactor to convert the (already
        // divided) PDF from surface area to image plane area, w.r.t. which the
        // pixel integral is actually defined. We also divide by the number of samples
        // this technique makes, which is equal to the number of light sub-paths
        const Rgb contrib = misWeight * aLightState.mThroughput * bsdfFactor /
            (mLightSubPathCount * surfaceToImageFactor);

        if(!contrib.isBlackOrNegative())
        {
            if(mScene.Occluded(aHitpoint, directionToCamera, distance, aLightState.mBoundaryStack))
                return;

            mFramebuffer.AddColor(imagePos, contrib);
        }
    }

    // Samples a scattering direction camera/light sample according to BSDF.
    // Returns false for termination
    bool SampleScattering(
        const BSDF               &aBSDF,
        const Pos                &aHitPoint,
        SubPathState             &aoState,
		const Isect              &isect)
    {
        // x,y for direction, z for component. No rescaling happens
        Dir   rndTriplet  = mRng.GetVec3f();
        float bsdfDirPdfW, cosThetaOut;
        uint  sampledEvent;

        Rgb bsdfFactor = aBSDF.Sample(rndTriplet, aoState.mDirection,
            bsdfDirPdfW, cosThetaOut, &sampledEvent);

        if(bsdfFactor.isBlackOrNegative())
            return false;

        // If we sampled specular event, then the reverse probability
        // cannot be evaluated, but we know it is exactly the same as
        // forward probability, so just set it. If non-specular event happened,
        // we evaluate the PDF
        float bsdfRevPdfW = bsdfDirPdfW;
        if((sampledEvent & BSDF::kSpecular) == 0)
			bsdfRevPdfW = aBSDF.Pdf(aoState.mDirection, BSDF::kReverse);

        // Russian roulette
        const float contProb = aBSDF.ContinuationProb();
        if (contProb == 0 || (contProb < 1.0f && mRng.GetFloat() > contProb))
            return false;

        bsdfDirPdfW *= contProb;
        bsdfRevPdfW *= contProb;

        // Sub-path MIS quantities for the next vertex. Only partial - the
        // evaluation is completed when the actual hit point is known,
        // i.e. after tracing the ray, in the sub-path loop.

        if(sampledEvent & BSDF::kSpecular)
        {
            // Specular scattering case [tech. rep. (53)-(55)] (partially, as noted above)
            aoState.dVCM = 0.f;
            //aoState.dVC *= Mis(cosThetaOut / bsdfDirPdfW) * Mis(bsdfRevPdfW);
            //aoState.dVM *= Mis(cosThetaOut / bsdfDirPdfW) * Mis(bsdfRevPdfW);
            assert(bsdfDirPdfW == bsdfRevPdfW);
            aoState.dVC *= Mis(cosThetaOut);
            aoState.dVM *= Mis(cosThetaOut);

            aoState.mSpecularPath &= 1;
        }
        else
        {
            // Implements [tech. rep. (34)-(36)] (partially, as noted above)
            aoState.dVC = Mis(cosThetaOut / bsdfDirPdfW) * (
                aoState.dVC * Mis(bsdfRevPdfW) +
                aoState.dVCM + mMisVmWeightFactor);

            aoState.dVM = Mis(cosThetaOut / bsdfDirPdfW) * (
                aoState.dVM * Mis(bsdfRevPdfW) +
                aoState.dVCM * mMisVcWeightFactor + 1.f);

            aoState.dVCM = Mis(1.f / bsdfDirPdfW);

            aoState.mSpecularPath &= 0;
        }

        aoState.mOrigin  = aHitPoint;
        aoState.mThroughput *= bsdfFactor * (cosThetaOut / bsdfDirPdfW);

		// Switch medium on refraction
		if ((sampledEvent & BSDF::kRefract) != 0)
			mScene.UpdateBoundaryStackOnRefract(isect, aoState.mBoundaryStack);
        
        return true;
    }

private:

    bool  mUseVM;             // Vertex merging (of some form) is used
    bool  mUseVC;             // Vertex connection (BPT) is used
    bool  mLightTraceOnly;    // Do only light tracing
    bool  mPpm;               // Do PPM, same terminates camera after first merge

    float mRadiusAlpha;       // Radius reduction rate parameter
    float mBaseRadius;        // Initial merging radius
    float mMisVmWeightFactor; // Weight of vertex merging (used in VC)
    float mMisVcWeightFactor; // Weight of vertex connection (used in VM)
    float mScreenPixelCount;  // Number of pixels
    float mLightSubPathCount; // Number of light sub-paths
    float mVmNormalization;   // 1 / (Pi * radius^2 * light_path_count)

    std::vector<PathVertex> mLightVertices; //!< Stored light vertices

    // For light path belonging to pixel index [x] it stores
    // where it's light vertices end (begin is at [x-1])
    std::vector<int> mPathEnds;
    HashGrid         mHashGrid;

    Rng              mRng;
};

#endif //__VERTEXCM_HXX__

// lodspheremesh.cpp
//
// Copyright (C) 2000-2009, theCelestia Development Team
// Original version by Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <cmath>
#include <cassert>
#include <iostream>
#include <algorithm>
#include <celmath/mathlib.h>
#include "glsupport.h"
#include "lodspheremesh.h"
#include "shadermanager.h"

using namespace std;
using namespace Eigen;
using namespace celmath;

//#define SHOW_PATCH_VISIBILITY
//#define SHOW_FRUSTUM
#define VERTEX_BUFFER_OBJECTS_ENABLED

static bool trigArraysInitialized = false;
static int maxDivisions = 16384;
static int thetaDivisions = maxDivisions;
static int phiDivisions = maxDivisions / 2;
static int minStep = 128;
static float* sinPhi = nullptr;
static float* cosPhi = nullptr;
static float* sinTheta = nullptr;
static float* cosTheta = nullptr;

// largest vertex:
//     position   - 3 floats,
//     normal     - 3 floats,
//     tangent    - 3 floats,
//     tex coords - 2 floats * MAX_SPHERE_MESH_TEXTURES
static int MaxVertexSize = 3 + 3 + 3 + MAX_SPHERE_MESH_TEXTURES * 2;

#ifdef SHOW_PATCH_VISIBILITY
static const int MaxPatchesShown = 4096;
static int visiblePatches[MaxPatchesShown];
#endif


// TODO: figure out how to use std eigen's methods instead
static Vector3f intersect3(const Frustum::PlaneType& p0,
                           const Frustum::PlaneType& p1,
                           const Frustum::PlaneType& p2)
{
    Matrix3f m;
    m.row(0) = p0.normal();
    m.row(1) = p1.normal();
    m.row(2) = p2.normal();
    float d = m.determinant();

    return (p0.offset() * p1.normal().cross(p2.normal()) +
            p1.offset() * p2.normal().cross(p0.normal()) +
            p2.offset() * p0.normal().cross(p1.normal())) * (1.0f / d);
}

static void InitTrigArrays()
{
    sinTheta = new float[thetaDivisions + 1];
    cosTheta = new float[thetaDivisions + 1];
    sinPhi = new float[phiDivisions + 1];
    cosPhi = new float[phiDivisions + 1];

    int i;
    for (i = 0; i <= thetaDivisions; i++)
    {
        double theta = (double) i / (double) thetaDivisions * 2.0 * PI;
        sinTheta[i] = (float) sin(theta);
        cosTheta[i] = (float) cos(theta);
    }

    for (i = 0; i <= phiDivisions; i++)
    {
        double phi = ((double) i / (double) phiDivisions - 0.5) * PI;
        sinPhi[i] = (float) sin(phi);
        cosPhi[i] = (float) cos(phi);
    }

    trigArraysInitialized = true;
}


static float getSphereLOD(float discSizeInPixels)
{
    if (discSizeInPixels < 10)
        return -3.0f;
    if (discSizeInPixels < 20)
        return -2.0f;
    if (discSizeInPixels < 50)
        return -1.0f;
    if (discSizeInPixels < 200)
        return 0.0f;
    if (discSizeInPixels < 1200)
        return 1.0f;
    if (discSizeInPixels < 7200)
        return 2.0f;
    if (discSizeInPixels < 53200)
        return 3.0f;

    return 4.0f;
}


LODSphereMesh::LODSphereMesh()
{
    if (!trigArraysInitialized)
        InitTrigArrays();

    int maxThetaSteps = thetaDivisions / minStep;
    int maxPhiSteps = phiDivisions / minStep;
    maxVertices = (maxPhiSteps + 1) * (maxThetaSteps + 1);
    vertices = new float[MaxVertexSize * maxVertices];

    nIndices = maxPhiSteps * 2 * (maxThetaSteps + 1);
    indices = new unsigned short[nIndices];
}


LODSphereMesh::~LODSphereMesh()
{
    delete[] vertices;
    delete[] indices;
}


static Vector3f spherePoint(int theta, int phi)
{
    return Vector3f(cosPhi[phi] * cosTheta[theta],
                    sinPhi[phi],
                    cosPhi[phi] * sinTheta[theta]);
}


void LODSphereMesh::render(const Frustum& frustum,
                           float pixWidth,
                           Texture** tex,
                           int nTextures)
{
    render(Normals | TexCoords0, frustum, pixWidth, tex, nTextures);
}


void LODSphereMesh::render(unsigned int attributes,
                           const Frustum& frustum,
                           float pixWidth,
                           Texture* tex0,
                           Texture* tex1,
                           Texture* tex2,
                           Texture* tex3)
{
    Texture* textures[MAX_SPHERE_MESH_TEXTURES];
    int nTextures = 0;

    if (tex0 != nullptr)
        textures[nTextures++] = tex0;
    if (tex1 != nullptr)
        textures[nTextures++] = tex1;
    if (tex2 != nullptr)
        textures[nTextures++] = tex2;
    if (tex3 != nullptr)
        textures[nTextures++] = tex3;
    render(attributes, frustum, pixWidth, textures, nTextures);
}


void LODSphereMesh::render(unsigned int attributes,
                           const Frustum& frustum,
                           float pixWidth,
                           Texture** tex,
                           int nTextures)
{
    int lod = 64;
    float lodBias = getSphereLOD(pixWidth);

    if (lodBias < 0.0f)
    {
        if (lodBias < -30)
            lodBias = -30;
        lod = lod / (1 << (int) (-lodBias));
        if (lod < 2)
            lod = 2;
    }
    else if (lodBias > 0.0f)
    {
        if (lodBias > 30)
            lodBias = 30;
        lod = lod * (1 << (int) lodBias);
        if (lod > maxDivisions)
            lod = maxDivisions;
    }

    int step = maxDivisions / lod;
    int thetaExtent = maxDivisions;
    int phiExtent = thetaExtent / 2;

    int split = 1;
    if (step < minStep)
    {
        split = minStep / step;
        thetaExtent /= split;
        phiExtent /= split;
    }

    if (tex == nullptr)
        nTextures = 0;


    RenderInfo ri(step, attributes, frustum);

    // If one of the textures is split into subtextures, we may have to
    // use extra patches, since there can be at most one subtexture per patch.
    int i;
    int minSplit = 1;
    for (i = 0; i < nTextures; i++)
    {
        float pixelsPerTexel = pixWidth * 2.0f /
            ((float) tex[i]->getWidth() / 2.0f);
        double l = log(pixelsPerTexel) / log(2.0);

        ri.texLOD[i] = max(min(tex[i]->getLODCount() - 1, (int) l), 0);
        if (tex[i]->getUTileCount(ri.texLOD[i]) > minSplit)
            minSplit = tex[i]->getUTileCount(ri.texLOD[i]);
        if (tex[i]->getVTileCount(ri.texLOD[i]) > minSplit)
            minSplit = tex[i]->getVTileCount(ri.texLOD[i]);
    }

    if (split < minSplit)
    {
        thetaExtent /= (minSplit / split);
        phiExtent /= (minSplit / split);
        split = minSplit;
        if (phiExtent <= ri.step)
            ri.step /= ri.step / phiExtent;
    }

    // Set the current textures
    nTexturesUsed = nTextures;
    for (i = 0; i < nTextures; i++)
    {
        tex[i]->beginUsage();
        textures[i] = tex[i];
        subtextures[i] = 0;
        if (nTextures > 1)
            glActiveTexture(GL_TEXTURE0 + i);
    }

#ifdef VERTEX_BUFFER_OBJECTS_ENABLED
    if (!vertexBuffersInitialized)
    {
        // TODO: assumes that the same context is used every time we
        // render.  Valid now, but not necessarily in the future.  Still,
        // would only cause problems if we rendered in two different contexts
        // and only one had vertex buffer objects.
        vertexBuffersInitialized = true;
        if (true)
        {
            for (unsigned int & vertexBuffer : vertexBuffers)
            {
                GLuint vbname = 0;
                glGenBuffers(1, &vbname);
                vertexBuffer = (unsigned int) vbname;
                glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
                glBufferData(GL_ARRAY_BUFFER,
                                     maxVertices * MaxVertexSize * sizeof(float),
                                     nullptr,
                                     GL_STREAM_DRAW);
            }
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            glGenBuffers(1, &indexBuffer);

            useVertexBuffers = true;

            // HACK: delete the user arrays--we shouldn't need to allocate
            // these at all if we're using vertex buffer objects.
            delete[] vertices;
        }
    }
#endif

    if (useVertexBuffers)
    {
        currentVB = 0;
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffers[currentVB]);
    }

    // Set up the mesh vertices
    int nRings = phiExtent / ri.step;
    int nSlices = thetaExtent / ri.step;

    int n2 = 0;
    for (i = 0; i < nRings; i++)
    {
        for (int j = 0; j <= nSlices; j++)
        {
            indices[n2 + 0] = i * (nSlices + 1) + j;
            indices[n2 + 1] = (i + 1) * (nSlices + 1) + j;
            n2 += 2;
        }
    }

    if (useVertexBuffers)
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                             nIndices * sizeof(indices[0]),
                             indices,
                             GL_DYNAMIC_DRAW);
    }

    // Compute the size of a vertex
    vertexSize = 3;
    if ((attributes & Tangents) != 0)
        vertexSize += 3;
    for (i = 0; i < nTextures; i++)
        vertexSize += 2;

    glEnableClientState(GL_VERTEX_ARRAY);
    if ((attributes & Normals) != 0)
        glEnableClientState(GL_NORMAL_ARRAY);

    for (i = 0; i < nTextures; i++)
    {
        if (nTextures > 1)
            glClientActiveTexture(GL_TEXTURE0 + i);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    }

    glDisableClientState(GL_COLOR_ARRAY);

    if ((attributes & Tangents) != 0)
        glEnableVertexAttribArray(CelestiaGLProgram::TangentAttributeIndex);

    if (split == 1)
    {
        renderSection(0, 0, thetaExtent, ri);
    }
    else
    {
        // Render the sphere section by section.
        /*int reject = 0;   Unused*/

        // Compute the vertices of the view frustum.  These will be used for
        // culling patches.
        ri.fp[0] = intersect3(frustum.plane(Frustum::Near),
                              frustum.plane(Frustum::Top),
                              frustum.plane(Frustum::Left));
        ri.fp[1] = intersect3(frustum.plane(Frustum::Near),
                              frustum.plane(Frustum::Top),
                              frustum.plane(Frustum::Right));
        ri.fp[2] = intersect3(frustum.plane(Frustum::Near),
                              frustum.plane(Frustum::Bottom),
                              frustum.plane(Frustum::Left));
        ri.fp[3] = intersect3(frustum.plane(Frustum::Near),
                              frustum.plane(Frustum::Bottom),
                              frustum.plane(Frustum::Right));
        ri.fp[4] = intersect3(frustum.plane(Frustum::Far),
                              frustum.plane(Frustum::Top),
                              frustum.plane(Frustum::Left));
        ri.fp[5] = intersect3(frustum.plane(Frustum::Far),
                              frustum.plane(Frustum::Top),
                              frustum.plane(Frustum::Right));
        ri.fp[6] = intersect3(frustum.plane(Frustum::Far),
                              frustum.plane(Frustum::Bottom),
                              frustum.plane(Frustum::Left));
        ri.fp[7] = intersect3(frustum.plane(Frustum::Far),
                              frustum.plane(Frustum::Bottom),
                              frustum.plane(Frustum::Right));


#ifdef SHOW_PATCH_VISIBILITY
        {
            for (int i = 0; i < MaxPatchesShown; i++)
                visiblePatches[i] = 0;
        }
#endif // SHOW_PATCH_VISIBILITY

        int nPatches = 0;
        {
            int extent = maxDivisions / 2;

            for (int i = 0; i < 2; i++)
            {
                for (int j = 0; j < 2; j++)
                {
                    nPatches += renderPatches(i * extent / 2, j * extent,
                                              extent, split / 2, ri);
                }
            }
        }
        // cout << "Rendered " << nPatches << " of " << square(split) << " patches\n";
    }

    glDisableClientState(GL_VERTEX_ARRAY);
    if ((attributes & Normals) != 0)
        glDisableClientState(GL_NORMAL_ARRAY);

    if ((attributes & Tangents) != 0)
        glDisableVertexAttribArray(CelestiaGLProgram::TangentAttributeIndex);

    for (i = 0; i < nTextures; i++)
    {
        tex[i]->endUsage();

        if (nTextures > 1)
        {
            glClientActiveTexture(GL_TEXTURE0 + i);
            glActiveTexture(GL_TEXTURE0 + i);
        }
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }

    if (nTextures > 1)
    {
        glClientActiveTexture(GL_TEXTURE0);
        glActiveTexture(GL_TEXTURE0);
    }

    if (useVertexBuffers)
    {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        vertices = nullptr;
    }

#ifdef SHOW_FRUSTUM
    // Debugging code for visualizing the frustum.
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluPerspective(45.0, 1.3333f, 1.0f, 100.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glColor4f(1, 0, 0, 1);
    glTranslatef(0, 0, -20);
    glBegin(GL_LINES);
    glVertex(ri.fp[0]); glVertex(ri.fp[1]);
    glVertex(ri.fp[0]); glVertex(ri.fp[2]);
    glVertex(ri.fp[3]); glVertex(ri.fp[1]);
    glVertex(ri.fp[3]); glVertex(ri.fp[2]);
    glVertex(ri.fp[4]); glVertex(ri.fp[5]);
    glVertex(ri.fp[4]); glVertex(ri.fp[6]);
    glVertex(ri.fp[7]); glVertex(ri.fp[5]);
    glVertex(ri.fp[7]); glVertex(ri.fp[6]);
    glVertex(ri.fp[0]); glVertex(ri.fp[4]);
    glVertex(ri.fp[1]); glVertex(ri.fp[5]);
    glVertex(ri.fp[2]); glVertex(ri.fp[6]);
    glVertex(ri.fp[3]); glVertex(ri.fp[7]);
    glEnd();

    // Render axes representing the unit sphere.
    glColor4f(0, 1, 0, 1);
    glBegin(GL_LINES);
    glVertex3f(-1, 0, 0); glVertex3f(1, 0, 0);
    glVertex3f(0, -1, 0); glVertex3f(0, 1, 0);
    glVertex3f(0, 0, -1); glVertex3f(1, 0, 1);
    glEnd();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
#endif

#ifdef SHOW_PATCH_VISIBILITY
    // Debugging code for visualizing the frustum.
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glColor4f(1, 0, 1, 1);

    {
        int width = split;
        int height = width / 2;
        float patchWidth = 1.0f / (float) width;
        float patchHeight = 1.0f / (float) height;
        if (width * height <= MaxPatchesShown)
        {
            for (int i = 0; i < height; i++)
            {
                for (int j = 0; j < width; j++)
                {
                    glPushMatrix();
                    glTranslatef(-0.5f + j * patchWidth,
                                 1.0f - i * patchHeight,
                                 0.0f);
                    if (visiblePatches[i * width + j])
                        glBegin(GL_QUADS);
                    else
                        glBegin(GL_LINE_LOOP);
                    glVertex3f(0.0f, 0.0f, 0.0f);
                    glVertex3f(0.0f, -patchHeight, 0.0f);
                    glVertex3f(patchWidth, -patchHeight, 0.0f);
                    glVertex3f(patchWidth, 0.0f, 0.0f);
                    glEnd();
                    glPopMatrix();
                }
            }
        }
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
#endif // SHOW_PATCH_VISIBILITY
}


int LODSphereMesh::renderPatches(int phi0, int theta0,
                                 int extent,
                                 int level,
                                 const RenderInfo& ri)
{
    int thetaExtent = extent;
    int phiExtent = extent / 2;

    // Compute the plane separating this section of the sphere from
    // the rest of the sphere.  If the view frustum lies entirely
    // on the side of the plane that does not contain the sphere
    // patch, we cull the patch.
    Vector3f p0 = spherePoint(theta0, phi0);
    Vector3f p1 = spherePoint(theta0 + thetaExtent, phi0);
    Vector3f p2 = spherePoint(theta0 + thetaExtent,
                             phi0 + phiExtent);
    Vector3f p3 = spherePoint(theta0, phi0 + phiExtent);
    Vector3f v0 = p1 - p0;
    Vector3f v2 = p3 - p2;
    Vector3f normal;

    if (v0.squaredNorm() > v2.squaredNorm())
        normal = (p0 - p3).cross(v0);
    else
        normal = (p2 - p1).cross(v2);

    // If the normal is near zero length, something's going wrong
    assert(normal.norm() != 0.0f);
    normal.normalize();
    Frustum::PlaneType separatingPlane(normal, p0);

    bool outside = true;
#if 1
    for (int k = 0; k < 8; k++)
    {
        if (separatingPlane.absDistance(ri.fp[k]) > 0.0f)
        {
            outside = false;
            break;
        }
    }

    // If this patch is outside the view frustum, so are all of its subpatches
    if (outside)
        return 0;
#else
    outside = false;
#endif

    // Second cull test uses the bounding sphere of the patch
#if 0
    // Is this a better choice for the patch center?
    Point3f patchCenter = spherePoint(theta0 + thetaExtent / 2,
                                      phi0 + phiExtent / 2);
#else
    // . . . or is the average of the points better?
    Vector3f patchCenter = Vector3f(p0.x() + p1.x() + p2.x() + p3.x(),
                                    p0.y() + p1.y() + p2.y() + p3.y(),
                                    p0.z() + p1.z() + p2.z() + p3.z()) * 0.25f;
#endif
    float boundingRadius = 0.0f;
    boundingRadius = max(boundingRadius, (patchCenter - p0).norm()); // patchCenter.distanceTo(p0)
    boundingRadius = max(boundingRadius, (patchCenter - p1).norm());
    boundingRadius = max(boundingRadius, (patchCenter - p2).norm());
    boundingRadius = max(boundingRadius, (patchCenter - p3).norm());
    if (ri.frustum.testSphere(patchCenter, boundingRadius) == Frustum::Outside)
        outside = true;

    if (outside)
        return 0;

    if (level == 1)
    {
        renderSection(phi0, theta0, thetaExtent, ri);
        return 1;
    }

    int nRendered = 0;
    for (int i = 0; i < 2; i++)
    {
        for (int j = 0; j < 2; j++)
        {
            nRendered += renderPatches(phi0 + phiExtent / 2 * i,
                                       theta0 + thetaExtent / 2 * j,
                                       extent / 2,
                                       level / 2,
                                       ri);
        }
    }
    return nRendered;
}


void LODSphereMesh::renderSection(int phi0, int theta0, int extent,
                                  const RenderInfo& ri)

{
#ifdef SHOW_PATCH_VISIBILITY
    {
        int width = thetaDivisions / extent;
        int height = phiDivisions / extent;
        int x = theta0 / extent;
        int y = phi0 / extent;
        if (width * height <= MaxPatchesShown)
            visiblePatches[y * width + x] = 1;
    }
#endif // SHOW_PATCH_VISIBILITY

    auto stride = (GLsizei) (vertexSize * sizeof(float));
    int texCoordOffset = ((ri.attributes & Tangents) != 0) ? 6 : 3;
    float* vertexBase = useVertexBuffers ? (float*) nullptr : vertices;

    glVertexPointer(3, GL_FLOAT, stride, vertexBase + 0);
    if ((ri.attributes & Normals) != 0)
        glNormalPointer(GL_FLOAT, stride, vertexBase);

    for (int tc = 0; tc < nTexturesUsed; tc++)
    {
        if (nTexturesUsed > 1)
            glClientActiveTexture(GL_TEXTURE0 + tc);
        glTexCoordPointer(2, GL_FLOAT, stride,  vertexBase + (tc * 2) + texCoordOffset);
    }

    if ((ri.attributes & Tangents) != 0)
    {
        glVertexAttribPointer(CelestiaGLProgram::TangentAttributeIndex,
                              3, GL_FLOAT, GL_FALSE,
                              stride, vertexBase + 3); // 3 == tangentOffset
    }

    // assert(ri.step >= minStep);
    // assert(phi0 + extent <= maxDivisions);
    // assert(theta0 + extent / 2 < maxDivisions);
    // assert(isPow2(extent));
    int thetaExtent = extent;
    int phiExtent = extent / 2;
    int theta1 = theta0 + thetaExtent;
    int phi1 = phi0 + phiExtent;
    /*int n3 = 0;   Unused*/
    /*int n2 = 0;   Unused*/

    float du[MAX_SPHERE_MESH_TEXTURES];
    float dv[MAX_SPHERE_MESH_TEXTURES];
    float u0[MAX_SPHERE_MESH_TEXTURES];
    float v0[MAX_SPHERE_MESH_TEXTURES];


    if (useVertexBuffers)
    {
        // Calling glBufferData() with nullptr before mapping the buffer
        // is a hint to OpenGL that previous contents of vertex buffer will
        // be discarded and overwritten. It enables renaming in the driver,
        // hopefully resulting in performance gains.
        glBufferData(GL_ARRAY_BUFFER,
                             maxVertices * vertexSize * sizeof(float),
                             nullptr,
                             GL_STREAM_DRAW);

        vertices = reinterpret_cast<float*>(glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY));
        if (vertices == nullptr)
            return;
    }

    // Set the current texture.  This is necessary because the texture
    // may be split into subtextures.
    for (int tex = 0; tex < nTexturesUsed; tex++)
    {
        du[tex] = (float) 1.0f / thetaDivisions;;
        dv[tex] = (float) 1.0f / phiDivisions;;
        u0[tex] = 1.0f;
        v0[tex] = 1.0f;

        if (textures[tex] != nullptr)
        {
            int uTexSplit = textures[tex]->getUTileCount(ri.texLOD[tex]);
            int vTexSplit = textures[tex]->getVTileCount(ri.texLOD[tex]);
            int patchSplit = maxDivisions / extent;
            assert(patchSplit >= uTexSplit && patchSplit >= vTexSplit);

            int u = theta0 / thetaExtent;
            int v = phi0 / phiExtent;
            int patchesPerUSubtex = patchSplit / uTexSplit;
            int patchesPerVSubtex = patchSplit / vTexSplit;

            du[tex] *= uTexSplit;
            dv[tex] *= vTexSplit;
            u0[tex] = 1.0f - ((float) (u % patchesPerUSubtex) /
                              (float) patchesPerUSubtex);
            v0[tex] = 1.0f - ((float) (v % patchesPerVSubtex) /
                              (float) patchesPerVSubtex);
            u0[tex] += theta0 * du[tex];
            v0[tex] += phi0 * dv[tex];

            u /= patchesPerUSubtex;
            v /= patchesPerVSubtex;

            if (nTexturesUsed > 1)
                glActiveTexture(GL_TEXTURE0 + tex);
            TextureTile tile = textures[tex]->getTile(ri.texLOD[tex],
                                                      uTexSplit - u - 1,
                                                      vTexSplit - v - 1);
            du[tex] *= tile.du;
            dv[tex] *= tile.dv;
            u0[tex] = u0[tex] * tile.du + tile.u;
            v0[tex] = v0[tex] * tile.dv + tile.v;

            // We track the current texture to avoid unnecessary and costly
            // texture state changes.
            if (tile.texID != subtextures[tex])
            {
                glBindTexture(GL_TEXTURE_2D, tile.texID);
                subtextures[tex] = tile.texID;
            }
        }
    }

    int vindex = 0;
    for (int phi = phi0; phi <= phi1; phi += ri.step)
    {
        float cphi = cosPhi[phi];
        float sphi = sinPhi[phi];

        if ((ri.attributes & Tangents) != 0)
        {
            for (int theta = theta0; theta <= theta1; theta += ri.step)
            {
                float ctheta = cosTheta[theta];
                float stheta = sinTheta[theta];

                vertices[vindex]      = cphi * ctheta;
                vertices[vindex + 1]  = sphi;
                vertices[vindex + 2]  = cphi * stheta;

                // Compute the tangent--required for bump mapping
                vertices[vindex + 3] = stheta;
                vertices[vindex + 4] = 0.0f;
                vertices[vindex + 5] = -ctheta;

                vindex += 6;

                for (int tex = 0; tex < nTexturesUsed; tex++)
                {
                    vertices[vindex]     = u0[tex] - theta * du[tex];
                    vertices[vindex + 1] = v0[tex] - phi * dv[tex];
                    vindex += 2;
                }
            }
        }
        else
        {
            for (int theta = theta0; theta <= theta1; theta += ri.step)
            {
                float ctheta = cosTheta[theta];
                float stheta = sinTheta[theta];

                vertices[vindex]      = cphi * ctheta;
                vertices[vindex + 1]  = sphi;
                vertices[vindex + 2]  = cphi * stheta;

                vindex += 3;

                for (int tex = 0; tex < nTexturesUsed; tex++)
                {
                    vertices[vindex]     = u0[tex] - theta * du[tex];
                    vertices[vindex + 1] = v0[tex] - phi * dv[tex];
                    vindex += 2;
                }
            }
        }
    }

    if (useVertexBuffers)
    {
        vertices = nullptr;
        if (!glUnmapBuffer(GL_ARRAY_BUFFER))
            return;
    }

    // TODO: Fix this--number of rings can reach zero and cause dropout
    // int nRings = max(phiExtent / ri.step, 1); // buggy
    int nRings = phiExtent / ri.step;
    int nSlices = thetaExtent / ri.step;
    unsigned short* indexBase = useVertexBuffers ? (unsigned short*) nullptr : indices;
    for (int i = 0; i < nRings; i++)
    {
        glDrawElements(GL_QUAD_STRIP,
                       (nSlices + 1) * 2,
                       GL_UNSIGNED_SHORT,
                       indexBase + (nSlices + 1) * 2 * i);
    }

    // Cycle through the vertex buffers
    if (useVertexBuffers)
    {
        currentVB++;
        if (currentVB == NUM_SPHERE_VERTEX_BUFFERS)
            currentVB = 0;
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffers[currentVB]);
    }
}

/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "OpenGLRenderer"

#include "Caches.h"

#include "GammaFontRenderer.h"
#include "LayerRenderer.h"
#include "Properties.h"
#include "renderstate/RenderState.h"
#include "ShadowTessellator.h"
#include "RenderState.h"
#include "utils/GLUtils.h"

#include <utils/Log.h>
#include <utils/String8.h>

namespace android {
namespace uirenderer {

Caches* Caches::sInstance = nullptr;

///////////////////////////////////////////////////////////////////////////////
// Macros
///////////////////////////////////////////////////////////////////////////////

#if DEBUG_CACHE_FLUSH
    #define FLUSH_LOGD(...) ALOGD(__VA_ARGS__)
#else
    #define FLUSH_LOGD(...)
#endif

///////////////////////////////////////////////////////////////////////////////
// Constructors/destructor
///////////////////////////////////////////////////////////////////////////////

Caches::Caches(RenderState& renderState)
        : gradientCache(mExtensions)
        , patchCache(renderState)
        , programCache(mExtensions)
        , dither(*this)
        , mRenderState(&renderState)
        , mInitialized(false) {
    INIT_LOGD("Creating OpenGL renderer caches");
    init();
    initFont();
    initConstraints();
    initStaticProperties();
    initExtensions();
}

bool Caches::init() {
    if (mInitialized) return false;

    ATRACE_NAME("Caches::init");

    mRegionMesh = nullptr;
    mProgram = nullptr;

    mFunctorsCount = 0;

    patchCache.init();

    mInitialized = true;

    mPixelBufferState = new PixelBufferState();
    mTextureState = new TextureState();

    return true;
}

void Caches::initFont() {
    fontRenderer = GammaFontRenderer::createRenderer();
}

void Caches::initExtensions() {
    if (mExtensions.hasDebugMarker()) {
        eventMark = glInsertEventMarkerEXT;

        startMark = glPushGroupMarkerEXT;
        endMark = glPopGroupMarkerEXT;
    } else {
        eventMark = eventMarkNull;
        startMark = startMarkNull;
        endMark = endMarkNull;
    }
}

void Caches::initConstraints() {
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
}

void Caches::initStaticProperties() {
    gpuPixelBuffersEnabled = false;

    // OpenGL ES 3.0+ specific features
    if (mExtensions.hasPixelBufferObjects()) {
        char property[PROPERTY_VALUE_MAX];
        if (property_get(PROPERTY_ENABLE_GPU_PIXEL_BUFFERS, property, "true") > 0) {
            gpuPixelBuffersEnabled = !strcmp(property, "true");
        }
    }
}

void Caches::terminate() {
    if (!mInitialized) return;
    mRegionMesh.release();

    fboCache.clear();

    programCache.clear();
    mProgram = nullptr;

    patchCache.clear();

    clearGarbage();

    delete mPixelBufferState;
    mPixelBufferState = nullptr;
    delete mTextureState;
    mTextureState = nullptr;
    mInitialized = false;
}

void Caches::setProgram(const ProgramDescription& description) {
    setProgram(programCache.get(description));
}

void Caches::setProgram(Program* program) {
    if (!program || !program->isInUse()) {
        if (mProgram) {
            mProgram->remove();
        }
        if (program) {
            program->use();
        }
        mProgram = program;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Debug
///////////////////////////////////////////////////////////////////////////////

uint32_t Caches::getOverdrawColor(uint32_t amount) const {
    static uint32_t sOverdrawColors[2][4] = {
            { 0x2f0000ff, 0x2f00ff00, 0x3fff0000, 0x7fff0000 },
            { 0x2f0000ff, 0x4fffff00, 0x5fff8ad8, 0x7fff0000 }
    };
    if (amount < 1) amount = 1;
    if (amount > 4) amount = 4;

    int overdrawColorIndex = static_cast<int>(Properties::overdrawColorSet);
    return sOverdrawColors[overdrawColorIndex][amount - 1];
}

void Caches::dumpMemoryUsage() {
    String8 stringLog;
    dumpMemoryUsage(stringLog);
    ALOGD("%s", stringLog.string());
}

void Caches::dumpMemoryUsage(String8 &log) {
    uint32_t total = 0;
    log.appendFormat("Current memory usage / total memory usage (bytes):\n");
    log.appendFormat("  TextureCache         %8d / %8d\n",
            textureCache.getSize(), textureCache.getMaxSize());
    log.appendFormat("  LayerCache           %8d / %8d (numLayers = %zu)\n",
            layerCache.getSize(), layerCache.getMaxSize(), layerCache.getCount());
    if (mRenderState) {
        int memused = 0;
        for (std::set<Layer*>::iterator it = mRenderState->mActiveLayers.begin();
                it != mRenderState->mActiveLayers.end(); it++) {
            const Layer* layer = *it;
            log.appendFormat("    Layer size %dx%d; isTextureLayer()=%d; texid=%u fbo=%u; refs=%d\n",
                    layer->getWidth(), layer->getHeight(),
                    layer->isTextureLayer(), layer->getTextureId(),
                    layer->getFbo(), layer->getStrongCount());
            memused += layer->getWidth() * layer->getHeight() * 4;
        }
        log.appendFormat("  Layers total   %8d (numLayers = %zu)\n",
                memused, mRenderState->mActiveLayers.size());
        total += memused;
    }
    log.appendFormat("  RenderBufferCache    %8d / %8d\n",
            renderBufferCache.getSize(), renderBufferCache.getMaxSize());
    log.appendFormat("  GradientCache        %8d / %8d\n",
            gradientCache.getSize(), gradientCache.getMaxSize());
    log.appendFormat("  PathCache            %8d / %8d\n",
            pathCache.getSize(), pathCache.getMaxSize());
    log.appendFormat("  TessellationCache    %8d / %8d\n",
            tessellationCache.getSize(), tessellationCache.getMaxSize());
    log.appendFormat("  TextDropShadowCache  %8d / %8d\n", dropShadowCache.getSize(),
            dropShadowCache.getMaxSize());
    log.appendFormat("  PatchCache           %8d / %8d\n",
            patchCache.getSize(), patchCache.getMaxSize());
    for (uint32_t i = 0; i < fontRenderer->getFontRendererCount(); i++) {
        const uint32_t sizeA8 = fontRenderer->getFontRendererSize(i, GL_ALPHA);
        const uint32_t sizeRGBA = fontRenderer->getFontRendererSize(i, GL_RGBA);
        log.appendFormat("  FontRenderer %d A8    %8d / %8d\n", i, sizeA8, sizeA8);
        log.appendFormat("  FontRenderer %d RGBA  %8d / %8d\n", i, sizeRGBA, sizeRGBA);
        log.appendFormat("  FontRenderer %d total %8d / %8d\n", i, sizeA8 + sizeRGBA,
                sizeA8 + sizeRGBA);
    }
    log.appendFormat("Other:\n");
    log.appendFormat("  FboCache             %8d / %8d\n",
            fboCache.getSize(), fboCache.getMaxSize());

    total += textureCache.getSize();
    total += renderBufferCache.getSize();
    total += gradientCache.getSize();
    total += pathCache.getSize();
    total += tessellationCache.getSize();
    total += dropShadowCache.getSize();
    total += patchCache.getSize();
    for (uint32_t i = 0; i < fontRenderer->getFontRendererCount(); i++) {
        total += fontRenderer->getFontRendererSize(i, GL_ALPHA);
        total += fontRenderer->getFontRendererSize(i, GL_RGBA);
    }

    log.appendFormat("Total memory usage:\n");
    log.appendFormat("  %d bytes, %.2f MB\n", total, total / 1024.0f / 1024.0f);
}

///////////////////////////////////////////////////////////////////////////////
// Memory management
///////////////////////////////////////////////////////////////////////////////

void Caches::clearGarbage() {
    textureCache.clearGarbage();
    pathCache.clearGarbage();
    patchCache.clearGarbage();
}

void Caches::flush(FlushMode mode) {
    FLUSH_LOGD("Flushing caches (mode %d)", mode);

    switch (mode) {
        case kFlushMode_Full:
            textureCache.clear();
            patchCache.clear();
            dropShadowCache.clear();
            gradientCache.clear();
            fontRenderer->clear();
            fboCache.clear();
            dither.clear();
            // fall through
        case kFlushMode_Moderate:
            fontRenderer->flush();
            textureCache.flush();
            pathCache.clear();
            tessellationCache.clear();
            // fall through
        case kFlushMode_Layers:
            layerCache.clear();
            renderBufferCache.clear();
            break;
    }

    clearGarbage();
    glFinish();

    // Errors during cleanup should be considered non-fatal, dump them and
    // and move on. TODO: All errors or just errors like bad surface?
    GLUtils::dumpGLErrors();
}

///////////////////////////////////////////////////////////////////////////////
// VBO
///////////////////////////////////////////////////////////////////////////////

bool Caches::bindMeshBuffer() {
    return bindMeshBuffer(meshBuffer);
}

bool Caches::bindMeshBuffer(const GLuint buffer) {
    if (mCurrentBuffer != buffer) {
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        mCurrentBuffer = buffer;
        return true;
    }
    return false;
}

bool Caches::unbindMeshBuffer() {
    if (mCurrentBuffer) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        mCurrentBuffer = 0;
        return true;
    }
    return false;
}

bool Caches::bindIndicesBufferInternal(const GLuint buffer) {
    if (mCurrentIndicesBuffer != buffer) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
        mCurrentIndicesBuffer = buffer;
        return true;
    }
    return false;
}

bool Caches::bindQuadIndicesBuffer() {
    if (!mMeshIndices) {
        uint16_t* regionIndices = new uint16_t[gMaxNumberOfQuads * 6];
        for (uint32_t i = 0; i < gMaxNumberOfQuads; i++) {
            uint16_t quad = i * 4;
            int index = i * 6;
            regionIndices[index    ] = quad;       // top-left
            regionIndices[index + 1] = quad + 1;   // top-right
            regionIndices[index + 2] = quad + 2;   // bottom-left
            regionIndices[index + 3] = quad + 2;   // bottom-left
            regionIndices[index + 4] = quad + 1;   // top-right
            regionIndices[index + 5] = quad + 3;   // bottom-right
        }

        glGenBuffers(1, &mMeshIndices);
        bool force = bindIndicesBufferInternal(mMeshIndices);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, gMaxNumberOfQuads * 6 * sizeof(uint16_t),
                regionIndices, GL_STATIC_DRAW);

        delete[] regionIndices;
        return force;
    }

    return bindIndicesBufferInternal(mMeshIndices);
}

bool Caches::bindShadowIndicesBuffer() {
    if (!mShadowStripsIndices) {
        uint16_t* shadowIndices = new uint16_t[MAX_SHADOW_INDEX_COUNT];
        ShadowTessellator::generateShadowIndices(shadowIndices);
        glGenBuffers(1, &mShadowStripsIndices);
        bool force = bindIndicesBufferInternal(mShadowStripsIndices);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, MAX_SHADOW_INDEX_COUNT * sizeof(uint16_t),
            shadowIndices, GL_STATIC_DRAW);

        delete[] shadowIndices;
        return force;
    }

    return bindIndicesBufferInternal(mShadowStripsIndices);
}

bool Caches::unbindIndicesBuffer() {
    if (mCurrentIndicesBuffer) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        mCurrentIndicesBuffer = 0;
        return true;
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////////
// PBO
///////////////////////////////////////////////////////////////////////////////

bool Caches::bindPixelBuffer(const GLuint buffer) {
    if (mCurrentPixelBuffer != buffer) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
        mCurrentPixelBuffer = buffer;
        return true;
    }
    return false;
}

bool Caches::unbindPixelBuffer() {
    if (mCurrentPixelBuffer) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        mCurrentPixelBuffer = 0;
        return true;
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////////
// Meshes and textures
///////////////////////////////////////////////////////////////////////////////

void Caches::bindPositionVertexPointer(bool force, const GLvoid* vertices, GLsizei stride) {
    if (force || vertices != mCurrentPositionPointer || stride != mCurrentPositionStride) {
        GLuint slot = currentProgram->position;
        glVertexAttribPointer(slot, 2, GL_FLOAT, GL_FALSE, stride, vertices);
        mCurrentPositionPointer = vertices;
        mCurrentPositionStride = stride;
    }
}

void Caches::bindTexCoordsVertexPointer(bool force, const GLvoid* vertices, GLsizei stride) {
    if (force || vertices != mCurrentTexCoordsPointer || stride != mCurrentTexCoordsStride) {
        GLuint slot = currentProgram->texCoords;
        glVertexAttribPointer(slot, 2, GL_FLOAT, GL_FALSE, stride, vertices);
        mCurrentTexCoordsPointer = vertices;
        mCurrentTexCoordsStride = stride;
    }
}

void Caches::resetVertexPointers() {
    mCurrentPositionPointer = this;
    mCurrentTexCoordsPointer = this;
}

void Caches::resetTexCoordsVertexPointer() {
    mCurrentTexCoordsPointer = this;
}

void Caches::enableTexCoordsVertexArray() {
    if (!mTexCoordsArrayEnabled) {
        glEnableVertexAttribArray(Program::kBindingTexCoords);
        mCurrentTexCoordsPointer = this;
        mTexCoordsArrayEnabled = true;
    }
}

void Caches::disableTexCoordsVertexArray() {
    if (mTexCoordsArrayEnabled) {
        glDisableVertexAttribArray(Program::kBindingTexCoords);
        mTexCoordsArrayEnabled = false;
    }
}

void Caches::activeTexture(GLuint textureUnit) {
    if (mTextureUnit != textureUnit) {
        glActiveTexture(gTextureUnits[textureUnit]);
        mTextureUnit = textureUnit;
    }
}

void Caches::resetActiveTexture() {
    mTextureUnit = -1;
}

void Caches::bindTexture(GLuint texture) {
    if (mBoundTextures[mTextureUnit] != texture) {
        glBindTexture(GL_TEXTURE_2D, texture);
        mBoundTextures[mTextureUnit] = texture;
    }
}

void Caches::bindTexture(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_2D) {
        bindTexture(texture);
    } else {
        // GLConsumer directly calls glBindTexture() with
        // target=GL_TEXTURE_EXTERNAL_OES, don't cache this target
        // since the cached state could be stale
        glBindTexture(target, texture);
    }
}

void Caches::deleteTexture(GLuint texture) {
    // When glDeleteTextures() is called on a currently bound texture,
    // OpenGL ES specifies that the texture is then considered unbound
    // Consider the following series of calls:
    //
    // glGenTextures -> creates texture name 2
    // glBindTexture(2)
    // glDeleteTextures(2) -> 2 is now unbound
    // glGenTextures -> can return 2 again
    //
    // If we don't call glBindTexture(2) after the second glGenTextures
    // call, any texture operation will be performed on the default
    // texture (name=0)

    unbindTexture(texture);

    glDeleteTextures(1, &texture);
}

void Caches::resetBoundTextures() {
    memset(mBoundTextures, 0, REQUIRED_TEXTURE_UNITS_COUNT * sizeof(GLuint));
}

void Caches::unbindTexture(GLuint texture) {
    for (int i = 0; i < REQUIRED_TEXTURE_UNITS_COUNT; i++) {
        if (mBoundTextures[i] == texture) {
            mBoundTextures[i] = 0;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Scissor
///////////////////////////////////////////////////////////////////////////////

bool Caches::setScissor(GLint x, GLint y, GLint width, GLint height) {
    if (scissorEnabled && (x != mScissorX || y != mScissorY ||
            width != mScissorWidth || height != mScissorHeight)) {

        if (x < 0) {
            width += x;
            x = 0;
        }
        if (y < 0) {
            height += y;
            y = 0;
        }
        if (width < 0) {
            width = 0;
        }
        if (height < 0) {
            height = 0;
        }
        glScissor(x, y, width, height);

        mScissorX = x;
        mScissorY = y;
        mScissorWidth = width;
        mScissorHeight = height;

        return true;
    }
    return false;
}

bool Caches::enableScissor() {
    if (!scissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
        scissorEnabled = true;
        resetScissor();
        return true;
    }
    return false;
}

bool Caches::disableScissor() {
    if (scissorEnabled) {
        glDisable(GL_SCISSOR_TEST);
        scissorEnabled = false;
        return true;
    }
    return false;
}

void Caches::setScissorEnabled(bool enabled) {
    if (scissorEnabled != enabled) {
        if (enabled) glEnable(GL_SCISSOR_TEST);
        else glDisable(GL_SCISSOR_TEST);
        scissorEnabled = enabled;
    }
}

void Caches::resetScissor() {
    mScissorX = mScissorY = mScissorWidth = mScissorHeight = 0;
}

///////////////////////////////////////////////////////////////////////////////
// Tiling
///////////////////////////////////////////////////////////////////////////////

void Caches::startTiling(GLuint x, GLuint y, GLuint width, GLuint height, bool discard) {
    if (mExtensions.hasTiledRendering() && !Properties::debugOverdraw) {
        glStartTilingQCOM(x, y, width, height, (discard ? GL_NONE : GL_COLOR_BUFFER_BIT0_QCOM));
    }
}

void Caches::endTiling() {
    if (mExtensions.hasTiledRendering() && !Properties::debugOverdraw) {
        glEndTilingQCOM(GL_COLOR_BUFFER_BIT0_QCOM);
    }
}

bool Caches::hasRegisteredFunctors() {
    return mFunctorsCount > 0;
}

void Caches::registerFunctors(uint32_t functorCount) {
    mFunctorsCount += functorCount;
}

void Caches::unregisterFunctors(uint32_t functorCount) {
    if (functorCount > mFunctorsCount) {
        mFunctorsCount = 0;
    } else {
        mFunctorsCount -= functorCount;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Regions
///////////////////////////////////////////////////////////////////////////////

TextureVertex* Caches::getRegionMesh() {
    // Create the mesh, 2 triangles and 4 vertices per rectangle in the region
    if (!mRegionMesh) {
        mRegionMesh.reset(new TextureVertex[kMaxNumberOfQuads * 4]);
    }

    return mRegionMesh.get();
}

///////////////////////////////////////////////////////////////////////////////
// Temporary Properties
///////////////////////////////////////////////////////////////////////////////

}; // namespace uirenderer
}; // namespace android

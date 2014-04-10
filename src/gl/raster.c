#include "raster.h"
#include "murmur3.h"
#include "texture.h"

static rasterpos_t rPos = {0};
static viewport_t viewport = {0};
static GLubyte *raster = NULL;

static khash_t(tex) *cache;

/* raster engine:
    we render pixels to memory somewhere
    until someone else wants to use the framebuffer
    then we throw 'em quickly into a texture, render to the whole screen
    then let the other function do their thing
*/

uint32_t hash_pixels(GLsizei width, GLsizei height, const GLvoid *pixels) {
    return murmur3((const char *)pixels, width * height * 4, 0);
}

static void texture_cache_put(uint32_t hash, gltexture_t *tex) {
    // TODO: what if this texture gets deleted?
    if (! tex)
        return;

    printf("cache put: %d\n", hash);

    int ret;
    if (! cache) {
        cache = kh_init(tex);
        // segfaults if we don't do a single put
        kh_put(tex, cache, 1, &ret);
        kh_del(tex, cache, 1);
    }

    khint_t k = kh_put(tex, cache, hash, &ret);
    kh_value(cache, k) = tex;
}

static gltexture_t *texture_cache_get(uint32_t hash) {
    if (! cache)
        return NULL;

    khint_t k;
    k = kh_get(tex, cache, hash);
    if (k != kh_end(cache)) {
        return kh_value(cache, k);
    }
    return NULL;
}

static GLuint texture_cache(GLsizei width, GLsizei height, const GLvoid *pixels) {

    GLuint texture;

    glPushAttrib(GL_TEXTURE_BIT);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glPopClientAttrib();
    glPopAttrib();
    return texture;

    gltexture_t *tex;
    uint32_t hash = hash_pixels(width, height, pixels);
    tex = texture_cache_get(hash);
    // TODO: pixel-by-pixel test for collisions?
    if (tex) {
        if (tex->width == width && tex->height == height) {
            printf("cache hit: %d\n", hash);
            return tex->texture;
        }
    } else {
        GLuint texture;

        glPushAttrib(GL_TEXTURE_BIT);
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);

        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);

        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        printf("loading texture (%d x %d)\n", width, height);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        texture_cache_put(hash, state.texture.bound);

        glPopClientAttrib();
        glPopAttrib();
        return texture;
    }
    return 0;
}

void render_pixels(GLint x, GLint y, GLsizei width, GLsizei height, const GLvoid *pixels) {
    GLuint texture = texture_cache(width, height, pixels);

    glPushAttrib(GL_TEXTURE_BIT | GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(viewport.x, viewport.x + viewport.width, viewport.y + viewport.height, viewport.y, 0, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    GLfloat vert[] = {
        x, y, 0,
        x, y + height, 0,
        x + width, y, 0,
        x + width, y + height, 0,
    };

    float sw = width / (GLfloat)npot(width);
    float sh = height / (GLfloat)npot(height);

    GLfloat tex[] = {
        0, 0,
        0, sh,
        sw, 0,
        sw, sh,
    };

    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT | GL_CLIENT_PIXEL_STORE_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, vert);
    glTexCoordPointer(2, GL_FLOAT, 0, tex);

    glEnable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    LOAD_GLES(glDrawArrays);
    gles_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glPopClientAttrib();

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();

    glDeleteTextures(1, &texture);
}

void glRasterPos3f(GLfloat x, GLfloat y, GLfloat z) {
    rPos.x = x;
    rPos.y = y;
    rPos.z = z;
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    PUSH_IF_COMPILING(glViewport);
    LOAD_GLES(glViewport);
    if (raster) {
        render_raster();
    }
    gles_glViewport(x, y, width, height);
    viewport.x = x;
    viewport.y = y;
    viewport.width = width;
    viewport.height = height;
    viewport.nwidth = npot(width);
    viewport.nheight = npot(height);
}

void init_raster() {
    if (!viewport.width || !viewport.height) {
        glGetIntegerv(GL_VIEWPORT, (GLint *)&viewport);
        viewport.nwidth = npot(viewport.width);
        viewport.nheight = npot(viewport.height);
    }
    if (!raster) {
        raster = (GLubyte *)malloc(4 * viewport.nwidth * viewport.nheight * sizeof(GLubyte));
    }
}

void glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig,
              GLfloat xmove, GLfloat ymove, const GLubyte *bitmap) {
    // TODO: shouldn't be drawn if the raster pos is outside the viewport?
    // TODO: negative width/height mirrors bitmap?
    if (!width && !height) {
        rPos.x += xmove;
        rPos.y -= ymove;
        return;
    }
    int rwidth = width + 8 - width % 8;
    GLuint *pixels = calloc(1, rwidth * height * sizeof(GLuint));

    const GLubyte *from;
    GLuint *to = pixels;
    int x, y;

    // copy to pixel data
    // TODO: strip blank lines and mirror vertically?
    // printf("\nbitmap:\n");
    for (y = 0; y < height; y++) {
        to = (GLuint *)raster + (GLuint)((height - y) * rwidth);
        from = bitmap + (y * 2);
        for (x = 0; x < width; x += 8) {
            GLubyte b = *from++;
            for (int j = 8; j--; ) {
                // printf("%d", (b & (1 << j)) ? 1 : 0);
                *to++ = (b & (1 << j)) ? 0xFFFFFFFF : 0;
            }
            // printf("\n");
        }
    }

    render_pixels(rPos.x, rPos.y, rwidth, height, pixels);
    free(pixels);

    rPos.x += xmove;
    rPos.y += ymove;
}

void glDrawPixels(GLsizei width, GLsizei height, GLenum format,
                  GLenum type, const GLvoid *data) {
    GLubyte *pixels, *from, *to;
    GLvoid *dst = NULL;

    init_raster();
    if (! pixel_convert(data, &dst, width, height,
                        format, type, GL_RGBA, GL_UNSIGNED_BYTE)) {
        return;
    }
    pixels = (GLubyte *)dst;
    render_pixels(rPos.x, rPos.y, width, height, pixels);
    if (pixels != data) {
        free(pixels);
    }
}

void render_raster() {
    return;
    if (!viewport.width || !viewport.height || !raster)
        return;

// FIXME
#ifndef USE_ES2
    glPushAttrib(GL_TEXTURE_BIT | GL_ENABLE_BIT);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    GLfloat vert[] = {
        -1, -1, 0,
        1, -1, 0,
        1, 1, 0,
        -1, 1, 0,
    };

    float sw = viewport.width / (GLfloat)viewport.nwidth;
    float sh = viewport.height / (GLfloat)viewport.nheight;

    GLfloat tex[] = {
        0, sh,
        sw, sh,
        sw, 0,
        0, 0,
    };

    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT | GL_CLIENT_PIXEL_STORE_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, vert);
    glTexCoordPointer(2, GL_FLOAT, 0, tex);

    glEnable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, viewport.nwidth, viewport.nheight,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, raster);

    LOAD_GLES(glDrawArrays);
    gles_glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDeleteTextures(1, &texture);

    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glPopClientAttrib();

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
#endif
    free(raster);
    raster = NULL;
}

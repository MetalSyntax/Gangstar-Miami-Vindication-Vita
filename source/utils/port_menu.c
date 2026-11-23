/**
 * @file  port_menu.c
 * @brief SELECT+START port options menu (Fase 65), modelled on the Zenonia2-vita
 *        (loader/controls.c) and Inotia4-iOS-vita (source/input.c) menus:
 *        per-action button remap (X replace, [] add, /\ unbind), touch-controls
 *        visibility, frame-rate cap, draw distance, aspect ratio, rear-touch
 *        quadrants as extra bindable buttons, restore defaults, saved to
 *        DATA_PATH "port_menu.txt".
 *
 * While open the game is frozen (main.c skips nativeRender) and the menu is
 * drawn with the GLES1 fixed pipeline over a 480x272 texture scaled 2x.
 */

#include "utils/port_menu.h"
#include "utils/gamepad_actions.h"
#include "utils/perf_lod.h"
#include "utils/logger.h"
#include "reimpl/gl.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <stdio.h>
#include <string.h>
#include <vitaGL.h>

#define MENU_FILE DATA_PATH "port_menu.txt"

extern const unsigned char menu_font8x8[];
extern uint32_t vsync_interval; /* vitaGL (vgl.c): 1 = 60 Hz, 2 = 30 Hz */

static int s_open, s_dirty = 1, s_cursor, s_capture; /* capture: 0 none, 1 replace, 2 add */
static int s_showTouch, s_fps30, s_lodNear, s_aspectOrig;

static const struct { uint32_t bit; const char *name; } s_btns[] = {
    { SCE_CTRL_CROSS, "CROSS" },   { SCE_CTRL_CIRCLE, "CIRCLE" },
    { SCE_CTRL_SQUARE, "SQUARE" }, { SCE_CTRL_TRIANGLE, "TRIANGLE" },
    { SCE_CTRL_LTRIGGER, "L" },    { SCE_CTRL_RTRIGGER, "R" },
    { SCE_CTRL_L2, "REAR-TL" },    { SCE_CTRL_R2, "REAR-TR" },
    { SCE_CTRL_L3, "REAR-BL" },    { SCE_CTRL_R3, "REAR-BR" },
};
#define NBTNS (int)(sizeof(s_btns) / sizeof(s_btns[0]))

enum { ROW_TOUCH, ROW_FPS, ROW_LOD, ROW_ASPECT, ROW_DEFAULTS, ROW_CLOSE, NEXTRA };
#define NROWS (gamepad_action_count() + NEXTRA)

static void apply(void) {
    gamepad_set_always_show(s_showTouch);
    vsync_interval = s_fps30 ? 2 : 1;
    perf_lod_set_near(s_lodNear);
    gl_aspect_original = s_aspectOrig;
}

static void save(void) {
    FILE *f = fopen(MENU_FILE, "w");
    if (!f) {
        l_warn("[menu] cannot write " MENU_FILE);
        return;
    }
    for (int i = 0; i < gamepad_action_count(); i++)
        fprintf(f, "BIND%d %u\n", i, (unsigned)gamepad_action_mask(i));
    fprintf(f, "SHOW_TOUCH %d\nFPS30 %d\nLOD_NEAR %d\nASPECT_ORIGINAL %d\n",
            s_showTouch, s_fps30, s_lodNear, s_aspectOrig);
    fclose(f);
}

void port_menu_init(void) {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    FILE *f = fopen(MENU_FILE, "r");
    if (f) {
        char key[32];
        unsigned v;
        while (fscanf(f, "%31s %u", key, &v) == 2) {
            int i;
            if (sscanf(key, "BIND%d", &i) == 1 && i >= 0 && i < gamepad_action_count())
                gamepad_action_set_mask(i, v);
            else if (!strcmp(key, "SHOW_TOUCH")) s_showTouch = !!v;
            else if (!strcmp(key, "FPS30")) s_fps30 = !!v;
            else if (!strcmp(key, "LOD_NEAR")) s_lodNear = !!v;
            else if (!strcmp(key, "ASPECT_ORIGINAL")) s_aspectOrig = !!v;
        }
        fclose(f);
    }
    apply();
    l_note("[menu] SELECT+START menu ready (touch=%d fps30=%d lodNear=%d aspectOrig=%d)",
           s_showTouch, s_fps30, s_lodNear, s_aspectOrig);
}

uint32_t port_menu_rear_buttons(void) {
    SceTouchData t;
    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &t, 1) < 0)
        return 0;
    uint32_t b = 0;
    for (int i = 0; i < (int)t.reportNum; i++) {
        int right = t.report[i].x >= 960, bottom = t.report[i].y >= 500; /* rear panel ~1920x890 */
        b |= bottom ? (right ? SCE_CTRL_R3 : SCE_CTRL_L3) : (right ? SCE_CTRL_R2 : SCE_CTRL_L2);
    }
    return b;
}

int port_menu_active(void) { return s_open; }

void port_menu_open(void) {
    s_open = 1;
    s_cursor = 0;
    s_capture = 0;
    s_dirty = 1;
}

void port_menu_update(uint32_t pressed) {
    int na = gamepad_action_count();
    if (s_capture) {
        if (pressed & SCE_CTRL_START) {
            s_capture = 0;
        } else {
            for (int b = 0; b < NBTNS; b++)
                if (pressed & s_btns[b].bit) {
                    uint32_t m = s_capture == 2 ? gamepad_action_mask(s_cursor) : 0;
                    gamepad_action_set_mask(s_cursor, m | s_btns[b].bit);
                    s_capture = 0;
                    break;
                }
        }
        s_dirty = 1;
        return;
    }
    if (!pressed)
        return;
    s_dirty = 1;
    if (pressed & SCE_CTRL_UP) s_cursor = (s_cursor + NROWS - 1) % NROWS;
    if (pressed & SCE_CTRL_DOWN) s_cursor = (s_cursor + 1) % NROWS;
    if (pressed & SCE_CTRL_CIRCLE) goto close;
    if (s_cursor < na) {
        if (pressed & SCE_CTRL_CROSS) s_capture = 1;
        else if (pressed & SCE_CTRL_SQUARE) s_capture = 2;
        else if (pressed & SCE_CTRL_TRIANGLE) gamepad_action_set_mask(s_cursor, 0);
        return;
    }
    if (!(pressed & (SCE_CTRL_CROSS | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
        return;
    switch (s_cursor - na) {
    case ROW_TOUCH: s_showTouch ^= 1; break;
    case ROW_FPS: s_fps30 ^= 1; break;
    case ROW_LOD: s_lodNear ^= 1; break;
    case ROW_ASPECT: s_aspectOrig ^= 1; break;
    case ROW_DEFAULTS:
        if (!(pressed & SCE_CTRL_CROSS)) return;
        for (int i = 0; i < na; i++)
            gamepad_action_set_mask(i, gamepad_action_default_mask(i));
        s_showTouch = s_fps30 = s_lodNear = s_aspectOrig = 0;
        break;
    case ROW_CLOSE:
        if (pressed & SCE_CTRL_CROSS) goto close;
        return;
    }
    apply();
    return;
close:
    apply();
    save();
    s_open = 0;
    l_note("[menu] closed and saved");
}

/* ---- drawing ---------------------------------------------------------- */

#define MW 480
#define MH 272
static uint32_t s_px[MW * MH]; /* ABGR (GL_RGBA/UNSIGNED_BYTE, little endian) */
static GLuint s_tex;

static void fill(int y, int h, uint32_t c) {
    for (int i = y * MW; i < (y + h) * MW; i++) s_px[i] = c;
}

static void text(int x, int y, uint32_t c, const char *s) {
    for (; *s && x + 8 <= MW; s++, x += 8)
        for (int r = 0; r < 8; r++) {
            unsigned char bits = menu_font8x8[(unsigned char)*s * 8 + r];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col)) s_px[(y + r) * MW + x + col] = c;
        }
}

static void render(void) {
    static const char *extra[NEXTRA] = { "Touch controls (L+R)", "Frame rate", "Draw distance",
                                         "Aspect ratio", "Restore defaults", "Save and close" };
    int na = gamepad_action_count();
    fill(0, MH, 0xFF201008);
    text(16, 10, 0xFF40C0FF, "GANGSTAR MV - PORT OPTIONS");
    for (int r = 0; r < NROWS; r++) {
        int y = 30 + r * 15;
        if (r == s_cursor) fill(y - 4, 15, 0xFF603818);
        char val[96] = "";
        if (r < na) {
            text(16, y, 0xFFE0E0E0, gamepad_action_name(r));
            if (r == s_cursor && s_capture) {
                strcpy(val, s_capture == 2 ? "press a button to add..." : "press a button...");
            } else {
                uint32_t m = gamepad_action_mask(r);
                for (int b = 0; b < NBTNS; b++)
                    if (m & s_btns[b].bit) {
                        if (val[0]) strcat(val, ", ");
                        strcat(val, s_btns[b].name);
                    }
                if (!val[0]) strcpy(val, "NONE");
            }
            text(136, y, 0xFFE0E0E0, val);
            continue;
        }
        int e = r - na;
        text(16, y, 0xFFE0E0E0, extra[e]);
        const char *v = e == ROW_TOUCH  ? (s_showTouch ? "SHOWN" : "HIDDEN (0%)")
                      : e == ROW_FPS    ? (s_fps30 ? "30 FPS (stable)" : "60 FPS (vsync)")
                      : e == ROW_LOD    ? (s_lodNear ? "NEAR (faster)" : "FAR")
                      : e == ROW_ASPECT ? (s_aspectOrig ? "ORIGINAL (960x480)" : "FULL SCREEN")
                                        : "";
        text(200, y, 0xFFE0E0E0, v);
    }
    text(16, 236, 0xFF909090, "X replace/toggle  [] add  /\\ unbind  O save&close");
    text(16, 250, 0xFF909090, "Rear touch quadrants = REAR-TL/TR/BL/BR buttons");
}

void port_menu_draw(void) {
    GLint texBefore;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texBefore);
    if (!s_tex) {
        glGenTextures(1, &s_tex);
        glBindTexture(GL_TEXTURE_2D, s_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        s_dirty = 1;
    }
    glBindTexture(GL_TEXTURE_2D, s_tex);
    if (s_dirty) {
        render();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MW, MH, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_px);
        s_dirty = 0;
    }

    /* The engine is frozen mid-state: draw on a clean slate and restore what
     * it could rely on (it re-sets client pointers per mesh, not caps). */
    static const GLenum caps[] = { GL_DEPTH_TEST, GL_BLEND, GL_ALPHA_TEST, GL_CULL_FACE,
                                   GL_SCISSOR_TEST, GL_LIGHTING, GL_FOG, GL_TEXTURE_2D };
    GLboolean was[sizeof(caps) / sizeof(caps[0])];
    for (unsigned i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        was[i] = glIsEnabled(caps[i]);
        glDisable(caps[i]);
    }
    GLint vp[4], abuf, ebuf, fbo, oldTex, oldEnv;
    oldTex = texBefore;
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &oldEnv);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebuf);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    GLboolean colArr = glIsEnabled(GL_COLOR_ARRAY), nrmArr = glIsEnabled(GL_NORMAL_ARRAY);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    static const float verts[] = { 0, 0, 960, 0, 0, 544, 960, 544 };
    static const float uvs[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glBindTexture(GL_TEXTURE_2D, oldTex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, oldEnv);
    if (colArr) glEnableClientState(GL_COLOR_ARRAY);
    if (nrmArr) glEnableClientState(GL_NORMAL_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, abuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebuf);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    for (unsigned i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
        if (was[i]) glEnable(caps[i]);
}

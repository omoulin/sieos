/* pkg-config: xkbcommon epoxy */
/*
 * wtest-input (make webdeps-test): libxkbcommon as WebKit's key events use it
 * (key names, their characters, a keymap given as text, as SIEOS's platform
 * gives it, keys pressed with Shift), and libepoxy linked (no GL on SIEOS:
 * only its string helper runs).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <epoxy/gl.h>
#include <stdio.h>
#include <xkbcommon/xkbcommon.h>

static int fails;
static void check(const char *what, int ok) { printf("wtest input: %s: %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }

static const char keymap[] =
    "xkb_keymap {\n"
    " xkb_keycodes { <AE01> = 10; <AC01> = 38; <LFSH> = 50; <RTRN> = 36; };\n"
    " xkb_types { virtual_modifiers NumLock;\n"
    "  type \"ONE_LEVEL\" { modifiers = none; map[none] = Level1; };\n"
    "  type \"TWO_LEVEL\" { modifiers = Shift; map[Shift] = Level2; }; };\n"
    " xkb_compat { interpret Shift_L { action = SetMods(modifiers=Shift); }; };\n"
    " xkb_symbols {\n"
    "  key <AE01> { type = \"TWO_LEVEL\", [ 1, exclam ] }; key <AC01> { type = \"TWO_LEVEL\", [ a, A ] };\n"
    "  key <RTRN> { [ Return ] }; key <LFSH> { [ Shift_L ] }; modifier_map Shift { <LFSH> }; };\n"
    "};\n";

int main(void)
{
    check("key names", xkb_keysym_from_name("Return", 0) == XKB_KEY_Return &&
                       xkb_keysym_from_name("eacute", 0) == XKB_KEY_eacute);
    check("characters", xkb_keysym_to_utf32(XKB_KEY_eacute) == 0xe9 && xkb_keysym_to_utf32(XKB_KEY_EuroSign) == 0x20ac &&
                        xkb_keysym_to_upper(XKB_KEY_eacute) == XKB_KEY_Eacute);
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES | XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
    struct xkb_keymap *km = ctx ? xkb_keymap_new_from_string(ctx, keymap, XKB_KEYMAP_FORMAT_TEXT_V1, 0) : 0;
    check("keymap from text", km != 0);
    if (km) {
        struct xkb_state *st = xkb_state_new(km);
        check("keys", xkb_state_key_get_one_sym(st, 38) == XKB_KEY_a && xkb_state_key_get_utf32(st, 10) == '1' &&
                      xkb_state_key_get_one_sym(st, 36) == XKB_KEY_Return);
        xkb_state_update_key(st, 50, XKB_KEY_DOWN);
        check("keys with Shift", xkb_state_key_get_one_sym(st, 38) == XKB_KEY_A && xkb_state_key_get_utf32(st, 10) == '!');
        xkb_state_update_key(st, 50, XKB_KEY_UP);
        check("Shift released", xkb_state_key_get_utf32(st, 10) == '1');
        xkb_state_unref(st);
        xkb_keymap_unref(km);
    }
    xkb_context_unref(ctx);
    check("libepoxy", epoxy_extension_in_string("GL_OES_a GL_OES_texture_npot", "GL_OES_texture_npot") &&
                      !epoxy_extension_in_string("GL_OES_texture_npot2", "GL_OES_texture_npot"));
    printf("wtest input: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}

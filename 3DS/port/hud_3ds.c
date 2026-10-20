// Bottom screen HUD text. The minimap (dlls/engine/49) queues its labels and
// area name into text box 0x83, which gameTextRun draws later in the frame;
// textrender_run.c is patched to call these wrappers, which send that box's
// text to the bottom screen like the minimap itself (gx_hud_begin).

#include <stddef.h>
#include "gx_init.h"
#include "main/gametext_internal.h"

#define MINIMAP_TEXT_BOX 0x83

void gameTextRenderStrs(char* str, int boxIdx);
void gameTextRenderById(int textId, int x, int y);

// The box gameTextRenderById will use. Looks the text up without
// gameTextGet, which has side effects while the fonts are not loaded.
static int text_box_for_id(int textId) {
    if (gCurTextBox != NULL) return (int)(((u8*)gCurTextBox - (u8*)gTextBoxes) / 0x20);
    TextFont* fonts = gameTextFonts;
    if (fonts == NULL || fonts->status != 2) return -1;
    for (int i = 0; i < fonts->entryCount; i++) {
        if (fonts->entries[i].identifier == textId) {
            int box = fonts->entries[i].boxId;
            return box == 255 ? 2 : box;
        }
    }
    return -1;
}

void port_text_render_by_id(int textId, int x, int y) {
    int hud = text_box_for_id(textId) == MINIMAP_TEXT_BOX && gx_hud_begin();
    gameTextRenderById(textId, x, y);
    if (hud) gx_hud_end();
}

void port_text_render_strs(char* str, int boxIdx) {
    int hud = boxIdx == MINIMAP_TEXT_BOX && gx_hud_begin();
    gameTextRenderStrs(str, boxIdx);
    if (hud) gx_hud_end();
}

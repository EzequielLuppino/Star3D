# Bottom screen HUD: text in the minimap's box is drawn on the bottom screen
# (port/hud_3ds.c)
1i\
void port_text_render_by_id(int textId, int x, int y);\
void port_text_render_strs(char* str, int boxIdx);
s|\bgameTextRenderById(|port_text_render_by_id(|g
s|\bgameTextRenderStrs(|port_text_render_strs(|g

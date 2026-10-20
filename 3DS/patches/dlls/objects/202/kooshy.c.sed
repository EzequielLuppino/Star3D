# One-hit-kill cheat (touch UI, port/touchui_3ds.c): any hit empties health
1i\
extern int port_cheat_ohk;
/if (damage > .*->current)/i\
        if (port_cheat_ohk) damage = 0x7fff;

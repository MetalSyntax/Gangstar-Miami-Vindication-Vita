#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SELECT+START port options menu (Fase 65), see port_menu.c. */
void port_menu_init(void);          /* load DATA_PATH "port_menu.txt" and apply */
int port_menu_active(void);
void port_menu_open(void);
void port_menu_update(uint32_t pressed); /* rising-edge buttons */
void port_menu_draw(void);          /* call instead of nativeRender while active */
uint32_t port_menu_rear_buttons(void); /* rear quadrants as SCE_CTRL_L2/R2/L3/R3 */

#ifdef __cplusplus
}
#endif

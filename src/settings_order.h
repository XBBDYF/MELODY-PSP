#ifndef MELODY_SETTINGS_ORDER_H
#define MELODY_SETTINGS_ORDER_H
/* Keep stored/action IDs stable while presenting related controls together.
 * Row 8 is the new local-MV switch (id 24), grouped with the video rows. */
static const unsigned char melody_settings_order[25]={0,1,21,12,13,16,14,17,24,15,4,2,11,6,7,3,22,23,5,20,8,19,9,18,10};
static inline int melody_setting_id(int row){return row>=0&&row<25?melody_settings_order[row]:-1;}
#endif

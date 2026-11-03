/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* X11 keys as Windows key codes, so keyboard_map (main.c) means the same keys on both systems: backend_x11.c's keymap
   and the Play screen's Controls > Keyboard page on Linux (dread_ui/fe.cpp). Keys go by the layout's symbol, as Windows
   key codes do (an AZERTY A is A). Needs <X11/keysym.h> first */
#ifndef X11_VK_H
#define X11_VK_H

static const struct { unsigned long ks; int vk; const char* name; } X11_VK[]={
  {XK_BackSpace,8,"Backspace"},{XK_Tab,9,"Tab"},{XK_ISO_Left_Tab,9,"Tab"},{XK_Return,13,"Enter"},{XK_KP_Enter,13,"Enter"},
  {XK_Shift_L,16,"Shift"},{XK_Shift_R,16,"Shift"},{XK_Control_L,17,"Ctrl"},{XK_Control_R,17,"Ctrl"},{XK_Alt_L,18,"Alt"},{XK_Alt_R,18,"Alt"},
  {XK_ISO_Level3_Shift,18,"Alt"},{XK_Pause,19,"Pause"},{XK_Caps_Lock,20,"Caps Lock"},{XK_Escape,27,"Esc"},{XK_space,32,"Space"},
  {XK_Prior,33,"Page Up"},{XK_Next,34,"Page Down"},{XK_End,35,"End"},{XK_Home,36,"Home"},
  {XK_Left,37,"Left arrow"},{XK_Up,38,"Up arrow"},{XK_Right,39,"Right arrow"},{XK_Down,40,"Down arrow"},{XK_Insert,45,"Insert"},{XK_Delete,46,"Delete"},
  {XK_Super_L,0x5B,"Super"},{XK_Super_R,0x5C,"Super"},{XK_Menu,0x5D,"Menu"},
  /* the keypad with Num Lock on and off */
  {XK_KP_0,0x60,"Num 0"},{XK_KP_1,0x61,"Num 1"},{XK_KP_2,0x62,"Num 2"},{XK_KP_3,0x63,"Num 3"},{XK_KP_4,0x64,"Num 4"},
  {XK_KP_5,0x65,"Num 5"},{XK_KP_6,0x66,"Num 6"},{XK_KP_7,0x67,"Num 7"},{XK_KP_8,0x68,"Num 8"},{XK_KP_9,0x69,"Num 9"},
  {XK_KP_Insert,0x60,"Num 0"},{XK_KP_End,0x61,"Num 1"},{XK_KP_Down,0x62,"Num 2"},{XK_KP_Next,0x63,"Num 3"},{XK_KP_Left,0x64,"Num 4"},
  {XK_KP_Begin,0x65,"Num 5"},{XK_KP_Right,0x66,"Num 6"},{XK_KP_Home,0x67,"Num 7"},{XK_KP_Up,0x68,"Num 8"},{XK_KP_Prior,0x69,"Num 9"},
  {XK_KP_Multiply,0x6A,"Num *"},{XK_KP_Add,0x6B,"Num +"},{XK_KP_Subtract,0x6D,"Num -"},{XK_KP_Decimal,0x6E,"Num ."},{XK_KP_Delete,0x6E,"Num ."},{XK_KP_Divide,0x6F,"Num /"},
  {XK_Num_Lock,0x90,"Num Lock"},{XK_Scroll_Lock,0x91,"Scroll Lock"},
  {XK_semicolon,0xBA,";"},{XK_equal,0xBB,"="},{XK_comma,0xBC,","},{XK_minus,0xBD,"-"},{XK_period,0xBE,"."},{XK_slash,0xBF,"/"},
  {XK_grave,0xC0,"`"},{XK_dead_grave,0xC0,"`"},{XK_bracketleft,0xDB,"["},{XK_backslash,0xDC,"\\"},{XK_bracketright,0xDD,"]"},
  {XK_apostrophe,0xDE,"'"},{XK_dead_acute,0xDE,"'"},{XK_ccedilla,0xBA,"\xC3\x87"},{XK_less,0xE2,"<"},
};

/* the Windows key code of an X11 key (XLookupKeysym(e,0)), 0 = none */
static int x11_vk(unsigned long ks){
  if(ks>=XK_a && ks<=XK_z) return 'A'+(int)(ks-XK_a);
  if(ks>=XK_A && ks<=XK_Z) return 'A'+(int)(ks-XK_A);
  if(ks>=XK_0 && ks<=XK_9) return '0'+(int)(ks-XK_0);
  if(ks>=XK_F1 && ks<=XK_F24) return 0x70+(int)(ks-XK_F1);
  for(unsigned i=0;i<sizeof X11_VK/sizeof X11_VK[0];i++) if(X11_VK[i].ks==ks) return X11_VK[i].vk;
  return 0; }

/* a key's name for the Controls page: buf when it has to be built */
static const char* x11_vk_name(int vk, char* buf, int n){
  if(!vk) return "-";
  if((vk>='A' && vk<='Z') || (vk>='0' && vk<='9')){ buf[0]=(char)vk; buf[1]=0; return buf; }
  if(vk>=0x70 && vk<=0x87){ snprintf(buf,n,"F%d",vk-0x6F); return buf; }
  for(unsigned i=0;i<sizeof X11_VK/sizeof X11_VK[0];i++) if(X11_VK[i].vk==vk) return X11_VK[i].name;
  snprintf(buf,n,"Key %d",vk); return buf; }

#endif

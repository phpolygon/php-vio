/*
 * php-vio - PHP Video Input Output
 * All VIO_* constants (keys mapped to GLFW values)
 */

#ifndef VIO_CONSTANTS_H
#define VIO_CONSTANTS_H

/* ── Keyboard keys (GLFW-compatible values) ───────────────────────── */

#define VIO_KEY_UNKNOWN       -1
#define VIO_KEY_SPACE         32
#define VIO_KEY_APOSTROPHE    39
#define VIO_KEY_COMMA         44
#define VIO_KEY_MINUS         45
#define VIO_KEY_PERIOD        46
#define VIO_KEY_SLASH         47
#define VIO_KEY_0             48
#define VIO_KEY_1             49
#define VIO_KEY_2             50
#define VIO_KEY_3             51
#define VIO_KEY_4             52
#define VIO_KEY_5             53
#define VIO_KEY_6             54
#define VIO_KEY_7             55
#define VIO_KEY_8             56
#define VIO_KEY_9             57
#define VIO_KEY_SEMICOLON     59
#define VIO_KEY_EQUAL         61
#define VIO_KEY_A             65
#define VIO_KEY_B             66
#define VIO_KEY_C             67
#define VIO_KEY_D             68
#define VIO_KEY_E             69
#define VIO_KEY_F             70
#define VIO_KEY_G             71
#define VIO_KEY_H             72
#define VIO_KEY_I             73
#define VIO_KEY_J             74
#define VIO_KEY_K             75
#define VIO_KEY_L             76
#define VIO_KEY_M             77
#define VIO_KEY_N             78
#define VIO_KEY_O             79
#define VIO_KEY_P             80
#define VIO_KEY_Q             81
#define VIO_KEY_R             82
#define VIO_KEY_S             83
#define VIO_KEY_T             84
#define VIO_KEY_U             85
#define VIO_KEY_V             86
#define VIO_KEY_W             87
#define VIO_KEY_X             88
#define VIO_KEY_Y             89
#define VIO_KEY_Z             90
#define VIO_KEY_LEFT_BRACKET  91
#define VIO_KEY_BACKSLASH     92
#define VIO_KEY_RIGHT_BRACKET 93
#define VIO_KEY_GRAVE_ACCENT  96
#define VIO_KEY_ESCAPE        256
#define VIO_KEY_ENTER         257
#define VIO_KEY_TAB           258
#define VIO_KEY_BACKSPACE     259
#define VIO_KEY_INSERT        260
#define VIO_KEY_DELETE        261
#define VIO_KEY_RIGHT         262
#define VIO_KEY_LEFT          263
#define VIO_KEY_DOWN          264
#define VIO_KEY_UP            265
#define VIO_KEY_PAGE_UP       266
#define VIO_KEY_PAGE_DOWN     267
#define VIO_KEY_HOME          268
#define VIO_KEY_END           269
#define VIO_KEY_CAPS_LOCK     280
#define VIO_KEY_SCROLL_LOCK   281
#define VIO_KEY_NUM_LOCK      282
#define VIO_KEY_PRINT_SCREEN  283
#define VIO_KEY_PAUSE         284
#define VIO_KEY_F1            290
#define VIO_KEY_F2            291
#define VIO_KEY_F3            292
#define VIO_KEY_F4            293
#define VIO_KEY_F5            294
#define VIO_KEY_F6            295
#define VIO_KEY_F7            296
#define VIO_KEY_F8            297
#define VIO_KEY_F9            298
#define VIO_KEY_F10           299
#define VIO_KEY_F11           300
#define VIO_KEY_F12           301
#define VIO_KEY_KP_0          320
#define VIO_KEY_KP_1          321
#define VIO_KEY_KP_2          322
#define VIO_KEY_KP_3          323
#define VIO_KEY_KP_4          324
#define VIO_KEY_KP_5          325
#define VIO_KEY_KP_6          326
#define VIO_KEY_KP_7          327
#define VIO_KEY_KP_8          328
#define VIO_KEY_KP_9          329
#define VIO_KEY_KP_DECIMAL    330
#define VIO_KEY_KP_DIVIDE     331
#define VIO_KEY_KP_MULTIPLY   332
#define VIO_KEY_KP_SUBTRACT   333
#define VIO_KEY_KP_ADD        334
#define VIO_KEY_KP_ENTER      335
#define VIO_KEY_KP_EQUAL      336
#define VIO_KEY_LEFT_SHIFT    340
#define VIO_KEY_LEFT_CONTROL  341
#define VIO_KEY_LEFT_ALT      342
#define VIO_KEY_LEFT_SUPER    343
#define VIO_KEY_RIGHT_SHIFT   344
#define VIO_KEY_RIGHT_CONTROL 345
#define VIO_KEY_RIGHT_ALT     346
#define VIO_KEY_RIGHT_SUPER   347
#define VIO_KEY_MENU          348
#define VIO_KEY_LAST          VIO_KEY_MENU

/* ── Modifier keys (bitmask) ──────────────────────────────────────── */

#define VIO_MOD_SHIFT     0x0001
#define VIO_MOD_CONTROL   0x0002
#define VIO_MOD_ALT       0x0004
#define VIO_MOD_SUPER     0x0008
#define VIO_MOD_CAPS_LOCK 0x0010
#define VIO_MOD_NUM_LOCK  0x0020

#define VIO_MOUSE_LAST        7

/* ── Gamepad buttons ──────────────────────────────────────────────── */

#define VIO_GAMEPAD_A             0
#define VIO_GAMEPAD_B             1
#define VIO_GAMEPAD_X             2
#define VIO_GAMEPAD_Y             3
#define VIO_GAMEPAD_LEFT_BUMPER   4
#define VIO_GAMEPAD_RIGHT_BUMPER  5
#define VIO_GAMEPAD_BACK          6
#define VIO_GAMEPAD_START         7
#define VIO_GAMEPAD_GUIDE         8
#define VIO_GAMEPAD_LEFT_THUMB    9
#define VIO_GAMEPAD_RIGHT_THUMB   10
#define VIO_GAMEPAD_DPAD_UP       11
#define VIO_GAMEPAD_DPAD_RIGHT    12
#define VIO_GAMEPAD_DPAD_DOWN     13
#define VIO_GAMEPAD_DPAD_LEFT     14

/* ── Gamepad axes ─────────────────────────────────────────────────── */

#define VIO_GAMEPAD_AXIS_LEFT_X        0
#define VIO_GAMEPAD_AXIS_LEFT_Y        1
#define VIO_GAMEPAD_AXIS_RIGHT_X       2
#define VIO_GAMEPAD_AXIS_RIGHT_Y       3
#define VIO_GAMEPAD_AXIS_LEFT_TRIGGER  4
#define VIO_GAMEPAD_AXIS_RIGHT_TRIGGER 5

#endif /* VIO_CONSTANTS_H */

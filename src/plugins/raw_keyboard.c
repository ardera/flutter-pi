#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <flutter-pi.h>
#include <pluginregistry.h>
#include <keyboard.h>
#include <ctype.h>

#include <linux/input.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <plugins/raw_keyboard.h>

ATTR_CONST static uint64_t apply_key_plane(uint64_t keycode, uint64_t plane) {
    return (keycode & 0x000FFFFFFFF) | plane;
}

ATTR_CONST static uint64_t apply_gtk_key_plane(uint64_t keycode) {
    return apply_key_plane(keycode, 0x01500000000);
}

ATTR_CONST static uint64_t apply_unicode_key_plane(uint64_t keycode) {
    return apply_key_plane(keycode, 0x00000000000);
}

ATTR_CONST static uint64_t apply_flutter_key_plane(uint64_t keycode) {
    return apply_key_plane(keycode, 0x00200000000);
}

ATTR_CONST static uint64_t physical_key_for_evdev_keycode(uint16_t evdev_keycode) {
    // clang-format off
    static const uint32_t physical_keys[] = {
        [KEY_RESERVED] = 0,
        [KEY_ESC] = 0x00070029,  // escape
        [KEY_1] = 0x0007001e,  // digit1
        [KEY_2] = 0x0007001f,  // digit2
        [KEY_3] = 0x00070020,  // digit3
        [KEY_4] = 0x00070021,  // digit4
        [KEY_5] = 0x00070022,  // digit5
        [KEY_6] = 0x00070023,  // digit6
        [KEY_7] = 0x00070024,  // digit7
        [KEY_8] = 0x00070025,  // digit8
        [KEY_9] = 0x00070026,  // digit9
        [KEY_0] = 0x00070027,  // digit0
        [KEY_MINUS] = 0x0007002d,  // minus
        [KEY_EQUAL] = 0x0007002e,  // equal
        [KEY_BACKSPACE] = 0x0007002a,  // backspace
        [KEY_TAB] = 0x0007002b,  // tab
        [KEY_Q] = 0x00070014,  // keyQ
        [KEY_W] = 0x0007001a,  // keyW
        [KEY_E] = 0x00070008,  // keyE
        [KEY_R] = 0x00070015,  // keyR
        [KEY_T] = 0x00070017,  // keyT
        [KEY_Y] = 0x0007001c,  // keyY
        [KEY_U] = 0x00070018,  // keyU
        [KEY_I] = 0x0007000c,  // keyI
        [KEY_O] = 0x00070012,  // keyO
        [KEY_P] = 0x00070013,  // keyP
        [KEY_LEFTBRACE] = 0x0007002f,  // bracketLeft
        [KEY_RIGHTBRACE] = 0x00070030,  // bracketRight
        [KEY_ENTER] = 0x00070028,  // enter
        [KEY_LEFTCTRL] = 0x000700e0,  // controlLeft
        [KEY_A] = 0x00070004,  // keyA
        [KEY_S] = 0x00070016,  // keyS
        [KEY_D] = 0x00070007,  // keyD
        [KEY_F] = 0x00070009,  // keyF
        [KEY_G] = 0x0007000a,  // keyG
        [KEY_H] = 0x0007000b,  // keyH
        [KEY_J] = 0x0007000d,  // keyJ
        [KEY_K] = 0x0007000e,  // keyK
        [KEY_L] = 0x0007000f,  // keyL
        [KEY_SEMICOLON] = 0x00070033,  // semicolon
        [KEY_APOSTROPHE] = 0x00070034,  // quote
        [KEY_GRAVE] = 0x00070035,  // backquote
        [KEY_LEFTSHIFT] = 0x000700e1,  // shiftLeft
        [KEY_BACKSLASH] = 0x00070031,  // backslash
        [KEY_Z] = 0x0007001d,  // keyZ
        [KEY_X] = 0x0007001b,  // keyX
        [KEY_C] = 0x00070006,  // keyC
        [KEY_V] = 0x00070019,  // keyV
        [KEY_B] = 0x00070005,  // keyB
        [KEY_N] = 0x00070011,  // keyN
        [KEY_M] = 0x00070010,  // keyM
        [KEY_COMMA] = 0x00070036,  // comma
        [KEY_DOT] = 0x00070037,  // period
        [KEY_SLASH] = 0x00070038,  // slash
        [KEY_RIGHTSHIFT] = 0x000700e5,  // shiftRight
        [KEY_KPASTERISK] = 0x00070055,  // numpadMultiply
        [KEY_LEFTALT] = 0x000700e2,  // altLeft
        [KEY_SPACE] = 0x0007002c,  // space
        [KEY_CAPSLOCK] = 0x00070039,  // capsLock
        [KEY_F1] = 0x0007003a,  // f1
        [KEY_F2] = 0x0007003b,  // f2
        [KEY_F3] = 0x0007003c,  // f3
        [KEY_F4] = 0x0007003d,  // f4
        [KEY_F5] = 0x0007003e,  // f5
        [KEY_F6] = 0x0007003f,  // f6
        [KEY_F7] = 0x00070040,  // f7
        [KEY_F8] = 0x00070041,  // f8
        [KEY_F9] = 0x00070042,  // f9
        [KEY_F10] = 0x00070043,  // f10
        [KEY_NUMLOCK] = 0x00070053,  // numLock
        [KEY_SCROLLLOCK] = 0x00070047,  // scrollLock
        [KEY_KP7] = 0x0007005f,  // numpad7
        [KEY_KP8] = 0x00070060,  // numpad8
        [KEY_KP9] = 0x00070061,  // numpad9
        [KEY_KPMINUS] = 0x00070056,  // numpadSubtract
        [KEY_KP4] = 0x0007005c,  // numpad4
        [KEY_KP5] = 0x0007005d,  // numpad5
        [KEY_KP6] = 0x0007005e,  // numpad6
        [KEY_KPPLUS] = 0x00070057,  // numpadAdd
        [KEY_KP1] = 0x00070059,  // numpad1
        [KEY_KP2] = 0x0007005a,  // numpad2
        [KEY_KP3] = 0x0007005b,  // numpad3
        [KEY_KP0] = 0x00070062,  // numpad0
        [KEY_KPDOT] = 0x00070063,  // numpadDecimal
        [KEY_ZENKAKUHANKAKU] = 0x00070094,  // lang5
        [KEY_102ND] = 0x00070064,  // intlBackslash
        [KEY_F11] = 0x00070044,  // f11
        [KEY_F12] = 0x00070045,  // f12
        [KEY_RO] = 0x00070087,  // intlRo
        [KEY_KATAKANA] = 0x00070092,  // lang3
        [KEY_HIRAGANA] = 0x00070093,  // lang4
        [KEY_HENKAN] = 0x0007008a,  // convert
        [KEY_KATAKANAHIRAGANA] = 0x00070088,  // kanaMode
        [KEY_MUHENKAN] = 0x0007008b,  // nonConvert
        [KEY_KPJPCOMMA] = 0,
        [KEY_KPENTER] = 0x00070058,  // numpadEnter
        [KEY_RIGHTCTRL] = 0x000700e4,  // controlRight
        [KEY_KPSLASH] = 0x00070054,  // numpadDivide
        [KEY_SYSRQ] = 0x00070046,  // printScreen
        [KEY_RIGHTALT] = 0x000700e6,  // altRight
        [KEY_LINEFEED] = 0,
        [KEY_HOME] = 0x0007004a,  // home
        [KEY_UP] = 0x00070052,  // arrowUp
        [KEY_PAGEUP] = 0x0007004b,  // pageUp
        [KEY_LEFT] = 0x00070050,  // arrowLeft
        [KEY_RIGHT] = 0x0007004f,  // arrowRight
        [KEY_END] = 0x0007004d,  // end
        [KEY_DOWN] = 0x00070051,  // arrowDown
        [KEY_PAGEDOWN] = 0x0007004e,  // pageDown
        [KEY_INSERT] = 0x00070049,  // insert
        [KEY_DELETE] = 0x0007004c,  // delete
        [KEY_MACRO] = 0,
        [KEY_MUTE] = 0x0007007f,  // audioVolumeMute
        [KEY_VOLUMEDOWN] = 0x00070081,  // audioVolumeDown
        [KEY_VOLUMEUP] = 0x00070080,  // audioVolumeUp
        [KEY_POWER] = 0x00070066,  // power
        [KEY_KPEQUAL] = 0x00070067,  // numpadEqual
        [KEY_KPPLUSMINUS] = 0x000700d7,  // numpadSignChange
        [KEY_PAUSE] = 0x00070048,  // pause
        [KEY_SCALE] = 0x000c029f,  // showAllWindows
        [KEY_KPCOMMA] = 0x00070085,  // numpadComma
        [KEY_HANGEUL] = 0x00070090,  // lang1
        [KEY_HANJA] = 0x00070091,  // lang2
        [KEY_YEN] = 0x00070089,  // intlYen
        [KEY_LEFTMETA] = 0x000700e3,  // metaLeft
        [KEY_RIGHTMETA] = 0x000700e7,  // metaRight
        [KEY_COMPOSE] = 0x00070065,  // contextMenu
        [KEY_STOP] = 0x000c0226,  // browserStop
        [KEY_AGAIN] = 0x00070079,  // again
        [KEY_PROPS] = 0,
        [KEY_UNDO] = 0x0007007a,  // undo
        [KEY_FRONT] = 0x00070077,  // select
        [KEY_COPY] = 0x0007007c,  // copy
        [KEY_OPEN] = 0x00070074,  // open
        [KEY_PASTE] = 0x0007007d,  // paste
        [KEY_FIND] = 0x0007007e,  // find
        [KEY_CUT] = 0x0007007b,  // cut
        [KEY_HELP] = 0x00070075,  // help
        [KEY_MENU] = 0,
        [KEY_CALC] = 0x000c0192,  // launchApp2
        [KEY_SETUP] = 0,
        [KEY_SLEEP] = 0x00010082,  // sleep
        [KEY_WAKEUP] = 0x00010083,  // wakeUp
        [KEY_FILE] = 0x000c0194,  // launchApp1
        [KEY_SENDFILE] = 0,
        [KEY_DELETEFILE] = 0,
        [KEY_XFER] = 0,
        [KEY_PROG1] = 0,
        [KEY_PROG2] = 0,
        [KEY_WWW] = 0x000c0196,  // launchInternetBrowser
        [KEY_MSDOS] = 0,
        [KEY_COFFEE] = 0x000c019e,  // lockScreen
        [KEY_ROTATE_DISPLAY] = 0,
        [KEY_CYCLEWINDOWS] = 0,
        [KEY_MAIL] = 0x000c018a,  // launchMail
        [KEY_BOOKMARKS] = 0x000c022a,  // browserFavorites
        [KEY_COMPUTER] = 0,
        [KEY_BACK] = 0x000c0224,  // browserBack
        [KEY_FORWARD] = 0x000c0225,  // browserForward
        [KEY_CLOSECD] = 0,
        [KEY_EJECTCD] = 0x000c00b8,  // eject
        [KEY_EJECTCLOSECD] = 0,
        [KEY_NEXTSONG] = 0x000c00b5,  // mediaTrackNext
        [KEY_PLAYPAUSE] = 0x000c00cd,  // mediaPlayPause
        [KEY_PREVIOUSSONG] = 0x000c00b6,  // mediaTrackPrevious
        [KEY_STOPCD] = 0x000c00b7,  // mediaStop
        [KEY_RECORD] = 0x000c00b2,  // mediaRecord
        [KEY_REWIND] = 0x000c00b4,  // mediaRewind
        [KEY_PHONE] = 0x000c008c,  // launchPhone
        [KEY_ISO] = 0,
        [KEY_CONFIG] = 0x000c0183,  // mediaSelect
        [KEY_HOMEPAGE] = 0x000c0223,  // browserHome
        [KEY_REFRESH] = 0x000c0227,  // browserRefresh
        [KEY_EXIT] = 0x000c0094,  // exit
        [KEY_MOVE] = 0,
        [KEY_EDIT] = 0,
        [KEY_SCROLLUP] = 0,
        [KEY_SCROLLDOWN] = 0,
        [KEY_KPLEFTPAREN] = 0x000700b6,  // numpadParenLeft
        [KEY_KPRIGHTPAREN] = 0x000700b7,  // numpadParenRight
        [KEY_NEW] = 0x000c0201,  // newKey
        [KEY_REDO] = 0x000c0279,  // redo
        [KEY_F13] = 0x00070068,  // f13
        [KEY_F14] = 0x00070069,  // f14
        [KEY_F15] = 0x0007006a,  // f15
        [KEY_F16] = 0x0007006b,  // f16
        [KEY_F17] = 0x0007006c,  // f17
        [KEY_F18] = 0x0007006d,  // f18
        [KEY_F19] = 0x0007006e,  // f19
        [KEY_F20] = 0x0007006f,  // f20
        [KEY_F21] = 0x00070070,  // f21
        [KEY_F22] = 0x00070071,  // f22
        [KEY_F23] = 0x00070072,  // f23
        [KEY_F24] = 0x00070073,  // f24
        [KEY_PLAYCD] = 0,
        [KEY_PAUSECD] = 0x000c00b1,  // mediaPause
        [KEY_PROG3] = 0,
        [KEY_PROG4] = 0,
        [KEY_ALL_APPLICATIONS] = 0,
        [KEY_SUSPEND] = 0,
        [KEY_CLOSE] = 0x000c0203,  // close
        [KEY_PLAY] = 0x000c00b0,  // mediaPlay
        [KEY_FASTFORWARD] = 0x000c00b3,  // mediaFastForward
        [KEY_BASSBOOST] = 0x000c00e5,  // bassBoost
        [KEY_PRINT] = 0x000c0208,  // print
        [KEY_HP] = 0,
        [KEY_CAMERA] = 0,
        [KEY_SOUND] = 0,
        [KEY_QUESTION] = 0,
        [KEY_EMAIL] = 0,
        [KEY_CHAT] = 0,
        [KEY_SEARCH] = 0x000c0221,  // browserSearch
        [KEY_CONNECT] = 0,
        [KEY_FINANCE] = 0,
        [KEY_SPORT] = 0,
        [KEY_SHOP] = 0,
        [KEY_ALTERASE] = 0,
        [KEY_CANCEL] = 0,
        [KEY_BRIGHTNESSDOWN] = 0x000c0070,  // brightnessDown
        [KEY_BRIGHTNESSUP] = 0x000c006f,  // brightnessUp
        [KEY_MEDIA] = 0,
        [KEY_SWITCHVIDEOMODE] = 0x000100b5,  // displayToggleIntExt
        [KEY_KBDILLUMTOGGLE] = 0,
        [KEY_KBDILLUMDOWN] = 0x000c007a,  // kbdIllumDown
        [KEY_KBDILLUMUP] = 0x000c0079,  // kbdIllumUp
        [KEY_SEND] = 0x000c028c,  // mailSend
        [KEY_REPLY] = 0x000c0289,  // mailReply
        [KEY_FORWARDMAIL] = 0x000c028b,  // mailForward
        [KEY_SAVE] = 0x000c0207,  // save
        [KEY_DOCUMENTS] = 0x000c01a7,  // launchDocuments
        [KEY_BATTERY] = 0,
        [KEY_BLUETOOTH] = 0,
        [KEY_WLAN] = 0,
        [KEY_UWB] = 0,
        [KEY_UNKNOWN] = 0,
        [KEY_VIDEO_NEXT] = 0,
        [KEY_VIDEO_PREV] = 0,
        [KEY_BRIGHTNESS_CYCLE] = 0,
        [KEY_BRIGHTNESS_AUTO] = 0x000c0075,  // brightnessAuto
        [KEY_DISPLAY_OFF] = 0,
        [KEY_WWAN] = 0,
        [KEY_RFKILL] = 0,
        [KEY_MICMUTE] = 0x00000018,  // microphoneMuteToggle
        // reserved for buttons (BTN_MISC ... BTN_GEAR_UP)
        [KEY_OK] = 0,
        [KEY_SELECT] = 0,
        [KEY_GOTO] = 0,
        [KEY_CLEAR] = 0,
        [KEY_POWER2] = 0,
        [KEY_OPTION] = 0,
        [KEY_INFO] = 0x000c0060,  // info
        [KEY_TIME] = 0,
        [KEY_VENDOR] = 0,
        [KEY_ARCHIVE] = 0,
        [KEY_PROGRAM] = 0x000c008d,  // programGuide
        [KEY_CHANNEL] = 0,
        [KEY_FAVORITES] = 0,
        [KEY_EPG] = 0,
        [KEY_PVR] = 0,
        [KEY_MHP] = 0,
        [KEY_LANGUAGE] = 0,
        [KEY_TITLE] = 0,
        [KEY_SUBTITLE] = 0x000c0061,  // closedCaptionToggle
        [KEY_ANGLE] = 0,
        [KEY_ZOOM] = 0x000c0232,  // zoomToggle
        [KEY_MODE] = 0,
        [KEY_KEYBOARD] = 0x000c01ae,  // launchKeyboardLayout
        [KEY_SCREEN] = 0,
        [KEY_PC] = 0,
        [KEY_TV] = 0,
        [KEY_TV2] = 0,
        [KEY_VCR] = 0,
        [KEY_VCR2] = 0,
        [KEY_SAT] = 0,
        [KEY_SAT2] = 0,
        [KEY_CD] = 0,
        [KEY_TAPE] = 0,
        [KEY_RADIO] = 0,
        [KEY_TUNER] = 0,
        [KEY_PLAYER] = 0,
        [KEY_TEXT] = 0,
        [KEY_DVD] = 0,
        [KEY_AUX] = 0,
        [KEY_MP3] = 0,
        [KEY_AUDIO] = 0x000c01b7,  // launchAudioBrowser
        [KEY_VIDEO] = 0,
        [KEY_DIRECTORY] = 0,
        [KEY_LIST] = 0,
        [KEY_MEMO] = 0,
        [KEY_CALENDAR] = 0x000c018e,  // launchCalendar
        [KEY_RED] = 0,
        [KEY_GREEN] = 0,
        [KEY_YELLOW] = 0,
        [KEY_BLUE] = 0,
        [KEY_CHANNELUP] = 0,
        [KEY_CHANNELDOWN] = 0,
        [KEY_FIRST] = 0,
        [KEY_LAST] = 0x000c0083,  // mediaLast
        [KEY_AB] = 0,
        [KEY_NEXT] = 0,
        [KEY_RESTART] = 0,
        [KEY_SLOW] = 0,
        [KEY_SHUFFLE] = 0x000c009c,  // channelUp
        [KEY_BREAK] = 0x000c009d,  // channelDown
        [KEY_PREVIOUS] = 0,
        [KEY_DIGITS] = 0,
        [KEY_TEEN] = 0,
        [KEY_TWEN] = 0,
        [KEY_VIDEOPHONE] = 0,
        [KEY_GAMES] = 0,
        [KEY_ZOOMIN] = 0x000c022d,  // zoomIn
        [KEY_ZOOMOUT] = 0x000c022e,  // zoomOut
        [KEY_ZOOMRESET] = 0,
        [KEY_WORDPROCESSOR] = 0x000c0184,  // launchWordProcessor
        [KEY_EDITOR] = 0,
        [KEY_SPREADSHEET] = 0x000c0186,  // launchSpreadsheet
        [KEY_GRAPHICSEDITOR] = 0,
        [KEY_PRESENTATION] = 0,
        [KEY_DATABASE] = 0,
        [KEY_NEWS] = 0,
        [KEY_VOICEMAIL] = 0,
        [KEY_ADDRESSBOOK] = 0x000c018d,  // launchContacts
        [KEY_MESSENGER] = 0,
        [KEY_DISPLAYTOGGLE] = 0x000c0072,  // brightnessToggle
        [KEY_SPELLCHECK] = 0x000c01ab,  // spellCheck
        [KEY_LOGOFF] = 0x000c019c,  // logOff
        [KEY_CONTROLPANEL] = 0x000c019f,  // launchControlPanel
        [KEY_APPSELECT] = 0x000c01a2,  // selectTask
        [KEY_SCREENSAVER] = 0x000c01b1,  // launchScreenSaver
        [KEY_VOICECOMMAND] = 0x000c00cf,  // speechInputToggle
        [KEY_ASSISTANT] = 0x000c01cb,  // launchAssistant

#ifndef KEY_KBD_LAYOUT_NEXT
#   define KEY_KBD_LAYOUT_NEXT 0x248
#endif
        [KEY_KBD_LAYOUT_NEXT] = 0x000c029d,  // keyboardLayoutSelect

#ifndef KEY_EMOJI_PICKER
#   define KEY_EMOJI_PICKER 0x249
#endif
        [KEY_EMOJI_PICKER] = 0,

#ifndef KEY_DICTATE
#   define KEY_DICTATE 0x24a
#endif
        [KEY_DICTATE] = 0,

        // unused
        [KEY_BRIGHTNESS_MIN] = 0x000c0073,  // brightnessMinimum
        [KEY_BRIGHTNESS_MAX] = 0x000c0074,  // brightnessMaximum
        // KEY_KBDINPUTASSIST_PREV ... KEY_ONSCREEN_KEYBOARD

#ifndef KEY_PRIVACY_SCREEN_TOGGLE
#   define KEY_PRIVACY_SCREEN_TOGGLE 0x279
#endif
        [KEY_PRIVACY_SCREEN_TOGGLE] = 0x00000017  // privacyScreenToggle
    };
    // clang-format on

    uint64_t physical = 0;
    if (evdev_keycode < ARRAY_SIZE(physical_keys)) {
        physical = physical_keys[evdev_keycode];
    }

    // In case we don't have a match for this evdev keycode,
    // instead return the XKB keycode (== evdev keycode + 8) with the GTK key plane.
    if (physical == 0) {
        physical = apply_gtk_key_plane(evdev_keycode + 8ull);
    }

    return physical;
}   

ATTR_CONST static uint64_t physical_key_for_xkb_keycode(xkb_keycode_t xkb_keycode) {
    DEBUG_ASSERT(xkb_keycode >= 8);
    return physical_key_for_evdev_keycode(xkb_keycode - 8);
}

ATTR_CONST static char eascii_to_lower(unsigned char n) {
    if (n >= 'A' && n <= 'Z') {
        return n - 'A' + 'a';
    }

    if (n >= XKB_KEY_Agrave && n <= XKB_KEY_THORN && n != XKB_KEY_multiply) {
        return n - XKB_KEY_Agrave + XKB_KEY_agrave;
    }

    return n;
}

ATTR_CONST static uint32_t logical_key_for_xkb_keysym(xkb_keysym_t keysym) {
    // clang-format off
    static const uint64_t logical_keys_1[] = {
        [0x0000fd06 - 0xfd06] =  0x00100000405,  // 3270_EraseEOF
        [0x0000fd0e - 0xfd06] =  0x00100000503,  // 3270_Attn
        [0x0000fd15 - 0xfd06] =  0x00100000402,  // 3270_Copy
        [0x0000fd16 - 0xfd06] =  0x00100000d2f,  // 3270_Play
        [0x0000fd1b - 0xfd06] =  0x00100000406,  // 3270_ExSelect
        [0x0000fd1d - 0xfd06] =  0x00100000608,  // 3270_PrintScreen
        [0x0000fd1e - 0xfd06] =  0x0010000000d,  // 3270_Enter
        [0x0000fe03 - 0xfd06] =  0x00200000105,  // ISO_Level3_Shift
        [0x0000fe08 - 0xfd06] =  0x00100000709,  // ISO_Next_Group
        [0x0000fe0a - 0xfd06] =  0x0010000070a,  // ISO_Prev_Group
        [0x0000fe0c - 0xfd06] =  0x00100000707,  // ISO_First_Group
        [0x0000fe0e - 0xfd06] =  0x00100000708,  // ISO_Last_Group
        [0x0000fe20 - 0xfd06] =  0x00100000009,  // ISO_Left_Tab
        [0x0000fe34 - 0xfd06] =  0x0010000000d,  // ISO_Enter
        [0x0000ff08 - 0xfd06] =  0x00100000008,  // BackSpace
        [0x0000ff09 - 0xfd06] =  0x00100000009,  // Tab
        [0x0000ff0b - 0xfd06] =  0x00100000401,  // Clear
        [0x0000ff0d - 0xfd06] =  0x0010000000d,  // Return
        [0x0000ff13 - 0xfd06] =  0x00100000509,  // Pause
        [0x0000ff14 - 0xfd06] =  0x0010000010c,  // Scroll_Lock
        [0x0000ff1b - 0xfd06] =  0x0010000001b,  // Escape
        [0x0000ff21 - 0xfd06] =  0x00100000719,  // Kanji
        [0x0000ff24 - 0xfd06] =  0x0010000071b,  // Romaji
        [0x0000ff25 - 0xfd06] =  0x00100000716,  // Hiragana
        [0x0000ff26 - 0xfd06] =  0x0010000071a,  // Katakana
        [0x0000ff27 - 0xfd06] =  0x00100000717,  // Hiragana_Katakana
        [0x0000ff28 - 0xfd06] =  0x0010000071c,  // Zenkaku
        [0x0000ff29 - 0xfd06] =  0x00100000715,  // Hankaku
        [0x0000ff2a - 0xfd06] =  0x0010000071d,  // Zenkaku_Hankaku
        [0x0000ff2f - 0xfd06] =  0x00100000714,  // Eisu_Shift
        [0x0000ff31 - 0xfd06] =  0x00100000711,  // Hangul
        [0x0000ff34 - 0xfd06] =  0x00100000712,  // Hangul_Hanja
        [0x0000ff37 - 0xfd06] =  0x00100000703,  // Codeinput
        [0x0000ff3c - 0xfd06] =  0x00100000710,  // SingleCandidate
        [0x0000ff3e - 0xfd06] =  0x0010000070e,  // PreviousCandidate
        [0x0000ff50 - 0xfd06] =  0x00100000306,  // Home
        [0x0000ff51 - 0xfd06] =  0x00100000302,  // Left
        [0x0000ff52 - 0xfd06] =  0x00100000304,  // Up
        [0x0000ff53 - 0xfd06] =  0x00100000303,  // Right
        [0x0000ff54 - 0xfd06] =  0x00100000301,  // Down
        [0x0000ff55 - 0xfd06] =  0x00100000308,  // Page_Up
        [0x0000ff56 - 0xfd06] =  0x00100000307,  // Page_Down
        [0x0000ff57 - 0xfd06] =  0x00100000305,  // End
        [0x0000ff60 - 0xfd06] =  0x0010000050c,  // Select
        [0x0000ff61 - 0xfd06] =  0x00100000a0c,  // Print
        [0x0000ff62 - 0xfd06] =  0x00100000506,  // Execute
        [0x0000ff63 - 0xfd06] =  0x00100000407,  // Insert
        [0x0000ff65 - 0xfd06] =  0x0010000040a,  // Undo
        [0x0000ff66 - 0xfd06] =  0x00100000409,  // Redo
        [0x0000ff67 - 0xfd06] =  0x00100000505,  // Menu
        [0x0000ff68 - 0xfd06] =  0x00100000507,  // Find
        [0x0000ff69 - 0xfd06] =  0x00100000504,  // Cancel
        [0x0000ff6a - 0xfd06] =  0x00100000508,  // Help
        [0x0000ff7e - 0xfd06] =  0x0010000070b,  // Mode_switch
        [0x0000ff7f - 0xfd06] =  0x0010000010a,  // Num_Lock
        [0x0000ff80 - 0xfd06] =  0x00000000020,  // KP_Space
        [0x0000ff89 - 0xfd06] =  0x00100000009,  // KP_Tab
        [0x0000ff8d - 0xfd06] =  0x0020000020d,  // KP_Enter
        [0x0000ff91 - 0xfd06] =  0x00100000801,  // KP_F1
        [0x0000ff92 - 0xfd06] =  0x00100000802,  // KP_F2
        [0x0000ff93 - 0xfd06] =  0x00100000803,  // KP_F3
        [0x0000ff94 - 0xfd06] =  0x00100000804,  // KP_F4
        [0x0000ff95 - 0xfd06] =  0x00200000237,  // KP_Home
        [0x0000ff96 - 0xfd06] =  0x00200000234,  // KP_Left
        [0x0000ff97 - 0xfd06] =  0x00200000238,  // KP_Up
        [0x0000ff98 - 0xfd06] =  0x00200000236,  // KP_Right
        [0x0000ff99 - 0xfd06] =  0x00200000232,  // KP_Down
        [0x0000ff9a - 0xfd06] =  0x00200000239,  // KP_Page_Up
        [0x0000ff9b - 0xfd06] =  0x00200000233,  // KP_Page_Down
        [0x0000ff9c - 0xfd06] =  0x00200000231,  // KP_End
        [0x0000ff9e - 0xfd06] =  0x00200000230,  // KP_Insert
        [0x0000ff9f - 0xfd06] =  0x0020000022e,  // KP_Delete
        [0x0000ffaa - 0xfd06] =  0x0020000022a,  // KP_Multiply
        [0x0000ffab - 0xfd06] =  0x0020000022b,  // KP_Add
        [0x0000ffad - 0xfd06] =  0x0020000022d,  // KP_Subtract
        [0x0000ffae - 0xfd06] =  0x0000000002e,  // KP_Decimal
        [0x0000ffaf - 0xfd06] =  0x0020000022f,  // KP_Divide
        [0x0000ffb0 - 0xfd06] =  0x00200000230,  // KP_0
        [0x0000ffb1 - 0xfd06] =  0x00200000231,  // KP_1
        [0x0000ffb2 - 0xfd06] =  0x00200000232,  // KP_2
        [0x0000ffb3 - 0xfd06] =  0x00200000233,  // KP_3
        [0x0000ffb4 - 0xfd06] =  0x00200000234,  // KP_4
        [0x0000ffb5 - 0xfd06] =  0x00200000235,  // KP_5
        [0x0000ffb6 - 0xfd06] =  0x00200000236,  // KP_6
        [0x0000ffb7 - 0xfd06] =  0x00200000237,  // KP_7
        [0x0000ffb8 - 0xfd06] =  0x00200000238,  // KP_8
        [0x0000ffb9 - 0xfd06] =  0x00200000239,  // KP_9
        [0x0000ffbd - 0xfd06] =  0x0020000023d,  // KP_Equal
        [0x0000ffbe - 0xfd06] =  0x00100000801,  // F1
        [0x0000ffbf - 0xfd06] =  0x00100000802,  // F2
        [0x0000ffc0 - 0xfd06] =  0x00100000803,  // F3
        [0x0000ffc1 - 0xfd06] =  0x00100000804,  // F4
        [0x0000ffc2 - 0xfd06] =  0x00100000805,  // F5
        [0x0000ffc3 - 0xfd06] =  0x00100000806,  // F6
        [0x0000ffc4 - 0xfd06] =  0x00100000807,  // F7
        [0x0000ffc5 - 0xfd06] =  0x00100000808,  // F8
        [0x0000ffc6 - 0xfd06] =  0x00100000809,  // F9
        [0x0000ffc7 - 0xfd06] =  0x0010000080a,  // F10
        [0x0000ffc8 - 0xfd06] =  0x0010000080b,  // F11
        [0x0000ffc9 - 0xfd06] =  0x0010000080c,  // F12
        [0x0000ffca - 0xfd06] =  0x0010000080d,  // F13
        [0x0000ffcb - 0xfd06] =  0x0010000080e,  // F14
        [0x0000ffcc - 0xfd06] =  0x0010000080f,  // F15
        [0x0000ffcd - 0xfd06] =  0x00100000810,  // F16
        [0x0000ffce - 0xfd06] =  0x00100000811,  // F17
        [0x0000ffcf - 0xfd06] =  0x00100000812,  // F18
        [0x0000ffd0 - 0xfd06] =  0x00100000813,  // F19
        [0x0000ffd1 - 0xfd06] =  0x00100000814,  // F20
        [0x0000ffd2 - 0xfd06] =  0x00100000815,  // F21
        [0x0000ffd3 - 0xfd06] =  0x00100000816,  // F22
        [0x0000ffd4 - 0xfd06] =  0x00100000817,  // F23
        [0x0000ffd5 - 0xfd06] =  0x00100000818,  // F24
        [0x0000ffe1 - 0xfd06] =  0x00200000102,  // Shift_L
        [0x0000ffe2 - 0xfd06] =  0x00200000103,  // Shift_R
        [0x0000ffe3 - 0xfd06] =  0x00200000100,  // Control_L
        [0x0000ffe4 - 0xfd06] =  0x00200000101,  // Control_R
        [0x0000ffe5 - 0xfd06] =  0x00100000104,  // Caps_Lock
        [0x0000ffe7 - 0xfd06] =  0x00200000106,  // Meta_L
        [0x0000ffe8 - 0xfd06] =  0x00200000107,  // Meta_R
        [0x0000ffe9 - 0xfd06] =  0x00200000104,  // Alt_L
        [0x0000ffea - 0xfd06] =  0x00200000105,  // Alt_R
        [0x0000ffeb - 0xfd06] =  0x0010000010e,  // Super_L
        [0x0000ffec - 0xfd06] =  0x0010000010e,  // Super_R
        [0x0000ffed - 0xfd06] =  0x00100000108,  // Hyper_L
        [0x0000ffee - 0xfd06] =  0x00100000108,  // Hyper_R
        [0x0000ffff - 0xfd06] =  0x0010000007f,  // Delete
    };

    static const uint64_t logical_keys_2[] = {
        [0x1008ff02 - 0x1008ff02] = 0x00100000602,  // MonBrightnessUp
        [0x1008ff03 - 0x1008ff02] = 0x00100000601,  // MonBrightnessDown
        [0x1008ff10 - 0x1008ff02] = 0x0010000060a,  // Standby
        [0x1008ff11 - 0x1008ff02] = 0x00100000a0f,  // AudioLowerVolume
        [0x1008ff12 - 0x1008ff02] = 0x00100000a11,  // AudioMute
        [0x1008ff13 - 0x1008ff02] = 0x00100000a10,  // AudioRaiseVolume
        [0x1008ff14 - 0x1008ff02] = 0x00100000d2f,  // AudioPlay
        [0x1008ff15 - 0x1008ff02] = 0x00100000a07,  // AudioStop
        [0x1008ff16 - 0x1008ff02] = 0x00100000a09,  // AudioPrev
        [0x1008ff17 - 0x1008ff02] = 0x00100000a08,  // AudioNext
        [0x1008ff18 - 0x1008ff02] = 0x00100000c04,  // HomePage
        [0x1008ff19 - 0x1008ff02] = 0x00100000b03,  // Mail
        [0x1008ff1b - 0x1008ff02] = 0x00100000c06,  // Search
        [0x1008ff1c - 0x1008ff02] = 0x00100000d30,  // AudioRecord
        [0x1008ff20 - 0x1008ff02] = 0x00100000b02,  // Calendar
        [0x1008ff26 - 0x1008ff02] = 0x00100000c01,  // Back
        [0x1008ff27 - 0x1008ff02] = 0x00100000c03,  // Forward
        [0x1008ff28 - 0x1008ff02] = 0x00100000c07,  // Stop
        [0x1008ff29 - 0x1008ff02] = 0x00100000c05,  // Refresh
        [0x1008ff2a - 0x1008ff02] = 0x00100000607,  // PowerOff
        [0x1008ff2b - 0x1008ff02] = 0x0010000060b,  // WakeUp
        [0x1008ff2c - 0x1008ff02] = 0x00100000604,  // Eject
        [0x1008ff2d - 0x1008ff02] = 0x00100000b07,  // ScreenSaver
        [0x1008ff2f - 0x1008ff02] = 0x00200000002,  // Sleep
        [0x1008ff30 - 0x1008ff02] = 0x00100000c02,  // Favorites
        [0x1008ff31 - 0x1008ff02] = 0x00100000d2e,  // AudioPause
        [0x1008ff3e - 0x1008ff02] = 0x00100000d31,  // AudioRewind
        [0x1008ff56 - 0x1008ff02] = 0x00100000a01,  // Close
        [0x1008ff57 - 0x1008ff02] = 0x00100000402,  // Copy
        [0x1008ff58 - 0x1008ff02] = 0x00100000404,  // Cut
        [0x1008ff61 - 0x1008ff02] = 0x00100000605,  // LogOff
        [0x1008ff68 - 0x1008ff02] = 0x00100000a0a,  // New
        [0x1008ff6b - 0x1008ff02] = 0x00100000a0b,  // Open
        [0x1008ff6d - 0x1008ff02] = 0x00100000408,  // Paste
        [0x1008ff6e - 0x1008ff02] = 0x00100000b0d,  // Phone
        [0x1008ff72 - 0x1008ff02] = 0x00100000a03,  // Reply
        [0x1008ff77 - 0x1008ff02] = 0x00100000a0d,  // Save
        [0x1008ff7b - 0x1008ff02] = 0x00100000a04,  // Send
        [0x1008ff7c - 0x1008ff02] = 0x00100000a0e,  // Spell
        [0x1008ff8b - 0x1008ff02] = 0x0010000050d,  // ZoomIn
        [0x1008ff8c - 0x1008ff02] = 0x0010000050e,  // ZoomOut
        [0x1008ff90 - 0x1008ff02] = 0x00100000a02,  // MailForward
        [0x1008ff97 - 0x1008ff02] = 0x00100000d2c,  // AudioForward
        [0x1008ffa7 - 0x1008ff02] = 0x00200000000,  // Suspend
    };
    // clang-format on

    uint64_t logical = 0;
    if (keysym == XKB_KEY_yen) {
        return apply_flutter_key_plane(0x00022);
    } else if (keysym < 256) {
        return apply_unicode_key_plane(eascii_to_lower(keysym));
    } else if (keysym >= 0xfd06 && keysym - 0xfd06 < ARRAY_SIZE(logical_keys_1)) {
        logical = logical_keys_1[keysym];
    } else if (keysym >= 0x1008ff02 && keysym - 0x1008ff02 < ARRAY_SIZE(logical_keys_2)) {
        logical = logical_keys_2[keysym];
    }

    if (logical == 0) {
        return apply_gtk_key_plane(keysym);
    }

    return logical;
}

struct rawkb {
    struct key_event_interface interface;
    void *userdata;
};

int rawkb_send_android_keyevent(
    uint32_t flags,
    uint32_t code_point,
    unsigned int key_code,
    uint32_t plain_code_point,
    uint32_t scan_code,
    uint32_t meta_state,
    uint32_t source,
    uint16_t vendor_id,
    uint16_t product_id,
    uint16_t device_id,
    int repeat_count,
    bool is_down,
    char *character
) {
    /**
     * keymap: android
     * flags: flags
     * codePoint: code_point
     * keyCode: key_code
     * plainCodePoint: plain_code_point
     * scanCode: scan_code
     * metaState: meta_state
     * source: source
     * vendorId: vendor_id
     * productId: product_id
     * deviceId: device_id
     * repeatCount: repeatCount,
     * type: is_down? "keydown" : "keyup"
     * character: character
     */

    (void) plain_code_point;
    
    return platch_send(
        KEY_EVENT_CHANNEL,
        &(struct platch_obj) {
            .codec = kJSONMessageCodec,
            .json_value = {
                .type = kJsonObject,
                .size = 14,
                .keys = (char*[14]) {
                    "keymap",
                    "flags",
                    "codePoint",
                    "keyCode",
                    "plainCodePoint",
                    "scanCode",
                    "metaState",
                    "source",
                    "vendorId",
                    "productId",
                    "deviceId",
                    "repeatCount",
                    "type",
                    "character"
                },
                .values = (struct json_value[14]) {
                    /* keymap */            {.type = kJsonString, .string_value = "android"},
                    /* flags */             {.type = kJsonNumber, .number_value = flags},
                    /* codePoint */         {.type = kJsonNumber, .number_value = code_point},
                    /* keyCode */           {.type = kJsonNumber, .number_value = key_code},
                    /* plainCodePoint */    {.type = kJsonNumber, .number_value = code_point},
                    /* scanCode */          {.type = kJsonNumber, .number_value = scan_code},
                    /* metaState */         {.type = kJsonNumber, .number_value = meta_state},
                    /* source */            {.type = kJsonNumber, .number_value = source},
                    /* vendorId */          {.type = kJsonNumber, .number_value = vendor_id},
                    /* productId */         {.type = kJsonNumber, .number_value = product_id},
                    /* deviceId */          {.type = kJsonNumber, .number_value = device_id},
                    /* repeatCount */       {.type = kJsonNumber, .number_value = repeat_count},
                    /* type */              {.type = kJsonString, .string_value = is_down? "keydown" : "keyup"},
                    /* character */         {.type = character? kJsonString : kJsonNull, .string_value = character}
                }
            }
        },
        kJSONMessageCodec,
        NULL,
        NULL
    );
}

int rawkb_send_gtk_keyevent(
    uint32_t unicode_scalar_values,
    uint32_t key_code,
    uint32_t scan_code,
    uint32_t modifiers,
    bool is_down
) {
    /**
     * keymap: linux
     * toolkit: glfw
     * unicodeScalarValues: code_point
     * keyCode: key_code
     * scanCode: scan_code
     * modifiers: mods
     * type: is_down? "keydown" : "keyup"
     */

    return platch_send(
        KEY_EVENT_CHANNEL,
        &(struct platch_obj) {
            .codec = kJSONMessageCodec,
            .json_value = {
                .type = kJsonObject,
                .size = 7,
                .keys = (char*[7]) {
                    "keymap",
                    "toolkit",
                    "unicodeScalarValues",
                    "keyCode",
                    "scanCode",
                    "modifiers",
                    "type"
                },
                .values = (struct json_value[7]) {
                    /* keymap */                {.type = kJsonString, .string_value = "linux"},
                    /* toolkit */               {.type = kJsonString, .string_value = "gtk"},
                    /* unicodeScalarValues */   {.type = kJsonNumber, .number_value = unicode_scalar_values},
                    /* keyCode */               {.type = kJsonNumber, .number_value = key_code},
                    /* scanCode */              {.type = kJsonNumber, .number_value = scan_code},
                    /* modifiers */             {.type = kJsonNumber, .number_value = modifiers},
                    /* type */                  {.type = kJsonString, .string_value = is_down? "keydown" : "keyup"}
                }
            }
        },
        kJSONMessageCodec,
        NULL,
        NULL
    );
}

int rawkb_send_flutter_keyevent(
    struct rawkb *rawkb,
    double timestamp_us,
    FlutterKeyEventType type,
    uint64_t physical,
    uint64_t logical,
    const char *character,
    bool synthesized
) {
    COMPILE_ASSERT(sizeof(FlutterKeyEvent) == 48 || sizeof(FlutterKeyEvent) == 56);
    rawkb->interface.send_key_event(
        rawkb->userdata,
        &(FlutterKeyEvent) {
            .struct_size = sizeof(FlutterKeyEvent),
            .timestamp = timestamp_us,
            .type = type,
            .physical = physical,
            .logical = logical,
            .character = character,
            .synthesized = synthesized,
        }
    );
    return 0;
}

int rawkb_on_key_event(
    struct rawkb *rawkb,
    uint64_t timestamp_us,
    xkb_keycode_t xkb_keycode,
    xkb_keysym_t xkb_keysym,
    uint32_t plain_codepoint,
    key_modifiers_t modifiers,
    const char *text,
    bool is_down,
    bool is_repeat
) {
    FlutterKeyEventType type;
    uint64_t physical, logical;
    int ok;

    physical = physical_key_for_xkb_keycode(xkb_keycode);
    logical = logical_key_for_xkb_keysym(xkb_keysym);

    if (is_down && !is_repeat) {
        type = kFlutterKeyEventTypeDown;
    } else if (is_down && is_repeat) {
        type = kFlutterKeyEventTypeRepeat;
    } else {
        DEBUG_ASSERT(!is_repeat);
        type = kFlutterKeyEventTypeUp;
    }

    ok = rawkb_send_flutter_keyevent(
        rawkb,
        (double) timestamp_us,
        type,
        physical,
        logical,
        text,
        false
    );
    if (ok != 0) {
        return ok;
    }
    
    ok = rawkb_send_gtk_keyevent(
        plain_codepoint,
        xkb_keysym,
        xkb_keycode,
        modifiers.u32,
        is_down
    );
    if (ok != 0) {
        return ok;
    }

    return 0;
}

static void assert_key_modifiers_work() {
    key_modifiers_t mods = {0};

    mods.u32 = 1;
    DEBUG_ASSERT_EQUALS(mods.shift, true);
    DEBUG_ASSERT_EQUALS(mods.capslock, false);
    DEBUG_ASSERT_EQUALS(mods.ctrl, false);
    DEBUG_ASSERT_EQUALS(mods.alt, false);
    DEBUG_ASSERT_EQUALS(mods.numlock, false);
    DEBUG_ASSERT_EQUALS(mods.__pad, 0);
    DEBUG_ASSERT_EQUALS(mods.meta, false);

    mods.u32 = 1 << 1;
    DEBUG_ASSERT_EQUALS(mods.shift, false);
    DEBUG_ASSERT_EQUALS(mods.capslock, true);
    DEBUG_ASSERT_EQUALS(mods.ctrl, false);
    DEBUG_ASSERT_EQUALS(mods.alt, false);
    DEBUG_ASSERT_EQUALS(mods.numlock, false);
    DEBUG_ASSERT_EQUALS(mods.__pad, 0);
    DEBUG_ASSERT_EQUALS(mods.meta, false);

    mods.u32 = 1 << 2;
    DEBUG_ASSERT_EQUALS(mods.shift, false);
    DEBUG_ASSERT_EQUALS(mods.capslock, false);
    DEBUG_ASSERT_EQUALS(mods.ctrl, true);
    DEBUG_ASSERT_EQUALS(mods.alt, false);
    DEBUG_ASSERT_EQUALS(mods.numlock, false);
    DEBUG_ASSERT_EQUALS(mods.__pad, 0);
    DEBUG_ASSERT_EQUALS(mods.meta, false);

    mods.u32 = 1 << 3;
    DEBUG_ASSERT_EQUALS(mods.shift, false);
    DEBUG_ASSERT_EQUALS(mods.capslock, false);
    DEBUG_ASSERT_EQUALS(mods.ctrl, false);
    DEBUG_ASSERT_EQUALS(mods.alt, true);
    DEBUG_ASSERT_EQUALS(mods.numlock, false);
    DEBUG_ASSERT_EQUALS(mods.__pad, 0);
    DEBUG_ASSERT_EQUALS(mods.meta, false);

    mods.u32 = 1 << 4;
    DEBUG_ASSERT_EQUALS(mods.shift, false);
    DEBUG_ASSERT_EQUALS(mods.capslock, false);
    DEBUG_ASSERT_EQUALS(mods.ctrl, false);
    DEBUG_ASSERT_EQUALS(mods.alt, false);
    DEBUG_ASSERT_EQUALS(mods.numlock, true);
    DEBUG_ASSERT_EQUALS(mods.__pad, 0);
    DEBUG_ASSERT_EQUALS(mods.meta, false);

    mods.u32 = 1 << 28;
    DEBUG_ASSERT_EQUALS(mods.shift, false);
    DEBUG_ASSERT_EQUALS(mods.capslock, false);
    DEBUG_ASSERT_EQUALS(mods.ctrl, false);
    DEBUG_ASSERT_EQUALS(mods.alt, false);
    DEBUG_ASSERT_EQUALS(mods.numlock, false);
    DEBUG_ASSERT_EQUALS(mods.__pad, 0);
    DEBUG_ASSERT_EQUALS(mods.meta, true);

    memset(&mods, 0, sizeof(mods));
    mods.shift = true;
    mods.meta = true;
    DEBUG_ASSERT_EQUALS(mods.u32, ((1 << 0) | (1 << 28)));

    mods.u32 = (1 << 1) | (1 << 4);
    DEBUG_ASSERT_EQUALS(mods.shift, false);
    DEBUG_ASSERT_EQUALS(mods.capslock, true);
    DEBUG_ASSERT_EQUALS(mods.ctrl, false);
    DEBUG_ASSERT_EQUALS(mods.alt, false);
    DEBUG_ASSERT_EQUALS(mods.numlock, true);
    DEBUG_ASSERT_EQUALS(mods.__pad, 0);
    DEBUG_ASSERT_EQUALS(mods.meta, false);
    
    (void) mods;
}

enum plugin_init_result rawkb_init(struct flutterpi *flutterpi, void **userdata_out) {
    (void) flutterpi;
    (void) userdata_out;

    assert_key_modifiers_work();

    return kInitialized_PluginInitResult;
}

void rawkb_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) flutterpi;
    (void) userdata;
}

FLUTTERPI_PLUGIN(
    "raw keyboard plugin",
    rawkb,
    rawkb_init,
    rawkb_deinit
)

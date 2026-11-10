// resource.h — 资源 ID 与版本宏；app.rc（rc.exe）与 C++ 代码共用，只能放预处理宏。
#ifndef MOTIONCALM_RESOURCE_H
#define MOTIONCALM_RESOURCE_H

#define IDI_APP          101   /* 托盘 / exe 图标 */
#define IDR_DEFAULT_INI  201   /* RCDATA：MotionCalm.default.ini 模板（UTF-8 BOM + CRLF） */

#define MC_VERSION_MAJOR 1
#define MC_VERSION_MINOR 2
#define MC_VERSION_PATCH 0
#define MC_VERSION_BUILD 0
#define MC_VERSION_STR       "1.2.0.0"   /* VERSIONINFO 的 FileVersion / ProductVersion */
#define MC_VERSION_SHORT_STR "1.2"       /* 菜单标题、关于框："MotionCalm v1.2" */

/* ---------------------------------------------------------------- 设置窗口（变更 4 settings-window）
   对话框模板用 300–306（与 RCDATA 201 错开，避免看起来像同一个 ID）。
   控件 ID：外壳 1000–1099、遮罩 1100–1199、参照物 1200–1299、触发 1300–1399、校准 1400–1499、
   快捷键 1500–1599、通用 1600–1699。控件全部由代码按 docs/settings-ui.md 的版式创建（模板只是容器）；
   滑块右侧的数值读数 = 滑块 ID + 1。 */
#define IDD_SETTINGS        300
#define IDD_PAGE_MASK       301
#define IDD_PAGE_DOTS       302
#define IDD_PAGE_TRIGGER    303
#define IDD_PAGE_CALIB      304
#define IDD_PAGE_HOTKEYS    305
#define IDD_PAGE_GENERAL    306

/* 外壳（"关闭"按钮直接用 IDCANCEL，没有 IDOK 按钮） */
#define IDC_TAB                     1001
#define IDC_STATUS_TEXT             1002   /* 底部状态栏："所有更改立即生效并自动保存 · 上次保存 …" */
#define IDC_RESTORE_DEFAULTS        1003   /* 状态栏右侧"恢复默认…" */
#define IDC_STATUS_LINE             1004   /* 状态栏上方的分隔线 */
#define IDC_SHOW_ADVANCED           1090   /* 各页页底"显示高级选项"（每页一个，同 ID） */

/* 遮罩页 */
#define IDC_MASK_ENABLED            1101
#define IDC_MASK_LEVEL_LIGHT        1103
#define IDC_MASK_LEVEL_MEDIUM       1104
#define IDC_MASK_LEVEL_STRONG       1105
#define IDC_MASK_COLOR              1106
#define IDC_MASK_COLOR_SWATCH       1107
#define IDC_MASK_OPACITY_L          1110
#define IDC_MASK_OPACITY_L_VAL      1111
#define IDC_MASK_OPACITY_M          1114
#define IDC_MASK_OPACITY_M_VAL      1115
#define IDC_MASK_OPACITY_S          1118
#define IDC_MASK_OPACITY_S_VAL      1119
#define IDC_MASK_CLEAR_L            1122
#define IDC_MASK_CLEAR_L_VAL        1123
#define IDC_MASK_CLEAR_M            1126
#define IDC_MASK_CLEAR_M_VAL        1127
#define IDC_MASK_CLEAR_S            1130
#define IDC_MASK_CLEAR_S_VAL        1131
#define IDC_MASK_ELLIPSE            1140
#define IDC_MASK_ELLIPSE_VAL        1141
#define IDC_MASK_FEATHER            1144
#define IDC_MASK_FEATHER_VAL        1145
#define IDC_MASK_ATTACK             1148
#define IDC_MASK_ATTACK_VAL         1149
#define IDC_MASK_RELEASE            1152
#define IDC_MASK_RELEASE_VAL        1153
#define IDC_MASK_PREVIEW            1160
#define IDC_MASK_PREVIEW_VAL        1161

/* 参照物页 */
#define IDC_DOTS_CENTER             1201
#define IDC_DOTS_CENTER_RADIUS      1204
#define IDC_DOTS_CENTER_RADIUS_VAL  1205
#define IDC_DOTS_CENTER_RADIUS_UNIT 1206
#define IDC_DOTS_CENTER_RADIUS_LBL  1207
#define IDC_DOTS_CENTER_COLOR       1210
#define IDC_DOTS_CENTER_SWATCH      1211
#define IDC_DOTS_CENTER_COLOR_LBL   1212
#define IDC_DOTS_CENTER_OUTLINE     1214
#define IDC_DOTS_EDGE               1221
#define IDC_DOTS_EDGE_RADIUS        1224
#define IDC_DOTS_EDGE_RADIUS_VAL    1225
#define IDC_DOTS_EDGE_RADIUS_UNIT   1226
#define IDC_DOTS_EDGE_RADIUS_LBL    1227
#define IDC_DOTS_EDGE_COLOR         1230
#define IDC_DOTS_EDGE_SWATCH        1231
#define IDC_DOTS_EDGE_COLOR_LBL     1232
#define IDC_DOTS_EDGE_MIN           1236
#define IDC_DOTS_EDGE_MIN_VAL       1237
#define IDC_DOTS_EDGE_MIN_UNIT      1238
#define IDC_DOTS_EDGE_MIN_LBL       1239
#define IDC_DOTS_EDGE_MAX           1240
#define IDC_DOTS_EDGE_MAX_VAL       1241
#define IDC_DOTS_EDGE_MAX_UNIT      1242
#define IDC_DOTS_EDGE_MAX_LBL       1243
#define IDC_DOTS_EDGE_MARGIN        1244
#define IDC_DOTS_EDGE_MARGIN_VAL    1245
#define IDC_DOTS_EDGE_MARGIN_UNIT   1246
#define IDC_DOTS_EDGE_MARGIN_LBL    1247

/* 触发页 */
#define IDC_TRIG_MOUSE              1301
#define IDC_TRIG_KEYBOARD           1302
#define IDC_TRIG_GAMEPAD            1303
#define IDC_TRIG_CAPTURE            1304
#define IDC_TRIG_SENS               1305
#define IDC_TRIG_CAPTURE_SENS       1306
#define IDC_TRIG_LOCK_RULE          1307
#define IDC_TRIG_MOVE               1310
#define IDC_TRIG_MOVE_VAL           1311
#define IDC_TRIG_PAD_MAX            1314
#define IDC_TRIG_PAD_MAX_VAL        1315
#define IDC_TRIG_PAD_DZ             1318
#define IDC_TRIG_PAD_DZ_VAL         1319
#define IDC_TRIG_VWIN               1322
#define IDC_TRIG_VWIN_VAL           1323
#define IDC_TRIG_VSTART_LOW         1330
#define IDC_TRIG_VFULL_LOW          1331
#define IDC_TRIG_VSTART_MEDIUM      1332
#define IDC_TRIG_VFULL_MEDIUM       1333
#define IDC_TRIG_VSTART_HIGH        1334
#define IDC_TRIG_VFULL_HIGH         1335
#define IDC_TRIG_HFOV               1340
#define IDC_TRIG_HFOV_VAL           1341
#define IDC_TRIG_MINCORR            1344
#define IDC_TRIG_MINCORR_VAL        1345
#define IDC_TRIG_HOLD               1348
#define IDC_TRIG_HOLD_VAL           1349
#define IDC_TRIG_MAXSHIFT           1352
#define IDC_TRIG_GRIDW              1353
#define IDC_TRIG_GRIDH              1354
#define IDC_TRIG_FWD_GAIN           1356   /* 变更 5：前进 / 后退的灵敏度 */
#define IDC_TRIG_FWD_GAIN_VAL       1357
#define IDC_TRIG_ROLL               1358   /* 变更 5：识别画面倾斜 */
/* 变更 5：前进 / 后退与倾斜各自的阈值，3 档 × 开始 / 完全：前进 1360–1365，倾斜 1366–1371（start_low, full_low, start_medium, …） */
#define IDC_TRIG_ZOOM_TH_BASE       1360
#define IDC_TRIG_ROLL_TH_BASE       1366

/* 校准页 */
#define IDC_CAL_LIST                1401
#define IDC_CAL_RECAL               1402
#define IDC_CAL_DELETE              1403
#define IDC_CAL_HINT                1405
#define IDC_CAL_DEFAULT_CPD         1406

/* 快捷键页：行按热键动作表生成——标签 1510+i、捕获框 1530+i、状态文字 1550+i、"清除" 1570+i、状态图标 1590+i
   （i = 动作表序号，最多 20 行） */
#define IDC_HK_LABEL_BASE           1510
#define IDC_HK_EDIT_BASE            1530
#define IDC_HK_STATUS_BASE          1550
#define IDC_HK_CLEAR_BASE           1570
#define IDC_HK_ICON_BASE            1590

/* 通用页 */
#define IDC_GEN_AUTOSTART           1601
#define IDC_GEN_HIDE                1602
#define IDC_GEN_LOGLEVEL            1603
#define IDC_GEN_VERBOSE_NOTE        1604
#define IDC_GEN_EXCLUDE_LIST        1610
#define IDC_GEN_EXCLUDE_EDIT        1611
#define IDC_GEN_EXCLUDE_ADD         1612
#define IDC_GEN_EXCLUDE_DEL         1613
#define IDC_GEN_FORCE_LIST          1620
#define IDC_GEN_FORCE_EDIT          1621
#define IDC_GEN_FORCE_ADD           1622
#define IDC_GEN_FORCE_DEL           1623
#define IDC_GEN_KNOWN_COUNT         1630
#define IDC_GEN_KNOWN_CLEAR         1631
#define IDC_GEN_OPEN_CONFIG         1640
#define IDC_GEN_LANGUAGE            1650   /* localization：界面语言 */

#endif /* MOTIONCALM_RESOURCE_H */

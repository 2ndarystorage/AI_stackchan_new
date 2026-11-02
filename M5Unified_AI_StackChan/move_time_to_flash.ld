/* 第9.23弾: IRAM 救済用の追加リンカスクリプト
 * BT コントローラ (libbtdm_app.a, prebuilt) の IRAM 占有で iram0_0_seg が
 * 超過するため、タスク文脈からしか呼ばれない libc の時刻系オブジェクトを
 * IRAM からフラッシュへ移す（合計 約9KB）。
 *   strftime/strptime : Arduino Print・M5Unified の時刻整形 / HTTPClient の日時解析
 *   mktime/timelocal  : M5Unified RTC・スケジューラの時刻変換
 *   gmtime/gmtime_r/lcltime/lcltime_r/tzset/tzset_r/tzcalc_limits : 時刻表示・SNTP
 * いずれも ISR やフラッシュ書込みのクリティカル区間からは呼ばれない純粋な
 * 計算処理であり、通常のアプリコードと同じフラッシュ配置で問題ない。
 * framework の .flash.text と同名の出力セクションとして定義して統合し、
 * INSERT BEFORE .iram0.text で .iram0.text の明示リストより先に確保する
 * （framework 側 .flash.text のワイルドカードは EXCLUDE_FILE でこれらを
 * 除外しているため競合しない）。
 */
SECTIONS
{
  .flash.text :
  {
    . = ALIGN(4);
    *libc.a:lib_a-strftime.*(.literal .literal.* .text .text.*)
    *libc.a:lib_a-strptime.*(.literal .literal.* .text .text.*)
    *libc.a:lib_a-mktime.*(.literal .literal.* .text .text.*)
    *libc.a:lib_a-timelocal.*(.literal .literal.* .text .text.*)
    *libc.a:lib_a-gmtime.*(.literal .literal.* .text .text.*)
    *libc.a:lib_a-gmtime_r.*(.literal .literal.* .text .text.*)
    *libc.a:lib_a-lcltime.*(.literal .literal.* .text .text.*)
    *libc.a:lib_a-lcltime_r.*(.literal .literal.* .text .text.*)
    *libc.a:lib_a-tzset.*(.literal .literal.* .text .text.*)
    *libc.a:lib_a-tzset_r.*(.literal .literal.* .text .text.*)
    *libc.a:lib_a-tzcalc_limits.*(.literal .literal.* .text .text.*)
    . = ALIGN(4);
  } > iram0_2_seg
}
INSERT BEFORE .iram0.text;

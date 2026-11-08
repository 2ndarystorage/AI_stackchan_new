void clear_buff();
void wakeword_setup();
void wakeword_init();
bool wakeword_has_template();  // 登録テンプレートが読み込まれているか（第9.34弾）
bool wakeword_regist();
bool wakeword_compare();
extern int mode;   // 0: none, <0: REGIST, >0: COMPARE

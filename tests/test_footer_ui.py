"""Regression checks for shared footer routing and non-overlapping layout."""
import unittest
from test_tts_transport import SRC, body

class FooterTests(unittest.TestCase):
    def test_button_and_touch_share_dispatch_and_legacy_gestures_are_removed(self):
        source = (SRC/'main.cpp').read_text()
        loop = body(source, 'void loop()')
        self.assertIn('if (key >= 0) footerAction(key);', loop)
        for key in ('BtnA.wasPressed()', 'BtnB.wasPressed()', 'BtnC.wasPressed()'):
            self.assertIn(key, loop)
        self.assertIn('touch.y >= 208', loop)
        self.assertNotIn('pressedFor(2000)', loop)
        self.assertNotIn('confirm_menu_selection();', loop)
        action = body(source, 'static void footerAction(int key)')
        self.assertIn('confirm_menu_selection();', action)
        self.assertIn('stop_news_speech();', action)
        self.assertIn('change_led_level(key == 0 ? -1 : 1)', action)

    def test_footer_and_captions_have_separate_areas(self):
        source = (SRC/'main.cpp').read_text()
        draw = body(source, 'void stackchan_draw_footer(M5Canvas* spi) {')
        self.assertIn('fillRect(0, 208, 320, 32', draw)
        self.assertIn('g_footer_state.load()', draw)
        self.assertNotIn('footerState()', draw)
        face = (SRC/'KinakoFace.h').read_text()
        self.assertIn('fillRoundRect(0, 164, 320, 44', face)
        self.assertIn('setCursor(8, 186)', face)
        self.assertIn('NV_PAGE_LINES 9', face)

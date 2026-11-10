"""Exercise menu confirmation and RSS item limits without fetching news."""
import unittest

from test_tts_transport import body
import test_v916_diagnostics as diagnostics


class NewsMenuTests(unittest.TestCase):
    def test_menu_labels_and_settings_do_not_start_news(self):
        source = (diagnostics.SRC / 'main.cpp').read_text()
        definitions = source.split('struct NewsCatDef {', 1)[1].split(
            '// カテゴリごとの側面LED色', 1)[0]
        diagnostics.DiagnosticsTests().compile_run(r'''
#include <cassert>
#include <cstring>
constexpr int NEWS_MENU_ITEM_COUNT=6;
''' + 'struct NewsCatDef {' + definitions + r'''
bool g_menu_open=true, settings=false;
int g_menu_sel=0, g_last_news_cat=0, played=-1, speech=0;
struct { void setSpeechText(const char*) { ++speech; } } avatar;
void open_settings_dialog() { settings=true; }
void exec_news_category(int cat) { played=cat; }
''' + body(source, 'static void confirm_menu_selection()') + r'''
int main() {
  const char* labels[]={"一般ニュース","AI関連ニュース","芸能ニュース",
                        "経済ニュース","その他","設定"};
  for (int i=0;i<NEWS_MENU_ITEM_COUNT;++i) assert(!strcmp(newsMenuLabel(i),labels[i]));
  g_menu_sel=3; confirm_menu_selection();
  assert(!g_menu_open && !settings && g_last_news_cat==3 && played==3 && speech==1);
  g_menu_open=true; g_menu_sel=5; confirm_menu_selection();
  assert(!g_menu_open && settings && g_last_news_cat==3 && played==3 && speech==1);
}
''')

    def test_manual_rss_can_read_five_and_default_stays_three(self):
        source = (diagnostics.SRC / 'main.cpp').read_text()
        diagnostics.DiagnosticsTests().compile_run(r'''
#include <cassert>
#include <string>
class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& s):std::string(s) {}
  int indexOf(const char* s,int pos) const {
    auto i=find(s,pos); return i==npos ? -1 : static_cast<int>(i);
  }
};
String feed;
int g_nhk_http_code=200;
bool g_news_feed_ok=false;
struct { void printf(const char*,int) {} } Serial;
void delay(int) {}
String fetchNhkRssBody(int) { return feed; }
String xmlExtract(const String& s,const char* tag,int pos) {
  auto key=std::string("<")+tag+">";
  auto a=s.find(key,pos); if (a==String::npos) return "";
  a+=key.size();
  return s.substr(a,s.find(std::string("</")+tag+">",a)-a);
}
int newsParseRssDate(const String&) { return 0; }
bool newsAcceptArticle(const String& title,int) { return title!="seen"; }
''' + body(source, 'String fetchNewsText(int nhkCat,') + r'''
int main() {
  feed="<item><title>seen</title></item>";
  for(int i=1;i<=6;++i) feed+="<item><title>"+std::to_string(i)+"</title></item>";
  String items[5],titles[5],descs[5];
  int count=0;
  fetchNewsText(0,"",items,titles,descs,5,count);
  assert(count==3 && titles[2]=="3");
  fetchNewsText(0,"",items,titles,descs,5,count,5);
  assert(count==5 && titles[0]=="1" && titles[4]=="5");
  feed="<item><title>seen</title></item><item><title>fresh</title></item>";
  fetchNewsText(0,"",items,titles,descs,5,count,5);
  assert(count==1 && titles[0]=="fresh");
}
''')

"""Compile the firmware's actual splitter with a small Arduino String adapter."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class SpeechChunks(unittest.TestCase):
    def test_merging_and_punctuation(self):
        source = (ROOT / 'M5Unified_AI_StackChan/src/main.cpp').read_text()
        code = source.split('static std::deque<TtsChunk> splitSpeechChunks(', 1)[1]
        code = 'static std::deque<TtsChunk> splitSpeechChunks(' + code.split('\n// 発話テキスト', 1)[0]
        adapter = r'''
#include <string>
#include <deque>
#include <cstdint>
#include <cassert>
class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& s):std::string(s) {}
  String substring(int a,int b) const { return substr(a,b-a); }
  void trim() { auto a=find_first_not_of(" \r\n\t"); if(a==npos) clear(); else *this=substr(a,find_last_not_of(" \r\n\t")-a+1); }
};
struct TtsChunk { String text,parms; };
int newsUtf8CharLen(uint8_t b) { return b<128?1:b<224?2:b<240?3:4; }
'''
        checks = r'''
int main() {
 auto merged=splitSpeechChunks("今日のニュースです。長い記事の見出しです。次の記事です。","speaker");
 assert(merged.size()==1);
 String sentence; for(int i=0;i<95;i++) sentence+="あ";
 auto comma=splitSpeechChunks(sentence+"、続きです。","p");
 assert(comma.size()==1); // Japanese comma does not end a sentence.
 auto bang=splitSpeechChunks(sentence+"！"+sentence+"？","p");
 assert(bang.size()==2);
 assert(bang[0].text==sentence+"！" && bang[1].text==sentence+"？");
 assert(splitSpeechChunks("\n\n","p").empty());
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src=pathlib.Path(tmp)/'split.cpp'; exe=pathlib.Path(tmp)/'split'
            src.write_text(adapter+code+checks)
            subprocess.run(['c++','-std=c++11',str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__ == '__main__': unittest.main()

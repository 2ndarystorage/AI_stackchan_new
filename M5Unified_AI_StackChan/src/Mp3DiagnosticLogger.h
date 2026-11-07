#pragma once
#include <Arduino.h>
#include <string.h>

class Mp3DiagnosticLogger : public Print {
 public:
  Mp3DiagnosticLogger(Print& output, bool (*atEnd)()) : output_(output), atEnd_(atEnd) {}
  using Print::write;
  size_t write(uint8_t ch) override {
    if (ch == '\n') {
      line_[length_] = '\0';
      if (!overflow_ && atEnd_() && strcmp(line_, "MP3:ERROR_BUFLEN 0") == 0) {
        output_.println("[MP3] buffer refill at source EOF (decoder continues)");
      } else {
        output_.write(reinterpret_cast<const uint8_t*>(line_), length_);
        output_.write(ch);
      }
      length_ = 0;
      overflow_ = false;
      return 1;
    }
    if (length_ == sizeof(line_) - 1) {
      output_.write(reinterpret_cast<const uint8_t*>(line_), length_);
      length_ = 0;
      overflow_ = true;
    }
    line_[length_++] = ch;
    return 1;
  }
 private:
  Print& output_;
  bool (*atEnd_)();
  char line_[80] = {};
  size_t length_ = 0;
  bool overflow_ = false;
};

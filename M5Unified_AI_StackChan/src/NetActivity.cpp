#include "NetActivity.h"
#include "Forensics.h"
#include <atomic>

namespace {
std::atomic<int> g_net_count{0};
}  // namespace

void netActivityBegin() { g_net_count.fetch_add(1); }
void netActivityEnd() { g_net_count.fetch_sub(1); }
bool netActivityActive() { return g_net_count.load() > 0; }
int netActivityCount() { return g_net_count.load(); }

void netActivityBeginTag(uint8_t fxNetTag) {
  netActivityBegin();
  fxLog(FXK_NET_BEGIN, fxNetTag);
}

void netActivityEndTag(uint8_t fxNetTag) {
  fxLog(FXK_NET_END, fxNetTag);
  netActivityEnd();
}

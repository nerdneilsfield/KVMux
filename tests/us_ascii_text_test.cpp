#include "input/us_ascii_text.hpp"
#include <cstdlib>
#include <stdexcept>
using namespace kvmux;
void check(bool b) { if (!b) throw std::runtime_error("failed"); }
int main() {
 auto a=map_us_ascii_text("aA!\t\r\n"); check(a && a.normalized_characters==5 && a.gestures.size()==5);
 check(a.gestures[1].edges.size()==4 && a.gestures[1].edges[0].usage==0xe1 && a.gestures[1].edges[1].usage==0x04);
 auto invalid=map_us_ascii_text("ok\xC3\xA9"); check(invalid.error==TextPasteError::non_ascii && invalid.gestures.empty());
 check(map_us_ascii_text("x\ry").error==TextPasteError::bare_carriage_return);
 check(map_us_ascii_text(std::string(1025,'a')).error==TextPasteError::too_long);
 auto filtered=filter_us_ascii_text("A\r\nB\r\x01\xC3\xA9"); check(filtered.text=="A\nB" && filtered.removed==4 && filtered.normalized_characters==3);
 return EXIT_SUCCESS;
}
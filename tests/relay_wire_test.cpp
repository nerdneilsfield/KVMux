#include "network/relay_wire.hpp"
#include <cstdlib>
int main() { namespace w=kvmux::relay::wire; auto c=w::Control{w::PasteExecute{1}}; auto b=w::encode_control(c,w::Direction::client_to_server); if(!b || !w::decode_control(*b,w::Direction::client_to_server)) return 1; auto s=w::Control{w::PasteStatus{1,w::PasteState::finished,0,1,1,w::PasteOutcome::completed,w::PasteStatusReason::none}}; b=w::encode_control(s,w::Direction::server_to_client); return b&&w::decode_control(*b,w::Direction::server_to_client)?0:1; }

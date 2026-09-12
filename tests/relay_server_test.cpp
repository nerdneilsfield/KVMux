#include "relay_integration_fakes.hpp"
int main(int argc, char** argv) {
    assert(argc > 1);
    {
        RelayFixture capped(argv[1], 24000);
        GuiFixture gui(capped.options()); gui.activate();
        const auto initial = gui.client->traffic_snapshot().video_received_bytes;
        gui.run_for(1000ms);
        const auto bytes = gui.client->traffic_snapshot().video_received_bytes - initial;
        assert(bytes <= 24000 + 4800 && bytes > 0);
        assert(gui.session.snapshot().video_fresh);
    }
    RelayFixture relay(argv[1]);
    assert(relay.server.control_port() != relay.server.video_port());
    UdpProxy proxy(relay.options()); proxy.drop_welcome = 1; proxy.drop_ready = 1;
    proxy.wrong_source_only = true;
    GuiFixture gui(proxy.options()); gui.run_for(500ms);
    assert(gui.client->capture_snapshot().received_samples == 0);
    proxy.wrong_source_only = false; proxy.wrong_tuple_only = true;
    gui.run_for(100ms); assert(gui.client->capture_snapshot().received_samples == 0);
    proxy.wrong_tuple_only = false; gui.activate();
    const auto id = proxy.session_id.load(); assert(id);
    auto second = std::make_shared<kvmux::relay::RelayClient>(relay.options()); second->start();
    assert(until([&] { return second->control_snapshot().state == ControlConnectionState::disconnected; }));
    assert(second->control_snapshot().error == "Relay already controlled");
    gui.run_for(100ms); assert(proxy.session_id == id);
    assert(proxy.challenges > 2 && proxy.proofs > 2);
    gui.session.release_control();
    assert(gui.wait([&] { return gui.session.snapshot().input_state == InputState::preview; }));
}

#include <iostream>

#include "relay_integration_fakes.hpp"
int main(int argc, char** argv) {
  assert(argc > 1);
  for (const auto duration : {100ms, 300ms, 800ms, 2000ms}) {
    RelayFixture relay(argv[1]);
    UdpProxy proxy(relay.options());
    GuiFixture gui(proxy.options());
    gui.activate();
    const auto id = proxy.session_id.load();
    gui.session.handle_input({InputKey{0x04, true, false}});
    gui.run_for(60ms);
    proxy.blackout = true;
    gui.run_for(duration);
    if (duration >= 300ms)
      assert(gui.session.snapshot().input_state == InputState::recovering);
    gui.session.handle_input({InputKey{0x04, false, false}});
    gui.session.handle_input({InputRelativeMotion{23, 17}});
    gui.session.handle_input({InputWheel{2, 50, 50}});
    gui.session.handle_input(
        {InputButton{InputMouseButton::right, true, 50, 50}});
    gui.session.handle_input(
        {InputButton{InputMouseButton::right, false, 50, 50}});
    std::size_t resume_reports{};
    {
      std::lock_guard lock(relay.serial.mutex);
      resume_reports = relay.serial.received.size();
    }
    proxy.blackout = false;
    assert(gui.wait(
        [&] {
          return gui.session.snapshot().input_state == InputState::captured;
        },
        4000ms));
    assert(proxy.session_id == id);
    gui.run_for(60ms);
    if (duration >= 300ms) {
      {
        std::lock_guard lock(relay.serial.mutex);
        for (std::size_t i = resume_reports; i < relay.serial.received.size();
             ++i) {
          const auto& report = relay.serial.received[i];
          if (report.command == 0x04U) {
            assert(report.data.size() == 7 && report.data[1] == 0 &&
                   report.data[6] == 0);
          }
          if (report.command == 0x05U) {
            assert(report.data.size() == 5 && report.data[1] == 0 &&
                   report.data[2] == 0 && report.data[3] == 0 &&
                   report.data[4] == 0);
          }
        }
      }
      const auto applied = relay.sink.snapshot().applied;
      assert(applied.known && applied.state.buttons == 0 &&
             applied.state.modifiers == 0);
      assert(std::all_of(applied.state.keys.begin(), applied.state.keys.end(),
                         [](auto key) { return key == 0; }));
    }
    // Focus and Host revoke retained intent even while cancellation is lost.
    proxy.blackout = true;
    gui.run_for(300ms);
    gui.session.focus_lost();
    gui.run_for(30ms);
    proxy.blackout = false;
    assert(gui.wait([&] {
      return gui.session.snapshot().input_state == InputState::preview;
    }));
    gui.run_for(150ms);
    assert(gui.session.snapshot().input_state == InputState::preview);
    gui.activate();
    proxy.blackout = true;
    gui.run_for(300ms);
    gui.session.handle_input({InputKey{0xe4, true, false}});
    proxy.blackout = false;
    assert(gui.wait([&] {
      return gui.session.snapshot().input_state == InputState::preview;
    }));
    gui.run_for(100ms);
    assert(gui.session.snapshot().input_state == InputState::preview);
    std::cout << "real UDP blackout " << duration.count()
              << "ms recovered same session\n";
  }
}

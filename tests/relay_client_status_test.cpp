#include "network/relay_client.hpp"

#include <cassert>

int main() {
    using namespace kvmux::relay;
    using namespace kvmux::relay::wire;

    PasteUploadSnapshot snapshot{PasteUploadState::uploaded, 7, 100, 80, 0, PasteStatusReason::none};
    snapshot = merge_paste_status(snapshot,
        {7, PasteState::preparing, 1, 100, 0, PasteOutcome::none, PasteStatusReason::none});
    assert(snapshot.state == PasteUploadState::preparing && snapshot.accepted_bytes == 100);

    snapshot = merge_paste_status(snapshot,
        {7, PasteState::uploaded, 1, 80, 0, PasteOutcome::none, PasteStatusReason::none});
    assert(snapshot.state == PasteUploadState::preparing && snapshot.accepted_bytes == 100);

    snapshot = merge_paste_status(snapshot,
        {7, PasteState::executing, 1, 100, 10, PasteOutcome::none, PasteStatusReason::none});
    snapshot = merge_paste_status(snapshot,
        {7, PasteState::uploaded, 1, 90, 0, PasteOutcome::none, PasteStatusReason::none});
    assert(snapshot.state == PasteUploadState::executing && snapshot.completed_bytes == 10);

    snapshot = merge_paste_status(snapshot,
        {7, PasteState::finished, 1, 100, 100, PasteOutcome::completed, PasteStatusReason::none});
    assert(snapshot.state == PasteUploadState::completed);
    const auto terminal = merge_paste_status(snapshot,
        {7, PasteState::uploaded, 1, 0, 0, PasteOutcome::none, PasteStatusReason::busy});
    assert(terminal.state == PasteUploadState::completed && terminal.completed_bytes == 100);
}

#include "aether/gfx/command_list.h"

#include "aether/gfx/device.h"

namespace aether::gfx {

CommandList::CommandList(Device& device, D3D12_COMMAND_LIST_TYPE type) {
    AETHER_D3D_CHECK(device.Handle()->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator_)));
    AETHER_D3D_CHECK(
        device.Handle()->CreateCommandList(0, type, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_)));
    AETHER_D3D_CHECK(list_->Close()); // command lists start open; Reset() expects a closed list
}

void CommandList::Reset(ID3D12PipelineState* initial_state) {
    AETHER_D3D_CHECK(allocator_->Reset());
    AETHER_D3D_CHECK(list_->Reset(allocator_.Get(), initial_state));
}

void CommandList::Close() {
    AETHER_D3D_CHECK(list_->Close());
}

} // namespace aether::gfx

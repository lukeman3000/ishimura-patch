// Vtable slots for the interfaces ishimurapatch hooks, taken from the SDK's
// C interface definitions so they can never drift from the real layout.
// Compiled as C (CINTERFACE exists only there); read by ishimurapatch.cpp.
#define CINTERFACE
#include <stddef.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#define DEV(f) (int)(offsetof(ID3D12Device10Vtbl, f) / sizeof(void*))
#define SWAP(f) (int)(offsetof(IDXGISwapChain3Vtbl, f) / sizeof(void*))

const int VT_CreateCommandQueue       = DEV(CreateCommandQueue);
const int VT_GetResourceAllocationInfo = DEV(GetResourceAllocationInfo);
const int VT_CreateCommittedResource  = DEV(CreateCommittedResource);
const int VT_CreateHeap               = DEV(CreateHeap);
const int VT_CreatePlacedResource     = DEV(CreatePlacedResource);
const int VT_CreateReservedResource   = DEV(CreateReservedResource);
const int VT_MakeResident             = DEV(MakeResident);
const int VT_Evict                    = DEV(Evict);
const int VT_EnqueueMakeResident      = DEV(EnqueueMakeResident);
const int VT_CreateCommittedResource1 = DEV(CreateCommittedResource1);
const int VT_CreateHeap1              = DEV(CreateHeap1);
const int VT_CreateReservedResource1  = DEV(CreateReservedResource1);
const int VT_CreateCommittedResource2 = DEV(CreateCommittedResource2);
const int VT_CreatePlacedResource1    = DEV(CreatePlacedResource1);
const int VT_CreateCommittedResource3 = DEV(CreateCommittedResource3);
const int VT_CreatePlacedResource2    = DEV(CreatePlacedResource2);
const int VT_CreateReservedResource2  = DEV(CreateReservedResource2);
const int VT_Present                  = SWAP(Present);
const int VT_Present1                 = SWAP(Present1);

#pragma once
#include "Utilities.h"






/*
 * DBDRootMotionSync (Dead by Daylight 6.7.2)
 *
 * Fixes P2P host <-> client desync where remote players' vaults/pallets (root-motion montages) run at a speed
 * proportional to the CLIENT's frame rate relative to the host (30 FPS client vs 60 FPS host = half speed,
 * 120 FPS client = ~double speed).
 *
 * Cause: player meshes are PlayerSkeletalMeshComponentBudgeted, i.e. tick rate controlled externally by the
 * animation budget allocator (USkinnedMeshComponent::bExternalTickRateControlled). The host replays every client move
 * (MoveAutonomous -> PerformMovement -> TickCharacterPose -> Mesh->TickPose(MoveDeltaTime)) to advance the root-motion
 * montage, but USkeletalMeshComponent::TickPose (0x14700DD60) ignores the passed DeltaTime when the mesh is externally
 * controlled and uses ExternalDeltaTime (= host frame time, set once per frame by the allocator) instead:
 *     14700de61  movss xmm6, [rdi+0x650]     ; DeltaTime = ExternalDeltaTime
 * so the montage advances one host frame per received client move, not by the move's real duration.
 * It also skips the tick entirely when the allocator did not flag the mesh for update this frame (bExternalUpdate).
 *
 * Fix: hook TickPose (vtable +0x970) of player meshes. Only for movement-driven ticks (bIsAutonomousTickPose, set by
 * UCharacterMovementComponent::TickCharacterPose) on externally controlled meshes, temporarily set
 * ExternalDeltaTime = DeltaTime and bExternalUpdate = 1, call the original, then restore both.
 * All other ticks (normal per-frame / budgeted ticks) are untouched.
 *
 * Offsets (not reflected, taken from 6.7.2 disassembly of TickPose / TickCharacterPose):
 *   USkinnedMeshComponent + 0x0650  float ExternalDeltaTime
 *   USkinnedMeshComponent + 0x06A9  bit 0 bExternalTickRateControlled, bit 2 bExternalUpdate
 *   USkeletalMeshComponent + 0x0962 bit 3 bIsAutonomousTickPose (SDK: bIsAutonomousTickPose)
 *
 * Should run on the host; harmless (and also correct) on clients.
 * Command line: -rootMotionSyncDebug  -> print every corrected tick (DeltaTime vs ExternalDeltaTime).
 */
class RootMotionSync
{
private:
	using TickPoseFn = void(__fastcall*)(void* thisPtr, float deltaTime, bool bNeedsValidRootMotion);

	static constexpr size_t SLOT_TICK_POSE = 0x970;

	static constexpr size_t OFFSET_EXTERNAL_DELTA_TIME = 0x650;
	static constexpr size_t OFFSET_EXTERNAL_FLAGS = 0x6A9;
	static constexpr uint8_t FLAG_EXTERNAL_TICK_RATE_CONTROLLED = 0x01;
	static constexpr uint8_t FLAG_EXTERNAL_UPDATE = 0x04;
	static constexpr size_t OFFSET_AUTONOMOUS_FLAGS = 0x962;
	static constexpr uint8_t FLAG_IS_AUTONOMOUS_TICK_POSE = 0x08;

	static constexpr int MAX_VTABLES = 16;

	struct HookedVTable
	{
		void** vtable;
		void* original;
	};

	static inline HookedVTable meshVTables[MAX_VTABLES] = {};
	static inline bool debugLogging = false;
	static inline volatile LONG correctedTicks = 0;

	static uintptr_t ToRVA(void* address);
	static void* FindOriginal(void* object);
	static bool HookMesh(void* mesh, const char* className);

	static void __fastcall HookTickPose(void* thisPtr, float deltaTime, bool bNeedsValidRootMotion);

	static void ScanCharacters();
	static DWORD WINAPI ThreadSafeWorker();


public:
	static void Init();
};

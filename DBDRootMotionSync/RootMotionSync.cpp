#include "RootMotionSync.h"






uintptr_t RootMotionSync::ToRVA(void* address)
{
	return (uintptr_t)address - (uintptr_t)GetModuleHandle(nullptr);
}

void* RootMotionSync::FindOriginal(void* object)
{
	void** vtable = *reinterpret_cast<void***>(object);
	for (int i = 0; i < MAX_VTABLES; i++)
		if (meshVTables[i].vtable == vtable)
			return meshVTables[i].original;
	return nullptr;
}

bool RootMotionSync::HookMesh(void* mesh, const char* className)
{
	void** vtable = *reinterpret_cast<void***>(mesh);

	for (int i = 0; i < MAX_VTABLES; i++)
		if (meshVTables[i].vtable == vtable)
			return true; /* already hooked */

	for (int i = 0; i < MAX_VTABLES; i++)
	{
		if (meshVTables[i].vtable != nullptr)
			continue;

		void** slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(vtable) + SLOT_TICK_POSE);
		DWORD oldProtect = 0;
		if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect) == FALSE)
		{
			printf("[DBDRootMotionSync] VirtualProtect failed for %s vtable RVA 0x%llX\n", className, (unsigned long long)ToRVA(vtable));
			return false;
		}

		/* Publish the original before redirecting the slot: the game thread may call the hook immediately. */
		meshVTables[i].original = *slot;
		MemoryBarrier();
		meshVTables[i].vtable = vtable;
		MemoryBarrier();
		*slot = reinterpret_cast<void*>(&HookTickPose);
		VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);

		printf("[DBDRootMotionSync] Hooked TickPose of %s: vtable RVA 0x%llX, original RVA 0x%llX\n",
			className, (unsigned long long)ToRVA(vtable), (unsigned long long)ToRVA(meshVTables[i].original));
		return true;
	}

	printf("[DBDRootMotionSync] No free vtable slot for %s\n", className);
	return false;
}




void __fastcall RootMotionSync::HookTickPose(void* thisPtr, float deltaTime, bool bNeedsValidRootMotion)
{
	TickPoseFn original = reinterpret_cast<TickPoseFn>(FindOriginal(thisPtr));
	if (original == nullptr)
		return;

	uint8_t* mesh = reinterpret_cast<uint8_t*>(thisPtr);
	const bool isAutonomousTick = (mesh[OFFSET_AUTONOMOUS_FLAGS] & FLAG_IS_AUTONOMOUS_TICK_POSE) != 0;
	const bool isExternallyControlled = (mesh[OFFSET_EXTERNAL_FLAGS] & FLAG_EXTERNAL_TICK_RATE_CONTROLLED) != 0;

	if (isAutonomousTick == false || isExternallyControlled == false)
	{
		original(thisPtr, deltaTime, bNeedsValidRootMotion);
		return;
	}

	float* externalDeltaTime = reinterpret_cast<float*>(mesh + OFFSET_EXTERNAL_DELTA_TIME);
	const float savedExternalDeltaTime = *externalDeltaTime;
	const uint8_t savedExternalUpdate = mesh[OFFSET_EXTERNAL_FLAGS] & FLAG_EXTERNAL_UPDATE;

	if (debugLogging)
	{
		LONG count = InterlockedIncrement(&correctedTicks);
		printf("[DBDRootMotionSync] #%ld mesh=%p DeltaTime=%.6f ExternalDeltaTime=%.6f bExternalUpdate=%d rootMotion=%d\n",
			count, thisPtr, deltaTime, savedExternalDeltaTime, savedExternalUpdate ? 1 : 0, bNeedsValidRootMotion ? 1 : 0);
	}

	*externalDeltaTime = deltaTime;
	mesh[OFFSET_EXTERNAL_FLAGS] |= FLAG_EXTERNAL_UPDATE;

	original(thisPtr, deltaTime, bNeedsValidRootMotion);

	*externalDeltaTime = savedExternalDeltaTime;
	mesh[OFFSET_EXTERNAL_FLAGS] = (mesh[OFFSET_EXTERNAL_FLAGS] & ~FLAG_EXTERNAL_UPDATE) | savedExternalUpdate;
}




void RootMotionSync::ScanCharacters()
{
	if (Utilities::Game::IsInMatch() == false)
		return;

	int32_t objectsNum = SDK::UObject::GObjects->Num();
	for (int i = 0; i < objectsNum; i++)
	{
		SDK::UObject* objectReference = SDK::UObject::GObjects->GetByIndex(i);
		if (objectReference == nullptr || objectReference->IsDefaultObject())
			continue;

		if (objectReference->IsA(SDK::ACharacter::StaticClass()) == false)
			continue;

		SDK::ACharacter* character = static_cast<SDK::ACharacter*>(objectReference);
		SDK::USkeletalMeshComponent* mesh = character->Mesh;
		if (mesh == nullptr || mesh->Class == nullptr)
			continue;

		/* Only player meshes driven by the animation budget allocator are affected. */
		std::string className = mesh->Class->GetName();
		if (className.find("Budgeted") == std::string::npos)
			continue;

		HookMesh(mesh, className.c_str());
	}
}

DWORD __stdcall RootMotionSync::ThreadSafeWorker()
{
	printf("[DBDRootMotionSync] ThreadSafeWorker() is now entering the loop\n");

	while (true)
	{
		EXCEPTION_POINTERS* exceptionInfo = nullptr;

		__try
		{
			ScanCharacters();
			Sleep(1000);
		}
		__except (exceptionInfo = GetExceptionInformation(), EXCEPTION_EXECUTE_HANDLER)
		{
			if (exceptionInfo)
			{
				void* crashAddress = exceptionInfo->ExceptionRecord->ExceptionAddress;
				DWORD exceptionCode = exceptionInfo->ExceptionRecord->ExceptionCode;

				printf("[DBDRootMotionSync] FATAL ERROR / CRASH in %s\n", __FUNCSIG__);
				printf("[DBDRootMotionSync] Exception Code:   0x%08X\n", exceptionCode);
				printf("[DBDRootMotionSync] Relative Offset:  0x%llX\n", (unsigned long long)ToRVA(crashAddress));
			}

			printf("[DBDRootMotionSync] Restarting loop in 3 seconds...\n");
			Sleep(3000);
		}
	}

	return 0;
}




void RootMotionSync::Init()
{
	printf("[DBDRootMotionSync] Starting initialization sequence...\n");
	while (Utilities::Game::IsInitialized() == false)
	{
		Sleep(10);
	}

	std::vector<std::wstring> processArguments = Utilities::CommandLine::GetArgumentsUtf16();
	for (std::wstring& arg : processArguments)
	{
		if (Utilities::String::StartsWith(arg, L"-rootMotionSyncDebug"))
		{
			printf("[DBDRootMotionSync] debugLogging = true\n");
			debugLogging = true;
		}
	}

	printf("[DBDRootMotionSync] Starting ThreadSafeWorker()...\n");
	CreateThread(0, 0, (LPTHREAD_START_ROUTINE)ThreadSafeWorker, 0, 0, 0);
}

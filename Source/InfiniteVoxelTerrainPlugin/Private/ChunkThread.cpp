// Copyright(c) 2024 Endless98. All Rights Reserved.

#include "ChunkThread.h"
#include "ChunkManager.h"
#include "Engine/World.h"
#include "Containers/Array.h"
#include "Math/IntVector.h"
#include "DrawDebugHelpers.h"

FCriticalSection FChunkThread::ChunkZMutex;
TMap<FIntPoint, TArray<int32>> FChunkThread::ChunkZIndicesBy2DCell{};
TMap<FIntPoint, TArray<int32>> FChunkThread::ModifiedAdditionalChunkZIndicesBy2DCell{};

bool FChunkThread::Init()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::Init);
	return true;
}

uint32 FChunkThread::Run()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::Run);

	InitializeNoiseGenerators();
	if (!WorldRef) return 1;

	while (bIsRunning) // Generate chunks until we are told to stop
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::RunLoop);

		UpdateTrackingVariables();
		UpdateTempVariables();

		if (PlayerLocations.IsEmpty() || !PlayerLocations.IsValidIndex(TrackedIndex))
		{
			if(WorldRef->GetNetMode() != NM_DedicatedServer)
				UE_LOG(LogTemp, Warning, TEXT("Thread %i has no tracked locations!"), ThreadIndex);
			FPlatformProcess::Sleep(ThreadIdleSleepTime);
			continue;
		}

		if (!bIsRunning || !WorldRef) return 0;

		UpdateChunks();

		if (bIsFirstTime)
			LastHeightmapLocation = PlayerLocations[TrackedIndex];

		if (!PrepareRegionForGeneration())
		{
			FPlatformProcess::Sleep(ThreadIdleSleepTime);
			continue;
		}

		FVector2D HeightmapLocation{};
		TArray<FVector2D>* CheckLocationsNeedingUnhide{}; // If we are the client, we need to check if we need to unhide chunks that were hidden
		bool bWasHeightmapNeeded{ FindNextNeededHeightmap(HeightmapLocation, CheckLocationsNeedingUnhide) };
		ChunkManagerRef->UnhideChunksInHeightmapLocations(CheckLocationsNeedingUnhide);
		LastHeightmapLocation = HeightmapLocation;

		if (!bWasHeightmapNeeded)
		{
			FPlatformProcess::Sleep(ThreadIdleSleepTime);
			continue;
		}

		TArray<TSharedPtr<FChunkConstructionData>> ChunkConstructionDataArray{};
		TArray<int32> TerrainZIndices{};
		
		if (!GenerateChunkData(HeightmapLocation, TerrainZIndices, ChunkConstructionDataArray))
			continue;
		
		AsyncSpawnChunks(ChunkConstructionDataArray, HeightmapLocation, TerrainZIndices);

		FPlatformProcess::Sleep(ThreadWorkingSleepTime);
	}

	return 0;
}

void FChunkThread::Stop()
{
	if (ThreadIndex > 0)
	{
		bIsRunning = false;

		return; // Only the first thread should save the world
	}

	if (WorldRef->GetNetMode() == ENetMode::NM_DedicatedServer || WorldRef->GetNetMode() == ENetMode::NM_ListenServer || WorldRef->GetNetMode() == ENetMode::NM_Standalone)
		SaveUnsavedRegions(false);

	bIsRunning = false;
}

void FChunkThread::InitializeNoiseGenerators()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::InitializeNoiseGenerators);

	BiomeNoiseGenerator = FastNoise::NewFromEncodedNodeTree("IgAAAEBAmpmZPhsAEABxPQo/GwAeABcAAAAAAAAAgD9cj8I+AACAPw0AAwAAAAAAQEAJAADsUbg+AOxRuD4AAAAAAAETAI/CdT7//wEAAOxROD4AAAAAQA==");
	PlainsNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EQACAAAAAAAgQBAAAAAAQCcAAQAAABsAIAAJAAAAAAAAAArXoz8BEwAK1yM/DQACAAAArkexQP//AAAAKVxPPwDNzEw+AM3MTD4AMzMzPwAAAAA/");
	ForestNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EQACAAAAAAAgQBAAAAAAQCcAAQAAABsAIAAJAAAAAAAAAArXoz8BEwAK1yM/DQACAAAArkexQP//AAAAKVxPPwDNzEw+AM3MTD4AMzMzPwAAAAA/");
	HillsNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EwBcj8I+EQADAAAAcT1qQBAAzcxMPg0AAwAAAB+FS0AnAAEAAAAJAAAfhes+AHE9Cj8ArkdhPwApXI8+AD0K1z4=");
	MountainsNoiseGenerator = FastNoise::NewFromEncodedNodeTree("EwAzM7M+EADhehQ/DQADAAAAhevBQCcAAQAAAAYAAAAAAD8AAACAPwAK1yM+");
}

// Returns false if no TrackedPlayers have moved
bool FChunkThread::UpdateTrackingVariables()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::UpdateTrackingVariables);

	TArray<FVector2D> NewTrackedLocations{  };

	{
		FReadScopeLock Lock(ChunkManagerRef->ThreadPlayerLocationsLock);
		NewTrackedLocations = ChunkManagerRef->ThreadUseableLocations;
	}

	if (NewTrackedLocations.IsEmpty() || !bIsRunning)
	{
		if (WorldRef->GetNetMode() != NM_DedicatedServer)
			UE_LOG(LogTemp, Warning, TEXT("No tracked locations found!"));

		bDidTrackedActorMove = false;
		return bDidTrackedActorMove;
	}

	for (FVector2D& TrackedLocation : NewTrackedLocations)
		TrackedLocation = GetLocationSnappedToChunkGrid2D(TrackedLocation, ChunkSize);

	if (PlayerLocations.Num() > 0)
		TrackedIndex = (TrackedIndex + 1) % PlayerLocations.Num();

	// If nothing has changed we don't need to continue
	if (NewTrackedLocations == PlayerLocations)
	{
		bDidTrackedActorMove = false;
		return bDidTrackedActorMove;
	}

	for (int32 PlayerIndex{}; PlayerIndex < NewTrackedLocations.Num(); PlayerIndex++)
	{
		if (!TrackedChunkRingCount.IsValidIndex(PlayerIndex))
		{
			TrackedChunkRingCount.Add(0);
			TrackedChunkRingDistance.Add(0);
		}
	}

	for (int32 TrackedActorIndex{}; TrackedActorIndex < PlayerLocations.Num(); TrackedActorIndex++)
	{
		if (!TrackedChunkRingCount.IsValidIndex(TrackedActorIndex))
		{
			TrackedChunkRingCount.Add(0);
			TrackedChunkRingDistance.Add(0);
		}

		if (!NewTrackedLocations.IsValidIndex(TrackedActorIndex) || !PlayerLocations.IsValidIndex(TrackedActorIndex) || !TrackedChunkRingCount.IsValidIndex(TrackedActorIndex) || !TrackedChunkRingDistance.IsValidIndex(TrackedActorIndex))
			continue;

		FVector2D CurrentActorLocation = NewTrackedLocations[TrackedActorIndex];
		FVector2D OldActorLocation = PlayerLocations[TrackedActorIndex];
		int32 ChunksMoved = FMath::Max(FMath::CeilToInt32(FVector2D::Distance(CurrentActorLocation, OldActorLocation)) / ChunkSize, 2) + 1;
		TrackedChunkRingCount[TrackedActorIndex] = FMath::Max(TrackedChunkRingCount[TrackedActorIndex] - ChunksMoved, 0);
		TrackedChunkRingDistance[TrackedActorIndex] = FMath::Max(TrackedChunkRingDistance[TrackedActorIndex] - ChunksMoved, 0);

		ChunkAngleIndex = 0;
	}

	PlayerLocations = NewTrackedLocations;

	bDidTrackedActorMove = true;
	return bDidTrackedActorMove;
}

void FChunkThread::UpdateTempVariables()
{
	if (bWasRangeChanged)
		bDidTrackedActorMove = true;

	{
		// These values might be changed by the game thread while we loop, so we copy them to local variables
		FScopeLock Lock(&ChunkGenMutex);

		TempGenerationRadius = ChunkGenerationRadius;
		TempCollisionGenRadius = CollisionGenerationRadius;
		TempChunkGenRadius = ChunkGenerationRadius;
	}

	if (GetGenDistanceShouldBeCollision(TrackedIndex))
		TempGenerationRadius = TempCollisionGenRadius;
	else
		TempGenerationRadius = TempChunkGenRadius;
}

void FChunkThread::UpdateChunks()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::UpdateChunks);

	if (!(bDidTrackedActorMove && ThreadIndex == 0))
		return;


	if (!WorldRef)
		return;
	TUniquePtr<TArray<FIntVector>> CellsToRemovePtr{ MakeUnique<TArray<FIntVector>>() };
	TUniquePtr<TArray<FIntVector>> CellsToUnreplicatePtr{ MakeUnique<TArray<FIntVector>>() };
	TUniquePtr<TArray<FIntVector>> CellsToUnhidePtr{ MakeUnique<TArray<FIntVector>>() };
	TUniquePtr<TArray<FIntVector>> CellsToHidePtr{ MakeUnique<TArray<FIntVector>>() };

	TArray TempPlayerLocations{ PlayerLocations };
	{
		FScopeLock HeightmapLock(&ChunkManagerRef->HeightmapMutex);
		FScopeLock ChunkZLock(&ChunkZMutex);
		bool bIsDedicatedServer{ WorldRef->GetNetMode() == ENetMode::NM_DedicatedServer };
		bool bIsListenServer{ WorldRef->GetNetMode() == ENetMode::NM_ListenServer };
		for (const FVector2D& ExistingHeightmapLocation : ChunkManagerRef->ExistingHeightmapLocations)
		{
			FIntPoint ChunkCell2D{ AChunkManager::Get2DCellFromChunkLocation2D(ExistingHeightmapLocation, ChunkSize) };
			TArray<int32>* ChunkZIndices{ ChunkZIndicesBy2DCell.Find(ChunkCell2D) };
			if (!ChunkZIndices)
				continue;

			bool bIsNeeded{ IsNeededHeightmapLocation(ExistingHeightmapLocation, TempPlayerLocations, TempChunkGenRadius + ChunkDeletionBuffer, TempCollisionGenRadius) };
			if (!bIsNeeded) // If we don't need the chunk at all, no other checks are performed
			{
				for (int32& ChunkZ : *ChunkZIndices)
				{
					FIntVector ChunkCell{ ChunkCell2D.X, ChunkCell2D.Y, ChunkZ };
					CellsToRemovePtr->Add(ChunkCell);
				}
				continue;
			}

			if (!bIsListenServer) // Only the listen server does chunk hiding here
				continue;

			bool bServerNeedsChunk{ bServerNeedsChunk = IsHeightmapInRange(ExistingHeightmapLocation, TempPlayerLocations[0], TempChunkGenRadius + ChunkDeletionBuffer) };
			if (bServerNeedsChunk)
			{
				for (int32& ChunkZ : *ChunkZIndices)
				{
					FIntVector ChunkCell{ ChunkCell2D.X, ChunkCell2D.Y, ChunkZ };
					CellsToUnhidePtr->Add(ChunkCell);
				}
			}
			else // Listen Server does not need to see the chunk
			{
				for (int32& ChunkZ : *ChunkZIndices)
				{
					FIntVector ChunkCell{ ChunkCell2D.X, ChunkCell2D.Y, ChunkZ };
					CellsToHidePtr->Add(ChunkCell);
				}
			}
			ChunkZIndices = nullptr;
		}

		for (const FIntVector& CellToRemove : *CellsToRemovePtr)
		{
			FIntPoint ChunkCell2D{ CellToRemove.X, CellToRemove.Y };
			FVector2D HeightmapLocation{ AChunkManager::GetLocationFromChunkCell(CellToRemove, ChunkSize) };
			ChunkManagerRef->ExistingHeightmapLocations.Remove(HeightmapLocation);
			ChunkZIndicesBy2DCell.Remove(ChunkCell2D);
		}
	}

	AsyncTask(ENamedThreads::GameThread, [ChunkManager = ChunkManagerRef, CellsToRemovePtr = MoveTemp(CellsToRemovePtr), CellsToUnreplicatePtr = MoveTemp(CellsToUnreplicatePtr), CellsToUnhidePtr = MoveTemp(CellsToUnhidePtr), CellsToHidePtr = MoveTemp(CellsToHidePtr)]() mutable
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::UpdateChunks::GameThread);

			{
				TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::UpdateChunks::GameThread::DestroyOrHideChunk);
				for (FIntVector& CellToRemove : *CellsToRemovePtr)
				{
					bool bWasHidden{};
					ChunkManager->DestroyOrHideChunk(CellToRemove, bWasHidden); // If this is the client, we do the chunk hiding here instead of destroying it
				}
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::UpdateChunks::GameThread::UnreplicateChunk);
				for (FIntVector& CellToUnreplicate : *CellsToUnreplicatePtr)
					ChunkManager->UnreplicateChunk(CellToUnreplicate);
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::UpdateChunks::GameThread::UnhideChunk);
				for (FIntVector& CellToUnhide : *CellsToUnhidePtr)
					ChunkManager->UnhideChunk(CellToUnhide);
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::UpdateChunks::GameThread::HideChunk);
				for (FIntVector& CellToHide : *CellsToHidePtr)
					ChunkManager->HideChunk(CellToHide); // We are using a queue so we can spread the hiding over multiple frames, as it is costly
			}
		});
}

bool FChunkThread::IsNeededHeightmapLocation(FVector2D ChunkLocation2D, const TArray<FVector2D>& TrackedLocationsRef, int32 ChunkGenRadius, int32 CollisionGenRadius)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::IsNeededHeightmapLocation);

	for (int32 LocationIndex{}; LocationIndex < TrackedLocationsRef.Num(); LocationIndex++)
	{
		FVector2D TrackedLocation{ TrackedLocationsRef[LocationIndex] };
		int32 GenRadius{ (GetGenDistanceShouldBeCollision(LocationIndex) ? CollisionGenRadius + ChunkDeletionBuffer : ChunkGenRadius) };
		if (IsHeightmapInRange(ChunkLocation2D, TrackedLocation, GenRadius))
			return true;
	}

	return false;
}

bool FChunkThread::PrepareRegionForGeneration()
{
	FIntPoint Region{ GetRegionByLocation(FVector2D(LastHeightmapLocation)) };
	bool bIsReadyForGeneration{ false };

	bool bShouldCheckForRegionData{ (bDidTrackedActorMove || bWasRangeChanged || bIsFirstTime) && ThreadIndex == 0 };
	if (bShouldCheckForRegionData)
	{
		bIsFirstTime = false;
		bWasRangeChanged = false;

		TArray<FIntPoint> RegionsToLoad{};
		TArray<FIntPoint> RegionsToSave{};
		GetRegionsToLoad(RegionsToLoad);
		GetRegionsToSave(RegionsToSave);
		if (WorldRef->GetNetMode() != NM_Client) // We don't need to load voxels on the client, we will get them from the server
		{
			for (FIntPoint RegionToLoad : RegionsToLoad)
			{
				LoadVoxelsForRegion(RegionToLoad, WorldSaveName);
				ChunkManagerRef->SendNeededRegionDataOnGameThread(RegionToLoad);
			}
		}

		bool bRemoveVoxelWhenSaved{ true };
		for (FIntPoint RegionToSave : RegionsToSave)
		{
			if (WorldRef->GetNetMode() != NM_Client) // When regions are getting saved this way, it's because they are no longer relevant, so we can remove the ModifiedVoxels stored in memory
				AsyncSaveVoxelsForRegion(RegionToSave, WorldSaveName, bRemoveVoxelWhenSaved);
			else // If we are on the client, we don't save data, but we use the RegionsPendingSave tracking system to know which regions we are safe to remove from memory. They will be sent again when needed
			{
				{
					FScopeLock Lock(&ChunkManagerRef->ModifiedVoxelsMutex);
					ChunkManagerRef->ModifiedVoxelsByCellByRegion.Remove(Region);
				}
				FScopeLock Lock(&ChunkManagerRef->RegionMutex);
				ChunkManagerRef->RegionsPendingSave.Remove(Region);
			}
		}
	}

	if (WorldRef->GetNetMode() != NM_Client)
	{
		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		if (!ChunkManagerRef->RegionsAlreadyLoaded.Contains(Region) && ChunkManagerRef->RegionsPendingLoad.Contains(Region))
		{
			Lock.Unlock();

			if (!bIsRunning || !WorldRef)
				return false;

			if (WorldRef->GetNetMode() != NM_Client && ThreadIndex == 0)
				LoadVoxelsForRegion(Region, WorldSaveName);

			return false;
		}
		else if (!ChunkManagerRef->RegionsAlreadyLoaded.Contains(Region) && !ChunkManagerRef->RegionsPendingLoad.Contains(Region))
		{
			ChunkManagerRef->RegionsPendingLoad.Add(Region);
			return false;
		}
	}
	else // Client
	{
		if (!ChunkManagerRef)
			return false;
		// If this region is not tracked as having data
		bool bDoesClientHaveRegionData{};
		{
			FScopeLock Lock(&ChunkManagerRef->RegionMutex);
			bDoesClientHaveRegionData = ChunkManagerRef->GetDoesClientHaveRegionData(nullptr, Region);
		}

		if (!bDoesClientHaveRegionData)
		{
			FPlatformProcess::Sleep(ThreadIdleSleepTime);
			return false;
		}
	}

	return true;
}

bool FChunkThread::FindNextNeededHeightmap(FVector2D& OutHeightmapLocation, TArray<FVector2D>*& OutLocationsNeedingUnhide)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::FindNextNeededHeightmap);
	const FVector2D& TrackedLocation{ PlayerLocations[TrackedIndex] };
	int32& RingChunkDistance{ TrackedChunkRingDistance[TrackedIndex] };
	int32& RingCount{ TrackedChunkRingCount[TrackedIndex] };
	const int32& ChunkGenRadius{ TempGenerationRadius };
	const int32& CollisionGenRadius{ TempCollisionGenRadius };

	OutLocationsNeedingUnhide = nullptr;
	if (!WorldRef)
	{
		UE_LOG(LogTemp, Error, TEXT("WorldRef was nullptr!"));
		return false;
	}

	bool bFoundNeededHeightmap{ false };
	while (RingChunkDistance <= ChunkGenRadius && bIsRunning) // Loop until we find a needed heightmap or we reach the edge of the generation radius
	{
		if (LastRingCount != RingCount) // If the radius has changed
		{
			CircumferenceInChunks = FMath::Max(CalculateCircumferenceInChunks(RingCount, ChunkSize), 1);
			ChunkAngleIndex = 0;
		}
		LastRingCount = RingCount;

		while (ChunkAngleIndex < CircumferenceInChunks && bIsRunning)
		{
			float ChunkYawAngle = (360.f / CircumferenceInChunks) * ChunkAngleIndex;
			FVector2D HeightmapLocation = FVector2D(GetLocationSnappedToChunkGrid2D(TrackedLocation + FVector2D(FRotator(0, ChunkYawAngle, 0).Vector()) * (FVector2D(ChunkSize) * RingCount / 2.0), ChunkSize));

			if (ChunkAngleIndex == 0)
				RingChunkDistance = FMath::RoundToInt32(FMath::Abs(FVector2D::Distance(HeightmapLocation, TrackedLocation)) / ChunkSize);

			bool bHeightmapNeedsCollision{ DoesLocationNeedCollision(HeightmapLocation, PlayerLocations, CollisionGenRadius) };
			{
				FScopeLock Lock(&ChunkManagerRef->HeightmapMutex);
				if (ChunkManagerRef && !ChunkManagerRef->ExistingHeightmapLocations.Contains(HeightmapLocation))
				{
					ChunkManagerRef->ExistingHeightmapLocations.Emplace(HeightmapLocation);
					OutHeightmapLocation = HeightmapLocation;
					ChunkAngleIndex++;
					bFoundNeededHeightmap = true;

					return bFoundNeededHeightmap;
				}
				else if (WorldRef->GetNetMode() == NM_Client || WorldRef->GetNetMode() == NM_ListenServer) // Location did have a chunk
				{
					if (OutLocationsNeedingUnhide)
						OutLocationsNeedingUnhide->Add(HeightmapLocation);
					else
					{
						OutLocationsNeedingUnhide = new TArray<FVector2D>();
						OutLocationsNeedingUnhide->Add(HeightmapLocation);
					}
				}
			}
			ChunkAngleIndex++;
		}
        // We've completed a circle
		if (ChunkAngleIndex == CircumferenceInChunks)
			RingCount++;
	}

	return bFoundNeededHeightmap;
}

int32 FChunkThread::CalculateCircumferenceInChunks(const int32 RadiusInChunks, float ChunkSize)
{
	// Calculate the circumference of the circle in units
	float CircumferenceInUnits{ 2.0f * PI * (RadiusInChunks * ChunkSize) };
	// Convert the circumference to chunks based on the chunk scale along the X and Y axes
	int32 TempCircumferenceInChunks{ FMath::CeilToInt32(CircumferenceInUnits / ChunkSize) };

	return TempCircumferenceInChunks;
}

bool FChunkThread::GenerateChunkData(FVector2D& HeightmapLocation, TArray<int32>& TerrainZIndices, TArray<TSharedPtr<FChunkConstructionData>>& ChunkConstructionDataArray)
{
	TArray<int16> Heightmap{};
	GenerateHeightmap(Heightmap, HeightmapLocation, TerrainZIndices);
	CombineChunkZIndices(HeightmapLocation, TerrainZIndices);

	if (!AddConstructionData(ChunkConstructionDataArray, HeightmapLocation, TerrainZIndices))
		return false;
	
	GenerateVoxelsForChunks(ChunkConstructionDataArray, Heightmap);
	GenerateMeshDataForChunks(ChunkConstructionDataArray);
	// If none of the tracked actors have collision for this chunk, it shouldn't be modifiable, so we are probably safe to compress it. You may want to modify this if this assumption doesn't work with your code
	if (!DoesLocationNeedCollision(HeightmapLocation, PlayerLocations, TempCollisionGenRadius))
		CompressVoxelData(ChunkConstructionDataArray); // Compress the voxel data if it is not close enough for collision, as we can't modify chunks without collision anyway (Assuming we are using line traces)

	return true;
}

void FChunkThread::GenerateHeightmap(TArray<int16>& OutGeneratedHeightmap, const FVector2D& NeededHeightmapLocation, TArray<int32>& OutNeededChunksVerticalIndices)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateHeightmap);

	int32 const HeightmapVoxels1D{ VoxelCount + 2 };
	int32 const TotalHeightmapVoxels{ HeightmapVoxels1D * HeightmapVoxels1D };

	OutGeneratedHeightmap.Empty();
	std::vector<float> BiomeHeightmap{};
	OutGeneratedHeightmap.Reserve(TotalHeightmapVoxels);
	BiomeHeightmap.reserve(TotalHeightmapVoxels);
	const FVector2D NoiseStartPoint((NeededHeightmapLocation / FVector2D(VoxelSize)) - 1);
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateHeightmap::FastNoise2NoiseGen);
		BiomeNoiseGenerator->GenUniformGrid2D(BiomeHeightmap.data(), NoiseStartPoint.X, NoiseStartPoint.Y, HeightmapVoxels1D, HeightmapVoxels1D, TerrainNoiseScale * BiomeNoiseScale, Seed);
	}

	float HighestVoxel{ FLT_MIN };
	float LowestVoxel{ FLT_MAX };

	int32 LowerIndex{};
	int32 UpperIndex{};

	TArray<TPair<int32, float>> BiomeIndexPercentPair;
	BiomeIndexPercentPair.Reserve(TotalHeightmapVoxels);
	// === First we calculate which biomes are present at each NoiseIndex ===
	const TArray<float> BiomeValues{ -0.666666667, -0.333333333, 0.0, 0.333333333, 0.666666667 };
	for (int32 NoiseIndex{}; NoiseIndex < TotalHeightmapVoxels; NoiseIndex++)
	{
		LowerIndex = 1;
		UpperIndex = 0;

		const float& BiomeNoisePoint = BiomeHeightmap[NoiseIndex];
		for (int32 BiomeIndex{}; BiomeIndex < BiomeValues.Num(); ++BiomeIndex)
		{
			float BiomeValue = BiomeValues[BiomeIndex];
			if (BiomeNoisePoint == BiomeValue)
			{
				LowerIndex = BiomeIndex;
				UpperIndex = BiomeIndex;
				BiomeIndexPercentPair.Add(TPair<int32, float>(BiomeIndex, 1.0f));
				break; // No need to continue if BiomeNoisePoint matches exactly
			}
			else if (BiomeNoisePoint > BiomeValue)
			{
				LowerIndex = BiomeIndex;
			}
			else if (BiomeNoisePoint < BiomeValue)
			{
				UpperIndex = BiomeIndex;
				break; // No need to continue further once UpperIndex is found
			}
		}

		if (LowerIndex != UpperIndex)
		{
			float LowerPercentage = (BiomeNoisePoint - BiomeValues[UpperIndex]) / (BiomeValues[LowerIndex] - BiomeValues[UpperIndex]);
			float UpperPercentage = 1.0f - LowerPercentage;

			BiomeIndexPercentPair.Add(TPair<int32, float>(LowerIndex, LowerPercentage));
			BiomeIndexPercentPair.Add(TPair<int32, float>(UpperIndex, UpperPercentage));
		}
	}

	/// === Next we use that BiomeIndexPercentPair to determine what Noise should be generated for this Point
	int32 PositionIndex{};
	FVector2D NoiseLocation{};
	bool bIsFirstPoint{ true };
	bool bHasAnotherPoint{ false };

	// BiomeIndexPercentPair Contains either 1 or 2 elements for each noise Point in this chunk
	for (const TPair<int32, float>& BiomePoint : BiomeIndexPercentPair)
	{
		bool bPointBelongsToAdjacentCell{ false };

		if (bIsFirstPoint) // Only calculate the postion once per PositionIndex change
		{
			int32 LocationX = PositionIndex % HeightmapVoxels1D;
			int32 LocationY = PositionIndex / HeightmapVoxels1D;
			NoiseLocation = (NoiseStartPoint + FVector2D(LocationX, LocationY)) * TerrainNoiseScale;

			// Check if the location is on the border
			if (LocationX <= 0 || LocationY <= 0 || LocationX >= HeightmapVoxels1D - 1 || LocationY >= HeightmapVoxels1D - 1)
				bPointBelongsToAdjacentCell = true;

			// If the Point isn't 100% one PositionIndex then we know there is another Point
			bHasAnotherPoint = (BiomePoint.Value != 1.0);
		}
		else
			bHasAnotherPoint = false;

		float NoisePoint{};

		if (!bIsRunning)
			return;

		switch (BiomePoint.Key)
		{
		default:
			NoisePoint = PlainsNoiseGenerator->GenSingle2D(NoiseLocation.X, NoiseLocation.Y, Seed);
			break;
		case 0: // Flat
			NoisePoint = 0.0;
			break;
		case 1: // Forest
			NoisePoint = ForestNoiseGenerator->GenSingle2D(NoiseLocation.X, NoiseLocation.Y, Seed) * 0.4;
			break;
		case 2: // Grassy Plains
			NoisePoint = PlainsNoiseGenerator->GenSingle2D(NoiseLocation.X, NoiseLocation.Y, Seed) * 0.7;
			break;
		case 3: // Rough Hills
			NoisePoint = HillsNoiseGenerator->GenSingle2D(NoiseLocation.X, NoiseLocation.Y, Seed) * 1.4;
			break;
		case 4: // Mountains
			NoisePoint = MountainsNoiseGenerator->GenSingle2D(NoiseLocation.X, NoiseLocation.Y, Seed) * 6.3;
			break;
		}

		// Scale the noise by the percentage of it's biome found at this location
		NoisePoint *= BiomePoint.Value;
		int32 VoxelHeight = (NoisePoint * VoxelSize) * TerrainHeightMultiplier;
		if (bIsFirstPoint)
		{
			VoxelHeight -= VoxelCount / 2.0;
			OutGeneratedHeightmap.Add(VoxelHeight);
		}
		else // Add the second Point to the first
			VoxelHeight = OutGeneratedHeightmap[PositionIndex] += VoxelHeight;

		if (!bHasAnotherPoint)
		{
			//if (!bPointBelongsToAdjacentCell)
			//{
			VoxelHeight *= VoxelSize;
			VoxelHeight -= VoxelSize;
			VoxelHeight -= FMath::GridSnap(ChunkSize / 2, VoxelSize);

			// Calculate the lowest and highest voxels so we know which vertical chunks to spawn
			if (VoxelHeight > HighestVoxel)
				HighestVoxel = VoxelHeight;
			if (VoxelHeight < LowestVoxel)
				LowestVoxel = VoxelHeight;
			//}

			PositionIndex++; // Move on to next position
			bIsFirstPoint = true;
		}
		else // Check this Point again
			bIsFirstPoint = false;
	}

	int32 HighestChunkIndex = FMath::GridSnap(HighestVoxel, ChunkSize) / ChunkSize;
	int32 LowestChunkIndex = FMath::GridSnap(LowestVoxel, ChunkSize) / ChunkSize;
	for (int32 ChunkIndex{ LowestChunkIndex }; ChunkIndex <= HighestChunkIndex; ChunkIndex++)
		OutNeededChunksVerticalIndices.Add(ChunkIndex);
}

void FChunkThread::CombineChunkZIndices(const FVector2D& HeightmapLocation, TArray<int32>& TerrainZIndices)
{
	FScopeLock Lock(&ChunkZMutex);
	FIntPoint Cell2D{ AChunkManager::Get2DCellFromChunkLocation2D(HeightmapLocation, ChunkSize) };
	TArray<int32>& CombinedIndices{ ChunkZIndicesBy2DCell.FindOrAdd(Cell2D) };
	for (int32 TerrainZIndex : TerrainZIndices)
		if (!CombinedIndices.Contains(TerrainZIndex))
			CombinedIndices.Add(TerrainZIndex);

	TArray<int32>* ModifiedChunkAdditionalIndices{ ModifiedAdditionalChunkZIndicesBy2DCell.Find(Cell2D) };
	if (ModifiedChunkAdditionalIndices) // Add every Z Index we don't already have
		for (int32 AdditionalIndex : *ModifiedChunkAdditionalIndices)
			if (!CombinedIndices.Contains(AdditionalIndex))
				CombinedIndices.Add(AdditionalIndex);

	TerrainZIndices = CombinedIndices;
}

bool FChunkThread::AddConstructionData(TArray<TSharedPtr<FChunkConstructionData>>& OutChunkConstructionDataArray, const FVector2D& ChunkLocation2D, const TArray<int32>& VerticalChunkIndices)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::AddConstructionData);

	int32 LowestIndex{ INT32_MAX };
	int32 HighestIndex{ INT32_MIN };

	bool bNeedsCollision = DoesLocationNeedCollision(ChunkLocation2D, PlayerLocations, CollisionGenerationRadius);

	for (int32 ChunkIndex : VerticalChunkIndices)
	{
		float ChunkHeight = ChunkIndex * ChunkSize;
		FVector ChunkLocation{ ChunkLocation2D.X, ChunkLocation2D.Y, ChunkHeight };

		bool bNeedsToUnhideChunk{};
		FIntVector ChunkCell{ AChunkManager::GetCellFromChunkLocation(ChunkLocation, ChunkSize) };
		OutChunkConstructionDataArray.Emplace(MakeShared<FChunkConstructionData>(ChunkLocation, ChunkCell, bNeedsCollision));
	}

	return !OutChunkConstructionDataArray.IsEmpty();
}

void FChunkThread::GenerateVoxelsForChunks(TArray<TSharedPtr<FChunkConstructionData>>& OutChunksConstructionData, const TArray<int16>& Heightmap)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateVoxelsForChunks);

	TArray<int32> IndicesToRemove;

	for (int32 Index{}; Index < OutChunksConstructionData.Num(); ++Index)
	{
		TSharedPtr<FChunkConstructionData>& ConstructionData = OutChunksConstructionData[Index];

		GenerateChunkVoxels(ConstructionData->Voxels, Heightmap, ConstructionData->ChunkLocation);

		// ModifiedVoxelsByCell could exist from changes we made this session, changes from a loaded save, or they could be Received from the server if other players have modified this chunk
		ApplyModifiedVoxelsToChunk(ConstructionData->Voxels, ConstructionData->Cell);
	}

	// Remove elements in reverse order to avoid shifting indices
	for (int32 Index = IndicesToRemove.Num() - 1; Index >= 0; --Index)
		OutChunksConstructionData.RemoveAt(IndicesToRemove[Index]);
}

bool FChunkThread::GenerateChunkVoxels(TArray<uint8>& Voxels, const TArray<int16>& Heightmap, const FVector& ChunkLocation)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateChunkVoxels);

	Voxels.Empty(TotalChunkVoxels);

	bool bIsBuried{ false }; // Used to determine if the ChunkMesh would be empty
	bool bIsAllAir{ false }; // Used to determine if the ChunkMesh would be empty

	const uint8 GrassBlockIndex = 1;
	const uint8 DirtBlockIndex = 2;
	const uint8 StoneBlockIndex = 4;
	const uint8 DirtDepth = 2;

	for (int32 Y{ -1 }; Y < VoxelCount + 1; Y++)
	{
		for (int32 X{ -1 }; X < VoxelCount + 1; X++)
		{
			int32 TerrainNoiseSample{ 25 };
			int32 SampleIndex{ (X + 1) * (VoxelCount + 2) + (Y + 1) };

			if (Heightmap.IsValidIndex(SampleIndex))
				TerrainNoiseSample = Heightmap[SampleIndex];

			for (int32 Z{ -1 }; Z < VoxelCount + 1; Z++)
			{
				int32 VoxelZ{ Z + FMath::RoundToInt32((ChunkLocation.Z / VoxelSize)) };

				if (VoxelZ == TerrainNoiseSample - 1)
				{
					bIsAllAir = false;

					Voxels.Add(GrassBlockIndex);
				}
				else if (VoxelZ < TerrainNoiseSample - 1)
				{
					if (VoxelZ < TerrainNoiseSample - 1 - DirtDepth)
					{
						bIsAllAir = false;

						Voxels.Add(StoneBlockIndex);
					}
					else
					{
						bIsAllAir = false;

						Voxels.Add(DirtBlockIndex);
					}
				}
				else if (VoxelZ >= TerrainNoiseSample)
				{
					bIsBuried = false;

					Voxels.Add(0);
				}
			}
		}
	}

	if (Voxels.IsEmpty() || bIsBuried || bIsAllAir)
		return false;

	return true;
}

void FChunkThread::ApplyModifiedVoxelsToChunk(TArray<uint8>& Voxels, FIntVector ChunkCell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::ApplyModifiedVoxelsToChunk);

	FScopeLock Lock(&ChunkManagerRef->ModifiedVoxelsMutex);
	FIntPoint Region{ GetRegionByLocation(FVector2D(FVector(ChunkCell) * ChunkSize)) };
	TMap<FIntVector, TArray<uint8>>* ModifiedVoxelsByCell{ ChunkManagerRef->ModifiedVoxelsByCellByRegion.Find(Region) };

	if (!ModifiedVoxelsByCell)
		return;
	TArray<uint8>* ModifiedVoxels{ ModifiedVoxelsByCell->Find(ChunkCell) };
	if (!ModifiedVoxels || ModifiedVoxels->IsEmpty())
		return;

	if (ModifiedVoxels->Num() > TotalChunkVoxels || ModifiedVoxels->Num() < TotalChunkVoxels)
	{
		UE_LOG(LogTemp, Error, TEXT("ModifiedVoxels at %s has an invalid number of elements %i. Should be %i"), *ChunkCell.ToString(), ModifiedVoxels->Num(), TotalChunkVoxels);
		return;
	}

	for (int32 VoxelIndex{}; VoxelIndex < TotalChunkVoxels; VoxelIndex++) // We loop through and overwrite every voxel that was moddified. UINT8_MAX indicates the voxel has not been modified
	{
		if (!ModifiedVoxels->IsValidIndex(VoxelIndex))
		{
			UE_LOG(LogTemp, Warning, TEXT("Failed to apply modified voxels to ChunkCell %s"), *ChunkCell.ToString());
			return;
		}
		if (!Voxels.IsValidIndex(VoxelIndex))
		{
			UE_LOG(LogTemp, Warning, TEXT("Voxels[%i] is an invalid index! Failed to apply modified voxels to ChunkCell %s"), VoxelIndex, *ChunkCell.ToString());
			return;
		}
		if ((*ModifiedVoxels)[VoxelIndex] == UINT8_MAX)
			continue;

		Voxels[VoxelIndex] = (*ModifiedVoxels)[VoxelIndex]; // Set the Chunk's voxel to match the player-modified one
	}
}

void FChunkThread::GenerateMeshDataForChunks(TArray<TSharedPtr<FChunkConstructionData>>& OutConstructionChunks)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateMeshDataForChunks);

	for (TSharedPtr<FChunkConstructionData>& NeededChunk : OutConstructionChunks)
	{
		GenerateChunkMeshData(
			NeededChunk->MeshData,
			NeededChunk->Voxels,
			NeededChunk->Cell,
			NeededChunk->bShouldGenerateCollision);
	}
}

// Can be called from any thread
void FChunkThread::GenerateChunkMeshData(FChunkMeshData& OutChunkMeshData, TArray<uint8>& Voxels, const FIntVector ChunkCell, const bool bShouldGenerateCollisionAtChunkSpawn)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTerrainChunkThread::GenerateChunkMeshData);

	OutChunkMeshData.CollisionType = ECR_Block;
	OutChunkMeshData.ChunkCell = ChunkCell;
	OutChunkMeshData.bShouldGenCollision = bShouldGenerateCollisionAtChunkSpawn;

	if (Voxels.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Tried to generate a chunk with no voxels!"));
		return;
	}

	RealtimeMesh::TRealtimeMeshStreamBuilder<FVector3f> PositionBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Position, RealtimeMesh::GetRealtimeMeshBufferLayout<FVector3f>()));
	RealtimeMesh::TRealtimeMeshStreamBuilder<RealtimeMesh::FRealtimeMeshTangentsHighPrecision, RealtimeMesh::FRealtimeMeshTangentsNormalPrecision> TangentBuilder(
		OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Tangents, RealtimeMesh::GetRealtimeMeshBufferLayout<RealtimeMesh::FRealtimeMeshTangentsNormalPrecision>()));
	RealtimeMesh::TRealtimeMeshStreamBuilder<FVector2f, FVector2DHalf> TexCoordsBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::TexCoords, RealtimeMesh::GetRealtimeMeshBufferLayout<FVector2DHalf>()));
	RealtimeMesh::TRealtimeMeshStreamBuilder<FColor> ColorBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Color, RealtimeMesh::GetRealtimeMeshBufferLayout<FColor>()));
	RealtimeMesh::TRealtimeMeshStreamBuilder<uint32, uint16> PolygroupsBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::PolyGroups, RealtimeMesh::GetRealtimeMeshBufferLayout<uint16>()));
	TArray<TArray<FVector>> TrianglesByVoxelValue{};

	int32 NumberOfTris{};
	FVector3f ChunkMeshOffset{ -ChunkSize / 2 };
	TSet<uint8> VoxelValuesInThisChunk{};

	int32 VoxelIndex{};
	int32 AdjacentVoxelIndex{};
	FVector3f VoxelLocation{ ChunkMeshOffset };
	FIntVector XYZ{};
	// Loop through all voxels in the chunk except the border voxels technically belonging to adjacent chunks
	// The chunk is the same size in each direction. This knowledge can help us take some shortcuts
	for (int32 X{}; X < VoxelCount; X++)
	{
		XYZ.X = X;
		VoxelLocation.X = ChunkMeshOffset.X + (X * VoxelSize);
		for (int32 Y{}; Y < VoxelCount; Y++)
		{
			XYZ.Y = Y;
			VoxelLocation.Y = ChunkMeshOffset.Y + (Y * VoxelSize);
			for (int32 Z{}; Z < VoxelCount; Z++)
			{
				XYZ.Z = Z;
				GetVoxelIndex(VoxelIndex, X, Y, Z);
				VoxelLocation.Z = ChunkMeshOffset.Z + (Z * VoxelSize);

				if (!Voxels.IsValidIndex(VoxelIndex))
					continue;
				const uint8& VoxelValue{ Voxels[VoxelIndex] };
				if (VoxelDefinitions[VoxelValue].bIsAir) // Skip the voxel if it is air
					continue;

				FSetElementId PolyGroupID = VoxelValuesInThisChunk.FindId(VoxelValue);

				for (int32 FaceIndex{}; FaceIndex < 6; FaceIndex++)
				{
					GetVoxelIndex(AdjacentVoxelIndex, XYZ + FaceIntDirections[FaceIndex]);
					if (!(Voxels.IsValidIndex(AdjacentVoxelIndex)))
						continue;
					const uint8& AdjacentVoxelValue = Voxels[AdjacentVoxelIndex];
					if (AdjacentVoxelValue > 0) // If this voxel is solid, we assume this face is buried
						continue;

					if (!PolyGroupID.IsValidId())
					{
						VoxelValuesInThisChunk.Add(VoxelValue);
						PolyGroupID = VoxelValuesInThisChunk.FindId(VoxelValue);
						TrianglesByVoxelValue.Add(TArray<FVector>());
					}

					TArray<int32> Verts{};
					Verts.Reserve(4);
					for (int32 VertIndex{}; VertIndex < 4; VertIndex++)
					{
						FVector3f Tangent{};
						FVector Normal{ FaceDirections[FaceIndex] };
						CalculateTangent(Normal);
						Verts.Add(PositionBuilder.Add(VoxelLocation + (CubeVertLocations[FaceIndex][VertIndex] * FVector3f(VoxelSize))));
						TangentBuilder.Add(RealtimeMesh::FRealtimeMeshTangentsHighPrecision(FVector3f(Normal), Tangent));
						ColorBuilder.Add(FColor(FaceIndex, 0, 0, 0));
						TexCoordsBuilder.Add(CalculateUV(FaceIndex, VertIndex));
					}

					TrianglesByVoxelValue[PolyGroupID.AsInteger()].Add(FVector(Verts[0], Verts[3], Verts[2]));
					TrianglesByVoxelValue[PolyGroupID.AsInteger()].Add(FVector(Verts[2], Verts[1], Verts[0]));
					NumberOfTris += 2;
				}
			}
		}
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::GenerateChunkMeshData::CombineStreams);
		RealtimeMesh::TRealtimeMeshStreamBuilder<RealtimeMesh::TIndex3<uint32>, RealtimeMesh::TIndex3<uint16>> TrianglesBuilder(OutChunkMeshData.ChunkStreamSet.AddStream(RealtimeMesh::FRealtimeMeshStreams::Triangles, RealtimeMesh::GetRealtimeMeshBufferLayout<RealtimeMesh::TIndex3<uint16>>()));
		TrianglesBuilder.Reserve(NumberOfTris);
		for (int32 GroupIndex{}; GroupIndex < TrianglesByVoxelValue.Num(); GroupIndex++)
		{
			int32 TrisInThisSection{ TrianglesByVoxelValue[GroupIndex].Num() };
			for (int32 TriangleIndex{}; TriangleIndex < TrisInThisSection; TriangleIndex++)
			{
				PolygroupsBuilder.Add(GroupIndex);
				TrianglesBuilder.Add(RealtimeMesh::TIndex3<uint32>(TrianglesByVoxelValue[GroupIndex][TriangleIndex].X, TrianglesByVoxelValue[GroupIndex][TriangleIndex].Y, TrianglesByVoxelValue[GroupIndex][TriangleIndex].Z));
			}
		}

		for (uint8 VoxelValue : VoxelValuesInThisChunk)
			OutChunkMeshData.VoxelSections.Add(VoxelValue);
	}
	OutChunkMeshData.bIsMeshEmpty = VoxelValuesInThisChunk.IsEmpty();
}

FVector2f FChunkThread::CalculateUV(const int32& FaceIndex, const int32& VertIndex)
{
	FVector2f UV;

	switch (FaceIndex)
	{
	case 0: // Up
		UV = FVector2f(CubeVertLocations[FaceIndex][VertIndex].X, CubeVertLocations[FaceIndex][VertIndex].Y);
		break;
	case 1: // Down
		UV = FVector2f(CubeVertLocations[FaceIndex][VertIndex].X, -CubeVertLocations[FaceIndex][VertIndex].Y);
		break;
	case 2: // Right
		UV = FVector2f(CubeVertLocations[FaceIndex][VertIndex].X, CubeVertLocations[FaceIndex][VertIndex].Z);
		break;
	case 3: // Left
		UV = FVector2f(-CubeVertLocations[FaceIndex][VertIndex].X, CubeVertLocations[FaceIndex][VertIndex].Z);
		break;
	case 4: // Forward
		UV = FVector2f(CubeVertLocations[FaceIndex][VertIndex].Y, CubeVertLocations[FaceIndex][VertIndex].Z);
		break;
	case 5: // Backward
		UV = FVector2f(-CubeVertLocations[FaceIndex][VertIndex].Y, CubeVertLocations[FaceIndex][VertIndex].Z);
		break;
	}

	UV -= FVector2f(0.5f);
	UV *= -1.f;

	return UV;
}

bool FChunkThread::DoesLocationNeedCollision(FVector2D ChunkLocation2D, const TArray<FVector2D>& TrackedLocationsRef, int32 ChunkGenRadius)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::DoesLocationNeedCollision);

	for (int32 LocationIndex{}; LocationIndex < TrackedLocationsRef.Num(); LocationIndex++)
	{
		FVector2D TrackedLocation{ TrackedLocationsRef[LocationIndex] };
		if (IsHeightmapInRange(ChunkLocation2D, TrackedLocation, ChunkGenRadius))
			return true;
	}

	return false;
}

void FChunkThread::CompressVoxelData(TArray<TSharedPtr<FChunkConstructionData>>& ChunkConstructionDataArray)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::CompressVoxelData);

	for (TSharedPtr<FChunkConstructionData>& ConstrctionChunk : ChunkConstructionDataArray)
	{
		ConstrctionChunk->bAreVoxelsCompressed = true;
		RunLengthEncode(ConstrctionChunk->Voxels, ConstrctionChunk->Cell);
	}
}

void FChunkThread::AsyncSpawnChunks(TArray<TSharedPtr<FChunkConstructionData>>& ChunkConstructionDataArray, const FVector2D& HeightmapLocation, const TArray<int32>& TerrainZIndices)
{
	int32 ChunkCount{};
	for (TSharedPtr<FChunkConstructionData>& ChunkConstructionData : ChunkConstructionDataArray)
	{
		// Throttle this so we don't have a massive number of chunks needed to be generated on the game thread at once (especially useful when we have large stacks of vertical chunks)
		FPlatformProcess::Sleep(FMath::Min(ThreadWorkingSleepTime * ChunkCount++, 0.05));
		if (!bIsRunning || !WorldRef) return;

		int32 TempChunkRadius{ TempChunkGenRadius };
		int32 TempCollisionRadius{ TempCollisionGenRadius };
		// This function runs on the GameThread, we can fire and forget it
		AsyncTask(ENamedThreads::GameThread, [ChunkConstructionData, TerrainZIndices, TempChunkRadius, TempCollisionRadius, this]()
			{
				// Pass the moved data to the function
				SpawnChunkFromConstructionData(ChunkConstructionData, TempChunkRadius, TempCollisionRadius);
			});
	}
	ChunkConstructionDataArray.Empty();
}

bool FChunkThread::ShouldSpawnHidden(FVector2D ChunkLocation, int32 ChunkGenRadius)
{
	return ChunkManagerRef->GetNetMode() == ENetMode::NM_ListenServer && !IsHeightmapInRange(ChunkLocation, PlayerLocations[0], ChunkGenRadius);
}

// This function runs on the game thread. Called by AsyncTaskGraph in ChunkThread's Run()
void FChunkThread::SpawnChunkFromConstructionData(TSharedPtr<FChunkConstructionData> OutNeededChunk, int32 ChunkGenRadius, int32 CollisionGenRadius, bool bShouldGenerateMesh)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::SpawnChunkFromConstructionData);

	if (!WorldRef || WorldRef->bIsTearingDown)
		return;

	if (!ChunkManagerRef || !IsValid(ChunkManagerRef))
		return;

	if (!OutNeededChunk || !OutNeededChunk.IsValid())
		return;

	FIntPoint Cell2D{ OutNeededChunk->Cell.X, OutNeededChunk->Cell.Y };
	FIntVector ChunkCell{ OutNeededChunk->Cell };
	AChunkActor* Chunk{};
	
	if(ChunkManagerRef->ChunksToDestroyQueue.Contains(ChunkCell))
		ChunkManagerRef->ChunksToDestroyQueue.Remove(ChunkCell);
	
	if (ChunkManagerRef->ChunksByCell.Contains(ChunkCell))
		Chunk = *ChunkManagerRef->ChunksByCell.Find(ChunkCell);

	bool bClientHadChunkName{ false };
	bool bIsNewChunk{ Chunk == nullptr };
	if (bIsNewChunk)
	{
		// Set actor parameters
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParameters.bDeferConstruction = true;
		SpawnParameters.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		SpawnParameters.Owner = ChunkManagerRef;
		FString ChunkName{};

		Chunk = WorldRef->SpawnActor<AChunkActor>(OutNeededChunk->ChunkLocation, FRotator::ZeroRotator, SpawnParameters);
	}
	if (!Chunk)
	{
		DrawDebugPoint(WorldRef, OutNeededChunk->ChunkLocation, 15, FColor(200, 25, 55), false, 5.f);
		DrawDebugString(WorldRef, OutNeededChunk->ChunkLocation + FVector(0, 0, 50), TEXT("Failed to find or spawn Chunk Actor"), nullptr, FColor(200, 25, 55), 5.f);

		return;
	}

	// Set up the chunk
	if (bIsNewChunk)
	{
		Chunk->bReplicates = false;
		Chunk->bAlwaysRelevant = true;
		Chunk->bNetLoadOnClient = false;
		FString ChunkCellString{ ChunkCell.ToString() };
		Chunk->Tags.Add(*ChunkCellString);
		Chunk->ChunkCell = ChunkCell;
		Chunk->VoxelCount = VoxelCount;
		Chunk->VoxelSize = VoxelSize;
		Chunk->ChunkSize = ChunkSize;
		Chunk->Voxels = MoveTemp(OutNeededChunk->Voxels);
		Chunk->bAreVoxelsCompressed = OutNeededChunk->bAreVoxelsCompressed;
	}
	if (ChunkManagerRef->GetNetMode() == ENetMode::NM_Client)
	{
		if (ChunkManagerRef->GetIsReplicated() == false)
			UE_LOG(LogTemp, Error, TEXT("ChunkManagerRef was not replicated!"));

		if (ChunkManagerRef->ChunkSpawnCountByCell.Contains(ChunkCell))
		{
			bClientHadChunkName = true;
			int32 ChunkSpawnCount{ ChunkManagerRef->ChunkSpawnCountByCell.FindRef(ChunkCell) };

			ChunkManagerRef->SetChunkName(Chunk, ChunkCell, ChunkSpawnCount);
		}
	}

	ChunkManagerRef->ChunkZIndicesBy2DCell.FindOrAdd(FIntPoint(ChunkCell.X, ChunkCell.Y)).Add(ChunkCell.Z);

	if (bClientHadChunkName)
		Chunk->bIsSafeToDestroy = false;
	else
		Chunk->bIsSafeToDestroy = true;

	if (bIsNewChunk) // Finish spawning the actor
		Chunk->FinishSpawning(FTransform(OutNeededChunk->ChunkLocation));

	if (!Chunk)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to spawn Chunk Actor at %s"), *OutNeededChunk->ChunkLocation.ToString());
		return;
	}

	if ((ChunkManagerRef->GetNetMode() == ENetMode::NM_DedicatedServer || ChunkManagerRef->GetNetMode() == NM_ListenServer) && IsNeededHeightmapLocation(FVector2D(OutNeededChunk->ChunkLocation), PlayerLocations, CollisionGenRadius, CollisionGenRadius)) // We don't want to modify this data if we are on a client, as the client populates this data from the server
		EnableReplicationForChunk(Chunk);

	// This indicates we generated this chunk for a player other than the host player, so we can hide it
	if (ShouldSpawnHidden(FVector2D(Chunk->GetActorLocation()), ChunkGenRadius + ChunkDeletionBuffer))
		ChunkManagerRef->HideChunk(Chunk);

	if (!ChunkManagerRef->VoxelTypesDatabase)
	{
		DrawDebugPoint(WorldRef, OutNeededChunk->ChunkLocation, 15, FColor(255, 25, 75), false, 5.f);
		DrawDebugString(WorldRef, OutNeededChunk->ChunkLocation + FVector(0, 0, 50), TEXT("VoxelTypesDatabase was nullptr!"), nullptr, FColor(255, 25, 75), 5.f);

		return;
	}

	if (bIsNewChunk)
		ChunkManagerRef->ChunksByCell.Add(OutNeededChunk->Cell, Chunk);
	if (!bShouldGenerateMesh)
		return;

	TArray<UMaterial*> VoxelMaterials{};
	ChunkManagerRef->GetMaterialsForChunkData(OutNeededChunk->MeshData.VoxelSections, VoxelMaterials);
	Chunk->GenerateChunkMesh(OutNeededChunk->MeshData, VoxelMaterials);
}

void FChunkThread::SaveUnsavedRegions(bool bSaveAsync)
{
	TArray<FIntPoint> RegionsToSave;
	{
		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		RegionsToSave = ChunkManagerRef->RegionsChangedSinceLastSave;
	}
	for (FIntPoint Region : RegionsToSave) // We only run this async if we aren't closing out the thread. If we are we need to make sure we save the data before we close, so we can't do it Async
		AsyncSaveVoxelsForRegion(Region, WorldSaveName, false, bSaveAsync);
}

void FChunkThread::AsyncSaveVoxelsForRegion(FIntPoint Region, FString SaveName, bool bRemoveDataWhenDone, bool bRunAsync)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::AsyncSaveVoxelsForRegion);

	if (IsInGameThread() && bRunAsync)
	{
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, Region, SaveName, bRemoveDataWhenDone]()
			{
				SaveVoxelsForRegion(SaveName, Region, bRemoveDataWhenDone);
			});

	}
	else
		SaveVoxelsForRegion(SaveName, Region, bRemoveDataWhenDone);
}

void FChunkThread::SaveVoxelsForRegion(const FString& SaveName, const FIntPoint& Region, bool bRemoveDataWhenDone)
{
	if (SaveName.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid WorldSaveName: %s"), *SaveName);

		return;
	}

	bool bRegionWasPendingLoad{};
	{
		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		bRegionWasPendingLoad = ChunkManagerRef->RegionsPendingLoad.Contains(Region);
	}

	if (bRegionWasPendingLoad)
		LoadVoxelsForRegion(Region, SaveName);

	bool bRegionHadModifiedVoxels{};
	{
		FScopeLock Lock(&ChunkManagerRef->ModifiedVoxelsMutex);
		bRegionHadModifiedVoxels = ChunkManagerRef->ModifiedVoxelsByCellByRegion.Contains(Region);
	}

	if (!bRegionHadModifiedVoxels)
	{
		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		ChunkManagerRef->RegionsPendingSave.Remove(Region);
		UE_LOG(LogTemp, Warning, TEXT("No modified voxels to save for region %s"), *Region.ToString());


		return;
	}

	FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), SaveFolderName, SaveName, Region.ToString() + "Voxels.dat");

	TArray<FVoxelSaveData> VoxelDataArray{};

	{
		FScopeLock Lock(&ChunkManagerRef->ModifiedVoxelsMutex);
		TMap<FIntVector, TArray<uint8>> ModifiedVoxelsByCell{ ChunkManagerRef->ModifiedVoxelsByCellByRegion.FindRef(Region) };
		for (TPair<FIntVector, TArray<uint8>>& CellVoxelPair : ModifiedVoxelsByCell)
		{
			const FIntVector& Cell = CellVoxelPair.Key;
			TArray<uint8>& Voxels = CellVoxelPair.Value;
			RunLengthEncode(Voxels, Cell);
			FVoxelSaveData SaveData(Cell, Voxels);
			VoxelDataArray.Add(SaveData);
		}
		if (bRemoveDataWhenDone)
			ChunkManagerRef->ModifiedVoxelsByCellByRegion.Remove(Region);
	}

	TArray<uint8> SerializedData{};
	FMemoryWriter MemoryWriter(SerializedData, true);
	MemoryWriter << VoxelDataArray;

	FFileHelper::SaveArrayToFile(SerializedData, *SavePath);

	{
		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		ChunkManagerRef->RegionsPendingSave.Remove(Region);
	}
}

// Do not call from game thread
void FChunkThread::LoadVoxelsForRegion(FIntPoint Region, FString SaveName)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::LoadVoxelsForRegion);

	{
		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		ChunkManagerRef->RegionsPendingLoad.Remove(Region);
	}

	if (SaveName.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid WorldSaveName: %s"), *SaveName);

		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		ChunkManagerRef->RegionsAlreadyLoaded.Add(Region);
		ChunkManagerRef->RegionsPendingLoad.Remove(Region);

		return;
	}

	FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), SaveFolderName, SaveName, Region.ToString() + "Voxels.dat");

	if (!FPaths::FileExists(SavePath))
	{
		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		ChunkManagerRef->RegionsAlreadyLoaded.Add(Region);
		ChunkManagerRef->RegionsPendingLoad.Remove(Region);

		return;
	}

	// Read the data from the file
	TArray<uint8> SerializedData{};
	if (!FFileHelper::LoadFileToArray(SerializedData, *SavePath))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load chunk data from file: %s"), *SavePath);

		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		ChunkManagerRef->RegionsAlreadyLoaded.Add(Region);
		ChunkManagerRef->RegionsPendingLoad.Remove(Region);

		return;
	}

	FMemoryReader MemoryReader(SerializedData, true);
	TArray<FVoxelSaveData> VoxelDataArray{};
	MemoryReader << VoxelDataArray;

	if (VoxelDataArray.IsEmpty())
	{
		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		ChunkManagerRef->RegionsAlreadyLoaded.Add(Region);
		ChunkManagerRef->RegionsPendingLoad.Remove(Region);

		return;
	}

	TMap<FIntVector, TArray<uint8>> ModifiedVoxelsByCell{};
	for (FVoxelSaveData& VoxelData : VoxelDataArray)
	{
		RunLengthDecode(VoxelData.CompressedVoxelData, VoxelData.ChunkCell);
		ModifiedVoxelsByCell.Add(VoxelData.ChunkCell, VoxelData.CompressedVoxelData);
		FVector2D HeightmapLocation{ FVector2D(FVector(VoxelData.ChunkCell * ChunkSize)) };
		FScopeLock Lock(&ChunkZMutex);
		ModifiedAdditionalChunkZIndicesBy2DCell.FindOrAdd(FIntPoint(VoxelData.ChunkCell.X, VoxelData.ChunkCell.Y)).Add(VoxelData.ChunkCell.Z);
	}

	{
		FScopeLock Lock(&ChunkManagerRef->ModifiedVoxelsMutex);
		ChunkManagerRef->ModifiedVoxelsByCellByRegion.Add(Region, ModifiedVoxelsByCell);
	}

	{
		FScopeLock Lock(&ChunkManagerRef->RegionMutex);
		ChunkManagerRef->RegionsAlreadyLoaded.Add(Region);
		ChunkManagerRef->RegionsPendingLoad.Remove(Region);
	}
}

void FChunkThread::GetRegionsToSave(TArray<FIntPoint>& RegionsToSave)
{
	FScopeLock Lock(&ChunkManagerRef->RegionMutex);
	while (!ChunkManagerRef->RegionsPendingSave.IsEmpty() && bIsRunning)
	{
		if(!ChunkManagerRef->RegionsPendingSave.IsValidIndex(0) || !bIsRunning)
			break;
		FIntPoint RegionPendingSave{ ChunkManagerRef->RegionsPendingSave[0] };
		ChunkManagerRef->RegionsPendingSave.RemoveAt(0);
		RegionsToSave.Add(RegionPendingSave);
	}
}

void FChunkThread::GetRegionsToLoad(TArray<FIntPoint>& RegionsToLoad)
{
	FScopeLock Lock(&ChunkManagerRef->RegionMutex);
	if (WorldRef->GetNetMode() != NM_Client)
	{
		while (!ChunkManagerRef->RegionsPendingLoad.IsEmpty() && bIsRunning)
		{
			if (!ChunkManagerRef->RegionsPendingLoad.IsValidIndex(0) || !bIsRunning)
				break;

			FIntPoint RegionPendingLoad{ ChunkManagerRef->RegionsPendingLoad[0] };
			ChunkManagerRef->RegionsPendingLoad.RemoveAt(0);
			RegionsToLoad.Add(RegionPendingLoad);
		}
	}
}

void FChunkThread::SetChunkGenRadius(int32 Radius)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::SetChunkGenRadius);
	FScopeLock Lock(&ChunkGenMutex);

	if (Radius < CollisionGenerationRadius)
	{
		Radius = CollisionGenerationRadius;
		UE_LOG(LogTemp, Warning, TEXT("Chunk Generation Radius cannot be below Collision Generation Radius"));
	}

	bWasRangeChanged = true;
	ChunkGenerationRadius = Radius;     
	LastRingCount = 0;
	for (int32 TrackedActorIndex{}; TrackedActorIndex < PlayerLocations.Num(); TrackedActorIndex++)
	{
		if (!TrackedChunkRingDistance.IsValidIndex(TrackedActorIndex))
			continue;
		TrackedChunkRingDistance[TrackedActorIndex] = FMath::Min(TrackedChunkRingDistance[TrackedActorIndex], Radius);

		if (!TrackedChunkRingCount.IsValidIndex(TrackedActorIndex))
			continue;

		TrackedChunkRingCount[TrackedActorIndex] = FMath::Min(TrackedChunkRingCount[TrackedActorIndex], ChunkGenerationRadius * 1.4);
	}
}

void FChunkThread::GetVoxelIndex(int32& VoxelIndex, int32& X, int32& Y, int32& Z)
{
	VoxelIndex = (X + 1) * (VoxelCount + 2) * (VoxelCount + 2) + (Y + 1) * (VoxelCount + 2) + (Z + 1);
}

void FChunkThread::GetVoxelIndex(int32& VoxelIndex, const FIntVector XYZ)
{
	VoxelIndex = (XYZ.X + 1) * (VoxelCount + 2) * (VoxelCount + 2) + (XYZ.Y + 1) * (VoxelCount + 2) + (XYZ.Z + 1);
}

FVector FChunkThread::CalculateTangent(const FVector& Normal)
{
	FVector NormalizedNormal{ Normal.GetSafeNormal() };
	FVector ArbitraryVector;
	if (FMath::Abs(NormalizedNormal.X) < KINDA_SMALL_NUMBER && FMath::Abs(NormalizedNormal.Z) < KINDA_SMALL_NUMBER)
		ArbitraryVector = FVector(1, 0, 0);
	else
		ArbitraryVector = FVector(0, 1, 0);

	FVector Tangent{ FVector::CrossProduct(NormalizedNormal, ArbitraryVector) };
	FVector NormalizedTangent{ Tangent.GetSafeNormal() };

	return NormalizedTangent;
}

// Only call from the game thread, Only call from server
bool FChunkThread::EnableReplicationForChunk(AChunkActor* Chunk, bool bShouldDirectlySetbReplicates)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FChunkThread::EnableReplicationForChunk);

	if (!WorldRef)
		return false;

	if (!IsInGameThread())
	{
		UE_LOG(LogTemp, Error, TEXT("EnableChunkReplication was called from a non-game thread!"));
		return false;
	}

	if (ChunkManagerRef->GetNetMode() == NM_Client)
	{
		UE_LOG(LogTemp, Error, TEXT("EnableChunkReplication was called on a client!"));
		return false;
	}

	if (!Chunk || !IsValid(Chunk))
	{
		UE_LOG(LogTemp, Error, TEXT("Chunk was nullptr!"));
		return false;
	}

	if (!ChunkManagerRef)
	{
		UE_LOG(LogTemp, Error, TEXT("ChunkManagerRef was nullptr!"));
		return false;
	}

	FIntVector& ChunkCell{ Chunk->ChunkCell };
	bool bDidChunkSpawnCountExist{ ChunkManagerRef->ChunkSpawnCountByCell.Contains(ChunkCell) };
	int32* ChunkSpawnCount{ &ChunkManagerRef->ChunkSpawnCountByCell.FindOrAdd(ChunkCell, 0) };

	if (Chunk->bReplicates)
	{
		if (!bDidChunkSpawnCountExist)
			UE_LOG(LogTemp, Error, TEXT("Chunk %s was replicated, but no spawn count was found!"), *Chunk->GetName());
		return true;
	}

	if (bDidChunkSpawnCountExist)
		(*ChunkSpawnCount)++;
	FString NewName{ FString(GetDeterministicNameByLocationAndRepCount(ChunkCell, *ChunkSpawnCount)) };

	if (NewName.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to generate a new name for Chunk %s!"), *Chunk->GetName());
		return false;
	}

	if (!Chunk->GetName().Equals(NewName))
	{
		if (!Chunk->Rename(*NewName, ChunkManagerRef, REN_ForceNoResetLoaders))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to rename Chunk %s to %s!"), *Chunk->GetName(), *NewName);
			return false;
		}

		if (bShouldDirectlySetbReplicates)
			Chunk->bReplicates = true;
		else
			Chunk->SetReplicates(true);

		Chunk->bAlwaysRelevant = true;
		Chunk->bOnlyRelevantToOwner = false;

		Chunk->bIsSafeToDestroy = false;

		// for every key in TrackedChunkNamesUpToDate, remove the ChunkKey from the value at that key
		for (TPair<APlayerController*, TArray<FIntVector>>& TrackedCellArray : ChunkManagerRef->TrackedChunkNamesUpToDate)
		{
			TArray<FIntVector>& ChunkCells{ TrackedCellArray.Value };
			ChunkCells.Remove(ChunkCell); // We know it's not up to date, because we just modified the count, and haven't sent it to the client yet
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Chunk %s was replicated, but its name was already %s!"), *Chunk->GetName(), *NewName);
		return false;
	}

	if (!WorldRef)
	{
		UE_LOG(LogTemp, Error, TEXT("WorldRef is nullptr! Cannot draw debug string for Chunk %s"), *Chunk->GetName());
		return false;
	}

	return true;
}

FString FChunkThread::GetDeterministicNameByLocationAndRepCount(const FIntVector& ChunkCell, int32 ReplicationCount)
{
	return FString::Printf(TEXT("X%i_Y%i_Z%i_N%i"), ChunkCell.X, ChunkCell.Y, ChunkCell.Z, ReplicationCount);
}

void FChunkThread::DeleteSaveGame(FString SaveName)
{
	// Validate SaveName
	if (SaveName.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid WorldSaveName: %s"), *SaveName);
		return;
	}

	FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), SaveFolderName, SaveName);

	// Check if the file exists
	if (FPaths::DirectoryExists(SavePath))
	{
		// Try to delete the save file
		if (FPlatformFileManager::Get().GetPlatformFile().DeleteDirectoryRecursively(*SavePath))
		{
			UE_LOG(LogTemp, Warning, TEXT("SaveGame %s deleted successfully."), *SaveName);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to delete SavePath %s."), *SavePath);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("SavePath %s does not exist. SaveFolderName %s, SaveName entered: %s"), *SavePath, *SaveFolderName, *SaveName);
	}
}

TArray<FString> FChunkThread::GetSaveFoldersNames()
{
	TArray<FString> SaveFolderNames{};

	FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), SaveFolderName);

	if (FPaths::DirectoryExists(SavePath))
	{
		TArray<FString> SubDirectoryNames;
		IFileManager::Get().FindFilesRecursive(SubDirectoryNames, *SavePath, TEXT("*"), false, true);

		for (const FString& SubDirectoryName : SubDirectoryNames)
		{
			if (!IFileManager::Get().DirectoryExists(*SubDirectoryName))
				continue;

			FString FolderName = FPaths::GetPathLeaf(SubDirectoryName);
			SaveFolderNames.Add(FolderName);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Save directory does not exist: %s"), *SavePath);
	}

	return SaveFolderNames;
}

void RunLengthEncode(TArray<uint8>& VoxelData, FIntVector OwningChunkCell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RunLengthEncode);

	if (VoxelData.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("Input data was empty. No Voxels to RunLengthEncode"));
		return;
	}

	TArray<uint8> EncodedData;
	EncodedData.Reserve(VoxelData.Num() / 2);

	int32 CurrentCount{ 1 };
	uint8 CurrentValue{ VoxelData[0] };

	for (int32 VoxelIndex{ 1 }; VoxelIndex < VoxelData.Num(); ++VoxelIndex)
	{
		if (VoxelData[VoxelIndex] == CurrentValue && CurrentCount < MAX_uint8)
			++CurrentCount;
		else
		{
			EncodedData.Add(CurrentCount);
			EncodedData.Add(CurrentValue);
			CurrentCount = 1;
			CurrentValue = VoxelData[VoxelIndex];
		}
	}

	EncodedData.Add(CurrentCount);
	EncodedData.Add(CurrentValue);

	VoxelData = MoveTemp(EncodedData);
}

void RunLengthDecode(TArray<uint8>& EncodedData, FIntVector OwningChunkCell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RunLengthDecode);

	if (EncodedData.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("Input data was empty. No Voxels to RunLengthDecode"));
		return;
	}

	int32 DecodedSize{ 0 };
	for (int32 Index{ 0 }; Index < EncodedData.Num(); Index += 2)
		DecodedSize += EncodedData[Index];

	TArray<uint8> DecodedData{};
	DecodedData.Reserve(DecodedSize);

	for (int32 Index{ 0 }; Index < EncodedData.Num(); Index += 2)
	{
		uint8 Count{ EncodedData[Index] };
		uint8 Value{ EncodedData[Index + 1] };

		for (int32 ValueIndex{ 0 }; ValueIndex < Count; ++ValueIndex)
			DecodedData.Add(Value);
	}

	EncodedData = MoveTemp(DecodedData);
}
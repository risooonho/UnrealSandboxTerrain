
#include "UnrealSandboxTerrainPrivatePCH.h"
#include "SandboxTerrainController.h"
#include "Serialization/ArchiveLoadCompressedProxy.h"
#include "Serialization/ArchiveSaveCompressedProxy.h"
#include "Async.h"
#include "Json.h"
#include "JsonObjectConverter.h"
#include "DrawDebugHelpers.h"

#include "TerrainZoneComponent.h"
#include "VdServerComponent.h"
#include "VdClientComponent.h"
#include "VoxelMeshComponent.h"

#include <cmath>
#include <list>

#include "serialization.hpp"
#include "utils.hpp"
#include "VoxelDataInfo.hpp"
#include "TerrainGenerator.hpp"
#include "TerrainData.hpp"
#include "TerrainAreaPipeline.hpp"




bool LoadDataFromKvFile(TKvFile& KvFile, const TVoxelIndex& Index, std::function<void(TValueDataPtr)> Function);
//TValueDataPtr SerializeMeshData(TMeshData const * MeshDataPtr);
TValueDataPtr SerializeMeshData(TMeshDataPtr MeshDataPtr);
//bool CheckSaveDir(FString SaveDir);

//FIXME 
bool bIsGameShutdown;


//======================================================================================================================================================================
// Terrain Controller
//======================================================================================================================================================================

void ASandboxTerrainController::InitializeTerrainController() {
	PrimaryActorTick.bCanEverTick = true;
	MapName = TEXT("World 0");
	bEnableLOD = false;
	SaveGeneratedZones = 1000;
	ServerPort = 6000;
    AutoSavePeriod = 20;
    TerrainData = new TTerrainData();
    CheckAreaMap = new TCheckAreaMap();
    Generator = new TTerrainGenerator(this);
    
    //FString SaveDir = FPaths::ProjectSavedDir() + TEXT("/Map/") + MapName + TEXT("/");
    //CheckSaveDir(SaveDir);
}

void ASandboxTerrainController::BeginDestroy() {
    Super::BeginDestroy();

	bIsGameShutdown = true;

    //delete TerrainData;
    //delete CheckAreaMap;
    //delete Generator;
}

void ASandboxTerrainController::FinishDestroy() {
	Super::FinishDestroy();
	UE_LOG(LogTemp, Warning, TEXT("ASandboxTerrainController::FinishDestroy()"));
	delete TerrainData;
	delete CheckAreaMap;
	delete Generator;
}

ASandboxTerrainController::ASandboxTerrainController(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {
	InitializeTerrainController();
}

ASandboxTerrainController::ASandboxTerrainController() {
	InitializeTerrainController();
}

void ASandboxTerrainController::PostLoad() {
	Super::PostLoad();
	UE_LOG(LogTemp, Log, TEXT("ASandboxTerrainController::PostLoad()"));

	bIsGameShutdown = false;

#if WITH_EDITOR
	//spawnInitialZone();
#endif

}

void ASandboxTerrainController::BeginPlay() {
	Super::BeginPlay();
	UE_LOG(LogTemp, Warning, TEXT("ASandboxTerrainController::BeginPlay()"));
    
    Generator->OnBeginPlay();
    
    GlobalTerrainZoneLOD[0] = 0;
    GlobalTerrainZoneLOD[1] = LodDistance.Distance1;
    GlobalTerrainZoneLOD[2] = LodDistance.Distance2;
    GlobalTerrainZoneLOD[3] = LodDistance.Distance3;
    GlobalTerrainZoneLOD[4] = LodDistance.Distance4;
    GlobalTerrainZoneLOD[5] = LodDistance.Distance5;
    GlobalTerrainZoneLOD[6] = LodDistance.Distance6;

	FoliageMap.Empty();
	if (FoliageDataAsset) {
		FoliageMap = FoliageDataAsset->FoliageMap;
	}

	MaterialMap.Empty();
	if (TerrainParameters) {
		MaterialMap = TerrainParameters->MaterialMap;
	}

	if (!GetWorld()) return;
	bIsLoadFinished = false;

	if (GetWorld()->IsServer()) {
		UE_LOG(LogTemp, Warning, TEXT("SERVER"));
		BeginPlayServer();
	} else {
		UE_LOG(LogTemp, Warning, TEXT("CLIENT"));
		BeginClient();
	}
 
	StartCheckArea();
}

void ASandboxTerrainController::EndPlay(const EEndPlayReason::Type EndPlayReason) {
	Super::EndPlay(EndPlayReason);

	bIsWorkFinished = true;

	{
		std::unique_lock<std::shared_timed_mutex> Lock(ThreadListMutex);
		UE_LOG(LogTemp, Warning, TEXT("TerrainControllerEventList -> %d threads. Waiting for finish..."), TerrainControllerEventList.Num());
		for (auto& TerrainControllerEvent : TerrainControllerEventList) {
			while (!TerrainControllerEvent->IsComplete()) {};
		}
	}

	Save();
	CloseFile();
    TerrainData->Clean();
}

void ASandboxTerrainController::Tick(float DeltaTime) {
	Super::Tick(DeltaTime);

	//if (bIsGeneratingTerrain) {
	//	OnProgressBuildTerrain(GeneratingProgress);
	//}
}

//======================================================================================================================================================================
// Swapping terrain area according player position
//======================================================================================================================================================================

void ASandboxTerrainController::StartPostLoadTimers() {
	GetWorld()->GetTimerManager().SetTimer(TimerAutoSave, this, &ASandboxTerrainController::AutoSaveByTimer, AutoSavePeriod, true);
}

void ASandboxTerrainController::StartCheckArea() {
    AsyncTask(ENamedThreads::GameThread, [=]() {
        for (auto Iterator = GetWorld()->GetPlayerControllerIterator(); Iterator; ++Iterator) {
            APlayerController* PlayerController = Iterator->Get();
            if (PlayerController){
                const auto PlayerId = PlayerController->GetUniqueID();
                const auto Pawn = PlayerController->GetCharacter();
				if (Pawn) {
					const FVector Location = Pawn->GetActorLocation();
					CheckAreaMap->PlayerSwapPosition.Add(PlayerId, Location);
				}
            }
        }
        GetWorld()->GetTimerManager().SetTimer(TimerSwapArea, this, &ASandboxTerrainController::PerformCheckArea, 0.25, true);
    });
}

void ASandboxTerrainController::PerformCheckArea() {
    if(!bEnableAreaSwapping){
        return;
    }
    
    double Start = FPlatformTime::Seconds();
        
    for (auto Iterator = GetWorld()->GetPlayerControllerIterator(); Iterator; ++Iterator) {
        APlayerController* PlayerController = Iterator->Get();
        if (PlayerController){
            const auto PlayerId = PlayerController->GetUniqueID();
            const auto Pawn = PlayerController->GetCharacter();
            const FVector Location = Pawn->GetActorLocation();
            const FVector PrevLocation = CheckAreaMap->PlayerSwapPosition.FindOrAdd(PlayerId);
            const float Distance = FVector::Distance(Location, PrevLocation);
            const float Threshold = PlayerLocationThreshold;
            if(Distance > Threshold) {
                CheckAreaMap->PlayerSwapPosition[PlayerId] = Location;
                TVoxelIndex LocationIndex = GetZoneIndex(Location);
                FVector Tmp = GetZonePos(LocationIndex);
                                
                if(CheckAreaMap->PlayerSwapHandler.Contains(PlayerId)){
                    // cancel old
                    std::shared_ptr<TTerrainLoadPipeline> HandlerPtr2 = CheckAreaMap->PlayerSwapHandler[PlayerId];
                    HandlerPtr2->Cancel();
                    CheckAreaMap->PlayerSwapHandler.Remove(PlayerId);
                }
                
                // start new
                std::shared_ptr<TTerrainLoadPipeline> HandlerPtr = std::make_shared<TTerrainLoadPipeline>();
				CheckAreaMap->PlayerSwapHandler.Add(PlayerId, HandlerPtr);

                if(bShowStartSwapPos){
                    DrawDebugBox(GetWorld(), Location, FVector(100), FColor(255, 0, 255, 0), false, 15);
                    static const float Len = 1000;
                    DrawDebugCylinder(GetWorld(), FVector(Tmp.X, Tmp.Y, Len), FVector(Tmp.X, Tmp.Y, -Len), DynamicLoadArea.Radius, 128, FColor(255, 0, 255, 128), false, 30);
                }
                
				TTerrainAreaPipelineParams Params;
                Params.FullLodDistance = DynamicLoadArea.FullLodDistance;
                Params.Radius = DynamicLoadArea.Radius;
                Params.TerrainSizeMinZ = DynamicLoadArea.TerrainSizeMinZ;
                Params.TerrainSizeMaxZ = DynamicLoadArea.TerrainSizeMaxZ;
                HandlerPtr->SetParams(TEXT("Player_Swap_Terrain_Task"), this, Params);
                
                //TSharedPtr<ASandboxTerrainController> ControllerPtr = MakeShareable(this);
                //TSharedRef<ASandboxTerrainController> NewReference = this;
                
                RunThread([=]() {
                    //TTerrainLoadPipeline& Handler2 = CheckAreaMap->Get(PlayerId);
                    HandlerPtr->LoadArea(Location);
                });
            }
        }
    }
    
    double End = FPlatformTime::Seconds();
    double Time = (End - Start) * 1000;
    //UE_LOG(LogSandboxTerrain, Warning, TEXT("PerformCheckArea -> %f ms"), Time);
}

//======================================================================================================================================================================
// 
//======================================================================================================================================================================


void ASandboxTerrainController::RunGenerateTerrainPipeline(std::function<void()> OnFinish, std::function<void(uint32, uint32)> OnProgress) {
	RunThread([=]() {

		TTerrainAreaPipelineParams Params;
		Params.FullLodDistance = InitialLoadArea.FullLodDistance;
		Params.Radius = InitialLoadArea.Radius;
		Params.TerrainSizeMinZ = InitialLoadArea.TerrainSizeMinZ;
		Params.TerrainSizeMaxZ = InitialLoadArea.TerrainSizeMaxZ;
		Params.OnProgress = OnProgress;

		TTerrainGeneratorPipeline GeneratorPipeline(TEXT("Generate_Terrain_Pipeline"), this, Params);
		GeneratorPipeline.LoadArea(FVector(0));

		if (OnFinish) {
			OnFinish();
		}
	});
}

//======================================================================================================================================================================
// begin play
//======================================================================================================================================================================

void ASandboxTerrainController::BeginPlayServer() {
	if (!OpenFile()) {
		return;
	}

	BeginTerrainLoad();

	UVdServerComponent* VdServerComponent = NewObject<UVdServerComponent>(this, TEXT("VdServer"));
	VdServerComponent->RegisterComponent();
	VdServerComponent->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform, NAME_None);
}

void ASandboxTerrainController::BeginTerrainLoad() {
	LoadJson();

	SpawnInitialZone();
    
    if(bShowInitialArea){
        static const float Len = 5000;
        DrawDebugCylinder(GetWorld(), FVector(0, 0, Len), FVector(0, 0, -Len), InitialLoadArea.FullLodDistance, 128, FColor(255, 255, 255, 0), true);
        DrawDebugCylinder(GetWorld(), FVector(0, 0, Len), FVector(0, 0, -Len), InitialLoadArea.FullLodDistance + LodDistance.Distance2, 128, FColor(255, 255, 255, 0), true);
        DrawDebugCylinder(GetWorld(), FVector(0, 0, Len), FVector(0, 0, -Len), InitialLoadArea.FullLodDistance + LodDistance.Distance5, 128, FColor(255, 255, 255, 0), true);
    }

    if (!bGenerateOnlySmallSpawnPoint) {
        // async loading other zones
        RunThread([&]() {
			TTerrainAreaPipelineParams Params;
            Params.FullLodDistance = InitialLoadArea.FullLodDistance;
            Params.Radius = InitialLoadArea.Radius;
            Params.TerrainSizeMinZ = InitialLoadArea.TerrainSizeMinZ;
            Params.TerrainSizeMaxZ = InitialLoadArea.TerrainSizeMaxZ;
            
			TTerrainLoadPipeline Loader(TEXT("Initial_Load_Task"), this, Params);
            Loader.LoadArea(FVector(0));
			//Generator->Clean;
			UE_LOG(LogTemp, Warning, TEXT("Finish initial terrain load"));
			StartPostLoadTimers();
        });
    }
}

void ASandboxTerrainController::BeginClient() {
	UVdClientComponent* VdClientComponent = NewObject<UVdClientComponent>(this, TEXT("VdClient"));
	VdClientComponent->RegisterComponent();
	VdClientComponent->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform, NAME_None);
}

void ASandboxTerrainController::NetworkSerializeVd(FBufferArchive& Buffer, const TVoxelIndex& VoxelIndex) {
	TVoxelDataInfo* VoxelDataInfo = GetVoxelDataInfo(VoxelIndex);
	// TODO: shared lock Vd
	if (VoxelDataInfo) {
		if (VoxelDataInfo->DataState == TVoxelDataState::READY_TO_LOAD) {
			TVoxelData* Vd = LoadVoxelDataByIndex(VoxelIndex);
			//serializeVoxelData(*Vd, Buffer);
			delete Vd;
		} else if (VoxelDataInfo->DataState == TVoxelDataState::LOADED)  {
			//serializeVoxelData(*VoxelDataInfo->Vd, Buffer);
		}
	}
}

//======================================================================================================================================================================
// save
//======================================================================================================================================================================

void ASandboxTerrainController::FastSave() {
    const std::lock_guard<std::mutex> lock(FastSaveMutex);
    
    if (!VdFile.isOpen() || !MdFile.isOpen() || !ObjFile.isOpen()) return;
    double Start, End, Time1, Time2;
    
    Start = FPlatformTime::Seconds();
    
    std::list<TVoxelIndex> VdList;
    //std::list<FVector> MdList;
    std::list<FVector> ObjList;

	uint32 SavedMd = 0;
	uint32 SavedObj = 0;
    
    //save voxel data
    TerrainData->ForEachVdSafe([&](TVoxelIndex Index, TVoxelDataInfo* VdInfo){
        if (VdInfo->Vd == nullptr) return;
        if (VdInfo->IsChanged()) {
            VdList.push_back(Index);
        }
    });

	//save mesh data
	TerrainData->ForEachMeshDataSafeAndClear([&](TVoxelIndex Index, TMeshDataPtr MeshDataPtr) {
		TValueDataPtr DataPtr = SerializeMeshData(MeshDataPtr);
		if (DataPtr) {
			MdFile.save(Index, *DataPtr);
			SavedMd++;
		}
	});

    TerrainData->ForEachZoneSafe([&](FVector ZoneIndex, UTerrainZoneComponent* Zone){
        // save instanced meshes
        if (FoliageDataAsset) {
            if (Zone->IsNeedSave()) {
				ObjList.push_back(ZoneIndex);
            }
        }
    });

	TerrainData->ForEachInstanceObjectSafeAndClear([&](const TVoxelIndex& Index, const TInstanceMeshTypeMap& InstanceObjectMap) {
		// save instanced meshes
		if (FoliageDataAsset) {
			TValueDataPtr DataPtr = UTerrainZoneComponent::SerializeInstancedMesh(InstanceObjectMap);
			if (DataPtr) {
				ObjFile.save(Index, *DataPtr);
				SavedObj++;
			}
		}
	});
    
    End = FPlatformTime::Seconds();
    Time1 = (End - Start) * 1000;
    Start = FPlatformTime::Seconds();
    
    for(auto& Index : VdList){
        TVoxelDataInfo* VdInfo = GetVoxelDataInfo(Index);
        VdInfo->LoadVdMutexPtr->lock();
        if (VdInfo->Vd == nullptr) return;
        if (VdInfo->IsChanged()) {
            auto Data = VdInfo->Vd->serialize();
            VdFile.save(Index, *Data);
            VdInfo->ResetLastSave();
        }
        VdInfo->Unload();
        VdInfo->LoadVdMutexPtr->unlock();
    }
    
    for(auto& Index : ObjList) {
        TVoxelIndex Index2 = TVoxelIndex(Index.X, Index.Y, Index.Z);
        auto Zone = GetZoneByVectorIndex(Index2);
        if (Zone->IsNeedSave()) {
            auto Data = Zone->SerializeAndResetObjectData();
            ObjFile.save(Index2, *Data);
        }
    }
    
    SaveJson();

    End = FPlatformTime::Seconds();
    Time2 = (End - Start) * 1000;
    UE_LOG(LogSandboxTerrain, Warning, TEXT("Terrain saved: vd/md/obj -> %d/%d/%d  -> %f ms - %f ms"), VdList.size(), SavedMd, ObjList.size() + SavedObj, Time1 , Time2);
}

void ASandboxTerrainController::Save() {
	if (!VdFile.isOpen() || !MdFile.isOpen() || !ObjFile.isOpen()) return;
	double Start = FPlatformTime::Seconds();

	uint32 SavedVd = 0;
	uint32 SavedMd = 0;
	uint32 SavedObj = 0;

    //save voxel data
    TerrainData->ForEachVdSafe([&](TVoxelIndex Index, TVoxelDataInfo* VdInfo){
        if (VdInfo->Vd == nullptr) return;
        if (VdInfo->IsChanged()) {
            //TVoxelIndex Index = GetZoneIndex(VdInfo.Vd->getOrigin());
            auto Data = VdInfo->Vd->serialize();
            VdFile.save(Index, *Data);
            VdInfo->ResetLastSave();
            SavedVd++;
        }

        VdInfo->Unload();
    });

	//save mesh data
	TerrainData->ForEachMeshDataSafeAndClear([&](TVoxelIndex Index, TMeshDataPtr MeshDataPtr) {
		TValueDataPtr DataPtr = SerializeMeshData(MeshDataPtr);
		if (DataPtr) {
			MdFile.save(Index, *DataPtr);
			SavedMd++;
		}
	});
    
	TerrainData->ForEachInstanceObjectSafeAndClear([&](const TVoxelIndex& Index, const TInstanceMeshTypeMap& InstanceObjectMap) {
		// save instanced meshes
		if (FoliageDataAsset) {
			TValueDataPtr DataPtr = UTerrainZoneComponent::SerializeInstancedMesh(InstanceObjectMap);
			if (DataPtr) {
				ObjFile.save(Index, *DataPtr);
				SavedObj++;
			}
		}
	});

    TerrainData->ForEachZoneSafe([&](FVector ZoneIndex, UTerrainZoneComponent* Zone){
        // save instanced meshes
        if (FoliageDataAsset) {
            if (Zone->IsNeedSave()) {
                auto DataPtr = Zone->SerializeAndResetObjectData();
                ObjFile.save(TVoxelIndex(ZoneIndex.X, ZoneIndex.Y, ZoneIndex.Z), *DataPtr);
                SavedObj++;
            }
        }
    });
    
	SaveJson();

	double End = FPlatformTime::Seconds();
	double Time = (End - Start) * 1000;
    UE_LOG(LogSandboxTerrain, Warning, TEXT("Terrain saved: vd/md/obj -> %d/%d/%d  -> %f ms "), SavedVd, SavedMd, SavedObj, Time);
}

void ASandboxTerrainController::SaveMapAsync() {
	UE_LOG(LogSandboxTerrain, Log, TEXT("Start save terrain async"));
	RunThread([&]() {
		FastSave();
	});
}

void ASandboxTerrainController::AutoSaveByTimer() {
    UE_LOG(LogSandboxTerrain, Log, TEXT("Start auto save..."));
    FastSave();
}

void ASandboxTerrainController::SaveJson() {
    UE_LOG(LogTemp, Log, TEXT("Save terrain json"));
    
	MapInfo.SaveTimestamp = FPlatformTime::Seconds();
	FString JsonStr;
    
	FString FileName = TEXT("terrain.json");
	FString SavePath = FPaths::ProjectSavedDir();
	FString FullPath = SavePath + TEXT("/Map/") + MapName + TEXT("/") + FileName;

	FJsonObjectConverter::UStructToJsonObjectString(MapInfo, JsonStr);
	//UE_LOG(LogSandboxTerrain, Warning, TEXT("%s"), *JsonStr);
	FFileHelper::SaveStringToFile(*JsonStr, *FullPath);
}

//======================================================================================================================================================================
// kv file
//======================================================================================================================================================================

bool OpenKvFile(kvdb::KvFile<TVoxelIndex, TValueData>& KvFile, const FString& FileName, const FString& SaveDir) {
	FString FullPath = SaveDir + FileName;
	std::string FilePathString = std::string(TCHAR_TO_UTF8(*FullPath));

	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*FullPath)) {
		kvdb::KvFile<TVoxelIndex, TValueData>::create(FilePathString, std::unordered_map<TVoxelIndex, TValueData>());// create new empty file
	}

	if (!KvFile.open(FilePathString)) {
		UE_LOG(LogSandboxTerrain, Warning, TEXT("Unable to open file: %s"), *FullPath);
		return false;
	}

	return true;
}

bool CheckSaveDir(FString SaveDir){
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*SaveDir)) {
        PlatformFile.CreateDirectory(*SaveDir);
        if (!PlatformFile.DirectoryExists(*SaveDir)) {
            UE_LOG(LogTemp, Warning, TEXT("Unable to create save directory -> %s"), *SaveDir);
            return false;
        }
    }
    
    return true;
}

bool ASandboxTerrainController::OpenFile() {
	// open vd file 	
	FString FileNameVd = TEXT("terrain_voxeldata.dat");
	FString FileNameMd = TEXT("terrain_mesh.dat");
	FString FileNameObj = TEXT("terrain_objects.dat");

	FString SavePath = FPaths::ProjectSavedDir();
	FString SaveDir = SavePath + TEXT("/Map/") + MapName + TEXT("/");

	if (!GetWorld()->IsServer()) {
		SaveDir = SaveDir + TEXT("/ClientCache/");
	}

    if(!CheckSaveDir(SaveDir)){
        return false;
    }

	if (!OpenKvFile(VdFile, FileNameVd, SaveDir)) {
		return false;
	}

	if (!OpenKvFile(MdFile, FileNameMd, SaveDir)) {
		return false;
	}

	if (!OpenKvFile(ObjFile, FileNameObj, SaveDir)) {
		return false;
	}

	return true;
}

void ASandboxTerrainController::CloseFile() {
	VdFile.close();
	MdFile.close();
	ObjFile.close();
}



//======================================================================================================================================================================
// load
//======================================================================================================================================================================

bool ASandboxTerrainController::LoadJson() {
	UE_LOG(LogTemp, Warning, TEXT("Load terrain json"));

	FString FileName = TEXT("terrain.json");
	FString SavePath = FPaths::ProjectSavedDir();
	FString FullPath = SavePath + TEXT("/Map/") + MapName + TEXT("/") + FileName;

	FString JsonRaw;
	if (!FFileHelper::LoadFileToString(JsonRaw, *FullPath, FFileHelper::EHashOptions::None)) {
		UE_LOG(LogTemp, Error, TEXT("Error loading json file"));
		return false;
	}

	if (!FJsonObjectConverter::JsonObjectStringToUStruct(JsonRaw, &MapInfo, 0, 0)) {
		UE_LOG(LogTemp, Error, TEXT("Error parsing json file"));
		return false;
	}

	UE_LOG(LogSandboxTerrain, Warning, TEXT("%s"), *JsonRaw);
	return true;
}

//======================================================================================================================================================================
//  spawn zone
//======================================================================================================================================================================

// spawn received zone on client
void ASandboxTerrainController::NetworkSpawnClientZone(const TVoxelIndex& Index, FArrayReader& RawVdData) {
	FVector Pos = GetZonePos(Index);

	TVoxelDataInfo VdInfo;
	VdInfo.Vd = NewVoxelData();
	VdInfo.Vd->setOrigin(Pos);

	FMemoryReader BinaryData = FMemoryReader(RawVdData, true); 
	BinaryData.Seek(RawVdData.Tell());
	//deserializeVoxelDataFast(*VdInfo.Vd, BinaryData, true);

	VdInfo.DataState = TVoxelDataState::GENERATED;
	VdInfo.SetChanged();
	VdInfo.Vd->setCacheToValid();

    //TerrainData->RegisterVoxelData(VdInfo, Index);

	if (VdInfo.Vd->getDensityFillState() == TVoxelDataFillState::MIXED) {
		TMeshDataPtr MeshDataPtr = GenerateMesh(VdInfo.Vd);
		/*
		InvokeSafe([=]() {
			UTerrainZoneComponent* Zone = AddTerrainZone(Pos);
			//Zone->ApplyTerrainMesh(MeshDataPtr);
		});
		*/
	}
}

TVoxelData* ASandboxTerrainController::NewVoxelData() {
	return new TVoxelData(USBT_ZONE_DIMENSION, USBT_ZONE_SIZE);
}

int ASandboxTerrainController::GeneratePipeline(const TVoxelIndex& Index) {
	TVoxelDataInfo* VdInfo = new TVoxelDataInfo();
	if (!VdFile.isExist(Index)) {
		FVector Pos = GetZonePos(Index);

		// generate new voxel data
		VdInfo->Vd = NewVoxelData();
		VdInfo->Vd->setOrigin(Pos);
		Generator->GenerateVoxelTerrain(*VdInfo->Vd);
		VdInfo->DataState = TVoxelDataState::GENERATED;
		VdInfo->SetChanged();
		VdInfo->Vd->setCacheToValid();
		TerrainData->RegisterVoxelData(VdInfo, Index);

		TInstanceMeshTypeMap& ZoneInstanceObjectMap = TerrainData->GetInstanceObjectTypeMap(Index);
		Generator->GenerateNewFoliage(Index, ZoneInstanceObjectMap);

		if (VdInfo->Vd->getDensityFillState() == TVoxelDataFillState::MIXED) {
			TMeshDataPtr MeshDataPtr = GenerateMesh(VdInfo->Vd);
			TerrainData->PutMeshDataToCache(Index, MeshDataPtr);
		}

		return TZoneSpawnResult::GeneratedNewVd;
	}

	return TZoneSpawnResult::None;
}


// load or generate new zone voxel data and mesh
int ASandboxTerrainController::SpawnZonePipeline(const TVoxelIndex& Index, const TTerrainLodMask TerrainLodMask) {
	//UE_LOG(LogTemp, Log, TEXT("SpawnZone -> %d %d %d "), Index.X, Index.Y, Index.Z);
	FVector Pos = GetZonePos(Index);

    bool bMeshExist = false;
    auto ExistingZone = GetZoneByVectorIndex(Index);
    if(ExistingZone){
		//UE_LOG(LogTemp, Log, TEXT("ExistingZone -> %d %d %d "), Index.X, Index.Y, Index.Z);
        if(ExistingZone->GetTerrainLodMask() <= TerrainLodMask){
            return TZoneSpawnResult::None;
        } else {
            bMeshExist = true;
        }
    }
    
    //if no voxel data in memory
    bool bMemoryHasVoxelData = HasVoxelData(Index);
    bool bNewVdGenerated = false;
	if (!bMemoryHasVoxelData) {
        TVoxelDataInfo* VdInfo = new TVoxelDataInfo();
		// if voxel data exist in file
		if (VdFile.isExist(Index)) {
			VdInfo->DataState = TVoxelDataState::READY_TO_LOAD;
            TerrainData->RegisterVoxelData(VdInfo, Index);
		} else {
            // generate new voxel data
            VdInfo->Vd = NewVoxelData();
            VdInfo->Vd->setOrigin(Pos);
            Generator->GenerateVoxelTerrain(*VdInfo->Vd);
            VdInfo->DataState = TVoxelDataState::GENERATED;
            VdInfo->SetChanged();
            VdInfo->Vd->setCacheToValid();
            TerrainData->RegisterVoxelData(VdInfo, Index);
            bNewVdGenerated = true;
		}
	}

	// voxel data must exist in this point
	TVoxelDataInfo* VoxelDataInfo = GetVoxelDataInfo(Index);

	// if mesh data exist in file - load, apply and return
	TMeshDataPtr MeshDataPtr = LoadMeshDataByIndex(Index);
	if (MeshDataPtr) {
        if(bMeshExist){
            // just change lod mask
			ExecGameThreadZoneApplyMesh(ExistingZone, MeshDataPtr, TerrainLodMask);
            return (int)TZoneSpawnResult::ChangeLodMask;
        } else {
            // spawn new zone with mesh
			ExecGameThreadAddZoneAndApplyMesh(Index, MeshDataPtr, TerrainLodMask, TVoxelDataState::LOADED);
            return (int)TZoneSpawnResult::SpawnMesh;
        }
    } 
    
	// if no mesh data in file - generate mesh from voxel data
	if (VoxelDataInfo->Vd && VoxelDataInfo->Vd->getDensityFillState() == TVoxelDataFillState::MIXED) {
		MeshDataPtr = GenerateMesh(VoxelDataInfo->Vd);

		if (ExistingZone) {
			// just change lod mask
			ExecGameThreadZoneApplyMesh(ExistingZone, MeshDataPtr, TerrainLodMask);
		}

        TVoxelDataState State = VoxelDataInfo->DataState;
		TerrainData->PutMeshDataToCache(Index, MeshDataPtr);
		ExecGameThreadAddZoneAndApplyMesh(Index, MeshDataPtr, TerrainLodMask, State);
	}

	//UE_LOG(LogTemp, Log, TEXT("None -> %d %d %d "), Index.X, Index.Y, Index.Z);
	return bNewVdGenerated ? TZoneSpawnResult::GeneratedNewVd : TZoneSpawnResult::None;
}

void ASandboxTerrainController::SpawnInitialZone() {
	const int s = static_cast<int>(TerrainInitialArea);
	TSet<FVector> InitialZoneSet;

	if (s > 0) {
		for (auto z = -5; z <= 5; z++) {
			for (auto x = -s; x <= s; x++) {
				for (auto y = -s; y <= s; y++) {
					SpawnZonePipeline(TVoxelIndex(x, y, z));
				}
			}
			//TerrainGeneratorComponent->Clean();
		}
	} else {
		FVector Pos = FVector(0);
		SpawnZonePipeline(TVoxelIndex(0, 0, 0));
		InitialZoneSet.Add(Pos);
	}
}

// always in game thread
UTerrainZoneComponent* ASandboxTerrainController::AddTerrainZone(FVector Pos) {
    if (!IsInGameThread()) return nullptr;
    
    TVoxelIndex Index = GetZoneIndex(Pos);
    if(GetZoneByVectorIndex(Index)) return nullptr; // no duplicate

    FVector IndexTmp(Index.X, Index.Y,Index.Z);
    FString ZoneName = FString::Printf(TEXT("Zone -> [%.0f, %.0f, %.0f]"), IndexTmp.X, IndexTmp.Y, IndexTmp.Z);
    UTerrainZoneComponent* ZoneComponent = NewObject<UTerrainZoneComponent>(this, FName(*ZoneName));
    if (ZoneComponent) {
        ZoneComponent->RegisterComponent();
        //ZoneComponent->SetRelativeLocation(pos);

        ZoneComponent->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform, NAME_None);
        ZoneComponent->SetWorldLocation(Pos);

        FString TerrainMeshCompName = FString::Printf(TEXT("TerrainMesh -> [%.0f, %.0f, %.0f]"), IndexTmp.X, IndexTmp.Y, IndexTmp.Z);
        UVoxelMeshComponent* TerrainMeshComp = NewObject<UVoxelMeshComponent>(this, FName(*TerrainMeshCompName));
        TerrainMeshComp->RegisterComponent();
        TerrainMeshComp->SetMobility(EComponentMobility::Stationary);
        TerrainMeshComp->SetCanEverAffectNavigation(true);
        TerrainMeshComp->SetCollisionProfileName(TEXT("InvisibleWall"));
        TerrainMeshComp->AttachToComponent(ZoneComponent, FAttachmentTransformRules::KeepRelativeTransform, NAME_None);
        TerrainMeshComp->ZoneIndex = Index;

        ZoneComponent->MainTerrainMesh = TerrainMeshComp;
    }

    TerrainData->AddZone(IndexTmp, ZoneComponent);
    if(bShowZoneBounds) DrawDebugBox(GetWorld(), Pos, FVector(USBT_ZONE_SIZE / 2), FColor(255, 0, 0, 100), true);
    return ZoneComponent;
}

//======================================================================================================================================================================
//
//======================================================================================================================================================================

TVoxelIndex ASandboxTerrainController::GetZoneIndex(const FVector& Pos) {
	FVector Tmp = sandboxGridIndex(Pos, USBT_ZONE_SIZE);
	return TVoxelIndex(Tmp.X, Tmp.Y, Tmp.Z);
}

FVector ASandboxTerrainController::GetZonePos(const TVoxelIndex& Index) {
	return FVector((float)Index.X * USBT_ZONE_SIZE, (float)Index.Y * USBT_ZONE_SIZE, (float)Index.Z * USBT_ZONE_SIZE);
}

UTerrainZoneComponent* ASandboxTerrainController::GetZoneByVectorIndex(const TVoxelIndex& Index) {
	FVector TmpIndex(Index.X, Index.Y, Index.Z);
	return TerrainData->GetZone(TmpIndex);
}

TVoxelData* ASandboxTerrainController::GetVoxelDataByPos(const FVector& Pos) {
    return GetVoxelDataByIndex(GetZoneIndex(Pos));
}

TVoxelData* ASandboxTerrainController::GetVoxelDataByIndex(const TVoxelIndex& Index) {
    return TerrainData->GetVd(Index);
}

bool ASandboxTerrainController::HasVoxelData(const TVoxelIndex& Index) {
    return TerrainData->HasVoxelData(Index);
}

TVoxelDataInfo* ASandboxTerrainController::GetVoxelDataInfo(const TVoxelIndex& Index) {
    return TerrainData->GetVoxelDataInfo(Index);
}

//======================================================================================================================================================================
// Edit Terrain
//======================================================================================================================================================================

template<class H>
class FTerrainEditThread : public FRunnable {
public:
	H ZoneHandler;
	ASandboxTerrainController* ControllerInstance;

	virtual uint32 Run() {
		ControllerInstance->EditTerrain(ZoneHandler);
		return 0;
	}
};

struct TZoneEditHandler {
	bool changed = false;

	bool enableLOD = false;

	float Strength;

	FVector Pos;

	float Extend;
};

void ASandboxTerrainController::FillTerrainRound(const FVector& Origin, float Extend, int MatId) {
	if (!GetWorld()->IsServer()) return;

	struct ZoneHandler : TZoneEditHandler {
		int newMaterialId;
		bool operator()(TVoxelData* vd) {
			changed = false;

			vd->forEachWithCache([&](int x, int y, int z) {
				float density = vd->getDensity(x, y, z);
				FVector o = vd->voxelIndexToVector(x, y, z);
				o += vd->getOrigin();
				o -= Pos;

				float rl = std::sqrt(o.X * o.X + o.Y * o.Y + o.Z * o.Z);
				if (rl < Extend) {
					//2^-((x^2)/20)
					float d = density + 1 / rl * Strength;
					vd->setDensity(x, y, z, d);
					changed = true;
				}

				if (rl < Extend + 20) {
					vd->setMaterial(x, y, z, newMaterialId);
				}			
			}, enableLOD);

			return changed;
		}
	} Zh;

	Zh.newMaterialId = MatId;
	Zh.enableLOD = bEnableLOD;
	Zh.Strength = 5;
	Zh.Pos = Origin;
	Zh.Extend = Extend;
	ASandboxTerrainController::PerformTerrainChange(Zh);
}


void ASandboxTerrainController::DigTerrainRoundHole(const FVector& Origin, float Radius, float Strength) {
	if (GetWorld()->IsServer()) {
		//UVdServerComponent* VdServerComponent = Cast<UVdServerComponent>(GetComponentByClass(UVdServerComponent::StaticClass()));
		//VdServerComponent->SendToAllClients(USBT_NET_OPCODE_DIG_ROUND, Origin.X, Origin.Y, Origin.Z, Radius, Strength);
	} else {
		//UVdClientComponent* VdClientComponent = Cast<UVdClientComponent>(GetComponentByClass(UVdClientComponent::StaticClass()));
		//VdClientComponent->SendToServer(USBT_NET_OPCODE_DIG_ROUND, Origin.X, Origin.Y, Origin.Z, Radius, Strength);
		return;
	}

	DigTerrainRoundHole_Internal(Origin, Radius, Strength);
}

#define USBT_MAX_MATERIAL_HARDNESS 99999.9f

void ASandboxTerrainController::DigTerrainRoundHole_Internal(const FVector& Origin, float Radius, float Strength) {
	struct ZoneHandler : TZoneEditHandler {
		TMap<uint16, FSandboxTerrainMaterial>* MaterialMapPtr;

		bool operator()(TVoxelData* vd) {
			changed = false;

			vd->forEachWithCache([&](int x, int y, int z) {
				float density = vd->getDensity(x, y, z);
				FVector o = vd->voxelIndexToVector(x, y, z);
				o += vd->getOrigin();
				o -= Pos;

				float rl = std::sqrt(o.X * o.X + o.Y * o.Y + o.Z * o.Z);
				if (rl < Extend) {
					unsigned short  MatId = vd->getMaterial(x, y, z);
					FSandboxTerrainMaterial& Mat = MaterialMapPtr->FindOrAdd(MatId);

					if (Mat.RockHardness < USBT_MAX_MATERIAL_HARDNESS) {
						float ClcStrength = (Mat.RockHardness == 0) ? Strength : (Strength / Mat.RockHardness);
						if (ClcStrength > 0.1) {
							float d = density - 1 / rl * (ClcStrength);
							vd->setDensity(x, y, z, d);
						}
					}

					changed = true;
				}
			}, enableLOD);

			return changed;
		}
	} Zh;

	Zh.MaterialMapPtr = &MaterialMap;
	Zh.enableLOD = bEnableLOD;
	Zh.Strength = Strength;
	Zh.Pos = Origin;
	Zh.Extend = Radius;
	ASandboxTerrainController::PerformTerrainChange(Zh);
}


void ASandboxTerrainController::DigTerrainCubeHole(const FVector& Origin, float Extend) {
	if (!GetWorld()->IsServer()) return;

	struct ZoneHandler : TZoneEditHandler {
		TMap<uint16, FSandboxTerrainMaterial>* MaterialMapPtr;

		bool operator()(TVoxelData* vd) {
			changed = false;

			FBox Box(FVector(-Extend), FVector(Extend));
			FRotator Rotator(0, 30, 0);

			vd->forEachWithCache([&](int x, int y, int z) {
				FVector o = vd->voxelIndexToVector(x, y, z);
				o += vd->getOrigin();
				o -= Pos;
				//o = Rotator.RotateVector(o);
				//o = o.RotateAngleAxis(45, FVector(0, 0, 1));
				//bool bIsIntersect = FMath::PointBoxIntersection(o, Box);
				//if (bIsIntersect) {
				if (o.X < Extend && o.X > -Extend && o.Y < Extend && o.Y > -Extend && o.Z < Extend && o.Z > -Extend) {
					unsigned short  MatId = vd->getMaterial(x, y, z);
					FSandboxTerrainMaterial& Mat = MaterialMapPtr->FindOrAdd(MatId);
					if (Mat.RockHardness < USBT_MAX_MATERIAL_HARDNESS) {
						vd->setDensity(x, y, z, 0);
						changed = true;
					}
				}
			}, enableLOD);

			return changed;
		}
	} Zh;

	Zh.enableLOD = bEnableLOD;
	Zh.MaterialMapPtr = &MaterialMap;
	Zh.Pos = Origin;
	Zh.Extend = Extend;
	ASandboxTerrainController::PerformTerrainChange(Zh);
}

void ASandboxTerrainController::FillTerrainCube(const FVector& Origin, float Extend, int MatId) {
	if (!GetWorld()->IsServer()) return;

	struct ZoneHandler : TZoneEditHandler {
		int newMaterialId;
		bool operator()(TVoxelData* vd) {
			changed = false;

			vd->forEachWithCache([&](int x, int y, int z) {
				FVector o = vd->voxelIndexToVector(x, y, z);
				o += vd->getOrigin();
				o -= Pos;
				if (o.X < Extend && o.X > -Extend && o.Y < Extend && o.Y > -Extend && o.Z < Extend && o.Z > -Extend) {
					vd->setDensity(x, y, z, 1);
					changed = true;
				}

				float radiusMargin = Extend + 20;
				if (o.X < radiusMargin && o.X > -radiusMargin && o.Y < radiusMargin && o.Y > -radiusMargin && o.Z < radiusMargin && o.Z > -radiusMargin) {
					vd->setMaterial(x, y, z, newMaterialId);
				}
			}, enableLOD);

			return changed;
		}
	} Zh;

	Zh.newMaterialId = MatId;
	Zh.enableLOD = bEnableLOD;
	Zh.Pos = Origin;
	Zh.Extend = Extend;
	ASandboxTerrainController::PerformTerrainChange(Zh);
}

template<class H>
void ASandboxTerrainController::PerformTerrainChange(H Handler) {
	FTerrainEditThread<H>* EditThread = new FTerrainEditThread<H>();
	EditThread->ZoneHandler = Handler;
	EditThread->ControllerInstance = this;

	FString ThreadName = FString::Printf(TEXT("terrain_change-thread-%d"), FPlatformTime::Seconds());
	FRunnableThread* Thread = FRunnableThread::Create(EditThread, *ThreadName);
	//FIXME delete thread after finish

	FVector TestPoint(Handler.Pos);
	TestPoint.Z -= 10;
	TArray<struct FHitResult> OutHits;
	bool bIsOverlap = GetWorld()->SweepMultiByChannel(OutHits, Handler.Pos, TestPoint, FQuat(), ECC_Visibility, FCollisionShape::MakeSphere(Handler.Extend)); // ECC_Visibility
	if (bIsOverlap) {
		for (FHitResult& Overlap : OutHits) {
			if (Cast<ASandboxTerrainController>(Overlap.GetActor())) {
				UHierarchicalInstancedStaticMeshComponent* InstancedMesh = Cast<UHierarchicalInstancedStaticMeshComponent>(Overlap.GetComponent());
				if (InstancedMesh) {
					InstancedMesh->RemoveInstance(Overlap.Item);

					TArray<USceneComponent*> Parents;
					InstancedMesh->GetParentComponents(Parents);
					if (Parents.Num() > 0) {
						UTerrainZoneComponent* Zone = Cast<UTerrainZoneComponent>(Parents[0]);
						if (Zone) {
							Zone->SetNeedSave();
						}
					}
				}
			} else {
				OnOverlapActorDuringTerrainEdit(Overlap, Handler.Pos);
			}
		}
	}
}

template<class H>
void ASandboxTerrainController::PerformZoneEditHandler(TVoxelDataInfo* VdInfo, H handler, std::function<void(TMeshDataPtr)> OnComplete) {
	bool bIsChanged = false;
	TMeshDataPtr MeshDataPtr = nullptr;

	VdInfo->LoadVdMutexPtr->lock();
	VdInfo->Vd->vd_edit_mutex.lock();
	bIsChanged = handler(VdInfo->Vd);
	if (bIsChanged) {
		VdInfo->SetChanged();
		VdInfo->Vd->setCacheToValid();
		MeshDataPtr = GenerateMesh(VdInfo->Vd);
		VdInfo->ResetLastMeshRegenerationTime();
		MeshDataPtr->TimeStamp = FPlatformTime::Seconds();
	}
	VdInfo->Vd->vd_edit_mutex.unlock();
	VdInfo->LoadVdMutexPtr->unlock();

	if (bIsChanged) {
		OnComplete(MeshDataPtr);
	}
}

// TODO refactor concurency according new terrain data system
template<class H>
void ASandboxTerrainController::EditTerrain(const H& ZoneHandler) {
	double Start = FPlatformTime::Seconds();
	
	static float ZoneVolumeSize = USBT_ZONE_SIZE / 2;
	TVoxelIndex BaseZoneIndex = GetZoneIndex(ZoneHandler.Pos);

	static const float V[3] = { -1, 0, 1 };
	for (float x : V) {
		for (float y : V) {
			for (float z : V) {
				TVoxelIndex ZoneIndex = BaseZoneIndex + TVoxelIndex(x, y, z);
				UTerrainZoneComponent* Zone = GetZoneByVectorIndex(ZoneIndex);
				TVoxelDataInfo* VoxelDataInfo = GetVoxelDataInfo(ZoneIndex);
				TVoxelData* Vd = nullptr;

				// check zone bounds
				FVector ZoneOrigin = GetZonePos(ZoneIndex);
				FVector Upper(ZoneOrigin.X + ZoneVolumeSize, ZoneOrigin.Y + ZoneVolumeSize, ZoneOrigin.Z + ZoneVolumeSize);
				FVector Lower(ZoneOrigin.X - ZoneVolumeSize, ZoneOrigin.Y - ZoneVolumeSize, ZoneOrigin.Z - ZoneVolumeSize);

                if (FMath::SphereAABBIntersection(FSphere(ZoneHandler.Pos, ZoneHandler.Extend * 2.f), FBox(Lower, Upper))) {
					if (VoxelDataInfo == nullptr) {
						continue;
					}

					if (VoxelDataInfo->DataState == TVoxelDataState::READY_TO_LOAD) {
						// double-check locking
						VoxelDataInfo->LoadVdMutexPtr->lock();
						if (VoxelDataInfo->DataState == TVoxelDataState::READY_TO_LOAD) {
							Vd = LoadVoxelDataByIndex(ZoneIndex);
							VoxelDataInfo->DataState = TVoxelDataState::LOADED;
							VoxelDataInfo->Vd = Vd;
						} else {
							Vd = VoxelDataInfo->Vd;
						}
						VoxelDataInfo->LoadVdMutexPtr->unlock();
					} else {
						Vd = VoxelDataInfo->Vd;
					}

					if (Vd == nullptr) {
						continue;
					}

					if (Zone == nullptr) {
						PerformZoneEditHandler(VoxelDataInfo, ZoneHandler, [&](TMeshDataPtr MeshDataPtr){ 
							TerrainData->PutMeshDataToCache(ZoneIndex, MeshDataPtr);
							ExecGameThreadAddZoneAndApplyMesh(ZoneIndex, MeshDataPtr); 
						});
					} else {
						PerformZoneEditHandler(VoxelDataInfo, ZoneHandler, [&](TMeshDataPtr MeshDataPtr){
							TerrainData->PutMeshDataToCache(ZoneIndex, MeshDataPtr);
							ExecGameThreadZoneApplyMesh(Zone, MeshDataPtr); 
						});
					}
				}
			}
		}
	}

	double End = FPlatformTime::Seconds();
	double Time = (End - Start) * 1000;
	//UE_LOG(LogTemp, Warning, TEXT("ASandboxTerrainController::editTerrain -------------> %f ms"), Time);
}

//======================================================================================================================================================================
// invoke async
//======================================================================================================================================================================

/*
void ASandboxTerrainController::InvokeSafe(std::function<void()> Function) {
    if (IsInGameThread()) {
        Function();
    } else {
        AsyncTask(ENamedThreads::GameThread, [=]() { Function(); });
    }
}
*/

void ASandboxTerrainController::ExecGameThreadZoneApplyMesh(UTerrainZoneComponent* Zone, TMeshDataPtr MeshDataPtr,const TTerrainLodMask TerrainLodMask) {
	ASandboxTerrainController* Controller = this;

	TFunction<void()> Function = [=]() {
		if (!bIsGameShutdown) {
			if (MeshDataPtr) {
				Zone->ApplyTerrainMesh(MeshDataPtr, TerrainLodMask);
			}
		} else {
			UE_LOG(LogTemp, Log, TEXT("ASandboxTerrainController::ExecGameThreadZoneApplyMesh - game shutdown"));
		}
	};

	if (IsInGameThread()) {
		Function();
	} else {
		AsyncTask(ENamedThreads::GameThread, Function);
	}
}

void ASandboxTerrainController::ExecGameThreadAddZoneAndApplyMesh(const TVoxelIndex& Index, TMeshDataPtr MeshDataPtr, const TTerrainLodMask TerrainLodMask, const uint32 State) {
	FVector ZonePos = GetZonePos(Index);
	ASandboxTerrainController* Controller = this;

	TFunction<void()> Function = [=]() {
		if (!bIsGameShutdown) {
			if (MeshDataPtr) {
				UTerrainZoneComponent* Zone = AddTerrainZone(ZonePos);
				if (Zone) {
					Zone->ApplyTerrainMesh(MeshDataPtr, TerrainLodMask);

					if (State == TVoxelDataState::GENERATED) {
						OnGenerateNewZone(Index, Zone);
					}

					if (State == TVoxelDataState::LOADED) {
						OnLoadZone(Zone);
					}
				}
			}
		} else {
			UE_LOG(LogTemp, Log, TEXT("ASandboxTerrainController::ExecGameThreadAddZoneAndApplyMesh - game shutdown"));
		}
	};

	if (IsInGameThread()) {
		Function();
	} else {
		AsyncTask(ENamedThreads::GameThread, Function);
	}
}

void ASandboxTerrainController::RunThread(TUniqueFunction<void()> Function) {
    std::unique_lock<std::shared_timed_mutex> Lock(ThreadListMutex);
    TerrainControllerEventList.Add(FFunctionGraphTask::CreateAndDispatchWhenReady(MoveTemp(Function), TStatId(), nullptr, ENamedThreads::AnyBackgroundThreadNormalTask));
}

//======================================================================================================================================================================
// load vd
//======================================================================================================================================================================

// TODO: use shared_ptr
TVoxelData* ASandboxTerrainController::LoadVoxelDataByIndex(const TVoxelIndex& Index) {
	double Start = FPlatformTime::Seconds();

	TVoxelData* Vd = NewVoxelData();
	Vd->setOrigin(GetZonePos(Index));

	bool bIsLoaded = LoadDataFromKvFile(VdFile, Index, [=](TValueDataPtr DataPtr) {
		deserializeVoxelData(Vd, *DataPtr);
	});

	double End = FPlatformTime::Seconds();
	double Time = (End - Start) * 1000;

	if (bIsLoaded) {
		UE_LOG(LogTemp, Log, TEXT("loading voxel data block -> %d %d %d -> %f ms"), Index.X, Index.Y, Index.Z, Time);

		double Start2 = FPlatformTime::Seconds();

		Vd->makeSubstanceCache();

		double End2 = FPlatformTime::Seconds();
		double Time2 = (End2 - Start2) * 1000;
		UE_LOG(LogTemp, Log, TEXT("makeSubstanceCache() -> %d %d %d -> %f ms"), Index.X, Index.Y, Index.Z, Time2);
	}

	return Vd;
}

//======================================================================================================================================================================
// events
//======================================================================================================================================================================

void ASandboxTerrainController::OnGenerateNewZone(const TVoxelIndex& Index, UTerrainZoneComponent* Zone) {
    if (FoliageDataAsset) {
		TInstanceMeshTypeMap ZoneInstanceMeshMap;
        Generator->GenerateNewFoliage(Index, ZoneInstanceMeshMap);
		Zone->SpawnAll(ZoneInstanceMeshMap);
		Zone->SetNeedSave();
    }
}

void ASandboxTerrainController::OnLoadZone(UTerrainZoneComponent* Zone) {
	if (FoliageDataAsset) {
		LoadFoliage(Zone);
	}
}

void ASandboxTerrainController::OnFinishAsyncPhysicsCook(const TVoxelIndex& ZoneIndex) {
    /*
	InvokeSafe([=]() {
		UTerrainZoneComponent* Zone = GetZoneByVectorIndex(ZoneIndex);
		if (Zone) {
			TVoxelDataInfo* VoxelDataInfo = GetVoxelDataInfo(ZoneIndex);
			if (VoxelDataInfo->IsNewGenerated()) {
				if (TerrainGeneratorComponent && FoliageDataAsset) {
					TerrainGeneratorComponent->GenerateNewFoliage(Zone);
				}
				OnGenerateNewZone(Zone);
			}
		}
	});
     */
}

//======================================================================================================================================================================
// virtual functions
//======================================================================================================================================================================

FORCEINLINE bool ASandboxTerrainController::OnCheckFoliageSpawn(const TVoxelIndex& ZoneIndex, const FVector& FoliagePos, FVector& Scale) {
    return true;
}

FORCEINLINE float ASandboxTerrainController::GeneratorDensityFunc(const TVoxelDensityFunctionData& FunctionData) {
    return FunctionData.Density;
}

FORCEINLINE bool ASandboxTerrainController::IsOverrideGroundLevel(const TVoxelIndex& Index) {
	return false;
}

FORCEINLINE float ASandboxTerrainController::GeneratorGroundLevelFunc(const TVoxelIndex& Index, const FVector& Pos, float GroundLevel) {
	return GroundLevel;
}

FORCEINLINE bool ASandboxTerrainController::GeneratorForcePerformZone(const TVoxelIndex& ZoneIndex) {
    return false;
}

FORCEINLINE void ASandboxTerrainController::OnOverlapActorDuringTerrainEdit(const FHitResult& OverlapResult, const FVector& Pos) {

}

FORCEINLINE FSandboxFoliage ASandboxTerrainController::GeneratorFoliageOverride(const int32 FoliageTypeId, const FSandboxFoliage& FoliageType, const TVoxelIndex& ZoneIndex, const FVector& WorldPos) {
	return FoliageType;
}


//======================================================================================================================================================================
// Perlin noise according seed
//======================================================================================================================================================================

// TODO use seed
float ASandboxTerrainController::PerlinNoise(const FVector& Pos) const {
	return Generator->PerlinNoise(Pos.X, Pos.Y, Pos.Z);
}

// range 0..1
float ASandboxTerrainController::NormalizedPerlinNoise(const FVector& Pos) const {
	float NormalizedPerlin = (Generator->PerlinNoise(Pos.X, Pos.Y, Pos.Z) + 0.87) / 1.73;
	return NormalizedPerlin;
}

//======================================================================================================================================================================
// Sandbox Foliage
//======================================================================================================================================================================

void ASandboxTerrainController::LoadFoliage(UTerrainZoneComponent* Zone) {
	TInstanceMeshTypeMap ZoneInstanceMeshMap;
	LoadObjectDataByIndex(Zone, ZoneInstanceMeshMap);
	Zone->SpawnAll(ZoneInstanceMeshMap);

	/*
	for (auto& Elem : ZoneInstMeshMap) {
		TInstanceMeshArray& InstMeshTransArray = Elem.Value;

		for (FTransform& Transform : InstMeshTransArray.TransformArray) {
			Zone->SpawnInstancedMesh(InstMeshTransArray.MeshType, Transform);
		}
	}
	*/
}

//======================================================================================================================================================================
// Materials
//======================================================================================================================================================================

UMaterialInterface* ASandboxTerrainController::GetRegularTerrainMaterial(uint16 MaterialId) {
	if (RegularMaterial == nullptr) {
		return nullptr;
	}

	if (!RegularMaterialCache.Contains(MaterialId)) {
		UE_LOG(LogTemp, Warning, TEXT("create new regular terrain material instance ----> id: %d"), MaterialId);

		UMaterialInstanceDynamic* DynMaterial = UMaterialInstanceDynamic::Create(RegularMaterial, this);

		if (MaterialMap.Contains(MaterialId)) {
			FSandboxTerrainMaterial Mat = MaterialMap[MaterialId];

			DynMaterial->SetTextureParameterValue("TextureTopMicro", Mat.TextureTopMicro);
			//DynMaterial->SetTextureParameterValue("TextureSideMicro", Mat.TextureSideMicro);
			DynMaterial->SetTextureParameterValue("TextureMacro", Mat.TextureMacro);
			DynMaterial->SetTextureParameterValue("TextureNormal", Mat.TextureNormal);
		}

		RegularMaterialCache.Add(MaterialId, DynMaterial);
		return DynMaterial;
	}

	return RegularMaterialCache[MaterialId];
}

UMaterialInterface* ASandboxTerrainController::GetTransitionTerrainMaterial(const std::set<unsigned short>& MaterialIdSet) {
	if (TransitionMaterial == nullptr) {
		return nullptr;
	}

	uint64 Code = TMeshMaterialTransitionSection::GenerateTransitionCode(MaterialIdSet);
	if (!TransitionMaterialCache.Contains(Code)) {
		TTransitionMaterialCode tmp;
		tmp.Code = Code;

		UE_LOG(LogTemp, Warning, TEXT("create new transition terrain material instance ----> id: %llu (%lu-%lu-%lu)"), Code, tmp.TriangleMatId[0], tmp.TriangleMatId[1], tmp.TriangleMatId[2]);
		UMaterialInstanceDynamic* DynMaterial = UMaterialInstanceDynamic::Create(TransitionMaterial, this);

		int Idx = 0;
		for (unsigned short MatId : MaterialIdSet) {
			if (MaterialMap.Contains(MatId)) {
				FSandboxTerrainMaterial Mat = MaterialMap[MatId];

				FName TextureTopMicroParam = FName(*FString::Printf(TEXT("TextureTopMicro%d"), Idx));
				//FName TextureSideMicroParam = FName(*FString::Printf(TEXT("TextureSideMicro%d"), Idx));
				FName TextureMacroParam = FName(*FString::Printf(TEXT("TextureMacro%d"), Idx));
				FName TextureNormalParam = FName(*FString::Printf(TEXT("TextureNormal%d"), Idx));

				DynMaterial->SetTextureParameterValue(TextureTopMicroParam, Mat.TextureTopMicro);
				//DynMaterial->SetTextureParameterValue(TextureSideMicroParam, Mat.TextureSideMicro);
				DynMaterial->SetTextureParameterValue(TextureMacroParam, Mat.TextureMacro);
				DynMaterial->SetTextureParameterValue(TextureNormalParam, Mat.TextureNormal);
			}

			Idx++;
		}

		TransitionMaterialCache.Add(Code, DynMaterial);
		return DynMaterial;
	}

	return TransitionMaterialCache[Code];
}

//======================================================================================================================================================================
// generate mesh
//======================================================================================================================================================================

std::shared_ptr<TMeshData> ASandboxTerrainController::GenerateMesh(TVoxelData* Vd) {
	double Start = FPlatformTime::Seconds();

	if (Vd == NULL || Vd->getDensityFillState() == TVoxelDataFillState::ZERO ||	Vd->getDensityFillState() == TVoxelDataFillState::FULL) {
		return NULL;
	}

	TVoxelDataParam Vdp;
    
    Vdp.bZCut = true;
    Vdp.ZCutLevel = -100;

	if (bEnableLOD) {
		Vdp.bGenerateLOD = true;
		Vdp.collisionLOD = GetCollisionMeshSectionLodIndex();
	} else {
		Vdp.bGenerateLOD = false;
		Vdp.collisionLOD = 0;
	}

	TMeshDataPtr MeshDataPtr = sandboxVoxelGenerateMesh(*Vd, Vdp);

	double End = FPlatformTime::Seconds();
	double Time = (End - Start) * 1000;

	//UE_LOG(LogTemp, Warning, TEXT("generateMesh -------------> %f %f %f --> %f ms"), Vd->getOrigin().X, Vd->getOrigin().Y, Vd->getOrigin().Z, Time);
	return MeshDataPtr;
}

//======================================================================================================================================================================
// mesh data de/serealization
//======================================================================================================================================================================

void SerializeMeshContainer(const TMeshContainer& MeshContainer, FastUnsafeSerializer& Serializer) {
	// save regular materials
	int32 LodSectionRegularMatNum = MeshContainer.MaterialSectionMap.Num();
	Serializer << LodSectionRegularMatNum;
	for (auto& Elem : MeshContainer.MaterialSectionMap) {
		unsigned short MatId = Elem.Key;
		const TMeshMaterialSection& MaterialSection = Elem.Value;

		Serializer << MatId;

		const FProcMeshSection& Mesh = MaterialSection.MaterialMesh;
		Mesh.SerializeMesh(Serializer);
	}

	// save transition materials
	int32 LodSectionTransitionMatNum = MeshContainer.MaterialTransitionSectionMap.Num();
	Serializer << LodSectionTransitionMatNum;
	for (auto& Elem : MeshContainer.MaterialTransitionSectionMap) {
		unsigned short MatId = Elem.Key;
		const TMeshMaterialTransitionSection& TransitionMaterialSection = Elem.Value;

		Serializer << MatId;

		int MatSetSize = TransitionMaterialSection.MaterialIdSet.size();
		Serializer << MatSetSize;

		for (unsigned short MatSetElement : TransitionMaterialSection.MaterialIdSet) {
			Serializer << MatSetElement;
		}

		const FProcMeshSection& Mesh = TransitionMaterialSection.MaterialMesh;
		Mesh.SerializeMesh(Serializer);
	}
}

TValueDataPtr Compress(TValueDataPtr CompressedDataPtr) {
	TValueDataPtr Result = std::make_shared<TValueData>();
	TArray<uint8> BinaryArray;
	BinaryArray.SetNum(CompressedDataPtr->size());
	FMemory::Memcpy(BinaryArray.GetData(), CompressedDataPtr->data(), CompressedDataPtr->size());

	TArray<uint8> CompressedData;
	FArchiveSaveCompressedProxy Compressor = FArchiveSaveCompressedProxy(CompressedData, NAME_Zlib);
	Compressor << BinaryArray;
	Compressor.Flush();

	//float CompressionRatio = (CompressedDataPtr->size() / DecompressedData.Num()) * 100.f;
	//UE_LOG(LogTemp, Log, TEXT("CompressedData -> %d bytes -> %f%%"), DecompressedData.Num(), CompressionRatio);

	Result->resize(CompressedData.Num());
	FMemory::Memcpy(Result->data(), CompressedData.GetData(), CompressedData.Num());
	CompressedData.Empty();

	return Result;
}


TValueDataPtr SerializeMeshData(TMeshDataPtr MeshDataPtr) {
	FastUnsafeSerializer Serializer;

	int32 LodArraySize = MeshDataPtr->MeshSectionLodArray.Num();
	Serializer << LodArraySize;

	for (int32 LodIdx = 0; LodIdx < LodArraySize; LodIdx++) {
		const TMeshLodSection& LodSection = MeshDataPtr->MeshSectionLodArray[LodIdx];
		Serializer << LodIdx;

		// save whole mesh
		LodSection.WholeMesh.SerializeMesh(Serializer);

		SerializeMeshContainer(LodSection.RegularMeshContainer, Serializer);

		if (LodIdx > 0) {
			for (auto i = 0; i < 6; i++) {
				SerializeMeshContainer(LodSection.TransitionPatchArray[i], Serializer);
			}
		}
	}

	TValueDataPtr Result = Compress(Serializer.data());
	return Result;
}

void DeserializeMeshContainerFast(TMeshContainer& MeshContainer, FastUnsafeDeserializer& Deserializer) {
	// regular materials
	int32 LodSectionRegularMatNum;
	Deserializer.readObj(LodSectionRegularMatNum);

	for (int RMatIdx = 0; RMatIdx < LodSectionRegularMatNum; RMatIdx++) {
		unsigned short MatId;
		Deserializer.readObj(MatId);

		TMeshMaterialSection& MatSection = MeshContainer.MaterialSectionMap.FindOrAdd(MatId);
		MatSection.MaterialId = MatId;

		MatSection.MaterialMesh.DeserializeMeshFast(Deserializer);
	}

	// transition materials
	int32 LodSectionTransitionMatNum;
	Deserializer.readObj(LodSectionTransitionMatNum);

	for (int TMatIdx = 0; TMatIdx < LodSectionTransitionMatNum; TMatIdx++) {
		unsigned short MatId;
		Deserializer.readObj(MatId);

		int MatSetSize;
		Deserializer.readObj(MatSetSize);

		std::set<unsigned short> MatSet;
		for (int MatSetIdx = 0; MatSetIdx < MatSetSize; MatSetIdx++) {
			unsigned short MatSetElement;
			Deserializer.readObj(MatSetElement);

			MatSet.insert(MatSetElement);
		}

		TMeshMaterialTransitionSection& MatTransSection = MeshContainer.MaterialTransitionSectionMap.FindOrAdd(MatId);
		MatTransSection.MaterialId = MatId;
		MatTransSection.MaterialIdSet = MatSet;

		MatTransSection.MaterialMesh.DeserializeMeshFast(Deserializer);
	}
}

TMeshDataPtr DeserializeMeshDataFast(const std::vector<uint8>& Data, uint32 CollisionMeshSectionLodIndex) {
	TMeshDataPtr MeshDataPtr(new TMeshData);
	FastUnsafeDeserializer Deserializer(Data.data());

	int32 LodArraySize;
	Deserializer.readObj(LodArraySize);

	for (int LodIdx = 0; LodIdx < LodArraySize; LodIdx++) {
		int32 LodIndex;
		Deserializer.readObj(LodIndex);

		// whole mesh
		MeshDataPtr.get()->MeshSectionLodArray[LodIndex].WholeMesh.DeserializeMeshFast(Deserializer);
		DeserializeMeshContainerFast(MeshDataPtr.get()->MeshSectionLodArray[LodIndex].RegularMeshContainer, Deserializer);

		if (LodIdx > 0) {
			for (auto i = 0; i < 6; i++) {
				DeserializeMeshContainerFast(MeshDataPtr.get()->MeshSectionLodArray[LodIndex].TransitionPatchArray[i], Deserializer);
			}
		}
	}

	MeshDataPtr.get()->CollisionMeshPtr = &MeshDataPtr.get()->MeshSectionLodArray[CollisionMeshSectionLodIndex].WholeMesh;
	return MeshDataPtr;
}

bool LoadDataFromKvFile(TKvFile& KvFile, const TVoxelIndex& Index, std::function<void(TValueDataPtr)> Function) {
	TValueDataPtr DataPtr = KvFile.loadData(Index);
	if (DataPtr == nullptr || DataPtr->size() == 0) { return false;	}
	Function(DataPtr);
	return true;
}

TValueDataPtr Decompress(TValueDataPtr CompressedDataPtr) {
	TValueDataPtr Result = std::make_shared<TValueData>();
	TArray<uint8> BinaryArray;
	BinaryArray.SetNum(CompressedDataPtr->size());
	FMemory::Memcpy(BinaryArray.GetData(), CompressedDataPtr->data(), CompressedDataPtr->size());

	FArchiveLoadCompressedProxy Decompressor = FArchiveLoadCompressedProxy(BinaryArray, NAME_Zlib);
	if (Decompressor.GetError()) {
		//UE_LOG(LogTemp, Log, TEXT("FArchiveLoadCompressedProxy -> ERROR : File was not compressed"));
		return Result;
	}

	TArray<uint8> DecompressedData;
	Decompressor << DecompressedData;

	float CompressionRatio = ((float)CompressedDataPtr->size() / (float)DecompressedData.Num()) * 100.f;
	//UE_LOG(LogTemp, Log, TEXT("DecompressedData -> %d bytes ==> %d bytes -> %f%%"), DecompressedData.Num(), CompressedDataPtr->size(), CompressionRatio);

	Result->resize(DecompressedData.Num());
	FMemory::Memcpy(Result->data(), DecompressedData.GetData(), DecompressedData.Num());
	Decompressor.FlushCache();
	DecompressedData.Empty();

	return Result;
}


TMeshDataPtr ASandboxTerrainController::LoadMeshDataByIndex(const TVoxelIndex& Index) {
	double Start = FPlatformTime::Seconds();
	TMeshDataPtr MeshDataPtr = nullptr;

	bool bIsLoaded = LoadDataFromKvFile(MdFile, Index, [&](TValueDataPtr DataPtr) {
		auto DecompressedDataPtr = Decompress(DataPtr);
		MeshDataPtr = DeserializeMeshDataFast(*DecompressedDataPtr, GetCollisionMeshSectionLodIndex());
	});

	double End = FPlatformTime::Seconds();
	double Time = (End - Start) * 1000;

	if (bIsLoaded) {
		//UE_LOG(LogTemp, Log, TEXT("loading mesh data block -> %d %d %d -> %f ms"), Index.X, Index.Y, Index.Z, Time);
	}

	return MeshDataPtr;
}

void ASandboxTerrainController::LoadObjectDataByIndex(UTerrainZoneComponent* Zone, TInstanceMeshTypeMap& ZoneInstMeshMap) {
	double Start = FPlatformTime::Seconds();
	TVoxelIndex Index = GetZoneIndex(Zone->GetComponentLocation());

	bool bIsLoaded = LoadDataFromKvFile(ObjFile, Index, [&](TValueDataPtr DataPtr) {
		Zone->DeserializeInstancedMeshes(*DataPtr, ZoneInstMeshMap);
	});

	double End = FPlatformTime::Seconds();
	double Time = (End - Start) * 1000;

	if (bIsLoaded) {
		//UE_LOG(LogTemp, Log, TEXT("loading inst-objects data block -> %d %d %d -> %f ms"), Index.X, Index.Y, Index.Z, Time);
	}
}

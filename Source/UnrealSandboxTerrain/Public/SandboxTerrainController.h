#pragma once

#include "Engine.h"
#include "Runtime/Engine/Classes/Engine/DataAsset.h"
#include <memory>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <unordered_map>
#include "VoxelIndex.h"
#include "kvdb.hpp"
#include "VoxelData.h"
#include "SandboxTerrainController.generated.h"

struct TMeshData;
class UVoxelMeshComponent;
class UTerrainZoneComponent;
struct TInstMeshTransArray;
class UVdClientComponent;
class TTerrainLoadHandler;
class TTerrainData;
class TCheckAreaMap;
class TTerrainGenerator;

typedef TMap<int32, TInstMeshTransArray> TInstMeshTypeMap;
typedef std::shared_ptr<TMeshData> TMeshDataPtr;
typedef kvdb::KvFile<TVoxelIndex, TValueData> TKvFile;


UENUM(BlueprintType)
enum class ETerrainInitialArea : uint8 {
	TIA_1_1 = 0	UMETA(DisplayName = "1x1"),
	TIA_3_3 = 1	UMETA(DisplayName = "3x3"),
};

enum TVoxelDataState {
	UNDEFINED = 0,
	GENERATED = 1,
	LOADED = 2,
	READY_TO_LOAD = 3
};

class TVoxelDataInfo {

private:
	volatile double LastChange;
	volatile double LastSave;
	volatile double LastMeshGeneration;
	volatile double LastCacheCheck;

public:
	TVoxelDataInfo() {	LoadVdMutexPtr = std::make_shared<std::mutex>(); }
	~TVoxelDataInfo() {	}

	TVoxelData* Vd = nullptr;
	TVoxelDataState DataState = TVoxelDataState::UNDEFINED;
	std::shared_ptr<std::mutex> LoadVdMutexPtr;

	bool IsNewGenerated() const { return DataState == TVoxelDataState::GENERATED; }
	bool IsNewLoaded() const { return DataState == TVoxelDataState::LOADED;	}
	void SetChanged() { LastChange = FPlatformTime::Seconds(); }
	bool IsChanged() { return LastChange > LastSave; }
	void ResetLastSave() { LastSave = FPlatformTime::Seconds(); }
	bool IsNeedToRegenerateMesh() { return LastChange > LastMeshGeneration; }
	void ResetLastMeshRegenerationTime() { LastMeshGeneration = FPlatformTime::Seconds(); }

	void Unload();
};

USTRUCT()
struct FMapInfo {
	GENERATED_BODY()

	UPROPERTY()
	uint32 FormatVersion = 0;

	UPROPERTY()
	uint32 FormatSubversion = 0;

	UPROPERTY()
	double SaveTimestamp;
};

USTRUCT()
struct FTerrainInstancedMeshType {
	GENERATED_BODY()

	UPROPERTY()
	int32 MeshTypeId;

	UPROPERTY()
	UStaticMesh* Mesh;

	UPROPERTY()
	int32 StartCullDistance;

	UPROPERTY()
	int32 EndCullDistance;
};

USTRUCT()
struct FSandboxFoliage {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	UStaticMesh* Mesh;

	UPROPERTY(EditAnywhere)
	int32 SpawnStep = 25;

	UPROPERTY(EditAnywhere)
	float Probability = 1;

	UPROPERTY(EditAnywhere)
	int32 StartCullDistance = 100;

	UPROPERTY(EditAnywhere)
	int32 EndCullDistance = 500;

	UPROPERTY(EditAnywhere)
	float OffsetRange = 10.0f;

	UPROPERTY(EditAnywhere)
	float ScaleMinZ = 0.5f;

	UPROPERTY(EditAnywhere)
	float ScaleMaxZ = 1.0f;
};

UCLASS(BlueprintType, Blueprintable)
class UNREALSANDBOXTERRAIN_API USandboxTarrainFoliageMap : public UDataAsset {
	GENERATED_BODY()

public:

	UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain Foliage")
	TMap<uint32, FSandboxFoliage> FoliageMap;
};


USTRUCT()
struct FSandboxTerrainMaterial {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FString Name;

	UPROPERTY(EditAnywhere)
	float RockHardness;

	UPROPERTY(EditAnywhere)
	UTexture* TextureTopMicro;

	//UPROPERTY(EditAnywhere)
	//UTexture* TextureSideMicro;

	UPROPERTY(EditAnywhere)
	UTexture* TextureMacro;

	UPROPERTY(EditAnywhere)
	UTexture* TextureNormal;
};

USTRUCT()
struct FTerrainUndergroundLayer {
    GENERATED_BODY()

    UPROPERTY(EditAnywhere)
    int32 MatId;

    UPROPERTY(EditAnywhere)
    float StartDepth;

    UPROPERTY(EditAnywhere)
    FString Name;
};

UCLASS(Blueprintable)
class UNREALSANDBOXTERRAIN_API USandboxTerrainParameters : public UDataAsset {
	GENERATED_BODY()

public:

	UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain Material")
	TMap<uint16, FSandboxTerrainMaterial> MaterialMap;
    
    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain Generator")
    TArray<FTerrainUndergroundLayer> UndergroundLayers;

};

extern float GlobalTerrainZoneLOD[LOD_ARRAY_SIZE];

USTRUCT()
struct FSandboxTerrainLODDistance {
    GENERATED_BODY()
    
    UPROPERTY(EditAnywhere)
    float Distance1 = 1500;
    
    UPROPERTY(EditAnywhere)
    float Distance2 = 3000;
    
    UPROPERTY(EditAnywhere)
    float Distance3 = 6000;
    
    UPROPERTY(EditAnywhere)
    float Distance4 = 8000;
    
    UPROPERTY(EditAnywhere)
    float Distance5 = 10000;
    
    UPROPERTY(EditAnywhere)
    float Distance6 = 12000;
};

typedef uint8 TTerrainLodMask;

UENUM(BlueprintType)
enum class ETerrainLodMaskPreset : uint8 {
    All      = 0            UMETA(DisplayName = "Show all"),
    Medium   = 0b00000011   UMETA(DisplayName = "Show medium"),
    Far      = 0b00011111   UMETA(DisplayName = "Show far"),
};

USTRUCT()
struct FTerrainSwapAreaParams {
    GENERATED_BODY()
    
    UPROPERTY(EditAnywhere)
    float Radius = 3000;
    
    UPROPERTY(EditAnywhere)
    float FullLodDistance = 1000;
    
    UPROPERTY(EditAnywhere)
    int TerrainSizeMinZ = 5;
    
    UPROPERTY(EditAnywhere)
    int TerrainSizeMaxZ = 5;
};


typedef struct TVoxelDensityFunctionData {
    float Density;
    float GroundLelel;
    FVector WorldPos;
    FVector LocalPos;
    TVoxelIndex ZoneIndex;
} TVoxelDensityFunctionData;


UCLASS()
class UNREALSANDBOXTERRAIN_API ASandboxTerrainController : public AActor {
	GENERATED_UCLASS_BODY()

public:
    ASandboxTerrainController();
    
    friend TTerrainLoadHandler;
    friend UTerrainZoneComponent;
	friend TTerrainGenerator;
	friend UVdClientComponent;

	virtual void BeginPlay() override;

	virtual void Tick(float DeltaSeconds) override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual void PostLoad() override;
    
    virtual void BeginDestroy() override;
    
	//========================================================================================
	// debug only
	//========================================================================================

	UPROPERTY(EditAnywhere, Category = "UnrealSandbox Debug")
	bool bGenerateOnlySmallSpawnPoint = false;
    
    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Debug")
    ETerrainInitialArea TerrainInitialArea = ETerrainInitialArea::TIA_3_3;

	UPROPERTY(EditAnywhere, Category = "UnrealSandbox Debug")
	bool bShowZoneBounds = false;
    
    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Debug")
    bool bShowInitialArea = false;
    
    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Debug")
    bool bShowStartSwapPos = false;
    
    //========================================================================================
    // general
    //========================================================================================

    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain")
    FString MapName;

    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain")
    int32 Seed;

    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain")
    FTerrainSwapAreaParams InitialLoadArea;
        
    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain")
    bool bEnableLOD;
    
    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain")
    FSandboxTerrainLODDistance LodDistance;
    
    //========================================================================================
    // Dynamic area swapping
    //========================================================================================
    
    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain")
    bool bEnableAreaSwapping;
    
    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain")
    float PlayerLocationThreshold = 1000;
    
    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain")
    FTerrainSwapAreaParams DynamicLoadArea;

	//========================================================================================
	// 
	//========================================================================================

	UFUNCTION(BlueprintImplementableEvent, meta = (DisplayName = "On Start Build Sandbox Terrain"))
	void OnStartBuildTerrain();

	UFUNCTION(BlueprintImplementableEvent, meta = (DisplayName = "On Finish Build Sandbox Terrain"))
	void OnFinishBuildTerrain();

	UFUNCTION(BlueprintImplementableEvent, meta = (DisplayName = "On Progress Build Sandbox Terrain"))
	void OnProgressBuildTerrain(float Progress);

	//========================================================================================
	// save/load
	//========================================================================================

	UFUNCTION(BlueprintCallable, Category = "UnrealSandbox")
	void SaveMapAsync();

    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain")
    int32 AutoSavePeriod;
    
	UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain")
	int32 SaveGeneratedZones;

	//========================================================================================
	// materials
	//========================================================================================

	UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain Material")
	UMaterialInterface* RegularMaterial;

	UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain Material")
	UMaterialInterface* TransitionMaterial;

	UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain Material")
	USandboxTerrainParameters* TerrainParameters;

	//========================================================================================
	// collision
	//========================================================================================

	UPROPERTY(EditAnywhere, Category = "UnrealSandbox Collision")
	unsigned int CollisionSection;

	void OnFinishAsyncPhysicsCook(const TVoxelIndex& ZoneIndex);

	//========================================================================================
	// foliage
	//========================================================================================

	UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain Foliage")
	USandboxTarrainFoliageMap* FoliageDataAsset;
    
    //========================================================================================
    // networking
    //========================================================================================

    UPROPERTY(EditAnywhere, Category = "UnrealSandbox Terrain Network")
    uint32 ServerPort;

	//========================================================================================
	
	//static bool CheckZoneBounds(FVector Origin, float Size);

	//========================================================================================

	void DigTerrainRoundHole(const FVector& Origin, float Radius, float Strength);

	void DigTerrainCubeHole(const FVector& Origin, float Extend);

	void FillTerrainCube(const FVector& Origin, float Extend, int MatId);

	void FillTerrainRound(const FVector& Origin, float Extend, int MatId);

	TVoxelIndex GetZoneIndex(const FVector& Pos);

	FVector GetZonePos(const TVoxelIndex& Index);

	UTerrainZoneComponent* GetZoneByVectorIndex(const TVoxelIndex& Index);

	template<class H>
	void PerformTerrainChange(H handler);

	template<class H>
	void EditTerrain(const H& ZoneHandler);

	UMaterialInterface* GetRegularTerrainMaterial(uint16 MaterialId);

	UMaterialInterface* GetTransitionTerrainMaterial(const std::set<unsigned short>& MaterialIdSet);

	//===============================================================================
	// async tasks
	//===============================================================================

	void InvokeSafe(std::function<void()> Function);

	void RunThread(TUniqueFunction<void()> Function);

	//========================================================================================
	// network
	//========================================================================================

	void NetworkSerializeVd(FBufferArchive& Buffer, const TVoxelIndex& VoxelIndex);

	void NetworkSpawnClientZone(const TVoxelIndex& Index, FArrayReader& RawVdData);

private:
    
    TCheckAreaMap* CheckAreaMap;

    FTimerHandle TimerSwapArea;
    
    void PerformCheckArea();
    
    void StartCheckArea();
    
    TMap<uint32, FVector> PlayerSwapAreaMap;
    
	void BeginClient();

	void DigTerrainRoundHole_Internal(const FVector& Origin, float Radius, float Strength);

	template<class H>
	FORCEINLINE void PerformZoneEditHandler(TVoxelDataInfo& VdInfo, H handler, std::function<void(TMeshDataPtr)> OnComplete);

	volatile bool bIsWorkFinished = false;

	volatile float GeneratingProgress;

	//===============================================================================
	// save/load
	//===============================================================================
    
    FTimerHandle TimerAutoSave;
    
    std::mutex FastSaveMutex;

	bool bIsLoadFinished;

	void Save();
    
    void FastSave();
    
    void AutoSaveByTimer();

	void SaveJson();

	bool LoadJson();
	
	void SpawnInitialZone();

	int SpawnZone(const TVoxelIndex& pos, const TTerrainLodMask TerrainLodMask = 0);

	UTerrainZoneComponent* AddTerrainZone(FVector pos);

	bool IsWorkFinished() { return bIsWorkFinished; };

	//===============================================================================
	// async tasks
	//===============================================================================

	void InvokeZoneMeshAsync(UTerrainZoneComponent* Zone, TMeshDataPtr MeshDataPtr);

	void InvokeLazyZoneAsync(TVoxelIndex& Index, TMeshDataPtr MeshDataPtr);

	//===============================================================================
	// threads
	//===============================================================================

	std::shared_timed_mutex ThreadListMutex;

	FGraphEventArray TerrainControllerEventList;

	//===============================================================================
	// voxel data storage
	//===============================================================================
    
    TTerrainGenerator* Generator;
    
    TTerrainData* TerrainData;

	TKvFile VdFile;

	TKvFile MdFile;

	TKvFile ObjFile;

	TVoxelData* GetVoxelDataByPos(const FVector& Pos);

	TVoxelData* GetVoxelDataByIndex(const TVoxelIndex& Index);

	bool HasVoxelData(const TVoxelIndex& Index);

	TVoxelDataInfo* GetVoxelDataInfo(const TVoxelIndex& Index);

	TVoxelData* LoadVoxelDataByIndex(const TVoxelIndex& Index);

	std::shared_ptr<TMeshData> GenerateMesh(TVoxelData* Vd);

	//===============================================================================
	// mesh data storage
	//===============================================================================

	TMeshDataPtr LoadMeshDataByIndex(const TVoxelIndex& Index);

	void LoadObjectDataByIndex(UTerrainZoneComponent* Zone, TInstMeshTypeMap& ZoneInstMeshMap);

	//===============================================================================
	// foliage
	//===============================================================================

	TMap<uint32, FSandboxFoliage> FoliageMap;

	void LoadFoliage(UTerrainZoneComponent* Zone);

	void SpawnFoliage(int32 FoliageTypeId, FSandboxFoliage& FoliageType, FVector& v, FRandomStream& rnd, UTerrainZoneComponent* Zone);

	//===============================================================================
	// materials
	//===============================================================================

	UPROPERTY()
	TMap<uint64, UMaterialInterface*> TransitionMaterialCache;

	UPROPERTY()
	TMap<uint16, UMaterialInterface*> RegularMaterialCache;

	TMap<uint16, FSandboxTerrainMaterial> MaterialMap;

	//===============================================================================
	// collision
	//===============================================================================

	int GetCollisionMeshSectionLodIndex() {
		if (bEnableLOD) {
			if (CollisionSection > 6) return 6;
			return CollisionSection;
		}
		return 0;
	}

    void OnGenerateNewZone(const TVoxelIndex& Index, UTerrainZoneComponent* Zone);

    void OnLoadZone(UTerrainZoneComponent* Zone);
    
    //===============================================================================
    // save/load
    //===============================================================================

    bool VerifyMap();

    bool OpenFile();

    void RunLoadMapAsync(std::function<void()> OnFinish);
    
    TVoxelData* NewVoxelData();
    
protected:

	virtual void InitializeTerrainController();

	virtual void BeginPlayServer();
       
    //===============================================================================
    // virtual functions
    //===============================================================================
    
    virtual bool OnCheckFoliageSpawn(const TVoxelIndex& ZoneIndex, const FVector& FoliagePos, FVector& Scale);
    
    virtual float GeneratorDensityFunc(const TVoxelDensityFunctionData& FunctionData);
    
    virtual bool GeneratorForcePerformZone(const TVoxelIndex& ZoneIndex);
	
};

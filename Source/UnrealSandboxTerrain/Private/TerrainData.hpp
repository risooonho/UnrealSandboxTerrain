//
//  TerrainData.h
//  UE4VoxelTerrain
//
//  Created by blackw2012 on 19.04.2020..
//

#pragma once

#include "EngineMinimal.h"
#include "VoxelIndex.h"
#include "VoxelData.h"
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

class TTerrainData {
    
private:
    std::shared_timed_mutex Mutex;
    std::unordered_map<TVoxelIndex, TVoxelDataInfo*> VoxelDataIndexMap;
    TMap<FVector, UTerrainZoneComponent*> TerrainZoneMap;
    
    
public:
    void AddZone(const FVector& Pos, UTerrainZoneComponent* ZoneComponent){
        std::unique_lock<std::shared_timed_mutex> Lock(Mutex);
        TerrainZoneMap.Add(Pos, ZoneComponent);
    }
    
    UTerrainZoneComponent* GetZone(const FVector& Pos){
        std::shared_lock<std::shared_timed_mutex> Lock(Mutex);
        if (TerrainZoneMap.Contains(Pos)) {
            return TerrainZoneMap[Pos];
        }
        return nullptr;
    }
    
    void ForEachZoneSafe(std::function<void(const FVector Pos, UTerrainZoneComponent* Zone)> Function){
        std::unique_lock<std::shared_timed_mutex> Lock(Mutex);
        for (auto& Elem : TerrainZoneMap) {
            FVector Pos = Elem.Key;
            UTerrainZoneComponent* Zone = Elem.Value;
            Function(Pos, Zone);
        }
    }
    
    void RegisterVoxelData(TVoxelDataInfo* VdInfo, TVoxelIndex Index) {
        std::unique_lock<std::shared_timed_mutex> Lock(Mutex);
        auto It = VoxelDataIndexMap.find(Index);
        if (It != VoxelDataIndexMap.end()) {
            VoxelDataIndexMap.erase(It);
        }
        VoxelDataIndexMap.insert({ Index, VdInfo });
    }
    
    TVoxelData* GetVd(const TVoxelIndex& Index) {
        std::shared_lock<std::shared_timed_mutex> Lock(Mutex);
        if (VoxelDataIndexMap.find(Index) != VoxelDataIndexMap.end()) {
            TVoxelDataInfo* VdInfo = VoxelDataIndexMap[Index];
            return VdInfo->Vd;
        }

        return nullptr;
    }

    bool HasVoxelData(const TVoxelIndex& Index) {
        std::shared_lock<std::shared_timed_mutex> Lock(Mutex);
        return VoxelDataIndexMap.find(Index) != VoxelDataIndexMap.end();
    }

    TVoxelDataInfo* GetVoxelDataInfo(const TVoxelIndex& Index) {
        std::shared_lock<std::shared_timed_mutex> Lock(Mutex);
        if (VoxelDataIndexMap.find(Index) != VoxelDataIndexMap.end()) {
            return VoxelDataIndexMap[Index];
        }

        return nullptr;
    }
    
    void ForEachVdSafe(std::function<void(const TVoxelIndex& Index, TVoxelDataInfo* VdInfo)> Function){
        std::unique_lock<std::shared_timed_mutex> Lock(Mutex);
        for (auto& It : VoxelDataIndexMap) {
            const auto& Index = It.first;
            TVoxelDataInfo* VdInfo = VoxelDataIndexMap[It.first];
            Function(Index, VdInfo);
        }
    }
        
    void Clean(){
        std::unique_lock<std::shared_timed_mutex> Lock(Mutex);
        TerrainZoneMap.Empty();
        
        for (auto& It : VoxelDataIndexMap) {
            TVoxelDataInfo* VdInfo = It.second;
            if(VdInfo){
                VdInfo->Unload();
                delete VdInfo;
            }
        }
        VoxelDataIndexMap.clear();
    }
};

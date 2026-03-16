#pragma once
#include "CoreMinimal.h"
#include "ReplicationGraph.h"
#include "DAReplicationGraph.generated.h"

enum class EClassRepPolicy : uint8
{
    NotRouted,
    RelevantAllConnections,
    Spatialize_Static,
    Spatialize_Dynamic,
    Spatialize_Dormancy
};

class UReplicationGraphNode_ActorList;
class UReplicationGraphNode_GridSpatialization2D;
class UReplicationGraphNode_AlwaysRelevant_ForConnection;

UCLASS(Transient, config = Engine)
class FLEXIBLECOMBATSYSTEM_API UDAReplicationGraph : public UReplicationGraph
{
    GENERATED_BODY()
public:
    virtual void ResetGameWorldState() override;
    virtual void InitConnectionGraphNodes(UNetReplicationGraphConnection* ConnectionManager) override;
    virtual void InitGlobalActorClassSettings() override;
    virtual void InitGlobalGraphNodes() override;
    virtual void RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo) override;
    virtual void RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo) override;

    UPROPERTY()
    TArray<UClass*> NonSpatializedClasses;

    UPROPERTY()
    UReplicationGraphNode_GridSpatialization2D* GridNode = nullptr;

    UPROPERTY()
    UReplicationGraphNode_ActorList* AlwaysRelevantNode = nullptr;

    TMap<FName, FActorRepListRefView> AlwaysRelevantStreamingLevelActors;

protected:
    class UDAReplicationGraphNode_AlwaysRelevant_ForConnection* GetAlwaysRelevantNode(APlayerController* PlayerController);

    FORCEINLINE bool IsSpatialized(EClassRepPolicy Mapping)
    {
        return Mapping >= EClassRepPolicy::Spatialize_Static;
    }

    EClassRepPolicy GetMappingPolicy(const UClass* InClass);
    TClassMap<EClassRepPolicy> ClassRepPolicies;

    float GridCellSize = 10000.f;
    float SpatialBiasX = -150000.f;
    float SpatialBiasY = -200000.f;
};

UCLASS()
class FLEXIBLECOMBATSYSTEM_API UDAReplicationGraphNode_AlwaysRelevant_ForConnection
    : public UReplicationGraphNode_AlwaysRelevant_ForConnection
{
    GENERATED_BODY()
public:
    virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;
    void OnClientLevelVisibilityAdd(FName LevelName, UWorld* LevelWorld);
    void OnClientLevelVisibilityRemove(FName LevelName);
    void ResetGameWorldState();

protected:
    TArray<FName, TInlineAllocator<64>> AlwaysRelevantStreamingLevels;
};
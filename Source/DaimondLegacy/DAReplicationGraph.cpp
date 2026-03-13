#include "DAReplicationGraph.h"
#include "Engine/LevelScriptActor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"

void UDAReplicationGraph::ResetGameWorldState()
{
    Super::ResetGameWorldState();
    AlwaysRelevantStreamingLevelActors.Empty();

    for (auto& ConnectionList : { Connections, PendingConnections })
    {
        for (UNetReplicationGraphConnection* Connection : ConnectionList)
        {
            for (UReplicationGraphNode* ConnectionNode : Connection->GetConnectionGraphNodes())
            {
                if (UDAReplicationGraphNode_AlwaysRelevant_ForConnection* Node =
                    Cast<UDAReplicationGraphNode_AlwaysRelevant_ForConnection>(ConnectionNode))
                {
                    Node->ResetGameWorldState();
                }
            }
        }
    }
}

void UDAReplicationGraph::InitConnectionGraphNodes(
    UNetReplicationGraphConnection* ConnectionManager)
{
    // Super çağırma — kasıtlı
    UDAReplicationGraphNode_AlwaysRelevant_ForConnection* Node =
        CreateNewNode<UDAReplicationGraphNode_AlwaysRelevant_ForConnection>();

    ConnectionManager->OnClientVisibleLevelNameAdd.AddUObject(
        Node, &UDAReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityAdd);
    ConnectionManager->OnClientVisibleLevelNameRemove.AddUObject(
        Node, &UDAReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityRemove);

    AddConnectionGraphNode(Node, ConnectionManager);
}

void UDAReplicationGraph::InitGlobalActorClassSettings()
{
    Super::InitGlobalActorClassSettings();

    auto SetRule = [&](UClass* InClass, EClassRepPolicy Mapping)
        {
            ClassRepPolicies.Set(InClass, Mapping);
        };

    // ── Routing kuralları ─────────────────────────────────────────
    SetRule(AReplicationGraphDebugActor::StaticClass(), EClassRepPolicy::NotRouted);
    SetRule(ALevelScriptActor::StaticClass(), EClassRepPolicy::NotRouted);
    SetRule(APlayerController::StaticClass(), EClassRepPolicy::NotRouted);
    SetRule(AInfo::StaticClass(), EClassRepPolicy::RelevantAllConnections);
    SetRule(AGameStateBase::StaticClass(), EClassRepPolicy::RelevantAllConnections);
    SetRule(APlayerState::StaticClass(), EClassRepPolicy::RelevantAllConnections);
    SetRule(AGameModeBase::StaticClass(), EClassRepPolicy::RelevantAllConnections);
    SetRule(APawn::StaticClass(), EClassRepPolicy::Spatialize_Dynamic);
    SetRule(ACharacter::StaticClass(), EClassRepPolicy::Spatialize_Dynamic);
    SetRule(AActor::StaticClass(), EClassRepPolicy::Spatialize_Dynamic); // ← ekle

    // ── Tüm replicated class'ları topla ──────────────────────────
    TArray<UClass*> ReplicatedClasses;

    for (TObjectIterator<UClass> Itr; Itr; ++Itr)
    {
        UClass* Class = *Itr;
        AActor* ActorCDO = Cast<AActor>(Class->GetDefaultObject());

        if (!ActorCDO || !ActorCDO->GetIsReplicated()) continue;

        FString ClassName = Class->GetName();
        if (ClassName.StartsWith(TEXT("SKEL_")) || ClassName.StartsWith(TEXT("REINST_"))) continue;
        if (Class->IsChildOf(APlayerController::StaticClass())) continue;

        ReplicatedClasses.Add(Class);

        if (ClassRepPolicies.Contains(Class, false)) continue;

        auto ShouldSpatialize = [](const AActor* Actor)
            {
                return Actor->GetIsReplicated() &&
                    !(Actor->bAlwaysRelevant || Actor->bOnlyRelevantToOwner || Actor->bNetUseOwnerRelevancy);
            };

        if (ShouldSpatialize(ActorCDO))
            SetRule(Class, EClassRepPolicy::Spatialize_Dynamic);
        else if (ActorCDO->bAlwaysRelevant && !ActorCDO->bOnlyRelevantToOwner)
            SetRule(Class, EClassRepPolicy::RelevantAllConnections);
    }

    // ── Tüm class'lara 15m cull distance ver ─────────────────────
    for (UClass* ReplicatedClass : ReplicatedClasses)
    {
        EClassRepPolicy Policy = GetMappingPolicy(ReplicatedClass);

        FClassReplicationInfo ClassInfo;
        ClassInfo.ReplicationPeriodFrame = 1;

        if (IsSpatialized(Policy))
            ClassInfo.SetCullDistanceSquared(1500.f * 1500.f); // 15m
        else
            ClassInfo.SetCullDistanceSquared(0.f); // AlwaysRelevant — sınırsız

        GlobalActorReplicationInfoMap.SetClassInfo(ReplicatedClass, ClassInfo);
    }
}

void UDAReplicationGraph::InitGlobalGraphNodes()
{
    GridNode = CreateNewNode<UReplicationGraphNode_GridSpatialization2D>();
    GridNode->CellSize = GridCellSize;
    GridNode->SpatialBias = FVector2D(SpatialBiasX, SpatialBiasY);
    AddGlobalGraphNode(GridNode);

    AlwaysRelevantNode = CreateNewNode<UReplicationGraphNode_ActorList>();
    AddGlobalGraphNode(AlwaysRelevantNode);
}

void UDAReplicationGraph::RouteAddNetworkActorToNodes(
    const FNewReplicatedActorInfo& ActorInfo,
    FGlobalActorReplicationInfo& GlobalInfo)
{
    EClassRepPolicy Policy = GetMappingPolicy(ActorInfo.Class);

    // Her spatialized actor için cull distance'ı direkt zorla
    if (IsSpatialized(Policy))
        GlobalInfo.Settings.SetCullDistanceSquared(1500.f * 1500.f); // 15m

    switch (Policy)
    {
    case EClassRepPolicy::RelevantAllConnections:
        if (ActorInfo.StreamingLevelName == NAME_None)
            AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
        else
        {
            FActorRepListRefView& RepList =
                AlwaysRelevantStreamingLevelActors.FindOrAdd(ActorInfo.StreamingLevelName);
            RepList.ConditionalAdd(ActorInfo.Actor);
        }
        break;

    case EClassRepPolicy::Spatialize_Static:
        GridNode->AddActor_Static(ActorInfo, GlobalInfo);
        break;

    case EClassRepPolicy::Spatialize_Dynamic:
        GridNode->AddActor_Dynamic(ActorInfo, GlobalInfo);
        break;

    case EClassRepPolicy::Spatialize_Dormancy:
        GridNode->AddActor_Dormancy(ActorInfo, GlobalInfo);
        break;

    default:
        break;
    }
}

void UDAReplicationGraph::RouteRemoveNetworkActorToNodes(
    const FNewReplicatedActorInfo& ActorInfo)
{
    EClassRepPolicy Policy = GetMappingPolicy(ActorInfo.Class);

    switch (Policy)
    {
    case EClassRepPolicy::RelevantAllConnections:
        if (ActorInfo.StreamingLevelName == NAME_None)
            AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo);
        else
        {
            if (FActorRepListRefView* RepList =
                AlwaysRelevantStreamingLevelActors.Find(ActorInfo.StreamingLevelName))
            {
                FActorRepListRefView NewList;
                for (FActorRepListType Actor : *RepList)
                    if (Actor != ActorInfo.Actor)
                        NewList.ConditionalAdd(Actor);
                *RepList = NewList;
            }
        }
        break;

    default:
        GridNode->RemoveActor_Dormancy(ActorInfo);
        break;
    }
}

EClassRepPolicy UDAReplicationGraph::GetMappingPolicy(const UClass* InClass)
{
    const UClass* Class = InClass;
    while (Class)
    {
        EClassRepPolicy* Found =
            const_cast<TClassMap<EClassRepPolicy>&>(ClassRepPolicies).Get(
                const_cast<UClass*>(Class));
        if (Found) return *Found;
        Class = Class->GetSuperClass();
    }
    return EClassRepPolicy::NotRouted;
}

UDAReplicationGraphNode_AlwaysRelevant_ForConnection*
UDAReplicationGraph::GetAlwaysRelevantNode(APlayerController* PlayerController)
{
    if (!PlayerController) return nullptr;

    UNetConnection* NetConnection = PlayerController->NetConnection;
    if (!NetConnection) return nullptr;

    UNetReplicationGraphConnection* GraphConnection =
        FindOrAddConnectionManager(NetConnection);
    if (!GraphConnection) return nullptr;

    for (UReplicationGraphNode* Node : GraphConnection->GetConnectionGraphNodes())
    {
        if (UDAReplicationGraphNode_AlwaysRelevant_ForConnection* ARNode =
            Cast<UDAReplicationGraphNode_AlwaysRelevant_ForConnection>(Node))
            return ARNode;
    }

    return nullptr;
}

// ─────────────────────────────────────────────────────────────────
// UDAReplicationGraphNode_AlwaysRelevant_ForConnection
// ─────────────────────────────────────────────────────────────────

void UDAReplicationGraphNode_AlwaysRelevant_ForConnection::GatherActorListsForConnection(
    const FConnectionGatherActorListParameters& Params)
{
    Super::GatherActorListsForConnection(Params);

    for (const FNetViewer& Viewer : Params.Viewers)
    {
        if (AActor* ViewTarget = Viewer.ViewTarget)
            ReplicationActorList.ConditionalAdd(ViewTarget);

        if (AActor* InViewer = Viewer.InViewer)
            ReplicationActorList.ConditionalAdd(InViewer);
    }

    if (ReplicationActorList.Num() > 0)
        Params.OutGatheredReplicationLists.AddReplicationActorList(ReplicationActorList);

    UDAReplicationGraph* RepGraph = CastChecked<UDAReplicationGraph>(GetOuter());
    FPerConnectionActorInfoMap& ConnectionActorInfoMap = Params.ConnectionManager.ActorInfoMap;
    TMap<FName, FActorRepListRefView>& StreamingLevelActors =
        RepGraph->AlwaysRelevantStreamingLevelActors;

    for (int32 Idx = AlwaysRelevantStreamingLevels.Num() - 1; Idx >= 0; --Idx)
    {
        FName StreamingLevel = AlwaysRelevantStreamingLevels[Idx];
        FActorRepListRefView* ListPtr = StreamingLevelActors.Find(StreamingLevel);

        if (!ListPtr)
        {
            AlwaysRelevantStreamingLevels.RemoveAtSwap(Idx, 1, EAllowShrinking::No);
            continue;
        }

        FActorRepListRefView& RepList = *ListPtr;
        if (RepList.Num() > 0)
        {
            bool bAllDormant = true;
            for (FActorRepListType Actor : RepList)
            {
                FConnectionReplicationActorInfo& ConnectionActorInfo =
                    ConnectionActorInfoMap.FindOrAdd(Actor);
                if (!ConnectionActorInfo.bDormantOnConnection)
                {
                    bAllDormant = false;
                    break;
                }
            }

            if (bAllDormant)
                AlwaysRelevantStreamingLevels.RemoveAtSwap(Idx, 1, EAllowShrinking::No);
            else
                Params.OutGatheredReplicationLists.AddReplicationActorList(RepList);
        }
    }
}


void UDAReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityAdd(
    FName LevelName, UWorld* LevelWorld)
{
    AlwaysRelevantStreamingLevels.Add(LevelName);
}

void UDAReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityRemove(
    FName LevelName)
{
    AlwaysRelevantStreamingLevels.Remove(LevelName);
}

void UDAReplicationGraphNode_AlwaysRelevant_ForConnection::ResetGameWorldState()
{
    AlwaysRelevantStreamingLevels.Empty();
}
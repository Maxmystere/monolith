// SPDX-License-Identifier: MIT
// Reflected fixture types for the instanced-struct path tests
// (MonolithInstancedStructPathTests.cpp). Test-only: nothing else references them, and the
// asset class is hidden from class pickers. UHT does not allow these declarations inside
// #if WITH_DEV_AUTOMATION_TESTS, so they are always compiled into this editor module.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/StaticMesh.h"
#include "StructUtils/InstancedStruct.h"
#include "MonolithInstancedStructTestTypes.generated.h"

USTRUCT()
struct FMonolithTestFragmentBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Weight = 0;
};

USTRUCT()
struct FMonolithTestMeshFragment : public FMonolithTestFragmentBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	TSoftObjectPtr<UStaticMesh> EquipmentMesh;

	UPROPERTY(EditAnywhere, Category = "Test")
	TObjectPtr<UStaticMesh> PreviewMesh;

	UPROPERTY(EditAnywhere, Category = "Test")
	FName Socket;
};

/** Holds TArray<TInstancedStruct<FMonolithTestFragmentBase>>, the layout of an item DataAsset's fragment list. */
UCLASS(NotBlueprintable, HideDropdown, meta = (Hidden))
class UMonolithInstancedStructTestAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<TInstancedStruct<FMonolithTestFragmentBase>> Fragments;
};

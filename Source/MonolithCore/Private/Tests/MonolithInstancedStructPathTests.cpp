// SPDX-License-Identifier: MIT
// =============================================================================
// MonolithInstancedStructPathTests.cpp
//
// Reflection paths through TInstancedStruct / FInstancedStruct, on a DataAsset holding
// TArray<TInstancedStruct<FMonolithTestFragmentBase>> Fragments:
//   [0] FMonolithTestFragmentBase{Weight=1}
//   [1] FMonolithTestMeshFragment{Weight=2, Socket=Hand}
//   [2] empty
//
//   - Monolith.Core.InstancedStructPaths.ReadNestedField
//       get_cdo_properties emits the held struct with __struct; a dry run on a nested
//       field reports the live value as "current".
//   - Monolith.Core.InstancedStructPaths.WriteNestedSoftObject
//       set_property_at_path Fragments[1].EquipmentMesh: dry run leaves the asset alone,
//       the real write sets the soft path.
//   - Monolith.Core.InstancedStructPaths.WriteWholeElement
//       Fragments[0] = /Script/...MonolithTestMeshFragment(...): the dry run's "current" is
//       the live element in the same ImportText form (not "None"); the write retypes it.
//   - Monolith.Core.InstancedStructPaths.EmptyElementError
//       a path through the empty element fails with a clear error.
//   - Monolith.Core.InstancedStructPaths.SetCdoPropertiesTree
//       set_cdo_properties (the bulk_fill walker) writes instanced elements from JSON objects
//       with __struct, refuses a __struct outside the base, and its dry run changes nothing.
//   - Monolith.Core.InstancedStructPaths.ReadWriteRoundTrip
//       what get_cdo_properties returns for Fragments (an empty element as {}, a null hard
//       reference as "None") writes back through set_cdo_properties without errors.
//
// The asset lives in an in-memory package and is never saved.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"

#include "MonolithToolRegistry.h"
#include "MonolithInstancedStructTestTypes.h"

namespace MonolithInstancedStructPathTests
{
	static const TCHAR* MeshFragmentPath = TEXT("/Script/MonolithCore.MonolithTestMeshFragment");
	static const TCHAR* BaseFragmentPath = TEXT("/Script/MonolithCore.MonolithTestFragmentBase");
	static const TCHAR* Cube = TEXT("/Engine/BasicShapes/Cube.Cube");

	struct FFixture
	{
		UPackage* Package = nullptr;
		UMonolithInstancedStructTestAsset* Asset = nullptr;
		FString Path;

		FFixture()
		{
			const FString Name = FString::Printf(TEXT("DA_InstancedFixture_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
			Path = FString::Printf(TEXT("/Game/Tests/Monolith/InstancedStruct/%s"), *Name);
			Package = CreatePackage(*Path);
			Asset = NewObject<UMonolithInstancedStructTestAsset>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);

			FMonolithTestFragmentBase Base;
			Base.Weight = 1;
			FMonolithTestMeshFragment Mesh;
			Mesh.Weight = 2;
			Mesh.Socket = TEXT("Hand");
			// InitializeAs with the explicit type: Make(Mesh) can bind to the base overload and slice.
			Asset->Fragments.AddDefaulted_GetRef().InitializeAs<FMonolithTestFragmentBase>(Base);
			Asset->Fragments.AddDefaulted_GetRef().InitializeAs<FMonolithTestMeshFragment>(Mesh);
			Asset->Fragments.AddDefaulted();
		}

		~FFixture()
		{
			if (Asset)
			{
				Asset->ClearFlags(RF_Standalone | RF_Public);
				Asset->MarkAsGarbage();
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}

		const FMonolithTestMeshFragment* MeshAt(int32 Index) const
		{
			return Asset->Fragments.IsValidIndex(Index) ? Asset->Fragments[Index].GetPtr<FMonolithTestMeshFragment>() : nullptr;
		}
	};

	static FMonolithActionResult Run(const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), Action, Params);
	}

	static FMonolithActionResult SetAtPath(const FFixture& F, const FString& Path, const FString& Value, bool bDryRun)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), F.Path);
		P->SetStringField(TEXT("path"), Path);
		P->SetStringField(TEXT("value"), Value);
		P->SetBoolField(TEXT("dry_run"), bDryRun);
		return Run(TEXT("set_property_at_path"), P);
	}

	static FString FirstWriteField(const FMonolithActionResult& R, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Writes = nullptr;
		if (R.bSuccess && R.Result.IsValid() && R.Result->TryGetArrayField(TEXT("field_writes"), Writes) && Writes && Writes->Num() > 0)
		{
			return (*Writes)[0]->AsObject()->GetStringField(Field);
		}
		return TEXT("<no field_writes>");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInstancedStructReadNestedTest,
	"Monolith.Core.InstancedStructPaths.ReadNestedField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInstancedStructReadNestedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithInstancedStructPathTests;
	FFixture F;

	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("asset_path"), F.Path);
	TArray<TSharedPtr<FJsonValue>> Names;
	Names.Add(MakeShared<FJsonValueString>(TEXT("Fragments")));
	P->SetArrayField(TEXT("property_names"), Names);
	const FMonolithActionResult R = Run(TEXT("get_cdo_properties"), P);
	if (TestTrue(FString::Printf(TEXT("get_cdo_properties succeeds (%s)"), *R.ErrorMessage), R.bSuccess))
	{
		const TArray<TSharedPtr<FJsonValue>>& Props = R.Result->GetArrayField(TEXT("properties"));
		if (TestEqual(TEXT("One property returned"), Props.Num(), 1))
		{
			const TArray<TSharedPtr<FJsonValue>>& Elements = Props[0]->AsObject()->GetArrayField(TEXT("value"));
			if (TestEqual(TEXT("Three elements"), Elements.Num(), 3))
			{
				const TSharedPtr<FJsonObject> Second = Elements[1]->AsObject();
				TestEqual(TEXT("Held struct type is reported"), Second->GetStringField(TEXT("__struct")), FString(MeshFragmentPath));
				TestEqual(TEXT("Nested field is read"), Second->GetStringField(TEXT("Socket")), FString(TEXT("Hand")));
			}
		}
	}

	const FMonolithActionResult Dry = SetAtPath(F, TEXT("Fragments[1].Socket"), TEXT("Back"), /*bDryRun=*/true);
	TestTrue(FString::Printf(TEXT("Dry run through the instanced struct resolves (%s)"), *Dry.ErrorMessage), Dry.bSuccess);
	TestEqual(TEXT("Dry run reports the live value as current"), FirstWriteField(Dry, TEXT("current")), FString(TEXT("Hand")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInstancedStructWriteNestedTest,
	"Monolith.Core.InstancedStructPaths.WriteNestedSoftObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInstancedStructWriteNestedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithInstancedStructPathTests;
	FFixture F;

	const FMonolithActionResult Dry = SetAtPath(F, TEXT("Fragments[1].EquipmentMesh"), Cube, /*bDryRun=*/true);
	TestTrue(FString::Printf(TEXT("Dry run succeeds (%s)"), *Dry.ErrorMessage), Dry.bSuccess);
	TestEqual(TEXT("Dry run reports ok"), FirstWriteField(Dry, TEXT("ok")), FString(TEXT("true")));
	TestTrue(TEXT("Dry run leaves the asset alone"), F.MeshAt(1) && F.MeshAt(1)->EquipmentMesh.IsNull());

	const FMonolithActionResult Real = SetAtPath(F, TEXT("Fragments[1].EquipmentMesh"), Cube, /*bDryRun=*/false);
	TestTrue(FString::Printf(TEXT("Write succeeds (%s)"), *Real.ErrorMessage), Real.bSuccess);
	TestEqual(TEXT("Soft path written"), F.MeshAt(1) ? F.MeshAt(1)->EquipmentMesh.ToSoftObjectPath().ToString() : FString(), FString(Cube));
	TestEqual(TEXT("Sibling field untouched"), F.MeshAt(1) ? F.MeshAt(1)->Socket : NAME_None, FName(TEXT("Hand")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInstancedStructWholeElementTest,
	"Monolith.Core.InstancedStructPaths.WriteWholeElement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInstancedStructWholeElementTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithInstancedStructPathTests;
	FFixture F;
	const FString NewElement = FString::Printf(TEXT("%s(Weight=5,Socket=\"Hip\")"), MeshFragmentPath);

	const FMonolithActionResult Dry = SetAtPath(F, TEXT("Fragments[0]"), NewElement, /*bDryRun=*/true);
	TestTrue(FString::Printf(TEXT("Dry run succeeds (%s)"), *Dry.ErrorMessage), Dry.bSuccess);
	const FString Current = FirstWriteField(Dry, TEXT("current"));
	TestTrue(FString::Printf(TEXT("Dry run 'current' is the live element as ImportText (%s)"), *Current),
		Current.StartsWith(BaseFragmentPath) && Current.Contains(TEXT("Weight=1")));
	TestNull(TEXT("Dry run leaves the element type alone"), F.MeshAt(0));

	const FMonolithActionResult Real = SetAtPath(F, TEXT("Fragments[0]"), NewElement, /*bDryRun=*/false);
	TestTrue(FString::Printf(TEXT("Write succeeds (%s)"), *Real.ErrorMessage), Real.bSuccess);
	if (TestNotNull(TEXT("Element retyped to the mesh fragment"), F.MeshAt(0)))
	{
		TestEqual(TEXT("Weight written"), F.MeshAt(0)->Weight, 5);
		TestEqual(TEXT("Socket written"), F.MeshAt(0)->Socket, FName(TEXT("Hip")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInstancedStructEmptyElementTest,
	"Monolith.Core.InstancedStructPaths.EmptyElementError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInstancedStructEmptyElementTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithInstancedStructPathTests;
	FFixture F;
	for (const bool bDryRun : { true, false })
	{
		const FMonolithActionResult R = SetAtPath(F, TEXT("Fragments[2].Weight"), TEXT("3"), bDryRun);
		TestFalse(TEXT("A path through an empty element fails"), R.bSuccess);
		TestTrue(FString::Printf(TEXT("The error says the element is empty and how to fill it (%s)"), *R.ErrorMessage),
			R.ErrorMessage.Contains(TEXT("empty")) && R.ErrorMessage.Contains(TEXT("Fragments[2]")));
	}
	TestFalse(TEXT("The empty element stays empty"), F.Asset->Fragments[2].IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInstancedStructSetCdoPropertiesTest,
	"Monolith.Core.InstancedStructPaths.SetCdoPropertiesTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInstancedStructSetCdoPropertiesTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithInstancedStructPathTests;
	FFixture F;

	auto MakeParams = [&F](const TCHAR* SecondStruct, bool bDryRun)
	{
		TSharedPtr<FJsonObject> First = MakeShared<FJsonObject>();
		First->SetStringField(TEXT("__struct"), BaseFragmentPath);
		First->SetNumberField(TEXT("Weight"), 7);
		TSharedPtr<FJsonObject> Second = MakeShared<FJsonObject>();
		Second->SetStringField(TEXT("__struct"), SecondStruct);
		Second->SetNumberField(TEXT("Weight"), 8);
		Second->SetStringField(TEXT("EquipmentMesh"), TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		TArray<TSharedPtr<FJsonValue>> Elements;
		Elements.Add(MakeShared<FJsonValueObject>(First));
		Elements.Add(MakeShared<FJsonValueObject>(Second));
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetArrayField(TEXT("Fragments"), Elements);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), F.Path);
		P->SetObjectField(TEXT("properties"), Properties);
		P->SetBoolField(TEXT("dry_run"), bDryRun);
		return P;
	};

	const FMonolithActionResult Dry = Run(TEXT("set_cdo_properties"), MakeParams(MeshFragmentPath, /*bDryRun=*/true));
	TestTrue(FString::Printf(TEXT("Dry run succeeds (%s)"), *Dry.ErrorMessage), Dry.bSuccess);
	if (Dry.bSuccess)
	{
		TestEqual(TEXT("Dry run reports no errors"), static_cast<int32>(Dry.Result->GetNumberField(TEXT("errors"))), 0);
	}
	TestEqual(TEXT("Dry run leaves the array alone"), F.Asset->Fragments.Num(), 3);

	const FMonolithActionResult Real = Run(TEXT("set_cdo_properties"), MakeParams(MeshFragmentPath, /*bDryRun=*/false));
	TestTrue(FString::Printf(TEXT("Write succeeds (%s)"), *Real.ErrorMessage), Real.bSuccess);
	if (TestEqual(TEXT("Array rebuilt with two elements"), F.Asset->Fragments.Num(), 2))
	{
		const FMonolithTestFragmentBase* First = F.Asset->Fragments[0].GetPtr<FMonolithTestFragmentBase>();
		TestTrue(TEXT("First element is the base type, Weight 7"), First && F.Asset->Fragments[0].GetScriptStruct() == FMonolithTestFragmentBase::StaticStruct() && First->Weight == 7);
		TestTrue(TEXT("Second element is the mesh fragment with its mesh"),
			F.MeshAt(1) && F.MeshAt(1)->Weight == 8 && F.MeshAt(1)->EquipmentMesh.ToSoftObjectPath().ToString() == TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	}

	// A __struct outside TInstancedStruct<FMonolithTestFragmentBase> is refused.
	const FMonolithActionResult Bad = Run(TEXT("set_cdo_properties"), MakeParams(TEXT("/Script/CoreUObject.Vector"), /*bDryRun=*/true));
	bool bReported = false;
	const TArray<TSharedPtr<FJsonValue>>* Writes = nullptr;
	if (Bad.bSuccess && Bad.Result->TryGetArrayField(TEXT("field_writes"), Writes) && Writes)
	{
		for (const TSharedPtr<FJsonValue>& W : *Writes)
		{
			FString Reason;
			bReported |= W->AsObject()->TryGetStringField(TEXT("reason"), Reason) && Reason.Contains(TEXT("is not a MonolithTestFragmentBase"));
		}
	}
	TestTrue(TEXT("__struct outside the base struct is refused by name"), bReported);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInstancedStructRoundTripTest,
	"Monolith.Core.InstancedStructPaths.ReadWriteRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInstancedStructRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithInstancedStructPathTests;
	FFixture F;

	TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("asset_path"), F.Path);
	TArray<TSharedPtr<FJsonValue>> Names;
	Names.Add(MakeShared<FJsonValueString>(TEXT("Fragments")));
	ReadParams->SetArrayField(TEXT("property_names"), Names);
	const FMonolithActionResult Read = Run(TEXT("get_cdo_properties"), ReadParams);
	if (!TestTrue(FString::Printf(TEXT("get_cdo_properties succeeds (%s)"), *Read.ErrorMessage), Read.bSuccess))
	{
		return true;
	}
	const TArray<TSharedPtr<FJsonValue>>& Props = Read.Result->GetArrayField(TEXT("properties"));
	if (!TestEqual(TEXT("One property returned"), Props.Num(), 1))
	{
		return true;
	}

	// Write back exactly what was read: the empty element and the null references included.
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetField(TEXT("Fragments"), Props[0]->AsObject()->TryGetField(TEXT("value")));
	for (const bool bDryRun : { true, false })
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), F.Path);
		P->SetObjectField(TEXT("properties"), Properties);
		P->SetBoolField(TEXT("dry_run"), bDryRun);
		const FMonolithActionResult R = Run(TEXT("set_cdo_properties"), P);
		if (TestTrue(FString::Printf(TEXT("Round trip succeeds (%s)"), *R.ErrorMessage), R.bSuccess))
		{
			TestEqual(TEXT("Round trip reports no errors"), static_cast<int32>(R.Result->GetNumberField(TEXT("errors"))), 0);
		}
	}

	if (TestEqual(TEXT("Three elements kept"), F.Asset->Fragments.Num(), 3))
	{
		const FMonolithTestFragmentBase* First = F.Asset->Fragments[0].GetPtr<FMonolithTestFragmentBase>();
		TestTrue(TEXT("First element is still the base type, Weight 1"),
			First && F.Asset->Fragments[0].GetScriptStruct() == FMonolithTestFragmentBase::StaticStruct() && First->Weight == 1);
		TestTrue(TEXT("Second element is still the mesh fragment, with null references"),
			F.MeshAt(1) && F.MeshAt(1)->Weight == 2 && F.MeshAt(1)->Socket == FName(TEXT("Hand"))
			&& F.MeshAt(1)->EquipmentMesh.IsNull() && F.MeshAt(1)->PreviewMesh == nullptr);
		TestFalse(TEXT("Empty element stays empty"), F.Asset->Fragments[2].IsValid());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

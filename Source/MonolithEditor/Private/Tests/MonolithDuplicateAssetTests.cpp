// =============================================================================
// MonolithDuplicateAssetTests.cpp
//
// editor.duplicate_asset — generic duplication through IAssetTools.
//
//   - Monolith.Editor.DuplicateAsset.Types
//       duplicates a StaticMesh, a DataAsset and a Texture2D (single form and batch form),
//       checks each copy's class and that its package is saved to disk.
//   - Monolith.Editor.DuplicateAsset.Refusals
//       neither/both forms, missing source, an existing destination without overwrite,
//       two pairs with one destination: each refuses with -32602 and writes nothing.
//   - Monolith.Editor.DuplicateAsset.OverwriteAndNoSave
//       overwrite=true replaces an existing destination; save=false leaves the copy unsaved.
//
// Every asset is created under /Game/Tests/Monolith/DuplicateAsset/<run id>/ and deleted,
// files included, at the end of the test.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Engine/PrimaryAssetLabel.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"

namespace MonolithDuplicateAssetTests
{
	static const TCHAR* EngineMesh = TEXT("/Engine/BasicShapes/Cube");
	static const TCHAR* EngineTexture = TEXT("/Engine/EngineResources/DefaultTexture");

	static FMonolithActionResult Run(const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("duplicate_asset"), Params);
	}

	static TSharedPtr<FJsonObject> Single(const FString& Source, const FString& Dest)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_path"), Source);
		Params->SetStringField(TEXT("dest_path"), Dest);
		return Params;
	}

	static TSharedPtr<FJsonValue> Pair(const FString& Source, const FString& Dest)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("source"), Source);
		Obj->SetStringField(TEXT("dest"), Dest);
		return MakeShared<FJsonValueObject>(Obj);
	}

	/** A run folder unique to this test run, cleaned (packages and files) on destruction. */
	struct FScratchFolder
	{
		FString Root;

		explicit FScratchFolder(const TCHAR* TestName)
		{
			Root = FString::Printf(TEXT("/Game/Tests/Monolith/DuplicateAsset/%s_%s"),
				TestName, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
		}

		FString Path(const TCHAR* AssetName) const { return Root / AssetName; }

		~FScratchFolder()
		{
			IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			TArray<FAssetData> Assets;
			AR.GetAssetsByPath(FName(*Root), Assets, /*bRecursive=*/true, /*bIncludeOnlyOnDiskAssets=*/false);

			TArray<TSharedPtr<FJsonValue>> Paths;
			for (const FAssetData& Asset : Assets)
			{
				Paths.Add(MakeShared<FJsonValueString>(Asset.PackageName.ToString()));
			}
			if (Paths.Num() > 0)
			{
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetArrayField(TEXT("asset_paths"), Paths);
				Params->SetBoolField(TEXT("force"), true);
				FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("delete_assets"), Params);
			}

			const FString Dir = FPackageName::LongPackageNameToFilename(Root + TEXT("/"));
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists=*/false, /*Tree=*/true);
		}
	};

	/** Creates and saves a PrimaryAssetLabel (a concrete UPrimaryDataAsset) at PackageName. */
	static bool MakeSavedDataAsset(const FString& PackageName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		UPrimaryAssetLabel* Asset = NewObject<UPrimaryAssetLabel>(Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone);
		if (!Asset)
		{
			return false;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Packages;
		Packages.Add(MakeShared<FJsonValueString>(PackageName));
		Params->SetArrayField(TEXT("packages"), Packages);
		const FMonolithActionResult Res = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("save_packages"), Params);
		bool bOk = false;
		return Res.bSuccess && Res.Result.IsValid() && Res.Result->TryGetBoolField(TEXT("ok"), bOk) && bOk;
	}

	static TSharedPtr<FJsonObject> FirstRow(const FMonolithActionResult& Res)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Res.bSuccess && Res.Result.IsValid() && Res.Result->TryGetArrayField(TEXT("results"), Rows) && Rows && Rows->Num() > 0)
		{
			return (*Rows)[0]->AsObject();
		}
		return nullptr;
	}

	static bool OnDisk(const FString& PackageName)
	{
		return FPackageName::DoesPackageExist(PackageName);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDuplicateAssetTypesTest,
	"Monolith.Editor.DuplicateAsset.Types",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDuplicateAssetTypesTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDuplicateAssetTests;
	FScratchFolder Folder(TEXT("Types"));

	const FString DataSource = Folder.Path(TEXT("DA_Source"));
	if (!TestTrue(TEXT("DataAsset source created and saved"), MakeSavedDataAsset(DataSource)))
	{
		return false;
	}

	// Single form: StaticMesh.
	const FString MeshDest = Folder.Path(TEXT("SM_Copy"));
	const FMonolithActionResult MeshRes = Run(Single(EngineMesh, MeshDest));
	TestTrue(TEXT("StaticMesh duplicate succeeds"), MeshRes.bSuccess);
	if (const TSharedPtr<FJsonObject> Row = FirstRow(MeshRes))
	{
		TestEqual(TEXT("StaticMesh copy class"), Row->GetStringField(TEXT("class")), UStaticMesh::StaticClass()->GetPathName());
		TestTrue(TEXT("StaticMesh copy saved"), Row->GetBoolField(TEXT("saved")));
	}
	TestTrue(TEXT("StaticMesh copy is on disk"), OnDisk(MeshDest));
	TestNotNull(TEXT("StaticMesh copy loads as UStaticMesh"), LoadObject<UStaticMesh>(nullptr, *(MeshDest + TEXT(".SM_Copy"))));

	// Batch form: DataAsset + Texture2D in one call (object-path spelling on the source).
	const FString DataDest = Folder.Path(TEXT("DA_Copy"));
	const FString TexDest = Folder.Path(TEXT("T_Copy"));
	TSharedPtr<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Pairs;
	Pairs.Add(Pair(DataSource + TEXT(".DA_Source"), DataDest));
	Pairs.Add(Pair(EngineTexture, TexDest));
	BatchParams->SetArrayField(TEXT("pairs"), Pairs);

	const FMonolithActionResult BatchRes = Run(BatchParams);
	TestTrue(TEXT("Batch succeeds"), BatchRes.bSuccess);
	if (BatchRes.bSuccess)
	{
		TestTrue(TEXT("Batch ok"), BatchRes.Result->GetBoolField(TEXT("ok")));
		TestEqual(TEXT("Batch duplicated count"), static_cast<int32>(BatchRes.Result->GetNumberField(TEXT("duplicated"))), 2);
		const TArray<TSharedPtr<FJsonValue>>& Rows = BatchRes.Result->GetArrayField(TEXT("results"));
		if (TestEqual(TEXT("Batch row count"), Rows.Num(), 2))
		{
			TestEqual(TEXT("DataAsset copy class"), Rows[0]->AsObject()->GetStringField(TEXT("class")), UPrimaryAssetLabel::StaticClass()->GetPathName());
			TestEqual(TEXT("Texture copy class"), Rows[1]->AsObject()->GetStringField(TEXT("class")), UTexture2D::StaticClass()->GetPathName());
		}
	}
	TestTrue(TEXT("DataAsset copy is on disk"), OnDisk(DataDest));
	TestTrue(TEXT("Texture copy is on disk"), OnDisk(TexDest));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDuplicateAssetRefusalsTest,
	"Monolith.Editor.DuplicateAsset.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDuplicateAssetRefusalsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDuplicateAssetTests;
	constexpr int32 InvalidParams = FMonolithJsonUtils::ErrInvalidParams;
	FScratchFolder Folder(TEXT("Refusals"));

	{
		const FMonolithActionResult Res = Run(MakeShared<FJsonObject>());
		TestFalse(TEXT("No form refuses"), Res.bSuccess);
		TestEqual(TEXT("No form is invalid params"), Res.ErrorCode, InvalidParams);
	}
	{
		TSharedPtr<FJsonObject> Params = Single(EngineMesh, Folder.Path(TEXT("SM_A")));
		TArray<TSharedPtr<FJsonValue>> Pairs;
		Pairs.Add(Pair(EngineMesh, Folder.Path(TEXT("SM_B"))));
		Params->SetArrayField(TEXT("pairs"), Pairs);
		const FMonolithActionResult Res = Run(Params);
		TestFalse(TEXT("Both forms refuse"), Res.bSuccess);
		TestEqual(TEXT("Both forms is invalid params"), Res.ErrorCode, InvalidParams);
	}
	{
		const FString Dest = Folder.Path(TEXT("SM_FromMissing"));
		const FMonolithActionResult Res = Run(Single(TEXT("/Game/Tests/Monolith/DuplicateAsset/DoesNotExist"), Dest));
		TestFalse(TEXT("Missing source refuses"), Res.bSuccess);
		TestEqual(TEXT("Missing source is invalid params"), Res.ErrorCode, InvalidParams);
		TestTrue(TEXT("Missing source error names it"), Res.ErrorMessage.Contains(TEXT("not found")));
		TestFalse(TEXT("Nothing written for a missing source"), OnDisk(Dest));
	}
	{
		// Existing destination: create it, then duplicate onto it without overwrite.
		const FString Dest = Folder.Path(TEXT("SM_Existing"));
		TestTrue(TEXT("Seed copy succeeds"), Run(Single(EngineMesh, Dest)).bSuccess);
		const FMonolithActionResult Res = Run(Single(EngineTexture, Dest));
		TestFalse(TEXT("Existing destination refuses"), Res.bSuccess);
		TestEqual(TEXT("Existing destination is invalid params"), Res.ErrorCode, InvalidParams);
		TestTrue(TEXT("Error points at overwrite"), Res.ErrorMessage.Contains(TEXT("overwrite")));
		TestNotNull(TEXT("Existing destination is untouched (still a mesh)"),
			LoadObject<UStaticMesh>(nullptr, *(Dest + TEXT(".SM_Existing"))));
	}
	{
		// Two pairs, one destination: refused as a whole, so the first pair is not written either.
		const FString Dest = Folder.Path(TEXT("SM_Twice"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Pairs;
		Pairs.Add(Pair(EngineMesh, Dest));
		Pairs.Add(Pair(EngineTexture, Dest));
		Params->SetArrayField(TEXT("pairs"), Pairs);
		const FMonolithActionResult Res = Run(Params);
		TestFalse(TEXT("Shared destination refuses"), Res.bSuccess);
		TestEqual(TEXT("Shared destination is invalid params"), Res.ErrorCode, InvalidParams);
		TestFalse(TEXT("Nothing written for a refused batch"), OnDisk(Dest));
	}
	{
		const FMonolithActionResult Res = Run(Single(EngineMesh, TEXT("Game/NoLeadingSlash/SM_Bad")));
		TestFalse(TEXT("Invalid destination refuses"), Res.bSuccess);
		TestEqual(TEXT("Invalid destination is invalid params"), Res.ErrorCode, InvalidParams);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDuplicateAssetOverwriteTest,
	"Monolith.Editor.DuplicateAsset.OverwriteAndNoSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDuplicateAssetOverwriteTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDuplicateAssetTests;
	FScratchFolder Folder(TEXT("Overwrite"));

	const FString Dest = Folder.Path(TEXT("T_Target"));
	TestTrue(TEXT("Seed copy (mesh) succeeds"), Run(Single(EngineMesh, Dest)).bSuccess);

	TSharedPtr<FJsonObject> Params = Single(EngineTexture, Dest);
	Params->SetBoolField(TEXT("overwrite"), true);
	const FMonolithActionResult Res = Run(Params);
	TestTrue(TEXT("Overwrite succeeds"), Res.bSuccess);
	if (const TSharedPtr<FJsonObject> Row = FirstRow(Res))
	{
		TestTrue(TEXT("Row reports overwritten"), Row->GetBoolField(TEXT("overwritten")));
		TestEqual(TEXT("Overwritten asset is now a texture"), Row->GetStringField(TEXT("class")), UTexture2D::StaticClass()->GetPathName());
	}
	TestTrue(TEXT("Overwritten package is on disk"), OnDisk(Dest));

	const FString Unsaved = Folder.Path(TEXT("SM_Unsaved"));
	TSharedPtr<FJsonObject> NoSave = Single(EngineMesh, Unsaved);
	NoSave->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult NoSaveRes = Run(NoSave);
	TestTrue(TEXT("save=false succeeds"), NoSaveRes.bSuccess);
	if (const TSharedPtr<FJsonObject> Row = FirstRow(NoSaveRes))
	{
		TestFalse(TEXT("Row reports not saved"), Row->GetBoolField(TEXT("saved")));
	}
	TestFalse(TEXT("save=false leaves nothing on disk"), OnDisk(Unsaved));
	UPackage* InMemory = FindPackage(nullptr, *Unsaved);
	TestTrue(TEXT("save=false copy exists in memory, dirty"), InMemory && InMemory->IsDirty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

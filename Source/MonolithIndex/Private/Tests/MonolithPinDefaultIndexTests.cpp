// =============================================================================
// MonolithPinDefaultIndexTests.cpp
//
// project search over Blueprint pin defaults (schema v4).
//
//   - Monolith.Index.PinDefaults.SearchFindsLiterals
//       a Blueprint with a literal SocketName on K2_AttachToComponent and a literal string on
//       PrintString, indexed by FBlueprintIndexer into a temp DB: search finds both, with
//       asset, graph, node and pin; a pin left at its default is not indexed. The JSON the
//       project search action builds from those hits (match_kind/graph/node/pin) is not
//       covered here: the action reads the editor subsystem's database.
//   - Monolith.Index.PinDefaults.MigrationFromV3
//       reopening a v3 database stamps the current version and marks the deep-index
//       checkpoint of Blueprint-class assets stale, so the batched stale re-index (or a
//       resumed full index) picks them up; saved hashes and poison assets are left alone.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformFileManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "AssetRegistry/AssetData.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "SQLiteDatabase.h"
#include "UObject/Package.h"

#include "Indexers/BlueprintIndexer.h"
#include "MonolithIndexDatabase.h"
#include "MonolithToolRegistry.h"

namespace MonolithPinDefaultIndexTests
{
	static const TCHAR* SocketLiteral = TEXT("MonolithPinFixtureSocket");
	static const TCHAR* PrintLiteral = TEXT("MonolithPinFixturePrint");

	struct FTempDb
	{
		FMonolithIndexDatabase Database;
		FString DbPath;

		bool Open()
		{
			DbPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithTests"),
				FString::Printf(TEXT("PinDefaults_%s.db"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			return Database.Open(DbPath);
		}

		~FTempDb()
		{
			Database.Close();
			if (!DbPath.IsEmpty())
			{
				FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
			}
		}
	};

	static FMonolithActionResult RunBlueprint(const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), Action, Params);
	}

	static FString AddNode(const FString& AssetPath, const TCHAR* FunctionName, const TCHAR* TargetClass)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), AssetPath);
		P->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		P->SetStringField(TEXT("function_name"), FunctionName);
		P->SetStringField(TEXT("target_class"), TargetClass);
		const FMonolithActionResult R = RunBlueprint(TEXT("add_node"), P);
		return R.bSuccess ? R.Result->GetStringField(TEXT("id")) : FString();
	}

	static bool SetPinDefault(const FString& AssetPath, const FString& NodeId, const TCHAR* Pin, const TCHAR* Value)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), AssetPath);
		P->SetStringField(TEXT("node_id"), NodeId);
		P->SetStringField(TEXT("pin_name"), Pin);
		P->SetStringField(TEXT("value"), Value);
		return RunBlueprint(TEXT("set_pin_default"), P).bSuccess;
	}

	static const FSearchResult* FindHit(const TArray<FSearchResult>& Results, const TCHAR* Pin)
	{
		return Results.FindByPredicate([Pin](const FSearchResult& R) { return R.PinName == Pin; });
	}

	static int32 CountRows(FMonolithIndexDatabase& Db, const TCHAR* SQL)
	{
		FSQLitePreparedStatement Stmt;
		int64 Count = -1;
		if (Stmt.Create(*Db.GetRawDatabase(), SQL) && Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, Count);
		}
		return static_cast<int32>(Count);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPinDefaultSearchTest,
	"Monolith.Index.PinDefaults.SearchFindsLiterals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPinDefaultSearchTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPinDefaultIndexTests;

	// Fixture Blueprint (in memory, never saved).
	const FString Name = FString::Printf(TEXT("BP_PinDefaultsFixture_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString AssetPath = FString::Printf(TEXT("/Game/Tests/Monolith/PinDefaults/%s"), *Name);
	UPackage* Package = CreatePackage(*AssetPath);
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, FName(*Name),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	ON_SCOPE_EXIT
	{
		if (BP) { BP->ClearFlags(RF_Standalone | RF_Public); BP->MarkAsGarbage(); }
		if (Package) { Package->SetDirtyFlag(false); Package->MarkAsGarbage(); }
	};
	if (!TestNotNull(TEXT("Fixture Blueprint"), BP))
	{
		return false;
	}

	const FString AttachId = AddNode(AssetPath, TEXT("K2_AttachToComponent"), TEXT("SceneComponent"));
	const FString PrintId = AddNode(AssetPath, TEXT("PrintString"), TEXT("KismetSystemLibrary"));
	if (!TestFalse(TEXT("Attach node added"), AttachId.IsEmpty()) || !TestFalse(TEXT("PrintString node added"), PrintId.IsEmpty()))
	{
		return false;
	}
	TestTrue(TEXT("SocketName literal set"), SetPinDefault(AssetPath, AttachId, TEXT("SocketName"), SocketLiteral));
	TestTrue(TEXT("InString literal set"), SetPinDefault(AssetPath, PrintId, TEXT("InString"), PrintLiteral));

	// Index it into a temp DB.
	FTempDb Db;
	if (!TestTrue(TEXT("Temp index DB opens"), Db.Open()))
	{
		return false;
	}
	TestTrue(TEXT("Fresh DB has the pin_defaults tables"), Db.Database.HasPinDefaults());

	FIndexedAsset Asset;
	Asset.PackagePath = AssetPath;
	Asset.AssetName = Name;
	Asset.AssetClass = TEXT("Blueprint");
	Asset.ModuleName = TEXT("Game");
	const int64 AssetId = Db.Database.InsertAsset(Asset);
	FBlueprintIndexer Indexer;
	TestTrue(TEXT("Blueprint indexed"), AssetId > 0 && Indexer.IndexAsset(FAssetData(BP), BP, Db.Database, AssetId));

	TArray<FSearchResult> Results;
	FString Error;
	TestTrue(TEXT("Socket search succeeds"), Db.Database.FullTextSearch(SocketLiteral, 50, Results, Error) == EMonolithProjectSearchStatus::Succeeded);
	if (const FSearchResult* Hit = FindHit(Results, TEXT("SocketName")))
	{
		TestEqual(TEXT("Socket hit asset"), Hit->AssetPath, AssetPath);
		TestEqual(TEXT("Socket hit graph"), Hit->GraphName, FString(TEXT("EventGraph")));
		TestTrue(FString::Printf(TEXT("Socket hit node (%s)"), *Hit->NodeName), Hit->NodeName.Contains(TEXT("Attach")));
	}
	else
	{
		AddError(TEXT("No pin-default hit for the SocketName literal"));
	}

	TestTrue(TEXT("Print search succeeds"), Db.Database.FullTextSearch(PrintLiteral, 50, Results, Error) == EMonolithProjectSearchStatus::Succeeded);
	if (const FSearchResult* Hit = FindHit(Results, TEXT("InString")))
	{
		TestTrue(FString::Printf(TEXT("Print hit node (%s)"), *Hit->NodeName), Hit->NodeName.Contains(TEXT("Print")));
	}
	else
	{
		AddError(TEXT("No pin-default hit for the PrintString literal"));
	}

	// Only typed literals are stored: PrintString's untouched pins (bPrintToScreen, Duration, ...)
	// and the exec/self pins are not.
	TestEqual(TEXT("Exactly the two typed literals are indexed"), CountRows(Db.Database, TEXT("SELECT COUNT(*) FROM pin_defaults;")), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPinDefaultMigrationTest,
	"Monolith.Index.PinDefaults.MigrationFromV3",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPinDefaultMigrationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPinDefaultIndexTests;
	FTempDb Db;
	if (!TestTrue(TEXT("Temp index DB opens"), Db.Open()))
	{
		return false;
	}

	FIndexedAsset Blueprint;
	Blueprint.PackagePath = TEXT("/Game/Tests/Monolith/BP_MigrationFixture");
	Blueprint.AssetName = TEXT("BP_MigrationFixture");
	Blueprint.AssetClass = TEXT("Blueprint");
	Blueprint.SavedHash = TEXT("hash-blueprint");
	FIndexedAsset Mesh;
	Mesh.PackagePath = TEXT("/Game/Tests/Monolith/SM_MigrationFixture");
	Mesh.AssetName = TEXT("SM_MigrationFixture");
	Mesh.AssetClass = TEXT("StaticMesh");
	Mesh.SavedHash = TEXT("hash-mesh");
	FIndexedAsset Poison;
	Poison.PackagePath = TEXT("/Game/Tests/Monolith/BP_MigrationPoisonFixture");
	Poison.AssetName = TEXT("BP_MigrationPoisonFixture");
	Poison.AssetClass = TEXT("Blueprint");
	Poison.SavedHash = TEXT("hash-poison");
	const int64 BlueprintId = Db.Database.InsertAsset(Blueprint);
	const int64 PoisonId = Db.Database.InsertAsset(Poison);
	TestTrue(TEXT("Assets inserted"), BlueprintId > 0 && PoisonId > 0 && Db.Database.InsertAsset(Mesh) > 0);

	// The Blueprint was deep-indexed by an interrupted full index; the poison one hit the attempt limit.
	TestTrue(TEXT("Checkpoint written"), Db.Database.SetDeepIndexedHash(BlueprintId, TEXT("hash-blueprint")));
	for (int32 Attempt = 0; Attempt < MonolithMaxDeepIndexAttempts; ++Attempt)
	{
		TestTrue(TEXT("Attempt recorded"), Db.Database.BumpDeepIndexAttempts({ PoisonId }));
	}

	// Rewind to v3 and reopen: the upgrade an existing user gets.
	TestTrue(TEXT("Rewind to v3"), Db.Database.WriteMeta(TEXT("schema_version"), TEXT("3")));
	Db.Database.Close();
	if (!TestTrue(TEXT("Reopens"), Db.Database.Open(Db.DbPath)))
	{
		return false;
	}

	TestEqual(TEXT("Stamped current"), Db.Database.ReadMeta(TEXT("schema_version")),
		FString::FromInt(FMonolithIndexDatabase::CurrentSchemaVersion));
	// Clearing saved hashes would hand every Blueprint to the incremental pass, which
	// loads its whole delta in one call.
	TestEqual(TEXT("Saved hashes kept"),
		CountRows(Db.Database, TEXT("SELECT COUNT(*) FROM assets WHERE saved_hash IN ('hash-blueprint', 'hash-poison');")), 2);
	// An empty checkpoint reads as "no child rows": a resumed full index would re-index on
	// top of the old ones. A stale one re-queues the asset and clears them first.
	const TArray<FIndexedAsset> Stale = Db.Database.GetAssetsWithStaleDeepIndex();
	if (TestEqual(TEXT("Only the healthy Blueprint is stale"), Stale.Num(), 1))
	{
		TestEqual(TEXT("Stale row id"), Stale[0].Id, BlueprintId);
		TestEqual(TEXT("Stale row keeps its saved hash"), Stale[0].SavedHash, FString(TEXT("hash-blueprint")));
		TestTrue(TEXT("A resume re-queues the Blueprint"),
			MonolithDecideDeepIndexQueueEntry(Stale[0].DeepIndexedHash, Stale[0].DeepIndexAttempts, Stale[0].SavedHash)
				== EMonolithDeepIndexQueueDecision::Queue);
	}
	// What a finished deep index writes takes the row out of the stale set.
	TestTrue(TEXT("Checkpoint written after re-index"), Db.Database.SetDeepIndexedHash(BlueprintId, TEXT("hash-blueprint")));
	TestEqual(TEXT("Nothing stale after the re-index"), Db.Database.GetAssetsWithStaleDeepIndex().Num(), 0);
	TestEqual(TEXT("Other asset hashes kept"),
		CountRows(Db.Database, TEXT("SELECT COUNT(*) FROM assets WHERE asset_class = 'StaticMesh' AND saved_hash = 'hash-mesh';")), 1);
	TestTrue(TEXT("Pin tables present after migration"), Db.Database.HasPinDefaults());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

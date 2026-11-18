#include "MonolithEditorAssetActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "CoreGlobals.h"          // GIsRunningUnattendedScript
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace MonolithEditorAssetActionsPrivate
{
	/** One requested duplication, after validation. */
	struct FDuplicatePair
	{
		FString SourceInput;   // as the caller wrote it
		FString DestInput;
		FString DestPackage;   // normalised long package name, e.g. /Game/Foo/SM_Copy
		FString DestFolder;    // /Game/Foo
		FString DestName;      // SM_Copy
		UObject* Source = nullptr;
		bool bDestExists = false;
	};

	/** True when a package exists for this long name, on disk or only in memory (created and not saved yet). */
	static bool PackageHasAsset(const FString& PackageName)
	{
		if (FPackageName::DoesPackageExist(PackageName))
		{
			return true;
		}
		if (UPackage* InMemory = FindPackage(nullptr, *PackageName))
		{
			return InMemory->FindAssetInPackage() != nullptr;
		}
		return false;
	}

	/** Packages on disk that reference PackageName (hard or soft), PackageName itself excluded. */
	static TArray<FString> GetReferencerPackages(const FString& PackageName)
	{
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FName> Referencers;
		AR.GetReferencers(FName(*PackageName), Referencers);

		TArray<FString> Out;
		for (const FName& Ref : Referencers)
		{
			const FString RefString = Ref.ToString();
			if (RefString != PackageName)
			{
				Out.Add(RefString);
			}
		}
		Out.Sort();
		return Out;
	}

	static bool ReadPairStrings(const TSharedPtr<FJsonObject>& PairObj, FString& OutSource, FString& OutDest)
	{
		if (!PairObj.IsValid())
		{
			return false;
		}
		// "source"/"dest" is the documented spelling; the single-form names are accepted too so a
		// caller can move from one form to the other without renaming keys.
		if (!PairObj->TryGetStringField(TEXT("source"), OutSource))
		{
			PairObj->TryGetStringField(TEXT("source_path"), OutSource);
		}
		if (!PairObj->TryGetStringField(TEXT("dest"), OutDest))
		{
			PairObj->TryGetStringField(TEXT("dest_path"), OutDest);
		}
		return !OutSource.IsEmpty() && !OutDest.IsEmpty();
	}

	/** Runs another registered action and returns its result payload, or sets OutError. */
	static TSharedPtr<FJsonObject> RunNested(const TCHAR* Action, const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		const FMonolithActionResult Res = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), Action, Params);
		if (!Res.bSuccess)
		{
			OutError = Res.ErrorMessage;
			return nullptr;
		}
		return Res.Result;
	}

	static bool DeleteExistingDestination(const FString& DestPackage, FString& OutError)
	{
		TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Paths;
		Paths.Add(MakeShared<FJsonValueString>(DestPackage));
		DeleteParams->SetArrayField(TEXT("asset_paths"), Paths);

		const TSharedPtr<FJsonObject> Res = RunNested(TEXT("delete_assets"), DeleteParams, OutError);
		if (!Res.IsValid())
		{
			OutError = FString::Printf(TEXT("could not delete the existing destination: %s"), *OutError);
			return false;
		}
		bool bOk = false;
		Res->TryGetBoolField(TEXT("success"), bOk);
		if (!bOk)
		{
			OutError = TEXT("could not delete the existing destination (it is still referenced or in use)");
			return false;
		}
		return true;
	}

	static bool SaveDestination(const FString& DestPackage, FString& OutError)
	{
		TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Packages;
		Packages.Add(MakeShared<FJsonValueString>(DestPackage));
		SaveParams->SetArrayField(TEXT("packages"), Packages);

		const TSharedPtr<FJsonObject> Res = RunNested(TEXT("save_packages"), SaveParams, OutError);
		if (!Res.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Res->TryGetArrayField(TEXT("results"), Rows) && Rows && Rows->Num() > 0)
		{
			const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
			bool bSaved = false;
			if (Row.IsValid() && Row->TryGetBoolField(TEXT("saved"), bSaved) && bSaved)
			{
				return true;
			}
			if (Row.IsValid())
			{
				Row->TryGetStringField(TEXT("error"), OutError);
			}
		}
		if (OutError.IsEmpty())
		{
			OutError = TEXT("save_packages did not save the package");
		}
		return false;
	}
}

void FMonolithEditorAssetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("editor"), TEXT("duplicate_asset"),
		TEXT("Duplicate any asset (StaticMesh, DataAsset, Texture2D, ...) to a new path through IAssetTools::DuplicateAsset. "
			 "Give source_path + dest_path for one asset, or pairs:[{source, dest}] for a batch in one call. "
			 "The whole request is validated before anything is written: a missing source, an invalid destination, two pairs "
			 "with the same destination, or a destination that already exists (unless overwrite=true) refuses the request. "
			 "overwrite=true deletes the existing destination first and refuses when other packages reference it. "
			 "Returns {ok, requested, duplicated, results:[{source, dest, class, overwritten, saved, ok, error?}]}. "
			 "Type-specific actions (duplicate_blueprint, duplicate_material, ...) remain the better choice for those types."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateAsset),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("source_path"), TEXT("Asset to duplicate (single form), e.g. /Game/Meshes/SM_Rock or /Game/Meshes/SM_Rock.SM_Rock"))
			.OptionalAssetPath(TEXT("dest_path"), TEXT("Destination asset path (single form), e.g. /Game/Meshes/SM_Rock_Copy. The asset name is the last path segment."))
			.Optional(TEXT("pairs"), TEXT("array"), TEXT("Batch form: [{source, dest}, ...]. Use instead of source_path/dest_path, not with them."))
			.Optional(TEXT("overwrite"), TEXT("bool"), TEXT("Replace a destination that already exists. Default false: an existing destination refuses the request."), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save each new package to disk (through save_packages). Default true."), TEXT("true"))
			.Build());
}

FMonolithActionResult FMonolithEditorAssetActions::HandleDuplicateAsset(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorAssetActionsPrivate;
	constexpr int32 InvalidParams = FMonolithJsonUtils::ErrInvalidParams;

	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing params"), InvalidParams);
	}

	FString SingleSource, SingleDest;
	Params->TryGetStringField(TEXT("source_path"), SingleSource);
	Params->TryGetStringField(TEXT("dest_path"), SingleDest);
	const TArray<TSharedPtr<FJsonValue>>* PairsArray = nullptr;
	const bool bHasPairs = Params->TryGetArrayField(TEXT("pairs"), PairsArray) && PairsArray;
	const bool bHasSingle = !SingleSource.IsEmpty() || !SingleDest.IsEmpty();

	if (bHasPairs && bHasSingle)
	{
		return FMonolithActionResult::Error(TEXT("Give either source_path + dest_path or pairs, not both"), InvalidParams);
	}
	if (!bHasPairs && !bHasSingle)
	{
		return FMonolithActionResult::Error(TEXT("Missing params: give source_path + dest_path, or pairs:[{source, dest}]"), InvalidParams);
	}

	bool bOverwrite = false;
	Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);

	// ---- Collect the raw pairs ----
	TArray<FDuplicatePair> Pairs;
	TArray<FString> Problems;
	if (bHasSingle)
	{
		if (SingleSource.IsEmpty() || SingleDest.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("The single form needs both source_path and dest_path"), InvalidParams);
		}
		FDuplicatePair& Pair = Pairs.AddDefaulted_GetRef();
		Pair.SourceInput = SingleSource;
		Pair.DestInput = SingleDest;
	}
	else
	{
		if (PairsArray->Num() == 0)
		{
			return FMonolithActionResult::Error(TEXT("pairs is empty"), InvalidParams);
		}
		for (int32 Index = 0; Index < PairsArray->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*PairsArray)[Index];
			FString Source, Dest;
			if (!Value.IsValid() || !ReadPairStrings(Value->AsObject(), Source, Dest))
			{
				Problems.Add(FString::Printf(TEXT("pairs[%d]: expected an object {source, dest} with two non-empty strings"), Index));
				continue;
			}
			FDuplicatePair& Pair = Pairs.AddDefaulted_GetRef();
			Pair.SourceInput = Source;
			Pair.DestInput = Dest;
		}
	}

	// ---- Validate everything before writing anything ----
	TMap<FString, int32> DestSeen;     // normalised dest package -> pair index
	for (int32 Index = 0; Index < Pairs.Num(); ++Index)
	{
		FDuplicatePair& Pair = Pairs[Index];
		const FString Label = bHasPairs ? FString::Printf(TEXT("pairs[%d]: "), Index) : FString();

		Pair.Source = FMonolithAssetUtils::LoadAssetByPath(Pair.SourceInput);
		if (!Pair.Source)
		{
			Problems.Add(FString::Printf(TEXT("%ssource '%s' not found"), *Label, *Pair.SourceInput));
		}

		const FString PathError = MonolithCore::ValidatePackagePath(Pair.DestInput, Pair.DestPackage);
		if (!PathError.IsEmpty())
		{
			Problems.Add(FString::Printf(TEXT("%sdest: %s"), *Label, *PathError));
			continue;
		}
		Pair.DestFolder = FPackageName::GetLongPackagePath(Pair.DestPackage);
		Pair.DestName = FPackageName::GetShortName(Pair.DestPackage);

		if (Pair.Source && Pair.Source->GetOutermost()->GetName() == Pair.DestPackage)
		{
			Problems.Add(FString::Printf(TEXT("%sdest '%s' is the source itself"), *Label, *Pair.DestInput));
			continue;
		}
		if (const int32* Earlier = DestSeen.Find(Pair.DestPackage))
		{
			Problems.Add(FString::Printf(TEXT("%sdest '%s' is also the destination of pairs[%d]"), *Label, *Pair.DestPackage, *Earlier));
			continue;
		}
		DestSeen.Add(Pair.DestPackage, Index);

		Pair.bDestExists = PackageHasAsset(Pair.DestPackage);
		if (Pair.bDestExists)
		{
			if (!bOverwrite)
			{
				Problems.Add(FString::Printf(TEXT("%sdest '%s' already exists (pass overwrite: true to replace it)"), *Label, *Pair.DestPackage));
				continue;
			}
			const TArray<FString> Referencers = GetReferencerPackages(Pair.DestPackage);
			if (Referencers.Num() > 0)
			{
				constexpr int32 MaxListed = 10;
				TArray<FString> Listed(Referencers.GetData(), FMath::Min(Referencers.Num(), MaxListed));
				Problems.Add(FString::Printf(
					TEXT("%sdest '%s' exists and is referenced by %d package(s) (%s%s): overwriting would break those references"),
					*Label, *Pair.DestPackage, Referencers.Num(), *FString::Join(Listed, TEXT(", ")),
					Referencers.Num() > MaxListed ? TEXT(", ...") : TEXT("")));
			}
		}
	}

	// A destination that is also a source in the same batch would make the result depend on order.
	for (int32 Index = 0; Index < Pairs.Num(); ++Index)
	{
		const FString& Dest = Pairs[Index].DestPackage;
		if (Dest.IsEmpty())
		{
			continue;
		}
		for (int32 Other = 0; Other < Pairs.Num(); ++Other)
		{
			if (Other != Index && Pairs[Other].Source && Pairs[Other].Source->GetOutermost()->GetName() == Dest)
			{
				Problems.Add(FString::Printf(TEXT("pairs[%d]: dest '%s' is the source of pairs[%d]"), Index, *Dest, Other));
				break;
			}
		}
	}

	if (Problems.Num() > 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ProblemValues;
		for (const FString& P : Problems)
		{
			ProblemValues.Add(MakeShared<FJsonValueString>(P));
		}
		Data->SetArrayField(TEXT("problems"), ProblemValues);
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("duplicate_asset refused, nothing was written: %s"), *FString::Join(Problems, TEXT("; "))),
			InvalidParams).WithErrorData(Data);
	}

	// ---- Execute ----
	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 DuplicatedCount = 0;
	FString FirstError;

	for (const FDuplicatePair& Pair : Pairs)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source"), Pair.Source->GetPathName());
		Row->SetStringField(TEXT("dest"), Pair.DestPackage);
		Row->SetBoolField(TEXT("overwritten"), false);
		Row->SetBoolField(TEXT("saved"), false);

		auto Fail = [&Row, &Rows, &FirstError, &Pair](const FString& Error)
		{
			Row->SetBoolField(TEXT("ok"), false);
			Row->SetStringField(TEXT("error"), Error);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			if (FirstError.IsEmpty())
			{
				FirstError = FString::Printf(TEXT("'%s' -> '%s': %s"), *Pair.SourceInput, *Pair.DestPackage, *Error);
			}
		};

		if (Pair.bDestExists)
		{
			FString DeleteError;
			if (!DeleteExistingDestination(Pair.DestPackage, DeleteError))
			{
				Fail(DeleteError);
				continue;
			}
			Row->SetBoolField(TEXT("overwritten"), true);
		}

		UObject* Duplicated = nullptr;
		{
			// AssetTools reports some failures through message dialogs; a non-interactive action
			// must never raise a modal (it blocks the game thread and the MCP server with it).
			TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
			Duplicated = AssetTools.DuplicateAsset(Pair.DestName, Pair.DestFolder, Pair.Source);
		}
		if (!Duplicated)
		{
			Fail(TEXT("IAssetTools::DuplicateAsset returned null (see the editor log)"));
			continue;
		}

		Row->SetStringField(TEXT("dest"), Duplicated->GetPathName());
		Row->SetStringField(TEXT("class"), Duplicated->GetClass()->GetPathName());
		++DuplicatedCount;

		if (bSave)
		{
			FString SaveError;
			if (!SaveDestination(Pair.DestPackage, SaveError))
			{
				Fail(FString::Printf(TEXT("duplicated but not saved: %s"), *SaveError));
				continue;
			}
			Row->SetBoolField(TEXT("saved"), true);
		}

		Row->SetBoolField(TEXT("ok"), true);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	const bool bAllOk = FirstError.IsEmpty();

	// The single form fails as a whole, like the per-type duplicate actions. A batch reports
	// per-pair rows so the caller sees which pairs went through.
	if (bHasSingle && !bAllOk)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("duplicate_asset failed: %s"), *FirstError));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), bAllOk);
	Result->SetNumberField(TEXT("requested"), Pairs.Num());
	Result->SetNumberField(TEXT("duplicated"), DuplicatedCount);
	Result->SetArrayField(TEXT("results"), Rows);
	if (!bAllOk)
	{
		Result->SetStringField(TEXT("first_error"), FirstError);
	}
	return FMonolithActionResult::Success(Result);
}

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
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"

namespace MonolithEditorAssetActionsPrivate
{
	/** One requested duplication, after validation. */
	struct FDuplicatePair
	{
		int32 InputIndex = 0;  // position in the caller's pairs[], which malformed entries do not shift
		FString SourceInput;   // as the caller wrote it
		FString DestInput;
		FString DestPackage;   // normalised long package name, e.g. /Game/Foo/SM_Copy
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

	static bool SaveDestination(const FString& DestPackage, FString& OutError)
	{
		TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
		SaveParams->SetField(TEXT("packages"), FMonolithJsonUtils::StringArrayToJson({ DestPackage }));

		const FMonolithActionResult Saved = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("save_packages"), SaveParams);
		if (!Saved.bSuccess || !Saved.Result.IsValid())
		{
			OutError = Saved.ErrorMessage;
			return false;
		}
		const TSharedPtr<FJsonObject>& Res = Saved.Result;
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
		TEXT("Duplicate any asset (StaticMesh, DataAsset, Texture2D, ...) to a new path through ObjectTools::DuplicateSingleObject. "
			 "Give source_path + dest_path for one asset, or pairs:[{source, dest}] for a batch in one call. "
			 "The whole request is validated before anything is written: a missing source, an invalid destination, two pairs "
			 "with the same destination, or a destination that already exists (unless overwrite=true) refuses the request. "
			 "overwrite=true replaces the existing destination in its package and refuses when other packages reference it; "
			 "with save=false the file on disk keeps the old asset until the package is saved. "
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
			const TSharedPtr<FJsonObject>* PairObj = nullptr;
			FString Source, Dest;
			if (!Value.IsValid() || !Value->TryGetObject(PairObj) || !PairObj || !ReadPairStrings(*PairObj, Source, Dest))
			{
				Problems.Add(FString::Printf(TEXT("pairs[%d]: expected an object {source, dest} with two non-empty strings"), Index));
				continue;
			}
			FDuplicatePair& Pair = Pairs.AddDefaulted_GetRef();
			Pair.InputIndex = Index;
			Pair.SourceInput = Source;
			Pair.DestInput = Dest;
		}
	}

	// ---- Validate everything before writing anything ----
	const IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
	TMap<FString, int32> DestSeen;        // normalised dest package -> pairs[] index
	TMap<FString, int32> SourcePackages;  // source package -> pairs[] index of the first pair reading it
	for (FDuplicatePair& Pair : Pairs)
	{
		const FString Label = bHasPairs ? FString::Printf(TEXT("pairs[%d]: "), Pair.InputIndex) : FString();

		Pair.Source = FMonolithAssetUtils::LoadAssetByPath(Pair.SourceInput);
		// A path that predates a rename resolves to the redirector left behind: duplicate what it points at.
		while (const UObjectRedirector* Redirector = Cast<UObjectRedirector>(Pair.Source))
		{
			Pair.Source = Redirector->DestinationObject;
		}
		FString SourcePackage;
		if (!Pair.Source)
		{
			Problems.Add(FString::Printf(TEXT("%ssource '%s' not found"), *Label, *Pair.SourceInput));
		}
		else if (Pair.Source->IsA<UClass>())
		{
			Problems.Add(FString::Printf(TEXT("%ssource '%s' is a class, not an asset (for a Blueprint, pass the Blueprint path without the _C suffix)"), *Label, *Pair.SourceInput));
		}
		else if (Pair.Source->GetOutermost()->HasAnyPackageFlags(PKG_FilterEditorOnly))
		{
			Problems.Add(FString::Printf(TEXT("%ssource '%s' is in a cooked package, which cannot be duplicated"), *Label, *Pair.SourceInput));
		}
		else
		{
			SourcePackage = Pair.Source->GetOutermost()->GetName();
			if (!SourcePackages.Contains(SourcePackage))
			{
				SourcePackages.Add(SourcePackage, Pair.InputIndex);
			}
		}

		const FString PathError = MonolithCore::ValidatePackagePath(Pair.DestInput, Pair.DestPackage);
		if (!PathError.IsEmpty())
		{
			Problems.Add(FString::Printf(TEXT("%sdest: %s"), *Label, *PathError));
			continue;
		}
		Pair.DestName = FPackageName::GetShortName(Pair.DestPackage);

		// A long package name allows characters an object name does not, e.g. "SM_Copy(1)".
		FText NameReason;
		if (!FName(*Pair.DestName).IsValidObjectName(NameReason))
		{
			Problems.Add(FString::Printf(TEXT("%sdest '%s': invalid asset name: %s"), *Label, *Pair.DestInput, *NameReason.ToString()));
			continue;
		}
		FText PathReason;
		if (!AssetTools.IsObjectPathAllowed(Pair.DestPackage, &PathReason))
		{
			Problems.Add(FString::Printf(TEXT("%sdest '%s' is not allowed: %s"), *Label, *Pair.DestInput, *PathReason.ToString()));
			continue;
		}

		if (SourcePackage == Pair.DestPackage)
		{
			Problems.Add(FString::Printf(TEXT("%sdest '%s' is the source itself"), *Label, *Pair.DestInput));
			continue;
		}
		if (const int32* Earlier = DestSeen.Find(Pair.DestPackage))
		{
			Problems.Add(FString::Printf(TEXT("%sdest '%s' is also the destination of pairs[%d]"), *Label, *Pair.DestPackage, *Earlier));
			continue;
		}
		DestSeen.Add(Pair.DestPackage, Pair.InputIndex);

		Pair.bDestExists = PackageHasAsset(Pair.DestPackage);
		if (Pair.bDestExists)
		{
			if (!bOverwrite)
			{
				Problems.Add(FString::Printf(TEXT("%sdest '%s' already exists (pass overwrite: true to replace it)"), *Label, *Pair.DestPackage));
				continue;
			}
			// The copy is written into the destination's package: replacing a map by an asset, or an
			// asset by a map, would leave both a .umap and a .uasset for one package name.
			if (FEditorFileUtils::IsMapPackageAsset(Pair.DestPackage) || (Pair.Source && Pair.Source->IsA<UWorld>()))
			{
				Problems.Add(FString::Printf(TEXT("%sdest '%s' exists and overwrite does not support maps: delete the destination first"), *Label, *Pair.DestPackage));
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
	// A pair whose destination is its own source is already reported above.
	for (const FDuplicatePair& Pair : Pairs)
	{
		const int32* Reader = SourcePackages.Find(Pair.DestPackage);
		if (Reader && *Reader != Pair.InputIndex)
		{
			Problems.Add(FString::Printf(TEXT("pairs[%d]: dest '%s' is the source of pairs[%d]"), Pair.InputIndex, *Pair.DestPackage, *Reader));
		}
	}

	if (Problems.Num() > 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetField(TEXT("problems"), FMonolithJsonUtils::StringArrayToJson(Problems));
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("duplicate_asset refused, nothing was written: %s"), *FString::Join(Problems, TEXT("; "))),
			InvalidParams).WithErrorData(Data);
	}

	// ---- Execute ----
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

		// Same preparation as delete_assets for the asset being replaced: a dirty package would raise
		// a save prompt when its editor closes, and an open editor holds a reference to it.
		bool bExistingWasDirty = false;
		if (Pair.bDestExists)
		{
			if (UObject* Existing = FMonolithAssetUtils::LoadAssetByPath(Pair.DestPackage))
			{
				UPackage* ExistingPackage = Existing->GetOutermost();
				bExistingWasDirty = ExistingPackage->IsDirty();
				ExistingPackage->SetDirtyFlag(false);
				if (UAssetEditorSubsystem* AssetEditors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
				{
					AssetEditors->CloseAllEditorsForAsset(Existing);
				}
			}
		}

		// ObjectTools::DuplicateSingleObject is what IAssetTools::DuplicateAsset runs, minus two things
		// an unattended action cannot afford: AssetTools refuses an existing destination outright, and
		// with source control enabled it saves the copy through a checkout prompt (a modal dialog).
		// DuplicateSingleObject replaces an existing destination itself and keeps its package rooted
		// meanwhile, so the file is neither removed nor opened for delete: saving the copy is then an
		// ordinary edit of that file.
		UObject* Duplicated = nullptr;
		{
			// ObjectTools reports failures through message dialogs; a non-interactive action must
			// never raise a modal (it blocks the game thread and the MCP server with it).
			TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
			ObjectTools::FPackageGroupName Target;
			Target.PackageName = Pair.DestPackage;
			Target.ObjectName = Pair.DestName;
			TSet<UPackage*> PackagesNotFullyLoaded;
			Duplicated = ObjectTools::DuplicateSingleObject(Pair.Source, Target, PackagesNotFullyLoaded, /*bPromptToOverwrite=*/false);
		}
		if (!Duplicated)
		{
			if (bExistingWasDirty)
			{
				UPackage* ExistingPackage = FindPackage(nullptr, *Pair.DestPackage);
				if (ExistingPackage && ExistingPackage->FindAssetInPackage())
				{
					ExistingPackage->SetDirtyFlag(true);
				}
			}
			Fail(Pair.bDestExists
				? TEXT("the existing destination could not be replaced; its package file on disk is untouched (see the editor log)")
				: TEXT("ObjectTools::DuplicateSingleObject returned null (see the editor log)"));
			continue;
		}
		Duplicated->SetFlags(RF_Public | RF_Standalone);

		Row->SetBoolField(TEXT("overwritten"), Pair.bDestExists);
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

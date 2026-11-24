#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

// Generic asset operations (editor namespace).
//
//   duplicate_asset - duplicate any asset (StaticMesh, DataAsset, Texture, ...) through
//                     ObjectTools::DuplicateSingleObject. Single form (source_path + dest_path) or
//                     batch form (pairs). The whole request is validated before anything is
//                     written: a bad source, an invalid or colliding destination, or a
//                     destination that already exists without overwrite refuses the request.
//
// The per-type duplicate actions (duplicate_blueprint, duplicate_material, ...) stay: they
// carry type-specific behaviour. This one covers every other asset class.
class FMonolithEditorAssetActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleDuplicateAsset(const TSharedPtr<FJsonObject>& Params);
};

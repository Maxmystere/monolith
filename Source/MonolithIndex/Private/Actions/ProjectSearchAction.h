#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FProjectSearchAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("search"); }
	static FString GetDescription() { return TEXT("Full-text search across indexed project assets (name, class, description, path, module), graph nodes (name, class, type) and literal values typed into Blueprint pins (value, pin name; those hits add match_kind \"pin_default\", graph, node, pin), optionally narrowed with asset_class"); }
	static TSharedPtr<FJsonObject> GetSchema();
};

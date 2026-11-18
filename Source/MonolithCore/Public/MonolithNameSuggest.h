// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

class UStruct;

/**
 * "Did you mean ...?" suggestions for a name the caller got wrong: a parameter key, a property
 * or struct field name. Shared by the dispatcher's unknown-param handling and by the reflection
 * readers and writers, so every "unknown X" message proposes the closest real names the same way.
 *
 * Similarity is case-insensitive: 1 - Levenshtein / max(len), raised to 0.6 when one name contains
 * the other (e.g. "Mesh" vs "StaticMesh"). Only candidates scoring at least MinScore are returned,
 * best first, so an unrelated name yields no suggestion rather than a misleading one.
 */
namespace MonolithNameSuggest
{
	/** Default minimum similarity for a suggestion. */
	inline constexpr float DefaultMinScore = 0.5f;

	/** Similarity in [0, 1] as described above. */
	MONOLITHCORE_API float Similarity(const FString& A, const FString& B);

	/** Up to TopN candidates closest to Needle, best first. Exact (case-insensitive) matches are excluded. */
	MONOLITHCORE_API TArray<FString> Closest(const FString& Needle, const TArray<FString>& Candidates,
		int32 TopN = 3, float MinScore = DefaultMinScore);

	/** Closest property names on Struct (including inherited ones). */
	MONOLITHCORE_API TArray<FString> ClosestPropertyNames(const UStruct* Struct, const FString& Needle,
		int32 TopN = 3, float MinScore = DefaultMinScore);

	/** " — did you mean 'A'?" / " — did you mean 'A', 'B' or 'C'?", or an empty string when Names is empty. */
	MONOLITHCORE_API FString DidYouMeanSuffix(const TArray<FString>& Names);
}

// SPDX-License-Identifier: MIT
#include "MonolithNameSuggest.h"

#include "Algo/LevenshteinDistance.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace MonolithNameSuggest
{
	float Similarity(const FString& A, const FString& B)
	{
		const FString LowerA = A.ToLower();
		const FString LowerB = B.ToLower();
		const int32 WorstCase = FMath::Max(LowerA.Len(), LowerB.Len());
		if (WorstCase == 0)
		{
			return 1.0f;
		}
		float Score = 1.0f - static_cast<float>(Algo::LevenshteinDistance(LowerA, LowerB)) / static_cast<float>(WorstCase);

		// A name that contains the other is usually a prefix/suffix slip ("Mesh" for "StaticMesh",
		// "path" for "asset_path"), which plain edit distance scores too low.
		constexpr int32 MinContainedLen = 3;
		const FString& Shorter = LowerA.Len() <= LowerB.Len() ? LowerA : LowerB;
		const FString& Longer = LowerA.Len() <= LowerB.Len() ? LowerB : LowerA;
		if (Shorter.Len() >= MinContainedLen && Longer.Contains(Shorter))
		{
			Score = FMath::Max(Score, 0.6f);
		}
		return Score;
	}

	TArray<FString> Closest(const FString& Needle, const TArray<FString>& Candidates, int32 TopN, float MinScore)
	{
		struct FScored
		{
			FString Name;
			float Score;
			int32 Order;
		};
		TArray<FScored> Scored;
		TSet<FString> Seen;
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			const FString& Candidate = Candidates[Index];
			if (Candidate.IsEmpty() || Candidate.Equals(Needle, ESearchCase::IgnoreCase) || Seen.Contains(Candidate))
			{
				continue;
			}
			Seen.Add(Candidate);
			const float Score = Similarity(Needle, Candidate);
			if (Score >= MinScore)
			{
				Scored.Add({ Candidate, Score, Index });
			}
		}
		Scored.Sort([](const FScored& L, const FScored& R)
		{
			return L.Score != R.Score ? L.Score > R.Score : L.Order < R.Order;
		});

		TArray<FString> Out;
		for (int32 Index = 0; Index < Scored.Num() && Index < TopN; ++Index)
		{
			Out.Add(Scored[Index].Name);
		}
		return Out;
	}

	TArray<FString> ClosestPropertyNames(const UStruct* Struct, const FString& Needle, int32 TopN, float MinScore)
	{
		TArray<FString> Names;
		if (Struct)
		{
			// Deprecated storage is never offered: the engine no longer reads it, and the
			// walker and set_cdo_property refuse it.
			FString DeprecatedReason;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (!FMonolithReflectionWalker::IsDeprecatedProperty(*It, DeprecatedReason))
				{
					Names.Add(It->GetName());
				}
			}
		}
		return Closest(Needle, Names, TopN, MinScore);
	}

	FString DidYouMeanSuffix(const TArray<FString>& Names)
	{
		if (Names.Num() == 0)
		{
			return FString();
		}
		TArray<FString> Quoted;
		for (const FString& Name : Names)
		{
			Quoted.Add(FString::Printf(TEXT("'%s'"), *Name));
		}
		if (Quoted.Num() == 1)
		{
			return FString::Printf(TEXT(" — did you mean %s?"), *Quoted[0]);
		}
		const FString Last = Quoted.Pop();
		return FString::Printf(TEXT(" — did you mean %s or %s?"), *FString::Join(Quoted, TEXT(", ")), *Last);
	}
}

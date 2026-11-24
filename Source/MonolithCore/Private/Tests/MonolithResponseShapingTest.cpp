// SPDX-License-Identifier: MIT
// Survivor B (Response Shaping) automation tests — plan §12 "Survivor B".
// Plan: Plugins/Monolith/Docs/plans/2026-05-27-mcp-llm-ergonomics.md
//
// DEVIATION NOTE: plan §6 file-table specifies `Source/MonolithCore/Tests/...`.
// This file lives under `Source/MonolithCore/Private/Tests/...` instead so UBT's
// auto-include of `Private/` picks it up without a Build.cs change. Matches the
// existing precedent at `Private/Reflection/Tests/MonolithReflectionWalkerTest.cpp`.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformMisc.h"
#include "MonolithNameSuggest.h"
#include "Reflection/MonolithReflectionWalker.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithResponseShapingTestDetail
{
	/** Build a response object with: name, debug_info, empty_array, empty_obj, null_field, empty_string. */
	static TSharedPtr<FJsonObject> MakeKitchenSinkResponse()
	{
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("name"), TEXT("Bob"));
		R->SetStringField(TEXT("debug_info"), TEXT("verbose-blob"));
		R->SetArrayField(TEXT("empty_array"), TArray<TSharedPtr<FJsonValue>>());
		R->SetObjectField(TEXT("empty_obj"), MakeShared<FJsonObject>());
		R->SetField(TEXT("null_field"), MakeShared<FJsonValueNull>());
		R->SetStringField(TEXT("empty_string"), TEXT(""));
		R->SetNumberField(TEXT("answer"), 42);
		return R;
	}

	static TSharedPtr<FJsonObject> MakeParams()
	{
		return MakeShared<FJsonObject>();
	}

	static void SetStringArray(TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, std::initializer_list<const TCHAR*> Items)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const TCHAR* I : Items)
		{
			Arr.Add(MakeShared<FJsonValueString>(FString(I)));
		}
		Obj->SetArrayField(Key, Arr);
	}
}

// ---------------------------------------------------------------------------
// Test 1: Top-level whitelist via _fields:["name"].
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingFieldsWhitelistTest,
	"Monolith.ResponseShaping.FieldsWhitelist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingFieldsWhitelistTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeKitchenSinkResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	SetStringArray(Params, TEXT("_fields"), {TEXT("name")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestTrue(TEXT("name retained"), Response->HasField(TEXT("name")));
	TestFalse(TEXT("debug_info dropped"), Response->HasField(TEXT("debug_info")));
	TestFalse(TEXT("answer dropped"), Response->HasField(TEXT("answer")));
	TestEqual(TEXT("no warnings emitted"), Warnings.Num(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Top-level blacklist via _omit:["debug_info"].
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingOmitBlacklistTest,
	"Monolith.ResponseShaping.OmitBlacklist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingOmitBlacklistTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeKitchenSinkResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	SetStringArray(Params, TEXT("_omit"), {TEXT("debug_info")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestFalse(TEXT("debug_info removed"), Response->HasField(TEXT("debug_info")));
	TestTrue(TEXT("name preserved"), Response->HasField(TEXT("name")));
	TestTrue(TEXT("answer preserved"), Response->HasField(TEXT("answer")));
	TestEqual(TEXT("no warnings emitted"), Warnings.Num(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: Mutually-exclusive — both _fields and _omit populated.
// Per plan §3.B: _fields wins, _omit ignored, warning emitted.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingMutuallyExclusiveTest,
	"Monolith.ResponseShaping.MutuallyExclusive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingMutuallyExclusiveTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeKitchenSinkResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	SetStringArray(Params, TEXT("_fields"), {TEXT("name")});
	SetStringArray(Params, TEXT("_omit"),   {TEXT("name")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	// _fields wins: only `name` survives.
	TestTrue(TEXT("name retained because _fields wins"), Response->HasField(TEXT("name")));
	TestFalse(TEXT("answer dropped by _fields whitelist"), Response->HasField(TEXT("answer")));
	TestTrue(TEXT("exactly one warning emitted"), Warnings.Num() == 1);
	if (Warnings.Num() == 1)
	{
		TestTrue(TEXT("warning text mentions mutually exclusive"),
			Warnings[0].Contains(TEXT("mutually exclusive")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: Empty _fields: [] — no-op (does NOT drop everything).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingEmptyFieldsNoOpTest,
	"Monolith.ResponseShaping.EmptyFieldsNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingEmptyFieldsNoOpTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeKitchenSinkResponse();
	const int32 InitialKeys = Response->Values.Num();

	TSharedPtr<FJsonObject> Params = MakeParams();
	Params->SetArrayField(TEXT("_fields"), TArray<TSharedPtr<FJsonValue>>()); // empty array

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestEqual(TEXT("all keys preserved with empty _fields"), Response->Values.Num(), InitialKeys);
	TestTrue(TEXT("name still present"), Response->HasField(TEXT("name")));
	TestTrue(TEXT("debug_info still present"), Response->HasField(TEXT("debug_info")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: _compact_json:true drops null + empty string + empty array + empty object.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingCompactJsonTest,
	"Monolith.ResponseShaping.CompactJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingCompactJsonTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeKitchenSinkResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	Params->SetBoolField(TEXT("_compact_json"), true);

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestFalse(TEXT("empty_array dropped"),   Response->HasField(TEXT("empty_array")));
	TestFalse(TEXT("empty_obj dropped"),     Response->HasField(TEXT("empty_obj")));
	TestFalse(TEXT("null_field dropped"),    Response->HasField(TEXT("null_field")));
	TestFalse(TEXT("empty_string dropped"),  Response->HasField(TEXT("empty_string")));
	TestTrue (TEXT("name retained"),         Response->HasField(TEXT("name")));
	TestTrue (TEXT("debug_info retained"),   Response->HasField(TEXT("debug_info")));
	TestTrue (TEXT("answer (number) retained"), Response->HasField(TEXT("answer")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 6: K3 STRICT_PARAMS smoke — universal allowlist must include
// _fields / _omit / _compact_json so STRICT_PARAMS=1 does not hard-fail.
//
// Registers a throwaway action under namespace "monolith_test_shaping" with
// a one-string-param schema; dispatches with `_fields: ["name"]` extra; asserts
// Success (no "Unknown param" hard-fail). Cleans up via UnregisterNamespace.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingStrictParamsAllowlistTest,
	"Monolith.ResponseShaping.StrictParamsAllowlist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingStrictParamsAllowlistTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	const FString TestNs = TEXT("monolith_test_shaping");
	const FString TestAction = TEXT("ping");

	// Set STRICT_PARAMS=1 for the duration of this test, restore after.
	const FString PriorEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("STRICT_PARAMS"));
	FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), TEXT("1"));
	ON_SCOPE_EXIT
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), *PriorEnv);
		FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("monolith_test_shaping"));
	};

	// Register a tiny throwaway action that just echoes back {"ok": true, "name": <input>}.
	TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.Required(TEXT("name"), TEXT("string"), TEXT("Throwaway test param."))
		.Build();

	auto Handler = FMonolithActionHandler::CreateLambda(
		[](const TSharedPtr<FJsonObject>& Params) -> FMonolithActionResult
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("ok"), true);
			FString Name;
			Params->TryGetStringField(TEXT("name"), Name);
			Result->SetStringField(TEXT("name"), Name);
			Result->SetStringField(TEXT("debug_info"), TEXT("noise"));
			return FMonolithActionResult::Success(Result);
		});

	FMonolithToolRegistry::Get().RegisterAction(TestNs, TestAction, TEXT("Test."), Handler, Schema);

	// Dispatch with `_fields: ["name"]` — must NOT trip STRICT_PARAMS=1 rejection.
	TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
	CallParams->SetStringField(TEXT("name"), TEXT("Alice"));
	SetStringArray(CallParams, TEXT("_fields"), {TEXT("name")});

	FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(TestNs, TestAction, CallParams);

	TestTrue(TEXT("STRICT_PARAMS=1 did not reject the _fields universal param"), R.bSuccess);
	if (R.bSuccess && R.Result.IsValid())
	{
		// And the shaping pass did its job — only `name` should remain (plus possibly warnings).
		TestTrue(TEXT("name field present"), R.Result->HasField(TEXT("name")));
		TestFalse(TEXT("debug_info filtered by _fields"), R.Result->HasField(TEXT("debug_info")));
	}
	return true;
}

// =============================================================================
// Phase 1.1 — RI ergonomics handover #3: _row_fields + _path_fields tests.
// Handover doc: Plugins/Monolith/Docs/plans/2026-05-29-ri-ergonomics-improvements-handover.md
// =============================================================================

namespace MonolithResponseShapingTestDetail
{
	/** Build a response with one top-level list payload `decisions[]` of 3 rows. */
	static TSharedPtr<FJsonObject> MakeListPayloadResponse()
	{
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetNumberField(TEXT("count"), 3);
		R->SetStringField(TEXT("cursor"), TEXT(""));

		TArray<TSharedPtr<FJsonValue>> Rows;
		for (int32 I = 0; I < 3; ++I)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("title"), FString::Printf(TEXT("decision-%d"), I));
			Row->SetStringField(TEXT("source_path"), FString::Printf(TEXT("Docs/Research/d%d.md"), I));
			Row->SetStringField(TEXT("rationale"), TEXT("long verbose blob that should be droppable"));
			Row->SetNumberField(TEXT("score"), I * 10);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		R->SetArrayField(TEXT("decisions"), Rows);
		return R;
	}

	/** Build a response with TWO top-level list payloads (ambiguity scenario). */
	static TSharedPtr<FJsonObject> MakeAmbiguousListPayloadResponse()
	{
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> RowsA;
		for (int32 I = 0; I < 2; ++I)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("title"), FString::Printf(TEXT("a-%d"), I));
			RowsA.Add(MakeShared<FJsonValueObject>(Row));
		}
		R->SetArrayField(TEXT("decisions"), RowsA);

		TArray<TSharedPtr<FJsonValue>> RowsB;
		for (int32 I = 0; I < 2; ++I)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), FString::Printf(TEXT("b-%d"), I));
			RowsB.Add(MakeShared<FJsonValueObject>(Row));
		}
		R->SetArrayField(TEXT("risks"), RowsB);

		return R;
	}

	/** Build a response with a nested `uclass` envelope (the _path_fields target). */
	static TSharedPtr<FJsonObject> MakeNestedEnvelopeResponse()
	{
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> UClass = MakeShared<FJsonObject>();
		UClass->SetStringField(TEXT("class_name"), TEXT("UMyClass"));
		UClass->SetStringField(TEXT("parent_class"), TEXT("UObject"));
		UClass->SetStringField(TEXT("source_path"), TEXT("Source/Foo/MyClass.h"));

		TArray<TSharedPtr<FJsonValue>> Props;
		TSharedPtr<FJsonObject> P0 = MakeShared<FJsonObject>();
		P0->SetStringField(TEXT("name"), TEXT("Health"));
		Props.Add(MakeShared<FJsonValueObject>(P0));
		UClass->SetArrayField(TEXT("uproperties"), Props);

		R->SetObjectField(TEXT("uclass"), UClass);
		R->SetStringField(TEXT("envelope_field"), TEXT("noise"));
		return R;
	}
}

// ---------------------------------------------------------------------------
// Test 7: _row_fields on a 3-row list — verify only retained keys appear per row.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingRowFieldsTest,
	"Monolith.ResponseShaping.RowFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingRowFieldsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeListPayloadResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	SetStringArray(Params, TEXT("_row_fields"), {TEXT("title"), TEXT("source_path")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestEqual(TEXT("no warnings emitted for clean single-payload case"), Warnings.Num(), 0);

	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	TestTrue(TEXT("decisions[] still present"),
		Response->TryGetArrayField(TEXT("decisions"), Rows) && Rows != nullptr);
	if (Rows)
	{
		TestEqual(TEXT("3 rows preserved"), Rows->Num(), 3);
		for (const TSharedPtr<FJsonValue>& RowVal : *Rows)
		{
			const TSharedPtr<FJsonObject>* RowObj = nullptr;
			if (RowVal.IsValid() && RowVal->TryGetObject(RowObj) && RowObj && (*RowObj).IsValid())
			{
				TestTrue (TEXT("row retains title"),       (*RowObj)->HasField(TEXT("title")));
				TestTrue (TEXT("row retains source_path"), (*RowObj)->HasField(TEXT("source_path")));
				TestFalse(TEXT("row drops rationale"),     (*RowObj)->HasField(TEXT("rationale")));
				TestFalse(TEXT("row drops score"),         (*RowObj)->HasField(TEXT("score")));
			}
		}
	}
	// Envelope keys (count, cursor) untouched by _row_fields.
	TestTrue(TEXT("envelope count untouched"), Response->HasField(TEXT("count")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 8: _row_fields with no list payload — warning + no-op.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingRowFieldsNoListTest,
	"Monolith.ResponseShaping.RowFieldsNoList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingRowFieldsNoListTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	// MakeKitchenSinkResponse has no array-of-objects key.
	TSharedPtr<FJsonObject> Response = MakeKitchenSinkResponse();
	const int32 InitialKeys = Response->Values.Num();

	TSharedPtr<FJsonObject> Params = MakeParams();
	SetStringArray(Params, TEXT("_row_fields"), {TEXT("title")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestEqual(TEXT("response keys untouched (no-op)"), Response->Values.Num(), InitialKeys);
	TestTrue(TEXT("exactly one warning emitted"), Warnings.Num() == 1);
	if (Warnings.Num() >= 1)
	{
		TestTrue(TEXT("warning mentions no list payload"),
			Warnings[0].Contains(TEXT("no top-level array-of-objects")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 9: _row_fields ambiguous — warning + no-op.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingRowFieldsAmbiguousTest,
	"Monolith.ResponseShaping.RowFieldsAmbiguous",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingRowFieldsAmbiguousTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeAmbiguousListPayloadResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	SetStringArray(Params, TEXT("_row_fields"), {TEXT("title")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	// No mutation should have happened — both rows in `risks` keep `name`.
	const TArray<TSharedPtr<FJsonValue>>* RisksRows = nullptr;
	if (Response->TryGetArrayField(TEXT("risks"), RisksRows) && RisksRows && RisksRows->Num() > 0)
	{
		const TSharedPtr<FJsonObject>* RowObj = nullptr;
		if ((*RisksRows)[0]->TryGetObject(RowObj) && RowObj && (*RowObj).IsValid())
		{
			TestTrue(TEXT("risks row retains `name` (no mutation)"), (*RowObj)->HasField(TEXT("name")));
		}
	}

	TestTrue(TEXT("at least one warning emitted"), Warnings.Num() >= 1);
	bool bSawAmbiguity = false;
	for (const FString& W : Warnings)
	{
		if (W.Contains(TEXT("_row_fields ambiguous")))
		{
			bSawAmbiguity = true;
			break;
		}
	}
	TestTrue(TEXT("warning text mentions `_row_fields ambiguous`"), bSawAmbiguity);
	return true;
}

// ---------------------------------------------------------------------------
// Test 10: _path_fields simple — extract a nested leaf via "foo.bar" path.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingPathFieldsTest,
	"Monolith.ResponseShaping.PathFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingPathFieldsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeNestedEnvelopeResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	SetStringArray(Params, TEXT("_path_fields"),
		{TEXT("uclass.class_name"), TEXT("uclass.parent_class")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestEqual(TEXT("no warnings emitted for clean path"), Warnings.Num(), 0);
	TestFalse(TEXT("envelope_field dropped"), Response->HasField(TEXT("envelope_field")));
	TestTrue (TEXT("uclass envelope present"), Response->HasField(TEXT("uclass")));

	const TSharedPtr<FJsonObject>* UClassObj = nullptr;
	if (Response->TryGetObjectField(TEXT("uclass"), UClassObj) && UClassObj && (*UClassObj).IsValid())
	{
		TestTrue (TEXT("class_name retained"),    (*UClassObj)->HasField(TEXT("class_name")));
		TestTrue (TEXT("parent_class retained"),  (*UClassObj)->HasField(TEXT("parent_class")));
		TestFalse(TEXT("source_path dropped"),    (*UClassObj)->HasField(TEXT("source_path")));
		TestFalse(TEXT("uproperties dropped"),    (*UClassObj)->HasField(TEXT("uproperties")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 11: _path_fields missing path — graceful skip, no warning.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingPathFieldsMissingTest,
	"Monolith.ResponseShaping.PathFieldsMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingPathFieldsMissingTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeNestedEnvelopeResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	// Mix one valid leaf with two missing paths — valid should be retained,
	// missing should drop silently with no warnings.
	SetStringArray(Params, TEXT("_path_fields"),
		{TEXT("uclass.class_name"), TEXT("uclass.does_not_exist"), TEXT("non_existent_root.child")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestEqual(TEXT("no warnings emitted for missing-path skip"), Warnings.Num(), 0);

	const TSharedPtr<FJsonObject>* UClassObj = nullptr;
	if (Response->TryGetObjectField(TEXT("uclass"), UClassObj) && UClassObj && (*UClassObj).IsValid())
	{
		TestTrue (TEXT("class_name (valid path) retained"), (*UClassObj)->HasField(TEXT("class_name")));
		TestFalse(TEXT("missing leaf NOT in output"),       (*UClassObj)->HasField(TEXT("does_not_exist")));
	}
	TestFalse(TEXT("non-existent root NOT in output"), Response->HasField(TEXT("non_existent_root")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 12: _row_fields with ALL-wrong keys — rows emptied AND "matched no keys"
// warning fires with the available-keys list. Guards the silent "[{},{}]" bug.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingRowFieldsNoMatchTest,
	"Monolith.ResponseShaping.RowFieldsNoMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingRowFieldsNoMatchTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeListPayloadResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	// Real row keys are title/source_path/rationale/score — request keys that
	// match NONE of them (the live risk.list_conditional_gates failure mode).
	SetStringArray(Params, TEXT("_row_fields"), {TEXT("file_path"), TEXT("gate_macro")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	// Every row should now be an empty object (all real keys dropped).
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	TestTrue(TEXT("decisions[] still present"),
		Response->TryGetArrayField(TEXT("decisions"), Rows) && Rows != nullptr);
	if (Rows)
	{
		for (const TSharedPtr<FJsonValue>& RowVal : *Rows)
		{
			const TSharedPtr<FJsonObject>* RowObj = nullptr;
			if (RowVal.IsValid() && RowVal->TryGetObject(RowObj) && RowObj && (*RowObj).IsValid())
			{
				TestEqual(TEXT("row emptied (no requested key matched)"), (*RowObj)->Values.Num(), 0);
			}
		}
	}

	// The no-match warning must fire and surface the available keys.
	bool bSawNoMatch = false;
	bool bSawAvailableKeys = false;
	for (const FString& W : Warnings)
	{
		if (W.Contains(TEXT("matched no keys")))
		{
			bSawNoMatch = true;
			// Available-keys list should name at least one real row key.
			bSawAvailableKeys = W.Contains(TEXT("available row keys"))
				&& W.Contains(TEXT("source_path"));
		}
	}
	TestTrue(TEXT("warning mentions `matched no keys`"), bSawNoMatch);
	TestTrue(TEXT("warning lists available row keys incl. source_path"), bSawAvailableKeys);
	return true;
}

// ---------------------------------------------------------------------------
// Test 13: _row_fields with SOME-right-some-wrong — partial match is success,
// NO no-match warning. (Only the total miss is confusing enough to warn on.)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingRowFieldsPartialMatchTest,
	"Monolith.ResponseShaping.RowFieldsPartialMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingRowFieldsPartialMatchTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeListPayloadResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	// One real key (title), one bogus key (nonexistent) — partial match.
	SetStringArray(Params, TEXT("_row_fields"), {TEXT("title"), TEXT("nonexistent")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	// The real key survives in each row; the bogus one simply never existed.
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	TestTrue(TEXT("decisions[] still present"),
		Response->TryGetArrayField(TEXT("decisions"), Rows) && Rows != nullptr);
	if (Rows)
	{
		for (const TSharedPtr<FJsonValue>& RowVal : *Rows)
		{
			const TSharedPtr<FJsonObject>* RowObj = nullptr;
			if (RowVal.IsValid() && RowVal->TryGetObject(RowObj) && RowObj && (*RowObj).IsValid())
			{
				TestTrue (TEXT("row retains title (matched)"),  (*RowObj)->HasField(TEXT("title")));
				TestFalse(TEXT("row drops rationale (unmatched)"), (*RowObj)->HasField(TEXT("rationale")));
			}
		}
	}

	// Partial match must NOT raise the no-match warning.
	bool bSawNoMatch = false;
	for (const FString& W : Warnings)
	{
		if (W.Contains(TEXT("matched no keys")))
		{
			bSawNoMatch = true;
			break;
		}
	}
	TestFalse(TEXT("partial match emits NO `matched no keys` warning"), bSawNoMatch);
	return true;
}

// ---------------------------------------------------------------------------
// Test 14: _path_fields with ALL-wrong paths — "matched no paths" warning fires
// and lists the top-level keys available.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingPathFieldsNoMatchTest,
	"Monolith.ResponseShaping.PathFieldsNoMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingPathFieldsNoMatchTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeNestedEnvelopeResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	// Top-level keys are uclass/envelope_field — request paths that resolve to none.
	SetStringArray(Params, TEXT("_path_fields"),
		{TEXT("wrong_root.class_name"), TEXT("also_wrong.child")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	bool bSawNoMatch = false;
	bool bSawTopLevelKeys = false;
	for (const FString& W : Warnings)
	{
		if (W.Contains(TEXT("matched no paths")))
		{
			bSawNoMatch = true;
			bSawTopLevelKeys = W.Contains(TEXT("top-level keys available"))
				&& W.Contains(TEXT("uclass"));
		}
	}
	TestTrue(TEXT("warning mentions `matched no paths`"), bSawNoMatch);
	TestTrue(TEXT("warning lists top-level keys incl. uclass"), bSawTopLevelKeys);
	return true;
}

// =============================================================================
// Unknown params: "did you mean", per-call _strict, run_python `code` alias,
// and closest property names in the reflection walker (the MPC case).
// =============================================================================

namespace MonolithParamSuggestTestDetail
{
	static const TCHAR* Ns = TEXT("monolith_test_params");

	/** Registers `echo` with a required `name` and an optional `count` (alias `total`); reports whether the handler saw `_strict`. */
	static void RegisterEcho()
	{
		TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Required test param."))
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Optional test param."), { TEXT("total") })
			.Build();
		FMonolithToolRegistry::Get().RegisterAction(Ns, TEXT("echo"), TEXT("Test."),
			FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>& Params)
			{
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetBoolField(TEXT("saw_strict"), Params->HasField(TEXT("_strict")));
				return FMonolithActionResult::Success(Result);
			}),
			Schema);
	}

	static TArray<FString> Warnings(const FMonolithActionResult& R)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (R.bSuccess && R.Result.IsValid() && R.Result->TryGetArrayField(TEXT("warnings"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				Out.Add(V->AsString());
			}
		}
		return Out;
	}

	static bool HasWarning(const FMonolithActionResult& R, const TCHAR* Key, const TCHAR* Suggestion)
	{
		return Warnings(R).ContainsByPredicate([Key, Suggestion](const FString& S)
		{
			const bool bSuggests = S.Contains(TEXT("did you mean"));
			return S.Contains(FString::Printf(TEXT("'%s'"), Key))
				&& (Suggestion ? S.Contains(FString::Printf(TEXT("did you mean '%s'"), Suggestion)) : !bSuggests);
		});
	}

	/** The only {given, param} entry of error.data[Field], or null. */
	static TSharedPtr<FJsonObject> SoleErrorDataEntry(const FMonolithActionResult& R, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (R.ErrorData.IsValid() && R.ErrorData->TryGetObject(Data) && (*Data)->TryGetArrayField(Field, Entries) && Entries->Num() == 1)
		{
			return (*Entries)[0]->AsObject();
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithParamSuggestUnknownWarningTest,
	"Monolith.ResponseShaping.ParamSuggest.UnknownParamWarningSuggests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamSuggestUnknownWarningTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithParamSuggestTestDetail;
	const FString PriorEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("STRICT_PARAMS"));
	FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), TEXT(""));
	ON_SCOPE_EXIT
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), *PriorEnv);
		FMonolithToolRegistry::Get().UnregisterNamespace(Ns);
	};
	RegisterEcho();

	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("name"), TEXT("x"));
	P->SetNumberField(TEXT("cuont"), 3);
	P->SetNumberField(TEXT("zzzzzz"), 1);
	P->SetBoolField(TEXT("fields"), true);
	const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("echo"), P);
	TestTrue(TEXT("Unknown params stay a soft warning without strict mode"), R.bSuccess);

	TestTrue(TEXT("Typo warning suggests the closest param"), HasWarning(R, TEXT("cuont"), TEXT("count")));
	TestTrue(TEXT("An unrelated key gets no suggestion"), HasWarning(R, TEXT("zzzzzz"), nullptr));
	TestTrue(TEXT("A slip on a universal key suggests it"), HasWarning(R, TEXT("fields"), TEXT("_fields")));

	{
		// An alias is offered while its param is free ...
		TSharedPtr<FJsonObject> Free = MakeShared<FJsonObject>();
		Free->SetStringField(TEXT("name"), TEXT("x"));
		Free->SetNumberField(TEXT("totl"), 2);
		TestTrue(TEXT("A free alias is suggested"),
			HasWarning(FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("echo"), Free), TEXT("totl"), TEXT("total")));

		// ... and not once the param is supplied: following it would be a collision error.
		TSharedPtr<FJsonObject> Taken = MakeShared<FJsonObject>();
		Taken->SetStringField(TEXT("name"), TEXT("x"));
		Taken->SetNumberField(TEXT("count"), 1);
		Taken->SetNumberField(TEXT("totl"), 2);
		TestTrue(TEXT("An alias of a supplied param is not suggested"),
			HasWarning(FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("echo"), Taken), TEXT("totl"), nullptr));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithParamSuggestStrictCallTest,
	"Monolith.ResponseShaping.ParamSuggest.PerCallStrict",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamSuggestStrictCallTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithParamSuggestTestDetail;
	const FString PriorEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("STRICT_PARAMS"));
	FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), TEXT(""));
	ON_SCOPE_EXIT
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), *PriorEnv);
		FMonolithToolRegistry::Get().UnregisterNamespace(Ns);
	};
	RegisterEcho();

	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), TEXT("x"));
		P->SetNumberField(TEXT("cuont"), 3);
		P->SetBoolField(TEXT("_strict"), true);
		const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("echo"), P);
		TestFalse(TEXT("_strict: true turns an unknown param into an error"), R.bSuccess);
		TestEqual(TEXT("Strict rejection is invalid params"), R.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("Strict error names the source"), R.ErrorMessage.StartsWith(TEXT("_strict")));
		TestTrue(TEXT("Strict error suggests the closest param"), R.ErrorMessage.Contains(TEXT("cuont (did you mean 'count'?)")));
		const TSharedPtr<FJsonObject> Entry = SoleErrorDataEntry(R, TEXT("unknown_params"));
		if (TestTrue(TEXT("Strict error carries one unknown_params entry"), Entry.IsValid()))
		{
			TestEqual(TEXT("unknown_params names the key"), Entry->GetStringField(TEXT("given")), FString(TEXT("cuont")));
			TestEqual(TEXT("unknown_params names the suggestion"), Entry->GetStringField(TEXT("param")), FString(TEXT("count")));
		}
	}
	{
		// A clean call with _strict runs, and the handler never sees the reserved key.
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), TEXT("x"));
		P->SetStringField(TEXT("_strict"), TEXT("true"));
		const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("echo"), P);
		TestTrue(TEXT("_strict on a clean call runs"), R.bSuccess);
		if (R.bSuccess)
		{
			TestFalse(TEXT("_strict is removed before the handler"), R.Result->GetBoolField(TEXT("saw_strict")));
		}
	}
	{
		// _strict: false keeps the soft warning.
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), TEXT("x"));
		P->SetNumberField(TEXT("cuont"), 3);
		P->SetBoolField(TEXT("_strict"), false);
		TestTrue(TEXT("_strict: false keeps the soft warning"), FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("echo"), P).bSuccess);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithParamSuggestMissingRequiredTest,
	"Monolith.ResponseShaping.ParamSuggest.MissingRequiredLinksUnknownKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamSuggestMissingRequiredTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithParamSuggestTestDetail;
	ON_SCOPE_EXIT { FMonolithToolRegistry::Get().UnregisterNamespace(Ns); };
	RegisterEcho();

	{
		// Close name: 'nmae' for 'name'.
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("nmae"), TEXT("x"));
		const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("echo"), P);
		TestFalse(TEXT("Missing required param fails"), R.bSuccess);
		TestEqual(TEXT("Missing required is invalid params"), R.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("Error links the typo to the missing param"), R.ErrorMessage.Contains(TEXT("Did you mean to pass 'nmae' as 'name'?")));
		const TSharedPtr<FJsonObject> Entry = SoleErrorDataEntry(R, TEXT("suggestions"));
		if (TestTrue(TEXT("Error carries one suggestions entry"), Entry.IsValid()))
		{
			TestEqual(TEXT("suggestions names the key"), Entry->GetStringField(TEXT("given")), FString(TEXT("nmae")));
			TestEqual(TEXT("suggestions names the param"), Entry->GetStringField(TEXT("param")), FString(TEXT("name")));
		}
	}
	{
		// Not close, but the only unknown key and the only missing param: 'label' for 'name'.
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("label"), TEXT("x"));
		const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("echo"), P);
		TestTrue(TEXT("A lone unknown key is linked to the lone missing param"), R.ErrorMessage.Contains(TEXT("'label' as 'name'")));
	}
	{
		// The only unknown key, but a typo of the optional 'count': not offered as 'name'.
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetNumberField(TEXT("cuont"), 3);
		const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("echo"), P);
		TestFalse(TEXT("A typo of another param is not linked to the missing one"), R.ErrorMessage.Contains(TEXT("Did you mean to pass")));
		TestFalse(TEXT("... and carries no suggestions data"), R.ErrorData.IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithParamSuggestRunPythonCodeAliasTest,
	"Monolith.ResponseShaping.ParamSuggest.RunPythonCodeAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamSuggestRunPythonCodeAliasTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("editor"), TEXT("run_python")))
	{
		AddInfo(TEXT("editor:run_python is not registered in this session; skipping"));
		return true;
	}
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("code"), TEXT("x = 1 + 1"));
		const FMonolithActionResult R = Registry.ExecuteAction(TEXT("editor"), TEXT("run_python"), P);
		// The alias is under test, not Python: without Python the handler is still reached.
		TestTrue(FString::Printf(TEXT("run_python accepts 'code' (%s)"), *R.ErrorMessage),
			R.bSuccess || R.ErrorMessage.Contains(TEXT("not available")));
	}
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("code"), TEXT("x = 1"));
		P->SetStringField(TEXT("command"), TEXT("x = 2"));
		const FMonolithActionResult R = Registry.ExecuteAction(TEXT("editor"), TEXT("run_python"), P);
		TestFalse(TEXT("'code' and 'command' together is the alias collision error"), R.bSuccess);
		TestTrue(TEXT("Collision error names both"), R.ErrorMessage.Contains(TEXT("collision")) && R.ErrorMessage.Contains(TEXT("code")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithParamSuggestPropertyNamesTest,
	"Monolith.ResponseShaping.ParamSuggest.PropertyNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamSuggestPropertyNamesTest::RunTest(const FString& /*Parameters*/)
{
	TestTrue(TEXT("Containment counts as close"), MonolithNameSuggest::Similarity(TEXT("Mesh"), TEXT("StaticMesh")) >= 0.6f);
	TestEqual(TEXT("Closest is case-insensitive and skips exact matches"),
		FString::Join(MonolithNameSuggest::Closest(TEXT("count"), { TEXT("Count"), TEXT("counter"), TEXT("zebra") }), TEXT(",")),
		FString(TEXT("counter")));
	TestEqual(TEXT("Empty suffix without names"), MonolithNameSuggest::DidYouMeanSuffix({}), FString());
	TestEqual(TEXT("Suffix with two names"), MonolithNameSuggest::DidYouMeanSuffix({ TEXT("A"), TEXT("B") }),
		FString(TEXT(" — did you mean 'A' or 'B'?")));

	// The 2026-11-18 case: a MaterialParameterCollection read with a wrong property name.
	const UClass* MpcClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.MaterialParameterCollection"));
	if (TestNotNull(TEXT("MaterialParameterCollection class"), MpcClass))
	{
		const FString Message = FMonolithReflectionWalker::UnknownFieldMessage(MpcClass, TEXT("ScalarParams"));
		TestTrue(FString::Printf(TEXT("Walker suggests ScalarParameters (%s)"), *Message),
			Message.Contains(TEXT("did you mean 'ScalarParameters'")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

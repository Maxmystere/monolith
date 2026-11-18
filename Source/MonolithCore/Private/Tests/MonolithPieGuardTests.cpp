// SPDX-License-Identifier: MIT
// =============================================================================
// MonolithPieGuardTests.cpp
//
// Registry PIE guard. PIE is faked with FMonolithToolRegistry::SetPieRunningOverride, so
// the tests run in any editor session without starting a play session.
//
//   - Monolith.Core.PieGuard.WriteRefusedDuringPie
//       a write (set_*) is refused with ErrPieRunning and its handler never runs;
//       allow_during_pie: true runs it and the handler never sees the key;
//       a read (get_*) runs; with PIE stopped the write runs.
//   - Monolith.Core.PieGuard.PolicyOverrides
//       AllowDuringPie lets a write-looking action through, Write blocks a read-looking one,
//       and bReadOnlyHint counts as a read.
//   - Monolith.Core.PieGuard.NestedCallsNotRechecked
//       a write allowed with allow_during_pie can dispatch another write.
//   - Monolith.Core.PieGuard.ReadNameClassification
//       spot checks of IsReadActionName.
// =============================================================================

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithPieGuardTestDetail
{
	static const TCHAR* Ns = TEXT("monolith_test_pie");

	/** Registers Action; the handler counts its calls and reports whether it saw allow_during_pie. */
	static void RegisterCounting(const TCHAR* Action, int32& Calls)
	{
		int32* CallsPtr = &Calls;
		FMonolithToolRegistry::Get().RegisterAction(Ns, Action, TEXT("Test."),
			FMonolithActionHandler::CreateLambda([CallsPtr](const TSharedPtr<FJsonObject>& Params)
			{
				++(*CallsPtr);
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetBoolField(TEXT("saw_allow_key"), Params->HasField(TEXT("allow_during_pie")));
				return FMonolithActionResult::Success(Result);
			}),
			FParamSchemaBuilder().Build());
	}

	static FMonolithActionResult Run(const TCHAR* Action, bool bAllow = false)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (bAllow)
		{
			Params->SetBoolField(TEXT("allow_during_pie"), true);
		}
		return FMonolithToolRegistry::Get().ExecuteAction(Ns, Action, Params);
	}

	struct FScopedFakePie
	{
		explicit FScopedFakePie(bool bRunning) { FMonolithToolRegistry::Get().SetPieRunningOverride(bRunning); }
		~FScopedFakePie()
		{
			FMonolithToolRegistry::Get().SetPieRunningOverride(TOptional<bool>());
			FMonolithToolRegistry::Get().UnregisterNamespace(Ns);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPieGuardWriteRefusedTest,
	"Monolith.Core.PieGuard.WriteRefusedDuringPie",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPieGuardWriteRefusedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPieGuardTestDetail;
	FScopedFakePie FakePie(true);
	int32 WriteCalls = 0;
	int32 ReadCalls = 0;
	RegisterCounting(TEXT("set_thing"), WriteCalls);
	RegisterCounting(TEXT("get_thing"), ReadCalls);

	const FMonolithActionResult Refused = Run(TEXT("set_thing"));
	TestFalse(TEXT("Write is refused during PIE"), Refused.bSuccess);
	TestEqual(TEXT("Refusal code is ErrPieRunning"), Refused.ErrorCode, FMonolithJsonUtils::ErrPieRunning);
	TestTrue(TEXT("Refusal names the escape hatch"), Refused.ErrorMessage.Contains(TEXT("allow_during_pie")));
	TestEqual(TEXT("Refused handler never ran"), WriteCalls, 0);

	const FMonolithActionResult Allowed = Run(TEXT("set_thing"), /*bAllow=*/true);
	TestTrue(TEXT("allow_during_pie: true runs the write"), Allowed.bSuccess);
	TestEqual(TEXT("Allowed handler ran once"), WriteCalls, 1);
	if (Allowed.bSuccess)
	{
		TestFalse(TEXT("allow_during_pie is removed before the handler"), Allowed.Result->GetBoolField(TEXT("saw_allow_key")));
	}

	TestTrue(TEXT("A read runs during PIE"), Run(TEXT("get_thing")).bSuccess);
	TestEqual(TEXT("Read handler ran"), ReadCalls, 1);

	FMonolithToolRegistry::Get().SetPieRunningOverride(false);
	TestTrue(TEXT("Without PIE the write runs"), Run(TEXT("set_thing")).bSuccess);
	TestEqual(TEXT("Write handler ran again"), WriteCalls, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPieGuardPolicyOverridesTest,
	"Monolith.Core.PieGuard.PolicyOverrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPieGuardPolicyOverridesTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPieGuardTestDetail;
	FScopedFakePie FakePie(true);
	int32 PieCalls = 0, ForcedWriteCalls = 0, HintCalls = 0;
	RegisterCounting(TEXT("pie_poke"), PieCalls);
	RegisterCounting(TEXT("get_but_writes"), ForcedWriteCalls);
	RegisterCounting(TEXT("frobnicate"), HintCalls);

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.SetActionPiePolicy(Ns, TEXT("pie_poke"), EMonolithPiePolicy::AllowDuringPie);
	Registry.SetActionPiePolicy(Ns, TEXT("get_but_writes"), EMonolithPiePolicy::Write);
	Registry.SetActionAnnotations(Ns, TEXT("frobnicate"), /*bReadOnly=*/true, false, true, FString());

	TestTrue(TEXT("AllowDuringPie runs a write-looking action"), Run(TEXT("pie_poke")).bSuccess);
	TestEqual(TEXT("Write policy blocks a read-looking action"), Run(TEXT("get_but_writes")).ErrorCode, FMonolithJsonUtils::ErrPieRunning);
	TestTrue(TEXT("bReadOnlyHint counts as a read"), Run(TEXT("frobnicate")).bSuccess);
	TestEqual(TEXT("Handler call counts"), PieCalls + ForcedWriteCalls * 10 + HintCalls * 100, 101);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPieGuardNestedCallsTest,
	"Monolith.Core.PieGuard.NestedCallsNotRechecked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPieGuardNestedCallsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPieGuardTestDetail;
	FScopedFakePie FakePie(true);
	int32 InnerCalls = 0;
	RegisterCounting(TEXT("set_inner"), InnerCalls);

	bool bInnerSucceeded = false;
	FMonolithToolRegistry::Get().RegisterAction(Ns, TEXT("set_outer"), TEXT("Test."),
		FMonolithActionHandler::CreateLambda([&bInnerSucceeded](const TSharedPtr<FJsonObject>&)
		{
			bInnerSucceeded = FMonolithToolRegistry::Get().ExecuteAction(Ns, TEXT("set_inner"), MakeShared<FJsonObject>()).bSuccess;
			return FMonolithActionResult::Success(MakeShared<FJsonObject>());
		}),
		FParamSchemaBuilder().Build());

	TestTrue(TEXT("Outer write allowed explicitly"), Run(TEXT("set_outer"), /*bAllow=*/true).bSuccess);
	TestTrue(TEXT("Its nested write is not re-checked"), bInnerSucceeded);
	TestEqual(TEXT("Inner handler ran"), InnerCalls, 1);

	// A top-level call to the inner write is still refused.
	TestEqual(TEXT("Top-level inner write is refused"), Run(TEXT("set_inner")).ErrorCode, FMonolithJsonUtils::ErrPieRunning);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPieGuardReadNamesTest,
	"Monolith.Core.PieGuard.ReadNameClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPieGuardReadNamesTest::RunTest(const FString& /*Parameters*/)
{
	for (const TCHAR* Read : { TEXT("get_graph_data"), TEXT("list_dirty_packages"), TEXT("search_nodes"), TEXT("find_references"),
		TEXT("describe_action"), TEXT("export_graph"), TEXT("render_graph"), TEXT("validate_blueprint"), TEXT("capture_scene_preview") })
	{
		TestTrue(FString::Printf(TEXT("%s reads"), Read), FMonolithToolRegistry::IsReadActionName(Read));
	}
	for (const TCHAR* Write : { TEXT("remove_interface"), TEXT("compile_blueprint"), TEXT("set_cdo_property"), TEXT("batch_execute"),
		TEXT("run_python"), TEXT("duplicate_asset"), TEXT("getaway") })
	{
		TestFalse(FString::Printf(TEXT("%s writes"), Write), FMonolithToolRegistry::IsReadActionName(Write));
	}

	// The real registrations made by the editor and AI modules.
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	auto PolicyOf = [&Registry](const TCHAR* Namespace, const TCHAR* Action, EMonolithPiePolicy& Out)
	{
		for (const FMonolithActionInfo& Info : Registry.GetActions(Namespace))
		{
			if (Info.Action == Action)
			{
				Out = Info.PiePolicy;
				return true;
			}
		}
		return false;
	};
	EMonolithPiePolicy Policy = EMonolithPiePolicy::Auto;
	if (PolicyOf(TEXT("editor"), TEXT("stop_pie"), Policy))
	{
		TestTrue(TEXT("editor stop_pie runs during PIE"), Policy == EMonolithPiePolicy::AllowDuringPie);
	}
	if (PolicyOf(TEXT("editor"), TEXT("run_python"), Policy))
	{
		TestTrue(TEXT("editor run_python is a write"), Policy == EMonolithPiePolicy::Write);
	}
	if (PolicyOf(TEXT("ai"), TEXT("runtime_set_bb_value"), Policy))
	{
		TestTrue(TEXT("ai runtime_set_bb_value runs during PIE"), Policy == EMonolithPiePolicy::AllowDuringPie);
	}
	if (PolicyOf(TEXT("mesh"), TEXT("find_replace_mesh"), Policy))
	{
		TestTrue(TEXT("mesh find_replace_mesh is a write despite its find_ prefix"), Policy == EMonolithPiePolicy::Write);
	}
	if (PolicyOf(TEXT("describe"), TEXT("action_schema"), Policy))
	{
		TestTrue(TEXT("describe action_schema is a read"), Policy == EMonolithPiePolicy::Read);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

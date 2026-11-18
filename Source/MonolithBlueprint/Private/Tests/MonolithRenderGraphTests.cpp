// =============================================================================
// MonolithRenderGraphTests.cpp
//
// blueprint render_graph (pseudo-code render) and the get_graph_data read options.
//
//   - Monolith.Blueprint.RenderGraph.Lines
//       a test Blueprint with a branch, a merge point, a chain of pure nodes, a split struct
//       pin, a disabled node, a comment box and an unreachable exec node: checks the lines.
//   - Monolith.Blueprint.RenderGraph.GetGraphDataOptions
//       graph_names [] reads every graph, include_pin_types=false drops pin types, and
//       max_nodes/offset page with truncated/next_offset.
//   - Monolith.Blueprint.RenderGraph.SizeReport
//       render_graph vs get_graph_data (all graphs) on engine Blueprints; logs both sizes and
//       checks the render is smaller.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

#include "MonolithBlueprintInternal.h"
#include "MonolithToolRegistry.h"

namespace MonolithRenderGraphTests
{
	struct FFixture
	{
		UPackage* Package = nullptr;
		UBlueprint* BP = nullptr;
		FString Path;

		FFixture()
		{
			const FString Name = FString::Printf(TEXT("BP_RenderFixture_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
			Path = FString::Printf(TEXT("/Game/Tests/Monolith/RenderGraph/%s"), *Name);
			Package = CreatePackage(*Path);
			BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, FName(*Name),
				BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		}

		~FFixture()
		{
			if (BP) { BP->ClearFlags(RF_Standalone | RF_Public); BP->MarkAsGarbage(); }
			if (Package) { Package->SetDirtyFlag(false); Package->MarkAsGarbage(); }
		}
	};

	static FMonolithActionResult Run(const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), Action, Params);
	}

	static TSharedPtr<FJsonObject> P(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		return Params;
	}

	static FString AddCall(const FFixture& F, const TCHAR* Function, const TCHAR* TargetClass, int32 X, int32 Y)
	{
		TSharedPtr<FJsonObject> Params = P(F.Path);
		Params->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		Params->SetStringField(TEXT("function_name"), Function);
		Params->SetStringField(TEXT("target_class"), TargetClass);
		TArray<TSharedPtr<FJsonValue>> Pos;
		Pos.Add(MakeShared<FJsonValueNumber>(X));
		Pos.Add(MakeShared<FJsonValueNumber>(Y));
		Params->SetArrayField(TEXT("position"), Pos);
		const FMonolithActionResult R = Run(TEXT("add_node"), Params);
		return R.bSuccess ? R.Result->GetStringField(TEXT("id")) : FString();
	}

	static FString AddNodeOfType(const FFixture& F, const TCHAR* NodeType, int32 X, int32 Y, const TCHAR* Key = nullptr, const TCHAR* Value = nullptr)
	{
		TSharedPtr<FJsonObject> Params = P(F.Path);
		Params->SetStringField(TEXT("node_type"), NodeType);
		if (Key)
		{
			Params->SetStringField(Key, Value);
		}
		TArray<TSharedPtr<FJsonValue>> Pos;
		Pos.Add(MakeShared<FJsonValueNumber>(X));
		Pos.Add(MakeShared<FJsonValueNumber>(Y));
		Params->SetArrayField(TEXT("position"), Pos);
		const FMonolithActionResult R = Run(TEXT("add_node"), Params);
		return R.bSuccess ? R.Result->GetStringField(TEXT("id")) : FString();
	}

	static bool Connect(const FFixture& F, const FString& Src, const TCHAR* SrcPin, const FString& Dst, const TCHAR* DstPin)
	{
		TSharedPtr<FJsonObject> Params = P(F.Path);
		Params->SetStringField(TEXT("source_node"), Src);
		Params->SetStringField(TEXT("source_pin"), SrcPin);
		Params->SetStringField(TEXT("target_node"), Dst);
		Params->SetStringField(TEXT("target_pin"), DstPin);
		return Run(TEXT("connect_pins"), Params).bSuccess;
	}

	static bool SetDefault(const FFixture& F, const FString& Node, const TCHAR* Pin, const TCHAR* Value)
	{
		TSharedPtr<FJsonObject> Params = P(F.Path);
		Params->SetStringField(TEXT("node_id"), Node);
		Params->SetStringField(TEXT("pin_name"), Pin);
		Params->SetStringField(TEXT("value"), Value);
		return Run(TEXT("set_pin_default"), Params).bSuccess;
	}

	static UEdGraphNode* Find(const FFixture& F, const FString& NodeId)
	{
		return MonolithBlueprintInternal::FindNodeById(F.BP, FString(), NodeId);
	}

	static FString ToJson(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithRenderGraphLinesTest,
	"Monolith.Blueprint.RenderGraph.Lines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRenderGraphLinesTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithRenderGraphTests;
	FFixture F;
	if (!TestNotNull(TEXT("Fixture Blueprint"), F.BP))
	{
		return false;
	}

	// Run() -> if (Abs(-3) > 1) [true: PrintString "yes" (disabled)] -> merged PrintString <- [false]
	//       -> SetActorLocation(NewLocation split, X=5)
	const FString Event = AddNodeOfType(F, TEXT("CustomEvent"), 0, 2000, TEXT("event_name"), TEXT("Run"));
	const FString Branch = AddNodeOfType(F, TEXT("Branch"), 300, 2000);
	const FString Greater = AddCall(F, TEXT("Greater_DoubleDouble"), TEXT("KismetMathLibrary"), 100, 2200);
	const FString Abs = AddCall(F, TEXT("Abs"), TEXT("KismetMathLibrary"), -100, 2200);
	const FString Yes = AddCall(F, TEXT("PrintString"), TEXT("KismetSystemLibrary"), 600, 1900);
	const FString Merged = AddCall(F, TEXT("PrintString"), TEXT("KismetSystemLibrary"), 900, 2000);
	const FString Move = AddCall(F, TEXT("K2_SetActorLocation"), TEXT("Actor"), 1200, 2000);
	const FString Orphan = AddCall(F, TEXT("PrintString"), TEXT("KismetSystemLibrary"), 0, 2600);
	for (const FString* Id : { &Event, &Branch, &Greater, &Abs, &Yes, &Merged, &Move, &Orphan })
	{
		if (!TestFalse(TEXT("Node added"), Id->IsEmpty()))
		{
			return false;
		}
	}

	TestTrue(TEXT("event -> branch"), Connect(F, Event, TEXT("then"), Branch, TEXT("execute")));
	TestTrue(TEXT("abs -> greater.A"), Connect(F, Abs, TEXT("ReturnValue"), Greater, TEXT("A")));
	TestTrue(TEXT("greater -> condition"), Connect(F, Greater, TEXT("ReturnValue"), Branch, TEXT("Condition")));
	TestTrue(TEXT("true -> yes"), Connect(F, Branch, TEXT("then"), Yes, TEXT("execute")));
	TestTrue(TEXT("yes -> merged"), Connect(F, Yes, TEXT("then"), Merged, TEXT("execute")));
	TestTrue(TEXT("false -> merged"), Connect(F, Branch, TEXT("else"), Merged, TEXT("execute")));
	TestTrue(TEXT("merged -> move"), Connect(F, Merged, TEXT("then"), Move, TEXT("execute")));
	TestTrue(TEXT("abs input"), SetDefault(F, Abs, TEXT("A"), TEXT("-3.0")));
	TestTrue(TEXT("greater B"), SetDefault(F, Greater, TEXT("B"), TEXT("1.0")));
	TestTrue(TEXT("yes text"), SetDefault(F, Yes, TEXT("InString"), TEXT("yes")));
	TestTrue(TEXT("merged text"), SetDefault(F, Merged, TEXT("InString"), TEXT("merged")));
	TestTrue(TEXT("orphan text"), SetDefault(F, Orphan, TEXT("InString"), TEXT("orphan")));

	// Disabled node, split struct pin, comment box around the merge point.
	Find(F, Yes)->SetEnabledState(ENodeEnabledState::Disabled);
	UEdGraphNode* MoveNode = Find(F, Move);
	UEdGraphPin* NewLocation = MoveNode ? MoveNode->FindPin(TEXT("NewLocation")) : nullptr;
	if (TestNotNull(TEXT("NewLocation pin"), NewLocation))
	{
		GetDefault<UEdGraphSchema_K2>()->SplitPin(NewLocation, /*bNotify=*/false);
		UEdGraphPin* X = MoveNode->FindPin(TEXT("NewLocation_X"));
		if (TestNotNull(TEXT("NewLocation_X sub-pin"), X))
		{
			X->DefaultValue = TEXT("5.0");
		}
	}
	UEdGraph* Graph = F.BP->UbergraphPages[0];
	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph);
	Comment->NodePosX = 850;
	Comment->NodePosY = 1950;
	Comment->NodeWidth = 200;
	Comment->NodeHeight = 200;
	Comment->NodeComment = TEXT("Merge here");
	Graph->AddNode(Comment, false, false);

	TSharedPtr<FJsonObject> Params = P(F.Path);
	TArray<TSharedPtr<FJsonValue>> Names;
	Names.Add(MakeShared<FJsonValueString>(Graph->GetName()));
	Params->SetArrayField(TEXT("graph_names"), Names);
	Params->SetBoolField(TEXT("include_positions"), false);
	const FMonolithActionResult R = Run(TEXT("render_graph"), Params);
	if (!TestTrue(FString::Printf(TEXT("render_graph succeeds (%s)"), *R.ErrorMessage), R.bSuccess))
	{
		return false;
	}
	const FString Text = R.Result->GetStringField(TEXT("text"));
	AddInfo(TEXT("Rendered:\n") + Text);

	auto Has = [this, &Text](const TCHAR* What, const TCHAR* Needle)
	{
		TestTrue(FString::Printf(TEXT("%s: '%s'"), What, Needle), Text.Contains(Needle));
	};
	Has(TEXT("graph header"), TEXT("== event graph EventGraph =="));
	Has(TEXT("event header"), TEXT("Run():"));
	Has(TEXT("branch with inlined pure chain"), TEXT("if Greater_DoubleDouble(A=Abs(A=-3.0), B=1.0)"));
	Has(TEXT("true label"), TEXT("true:"));
	Has(TEXT("false label"), TEXT("false:"));
	Has(TEXT("disabled tag"), TEXT("PrintString(InString=\"yes\") [DISABLED]"));
	Has(TEXT("merge label"), TEXT("L1: "));
	Has(TEXT("goto"), TEXT("goto L1 (PrintString)"));
	Has(TEXT("comment box"), TEXT("# [Merge here]"));
	Has(TEXT("split struct folded"), TEXT("NewLocation=(X=5.0)"));
	Has(TEXT("unreachable section"), TEXT("# not reachable from any entry"));
	Has(TEXT("unreachable node"), TEXT("PrintString(InString=\"orphan\")"));
	TestFalse(TEXT("no positions when include_positions=false"), Text.Contains(TEXT(" @(")));
	TestFalse(TEXT("unlinked self pins are omitted"), Text.Contains(TEXT("self=")));

	// The comment line comes right before the merged statement.
	const int32 CommentAt = Text.Find(TEXT("# [Merge here]"));
	const int32 MergedAt = Text.Find(TEXT("PrintString(InString=\"merged\")"));
	TestTrue(TEXT("comment precedes the first node it encloses"), CommentAt != INDEX_NONE && MergedAt != INDEX_NONE && CommentAt < MergedAt);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithRenderGraphReadOptionsTest,
	"Monolith.Blueprint.RenderGraph.GetGraphDataOptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRenderGraphReadOptionsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithRenderGraphTests;
	FFixture F;
	if (!TestNotNull(TEXT("Fixture Blueprint"), F.BP))
	{
		return false;
	}

	{
		TSharedPtr<FJsonObject> Params = P(F.Path);
		Params->SetArrayField(TEXT("graph_names"), TArray<TSharedPtr<FJsonValue>>());
		Params->SetBoolField(TEXT("include_pin_types"), false);
		const FMonolithActionResult R = Run(TEXT("get_graph_data"), Params);
		if (TestTrue(TEXT("graph_names [] succeeds"), R.bSuccess))
		{
			const TArray<TSharedPtr<FJsonValue>>& Graphs = R.Result->GetArrayField(TEXT("graphs"));
			TestTrue(TEXT("all graphs returned (event graph + construction script)"), Graphs.Num() >= 2);
			bool bAnyType = false;
			for (const TSharedPtr<FJsonValue>& G : Graphs)
			{
				for (const TSharedPtr<FJsonValue>& N : G->AsObject()->GetArrayField(TEXT("nodes")))
				{
					for (const TSharedPtr<FJsonValue>& Pin : N->AsObject()->GetArrayField(TEXT("pins")))
					{
						bAnyType |= Pin->AsObject()->HasField(TEXT("type"));
					}
				}
			}
			TestFalse(TEXT("include_pin_types=false drops pin types"), bAnyType);
		}
	}
	{
		TSharedPtr<FJsonObject> Params = P(F.Path);
		Params->SetArrayField(TEXT("graph_names"), TArray<TSharedPtr<FJsonValue>>());
		Params->SetNumberField(TEXT("max_nodes"), 1);
		const FMonolithActionResult R = Run(TEXT("get_graph_data"), Params);
		if (TestTrue(TEXT("paged read succeeds"), R.bSuccess))
		{
			TestEqual(TEXT("one node returned"), static_cast<int32>(R.Result->GetNumberField(TEXT("returned_nodes"))), 1);
			TestTrue(TEXT("truncated"), R.Result->GetBoolField(TEXT("truncated")));
			TestEqual(TEXT("next_offset"), static_cast<int32>(R.Result->GetNumberField(TEXT("next_offset"))), 1);
		}
	}
	{
		TSharedPtr<FJsonObject> Params = P(F.Path);
		TArray<TSharedPtr<FJsonValue>> Names;
		Names.Add(MakeShared<FJsonValueString>(TEXT("NoSuchGraph")));
		Params->SetArrayField(TEXT("graph_names"), Names);
		const FMonolithActionResult R = Run(TEXT("render_graph"), Params);
		TestFalse(TEXT("unknown graph name is refused"), R.bSuccess);
		TestTrue(TEXT("refusal lists available graphs"), R.ErrorMessage.Contains(TEXT("EventGraph")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithRenderGraphSizeReportTest,
	"Monolith.Blueprint.RenderGraph.SizeReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRenderGraphSizeReportTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithRenderGraphTests;
	for (const TCHAR* Asset : { TEXT("/Engine/EngineSky/BP_Sky_Sphere"), TEXT("/Engine/EditorBlueprintResources/StandardMacros") })
	{
		TSharedPtr<FJsonObject> DataParams = P(Asset);
		DataParams->SetArrayField(TEXT("graph_names"), TArray<TSharedPtr<FJsonValue>>());
		const FMonolithActionResult Data = Run(TEXT("get_graph_data"), DataParams);
		const FMonolithActionResult Render = Run(TEXT("render_graph"), P(Asset));
		if (!Data.bSuccess || !Render.bSuccess)
		{
			AddInfo(FString::Printf(TEXT("%s not available here (%s%s); skipped"), Asset, *Data.ErrorMessage, *Render.ErrorMessage));
			continue;
		}
		const int32 GraphCount = Data.Result->GetArrayField(TEXT("graphs")).Num();
		const int32 JsonChars = ToJson(Data.Result).Len();
		const FString Text = Render.Result->GetStringField(TEXT("text"));
		AddInfo(FString::Printf(TEXT("%s: %d graphs, get_graph_data (all graphs, condensed JSON) %d chars, render_graph %d chars (%.1f%%)"),
			Asset, GraphCount, JsonChars, Text.Len(), JsonChars > 0 ? 100.0 * Text.Len() / JsonChars : 0.0));
		TestTrue(FString::Printf(TEXT("%s renders smaller than its graph data"), Asset), Text.Len() < JsonChars);
		AddInfo(FString::Printf(TEXT("%s render (first 4000 chars):\n%s"), Asset, *Text.Left(4000)));
		TestEqual(FString::Printf(TEXT("%s renders every graph"), Asset), Render.Result->GetArrayField(TEXT("graphs")).Num(), GraphCount);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

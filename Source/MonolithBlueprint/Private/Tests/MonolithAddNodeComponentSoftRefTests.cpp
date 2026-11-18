// =============================================================================
// MonolithAddNodeComponentSoftRefTests.cpp
//
// add_node / resolve_node for "Add Component by Class" (UK2Node_AddComponentByClass) and
// "Resolve Soft Reference" (UK2Node_ConvertAsset), in an ActorComponent Blueprint.
//
//   - Monolith.Blueprint.AddNode.AddComponentByClass
//       component_class=NiagaraComponent types the return pin object:NiagaraComponent;
//       an unknown / non-component / abstract class is refused and leaves no node behind;
//       resolve_node reports the same typed return pin.
//   - Monolith.Blueprint.AddNode.ConvertAsset
//       the node starts with wildcard pins; connecting a softobject:NiagaraSystem variable
//       retypes the output to object:NiagaraSystem; ResolveSoftReference is accepted as an alias.
//   - Monolith.Blueprint.AddNode.ComponentSoftRefCompiles
//       CustomEvent -> AddComponentByClass(Target=GetOwner) -> Branch(IsValid(ConvertAsset(var)))
//       compiles without errors.
//
// The Blueprint lives in an in-memory package and is never saved.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Components/ActorComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

#include "MonolithPinTypeGrammar.h"
#include "MonolithToolRegistry.h"

namespace MonolithAddNodeComponentSoftRefTests
{
	struct FFixture
	{
		UPackage* Package = nullptr;
		UBlueprint* BP = nullptr;
		FString Path;

		~FFixture()
		{
			if (BP)
			{
				BP->ClearFlags(RF_Standalone | RF_Public);
				BP->MarkAsGarbage();
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}
	};

	static bool MakeComponentBlueprint(FFixture& Out, const TCHAR* Tag)
	{
		const FString Name = FString::Printf(TEXT("BP_AddNodeFixture_%s_%s"), Tag, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
		Out.Path = FString::Printf(TEXT("/Game/Tests/Monolith/AddNode/%s"), *Name);
		Out.Package = CreatePackage(*Out.Path);
		if (!Out.Package)
		{
			return false;
		}
		Out.BP = FKismetEditorUtilities::CreateBlueprint(UActorComponent::StaticClass(), Out.Package, FName(*Name),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		return Out.BP != nullptr;
	}

	static FMonolithActionResult Run(const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), Action, Params);
	}

	static TSharedPtr<FJsonObject> AddNodeParams(const FFixture& F, const TCHAR* NodeType)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), F.Path);
		P->SetStringField(TEXT("node_type"), NodeType);
		return P;
	}

	static UEdGraphNode* FindNode(const FFixture& F, const FString& NodeId)
	{
		TArray<UEdGraph*> Graphs;
		F.BP->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->GetName() == NodeId)
				{
					return Node;
				}
			}
		}
		return nullptr;
	}

	static FString AddNode(FAutomationTestBase& Test, const FFixture& F, const TSharedPtr<FJsonObject>& Params)
	{
		const FMonolithActionResult Res = Run(TEXT("add_node"), Params);
		if (!Test.TestTrue(FString::Printf(TEXT("add_node %s succeeds (%s)"), *Params->GetStringField(TEXT("node_type")), *Res.ErrorMessage), Res.bSuccess))
		{
			return FString();
		}
		return Res.Result->GetStringField(TEXT("id"));
	}

	static bool Connect(const FFixture& F, const FString& SrcNode, const TCHAR* SrcPin, const FString& DstNode, const TCHAR* DstPin)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), F.Path);
		P->SetStringField(TEXT("source_node"), SrcNode);
		P->SetStringField(TEXT("source_pin"), SrcPin);
		P->SetStringField(TEXT("target_node"), DstNode);
		P->SetStringField(TEXT("target_pin"), DstPin);
		return Run(TEXT("connect_pins"), P).bSuccess;
	}

	static FString PinTypeString(const UEdGraphPin* Pin)
	{
		return Pin ? MonolithPinTypeGrammar::PinTypeToString(Pin->PinType) : FString(TEXT("<no pin>"));
	}

	static UClass* NiagaraClass(const TCHAR* Name)
	{
		return FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Niagara.%s"), Name));
	}

	/** Adds a TSoftObjectPtr<UNiagaraSystem> member variable named SoftSystem. */
	static bool AddSoftSystemVariable(const FFixture& F)
	{
		UClass* SystemClass = NiagaraClass(TEXT("NiagaraSystem"));
		if (!SystemClass)
		{
			return false;
		}
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
		Type.PinSubCategoryObject = SystemClass;
		return FBlueprintEditorUtils::AddMemberVariable(F.BP, TEXT("SoftSystem"), Type);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAddNodeAddComponentByClassTest,
	"Monolith.Blueprint.AddNode.AddComponentByClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAddNodeAddComponentByClassTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithAddNodeComponentSoftRefTests;
	FFixture F;
	if (!TestTrue(TEXT("Fixture Blueprint created"), MakeComponentBlueprint(F, TEXT("AddComp"))))
	{
		return false;
	}
	if (!TestNotNull(TEXT("NiagaraComponent class is loaded"), NiagaraClass(TEXT("NiagaraComponent"))))
	{
		return false;
	}

	TSharedPtr<FJsonObject> P = AddNodeParams(F, TEXT("AddComponentByClass"));
	P->SetStringField(TEXT("component_class"), TEXT("NiagaraComponent"));
	const FString NodeId = AddNode(*this, F, P);
	UEdGraphNode* Node = FindNode(F, NodeId);
	if (!TestNotNull(TEXT("Node exists"), Node))
	{
		return false;
	}
	TestEqual(TEXT("Node class"), Node->GetClass()->GetName(), FString(TEXT("K2Node_AddComponentByClass")));
	TestEqual(TEXT("Return pin is typed by component_class"),
		PinTypeString(Node->FindPin(UEdGraphSchema_K2::PN_ReturnValue)), FString(TEXT("object:NiagaraComponent")));
	const UEdGraphPin* ClassPin = Node->FindPin(TEXT("Class"));
	TestTrue(TEXT("Class pin default is the component class"),
		ClassPin && ClassPin->DefaultObject == NiagaraClass(TEXT("NiagaraComponent")));

	// The alias spelling and no class: the node is created with an ActorComponent return pin.
	const FString BareId = AddNode(*this, F, AddNodeParams(F, TEXT("add_component_by_class")));
	TestEqual(TEXT("Without component_class the return pin is ActorComponent"),
		PinTypeString(FindNode(F, BareId) ? FindNode(F, BareId)->FindPin(UEdGraphSchema_K2::PN_ReturnValue) : nullptr),
		FString(TEXT("object:ActorComponent")));

	// Refusals leave no node behind.
	const int32 NodesBefore = F.BP->UbergraphPages[0]->Nodes.Num();
	for (const TCHAR* Bad : { TEXT("NoSuchComponentClassXyz"), TEXT("Actor"), TEXT("ActorComponent") })
	{
		TSharedPtr<FJsonObject> BadParams = AddNodeParams(F, TEXT("AddComponentByClass"));
		BadParams->SetStringField(TEXT("component_class"), Bad);
		const FMonolithActionResult Res = Run(TEXT("add_node"), BadParams);
		TestFalse(FString::Printf(TEXT("component_class=%s is refused"), Bad), Res.bSuccess);
	}
	TestEqual(TEXT("Refused add_node calls leave no node"), F.BP->UbergraphPages[0]->Nodes.Num(), NodesBefore);

	// resolve_node dry run.
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("node_type"), TEXT("AddComponentByClass"));
	R->SetStringField(TEXT("component_class"), TEXT("/Script/Niagara.NiagaraComponent"));
	const FMonolithActionResult Resolved = Run(TEXT("resolve_node"), R);
	if (TestTrue(TEXT("resolve_node AddComponentByClass succeeds"), Resolved.bSuccess))
	{
		bool bFoundReturn = false;
		for (const TSharedPtr<FJsonValue>& PinValue : Resolved.Result->GetArrayField(TEXT("pins")))
		{
			const TSharedPtr<FJsonObject> Pin = PinValue->AsObject();
			if (Pin->GetStringField(TEXT("name")) == UEdGraphSchema_K2::PN_ReturnValue.ToString())
			{
				bFoundReturn = true;
				TestEqual(TEXT("resolve_node return pin type"), Pin->GetStringField(TEXT("type")), FString(TEXT("object:NiagaraComponent")));
			}
		}
		TestTrue(TEXT("resolve_node lists the return pin"), bFoundReturn);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAddNodeConvertAssetTest,
	"Monolith.Blueprint.AddNode.ConvertAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAddNodeConvertAssetTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithAddNodeComponentSoftRefTests;
	FFixture F;
	if (!TestTrue(TEXT("Fixture Blueprint created"), MakeComponentBlueprint(F, TEXT("Convert"))))
	{
		return false;
	}
	if (!TestTrue(TEXT("SoftSystem variable added"), AddSoftSystemVariable(F)))
	{
		return false;
	}

	const FString ConvertId = AddNode(*this, F, AddNodeParams(F, TEXT("ResolveSoftReference")));
	UEdGraphNode* Convert = FindNode(F, ConvertId);
	if (!TestNotNull(TEXT("ConvertAsset node exists"), Convert))
	{
		return false;
	}
	TestEqual(TEXT("Node class"), Convert->GetClass()->GetName(), FString(TEXT("K2Node_ConvertAsset")));
	TestEqual(TEXT("Output starts as a wildcard"), PinTypeString(Convert->FindPin(TEXT("Output"))), FString(TEXT("wildcard")));

	TSharedPtr<FJsonObject> GetParams = AddNodeParams(F, TEXT("VariableGet"));
	GetParams->SetStringField(TEXT("variable_name"), TEXT("SoftSystem"));
	const FString GetId = AddNode(*this, F, GetParams);

	TestTrue(TEXT("Soft variable connects to ConvertAsset input"), Connect(F, GetId, TEXT("SoftSystem"), ConvertId, TEXT("Input")));
	TestEqual(TEXT("Input retyped to the soft type"), PinTypeString(Convert->FindPin(TEXT("Input"))), FString(TEXT("softobject:NiagaraSystem")));
	TestEqual(TEXT("Output retyped to the hard type"), PinTypeString(Convert->FindPin(TEXT("Output"))), FString(TEXT("object:NiagaraSystem")));

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("node_type"), TEXT("convert_asset"));
	const FMonolithActionResult Resolved = Run(TEXT("resolve_node"), R);
	TestTrue(TEXT("resolve_node ConvertAsset succeeds"), Resolved.bSuccess);
	if (Resolved.bSuccess)
	{
		TestEqual(TEXT("resolve_node class"), Resolved.Result->GetStringField(TEXT("resolved_class")), FString(TEXT("K2Node_ConvertAsset")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAddNodeComponentSoftRefCompilesTest,
	"Monolith.Blueprint.AddNode.ComponentSoftRefCompiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAddNodeComponentSoftRefCompilesTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithAddNodeComponentSoftRefTests;
	FFixture F;
	if (!TestTrue(TEXT("Fixture Blueprint created"), MakeComponentBlueprint(F, TEXT("Compile"))) ||
		!TestTrue(TEXT("SoftSystem variable added"), AddSoftSystemVariable(F)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> EventParams = AddNodeParams(F, TEXT("CustomEvent"));
	EventParams->SetStringField(TEXT("event_name"), TEXT("SpawnFx"));
	const FString EventId = AddNode(*this, F, EventParams);

	TSharedPtr<FJsonObject> OwnerParams = AddNodeParams(F, TEXT("CallFunction"));
	OwnerParams->SetStringField(TEXT("function_name"), TEXT("GetOwner"));
	OwnerParams->SetStringField(TEXT("target_class"), TEXT("ActorComponent"));
	const FString OwnerId = AddNode(*this, F, OwnerParams);

	TSharedPtr<FJsonObject> AddParams = AddNodeParams(F, TEXT("AddComponentByClass"));
	AddParams->SetStringField(TEXT("component_class"), TEXT("NiagaraComponent"));
	const FString AddId = AddNode(*this, F, AddParams);

	TSharedPtr<FJsonObject> GetParams = AddNodeParams(F, TEXT("VariableGet"));
	GetParams->SetStringField(TEXT("variable_name"), TEXT("SoftSystem"));
	const FString GetId = AddNode(*this, F, GetParams);
	const FString ConvertId = AddNode(*this, F, AddNodeParams(F, TEXT("ConvertAsset")));
	const FString IsValidId = AddNode(*this, F, AddNodeParams(F, TEXT("IsValid")));
	const FString BranchId = AddNode(*this, F, AddNodeParams(F, TEXT("Branch")));

	TestTrue(TEXT("event -> add component"), Connect(F, EventId, TEXT("then"), AddId, TEXT("execute")));
	TestTrue(TEXT("GetOwner -> Target"), Connect(F, OwnerId, TEXT("ReturnValue"), AddId, TEXT("self")));
	TestTrue(TEXT("add component -> branch"), Connect(F, AddId, TEXT("then"), BranchId, TEXT("execute")));
	TestTrue(TEXT("soft var -> convert"), Connect(F, GetId, TEXT("SoftSystem"), ConvertId, TEXT("Input")));
	TestTrue(TEXT("convert -> IsValid"), Connect(F, ConvertId, TEXT("Output"), IsValidId, TEXT("Object")));
	TestTrue(TEXT("IsValid -> branch condition"), Connect(F, IsValidId, TEXT("ReturnValue"), BranchId, TEXT("Condition")));

	FKismetEditorUtilities::CompileBlueprint(F.BP, EBlueprintCompileOptions::SkipGarbageCollection);
	TestNotEqual(TEXT("Blueprint compiles without errors"), static_cast<int32>(F.BP->Status), static_cast<int32>(BS_Error));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

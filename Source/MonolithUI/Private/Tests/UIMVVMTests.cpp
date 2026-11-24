// =============================================================================
// UIMVVMTests.cpp - ui mvvm_* actions (compiled only with WITH_MVVM).
//
//   - Monolith.UI.MVVM.ConditionsExtensionsRename
//       A Blueprint viewmodel with FieldNotify variables, an entry Widget Blueprint holding
//       that viewmodel, and a Widget Blueprint with a TextBlock, a Button and a VerticalBox.
//       Conditions (add / remove), event arguments (path and literal), a panel extension
//       (add / read back / remove), a viewmodel rename that bindings and conditions follow,
//       mvvm_get_view's widget filter on a binding whose widget source sits in a conversion
//       pin, and mvvm_remove_viewmodel counting and removing the conditions that use it.
//   - Monolith.UI.MVVM.RefusalsLeaveNoView
//       Refused writes on a Widget Blueprint without an MVVM view do not create one
//       (unknown conversion argument, bad condition path, non-panel widget); only
//       mvvm_get_view is annotated read-only.
// =============================================================================

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_MVVM

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MVVMViewModelBase.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

#include "MonolithToolRegistry.h"
#include "MonolithUIInternal.h"

namespace MonolithUIMVVMTests
{
    FString Unique(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    }

    void Discard(UObject* Asset)
    {
        if (Asset)
        {
            Asset->ClearFlags(RF_Standalone | RF_Public);
            Asset->MarkAsGarbage();
            if (UPackage* Package = Asset->GetPackage())
            {
                Package->SetDirtyFlag(false);
                Package->MarkAsGarbage();
            }
        }
    }

    /** A Blueprint viewmodel (parent UMVVMViewModelBase) with FieldNotify variables. */
    UBlueprint* MakeViewModel()
    {
        const FString Name = Unique(TEXT("VM_MonolithTest"));
        UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/Tests/Monolith/MVVM/%s"), *Name));
        UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(UMVVMViewModelBase::StaticClass(), Package, FName(*Name),
            BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
        if (!BP)
        {
            return nullptr;
        }
        auto AddVar = [BP](const TCHAR* VarName, FName Category, FName SubCategory = NAME_None)
        {
            FEdGraphPinType Type;
            Type.PinCategory = Category;
            Type.PinSubCategory = SubCategory;
            FBlueprintEditorUtils::AddMemberVariable(BP, VarName, Type);
            FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, VarName, nullptr, FBlueprintMetadata::MD_FieldNotify, TEXT(""));
        };
        AddVar(TEXT("bIsShown"), UEdGraphSchema_K2::PC_Boolean);
        AddVar(TEXT("Label"), UEdGraphSchema_K2::PC_Text);
        AddVar(TEXT("Ratio"), UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double);
        FKismetEditorUtilities::CompileBlueprint(BP);
        return BP;
    }

    UWidgetBlueprint* MakeWidgetBlueprint(const TCHAR* Prefix)
    {
        FMonolithActionResult Error;
        return MonolithUIInternal::CreateNewWidgetBlueprint(FString::Printf(TEXT("/Game/Tests/Monolith/MVVM/%s"), *Unique(Prefix)), Error);
    }

    template <typename TWidget>
    TWidget* AddChild(UWidgetBlueprint* WBP, const TCHAR* Name)
    {
        TWidget* Widget = WBP->WidgetTree->ConstructWidget<TWidget>(TWidget::StaticClass(), Name);
        Widget->bIsVariable = true;
        Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget)->AddChild(Widget);
        // A widget added outside the designer needs its variable GUID before the next compile.
        MonolithUIInternal::ReconcileWidgetVariableGuids(WBP);
        return Widget;
    }

    FMonolithActionResult Run(const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
    {
        return FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), Action, Params);
    }

    TSharedPtr<FJsonObject> P(const UWidgetBlueprint* WBP)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        return Params;
    }

    TSharedPtr<FJsonObject> Binding(const UWidgetBlueprint* WBP, const TCHAR* Source, const TCHAR* Destination)
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("source"), Source);
        Params->SetStringField(TEXT("destination"), Destination);
        return Params;
    }

    /** The pin of an event/condition/conversion JSON whose last name is Name. */
    TSharedPtr<FJsonObject> FindPin(const TSharedPtr<FJsonObject>& Owner, const TCHAR* Name)
    {
        for (const TSharedPtr<FJsonValue>& Value : Owner->GetArrayField(TEXT("pins")))
        {
            const TSharedPtr<FJsonObject> Pin = Value->AsObject();
            const TArray<TSharedPtr<FJsonValue>>& Names = Pin->GetArrayField(TEXT("names"));
            if (Names.Num() > 0 && Names.Last()->AsString() == Name)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    TSharedPtr<FJsonObject> GetView(const UWidgetBlueprint* WBP, const TCHAR* Widget = nullptr)
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        if (Widget)
        {
            Params->SetStringField(TEXT("widget"), Widget);
        }
        const FMonolithActionResult R = Run(TEXT("mvvm_get_view"), Params);
        return R.bSuccess ? R.Result : nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMVVMConditionsExtensionsRenameTest,
    "Monolith.UI.MVVM.ConditionsExtensionsRename",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMVVMConditionsExtensionsRenameTest::RunTest(const FString& /*Parameters*/)
{
    using namespace MonolithUIMVVMTests;

    UBlueprint* ViewModelBP = MakeViewModel();
    UWidgetBlueprint* Entry = MakeWidgetBlueprint(TEXT("WBP_MonolithMVVMEntry"));
    UWidgetBlueprint* WBP = MakeWidgetBlueprint(TEXT("WBP_MonolithMVVM"));
    ON_SCOPE_EXIT { Discard(WBP); Discard(Entry); Discard(ViewModelBP); };
    if (!TestNotNull(TEXT("viewmodel Blueprint"), ViewModelBP) || !TestNotNull(TEXT("entry widget"), Entry) || !TestNotNull(TEXT("widget"), WBP)
        || !TestNotNull(TEXT("viewmodel class compiled"), ViewModelBP->GeneratedClass.Get()))
    {
        return false;
    }
    const FString ViewModelClass = ViewModelBP->GeneratedClass->GetPathName();

    // Entry widget with its own viewmodel, for the panel extension.
    {
        TSharedPtr<FJsonObject> Params = P(Entry);
        Params->SetStringField(TEXT("viewmodel_class"), ViewModelClass);
        Params->SetStringField(TEXT("name"), TEXT("EntryVM"));
        Params->SetStringField(TEXT("creation_type"), TEXT("Manual"));
        const FMonolithActionResult R = Run(TEXT("mvvm_add_viewmodel"), Params);
        if (!TestTrue(FString::Printf(TEXT("entry viewmodel added (%s)"), *R.ErrorMessage), R.bSuccess))
        {
            return false;
        }
    }

    AddChild<UTextBlock>(WBP, TEXT("MyText"));
    AddChild<UButton>(WBP, TEXT("MyButton"));
    AddChild<UVerticalBox>(WBP, TEXT("MyList"));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("viewmodel_class"), ViewModelClass);
        Params->SetStringField(TEXT("name"), TEXT("VM"));
        const FMonolithActionResult R = Run(TEXT("mvvm_add_viewmodel"), Params);
        if (!TestTrue(FString::Printf(TEXT("viewmodel added (%s)"), *R.ErrorMessage), R.bSuccess))
        {
            return false;
        }
    }

    // A widget source behind a conversion function: the binding's SourcePath is cleared and the
    // widget lives in the conversion's argument pin, which the widget filter must still find.
    {
        TSharedPtr<FJsonObject> Params = Binding(WBP, TEXT("widget:MyButton.bIsEnabled"), TEXT("widget:MyText.Text"));
        Params->SetStringField(TEXT("conversion_function"), TEXT("KismetTextLibrary.Conv_BoolToText"));
        const FMonolithActionResult R = Run(TEXT("mvvm_add_binding"), Params);
        TestTrue(FString::Printf(TEXT("binding with a conversion function (%s)"), *R.ErrorMessage), R.bSuccess);
    }
    TestTrue(TEXT("plain binding"), Run(TEXT("mvvm_add_binding"), Binding(WBP, TEXT("viewmodel:VM.Label"), TEXT("widget:MyText.ToolTipText"))).bSuccess);

    // Event with an argument bound by path, then switched to a literal.
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("event"), TEXT("widget:MyButton.OnClicked"));
        Params->SetStringField(TEXT("destination"), TEXT("self:SetIsEnabled"));
        const FMonolithActionResult R = Run(TEXT("mvvm_add_event"), Params);
        TestTrue(FString::Printf(TEXT("event (%s)"), *R.ErrorMessage), R.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetNumberField(TEXT("event_index"), 0);
        Params->SetStringField(TEXT("argument"), TEXT("bInIsEnabled"));
        Params->SetStringField(TEXT("path"), TEXT("viewmodel:VM.bIsShown"));
        const FMonolithActionResult R = Run(TEXT("mvvm_set_event_argument"), Params);
        if (TestTrue(FString::Printf(TEXT("event argument by path (%s)"), *R.ErrorMessage), R.bSuccess))
        {
            const TSharedPtr<FJsonObject> Pin = FindPin(R.Result->GetObjectField(TEXT("event")), TEXT("bInIsEnabled"));
            TestTrue(TEXT("argument uses the viewmodel path"), Pin.IsValid() && Pin->GetBoolField(TEXT("uses_path"))
                && Pin->GetObjectField(TEXT("source_path"))->GetStringField(TEXT("path")) == TEXT("viewmodel:VM.bIsShown"));
        }
    }
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetNumberField(TEXT("event_index"), 0);
        Params->SetStringField(TEXT("argument"), TEXT("NoSuchArgument"));
        Params->SetBoolField(TEXT("value"), true);
        const FMonolithActionResult R = Run(TEXT("mvvm_set_event_argument"), Params);
        TestFalse(TEXT("an unknown argument is refused"), R.bSuccess);
        TestTrue(FString::Printf(TEXT("the refusal lists the arguments (%s)"), *R.ErrorMessage), R.ErrorMessage.Contains(TEXT("bInIsEnabled")));

        Params->SetStringField(TEXT("argument"), TEXT("bInIsEnabled"));
        Params->SetStringField(TEXT("path"), TEXT("viewmodel:VM.bIsShown"));
        TestFalse(TEXT("path and value together are refused"), Run(TEXT("mvvm_set_event_argument"), Params).bSuccess);
    }

    // Two conditions; the second is removed by index below.
    for (const double Value : { 0.5, 0.9 })
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("condition"), TEXT("viewmodel:VM.Ratio"));
        Params->SetStringField(TEXT("destination"), TEXT("self:RemoveFromParent"));
        Params->SetStringField(TEXT("operation"), TEXT("MoreThan"));
        Params->SetNumberField(TEXT("value"), Value);
        const FMonolithActionResult R = Run(TEXT("mvvm_add_condition"), Params);
        if (TestTrue(FString::Printf(TEXT("condition (%s)"), *R.ErrorMessage), R.bSuccess))
        {
            const TSharedPtr<FJsonObject> C = R.Result->GetObjectField(TEXT("condition"));
            TestEqual(TEXT("condition path"), C->GetObjectField(TEXT("condition"))->GetStringField(TEXT("path")), FString(TEXT("viewmodel:VM.Ratio")));
            TestEqual(TEXT("condition operation"), C->GetStringField(TEXT("operation")), FString(TEXT("MoreThan")));
            TestEqual(TEXT("condition value"), C->GetNumberField(TEXT("value")), Value, 1e-4);
        }
    }
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("condition"), TEXT("viewmodel:VM.Ratio"));
        Params->SetStringField(TEXT("destination"), TEXT("self:RemoveFromParent"));
        Params->SetStringField(TEXT("operation"), TEXT("Bigger"));
        TestFalse(TEXT("an unknown operation is refused"), Run(TEXT("mvvm_add_condition"), Params).bSuccess);
    }

    // Panel extension.
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("widget"), TEXT("MyList"));
        Params->SetStringField(TEXT("entry_widget_class"), Entry->GetPathName());
        const FMonolithActionResult R = Run(TEXT("mvvm_add_panel_extension"), Params);
        if (TestTrue(FString::Printf(TEXT("panel extension (%s)"), *R.ErrorMessage), R.bSuccess))
        {
            TestNotEqual(TEXT("the Widget Blueprint compiles"), R.Result->GetStringField(TEXT("blueprint_status")), FString(TEXT("error")));
        }
        TestFalse(TEXT("a second panel extension on the same panel is refused"), Run(TEXT("mvvm_add_panel_extension"), Params).bSuccess);
    }

    // ---- Read back.
    const TSharedPtr<FJsonObject> View = GetView(WBP);
    if (!TestTrue(TEXT("mvvm_get_view succeeds"), View.IsValid()))
    {
        return false;
    }
    TestEqual(TEXT("two conditions"), View->GetArrayField(TEXT("conditions")).Num(), 2);
    const TArray<TSharedPtr<FJsonValue>>& Extensions = View->GetArrayField(TEXT("view_extensions"));
    if (TestEqual(TEXT("one view extension"), Extensions.Num(), 1))
    {
        const TSharedPtr<FJsonObject> X = Extensions[0]->AsObject();
        TestEqual(TEXT("extension widget"), X->GetStringField(TEXT("widget")), FString(TEXT("MyList")));
        TestEqual(TEXT("extension entry class"), X->GetStringField(TEXT("entry_widget_class")), Entry->GeneratedClass->GetPathName());
        TestEqual(TEXT("extension entry viewmodel defaults to the only one"), X->GetStringField(TEXT("entry_viewmodel")), FString(TEXT("EntryVM")));
    }

    // The widget filter finds MyButton in the conversion pin and in the event path.
    if (const TSharedPtr<FJsonObject> Filtered = GetView(WBP, TEXT("MyButton")))
    {
        TestEqual(TEXT("filter: the conversion binding whose source is MyButton"), Filtered->GetArrayField(TEXT("bindings")).Num(), 1);
        TestEqual(TEXT("filter: the MyButton event"), Filtered->GetArrayField(TEXT("events")).Num(), 1);
        TestEqual(TEXT("filter: no view extension on MyButton"), Filtered->GetArrayField(TEXT("view_extensions")).Num(), 0);
    }

    // remove_viewmodel without remove_bindings counts the conditions too.
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("name"), TEXT("VM"));
        const FMonolithActionResult R = Run(TEXT("mvvm_remove_viewmodel"), Params);
        TestFalse(TEXT("removing a used viewmodel is refused"), R.bSuccess);
        TestTrue(FString::Printf(TEXT("the refusal counts the conditions (%s)"), *R.ErrorMessage), R.ErrorMessage.Contains(TEXT("2 condition(s)")));
    }

    // ---- Rename, then remove.
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("name"), TEXT("VM"));
        Params->SetStringField(TEXT("new_name"), TEXT("ShopVM"));
        const FMonolithActionResult R = Run(TEXT("mvvm_rename_viewmodel"), Params);
        TestTrue(FString::Printf(TEXT("rename viewmodel (%s)"), *R.ErrorMessage), R.bSuccess);
    }
    if (const TSharedPtr<FJsonObject> Renamed = GetView(WBP))
    {
        bool bBindingFollows = false;
        for (const TSharedPtr<FJsonValue>& Value : Renamed->GetArrayField(TEXT("bindings")))
        {
            bBindingFollows |= Value->AsObject()->GetObjectField(TEXT("source"))->GetStringField(TEXT("path")) == TEXT("viewmodel:ShopVM.Label");
        }
        TestTrue(TEXT("bindings follow the rename"), bBindingFollows);
        const TArray<TSharedPtr<FJsonValue>>& Conditions = Renamed->GetArrayField(TEXT("conditions"));
        TestTrue(TEXT("conditions follow the rename"), Conditions.Num() > 0
            && Conditions[0]->AsObject()->GetObjectField(TEXT("condition"))->GetStringField(TEXT("path")) == TEXT("viewmodel:ShopVM.Ratio"));
    }
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetNumberField(TEXT("condition_index"), 1);
        const FMonolithActionResult R = Run(TEXT("mvvm_remove_condition"), Params);
        TestTrue(FString::Printf(TEXT("remove condition (%s)"), *R.ErrorMessage), R.bSuccess && R.Result->GetNumberField(TEXT("remaining_conditions")) == 1);
        Params->SetNumberField(TEXT("condition_index"), 5);
        TestFalse(TEXT("an out-of-range condition index is refused"), Run(TEXT("mvvm_remove_condition"), Params).bSuccess);
    }
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("widget"), TEXT("MyList"));
        TestTrue(TEXT("remove panel extension"), Run(TEXT("mvvm_remove_panel_extension"), Params).bSuccess);
        TestFalse(TEXT("removing it again is refused"), Run(TEXT("mvvm_remove_panel_extension"), Params).bSuccess);
    }
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("name"), TEXT("ShopVM"));
        Params->SetBoolField(TEXT("remove_bindings"), true);
        const FMonolithActionResult R = Run(TEXT("mvvm_remove_viewmodel"), Params);
        if (TestTrue(FString::Printf(TEXT("remove viewmodel with its users (%s)"), *R.ErrorMessage), R.bSuccess))
        {
            TestEqual(TEXT("removed_conditions"), static_cast<int32>(R.Result->GetNumberField(TEXT("removed_conditions"))), 1);
        }
    }
    if (const TSharedPtr<FJsonObject> After = GetView(WBP))
    {
        TestEqual(TEXT("no viewmodels left"), After->GetArrayField(TEXT("viewmodels")).Num(), 0);
        TestEqual(TEXT("no conditions left"), After->GetArrayField(TEXT("conditions")).Num(), 0);
        TestEqual(TEXT("no view extensions left"), After->GetArrayField(TEXT("view_extensions")).Num(), 0);
        TestEqual(TEXT("the widget-only binding is kept"), After->GetArrayField(TEXT("bindings")).Num(), 1);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMVVMRefusalsLeaveNoViewTest,
    "Monolith.UI.MVVM.RefusalsLeaveNoView",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMVVMRefusalsLeaveNoViewTest::RunTest(const FString& /*Parameters*/)
{
    using namespace MonolithUIMVVMTests;
    UWidgetBlueprint* WBP = MakeWidgetBlueprint(TEXT("WBP_MonolithMVVMRefusals"));
    ON_SCOPE_EXIT { Discard(WBP); };
    if (!TestNotNull(TEXT("fixture"), WBP))
    {
        return false;
    }
    AddChild<UTextBlock>(WBP, TEXT("MyText"));
    AddChild<UButton>(WBP, TEXT("MyButton"));
    FKismetEditorUtilities::CompileBlueprint(WBP);

    {
        TSharedPtr<FJsonObject> Params = Binding(WBP, TEXT("widget:MyButton.bIsEnabled"), TEXT("widget:MyText.Text"));
        Params->SetStringField(TEXT("conversion_function"), TEXT("KismetTextLibrary.Conv_BoolToText"));
        Params->SetStringField(TEXT("conversion_argument"), TEXT("NoSuchArgument"));
        const FMonolithActionResult R = Run(TEXT("mvvm_add_binding"), Params);
        TestFalse(TEXT("an unknown conversion argument is refused"), R.bSuccess);
        TestTrue(FString::Printf(TEXT("the refusal names the argument (%s)"), *R.ErrorMessage), R.ErrorMessage.Contains(TEXT("NoSuchArgument")));
    }
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("condition"), TEXT("viewmodel:Nope.Ratio"));
        Params->SetStringField(TEXT("destination"), TEXT("self:RemoveFromParent"));
        Params->SetStringField(TEXT("operation"), TEXT("Equal"));
        TestFalse(TEXT("a condition on an unknown viewmodel is refused"), Run(TEXT("mvvm_add_condition"), Params).bSuccess);
    }
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetStringField(TEXT("widget"), TEXT("MyText"));
        Params->SetStringField(TEXT("entry_widget_class"), WBP->GetPathName());
        TestFalse(TEXT("a panel extension on a non-panel widget is refused"), Run(TEXT("mvvm_add_panel_extension"), Params).bSuccess);
    }
    {
        TSharedPtr<FJsonObject> Params = P(WBP);
        Params->SetNumberField(TEXT("event_index"), 0);
        Params->SetStringField(TEXT("argument"), TEXT("bInIsEnabled"));
        Params->SetBoolField(TEXT("value"), true);
        TestFalse(TEXT("an event argument without an MVVM view is refused"), Run(TEXT("mvvm_set_event_argument"), Params).bSuccess);
    }
    const TSharedPtr<FJsonObject> View = GetView(WBP);
    TestTrue(TEXT("refused writes create no MVVM view"), View.IsValid() && !View->GetBoolField(TEXT("has_view")));

    for (const FMonolithActionInfo& Info : FMonolithToolRegistry::Get().GetActions(TEXT("ui")))
    {
        if (Info.Action.StartsWith(TEXT("mvvm_")))
        {
            TestEqual(FString::Printf(TEXT("%s read-only annotation"), *Info.Action), Info.bReadOnlyHint, Info.Action == TEXT("mvvm_get_view"));
        }
    }
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_MVVM

// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"
#include "MonolithUICommon.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/SizeBox.h"
#include "Components/Image.h"

namespace MonolithUI::MoveWidgetTests
{
    static const FString GAssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MoveWidgetTest");

    /** NewParent nullptr = omit new_parent_name; Index unset = omit index. */
    static FMonolithActionResult Move(const TCHAR* Widget, const TCHAR* NewParent, TOptional<int32> Index = {})
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), GAssetPath);
        Params->SetStringField(TEXT("widget_name"), Widget);
        if (NewParent)
        {
            Params->SetStringField(TEXT("new_parent_name"), NewParent);
        }
        if (Index.IsSet())
        {
            Params->SetNumberField(TEXT("index"), Index.GetValue());
        }
        Params->SetBoolField(TEXT("compile"), false);
        return FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("move_widget"), Params);
    }

    /** Child names of a panel, in order, joined with commas. */
    static FString ChildOrder(UWidgetBlueprint* WBP, const TCHAR* PanelName)
    {
        TArray<FString> Names;
        if (const UPanelWidget* Panel = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(FName(PanelName))))
        {
            for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
            {
                Names.Add(Panel->GetChildAt(Index)->GetName());
            }
        }
        return FString::Join(Names, TEXT(","));
    }
} // namespace MonolithUI::MoveWidgetTests


/**
 * MonolithUI.MoveWidget.IndexAndGuards
 *
 * RootCanvas > { List (VerticalBox) > { A, B, C, Inner (HorizontalBox) }, Box (SizeBox) > { Filled } }
 *
 * Reorders within a panel (keeping the slot's padding), inserts into another parent
 * at an index, and checks the refusals that must not detach the widget: a full
 * single-child panel, the widget's own descendant, and the root.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMoveWidgetIndexAndGuardsTest,
    "MonolithUI.MoveWidget.IndexAndGuards",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMoveWidgetIndexAndGuardsTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::MoveWidgetTests;
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    FString FixtureError;
    UWidget* ListWidget = nullptr;
    if (!CreateOrReuseTestWidgetBlueprint(GAssetPath, FName(TEXT("List")), UVerticalBox::StaticClass(), FixtureError, &ListWidget))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }
    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *GAssetPath);
    UVerticalBox* List = Cast<UVerticalBox>(ListWidget);
    UPanelWidget* RootCanvas = WBP ? Cast<UPanelWidget>(WBP->WidgetTree->RootWidget) : nullptr;
    if (!WBP || !List || !RootCanvas)
    {
        AddError(TEXT("Fixture is missing the WBP, List or RootCanvas"));
        return false;
    }

    // Widgets built here must get variable GUIDs, or the compile move_widget runs ensures.
    UWidgetTree* Tree = WBP->WidgetTree;
    auto Make = [WBP, Tree](UClass* Class, const TCHAR* Name)
    {
        UWidget* Widget = Tree->ConstructWidget<UWidget>(Class, FName(Name));
        MonolithUI::RegisterCreatedWidget(WBP, Widget);
        return Widget;
    };
    MonolithUI::RegisterCreatedWidget(WBP, RootCanvas);
    MonolithUI::RegisterCreatedWidget(WBP, List);
    for (const TCHAR* Name : { TEXT("A"), TEXT("B"), TEXT("C") })
    {
        List->AddChild(Make(UImage::StaticClass(), Name));
    }
    List->AddChild(Make(UHorizontalBox::StaticClass(), TEXT("Inner")));
    UPanelWidget* Box = Cast<UPanelWidget>(Make(USizeBox::StaticClass(), TEXT("Box")));
    RootCanvas->AddChild(Box);
    Box->AddChild(Make(UImage::StaticClass(), TEXT("Filled")));

    UWidget* C = Tree->FindWidget(TEXT("C"));
    Cast<UVerticalBoxSlot>(C->Slot)->SetPadding(FMargin(7.f));

    // Reorder within the current parent: no new_parent_name.
    {
        const FMonolithActionResult Result = Move(TEXT("C"), nullptr, 0);
        TestTrue(TEXT("reorder succeeds"), Result.bSuccess);
        TestEqual(TEXT("C moved first"), ChildOrder(WBP, TEXT("List")), FString(TEXT("C,A,B,Inner")));
        const UVerticalBoxSlot* Slot = Cast<UVerticalBoxSlot>(C->Slot);
        TestTrue(TEXT("slot padding kept"), Slot && Slot->GetPadding() == FMargin(7.f));
        bool bPreserved = false;
        TestTrue(TEXT("slot_preserved reported"), Result.bSuccess && Result.Result->TryGetBoolField(TEXT("slot_preserved"), bPreserved) && bPreserved);
    }

    // Same parent named explicitly, index past the end clamps to last.
    {
        const FMonolithActionResult Result = Move(TEXT("C"), TEXT("List"), 99);
        TestTrue(TEXT("reorder to last succeeds"), Result.bSuccess);
        TestEqual(TEXT("C moved last"), ChildOrder(WBP, TEXT("List")), FString(TEXT("A,B,Inner,C")));
        double ReportedIndex = -1.0;
        TestTrue(TEXT("index reported"), Result.bSuccess && Result.Result->TryGetNumberField(TEXT("index"), ReportedIndex));
        TestEqual(TEXT("reported index is the final position"), static_cast<int32>(ReportedIndex), 3);
    }

    // Another parent, at an index.
    {
        const FMonolithActionResult Result = Move(TEXT("A"), TEXT("RootCanvas"), 0);
        TestTrue(TEXT("cross-parent insert succeeds"), Result.bSuccess);
        TestEqual(TEXT("A first in RootCanvas"), ChildOrder(WBP, TEXT("RootCanvas")), FString(TEXT("A,List,Box")));
        TestEqual(TEXT("A left List"), ChildOrder(WBP, TEXT("List")), FString(TEXT("B,Inner,C")));
    }

    // Full single-child panel: refused, and B stays where it was.
    {
        const FMonolithActionResult Result = Move(TEXT("B"), TEXT("Box"));
        TestFalse(TEXT("move into a full SizeBox is refused"), Result.bSuccess);
        TestEqual(TEXT("B not detached"), ChildOrder(WBP, TEXT("List")), FString(TEXT("B,Inner,C")));
    }

    // Into its own descendant: refused.
    {
        const FMonolithActionResult Result = Move(TEXT("List"), TEXT("Inner"));
        TestFalse(TEXT("move into own descendant is refused"), Result.bSuccess);
        TestEqual(TEXT("List still under RootCanvas"), ChildOrder(WBP, TEXT("RootCanvas")), FString(TEXT("A,List,Box")));
    }

    // The root has no parent to move within.
    {
        const FMonolithActionResult Result = Move(TEXT("RootCanvas"), TEXT("List"));
        TestFalse(TEXT("moving the root is refused"), Result.bSuccess);
    }

    // Neither destination nor index.
    {
        const FMonolithActionResult Result = Move(TEXT("B"), nullptr);
        TestFalse(TEXT("no new_parent_name and no index is refused"), Result.bSuccess);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
